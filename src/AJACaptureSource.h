// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    AJACaptureSource.h
 * @brief   Real-hardware capture thread driving AJA AutoCirculate.
 *
 * Replaces the synthetic FrameProducer once an AJA device is open. The
 * capture thread blocks on the SDI vertical interrupt, asks AutoCirculate
 * for the next available input frame, DMAs it into a host buffer, and
 * publishes it through the same SharedTextureRing the synthetic path uses.
 * Downstream code (off-thread GPU upload, shader, render, overlay) is
 * untouched.
 *
 * Channel model. Three distinct concepts that real AJA hardware keeps
 * separate and that this class is careful not to conflate:
 *
 *   - **Physical input source** (NTV2InputSource): which connector on
 *     the back plate the signal enters on. SDI1..SDI4 on a Kona X
 *     correspond to the four 12G BNCs; HDMI1 is the HDMI port. The
 *     user picks this through the Input parameter.
 *   - **Physical input channel** (NTV2Channel, by BNC index): used only
 *     to flip the bidirectional SDI port between transmit and receive
 *     modes via SetSDITransmitEnable. SDI N's BNC is channel index N.
 *   - **FrameStore** (NTV2Channel, by FrameStore index): the internal
 *     DMA target that AutoCirculate drives. The number of FrameStores
 *     a card has is independent of the number of BNCs (a Kona X base
 *     has 4 BNCs but only 2 FrameStores). Each plugin instance claims
 *     its own FrameStore from the process-wide FrameStorePool;
 *     several plugins coexisting on the same card route different
 *     physical inputs to different FrameStores via the on-board
 *     signal-routing matrix (the crosspoint).
 *
 * The capture object owns its thread for its entire lifetime: from Start()
 * to Stop(), regardless of whether the SDI input is currently providing a
 * valid signal. The thread implements a small state machine
 * (Initializing → Running ↔ NoSignal, with Failed as a terminal sink) so
 * that signal loss and recovery happen transparently while the plugin
 * keeps rendering. The plugin layer reads State() per render frame to
 * decide whether to show captured pixels or a "no signal" indicator.
 *
 * The selected physical SDI is configurable at runtime via
 * SetInputNumber(); the call is thread-safe and triggers a clean
 * teardown + re-init inside the capture thread without blocking the
 * caller. The FrameStore is fixed at construction (claimed from the
 * pool) and held for the object's lifetime.
 */
#pragma once

#include "AJADevice.h"
#include "CaptureTopology.h"
#include "PixelFormat.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace ajainput
{

class FrameQueue;
class SharedTextureRing;
class SharedGLContext;

class AJACaptureSource
{
public:
    /// @brief Coarse state of the capture pipeline at any given moment.
    enum class State
    {
        Idle,           ///< Not started yet, or fully stopped.
        Initializing,   ///< Capture thread is configuring AutoCirculate.
        Running,        ///< Frames are being captured and published.
        NoSignal,       ///< No (or unsupported) input format detected.
        Failed          ///< Unrecoverable error. Stop()/Start() required.
    };

    /// @brief Human-readable short name for State, suitable for overlays.
    static const char* ToString(State s) noexcept;

    /**
     * @param device              An already-Open()'d AJADevice. Owned by
     *                            the caller; must outlive this object.
     * @param queue               CPU-byte fallback target. Used when
     *                            textureRing is null (legacy on-thread
     *                            upload path).
     * @param textureRing         Off-thread upload ring (preferred).
     *                            Frames are published through it when
     *                            non-null.
     * @param width               Expected frame width.
     * @param height              Expected frame height.
     * @param frameStoreZeroBased Index of the FrameStore allocated to
     *                            this capture instance. Must already
     *                            have been claimed from
     *                            FrameStorePool::Claim() and within the
     *                            card's hardware limit. The object does
     *                            NOT release the FrameStore on its own;
     *                            ownership of the claim sits with the
     *                            caller (AJAInputPlugin), which is in a
     *                            better position to manage lifetime
     *                            across InitGL / DeInitGL.
     * @param ringFrameCount      Number of contiguous device frame
     *                            buffers AutoCirculate will cycle
     *                            through on this channel. Trades end-
     *                            to-end latency for resilience against
     *                            consumer stalls: a low value
     *                            (2 or 3) keeps the capture-to-render
     *                            path shallow; a higher value (up to 7
     *                            in this plugin) absorbs longer
     *                            consumer hiccups before frames start
     *                            dropping. The SDK itself documents
     *                            "Fewer frames reduces latency, but
     *                            increases the likelihood of frame
     *                            drops." Reasonable bounds: 2..7. The
     *                            default of 2 matches what users
     *                            chasing the lowest possible latency
     *                            should pick first; bumping it is the
     *                            mitigation when occasional drops are
     *                            observed under load.
     * @param frameStoreMask      v0.42: every FrameStore the caller claimed
     *                            for this capture (bit i = FrameStore i):
     *                            a pair for a TSI 4K picture, four for
     *                            squares (CaptureTopology.h). 0 means just
     *                            frameStoreZeroBased. AutoCirculate runs on
     *                            frameStoreZeroBased, the lowest of them.
     */
    AJACaptureSource(AJADevice&         device,
                     FrameQueue&        queue,
                     SharedTextureRing* textureRing,
                     SharedGLContext*   sharedContext,
                     std::size_t        width,
                     std::size_t        height,
                     int                frameStoreZeroBased,
                     int                initialNtv2FormatRaw = 0,
                     int                ringFrameCount = 2,
                     bool               lowLatencyMode = false,
                     PixelKind          pixelKind      = PixelKind::UYVY,
                     uint32_t           frameStoreMask = 0);

    ~AJACaptureSource();

    AJACaptureSource(const AJACaptureSource&)            = delete;
    AJACaptureSource& operator=(const AJACaptureSource&) = delete;

    /**
     * @brief Spawn the capture thread. The thread enters the Initializing
     *        state and works through the state machine on its own; this
     *        call returns immediately. Use State() to query progress.
     */
    void Start();

    /**
     * @brief Stop and join the capture thread. AutoCirculate is torn down
     *        cleanly. Safe to call multiple times.
     */
    void Stop();

    /**
     * @brief Change the input to capture, by input number (InputId.h).
     *        The call is non-blocking; the capture thread tears down
     *        AutoCirculate on the previous input and re-inits on the new
     *        one at the next loop turn.
     */
    void SetInputNumber(int inputNumber) noexcept;

    /**
     * @brief Change the AutoCirculate ring frame count at runtime. The
     *        call is non-blocking; if the new value differs from the
     *        current one, the capture loop tears down AutoCirculate
     *        on the next iteration and re-inits with the new ring
     *        count. There is a momentary NoSignal-like gap (a couple
     *        of frames) during the rebuild, identical to a channel
     *        switch. Out-of-range values (< 2 or > 7) are clamped at
     *        the bounds; the SDK demands at least 2 buffers for
     *        AutoCirculate to function, and going above 7 starts to
     *        compete with the device's other FrameStores for limited
     *        SDRAM in a way that can fail to allocate on busier
     *        multi-channel setups.
     */
    void SetRingFrameCount(int ringFrameCount) noexcept;

    /**
     * @brief Switch between the default AutoCirculate-based capture
     *        path and the AJA-recommended low-latency path that
     *        bypasses AutoCirculate entirely. The low-latency path
     *        mirrors the SDK's ntv2llburn demo: two device frame
     *        slots are reserved via AutoCirculateInitForInput +
     *        AutoCirculateGetStatus + AutoCirculateStop, then the
     *        capture loop manually ping-pongs between them using
     *        SetInputFrame, blocks on WaitForInputFieldID, and
     *        transfers each completed frame with a direct DMAReadFrame
     *        call. The result is deterministic ~1-frame capture
     *        latency without the driver-side ring buffer's variable
     *        jitter, at the cost of less tolerance to consumer
     *        stalls: if the render thread cannot keep up with the
     *        SDI clock, captured frames are silently overwritten by
     *        the next vblank rather than dropped at a driver
     *        counter we can observe. This trade-off is exactly what
     *        the demo header documents: "Because of the tight
     *        latency requirements, AutoCirculate and a ring buffer
     *        are not used." Default is off (AutoCirculate path).
     *
     *        The call is non-blocking and idempotent; flipping the
     *        flag forces the capture loop back through Initializing
     *        on its next iteration, identical to a channel switch.
     */
    void SetLowLatencyMode(bool enabled) noexcept;

    /// @brief Currently requested low-latency mode (atomic snapshot of
    ///        the target; may differ from what the capture thread has
    ///        actually applied for one or two iterations after a
    ///        SetLowLatencyMode toggle, until the capture session is
    ///        re-initialised). Useful for the render-thread diagnostic
    ///        log to confirm which path the capture thread is on.
    bool IsLowLatencyModeRequested() const noexcept
    { return m_lowLatencyMode.load(std::memory_order_acquire); }

    /// @brief Currently configured ring frame count (atomic snapshot of
    ///        the target). Same semantics as IsLowLatencyModeRequested
    ///        regarding the lag between toggle and capture re-init.
    int RingFrameCountRequested() const noexcept
    { return m_targetRingFrameCount.load(std::memory_order_acquire); }

    /// @brief Current state of the capture pipeline.
    State GetState() const noexcept
    { return m_state.load(std::memory_order_acquire); }

    /// @brief Input number currently selected (InputId.h).
    int SelectedInputNumber() const noexcept
    { return m_targetInputNumber.load(std::memory_order_relaxed); }

    /// @brief Numeric value of the last NTV2VideoFormat seen on the input;
    ///        useful for logs even when the format is not supported.
    int LastVideoFormatRaw() const noexcept
    { return m_lastVideoFormatRaw.load(std::memory_order_relaxed); }

    /// @brief Zero-based index of the FrameStore this instance uses, as
    ///        allocated by FrameStorePool. Surfaced for diagnostic logs.
    ///        v0.42: the lowest of a FrameStore set (FrameStoreMask).
    int FrameStoreIndex() const noexcept
    { return m_frameStoreZeroBased; }

    /// @brief v0.42: every FrameStore this capture holds.
    uint32_t FrameStoreMask() const noexcept
    { return m_frameStoreMask; }

    /// @brief v0.42: HDR transfer the input signals (SDI VPID, HDMI DRM
    ///        InfoFrame): 0 none / SDR / unknown, 1 PQ, 2 HLG. Sampled
    ///        about once a second while running. Any thread.
    int SignalledTransfer() const noexcept
    { return m_signalledTransfer.load(std::memory_order_relaxed); }

    /// @brief True if an RP188 timecode has been seen on the input since
    ///        capture started. Stays true even on frames that lack
    ///        timecode (the last-known TC is kept).
    bool HasTimecode() const noexcept
    { return m_lastTimecodeValid.load(std::memory_order_relaxed); }

    /// @brief Most recently captured RP188 timecode packed as
    ///        (hours<<24) | (minutes<<16) | (seconds<<8) | frames.
    ///        Each component fits comfortably in a byte
    ///        (max h=23/m=59/s=59/f=59) so the whole H:M:S:F tuple
    ///        stays consistent across a single 32-bit atomic read.
    ///        The decoding from RP188 register bits to BCD happens in
    ///        the capture thread where the AJA SDK headers are already
    ///        in scope.
    uint32_t LastTimecodeHMSF() const noexcept
    { return m_lastTimecodeHMSF.load(std::memory_order_relaxed); }

    /// @brief Latest sampled lock state of the SDI input. Always true
    ///        when the capture source is HDMI or when the card does not
    ///        support SDI error reporting; reflects mLocked from
    ///        NTV2SDIInputStatus otherwise.
    bool SdiLocked() const noexcept
    { return m_sdiLocked.load(std::memory_order_relaxed); }

    /// @brief Cumulative count of CRC errors detected on the SDI input
    ///        since capture started. Zero on HDMI or unsupported cards.
    uint32_t SdiCrcErrors() const noexcept
    { return m_sdiCrcErrors.load(std::memory_order_relaxed); }

    /// @brief Cumulative count of SDI unlock events (cable wiggle,
    ///        source restart, jitter spike) since capture started.
    uint32_t SdiUnlocks() const noexcept
    { return m_sdiUnlocks.load(std::memory_order_relaxed); }

    // ---------- Diagnostics counters ----------------------------------------

    /// @brief Number of frames successfully transferred from the card.
    uint64_t FramesCaptured() const noexcept
    { return m_framesCaptured.load(std::memory_order_relaxed); }

    /// @brief Driver-reported drops accumulated across all init cycles.
    uint64_t DriverDrops() const noexcept
    { return m_driverDrops.load(std::memory_order_relaxed); }

    /// @return Transfers done straight after another one because frames
    ///         were still waiting in the driver ring (v0.40). Non-zero
    ///         means the capture thread woke up late at least once; each
    ///         drain is latency the pre-v0.40 loop would have kept.
    uint64_t BacklogDrains() const noexcept
    { return m_backlogDrains.load(std::memory_order_relaxed); }

    /// @return Transfers discarded because they returned the slot the
    ///         card was still recording into (v0.40; only after a
    ///         driver drop).
    uint64_t ActiveSlotSkips() const noexcept
    { return m_activeSlotSkips.load(std::memory_order_relaxed); }

    /// @brief Vblank wake-ups where no completed frame was available.
    uint64_t IdleWakeups() const noexcept
    { return m_idleWakeups.load(std::memory_order_relaxed); }

    /// @brief Count of NoSignal → Running transitions. Incremented every
    ///        time the input recovers; a non-zero value tells the user
    ///        that the cable has been flapping.
    uint64_t RecoveryCount() const noexcept
    { return m_recoveryCount.load(std::memory_order_relaxed); }

    /**
     * @brief Atomically consume a pending raster-size change.
     *
     * The capture thread posts a pending change when it detects that
     * the input has switched to a different raster (e.g. the camera
     * was reconfigured from 1080p59.94 to 720p60 mid-show, or the
     * upstream device was replaced with one of a different format).
     * On detection, capture moves to NoSignal because the host-side
     * buffer, the GL textures and the PBOs in the pipeline are all
     * sized for the previous raster and would deliver torn or
     * mis-sized pictures otherwise.
     *
     * The render thread, which owns the GL context and is therefore
     * the only place GL resources can be re-allocated, calls this
     * once per ProcessOpenGL. When it returns true, the caller is
     * expected to:
     *   1. Stop and destroy this capture object.
     *   2. Re-allocate the upload pipeline (TextureUploader,
     *      SharedTextureRing, FrameQueue) at the new dimensions.
     *   3. Construct and start a new AJACaptureSource sized to the
     *      new raster.
     *
     * @param outWidth      Receives the new raster width in pixels.
     * @param outHeight     Receives the new raster height in pixels.
     * @param outRawFormat  Receives the raw NTV2VideoFormat enum value
     *                      so the caller can decode the full
     *                      CapturedFormat (frame rate, name) without
     *                      having to probe the card again.
     * @return True when a change was pending and has been consumed;
     *         the three out-parameters are then populated. False when
     *         no change is pending and the caller can proceed
     *         normally; the out-parameters are then untouched.
     */
    bool ConsumePendingFormatChange(int& outWidth,
                                    int& outHeight,
                                    int& outRawFormat) noexcept;

private:
    void CaptureLoop();
    bool TryConfigureChannel();   // Capture-thread only.
    void TeardownChannel();       // Capture-thread only.

    /**
     * @brief Read back and log the current effective configuration of
     *        the active FrameStore channel: video format, frame buffer
     *        format, the FrameStore input crosspoint, and the
     *        AutoCirculate status. Used both immediately after a
     *        successful setup (with tag "cfg-readback") and
     *        periodically inside the capture loop (with tag
     *        "diag-readback"). Comparing values across consecutive
     *        diag-readback emissions surfaces silent state corruption
     *        by another application that holds a co-operative claim
     *        on the same card (eg. Resolume's built-in AJA integration,
     *        which acquires the card at host startup regardless of
     *        whether the user has it configured as an output).
     *
     * @param[in] card       Pointer to the active CNTV2Card. Must be
     *                       non-null and the underlying device must
     *                       be open. No internal null-check.
     * @param[in] tag        Short tag string used as a log line
     *                       prefix discriminator. Caller-owned; the
     *                       function does not retain it.
     *
     * Thread safety: capture-thread only. All member reads/writes
     * touched here run only on the capture thread or are publishing
     * atomics whose value lifetime exceeds this call.
     */
    void EmitChannelReadback(class CNTV2Card* card,
                             const char*       tag);
    void ApplyThreadPriorityAndAffinity();

public:
    /// v0.40: another application (or AJA's retail services) using what
    /// this capture needs. Bits returned by RecentForeignUse().
    static constexpr uint32_t kForeignSdiOutput         = 1u;  ///< input connector is an SDI output
    static constexpr uint32_t kForeignFrameStorePlayout = 2u;  ///< our FrameStore plays out for someone
    static constexpr uint32_t kForeignFrameStoreCapture = 4u;  ///< our FrameStore was capturing for someone
    static constexpr uint32_t kForeignTaskMode          = 8u;  ///< AJA services were in control

    /// @return kForeign* bits seen in the last ~3 s. Any thread.
    uint32_t RecentForeignUse() const noexcept;

    /// v0.41: short, lower-case-friendly reason the capture is not
    /// running (e.g. "routing failed"), or nullptr when the last
    /// configure attempt succeeded or simply found no signal. Points to
    /// a string literal; safe from any thread. Shown in the Format
    /// readout and the stats overlay so the cause is visible without
    /// the log.
    const char* StatusReason() const noexcept
    { return m_uiFailText.load(std::memory_order_acquire); }

    /// v0.41: a clip showing this capture was rendered at nowMs (steady
    /// clock, ms), as render number renderTick of the process (see
    /// MultiFormatRegistry::AdvanceRenderTick). Called by
    /// SharedCapture::Update; any thread.
    void NoteRendered(int64_t nowMs, uint64_t renderTick) noexcept
    {
        m_lastRenderTick.store(renderTick, std::memory_order_release);
        m_lastRenderMs.store(nowMs, std::memory_order_release);
    }

    /// v0.41: true once this capture has given its FrameStore up
    /// (MaybeYield): AutoCirculate stopped and the FrameStorePool claim
    /// already released BY THE CAPTURE THREAD. The
    /// owner must then forget its FrameStore index without releasing
    /// it again (it may belong to another clip by now). Read it after
    /// Stop() for a stable answer.
    bool HasYielded() const noexcept
    { return m_yielded.load(std::memory_order_acquire); }

private:
    /// Record a foreign-use observation (capture thread); logs the first
    /// one of each kind per capture session.
    void NoteForeignUse(uint32_t bit, const char* what);
    std::atomic<int64_t> m_foreignSeenMs[4] = {};
    uint32_t             m_foreignLogged     = 0;   // capture thread only
    // v0.42: "RGB HDMI on a YCbCr-only HDMI input" logged once.
    bool                 m_loggedHdmiYuvOnly = false;
    // Connectors this capture switched to receive (bit = NTV2Channel).
    // v0.42: a mask, for the four connectors of a quad-link input.
    uint32_t             m_bncSwitchedMask = 0u;

    /// v0.40: log (once per distinct reason) why TryConfigureChannel
    /// failed; always returns false so call sites can `return` it.
    /// v0.41: uiText is the short reason shown on screen (nullptr for
    /// plain "no signal").
    bool ConfigFail(int inputNumber, const char* reason, int value,
                    const char* uiText);
    /// v0.40: re-arm ConfigFail; logs the recovery after failures.
    void ConfigSucceeded(int inputNumber, int formatRaw);
    const char* m_lastConfigFailReason = nullptr;  // capture thread only
    int         m_lastConfigFailValue  = 0;
    std::atomic<const char*> m_uiFailText{nullptr};

    /// v0.41: give the FrameStore up when a clip being shown needs one and
    /// no clip of this capture has been rendered for a while (see
    /// MultiFormatRegistry). Capture thread; rate-limited.
    /// @return true if this capture just yielded.
    bool MaybeYield();
    /// v0.41: stop AutoCirculate, release the format entry and the
    /// FrameStorePool claim, and park. Capture thread.
    void YieldFrameStore(const char* why);
    std::atomic<int64_t> m_lastRenderMs{0};
    std::atomic<uint64_t> m_lastRenderTick{0};
    std::atomic<bool>    m_yielded{false};
    int64_t              m_nextYieldCheckMs = 0;   // capture thread only
    /// Undo the per-thread scheduling registration (MMCSS on Windows).
    /// Capture thread only, right before it exits.
    void RevertThreadScheduling();

    // MMCSS task handle and the avrt.dll module that created it
    // (Windows only; void* so this header stays free of windows.h).
    void* m_mmcssTask   = nullptr;
    void* m_mmcssModule = nullptr;

    // Shared helpers used by both transfer paths inside the Running
    // state of CaptureLoop. Extracting these keeps a single source of
    // truth for the contract the render thread relies on: a state
    // machine that always transitions to NoSignal on lock loss, and
    // always publishes raster-change atomics in the same order. All
    // helpers are capture-thread-only; they read and publish atomics
    // so they are technically thread-safe, but in practice no other
    // thread invokes them.
    //
    // Note on signatures: we accept enum values as raw ints to avoid
    // pulling AJA SDK headers into this plugin header. CNTV2Card is
    // forward-declared in AJADevice.h (already transitively visible).
    // The int parameters are cast back to NTV2InputSource inside the
    // implementations.

    /**
     * @brief Run the "wait returned but no frame is available" path.
     *        Probes the input format on the physical SDI/HDMI source,
     *        publishes a raster-change atomic when the lock came back
     *        with a different size, transitions to NoSignal otherwise.
     *        Always bumps the idle-wakeup counter. The caller is
     *        expected to `continue` the capture loop immediately after
     *        invoking this helper.
     *
     * @param card                The opened CNTV2Card pointer.
     * @param inputSourceRaw      The physical input source being
     *                            polled, as raw NTV2InputSource int
     *                            (cable-facing source, not the
     *                            FrameStore).
     * @param lastNoSignalRetry   In/out: timestamp the capture loop
     *                            uses to throttle retries while in
     *                            NoSignal. Updated to "now" when the
     *                            transition to NoSignal fires.
     */
    void HandleNoFrameAvailable(
        CNTV2Card*                                   card,
        int                                          inputSourceRaw,
        std::chrono::steady_clock::time_point&       lastNoSignalRetry);

    /**
     * @brief Sample SDI input statistics if the periodic interval has
     *        elapsed. Updates m_sdiLocked, m_sdiCrcErrors, m_sdiUnlocks
     *        when a sample is taken; no-op on cards without SDI error
     *        checks, on HDMI inputs, or when the counter has not yet
     *        reached the interval. framesSinceSdiSample is reset to
     *        zero each time a sample is captured.
     *
     * @param card                  The opened CNTV2Card pointer.
     * @param isSDI                 True iff the active input is SDI.
     * @param canSdiChecks          Card-level capability bit cached
     *                              once at Init time.
     * @param inputSourceRaw        Raw NTV2InputSource int identifying
     *                              the row in NTV2SDIInStatistics.
     * @param framesSinceSdiSample  In/out counter. Caller is
     *                              responsible for the pre-increment;
     *                              this helper resets to 0 after
     *                              sampling.
     */
    void SampleSDIStatisticsIfDue(
        CNTV2Card*       card,
        bool             isSDI,
        bool             canSdiChecks,
        int              inputSourceRaw,
        uint32_t&        framesSinceSdiSample);

    /// v0.42: read the HDR transfer the input signals into
    /// m_signalledTransfer (capture thread).
    void SampleSignalledTransfer(CNTV2Card* card, int inputNumber);

    /// v0.42: the format the input carries now, as the capture configures
    /// it: Level B folded to A, and for a quad-link input the 4K picture of
    /// its links (0 when they do not make one). Capture thread.
    int ProbeLiveFormat(CNTV2Card* card, int inputNumber) const;

    /// v0.42: undo TSI / quad mode a 4K capture set on its FrameStores.
    void ClearMultiFrameStoreMode(CNTV2Card* card);

    /// v0.42: before an HD capture on a card without 12G routing, switch
    /// TSI / quad mode off for the FrameStore's pair / group, unless
    /// another FrameStore of it runs AutoCirculate (someone's 4K).
    void ClearStaleMultiFrameStoreModes(CNTV2Card* card, int fsZeroBased);

    // Wired up from AJAInputPlugin. Must outlive the capture thread.
    AJADevice&         m_device;
    FrameQueue&        m_queue;
    SharedTextureRing* m_textureRing;

    // Secondary GL context, shared with the host (Resolume) primary
    // context. The capture thread activates this on entry to Run()
    // before calling any function that ends up touching GL — most
    // critically m_textureRing->Upload(), which is otherwise a silent
    // no-op (every gl* call drops on the floor when no context is
    // current to the thread, the texture stays at its glTexImage2D
    // -allocated zero state, the shader samples zeros, and BT.709
    // limited yields a uniform green G≈0.30 / R=B=0). nullptr is
    // legal: it disables off-thread upload at runtime and falls
    // through to the byte-queue path, identical to the synthetic
    // producer's fallback.
    SharedGLContext*   m_sharedContext = nullptr;

    std::size_t        m_width;
    std::size_t        m_height;
    std::size_t        m_frameBytes;
    int                m_frameStoreZeroBased;
    uint32_t           m_frameStoreMask;          // v0.42: all FrameStores held
    // v0.42: topology of the configured session (capture thread).
    Topology           m_activeTopology = Topology::Single;
    std::atomic<int>   m_signalledTransfer { 0 };
    uint32_t           m_framesSinceTransferSample = 0;  // capture thread

    // Raw NTV2VideoFormat the capture instance was constructed for.
    // Used by the format-change detection in TryConfigureChannel and in
    // the Running-state probe: a mismatch on the integer raw format is
    // sufficient to dispatch a raster-change reallocation even when the
    // pixel dimensions of the active raster are unchanged. The classic
    // case this catches is a 1080p30 → 1080p60 transition (or vice
    // versa): both rasters are 1920x1080 so a dimensions-only check
    // would silently keep the FrameStore configured for the old rate,
    // resulting in a capture that runs at half the expected rate and
    // delivers torn or zero-filled frames. Zero means "no preference"
    // (legacy callers, tests) and disables the raw-format gate.
    int                m_initialNtv2FormatRaw = 0;

    // One-shot diagnostic: armed at the end of every successful
    // TryConfigureChannel; the capture loop checks it at the first
    // successful AutoCirculateTransfer, dumps the first 16 bytes of
    // the host-side DMA buffer in hex, and disarms the flag. Lets us
    // observe whether the FrameStore is delivering actual pixel data
    // or zeros without having to break out a debugger or instrument
    // the GL pipeline. The atomic is touched on both threads so it
    // needs acquire/release semantics; payload is otherwise trivial.
    std::atomic<bool>  m_armFirstFrameDump { false };

    // Captured at the end of every successful TryConfigureChannel:
    // the FrameStore input crosspoint we wired up and the SDI/HDMI
    // output crosspoint we expected it to be fed from. Both are used
    // by EmitChannelReadback() to verify that the routing has not
    // been silently rewired by another application (Resolume's
    // built-in AJA integration, or a backgrounded AJA Agent) since
    // setup. NTV2_XPT_INVALID sentinel means "not configured yet".
    int                m_expectedFbInXpt = -1;
    int                m_expectedFbOutXpt = -1;
    int                m_activeFsChannel = -1;

    // Tick counter for the periodic diag-readback emitted from the
    // capture loop. Bumped every successful AutoCirculateTransfer
    // and reset every 120 transfers (~2 s at 60 Hz, ~4 s at 30 Hz),
    // at which point EmitChannelReadback runs again with the
    // "diag-readback" tag. Capture-thread local; no atomic needed.
    int                m_periodicReadbackTick = 0;

    // Host-side DMA destination buffer.
    std::vector<uint8_t> m_transferBuffer;

    // v0.32 per-field DMA support.
    //
    // When the active video format is interlaced AND the off-thread
    // upload path is in use AND the pixel kind is UYVY, the standard
    // AutoCirculate path switches to FIELD mode (AUTOCIRCULATE_WITH_FIELDS):
    // each AutoCirculateTransfer delivers ONE FIELD's worth of contiguous
    // pixel data instead of a woven frame. This shaves ~1 display frame
    // off the YADIF read-ahead by letting us publish the new field to
    // the ring as soon as it arrives, instead of waiting for the
    // complete frame.
    //
    // m_fieldModeActive is the capture-thread-local snapshot decided
    // at every Initializing transition. It mirrors the conditions
    // listed above; the value is read by the capture loop on every
    // iteration to pick between the field-mode and frame-mode
    // branches.
    //
    // m_tffSource is also a capture-thread-local snapshot derived from
    // the active video format. Mirrors AJAInputPlugin's BFF heuristic
    // (height <= 486 ⇒ BFF). Used to map AC's F0/F1 (first / second
    // arrived) onto top / bottom field row offsets when scattering.
    //
    // m_fieldAccumBuffer is the per-instance scratch buffer that
    // accumulates BOTH fields of a frame at every-other-line layout.
    // After each field DMA, the contiguous half-frame bytes are
    // memcpy'd into this buffer at the appropriate row stride, and
    // the WHOLE buffer (full-frame size) is then handed to
    // SharedTextureRing::Upload along with a FieldHint identifying
    // which half was freshly written. The complementary half of the
    // texture still carries the previous publish's data — that's
    // exactly the half the YADIF render path won't sample on the
    // immediately following render, so it's safe.
    //
    // Allocated lazily on the first field-mode iteration; resized on
    // any subsequent format change that lands the capture session in
    // field mode at a different resolution. Pre-allocated to be exactly
    // m_frameBytes in size.
    bool                 m_fieldModeActive = false;
    bool                 m_tffSource       = true;  // updated per session

    // v0.37: capture-loop dispatch flag derived from
    // m_activeLowLatencyMode + m_fieldModeActive. True ⇒ run the AC
    // path (frame mode OR field mode depending on
    // m_fieldModeActive); False ⇒ run the manual LL path.
    //
    // Decision rule, applied at every session re-init:
    //   - If the source is interlaced + UYVY (m_fieldModeActive
    //     would be true): force m_useACPath = true even when the
    //     user asked for LL mode. Reason: AC field mode publishes
    //     at field-rate (60 Hz for 1080i59.94), which both feeds
    //     YADIF the per-field samples it can use earlier AND
    //     matches Resolume's compositor cadence at 60 Hz. The
    //     manual LL path is frame-rate-only (30 Hz publish for
    //     1080i59.94) and ends up SLOWER than AC field mode for
    //     interlaced content because of the publish/compositor
    //     cadence mismatch.
    //   - Otherwise: m_useACPath = !m_activeLowLatencyMode (the
    //     legacy semantic: LL toggle picks LL manual path).
    //
    // The user-facing "Low Latency" toggle still exists and still
    // means "give me minimum latency"; for interlaced UYVY sources
    // we just override the implementation to whichever path actually
    // delivers that. A log line on each session start records the
    // override so it doesn't surprise anyone reading the log.
    bool                 m_useACPath       = true;
    std::vector<uint8_t> m_fieldAccumBuffer;

    // v0.32 robustness fix: monotonic per-SOURCE-frame counter,
    // capture-thread-local. Ticks once per source frame regardless of
    // capture mode:
    //   - In frame mode: incremented after every successful
    //     AutoCirculateTransfer (each iteration is a frame).
    //   - In field mode: incremented ONLY on F0 arrivals (each F0
    //     starts a new source frame; F1 continues the same).
    //
    // Passed through the ring as ReadResult::sourceFrameSequence so
    // the render thread can decide when to rotate YadifSlots based on
    // "did the source frame advance" rather than "was this a
    // first-field-of-frame publish". This survives a lost F0 publish
    // (superseded by F1 before the render thread acquires): the next
    // acquire sees a sourceFrameSequence that has advanced and rotates
    // correctly even though no F0 was observed. Reset to 0 on every
    // session re-init.
    std::uint64_t        m_captureSourceFrameSeq = 0;

    // Thread state.
    std::thread        m_thread;
    std::atomic<bool>  m_running { false };
    std::atomic<State> m_state   { State::Idle };

    // Reconfigure inputs. The thread reads these whenever it goes through
    // the Initializing branch; the user sets them via SetInputNumber.
    std::atomic<int>   m_targetInputNumber { 1 };
    int                m_activeInputNumber = 0;  // capture-thread local.

    // AutoCirculate ring frame count. Drives the latency / robustness
    // trade-off: small (2) gives the lowest end-to-end latency at the
    // cost of dropping frames when the consumer stalls; larger (up to
    // 7) absorbs longer stalls before drops begin. The atomic is the
    // user-facing target; the capture-thread-local mirror is read at
    // every AutoCirculateInitForInput and only refreshed from the
    // atomic when the loop goes through the Initializing branch, so
    // mid-run changes are picked up on the next re-init (which is
    // triggered by SetRingFrameCount when the value changes).
    std::atomic<int>   m_targetRingFrameCount  { 2 };
    int                m_activeRingFrameCount  = 0;  // capture-thread local.

    // Low-latency mode flag. When true, the capture path replaces
    // AutoCirculate with a manual ping-pong scheme between two
    // device frame slots, mirroring the SDK's ntv2llburn demo. The
    // atomic is the user-facing target; the capture-thread-local
    // mirror is read once per Initializing pass so the loop reads a
    // stable value for the entire capture session. A toggle on the
    // atomic forces the loop back into Initializing the same way a
    // channel switch does.
    std::atomic<bool>  m_lowLatencyMode        { false };
    bool               m_activeLowLatencyMode  = false; // capture-thread local.

    // v0.39: configuration generation. Every setter that changes a
    // target bumps it. The Initializing pass snapshots it before reading
    // the targets and, if it moved while TryConfigureChannel ran (which
    // takes 170 ms or more), re-initialises instead of going to Running
    // with the old configuration. Before v0.39 a change landing during
    // Initializing was silently lost, because the setters only bump the
    // state from Running or NoSignal.
    std::atomic<std::uint32_t> m_configGeneration { 0 };
    // Generation the current session was configured with (capture-
    // thread local). The Running branch re-initialises when the atomic
    // has moved past it, which closes the window between the
    // Initializing re-check and the store of Running.
    std::uint32_t              m_activeConfigGeneration = 0;

    /// Bump the configuration generation and, when the loop is Running
    /// or NoSignal, send it back through Initializing. Uses a CAS so a
    /// concurrent transition to Failed is never overwritten.
    void RequestReinit() noexcept;

    // Source pixel kind. Same atomic + capture-thread-mirror pattern
    // as ring depth and low-latency mode: the construction-time
    // default lands in m_pixelKind, runtime changes via SetPixelKind
    // store atomically, and TryConfigureChannel snapshots into
    // m_activePixelKind for the duration of the next Running session.
    // A toggle on the atomic forces the loop back into Initializing
    // because changing kind also changes the device FrameStore
    // format (NTV2_FBF_8BIT_YCBCR vs NTV2_FBF_10BIT_YCBCR) and the
    // size of m_transferBuffer; neither can be touched while the
    // capture loop is in Running.
    std::atomic<int>   m_pixelKind             { 0 };   // PixelKind enum cast to int.
    PixelKind          m_activePixelKind       = PixelKind::UYVY;

    // Ping-pong state when m_activeLowLatencyMode is true. Both are
    // capture-thread-local and only meaningful inside the Running
    // state. m_inputStartFrame is the lower of the two reserved
    // Low-latency ping-pong state. AutoCirculateInitForInput reserves
    // N contiguous device frame slots (queried from
    // AutoCirculateGetStatus during Initializing); slot 0 is at
    // m_inputStartFrame, slot k at m_inputStartFrame + k.
    //
    // m_llCycleSlot tracks the slot the hardware is currently writing
    // to in this iteration's interval — i.e. the value that the
    // previous iteration's SetInputFrame call passed to the device.
    // After each WaitForInputFieldID, the "just completed" slot is
    // (m_llCycleSlot + N - 1) % N (the one BEFORE the current
    // cycleSlot in the rotation), and that is what we DMA-read into
    // m_currentInFrame. We then advance m_llCycleSlot by +1 mod N
    // and SetInputFrame to the new slot, scheduling the hardware to
    // switch at the next vblank.
    //
    // With N=2 the rotation is a strict ping-pong (slot 0 ↔ slot 1)
    // and the DMA target ends up being one slot behind the cycle
    // position, identical to v0.22's `m_currentInFrame ^= 1`. With
    // N=3 the third slot acts as a buffer cushion: at any given
    // moment one slot is being written, one was just completed and
    // is being DMA-read, and one is idle. That idle slot is the
    // slack that buys a stalled consumer one extra frame of safety
    // before hardware wraps around and overwrites a frame mid-DMA.
    // Either way, steady-state latency is one frame interval; only
    // the buffer slack differs.
    uint32_t           m_inputStartFrame       = 0;
    uint32_t           m_currentInFrame        = 0;
    uint32_t           m_llCycleSlot           = 0;  // 0..llN-1
    // v0.40: true until the first LL iteration of a session has run;
    // that iteration's DMA reads a never-written slot and is not
    // published.
    bool               m_llWarmupSkip          = false;
    // v0.40: one-shot log gate for a field-mode transfer that carries
    // neither field flag (capture thread only; re-armed per session).
    bool               m_fieldFlagAnomalyLogged = false;

    // Stats and last-seen state.
    std::atomic<int>      m_lastVideoFormatRaw { 0 };
    std::atomic<uint64_t> m_framesCaptured     { 0 };
    std::atomic<uint64_t> m_driverDrops        { 0 };
    // v0.40: transfers done without waiting for an interrupt because the
    // previous one left frames in the driver ring (backlog drains).
    std::atomic<uint64_t> m_backlogDrains      { 0 };
    // v0.40: transfers discarded because the driver handed us the slot
    // still being recorded (only happens after a driver drop).
    std::atomic<uint64_t> m_activeSlotSkips    { 0 };
    std::atomic<uint64_t> m_idleWakeups        { 0 };
    std::atomic<uint64_t> m_recoveryCount      { 0 };

    // RP188 timecode atomically published by the capture thread.
    // The 32-bit packed layout is (hours<<24) | (minutes<<16) |
    // (seconds<<8) | frames. Decoding from the raw register bits
    // happens once per captured frame on the capture thread using
    // the SDK's CRP188 helper, so the render thread can format it
    // with a single snprintf without ever touching an AJA header.
    std::atomic<uint32_t> m_lastTimecodeHMSF  { 0 };
    std::atomic<bool>     m_lastTimecodeValid { false };

    // SDI input statistics sampled periodically (every ~1 s) when the
    // active source is SDI and the card supports SDI error checks.
    // Held at sane defaults (locked=true, errors=0) for HDMI / unsupported
    // cards so the overlay does not flag spurious problems.
    std::atomic<bool>     m_sdiLocked    { true  };
    std::atomic<uint32_t> m_sdiCrcErrors { 0     };
    std::atomic<uint32_t> m_sdiUnlocks   { 0     };

    // Pending raster-size change. The capture thread observes the input
    // delivering a different raster than the one this object was
    // constructed for; rather than silently sitting in NoSignal until a
    // plugin reload, it publishes the new geometry here and the render
    // thread tears the object down and re-creates it at the new size
    // on its next ProcessOpenGL turn.
    //
    // m_pendingPacked packs (width << 16) | (height & 0xFFFF). Both
    // dimensions fit comfortably below 16 bits for everything up to and
    // including 8K (7680 x 4320). Packing them into a single 32-bit
    // atomic guarantees that the render thread sees a consistent pair
    // even if the capture thread is in the middle of publishing.
    //
    // m_pendingRawFormat carries the underlying NTV2VideoFormat value
    // so the consumer can re-decode the full CapturedFormat (frame
    // rate, interlace flag, pretty name) without re-probing the card.
    //
    // m_pendingFormatChange is the gate: store-release on publish,
    // load-acquire on consume. The release/acquire pair establishes
    // happens-before for the geometry and raw-format atomics so the
    // consumer is guaranteed to see the freshly-published values.
    std::atomic<uint32_t> m_pendingPacked       { 0 };
    std::atomic<int>      m_pendingRawFormat    { 0 };
    std::atomic<bool>     m_pendingFormatChange { false };
};

} // namespace ajainput
