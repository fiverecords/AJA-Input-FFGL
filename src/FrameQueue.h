// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    FrameQueue.h
 * @brief   Lock-free triple-buffer queue for handing frames from a producer
 *          thread to a consumer thread (typically the render thread).
 *
 * Design
 * ------
 * The queue holds three frame-sized slots. At any instant one slot is owned
 * by the producer (the "writer slot"), one by the consumer (the "reader
 * slot"), and one is the most-recently-completed frame held by the queue
 * (the "ready slot"). The invariant that the three slot indices are always
 * distinct is what makes the design lock-free: producer and consumer never
 * touch the same slot at the same time.
 *
 * The shared ready position m_mid packs the slot index (low bits) and a
 * fresh bit into ONE atomic, so each side hands off with a single
 * exchange and the two can never disagree.
 *
 * Publish path (producer):
 *   1. Write the new frame into the slot indexed by m_writerIdx.
 *   2. Exchange m_mid with (m_writerIdx | fresh), transferring the
 *      just-written slot to the ready position; the slot index that was
 *      previously ready becomes the new writer slot. If the previous
 *      value still had the fresh bit, that frame was never consumed and
 *      is counted as a drop.
 *
 * Acquire path (consumer):
 *   1. Load m_mid. If the fresh bit is set, exchange m_mid with
 *      m_readerIdx (fresh bit clear) to swap the ready slot into the
 *      reader position; only the consumer clears the bit, so the
 *      exchange always returns a fresh slot (the newest one). If the bit
 *      is not set, the consumer keeps the slot it already holds (a
 *      repeat).
 *
 * Behavioural notes
 * -----------------
 * - If the producer outpaces the consumer, intermediate frames are dropped.
 *   For live video this is desirable: the consumer always sees the most
 *   recent frame, never a stale one.
 * - If the consumer outpaces the producer, the consumer keeps re-rendering
 *   the same frame. This is the natural "frame repeat" behaviour that
 *   AutoCirculate also exhibits when the SDI input rate is below the host
 *   render rate.
 * - The queue allocates all storage in Initialise() and never touches the
 *   heap during steady-state operation; producer and consumer paths are
 *   bounded-time.
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ajainput
{

class FrameQueue
{
public:
    FrameQueue() = default;
    ~FrameQueue() { Release(); }

    FrameQueue(const FrameQueue&)            = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    /**
     * @brief Allocate the three internal slot buffers.
     * @param frameBytes  Size in bytes of one frame. All slots are sized
     *                    identically.
     */
    void Initialise(std::size_t frameBytes);

    /// @brief Free internal storage. Must not be called while a producer is
    ///        still running; the caller is responsible for joining the
    ///        producer thread first.
    void Release();

    /**
     * @brief Publish a new frame to the queue.
     * @param data        Pointer to a buffer of exactly the frameBytes
     *                    size passed to Initialise.
     * @param producedAt  Timestamp at which the frame became available;
     *                    used by the consumer to compute frame age.
     *
     * Never blocks. If a previously published frame had not yet been
     * consumed, it is overwritten and counted as a drop.
     */
    void WriteFrame(const uint8_t* data,
                    std::chrono::steady_clock::time_point producedAt);

    /// @brief Snapshot of a frame held by the consumer.
    struct ReadResult
    {
        const uint8_t*                          data        = nullptr;
        std::size_t                             size        = 0;
        std::chrono::steady_clock::time_point   producedAt  = {};
        std::chrono::steady_clock::time_point   publishedAt = {};
        uint64_t                                sequence    = 0;
        bool                                    isFresh     = false;
    };

    /**
     * @brief Acquire the most recent frame for consumption.
     *
     * Never blocks. If a new frame has been published since the last call,
     * the consumer's slot is swapped to point at it and isFresh is true.
     * Otherwise the previously-held slot is returned again with isFresh
     * false (a frame repeat). The returned pointer remains valid until the
     * next call to AcquireRead.
     */
    ReadResult AcquireRead();

    /// @brief Non-destructive check used by the producer to apply
    ///        backpressure: returns true while a published frame is still
    ///        waiting to be consumed.
    bool HasUnconsumedFrame() const noexcept
    { return (m_mid.load(std::memory_order_acquire) & kFreshBit) != 0; }

    /// @brief Zero the cumulative stats. Intended for clean test runs after
    ///        a configuration change.
    void ResetStats() noexcept;

    /// Counters update at the time of the relevant event and are safe to
    /// read at any time from any thread.
    /// @return Bytes per frame set by Initialise (0 when released).
    std::size_t FrameBytes() const noexcept { return m_frameBytes; }

    uint64_t ProducedCount() const noexcept
    { return m_producedCount.load(std::memory_order_relaxed); }
    uint64_t DroppedCount()  const noexcept
    { return m_droppedCount.load(std::memory_order_relaxed); }
    uint64_t RepeatedCount() const noexcept
    { return m_repeatedCount.load(std::memory_order_relaxed); }

private:
    struct Slot
    {
        std::vector<uint8_t>                    data;
        std::chrono::steady_clock::time_point   producedAt  = {};
        std::chrono::steady_clock::time_point   publishedAt = {};
        uint64_t                                sequence    = 0;
    };

    std::array<Slot, 3> m_slots;

    // Slot ownership indices. Only the producer touches m_writerIdx; only
    // the consumer touches m_readerIdx. m_mid is shared via atomic
    // exchange and is the publication channel: the low bits hold the
    // ready slot index, kFreshBit is set by the producer on publish and
    // cleared by the consumer on acquire. Keeping both in one atomic is
    // what makes the handoff race-free (see WriteFrame).
    static constexpr int kIndexMask = 0x3;
    static constexpr int kFreshBit  = 0x4;

    int                 m_writerIdx          = 0;
    int                 m_readerIdx          = 2;
    std::atomic<int>    m_mid                { 1 };

    // Producer-local sequence counter; written into each slot at publish
    // time and read by the consumer through the slot's own field.
    uint64_t            m_producerSequence   = 0;

    std::size_t         m_frameBytes         = 0;

    std::atomic<uint64_t> m_producedCount    { 0 };
    std::atomic<uint64_t> m_droppedCount     { 0 };
    std::atomic<uint64_t> m_repeatedCount    { 0 };
};

} // namespace ajainput
