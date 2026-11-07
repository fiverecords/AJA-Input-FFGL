// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    SharedTextureRing.h
 * @brief   Triple-buffered ring of GL textures + PBOs + fences for off-render
 *          -thread frame upload.
 *
 * This class implements the same triple-buffer atomic-index pattern that
 * FrameQueue uses for CPU byte buffers, but the slots hold GL resources
 * instead. The producer thread (running in a secondary GL context that
 * shares resources with Resolume's primary context) uploads UYVY frame
 * data directly into a writer-owned PBO and texture, records a fence,
 * and publishes by swapping the writer and mid indices atomically. The
 * render thread acquires by swapping the reader and mid indices and then
 * issuing a server-side glWaitSync on the slot's fence, so the GPU does
 * not start sampling the texture until the upload's commands have
 * completed, while never blocking the CPU.
 *
 * The expected effect on the end-to-end latency: the "wait" metric (time
 * between publication and consumption) drops from ~10-15 ms with on-render
 * -thread upload to sub-millisecond, because the texture is ready by the
 * time Resolume calls ProcessOpenGL.
 *
 * The producer publishes unconditionally at its own rate (matching AJA's
 * AutoCirculate DMA behaviour). If a previously published slot has not
 * been consumed before the next publish, that slot is silently superseded
 * and the event is tallied as a consumer miss for diagnostics.
 */
#pragma once

#include <FFGLSDK.h>
#include "PixelFormat.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace ajainput
{

class SharedTextureRing
{
public:
    /// @brief Tag attached to each publish describing what was freshly
    ///        written into the slot's texture.
    ///
    /// FullFrame: classic per-frame publish — every row of the texture
    ///   was just refreshed. Used by progressive sources, V210
    ///   interlaced, and any path that doesn't want field-rate
    ///   semantics. This is the only mode used before v0.32.
    ///
    /// FreshTop / FreshBottom: per-field publish (v0.32+). The texture
    ///   carries the WHOLE frame (the producer scattered the contiguous
    ///   field bytes into every-other-line of its host accumulator
    ///   before uploading), but only ONE of the two field row-sets was
    ///   refreshed by this publish. The other field rows still carry
    ///   the data from a previous publish (which, in steady state, is
    ///   the complementary field of the same source frame). Consumers
    ///   that sample only the freshly-written field (the YADIF /
    ///   field-rate Bob paths) get a half-frame latency win because
    ///   they don't have to wait for the second field to arrive. The
    ///   first field of a new source frame (isFirstFieldOfFrame=true)
    ///   is the trigger for rotating the YADIF history; the second
    ///   field of the same frame only refreshes the current slot.
    enum class FieldHint : uint8_t
    {
        FullFrame    = 0,
        FreshTop     = 1,
        FreshBottom  = 2,
    };

    /// @brief Snapshot of a slot held by the consumer.
    struct ReadResult
    {
        GLuint                                texture     = 0;
        std::chrono::steady_clock::time_point producedAt  = {};
        std::chrono::steady_clock::time_point publishedAt = {};
        uint64_t                              sequence    = 0;
        bool                                  isFresh     = false;
        bool                                  isValid     = false;
        /// Field metadata carried over from Upload. For FullFrame
        /// publishes (the default), `fieldHint` is FullFrame and
        /// `isFirstFieldOfFrame` is false (unused). For per-field
        /// publishes, `fieldHint` identifies which field was freshly
        /// written, and `isFirstFieldOfFrame` is true on the FIRST
        /// field of a NEW source frame (used by the YADIF render path
        /// to trigger history rotation only once per source frame).
        FieldHint                             fieldHint           = FieldHint::FullFrame;
        bool                                  isFirstFieldOfFrame = false;
        /// Monotonic per-source-frame counter from the producer.
        /// Ticks once per source frame regardless of capture mode:
        /// frame-mode publishes get a fresh value every Upload,
        /// field-mode publishes carry the SAME value for F0 and F1
        /// of the same source frame. The render thread uses this to
        /// decide when to rotate the YADIF history (rotate when the
        /// value advances vs the last-rotated value) — a sequence-
        /// based gate that is robust against a lost F0 publish
        /// (superseded in the ring before being acquired), where the
        /// next F1 will still carry the new source-frame number and
        /// trigger the rotation. Zero on the legacy / no-metadata
        /// path; that value is never "advanced into" since the
        /// rotation tracker also starts at zero.
        std::uint64_t                         sourceFrameSequence = 0;
        /// True when the upload's GPU fence was still unsignaled at
        /// Acquire time. The render thread will then have to wait
        /// (server-side) for the upload to complete in the GPU command
        /// queue before sampling the texture, which is hidden latency:
        /// even though the CPU returned from Acquire instantly, the
        /// subsequent draw is gated on PCIe DMA still in flight. A
        /// high rate of fencePending=true means the PCIe transfer is
        /// our bottleneck, and would justify a more aggressive
        /// pipeline (overlapping next-frame DMA with current-frame
        /// PBO→VRAM, or going to a non-server-side wait pattern).
        /// Always false on the very first call (no fences exist yet).
        bool                                  fencePending = false;
    };

    SharedTextureRing() = default;
    ~SharedTextureRing();

    SharedTextureRing(const SharedTextureRing&)            = delete;
    SharedTextureRing& operator=(const SharedTextureRing&) = delete;

    /**
     * @brief Allocate the textures, PBOs and storage for the ring.
     *
     * Must be called from the render thread while the primary GL context
     * is current. Allocation in the primary context makes the resources
     * automatically visible to the secondary (shared) context.
     *
     * @param widthPixels   Source image width in pixels.
     * @param heightPixels  Source image height in pixels.
     * @param kind          Pixel format of the source bytes. UYVY (the
     *                      default) yields a half-width RGBA8 texture and
     *                      UYVYFrameSize(w,h) bytes per slot. V210 (10-bit
     *                      packed, 4:2:2) yields a different texture width
     *                      and a 33% larger buffer per slot; the fragment
     *                      shader is responsible for unpacking the 10-bit
     *                      samples on read.
     *
     * @return True on success; false if GL allocation failed.
     */
    bool Initialise(std::size_t widthPixels,
                    std::size_t heightPixels,
                    PixelKind   kind = PixelKind::UYVY);

    /// @brief Release GL resources. Safe to call multiple times.
    ///        Must be called from the render thread.
    void Release();

    /// @return True if the ring has been successfully initialised.
    bool IsAvailable() const noexcept { return m_textures[0] != 0; }

    /// @return Bytes per frame the ring was initialised for (0 when
    ///         released). Producers must hand Upload exactly this many
    ///         bytes; it is also the size of each persistent PBO.
    ///         Stable between Initialise calls, which only happen on
    ///         the render thread with every producer stopped.
    std::size_t FrameBytes() const noexcept { return m_frameBytes; }

    /// @return True while a previously published slot has not yet been
    ///         picked up by the consumer. Used by the producer for
    ///         backpressure decisions, identical to FrameQueue.
    bool HasUnconsumedFrame() const noexcept
    { return (m_mid.load(std::memory_order_acquire) & kFreshBit) != 0; }

    /**
     * @brief Upload one frame's bytes.
     *
     * Must be called from the producer thread while the secondary GL
     * context is current. The upload writes into the writer-owned slot's
     * PBO, then transfers PBO contents to the slot's texture, then
     * records a fence and publishes the slot by atomic exchange.
     *
     * @param frameData   Source bytes. Must point to a buffer of size
     *                    FrameSize(kind, width, height) bytes where
     *                    `kind` was the one passed to Initialise.
     * @param producedAt  Time the frame was generated upstream. Carried
     *                    through unchanged for end-to-end latency stats.
     * @param hint        Optional field tag describing what was freshly
     *                    written. Defaults to FullFrame for backward
     *                    compatibility with frame-rate callers.
     * @param isFirstFieldOfFrame
     *                    Only meaningful when `hint` is FreshTop or
     *                    FreshBottom: true if this is the FIRST field
     *                    of a NEW source frame (the cue for the YADIF
     *                    render path to rotate its history). Defaults
     *                    to false. Ignored for FullFrame publishes.
     * @param sourceFrameSequence
     *                    Optional monotonic per-source-frame counter
     *                    from the producer. Ticks once per source
     *                    frame in both modes; the render thread uses
     *                    advancing values as the YADIF rotation
     *                    trigger (robust to lost publishes). Defaults
     *                    to 0 (legacy callers stay on the FullFrame /
     *                    isFirstFieldOfFrame-based rotation gate).
     */
    void Upload(const uint8_t*                        frameData,
                std::chrono::steady_clock::time_point producedAt,
                FieldHint                             hint                = FieldHint::FullFrame,
                bool                                  isFirstFieldOfFrame = false,
                std::uint64_t                         sourceFrameSequence = 0);

    /**
     * @brief Acquire the most recent published slot.
     *
     * Must be called from the render thread while the primary GL context
     * is current. Issues a server-side glWaitSync on the slot's fence so
     * subsequent GPU commands wait for the upload to complete without
     * blocking the CPU. Never blocks.
     */
    ReadResult Acquire();

    /**
     * @brief Mark the end of this frame's use of the acquired slot.
     *
     * Must be called from the render thread, primary context current,
     * after the last GL command that reads the texture returned by
     * Acquire (v0.39). Records a fence the producer waits on (GPU side)
     * before it next overwrites that slot's texture, closing the
     * cross-context write-after-read window. Skipping it for a frame is
     * safe: the producer then simply does not wait.
     */
    void EndRead();

    /// Counters update at the time of the relevant event and are safe to
    /// read at any time from any thread.
    uint64_t UploadedCount() const noexcept
    { return m_uploadedCount.load(std::memory_order_relaxed); }

    uint64_t RepeatedCount() const noexcept
    { return m_repeatedCount.load(std::memory_order_relaxed); }

    /// @return Number of uploads that overwrote a previously published
    ///         slot before the consumer acquired it. This is the analog
    ///         of an AutoCirculate "output drop": frames the producer
    ///         delivered that never reached the screen.
    uint64_t ConsumerMissedCount() const noexcept
    { return m_consumerMissedCount.load(std::memory_order_relaxed); }

    /// @return True if the PBOs were allocated with GL_ARB_buffer_storage
    ///         and are persistently mapped to host pointers. In that case
    ///         the producer can DMA frame data directly into the writer
    ///         slot's PBO (queried via WriterPersistentPtr) instead of
    ///         going through an intermediate host buffer + memcpy, and
    ///         must then call Upload with that same pointer — which
    ///         skips the redundant copy and just dispatches the
    ///         PBO→texture transfer. Decided once at Initialise based
    ///         on driver capability; older drivers fall back to the
    ///         classic map/unmap path and this returns false.
    bool HasPersistentMapping() const noexcept
    { return m_persistentMapping; }

    /// @return Host-visible pointer for the current writer slot's PBO
    ///         when persistent mapping is in use. The producer thread
    ///         is the only legal caller, and only between consecutive
    ///         Upload calls — after Upload the writer slot changes and
    ///         a fresh call to this method is required.
    ///         Returns nullptr if persistent mapping is not active.
    void* WriterPersistentPtr() const noexcept
    { return m_persistentMapping ? m_persistentPtrs[m_writerIdx] : nullptr; }

    /// @brief Same pointer as WriterPersistentPtr, for a caller about to
    ///        DMA into it (v0.39). Producer thread, secondary context
    ///        current. Waits (bounded, normally not at all) until the
    ///        GPU has finished the previous upload that read this PBO,
    ///        so the DMA cannot overwrite bytes still to be uploaded.
    void* WriterPersistentPtrForWrite();

    /// @return Host-visible pointer for slot index 0..kSize-1 when
    ///         persistent mapping is in use. Used by AJACaptureSource
    ///         to page-lock every slot's backing memory once at the
    ///         top of the capture loop.
    void* PersistentPtrForSlot(int slot) const noexcept
    {
        if (!m_persistentMapping || slot < 0 || slot >= kSize)
            return nullptr;
        return m_persistentPtrs[slot];
    }

    /// @return Number of ring slots (kSize). Stable, compile-time.
    static constexpr int SlotCount() noexcept { return kSize; }

private:
    static constexpr int kSize = 3;

    /// Producer side: wait (bounded) for this slot's last upload fence.
    void WaitForSlotUploadIdle(int slot);

    // GL resources. Allocated in render thread, accessed from either due
    // to share-list visibility.
    GLuint  m_textures[kSize] = { 0, 0, 0 };
    GLuint  m_pbos    [kSize] = { 0, 0, 0 };

    // Fences. Created by the producer after each upload, waited on by
    // the renderer, deleted by the producer at the next upload to the
    // same slot.
    GLsync  m_fences  [kSize] = { nullptr, nullptr, nullptr };

    // Read fences (v0.39). Created by the consumer in EndRead after its
    // last read of the reader slot, handed to the producer with the slot
    // through the m_mid exchange, waited on (GPU side) and deleted by the
    // producer before it overwrites that slot's texture.
    GLsync  m_readFences[kSize] = { nullptr, nullptr, nullptr };

    // Per-slot metadata, written by producer, read by consumer.
    std::chrono::steady_clock::time_point m_producedAt [kSize] = {};
    std::chrono::steady_clock::time_point m_publishedAt[kSize] = {};
    uint64_t                              m_sequences  [kSize] = { 0, 0, 0 };

    // Per-slot field metadata (v0.32+). For FullFrame publishes,
    // both fields are considered fresh and m_fieldHint stays
    // FullFrame; m_isFirstFieldOfFrame is unused. For per-field
    // publishes, m_fieldHint identifies which field rows the producer
    // refreshed in this slot's texture, and m_isFirstFieldOfFrame is
    // true on the first field of a new source frame.
    FieldHint                             m_fieldHints    [kSize] = {
        FieldHint::FullFrame, FieldHint::FullFrame, FieldHint::FullFrame
    };
    bool                                  m_isFirstFields [kSize] = {
        false, false, false
    };
    std::uint64_t                         m_srcFrameSeqs  [kSize] = {
        0, 0, 0
    };

    // Triple-buffer indices, identical pattern to FrameQueue: the shared
    // m_mid carries the ready slot index in its low bits and the fresh
    // bit, so publish and acquire are each a single atomic exchange.
    static constexpr int kIndexMask = 0x3;
    static constexpr int kFreshBit  = 0x4;

    int               m_writerIdx = 0;
    int               m_readerIdx = 2;
    std::atomic<int>  m_mid       { 1 };

    // Stats.
    std::atomic<uint64_t> m_uploadedCount       { 0 };
    std::atomic<uint64_t> m_repeatedCount       { 0 };
    std::atomic<uint64_t> m_consumerMissedCount { 0 };

    // Persistent host mappings of each PBO. Populated at Initialise time
    // when the driver advertises GL_ARB_buffer_storage. The producer
    // writes (or, with AJA, DMA-targets) the writer slot's pointer
    // directly, skipping the orphan + glMapBufferRange + glUnmapBuffer
    // cycle that the classic streaming path requires. Unmapped and
    // zeroed in Release. nullptr when fallback path is in use.
    void* m_persistentPtrs[kSize] = { nullptr, nullptr, nullptr };
    bool  m_persistentMapping = false;

    // Dimensions and bookkeeping.
    std::size_t m_width            = 0;
    std::size_t m_height           = 0;
    std::size_t m_frameBytes       = 0;
    PixelKind   m_kind             = PixelKind::UYVY;
    uint64_t    m_sequenceCounter  = 0;
};

} // namespace ajainput
