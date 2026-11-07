// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    SharedCapture.h
 * @brief   One capture per card input, shared by every clip that shows it.
 *
 * Resolume's own live inputs open each device input once and let every
 * clip of that source show the same stream. An FFGL source plugin does
 * not get that for free: each clip is an independent plugin instance,
 * and up to v0.40 each one opened its own capture, with its own
 * FrameStore, routed from the same connector. Two clips on SDI1 meant
 * two FrameStores reading one input, which on the Kona X stalled one of
 * them, and two FrameStores were gone for a card that only has two.
 *
 * A SharedCapture owns everything that exists once per input: the AJA
 * device handle, the FrameStore claim, the capture thread (or the
 * synthetic generator when no card opens), the texture ring and the
 * byte-queue fallback with its uploader, and the capture settings (Ring
 * Depth, Low Latency Mode, 10-bit) that, as in Resolume, apply to every
 * clip of the input. Each AJAInputPlugin (clip) attaches to the capture of
 * its (device, Input) and keeps only what is per clip: shader, colour
 * and de-interlace settings, YADIF history, overlay.
 *
 * Lifetime: AJAInputPlugin holds a std::shared_ptr; the process-wide
 * registry only a std::weak_ptr, so the capture is destroyed when the
 * last clip detaches, on the render thread with the host GL context
 * current (DeInitGL or ProcessOpenGL), which its GL resources need.
 *
 * Threading: everything is render-thread only except the setters, the
 * Settings*() getters and Snapshot(), which any thread may call (the
 * host polls text parameters from its GUI thread).
 *
 * The registry key includes the GL context current at attach time, so
 * clips rendered in different, unshared GL contexts never share
 * textures (Resolume uses one context; this is a safety net).
 */
#pragma once

#include "AJACaptureSource.h"
#include "AJADevice.h"
#include "FrameProducer.h"
#include "FrameQueue.h"
#include "PixelFormat.h"
#include "SharedGLContext.h"
#include "SharedTextureRing.h"
#include "TextureUploader.h"
#include "VideoFormat.h"

#include <FFGLSDK.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ajainput
{

/// Capture settings shared by every clip of one input.
struct CaptureSettings
{
    int  ringFrameCount = 2;      ///< AutoCirculate ring, 2..7
    bool lowLatency     = false;  ///< manual ping-pong path
    bool tenBit         = false;  ///< v210 when the card can
};

class SharedCapture : public std::enable_shared_from_this<SharedCapture>
{
public:
    /**
     * @brief Find the capture of (device, input) for the GL context
     *        current on this thread, or create and start it.
     * @param settingsIfNew  used only when a new capture is created
     * @param outCreated     true when this call created it
     * Render thread, host GL context current.
     */
    static std::shared_ptr<SharedCapture> Attach(uint32_t               deviceIndex,
                                                 int                    inputNumber,
                                                 const CaptureSettings& settingsIfNew,
                                                 bool&                  outCreated);

    ~SharedCapture();

    /**
     * @brief Close the card handles that captures destroyed since the last
     *        call left open for reuse. A capture does not close its card
     *        when it goes: the next capture of the same card (a clip
     *        switching from SDI1 to SDI2) takes the open handle instead of
     *        reopening the card, which would also hand AJA's stream claim
     *        and task mode back and forth. Call after detaching / attaching
     *        (render thread).
     */
    static void CloseSpareDevices();

    SharedCapture(const SharedCapture&)            = delete;
    SharedCapture& operator=(const SharedCapture&) = delete;

    /**
     * @brief Per-frame housekeeping, called by every attached clip at the
     *        start of its ProcessOpenGL (several times per host frame is
     *        fine: work only happens when something is pending). Marks the
     *        input as on screen, applies setting changes, re-sizes for a
     *        new input format or pixel kind, rebuilds a capture that lent
     *        its FrameStore, retries a capture waiting for a FrameStore,
     *        moves SDI N to FrameStore N when it frees up, refreshes the
     *        frame-rate clash check.
     */
    void Update();

    /// One acquired frame. texture is valid for this ProcessOpenGL call.
    struct Frame
    {
        bool     valid        = false;   ///< a frame to draw
        bool     needsEndRead = false;   ///< ring slot acquired: call EndRead()
        GLuint   texture      = 0;
        bool     fromRing     = false;   ///< ring texture (YADIF-capable layout)
        bool     fencePending = false;
        uint64_t publishSequence = 0;    ///< unique per publish; per-clip freshness
        SharedTextureRing::FieldHint fieldHint = SharedTextureRing::FieldHint::FullFrame;
        bool          isFirstFieldOfFrame  = false;
        std::uint64_t sourceFrameSequence  = 0;
        std::chrono::steady_clock::time_point producedAt  = {};
        std::chrono::steady_clock::time_point publishedAt = {};
    };

    /// Latest frame (render thread). Nothing while Offline().
    Frame Acquire();
    /// After the clip's last command reading a ring frame (render thread).
    void EndRead();

    // ----- render-thread state ---------------------------------------
    const CapturedFormat& Format() const noexcept { return m_format; }
    /// Bumped whenever the frame layout (raster or pixel kind) changes.
    uint64_t  FormatGeneration() const noexcept { return m_formatGeneration; }
    PixelKind Kind() const noexcept { return m_allocatedKind; }
    bool      OffThreadUpload() const noexcept { return m_useOffThreadUpload; }
    /// A card capture exists (or is waiting for a FrameStore) but delivers
    /// nothing right now: draw the neutral background.
    bool      Offline() const noexcept;
    /// The capture thread, or nullptr (synthetic, waiting). Valid until the
    /// next Update() of any clip.
    AJACaptureSource* Capture() noexcept { return m_capture.get(); }
    bool      IsSynthetic() const noexcept { return m_producer != nullptr; }
    bool      WaitingForFrameStore() const noexcept
    { return m_waitingForFrameStore.load(std::memory_order_acquire); }
    bool      NoRoutableFrameStore() const noexcept
    { return m_noRoutableFrameStore.load(std::memory_order_acquire); }
    int       FrameStoreIndex() const noexcept { return m_frameStoreIndex; }
    /// v0.42: every FrameStore held (a pair or four for 4K on cards
    /// without 12G routing); 0 when none.
    uint32_t  FrameStoreMask() const noexcept { return m_frameStoreMask; }
    /// v0.42: the card runs one video format on all its FrameStores.
    bool      SingleFormatCard() const noexcept
    { return m_device.IsOpen() && !m_device.GetStatus().canDoMultiFormat; }
    /// v0.42: HDR transfer the input signals (0 SDR / none, 1 PQ, 2 HLG);
    /// see AJACaptureSource::SignalledTransfer. Render thread.
    int       SignalledTransfer() const noexcept
    { return m_capture ? m_capture->SignalledTransfer() : 0; }
    /// Counts captured frames (AJA) or uploads (synthetic); frame-mode
    /// field parity anchors on it.
    uint64_t  CapturedFrameCount() const noexcept;
    const SharedTextureRing& Ring() const noexcept { return m_ring; }
    const FrameQueue&        Queue() const noexcept { return m_queue; }

    /// Frame-rate clash with another input on the card (any thread):
    /// the other input's NTV2VideoFormat and 1-based input, else 0.
    int RateClashFormatRaw() const noexcept
    { return m_rateClashFormatRaw.load(std::memory_order_relaxed); }
    int RateClashInput() const noexcept
    { return m_rateClashInput.load(std::memory_order_relaxed); }

    /// Clips attached (AJAInputPlugin calls ClipAttached / ClipDetached;
    /// shared_ptr use counts also include temporaries). Any thread.
    void ClipAttached() noexcept { m_clipCount.fetch_add(1, std::memory_order_acq_rel); }
    void ClipDetached() noexcept { m_clipCount.fetch_sub(1, std::memory_order_acq_rel); }
    int  Clips() const noexcept  { return m_clipCount.load(std::memory_order_acquire); }

    uint32_t DeviceIndex()   const noexcept { return m_deviceIndex; }
    int      InputNumber() const noexcept { return m_inputNumber; }

    // ----- settings (any thread) -------------------------------------
    CaptureSettings Settings() const noexcept;
    /// Bumped by every setter that changed something.
    uint64_t SettingsGeneration() const noexcept
    { return m_settingsGeneration.load(std::memory_order_acquire); }
    void SetRingFrameCount(int ringFrameCount) noexcept;
    void SetLowLatency(bool enabled) noexcept;
    void SetTenBit(bool enabled) noexcept;

    // ----- status for the host's GUI thread (any thread) -------------
    struct Status
    {
        bool deviceOpen     = false;
        bool synthetic      = false;
        bool hasCapture     = false;
        bool waitingForFrameStore = false;
        bool noRoutableFrameStore = false;   ///< routing ROM rules all out
        bool yielded        = false;
        AJACaptureSource::State state = AJACaptureSource::State::Idle;
        int  frameStore     = -1;        ///< zero-based, -1 none
        uint32_t frameStoreMask = 0;     ///< v0.42: all held (4K sets)
        bool singleFormatCard = false;   ///< v0.42: card runs one format
        int  signalledTransfer = 0;      ///< v0.42: 0 SDR, 1 PQ, 2 HLG
        int  inputNumber  = 0;
        int  lastFormatRaw  = 0;
        const char* reason  = nullptr;   ///< AJACaptureSource::StatusReason
        uint32_t foreignMask = 0;        ///< AJACaptureSource::kForeign*
        uint64_t frames = 0, drops = 0, recoveries = 0, idleWakeups = 0;
        int  rateClashFormatRaw = 0;
        int  rateClashInput     = 0;
        std::size_t width = 0, height = 0;
        double      fps   = 0.0;
        const char* openStatus = "";
        int  clips = 0;                  ///< clips attached
    };
    Status Snapshot() const;

private:
    SharedCapture(uint32_t deviceIndex, int inputNumber,
                  const CaptureSettings& settings);

    // Setup / teardown (render thread).
    void OpenDeviceAndProbe();
    void StartSource();
    void ReallocateSinks(const CapturedFormat& fmt, PixelKind kind);
    void Reconfigure(const CapturedFormat& fmt);
    PixelKind WantedKind() const noexcept;

    // Synthetic generator.
    void StartSyntheticProducer();
    void StopSyntheticProducer();

    // AJA capture and FrameStore.
    enum class StartResult { Started, NoFrameStore };
    StartResult StartCapture();
    /// v0.42: FrameStores the current input and format need
    /// (CaptureTopology.h).
    FrameStorePlan CurrentPlan() const noexcept;
    int  ClaimFrameStore();
    void ReleaseFrameStore();
    void CreateAndStartCapture();
    void StopCapture();
    void HandleYieldWaitingAndPairing();
    void UpdateRateClash();
    void ApplySettings();

    // Identity.
    const uint32_t m_deviceIndex;
    const int      m_inputNumber;

    // Settings (any thread writes, render thread applies).
    std::atomic<int>      m_targetRingFrameCount;
    std::atomic<bool>     m_targetLowLatency;
    std::atomic<bool>     m_targetTenBit;
    std::atomic<uint64_t> m_settingsGeneration { 1 };
    uint64_t              m_appliedSettingsGeneration = 0;

    // Pipeline. Declaration order: the producer and the capture (which
    // write into the ring and the queue) are stopped explicitly in the
    // destructor before the sinks go. The card handle may come from a
    // capture of the same card destroyed just before (CloseSpareDevices);
    // m_device refers to it for the whole lifetime.
    // Declared (so initialised) before m_devicePtr: TakeSpareOrNew sets it.
    bool                m_reusedDevice;
    std::unique_ptr<AJADevice> m_devicePtr;
    AJADevice&          m_device;
    FrameQueue          m_queue;
    SharedGLContext     m_sharedContext;
    SharedTextureRing   m_ring;
    TextureUploader     m_uploader;
    std::unique_ptr<FrameProducer>    m_producer;
    std::unique_ptr<AJACaptureSource> m_capture;
    bool                m_useOffThreadUpload = false;

    CapturedFormat      m_format = SyntheticFallbackFormat();
    uint64_t            m_formatGeneration = 1;
    PixelKind           m_allocatedKind = PixelKind::UYVY;

    int                 m_frameStoreIndex = -1;
    uint32_t            m_frameStoreMask  = 0u;   // v0.42: every one held
    uint32_t            m_lastForeignLogged = 0;   // FrameStores of other apps, logged
    std::atomic<bool>   m_waitingForFrameStore { false };
    // The crosspoint ROM allows no FrameStore for this input (see
    // ClaimFrameStore).
    std::atomic<bool>   m_noRoutableFrameStore { false };

    // Render-thread timing state.
    std::chrono::steady_clock::time_point m_captureStartedAt {};
    std::chrono::steady_clock::time_point m_nextStartAttempt {};
    std::chrono::steady_clock::time_point m_offlineSince {};
    std::chrono::steady_clock::time_point m_nextPairingCheck {};
    std::chrono::steady_clock::time_point m_nextRateClashCheck {};
    uint64_t            m_lastQueueSequence = ~0ull;  // inline path upload gate

    // Frame-rate clash with another input on the card (atomics: read by
    // Snapshot from the GUI thread).
    std::atomic<int>    m_rateClashFormatRaw { 0 };
    std::atomic<int>    m_rateClashInput     { 0 };

    std::atomic<int>    m_clipCount { 0 };

    // Guards the pointers Snapshot() follows (m_capture, m_producer) and
    // m_format against the render thread swapping them.
    mutable std::mutex  m_statusMutex;
};

} // namespace ajainput
