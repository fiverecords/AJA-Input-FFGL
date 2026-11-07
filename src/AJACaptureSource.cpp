// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    AJACaptureSource.cpp
 * @brief   Implementation of the AJA AutoCirculate capture path.
 */

// On Windows, force <windows.h> to be processed BEFORE any of our other
// includes. Background: SharedTextureRing.h (below) pulls in FFGLSDK.h →
// FFGL.h, which defines a long list of NOxxx macros (NOUSER, NORESOURCE,
// NOCTLMGR, NOWINMESSAGES and ~25 others) intended to slim down
// <windows.h> for FFGL's own purposes. Those suppression macros then
// remain in effect for the rest of the translation unit. When ntv2card.h
// later transitively pulls in commctrl.h / prsht.h via the
// ntv2devicecapabilities.h → ntv2driverinterface.h chain, those headers
// reference types (NMHDR, dialog manager structs, etc.) the FFGL NOxxx
// defines have suppressed, and the build fails with about a hundred
// C3646 "unknown override specifier" errors.
//
// Including <windows.h> as the very first thing this TU does loads the
// full Win32 API up-front, and the header-guard machinery in windows.h
// turns every subsequent inclusion (FFGL's, ajatypes.h's) into a no-op.
// FFGL's NOxxx defines still get set later but no longer have anything
// to suppress, because <windows.h> has already finished processing.
#ifdef _WIN32
#  include <windows.h>
#endif

#include "AJACaptureSource.h"
#include "CaptureTopology.h"
#include "FrameStorePool.h"
#include "InputNTV2.h"
#include "MultiFormatRegistry.h"

#include "FrameQueue.h"
#include "PixelFormat.h"
#include "SharedGLContext.h"
#include "SharedTextureRing.h"
#include "VideoFormat.h"

#include "ntv2card.h"
#include "ntv2devicefeatures.h"
#include "ntv2enums.h"
#include "ntv2publicinterface.h"
#include "ntv2rp188.h"
#include "ntv2signalrouter.h"
#include "ntv2utils.h"
#include "ntv2vpid.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <thread>

namespace ajainput
{
namespace
{

constexpr NTV2FrameBufferFormat kCaptureFBF       = NTV2_FBF_8BIT_YCBCR;
// Ring frame count for AutoCirculate is no longer a compile-time
// constant. It is now driven per-instance by the constructor's
// `ringFrameCount` argument (clamped 2..7) and can be updated at
// runtime through SetRingFrameCount. The user-facing UI dropdown
// "Ring Depth" maps directly onto this value. Default 2 = lowest
// latency that AutoCirculate accepts; users raise it if they observe
// occasional drops under load.
/// @brief Resolve an input number (InputId.h) into the AJA enums and the
///        "is SDI" flag callers need to know whether the BNC direction
///        flip applies. v0.42: SDI 1..8, HDMI 1..4 (outBNC is then the
///        HDMI input's channel, as the SDK's HDMI input calls take), and
///        quad-link inputs (their first link).
bool ResolveInputSelection(int inputNumber,
                           NTV2Channel&      outBNC,
                           NTV2InputSource&  outSource,
                           bool&             outIsSDI) noexcept
{
    return input::ToNTV2(inputNumber, outSource, outBNC, outIsSDI);
}

/// @brief Map a FrameStore zero-based index to the NTV2Channel enum.
///        Cards have 1..N FrameStores; index N-1 is the highest valid
///        value. Out-of-range yields NTV2_CHANNEL1 (safe default that
///        any AJA card has) and false.
bool FrameStoreZeroBasedToNTV2(int zeroBased, NTV2Channel& outChannel) noexcept
{
    switch (zeroBased)
    {
        case 0: outChannel = NTV2_CHANNEL1; return true;
        case 1: outChannel = NTV2_CHANNEL2; return true;
        case 2: outChannel = NTV2_CHANNEL3; return true;
        case 3: outChannel = NTV2_CHANNEL4; return true;
        case 4: outChannel = NTV2_CHANNEL5; return true;
        case 5: outChannel = NTV2_CHANNEL6; return true;
        case 6: outChannel = NTV2_CHANNEL7; return true;
        case 7: outChannel = NTV2_CHANNEL8; return true;
        default: outChannel = NTV2_CHANNEL1; return false;
    }
}

} // namespace

const char* AJACaptureSource::ToString(State s) noexcept
{
    switch (s)
    {
        case State::Idle:         return "idle";
        case State::Initializing: return "init";
        case State::Running:      return "running";
        case State::NoSignal:     return "no signal";
        case State::Failed:       return "failed";
    }
    return "?";
}

AJACaptureSource::AJACaptureSource(AJADevice&         device,
                                   FrameQueue&        queue,
                                   SharedTextureRing* textureRing,
                                   SharedGLContext*   sharedContext,
                                   std::size_t        width,
                                   std::size_t        height,
                                   int                frameStoreZeroBased,
                                   int                initialNtv2FormatRaw,
                                   int                ringFrameCount,
                                   bool               lowLatencyMode,
                                   PixelKind          pixelKind,
                                   uint32_t           frameStoreMask)
    : m_device(device)
    , m_queue(queue)
    , m_textureRing(textureRing)
    , m_sharedContext(sharedContext)
    , m_width(width)
    , m_height(height)
    , m_frameBytes(FrameSize(pixelKind, width, height))
    , m_frameStoreZeroBased(frameStoreZeroBased)
    , m_frameStoreMask(frameStoreMask != 0u
                           ? frameStoreMask
                           : ((frameStoreZeroBased >= 0 && frameStoreZeroBased < 32)
                                  ? (1u << frameStoreZeroBased) : 0u))
    , m_initialNtv2FormatRaw(initialNtv2FormatRaw)
{
    m_transferBuffer.assign(m_frameBytes, 0);
    // Clamp the requested ring depth into the SDK's usable range. The
    // floor is 2 (AutoCirculate refuses to init with fewer); the
    // ceiling 7 matches what we expose in the UI dropdown and the
    // SDK demo's default. Values outside the range silently snap to
    // the nearest bound rather than asserting because the caller is
    // also a string-derived value from the user's UI selection.
    if (ringFrameCount < 2) ringFrameCount = 2;
    if (ringFrameCount > 7) ringFrameCount = 7;
    m_targetRingFrameCount.store(ringFrameCount, std::memory_order_relaxed);
    m_lowLatencyMode.store(lowLatencyMode, std::memory_order_relaxed);
    m_pixelKind.store(static_cast<int>(pixelKind), std::memory_order_relaxed);
}

AJACaptureSource::~AJACaptureSource()
{
    Stop();
}

void AJACaptureSource::Start()
{
    if (m_running.load(std::memory_order_acquire))
        return;

    m_framesCaptured.store(0, std::memory_order_relaxed);
    m_driverDrops.store   (0, std::memory_order_relaxed);
    m_backlogDrains.store (0, std::memory_order_relaxed);
    m_activeSlotSkips.store(0, std::memory_order_relaxed);
    m_idleWakeups.store   (0, std::memory_order_relaxed);
    m_recoveryCount.store (0, std::memory_order_relaxed);

    m_state.store(State::Initializing, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&AJACaptureSource::CaptureLoop, this);
}

void AJACaptureSource::Stop()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel))
    {
        if (m_thread.joinable())
            m_thread.join();
        m_state.store(State::Idle, std::memory_order_release);
        return;
    }

    if (m_thread.joinable())
        m_thread.join();

    m_state.store(State::Idle, std::memory_order_release);
}

bool AJACaptureSource::ConsumePendingFormatChange(int& outWidth,
                                                  int& outHeight,
                                                  int& outRawFormat) noexcept
{
    // Acquire on the gate: pairs with the release-store in the capture
    // thread that publishes a new pending change. This establishes
    // happens-before for the geometry and raw-format atomics that the
    // capture thread populated before flipping the gate.
    if (!m_pendingFormatChange.load(std::memory_order_acquire))
        return false;

    const uint32_t packed = m_pendingPacked.load(std::memory_order_relaxed);
    outWidth     = static_cast<int>(packed >> 16);
    outHeight    = static_cast<int>(packed & 0xFFFFu);
    outRawFormat = m_pendingRawFormat.load(std::memory_order_relaxed);

    // Clear the gate. A release-store here means any later thread that
    // observes false on the gate definitely sees zeroed-out pending
    // fields if the producer has not yet posted a new change. Belt and
    // braces, since the caller is supposed to destroy this instance
    // anyway right after a successful consume.
    m_pendingFormatChange.store(false, std::memory_order_release);
    return true;
}

void AJACaptureSource::RequestReinit() noexcept
{
    m_configGeneration.fetch_add(1, std::memory_order_acq_rel);

    State current = m_state.load(std::memory_order_acquire);
    while (current == State::Running || current == State::NoSignal)
    {
        if (m_state.compare_exchange_weak(current, State::Initializing,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
            break;
    }
}

void AJACaptureSource::SetInputNumber(int inputNumber) noexcept
{
    if (!input::IsValid(inputNumber))
        return;

    const int previous =
        m_targetInputNumber.exchange(inputNumber,
                                         std::memory_order_acq_rel);
    if (previous == inputNumber)
        return;

    // Force the capture loop back through Initializing on its next turn so
    // it tears down AutoCirculate on the old channel and re-inits on the
    // new one. Done with a relaxed store to a state we know the thread
    // checks unconditionally each iteration.
    RequestReinit();
}

void AJACaptureSource::SetRingFrameCount(int ringFrameCount) noexcept
{
    // Same clamp as the constructor: 2..7. The setter is the one that
    // sees user-driven values from the UI dropdown, so the bounds
    // check is non-optional.
    if (ringFrameCount < 2) ringFrameCount = 2;
    if (ringFrameCount > 7) ringFrameCount = 7;

    const int previous =
        m_targetRingFrameCount.exchange(ringFrameCount,
                                        std::memory_order_acq_rel);
    if (previous == ringFrameCount)
        return;

    // Same trick as SetInputNumber: bump the state machine back to
    // Initializing so the capture loop's next iteration tears down
    // AutoCirculate and re-inits with the new ring frame count. There
    // is a momentary NoSignal-like gap of one or two frames while the
    // driver re-arms the new buffer chain; this is identical to what
    // a channel switch produces and the render path already handles
    // it cleanly (overlay shows "no signal" briefly, no crash).
    RequestReinit();
}

void AJACaptureSource::SetLowLatencyMode(bool enabled) noexcept
{
    const bool previous =
        m_lowLatencyMode.exchange(enabled, std::memory_order_acq_rel);
    if (previous == enabled)
        return;

    // Toggling the mode requires reconfiguring the channel from
    // scratch: low-latency uses two reserved slots and manual
    // SetInputFrame ping-pong, the standard path uses AutoCirculate
    // with a ring of N. They cannot coexist on the same FrameStore.
    // Same Initializing-bump trick as the other live-tunable setters.
    RequestReinit();
}

void AJACaptureSource::ApplyThreadPriorityAndAffinity()
{
#ifdef _WIN32
    // Highest practical priority short of TIME_CRITICAL: keeps the capture
    // ahead of Resolume's worker threads without starving the renderer.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // v0.39: no core pinning. Up to v0.38 every capture thread was pinned
    // to the highest-numbered logical core, on the theory that it would
    // be a performance core. On Intel hybrid CPUs Windows numbers the
    // P-cores FIRST, so the highest bit is usually an E-core; and with
    // two plugin instances both capture threads were pinned to that one
    // core, competing for it with the per-field scatter and upload
    // copies. Letting the scheduler place the thread avoids both.
    //
    // Instead, opt the thread out of EcoQoS power throttling where the
    // SDK headers know about it (Windows 10 1709+), which is the
    // documented way to keep a thread off efficiency-class scheduling.
#  if defined(THREAD_POWER_THROTTLING_CURRENT_VERSION)
    THREAD_POWER_THROTTLING_STATE throttling{};
    throttling.Version     = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask   = 0;  // 0 = never throttle execution speed
    ::SetThreadInformation(::GetCurrentThread(), ThreadPowerThrottling,
                           &throttling, sizeof(throttling));
#  endif

    // v0.40: register with MMCSS (Multimedia Class Scheduler Service) as
    // a "Capture" task. MMCSS lifts the thread into the real-time
    // priority range while it runs, so the wake-up after the card's
    // interrupt is not delayed behind Resolume's own HIGHEST-priority
    // workers; the service still reserves CPU for everything else, so
    // the system stays responsive. avrt.dll is loaded dynamically: no
    // new link dependency, and a system without it (or a failing call)
    // simply keeps the HIGHEST priority set above.
    using AvSetFn    = HANDLE (WINAPI*)(LPCWSTR, LPDWORD);
    using AvPrioFn   = BOOL   (WINAPI*)(HANDLE, int);
    // System32 only: never pick up an avrt.dll planted next to the
    // host executable or in the working directory.
    if (HMODULE avrt = ::LoadLibraryExW(L"avrt.dll", nullptr,
                                        LOAD_LIBRARY_SEARCH_SYSTEM32))
    {
        auto avSet  = reinterpret_cast<AvSetFn>(
            ::GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"));
        auto avPrio = reinterpret_cast<AvPrioFn>(
            ::GetProcAddress(avrt, "AvSetMmThreadPriority"));
        DWORD taskIndex = 0;
        HANDLE task = avSet ? avSet(L"Capture", &taskIndex) : nullptr;
        if (task != nullptr)
        {
            if (avPrio)
                avPrio(task, 1 /*AVRT_PRIORITY_HIGH*/);
            m_mmcssTask   = task;
            m_mmcssModule = avrt;   // keep loaded until the revert
        }
        else
        {
            ::FreeLibrary(avrt);
        }
    }
#endif
    // Linux / macOS: no pinning either; the default scheduler is fine
    // for the development builds that run there.
}

// v0.40 diagnostics: say once why configuring the capture fails, instead
// of cycling NoSignal <-> Initializing silently. Logged when the reason
// (or its value) changes; ConfigSucceeded re-arms it and logs the
// recovery after a failure.
bool AJACaptureSource::ConfigFail(int inputNumber, const char* reason,
                                  int value, const char* uiText)
{
    m_uiFailText.store(uiText, std::memory_order_release);
    if (reason != m_lastConfigFailReason || value != m_lastConfigFailValue)
    {
        m_lastConfigFailReason = reason;
        m_lastConfigFailValue  = value;
        char fs[16];
        FormatFrameStoreSet(m_frameStoreMask, fs, sizeof(fs));
        char line[320];
        std::snprintf(line, sizeof(line),
                      "AJAInput: %s -> %s: capture not "
                      "configured: %s [%d]",
                      input::LabelUpper(inputNumber), fs, reason, value);
        FFGLLog::LogToHost(line);
    }
    return false;
}

void AJACaptureSource::ConfigSucceeded(int inputNumber, int formatRaw)
{
    if (m_lastConfigFailReason != nullptr)
    {
        char fs[16];
        FormatFrameStoreSet(m_frameStoreMask, fs, sizeof(fs));
        char line[200];
        std::snprintf(line, sizeof(line),
                      "AJAInput: %s -> %s: capture "
                      "configured (NTV2VideoFormat %d) after earlier "
                      "failures",
                      input::LabelUpper(inputNumber), fs, formatRaw);
        FFGLLog::LogToHost(line);
    }
    m_lastConfigFailReason = nullptr;
    m_lastConfigFailValue  = 0;
    m_uiFailText.store(nullptr, std::memory_order_release);
}

void AJACaptureSource::NoteForeignUse(uint32_t bit, const char* what)
{
    const int idx = (bit == kForeignSdiOutput)          ? 0 :
                    (bit == kForeignFrameStorePlayout)  ? 1 :
                    (bit == kForeignFrameStoreCapture)  ? 2 : 3;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_foreignSeenMs[idx].store(nowMs, std::memory_order_relaxed);
    if ((m_foreignLogged & bit) == 0u)
    {
        m_foreignLogged |= bit;
        char line[320];
        std::snprintf(line, sizeof(line),
                      "AJAInput: FrameStore %d: %s",
                      m_frameStoreZeroBased + 1, what);
        FFGLLog::LogToHost(line);
    }
}

uint32_t AJACaptureSource::RecentForeignUse() const noexcept
{
    constexpr int64_t kWindowMs = 3000;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    static const uint32_t kBits[4] = {
        kForeignSdiOutput, kForeignFrameStorePlayout,
        kForeignFrameStoreCapture, kForeignTaskMode };
    uint32_t mask = 0;
    for (int i = 0; i < 4; ++i)
    {
        const int64_t t = m_foreignSeenMs[i].load(std::memory_order_relaxed);
        if (t != 0 && nowMs - t <= kWindowMs)
            mask |= kBits[i];
    }
    return mask;
}

namespace
{
int64_t SteadyNowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

bool AJACaptureSource::MaybeYield()
{
    // A clip that is on screen renders every frame (16-40 ms); 250 ms
    // without a render of any clip of this input means they are in the
    // deck or their layers are hidden.
    constexpr int64_t kIdleMs = 250;
    // Requests are refreshed every ~100 ms by a waiting input on screen.
    constexpr int64_t kWantWindowMs = 300;

    const int64_t now = SteadyNowMs();
    if (now < m_nextYieldCheckMs)
        return false;
    m_nextYieldCheckMs = now + 50;

    if (now - m_lastRenderMs.load(std::memory_order_acquire) < kIdleMs)
        return false;
    // v0.41: and other clips must have been rendered meanwhile. Wall time
    // alone would also call a capture idle while the whole render thread
    // stalls (another input starting up, a Resolume hitch): every clip is
    // late then, and an input on screen would lend its FrameStore.
    constexpr uint64_t kOtherRenders = 8;
    if (MultiFormatRegistry::RenderTick() -
            m_lastRenderTick.load(std::memory_order_acquire) < kOtherRenders)
        return false;
    const uint32_t dev = m_device.GetStatus().deviceIndex;
    // v0.42: only for a request one of our FrameStores helps.
    if (MultiFormatRegistry::ConsumeFrameStoreWant(dev, now - kWantWindowMs,
                                                   m_frameStoreMask))
    {
        YieldFrameStore("no clip of this input on screen; a clip being "
                        "shown needs a FrameStore");
        return true;
    }
    return false;
}

void AJACaptureSource::YieldFrameStore(const char* why)
{
    if (m_activeInputNumber != 0)
        TeardownChannel();
    MultiFormatRegistry::Withdraw(m_device.GetStatus().deviceIndex,
                                  m_frameStoreZeroBased);
    // Mark first: the owner reads HasYielded() after joining this thread
    // and must then not release the claim a second time. v0.42: every
    // FrameStore of a 4K set.
    m_yielded.store(true, std::memory_order_release);
    FrameStorePool::ReleaseMask(m_device.GetStatus().deviceIndex, m_frameStoreMask);
    m_state.store(State::NoSignal, std::memory_order_release);

    char fs[16];
    FormatFrameStoreSet(m_frameStoreMask, fs, sizeof(fs));
    char line[200];
    std::snprintf(line, sizeof(line),
                  "AJAInput: %s given up (%s)", fs, why);
    FFGLLog::LogToHost(line);
}

bool AJACaptureSource::TryConfigureChannel()
{
    CNTV2Card* card = m_device.Card();
    if (!card)
        return ConfigFail(0, "device not open", 0, "device not open");

    const int requestedInput =
        m_targetInputNumber.load(std::memory_order_acquire);

    // Resolve the input number (InputId.h) into the AJA enums.
    //   physBNCChannel = which physical SDI BNC (only used for SDI; the
    //                    bidir-direction flip lives on this concept). For
    //                    HDMI it is the HDMI input's index, which the
    //                    SDK's HDMI input calls take; for a quad-link
    //                    input, the first of its four connectors.
    //   inputSource    = where the signal enters the routing matrix.
    //   isSDI          = whether the bidir flip applies at all. HDMI is
    //                    unidirectional, no flip needed.
    //   fsChannel      = which FrameStore owns the AutoCirculate ring
    //                    (the lowest of a v0.42 FrameStore set).
    //                    Allocated from FrameStorePool by the owner.
    NTV2Channel     physBNCChannel = NTV2_CHANNEL1;
    NTV2InputSource inputSource    = NTV2_INPUTSOURCE_SDI1;
    bool            isSDI          = true;
    if (!ResolveInputSelection(requestedInput,
                               physBNCChannel, inputSource, isSDI))
        return ConfigFail(requestedInput, "invalid input selection", requestedInput, "invalid input");

    NTV2Channel fsChannel = NTV2_CHANNEL1;
    if (!FrameStoreZeroBasedToNTV2(m_frameStoreZeroBased, fsChannel))
        return ConfigFail(requestedInput, "invalid FrameStore index", m_frameStoreZeroBased, "invalid FrameStore");

    // v0.42: a quad-link input uses four connectors.
    const int  links  = input::ConnectorCount(requestedInput);
    const bool isQuad = input::IsQuad(requestedInput);
    const uint32_t dev = m_device.GetStatus().deviceIndex;

    // Before doing any I/O on the chosen connector, ask the card
    // whether it physically exposes that input source. On a Kona X
    // base with two SDI BNCs the user picking "SDI 3" in the dropdown
    // would otherwise drive us through several driver calls (bidir
    // flip, format probe, etc.) on a non-existent connector before
    // failing somewhere with an unhelpful error code. Bailing here
    // produces a clean NoSignal instead. v0.42: every link of a
    // quad-link input.
    for (int k = 0; k < links; ++k)
    {
        const NTV2InputSource src = input::LinkSource(requestedInput, k);
        if (!NTV2_IS_VALID_INPUT_SOURCE(src) ||
            !::NTV2DeviceCanDoInputSource(card->GetDeviceID(), src))
            return ConfigFail(requestedInput, "card has no such input", static_cast<int>(src), "card has no such input");
    }

    // Probe the actual input format. If no cable is plugged in or the
    // generator on the other side has not locked yet, this returns
    // NTV2_FORMAT_UNKNOWN and we move into NoSignal.
    //
    // The probe returns the receiver's view of the cable, which for a
    // 3G Level B source is one of the NTV2_FORMAT_*_B enums even after
    // the Level B→A conversion is engaged below (because the receiver
    // reports the format derived from the embedded VPID, which is
    // metadata in the source stream and does not change with the
    // conversion register). MapLevelBFormatToLevelA folds those into
    // their Level A equivalents so SetVideoFormat below programs the
    // FrameStore for the *display rate* of the converted stream — i.e.
    // what AutoCirculate will actually deliver — and not for the
    // half-rate wire format the FrameStore would otherwise refuse.
    //
    // v0.40: ownership checks and the receive switch come BEFORE the
    // probe. Up to v0.39 the probe ran first and the bidirectional
    // connector was only switched to receive after it, so an input left
    // (or put back) in transmit mode read as "no signal" forever, and
    // nothing checked whether the FrameStore or connector belonged to
    // another application.
    //
    // (a) AJA retail services. In standard task mode the AJA services
    //     keep applying the Control Panel's routing and formats over
    //     ours; only the default SDI1 -> FrameStore 1 path survives
    //     that. Re-assert OEM tasks and say so.
    bool servicesWereActive = false;
    {
        NTV2TaskMode taskMode = NTV2_OEM_TASKS;
        if (card->GetTaskMode(taskMode) && taskMode != NTV2_OEM_TASKS)
        {
            servicesWereActive = true;
            NoteForeignUse(kForeignTaskMode,
                "AJA retail services were controlling the card (task "
                "mode not OEM); switched back to OEM tasks");
            card->SetTaskMode(NTV2_OEM_TASKS);
        }
    }

    // (b) Our FrameStore already running AutoCirculate. Playout is
    //     never ours (this plugin only captures): leave it alone and
    //     fail with a reason instead of stopping someone's output. A
    //     capture is taken over, as before, but reported. v0.42: every
    //     FrameStore of a 4K set.
    for (int fs = 0; fs < 8; ++fs)
    {
        if ((m_frameStoreMask & (1u << fs)) == 0u)
            continue;
        AUTOCIRCULATE_STATUS fsStatus;
        if (card->AutoCirculateGetStatus(static_cast<NTV2Channel>(fs), fsStatus) &&
            !fsStatus.IsStopped())
        {
            if (fsStatus.IsOutput())
            {
                NoteForeignUse(kForeignFrameStorePlayout,
                    "FrameStore is playing out for another application "
                    "(e.g. Resolume's own AJA output); not touching it");
                return ConfigFail(requestedInput,
                    "FrameStore is used for output by another application",
                    fs + 1, "FrameStore used by another app");
            }
            NoteForeignUse(kForeignFrameStoreCapture,
                "FrameStore already had AutoCirculate running (another "
                "application, or Resolume's own AJA input?); taking it "
                "over");
            // v0.42: the other FrameStores of a 4K set are stopped here,
            // as AJA's ntv2capture4k does for all of them; AutoCirculate
            // on the first one is re-initialised below.
            if (fs != m_frameStoreZeroBased)
                card->AutoCirculateStop(static_cast<NTV2Channel>(fs), /*inAbort*/true);
        }
    }

    // (c) Step 1: a bidirectional SDI connector in transmit mode. Switch
    //     it to receive and give the receiver time to lock (10 output
    //     VBIs, as AJA's NTV2Capture demo), unless it is somebody's
    //     output: its SDI output is fed by a FrameStore another
    //     application plays out on (v0.41; v0.40 refused whenever any
    //     playout ran on the card), or we switched this connector to
    //     receive before and someone put it back to transmit. Only the
    //     connectors of the input the user picked are touched (v0.42:
    //     the four of a quad-link input, one lock wait for all).
    if (isSDI && ::NTV2DeviceHasBiDirectionalSDI(card->GetDeviceID()))
    {
        bool switched = false;
        for (int k = 0; k < links; ++k)
        {
            const NTV2Channel bnc = static_cast<NTV2Channel>(physBNCChannel + k);
            bool transmitting = false;
            card->GetSDITransmitEnable(bnc, transmitting);
            if (!transmitting)
                continue;
            // If the AJA services were in control until a moment ago,
            // they are the likely ones who put it back to transmit; with
            // OEM tasks re-asserted it is ours to switch again.
            const bool weSwitchedItBefore =
                !servicesWereActive &&
                (m_bncSwitchedMask & (1u << static_cast<int>(bnc))) != 0u;
            if (weSwitchedItBefore ||
                m_device.SdiOutputCarriesForeignPlayout(static_cast<int>(bnc)))
            {
                NoteForeignUse(kForeignSdiOutput,
                    weSwitchedItBefore
                        ? "the input connector was switched back to SDI "
                          "output by another application"
                        : "the input connector is an SDI output carrying "
                          "another application's playout");
                return ConfigFail(requestedInput,
                    "input connector is used as an SDI output",
                    static_cast<int>(bnc) + 1,
                    "connector is another app's SDI output");
            }
            card->SetSDITransmitEnable(bnc, false);
            m_bncSwitchedMask |= 1u << static_cast<int>(bnc);
            switched = true;
        }
        if (switched)
        {
            card->SubscribeOutputVerticalEvent(NTV2_CHANNEL1);
            card->WaitForOutputVerticalInterrupt(NTV2_CHANNEL1, 10);
            card->UnsubscribeOutputVerticalEvent(NTV2_CHANNEL1);
        }
    }

    const NTV2VideoFormat fmt =
        static_cast<NTV2VideoFormat>(ProbeLiveFormat(card, requestedInput));
    m_lastVideoFormatRaw.store(static_cast<int>(fmt),
                               std::memory_order_relaxed);
    // v0.40: record what this input carries, so every instance on the
    // card can warn about frame rates the card cannot mix (see
    // MultiFormatRegistry.h). Published as soon as the cable has a
    // format, even if configuring then fails: a clash may be exactly
    // why it fails. Withdrawn when the input loses its format and when
    // the capture loop exits.
    if (fmt == NTV2_FORMAT_UNKNOWN)
    {
        MultiFormatRegistry::Withdraw(dev, m_frameStoreZeroBased);
        if (isQuad &&
            card->GetInputVideoFormat(inputSource, false) != NTV2_FORMAT_UNKNOWN)
            return ConfigFail(requestedInput,
                "quad-link input: the first link is not a 1080p / 2K picture",
                static_cast<int>(card->GetInputVideoFormat(inputSource, false)),
                "quad-link needs 1080p links");
        return ConfigFail(requestedInput, "no signal / format not detected on the input", 0, nullptr);
    }
    MultiFormatRegistry::Publish(dev, m_frameStoreZeroBased, requestedInput,
                                 static_cast<int>(fmt));

    // Decode the format and verify it matches the raster we were sized
    // for at construction time. The GL pipeline allocates textures /
    // PBOs / host buffers at fixed dimensions and cannot adapt on the
    // fly; on a mismatch we publish a pending raster-change so the
    // render thread (which owns the GL context) can tear this capture
    // down, re-allocate everything at the new size and spin up a
    // fresh capture instance.
    //
    // The mismatch test compares BOTH the active raster (width/height)
    // AND the underlying NTV2VideoFormat code: identical dimensions
    // with different rates (the classic 1080p30 vs 1080p60 case, both
    // 1920x1080) MUST also dispatch a raster-change because the
    // FrameStore needs SetVideoFormat re-applied at the new rate to
    // capture correctly. Without this, the FrameStore stays configured
    // for the construction-time rate and silently captures torn or
    // zero-filled frames at the wrong rate.
    CapturedFormat decoded;
    if (!TryDecodeNTV2Format(static_cast<int>(fmt), decoded))
        return ConfigFail(requestedInput, "input format not supported (NTV2VideoFormat)", static_cast<int>(fmt), "unsupported format");
    const bool rasterDiffers =
        decoded.width != m_width || decoded.height != m_height;
    const bool formatDiffers =
        m_initialNtv2FormatRaw != 0 &&
        decoded.ntv2FormatRaw != m_initialNtv2FormatRaw;
    if (rasterDiffers || formatDiffers)
    {
        m_pendingPacked.store(
            (uint32_t(decoded.width)  << 16) |
             uint32_t(decoded.height & 0xFFFFu),
            std::memory_order_relaxed);
        m_pendingRawFormat.store(decoded.ntv2FormatRaw,
                                 std::memory_order_relaxed);
        // Release-store on the gate: the render thread's matching
        // acquire-load on ConsumePendingFormatChange will see both
        // packed dimensions and raw format value as published.
        m_pendingFormatChange.store(true, std::memory_order_release);
        return ConfigFail(requestedInput,
                          "input format differs from the pipeline's; re-allocation requested "
                          "(input NTV2VideoFormat)", decoded.ntv2FormatRaw, "new format, rebuilding");
    }

    // v0.42: which FrameStores this input and signal need on this card
    // (CaptureTopology.h). Checked before the format capability below so
    // that a 4K signal on one cable of a card without 12G routing says
    // why, rather than "card cannot run this format". The owner claimed
    // them from the same plan for the format it sized the pipeline for;
    // a different plan can only come with a different format, which
    // re-allocates above. Anything else (a capture created before the
    // format was known) is sent back the same way so the owner claims
    // again.
    const FrameStorePlan plan =
        PlanFrameStores(requestedInput, Is4KRaster(decoded), m_device.Caps());
    if (!plan.ok)
        return ConfigFail(requestedInput, plan.reason, static_cast<int>(fmt), plan.reason);
    {
        const uint32_t planMask = plan.fixedMask != 0u
            ? plan.fixedMask : (1u << m_frameStoreZeroBased);
        if (planMask != m_frameStoreMask)
        {
            m_pendingPacked.store(
                (uint32_t(decoded.width)  << 16) |
                 uint32_t(decoded.height & 0xFFFFu),
                std::memory_order_relaxed);
            m_pendingRawFormat.store(decoded.ntv2FormatRaw,
                                     std::memory_order_relaxed);
            m_pendingFormatChange.store(true, std::memory_order_release);
            return ConfigFail(requestedInput,
                              "the signal needs other FrameStores; re-allocation requested",
                              static_cast<int>(planMask), "new format, rebuilding");
        }
    }

    // Verify the card itself can actually run this format. Most modern
    // AJA hardware accepts everything its driver can detect, but the
    // capabilities table can return false for combinations like an old
    // card that sees a new format on its input or a card whose internal
    // bandwidth budget cannot sustain the requested rate. Bailing here
    // is cheaper than letting AutoCirculate fail in an unhelpful way
    // half a second later. We call the standalone NTV2DeviceCanDo...
    // function rather than the DeviceCapabilities class because the
    // latter transitively pulls in Windows UI headers (commctrl.h /
    // prsht.h) that conflict with our WIN32_LEAN_AND_MEAN build mode.
    if (!::NTV2DeviceCanDoVideoFormat(card->GetDeviceID(), fmt))
        return ConfigFail(requestedInput, "card cannot run this format (NTV2VideoFormat)", static_cast<int>(fmt), "card cannot run this format");

    // Belt-and-braces: confirm the card can put the FrameStore into the
    // pixel format the rest of our pipeline expects (8-bit UYVY 4:2:2).
    // AJADevice::Open() already checked this at open time, but the
    // capability table is keyed by device, not by the specific channel,
    // so we re-check here in case of a firmware path where a particular
    // FrameStore variant disagrees with the global flag. Cheap insurance
    // and matches the AJA NTV2Capture demo's defensive style.
    if (!::NTV2DeviceCanDoFrameBufferFormat(card->GetDeviceID(), kCaptureFBF))
        return ConfigFail(requestedInput, "card cannot do the 8-bit YCbCr frame buffer", 0, "card cannot do 8-bit YCbCr");

    // v0.42 quad-link checks (experimental, untested on hardware): the
    // four links carry the same signal, and the source uses the
    // interleave this card takes (VPID: 2160-line quad-link standards
    // are two-sample interleave; square division sends four plain 1080p
    // links). A source without VPID is tried as is.
    if (isQuad)
    {
        const int link1 = MapLevelBFormatToLevelA(
            static_cast<int>(card->GetInputVideoFormat(inputSource, false)));
        for (int k = 1; k < links; ++k)
        {
            const int lk = MapLevelBFormatToLevelA(static_cast<int>(
                card->GetInputVideoFormat(input::LinkSource(requestedInput, k), false)));
            if (lk != link1)
                return ConfigFail(requestedInput,
                    "quad-link input: the four links do not carry the same signal",
                    k + 1, "quad-link: a link is missing or differs");
        }
        ULWord vpidA = 0, vpidB = 0;
        if (card->ReadSDIInVPID(physBNCChannel, vpidA, vpidB))
        {
            const CNTV2VPID vpid(vpidA);
            if (vpid.IsValid())
            {
                const bool twoSI = vpid.IsStandardTwoSampleInterleave();
                if (plan.topology == Topology::Tsi2 && !twoSI)
                    return ConfigFail(requestedInput,
                        "quad-link source is not 2SI (square division is not supported on this card)",
                        static_cast<int>(vpid.GetStandard()), "quad-link source must be 2SI");
                if (plan.topology == Topology::Squares4 && twoSI)
                    return ConfigFail(requestedInput,
                        "quad-link source is 2SI; this card takes square division only",
                        static_cast<int>(vpid.GetStandard()), "quad-link source must be square division");
            }
        }
    }

    // For HDMI we need to know the input colour space because RGB HDMI
    // requires a CSC (colour space converter) widget inserted between
    // the HDMI input and the FrameStore — the FrameStore itself runs in
    // YCbCr 4:2:2 (UYVY) at all times, so an RGB input has to be
    // converted upstream of it. YCbCr HDMI and SDI go straight to the
    // FrameStore. The detection is a single register read on the HDMI
    // input widget, returning RGB or YCbCr based on the AVI InfoFrame
    // the source signalled or, failing that, the receiver's best
    // guess from the colorimetry of the picture. We default to YCbCr
    // on probe failure because that is what professional/broadcast
    // gear overwhelmingly sends; the only meaningful failure mode is
    // a wrong-coloured picture which is much better than no picture.
    // v0.42: the HDMI input's own channel (HDMI 2..4 on a Kona HDMI).
    bool isInputRGB = false;
    if (!isSDI)
    {
        NTV2LHIHDMIColorSpace hdmiCS = NTV2_LHIHDMIColorSpaceYCbCr;
        if (card->GetHDMIInputColor(hdmiCS, physBNCChannel))
            isInputRGB = (hdmiCS == NTV2_LHIHDMIColorSpaceRGB);
        // v0.42: the older HDMI input widgets (KONA LHi, Io XT, Io X3,
        // Io 4K, Corvid HB-R, Io Express) have only a YCbCr output in the
        // SDK's routing tables: there is no RGB output to route through a
        // CSC, the receiver hands the picture over as YCbCr. Take that
        // output (untested on such cards; up to v0.41 the route failed).
        const uint32_t bit = 1u << static_cast<int>(physBNCChannel);
        if (isInputRGB && (m_device.GetStatus().hdmiRgbMask & bit) == 0u)
        {
            isInputRGB = false;
            if (!m_loggedHdmiYuvOnly)
            {
                m_loggedHdmiYuvOnly = true;
                FFGLLog::LogToHost("AJAInput: RGB HDMI source on an HDMI input "
                                   "with only a YCbCr output; using that output");
            }
        }
    }

    // v0.42: the CSCs an RGB picture needs: the FrameStore's own one, or
    // four for a 4K HDMI picture through 425 muxes (TsiCscIndex). A few
    // cards have fewer CSCs than FrameStores (Io Express has none, the
    // KONA XM 3 for 4).
    if (isInputRGB)
    {
        const int numCSCs = static_cast<int>(m_device.GetStatus().numCSCs);
        int needed = m_frameStoreZeroBased + 1;
        if (plan.topology == Topology::Tsi2)
        {
            needed = 0;
            for (int path = 0; path < 4; ++path)
            {
                const int csc = TsiCscIndex(plan, path, m_device.Caps());
                if (csc + 1 > needed)
                    needed = csc + 1;
            }
        }
        if (numCSCs < needed)
            return ConfigFail(requestedInput,
                "RGB HDMI needs a colour space converter this card or FrameStore lacks",
                numCSCs, "RGB HDMI: no colour converter (send YCbCr)");
    }

    // Step 1 (switching a bidirectional SDI connector to receive) runs
    // before the format probe since v0.40; see the ownership checks
    // above.

    // Step 2: route the input widget's output into our FrameStore's
    // input. Without this, AutoCirculate on FrameStore K has no idea
    // which physical input to feed from; AJA cards default to a
    // "channel N feeds FrameStore N" wiring that breaks the moment we
    // try anything else, including capturing SDI3 on a 2-FrameStore
    // card or capturing HDMI through any FrameStore at all. The signal
    // routing matrix is what lets any physical input reach any
    // FrameStore.
    //
    // Two topologies depending on the input colour space:
    //
    //   YCbCr input (SDI always, HDMI when negotiated as YCbCr):
    //     a single direct connection FrameStore <== Input. The
    //     FrameStore consumes YCbCr 4:2:2 as kCaptureFBF expects.
    //
    //   RGB input (HDMI when negotiated as RGB):
    //     two connections with a CSC (colour space converter) widget
    //     between the HDMI input and the FrameStore:
    //         CSC      <== HDMIin (RGB output crosspoint)
    //         FrameStore <== CSC   (YCbCr output crosspoint)
    //     The CSC widget is the per-channel one associated with our
    //     FrameStore (CSC1 ↔ FrameStore1, etc.). It needs no explicit
    //     enable; connecting both its input and a consumer of its
    //     output is what activates it in the AJA routing fabric.
    //
    // The /*RGB*/ boolean passed to GetInputSourceOutputXpt selects
    // between the input widget's RGB and YCbCr output taps — getting
    // this wrong on an RGB HDMI source feeds raw 3x8-bit RGB pixels
    // into a circuit expecting 4:2:2 YCbCr and produces a heavily
    // colour-shifted picture. The reference for this topology is
    // CNTV2DemoCommon::GetInputRouting in the AJA SDK demos
    // (ntv2democommon.cpp, the !isFrameRGB && isInputRGB branch).
    //
    // v0.42, 4K on cards without 12G routing (experimental; the
    // reference is CNTV2DemoCommon::GetInputRouting4K, YUV frame buffer):
    //   Tsi2      four streams (SDI links 1..4, or the HDMI input's four
    //             quadrant outputs, through a CSC each when RGB) into the
    //             A / B inputs of two 425 muxes, whose A / B outputs feed
    //             the A / B inputs of the FrameStore pair.
    //   Squares4  SDI link k straight into FrameStore k of the four.
    //
    // Build the set of crosspoint connections that has to be applied
    // to route the input through to the FrameStore, then hand the
    // whole set to the driver in one call via ApplySignalRoute. This
    // is the canonical SDK pattern (mirrors what ntv2capture.cpp does
    // around `mDevice.ApplySignalRoute(connections, ...)`) and has
    // two concrete advantages over the per-connection Connect()
    // calls we had before:
    //
    //   1) Atomicity. ApplySignalRoute applies the whole set in one
    //      driver transaction. Per-connection Connect() leaves the
    //      routing in a half-applied state if the second call fails,
    //      with the FrameStore left pointing at the CSC output but
    //      the CSC input not yet wired - the first frame after that
    //      partial config arrives as garbage.
    //
    //   2) Conflict handling. The second argument is `inReplace`:
    //      `true` instructs the driver to replace the device's
    //      existing widget routing entirely with the new connection
    //      set (any pre-existing connection that is not in the new
    //      set gets torn down). `false` augments the existing
    //      routing: the new connections get added, everything else
    //      stays in place. We pass `false` because we run in
    //      MultiFormat mode: another plugin instance may have wired
    //      a different FrameStore against a different input, and we
    //      must not yank that out from under them. The SDK's demos
    //      pass `!fDoMultiFormat`, which evaluates to `false` for
    //      multi-format builds and matches our argument exactly.
    const NTV2InputXptID fbInputXpt =
        ::GetFrameStoreInputXptFromChannel(fsChannel);
    NTV2XptConnections connections;
    if (plan.topology == Topology::Single)
    {
        if (isInputRGB)
        {
            const NTV2OutputXptID inputRgbOXpt =
                ::GetInputSourceOutputXpt(inputSource,
                                          /*inIsSDI_DS2*/false,
                                          /*inIsRGB*/true);
            const NTV2InputXptID  cscInputXpt =
                ::GetCSCInputXptFromChannel(fsChannel);
            const NTV2OutputXptID cscYuvOXpt =
                ::GetCSCOutputXptFromChannel(fsChannel,
                                             /*inIsKey*/false,
                                             /*inIsRGB*/false);
            connections.insert(NTV2Connection(fbInputXpt,   cscYuvOXpt));
            connections.insert(NTV2Connection(cscInputXpt,  inputRgbOXpt));
        }
        else
        {
            const NTV2OutputXptID inputOutputXpt =
                ::GetInputSourceOutputXpt(inputSource,
                                          /*inIsSDI_DS2*/false,
                                          /*inIsRGB*/false);
            connections.insert(NTV2Connection(fbInputXpt,   inputOutputXpt));
        }
    }
    else if (plan.topology == Topology::Tsi2)
    {
        for (int path = 0; path < 4; ++path)
        {
            const NTV2Channel mux   = static_cast<NTV2Channel>(plan.muxBase + path / 2);
            const NTV2Channel fb    = static_cast<NTV2Channel>(plan.firstFrameStore + path / 2);
            const bool        linkB = (path & 1) != 0;
            NTV2OutputXptID   stream = NTV2_OUTPUT_CROSSPOINT_INVALID;
            if (isSDI)
            {
                stream = ::GetInputSourceOutputXpt(
                    input::LinkSource(requestedInput, path));
            }
            else if (isInputRGB)
            {
                const NTV2Channel csc = static_cast<NTV2Channel>(
                    TsiCscIndex(plan, path, m_device.Caps()));
                connections.insert(NTV2Connection(
                    ::GetCSCInputXptFromChannel(csc),
                    ::GetInputSourceOutputXpt(inputSource, false, /*RGB*/true,
                                              static_cast<UWord>(path))));
                stream = ::GetCSCOutputXptFromChannel(csc, /*key*/false, /*RGB*/false);
            }
            else
            {
                stream = ::GetInputSourceOutputXpt(inputSource, false, /*RGB*/false,
                                                   static_cast<UWord>(path));
            }
            connections.insert(NTV2Connection(
                ::GetTSIMuxInputXptFromChannel(mux, linkB), stream));
            connections.insert(NTV2Connection(
                ::GetFrameStoreInputXptFromChannel(fb, linkB),
                ::GetTSIMuxOutputXptFromChannel(mux, linkB, /*RGB*/false)));
        }
    }
    else  // Topology::Squares4
    {
        for (int path = 0; path < 4; ++path)
            connections.insert(NTV2Connection(
                ::GetFrameStoreInputXptFromChannel(
                    static_cast<NTV2Channel>(plan.firstFrameStore + path)),
                ::GetInputSourceOutputXpt(input::LinkSource(requestedInput, path))));
    }
    // v0.42: for the 4K routes through muxes or into four FrameStores
    // (untested on hardware), a card with a crosspoint ROM says up front
    // whether it allows every connection. Single routes keep the v0.41
    // check (ClaimFrameStore skips FrameStores the ROM rules out).
    if (plan.topology != Topology::Single)
    {
        for (const auto& c : connections)
        {
            bool allowed = true;
            if (card->CanConnect(c.first, c.second, allowed) && !allowed)
                return ConfigFail(requestedInput,
                    "the card's routing ROM refuses a connection this input needs",
                    static_cast<int>(c.first), "routing not allowed on this card");
        }
    }
    if (!card->ApplySignalRoute(connections, /*inReplace*/false))
        return ConfigFail(requestedInput, "signal routing (ApplySignalRoute) failed", 0, "routing failed");

    // Step 3: configure the FrameStore. Order matters: enable the
    // widget BEFORE setting its video format and pixel format, so the
    // driver's internal state machine sees a consistent sequence. We
    // additionally disable VANC line capture (the default on some
    // firmware is to embed VANC ancillary data in the top lines of the
    // raster, which would corrupt the picture). v0.42: every FrameStore
    // of a 4K set.
    for (int fs = 0; fs < 8; ++fs)
        if (m_frameStoreMask & (1u << fs))
            card->EnableChannel(static_cast<NTV2Channel>(fs));

    // 3G-SDI Level B → Level A input conversion. Set per-channel,
    // conditional on whether the SDI receiver currently reports a 3Gb
    // signal physically present on the cable. This matches the SDK
    // demo pattern in ntv2framegrabber.cpp:
    //     mNTV2Card.SetSDIInLevelBtoLevelAConversion(mChannel,
    //         IsInput3Gb(mInputSource) ? true : false);
    // and cudaapp.cpp (which uses the detected video format as an
    // indirect proxy for is3Gb but ends up at the same logic).
    //
    // Why conditional and not unconditional `true`: the AJA SDK
    // documentation states "Specify false for normal operation"; there
    // is no guarantee that activating the conversion on a cable that
    // is already Level A (or carrying no signal at all) is a safe no-
    // op. Empirically, an earlier iteration of this plugin enabled it
    // unconditionally for all four SDIs at device open time and saw
    // signals reported at the wrong rate. Both demos that touch this
    // function gate it on the live cable state.
    //
    // Why here and not in AJADevice::Open: GetSDIInput3GbPresent reads
    // the receiver block; the receiver only locks once the BNC is in
    // receive mode (post-bidir-flip) and has had a few VBIs to settle,
    // which is exactly the state we reach by Step 1. Running this in
    // Open() before any bidir flip would read stale or unlocked
    // receiver state. Running it per-channel here also means a
    // cable-side transition between Level A and Level B is handled
    // naturally: the running-state probe catches the resulting format
    // change and re-enters this function with the new is3Gb value.
    // v0.42: every link of a quad-link input.
    if (isSDI)
    {
        for (int k = 0; k < links; ++k)
        {
            const NTV2Channel bnc = static_cast<NTV2Channel>(physBNCChannel + k);
            bool is3Gb = false;
            card->GetSDIInput3GbPresent(is3Gb, bnc);
            card->SetSDIInLevelBtoLevelAConversion(static_cast<UWord>(bnc), is3Gb);
        }
    }

    // v0.42: a card without multi-format runs one video format on all
    // its FrameStores (the SDK programs channel 1 whatever channel is
    // passed). Another input running a different format keeps the card;
    // this one waits, showing why, instead of switching the format under
    // it. The claim is released on teardown and when configuring fails.
    const bool singleFormatCard = !m_device.GetStatus().canDoMultiFormat;
    if (singleFormatCard)
    {
        MultiFormatRegistry::Conflict other;
        if (!MultiFormatRegistry::ClaimCardFormat(dev, m_frameStoreZeroBased,
                                                  requestedInput,
                                                  static_cast<int>(fmt), &other))
            return ConfigFail(requestedInput,
                "the card runs one video format at a time and another input uses a different one",
                other.otherFormatRaw, "card runs one format: another input differs");
    }

    {
        // On a one-format card already running this format (another
        // input of the same format), leave it alone.
        bool program = true;
        if (singleFormatCard)
        {
            NTV2VideoFormat current = NTV2_FORMAT_UNKNOWN;
            if (card->GetVideoFormat(current, NTV2_CHANNEL1) && current == fmt)
                program = false;
        }
        if (program)
        {
            // v0.42: on a one-format card a new format switches every
            // FrameStore, another application's playout included
            // (Resolume's own AJA output, say): leave such a card alone.
            if (singleFormatCard && m_device.BusyFrameStores(/*playoutOnly*/true) != 0u)
                return ConfigFail(requestedInput,
                    "the card runs one video format at a time and another "
                    "application plays out on it in another format",
                    static_cast<int>(fmt), "card's format is another app's output");
            card->SetVideoFormat(fmt, false, false, fsChannel);
        }
    }
    for (int fs = 0; fs < 8; ++fs)
        if (m_frameStoreMask & (1u << fs))
            card->SetVANCMode(NTV2_VANCMODE_OFF, static_cast<NTV2Channel>(fs));

    // For single-link 4K UHD or DCI on a card with 12G-SDI routing
    // (Kona X family) the FrameStore needs to be told to treat its
    // raster as a TSI-packed 4K image. Without this the FrameStore
    // stays in HD mode and either AutoCirculate refuses the format or
    // the captured buffer ends up with one quadrant of the picture in
    // the wrong place. SetTsiFrameEnable is also the right call for
    // single-link 12G even though the public API name suggests it is
    // 2SI-related; AJA's own NTV2Capture4K demo uses it
    // unconditionally when CanDo12gRouting is true. For non-4K formats
    // we leave the call out so HD captures continue working on cards
    // that have CanDo12gRouting but are currently fed an HD signal.
    //
    // v0.42: on cards without 12G routing a 4K picture comes in over a
    // FrameStore pair in TSI mode or four in quad (squares) mode, set on
    // the first FrameStore as ntv2capture4k does; the SDK spreads it to
    // the pair / group (ntv2register.cpp). Up to v0.41 a 4K signal on
    // such a card set squares mode on its single FrameStore, which also
    // copied that FrameStore's format to the three others of its group.
    const bool can12g = ::NTV2DeviceCanDo12gRouting(card->GetDeviceID());
    switch (plan.topology)
    {
        case Topology::Tsi2:
            card->SetTsiFrameEnable(true, fsChannel);
            break;
        case Topology::Squares4:
            card->Set4kSquaresEnable(true, fsChannel);
            break;
        case Topology::Single:
            if (can12g)
            {
                // v0.41: a FrameStore that captured 4K before (FrameStores
                // move between inputs and formats: lending, pairing) must
                // not stay in TSI mode for an HD raster. On 12G-routing
                // cards in multi-format mode this clears only this
                // FrameStore's bit (SDK ntv2register.cpp), as AJA's
                // ntv2dolbycapture demo does for non-4K formats.
                card->SetTsiFrameEnable(NTV2_IS_4K_VIDEO_FORMAT(fmt), fsChannel);
            }
            else
            {
                // v0.42: an HD capture on a FrameStore a 4K capture left
                // in TSI / quad mode. SetVideoFormat for an HD format
                // already switches both off for the pair / group
                // (SetQuadFrameEnable(false), ntv2register.cpp); this
                // covers a one-format card whose format was not
                // reprogrammed above.
                ClearStaleMultiFrameStoreModes(card, fsChannel);
            }
            break;
    }
    m_activeTopology = plan.topology;

    // Snapshot the requested pixel kind for this Initializing pass.
    // If the user picked V210 (10-bit) but the card does not support
    // NTV2_FBF_10BIT_YCBCR, silently fall back to UYVY. This mirrors
    // the same defensive pattern other settings use: never refuse to
    // capture; degrade gracefully. The fallback is logged once per
    // configuration pass so the operator can see it in the host log.
    const auto requestedKind = static_cast<PixelKind>(
        m_pixelKind.load(std::memory_order_acquire));
    const bool cardSupports10Bit =
        m_device.GetStatus().canDoFBF10Bit;
    if (requestedKind == PixelKind::V210 && !cardSupports10Bit)
    {
        char line[160];
        std::snprintf(line, sizeof(line),
            "[AJAInput] V210 requested but card does not support "
            "NTV2_FBF_10BIT_YCBCR; falling back to 8-bit UYVY.");
        FFGLLog::LogToHost(line);
        m_activePixelKind = PixelKind::UYVY;
    }
    else
    {
        m_activePixelKind = requestedKind;
    }

    // Resize the host DMA buffer to match the active pixel kind. v210
    // is ~33% larger than UYVY for the same raster. The capture
    // thread owns m_transferBuffer for the lifetime of this Running
    // session, so resizing here is safe; the next DMA read will land
    // in the freshly-sized buffer.
    const std::size_t newBytes =
        FrameSize(m_activePixelKind, m_width, m_height);
    if (newBytes != m_frameBytes)
    {
        m_frameBytes = newBytes;
        m_transferBuffer.assign(m_frameBytes, 0);
    }

    // Select the device FrameStore format. The shader's V210 unpack
    // expects standard SMPTE layout (3 × 10-bit samples per 32-bit
    // word, LSB-first); AJA's NTV2_FBF_10BIT_YCBCR is documented to
    // use that layout. If the actual byte order turns out to differ
    // on the operator's card (which we cannot verify without
    // hardware), the symptom will be a colour-tinted or horizontally
    // striped image with PQ/HLG selected; the workaround is to
    // toggle 10-bit off and stay on 8-bit until the shader is fixed.
    const NTV2FrameBufferFormat fbf =
        (m_activePixelKind == PixelKind::V210)
            ? NTV2_FBF_10BIT_YCBCR
            : NTV2_FBF_8BIT_YCBCR;
    for (int fs = 0; fs < 8; ++fs)
        if (m_frameStoreMask & (1u << fs))
            card->SetFrameBufferFormat(static_cast<NTV2Channel>(fs), fbf);
    card->EnableInputInterrupt(fsChannel);

    // Refresh capture-thread-local mirrors of the user-facing atomic
    // settings BEFORE deciding on field-mode and the AC option flags.
    // SetRingFrameCount / SetLowLatencyMode setters force a transition
    // back to Initializing whenever the target changes, which lands the
    // capture loop right here on its next iteration with the new value
    // visible. Reading once per Initializing pass means the rest of the
    // capture session sees a stable snapshot.
    m_activeRingFrameCount =
        m_targetRingFrameCount.load(std::memory_order_acquire);
    m_activeLowLatencyMode =
        m_lowLatencyMode.load(std::memory_order_acquire);

    // v0.32 + v0.37: decide whether this capture session uses field
    // mode.
    //
    // Conditions:
    //   (a) format is interlaced (true 1080i / 480i / 576i — NOT PSF,
    //       whose picture content is progressive even though it's
    //       transported as fields), AND
    //   (b) pixel kind is UYVY (V210 stays in frame mode for v0.32+;
    //       the per-field scatter targets the 8-bit macropixel layout,
    //       and the V210 path uses simple bob anyway, where the
    //       latency win is smaller).
    //
    // v0.37: dropped the previous "LL OFF" restriction. For interlaced
    // UYVY sources, AC field mode is structurally better than the
    // manual LL frame-mode path (LL publishes at source-frame rate =
    // 30 Hz for 1080i59.94; AC field mode publishes at field rate =
    // 60 Hz, which both feeds YADIF its per-field samples earlier
    // AND eliminates the publish/compositor cadence mismatch that
    // costs ~2-3 display frames in LL+interlaced). So we override
    // the user's LL toggle for this specific case: when interlaced
    // + UYVY, m_useACPath is forced true regardless of LL toggle.
    //
    // BFF detection mirrors AJAInputPlugin's heuristic: NTSC-class 480i
    // is BFF, everything else (576i PAL, 1080i HD) is TFF. The
    // m_tffSource snapshot is used by the field-mode capture loop
    // to map AC's F0/F1 (first-arrived / second-arrived) onto the
    // top/bottom field row offsets when scattering into the
    // accumulator buffer.
    const bool fmtIsInterlaced = !::IsProgressivePicture(fmt);
    // v0.39: field mode also requires the texture ring. The field
    // scatter and the per-field metadata only exist on the ring path;
    // without it the byte queue would receive one contiguous field
    // followed by stale bytes.
    m_fieldModeActive = fmtIsInterlaced &&
                        (m_activePixelKind == PixelKind::UYVY) &&
                        (m_textureRing != nullptr);
    m_useACPath       = m_fieldModeActive
                        ? true
                        : !m_activeLowLatencyMode;
    m_tffSource       = (m_height > 486);  // BFF only for NTSC-class

    // v0.37: log the override so it doesn't surprise anyone reading
    // the log. Fires only on the specific case where the user asked
    // for LL and we routed them through AC field mode anyway. Logged
    // once per Initializing pass; subsequent re-inits at the same
    // raster will re-log only if the override would change.
    if (m_activeLowLatencyMode && m_useACPath)
    {
        char line[200];
        std::snprintf(line, sizeof(line),
            "AJAInput: LL toggle ON + interlaced UYVY: capture "
            "through AC field mode (field-rate publishes) and render "
            "with the low-latency (no lookahead) YADIF");
        FFGLLog::LogToHost(line);
    }
    // Reset the per-source-frame sequence on every session re-init
    // so the renderer's last-rotated tracker (which also starts at 0
    // via EnsureYadifHistory) sees the first source frame as a
    // proper advance (0 → 1) and rotates correctly.
    m_captureSourceFrameSeq = 0;

    // Allocate the per-instance scatter accumulator when field mode is
    // active. Size = full-frame bytes; the layout is "F0 rows scattered
    // at every-other-line offset 0, F1 rows scattered at offset 1".
    // Zero-initialise so the first publish's stale half reads as black
    // rather than uninitialised memory (the YADIF render path won't
    // sample stale rows anyway, but a clean buffer is friendlier to
    // any host-side observation tools we might bolt on later).
    if (m_fieldModeActive)
    {
        if (m_fieldAccumBuffer.size() != m_frameBytes)
            m_fieldAccumBuffer.assign(m_frameBytes, 0);
    }
    else
    {
        // Free the buffer when not in field mode so a long-running
        // session that toggles between progressive and interlaced
        // sources doesn't hold the memory permanently. ~4MB at 1080.
        std::vector<uint8_t>().swap(m_fieldAccumBuffer);
    }

    // Step 4: AutoCirculate is bound to the FrameStore. We initialise
    // first, and only subscribe to the input vertical interrupt AFTER
    // a successful start; this keeps the subscribe/unsubscribe pair
    // perfectly balanced even if init or start fails. The capture
    // loop's WaitForInputVerticalInterrupt requires the subscription
    // to be active when it is called, so subscribing right after
    // AutoCirculateStart is in time.
    //
    // AUTOCIRCULATE_WITH_RP188 enables driver-side capture of the
    // embedded SMPTE RP188 timecode that every professional SDI
    // signal carries (HD VITC for HD, ATC LTC for SD). The transfer
    // status's FRAME_STAMP exposes it through GetInputTimeCode. HDMI
    // does not carry RP188 so we skip the flag there; the option is
    // harmless on cards that ignore it, but skipping where it makes
    // no sense keeps the option bitmask self-explanatory in logs.
    //
    // AUTOCIRCULATE_WITH_FIELDS (v0.32) puts AC in field-mode: each
    // AutoCirculateTransfer delivers one FIELD (contiguous half-frame)
    // instead of a woven frame. Combined with the host-side scatter
    // step in the capture loop, this lets the render thread start
    // showing the YADIF reconstruction of the freshly-arrived field
    // before the complementary field has been captured. Only set
    // when m_fieldModeActive (which already gates on UYVY + AC path).
    const ULWord acOptionFlags =
        (isSDI ? static_cast<ULWord>(AUTOCIRCULATE_WITH_RP188) : 0u) |
        (m_fieldModeActive ? static_cast<ULWord>(AUTOCIRCULATE_WITH_FIELDS) : 0u);
    card->AutoCirculateStop(fsChannel, /*inAbort*/true);

    if (!m_useACPath)
    {
        // Low-latency path. Based on the SDK's ntv2llburn demo: we use
        // AutoCirculateInitForInput purely as a slot allocator (it
        // reserves N contiguous device frame buffers atomically and
        // tells us which ones) and drive the slots by hand with
        // SetInputFrame.
        //
        // Ring depth in this mode is clamped to 2..3. The user-facing
        // Ring Depth parameter is honoured but values above 3 have no
        // effect: the manual SetInputFrame rotation reads exactly one
        // slot back per vblank (the one the hardware just finished
        // writing), so a longer rotation buys nothing for latency. We
        // still accept 3 as a hedge for a stuttery consumer: with
        // three slots in flight, a single late ProcessOpenGL has an
        // extra slot's worth of buffering before the hardware
        // overwrites a frame mid-DMA.
        const UWord llFrameCount = static_cast<UWord>(
            std::min(m_activeRingFrameCount, 3));
        if (!card->AutoCirculateInitForInput(
                fsChannel,
                llFrameCount,
                NTV2_AUDIOSYSTEM_INVALID,
                /*inOptionFlags*/acOptionFlags,
                /*inNumChannels*/1))
            return ConfigFail(requestedInput, "AutoCirculateInitForInput failed (LL slots)", llFrameCount, "AutoCirculate init failed");

        AUTOCIRCULATE_STATUS acStatus;
        if (!card->AutoCirculateGetStatus(fsChannel, acStatus))
        {
            card->AutoCirculateStop(fsChannel, /*inAbort*/true);
            return ConfigFail(requestedInput, "AutoCirculateGetStatus failed (LL)", 0, "AutoCirculate status failed");
        }
        m_inputStartFrame = static_cast<uint32_t>(acStatus.acStartFrame);

        // v0.40: leave AutoCirculate in the INIT state instead of
        // stopping it (ntv2llburn stops it). The SDK only knows which
        // device frames are in use through its memory auditor
        // (SDRAMAuditor::TagVideoFrames): for a stopped channel it
        // marks just the current input frame, so a second instance's
        // AutoCirculateInitForInput could be handed our idle ping-pong
        // slot and both FrameStores would write the same memory. A
        // channel in INIT keeps its whole range tagged. INIT is inert
        // for the hardware: the driver's init writes no frame register
        // and its interrupt handler only timestamps an INIT channel
        // (driver ntv2autocirc.c), so our manual SetInputFrame rotation
        // is unaffected. TeardownChannel's AutoCirculateStop releases
        // the range as before.

        // We need vertical-event delivery for the WaitForInputFieldID
        // calls in the capture loop.
        card->SubscribeInputVerticalEvent(fsChannel);

        // Prime the hardware: aim it at slot 0. At the next vblank
        // the device will latch this and start writing slot 0. The
        // first iteration of the capture loop will wake from that
        // vblank, see m_llCycleSlot == 0, compute the "just completed"
        // slot (which will be garbage on this first turn — nothing
        // was being written before the device armed), DMA it (the
        // garbage), then advance m_llCycleSlot to 1 and tell the
        // hardware to switch to slot 1 next vblank. From iteration 2
        // onward steady-state begins and every DMA reads a freshly
        // completed slot at one frame interval of latency.
        m_llCycleSlot   = 0;
        m_llWarmupSkip  = true;   // see the publish in the LL loop
        m_currentInFrame = m_inputStartFrame + m_llCycleSlot;
        card->SetInputFrame(fsChannel, m_currentInFrame);

        m_activeInputNumber = requestedInput;
        SampleSignalledTransfer(card, requestedInput);
        ConfigSucceeded(requestedInput, static_cast<int>(fmt));
        return true;
    }

    // Standard AutoCirculate path (default).
    //
    // v0.32 fix: in FIELD mode, each AC slot holds one FIELD (not a
    // frame). With the user's Ring Depth at N, frame-mode gives N
    // source frames of buffering; field-mode would only give N/2
    // source frames of buffering — and our first hardware measurement
    // showed an extra ~33 ms (1 source frame) of latency added by AC
    // itself when buffering was too tight. Doubling the slot count
    // for field-mode restores parity with frame-mode in terms of
    // "source frames of slack between hardware and consumer", which
    // matches the SDK's ntv2fieldburn demo (default of 7 fields =
    // 3.5 source frames, well above any reasonable consumer cadence).
    // The user-facing Ring Depth knob keeps its frame-rate semantics;
    // the doubling happens internally inside the AC init only.
    //
    // v0.40: frame mode uses at least 3 slots. With 2, the driver's
    // record interrupt finds the next slot still full on every other
    // interrupt (the frame it just completed has not been transferred
    // yet), drops, and keeps recording into the active slot while
    // marking it valid (ntv2autocirc.c, "Application not reading frames
    // fast enough"). Before v0.40 that halved the delivered frame rate
    // at Ring Depth 2; with the level >= 1 transfer below it would hand
    // us the slot under the write head. Ring depth is margin only, so
    // the extra slot costs no latency. Field mode already has >= 4.
    const UWord acFrameCount = m_fieldModeActive
        ? static_cast<UWord>(m_activeRingFrameCount * 2)
        : static_cast<UWord>(std::max(m_activeRingFrameCount, 3));
    if (!card->AutoCirculateInitForInput(
            fsChannel,
            acFrameCount,
            NTV2_AUDIOSYSTEM_INVALID,
            /*inOptionFlags*/acOptionFlags,
            /*inNumChannels*/1))
        return ConfigFail(requestedInput, "AutoCirculateInitForInput failed (frames)", acFrameCount, "AutoCirculate init failed");

    if (!card->AutoCirculateStart(fsChannel))
    {
        card->AutoCirculateStop(fsChannel, true);
        return ConfigFail(requestedInput, "AutoCirculateStart failed", 0, "AutoCirculate start failed");
    }

    card->SubscribeInputVerticalEvent(fsChannel);

    // Compute and cache the expected output crosspoint feeding this
    // FrameStore's input: the connection the routing block above made
    // into it. Cached so EmitChannelReadback can verify the wiring is
    // still in place on every periodic check, not just the initial one.
    // The cache is invalidated by TeardownChannel; a subsequent
    // TryConfigureChannel recomputes it.
    {
        NTV2OutputXptID expectedOXpt = NTV2_XptBlack;
        for (const auto& c : connections)
            if (c.first == fbInputXpt)
                expectedOXpt = c.second;
        m_expectedFbInXpt  = static_cast<int>(fbInputXpt);
        m_expectedFbOutXpt = static_cast<int>(expectedOXpt);
        m_activeFsChannel  = static_cast<int>(fsChannel);
    }

    EmitChannelReadback(card, "cfg-readback");

    // Prime the per-session first-frame byte dump. The capture loop
    // will log the first 16 bytes of the first successful DMA transfer
    // once and then disarm the flag; this lets us see whether the
    // FrameStore is actually delivering bytes or returning zeros.
    m_armFirstFrameDump.store(true, std::memory_order_release);

    m_activeInputNumber = requestedInput;
    SampleSignalledTransfer(card, requestedInput);
    ConfigSucceeded(requestedInput, static_cast<int>(fmt));
    return true;
}

void AJACaptureSource::EmitChannelReadback(CNTV2Card*  card,
                                            const char* tag)
{
    if (m_activeFsChannel < 0)
        return;
    const NTV2Channel fsChannel =
        static_cast<NTV2Channel>(m_activeFsChannel);
    char line[256];

    NTV2VideoFormat actualFmt = NTV2_FORMAT_UNKNOWN;
    const bool fmtOk = card->GetVideoFormat(actualFmt, fsChannel);
    NTV2FrameBufferFormat actualFbf = NTV2_FBF_INVALID;
    const bool fbfOk = card->GetFrameBufferFormat(fsChannel, actualFbf);
    std::snprintf(line, sizeof(line),
        "AJAInput: %s fs%d videoFmt=%s (raw=%d, applied=%s) "
        "fbf=%d (applied=%s)",
        tag,
        static_cast<int>(fsChannel),
        fmtOk ? ::NTV2VideoFormatToString(actualFmt).c_str() : "?",
        fmtOk ? static_cast<int>(actualFmt) : -1,
        fmtOk ? "ok" : "fail",
        fbfOk ? static_cast<int>(actualFbf) : -1,
        fbfOk ? "ok" : "fail");
    FFGLLog::LogToHost(line);

    const NTV2InputXptID fbInputXpt =
        static_cast<NTV2InputXptID>(m_expectedFbInXpt);
    NTV2OutputXptID actualConnectedOXpt = NTV2_XptBlack;
    const bool getConnOk =
        card->GetConnectedOutput(fbInputXpt, actualConnectedOXpt);
    std::snprintf(line, sizeof(line),
        "AJAInput: %s fs%d FB.in <== xpt=%d (expected xpt=%d, "
        "get=%s, match=%s)",
        tag,
        static_cast<int>(fsChannel),
        getConnOk ? static_cast<int>(actualConnectedOXpt) : -1,
        m_expectedFbOutXpt,
        getConnOk ? "ok" : "fail",
        (getConnOk && static_cast<int>(actualConnectedOXpt)
            == m_expectedFbOutXpt) ? "YES" : "NO");
    FFGLLog::LogToHost(line);

    AUTOCIRCULATE_STATUS acStatus;
    if (card->AutoCirculateGetStatus(fsChannel, acStatus))
    {
        std::snprintf(line, sizeof(line),
            "AJAInput: %s fs%d AC startFrame=%d endFrame=%d "
            "state=%d running=%s framesProcessed=%u framesDropped=%u",
            tag,
            static_cast<int>(fsChannel),
            static_cast<int>(acStatus.acStartFrame),
            static_cast<int>(acStatus.acEndFrame),
            static_cast<int>(acStatus.acState),
            acStatus.IsRunning() ? "yes" : "no",
            static_cast<unsigned>(acStatus.acFramesProcessed),
            static_cast<unsigned>(acStatus.acFramesDropped));
        FFGLLog::LogToHost(line);
    }

    // Also surface the live SDI receiver line discipline: is the BNC
    // currently in receive mode, and what 3G sub-format does the
    // physical receiver report. Cheap; tells us if Resolume (or
    // anything else holding a claim) has flipped the SDI back to
    // transmit between configure and now.
    if (input::UsesSdi(m_activeInputNumber))
    {
        const NTV2Channel physBNC = static_cast<NTV2Channel>(
            input::SdiNumber(m_activeInputNumber) - 1);
        bool isTransmit = false;
        const bool xmitOk =
            card->GetSDITransmitEnable(physBNC, isTransmit);
        bool is3Gb = false;
        const bool gb3Ok =
            card->GetSDIInput3GbPresent(is3Gb, physBNC);
        std::snprintf(line, sizeof(line),
            "AJAInput: %s sdi%d direction=%s (get=%s) is3Gb=%s "
            "(get=%s)",
            tag,
            static_cast<int>(physBNC) + 1,
            xmitOk ? (isTransmit ? "TRANSMIT" : "receive") : "?",
            xmitOk ? "ok" : "fail",
            gb3Ok ? (is3Gb ? "yes" : "no") : "?",
            gb3Ok ? "ok" : "fail");
        FFGLLog::LogToHost(line);
    }
}

void AJACaptureSource::TeardownChannel()
{
    if (m_activeInputNumber < 1)
        return;
    CNTV2Card* card = m_device.Card();
    if (!card)
    {
        m_activeInputNumber = 0;
        return;
    }
    // AutoCirculate is bound to the FrameStore, not to the physical
    // SDI BNC. We stop the FrameStore. The signal routing connection
    // we installed in TryConfigureChannel survives across this teardown,
    // which is harmless: the next configure pass overwrites it.
    //
    // The input VBI subscription is the only event subscription we
    // still hold here; the output VBI was subscribed for the
    // bidir-flip wait inside TryConfigureChannel and unsubscribed
    // immediately after, so it does not need balancing on this path.
    NTV2Channel fsChannel = NTV2_CHANNEL1;
    if (FrameStoreZeroBasedToNTV2(m_frameStoreZeroBased, fsChannel))
    {
        // v0.40: if another application has meanwhile started a playout
        // on this FrameStore, it is theirs now; do not stop it.
        AUTOCIRCULATE_STATUS st;
        const bool foreignPlayout =
            card->AutoCirculateGetStatus(fsChannel, st) &&
            !st.IsStopped() && st.IsOutput();
        if (!foreignPlayout)
            card->AutoCirculateStop(fsChannel, /*inAbort*/true);
        card->UnsubscribeInputVerticalEvent(fsChannel);
        // v0.42: a 4K capture over several FrameStores hands them back in
        // normal mode.
        if (!foreignPlayout && m_activeTopology != Topology::Single)
            ClearMultiFrameStoreMode(card);
    }
    // v0.42: on a one-format card the card's format is free again.
    MultiFormatRegistry::ReleaseCardFormat(m_device.GetStatus().deviceIndex,
                                           m_frameStoreZeroBased);
    m_activeTopology = Topology::Single;
    m_activeInputNumber = 0;
}

int AJACaptureSource::ProbeLiveFormat(CNTV2Card* card, int inputNumber) const
{
    NTV2InputSource source = NTV2_INPUTSOURCE_INVALID;
    NTV2Channel     conn   = NTV2_CHANNEL1;
    bool            isSdi  = false;
    if (card == nullptr || !input::ToNTV2(inputNumber, source, conn, isSdi))
        return 0;
    const int raw = static_cast<int>(card->GetInputVideoFormat(source, false));
    if (input::IsQuad(inputNumber))
        return QuadLinkFormatFromLink(raw);
    return MapLevelBFormatToLevelA(raw);
}

void AJACaptureSource::ClearMultiFrameStoreMode(CNTV2Card* card)
{
    if (card == nullptr)
        return;
    NTV2Channel fs = NTV2_CHANNEL1;
    if (!FrameStoreZeroBasedToNTV2(m_frameStoreZeroBased, fs))
        return;
    if (m_activeTopology == Topology::Tsi2)
        card->SetTsiFrameEnable(false, fs);
    else if (m_activeTopology == Topology::Squares4)
        card->Set4kSquaresEnable(false, fs);
}

void AJACaptureSource::ClearStaleMultiFrameStoreModes(CNTV2Card* card, int fs)
{
    if (card == nullptr || fs < 0 || fs > 7)
        return;
    const NTV2DeviceID id = card->GetDeviceID();
    const uint32_t others = m_device.BusyFrameStores(/*playoutOnly*/false) &
                            ~(1u << fs);
    if (::NTV2DeviceCanDo425Mux(id))
    {
        const int      pairFirst = fs & ~1;
        const uint32_t pair      = 3u << pairFirst;
        bool on = false;
        if ((others & pair) == 0u &&
            card->GetTsiFrameEnable(on, static_cast<NTV2Channel>(pairFirst)) && on)
            card->SetTsiFrameEnable(false, static_cast<NTV2Channel>(fs));
    }
    if (::NTV2DeviceCanDo4KVideo(id))
    {
        const int      groupFirst = fs < 4 ? 0 : 4;
        const uint32_t group      = 0xFu << groupFirst;
        bool on = false;
        if ((others & group) == 0u &&
            card->Get4kSquaresEnable(on, static_cast<NTV2Channel>(groupFirst)) && on)
            card->Set4kSquaresEnable(false, static_cast<NTV2Channel>(fs));
    }
}

void AJACaptureSource::SampleSignalledTransfer(CNTV2Card* card, int inputNumber)
{
    // v0.42: which HDR transfer the source signals, for Colorimetry Auto.
    //   SDI:  the payload ID (SMPTE ST 352) transfer characteristics bits.
    //   HDMI: the EOTF of the Dynamic Range and Mastering InfoFrame
    //         (CTA-861: 2 = SMPTE ST 2084 / PQ, 3 = HLG); the SDK reads it
    //         for HDMI inputs 1 and 2.
    // Anything else, or nothing signalled, counts as SDR (0).
    NTV2InputSource source = NTV2_INPUTSOURCE_INVALID;
    NTV2Channel     conn   = NTV2_CHANNEL1;
    bool            isSdi  = false;
    int transfer = 0;
    if (card != nullptr && input::ToNTV2(inputNumber, source, conn, isSdi))
    {
        if (isSdi)
        {
            ULWord a = 0, b = 0;
            if (card->ReadSDIInVPID(conn, a, b))
            {
                const CNTV2VPID vpid(a);
                if (vpid.IsValid())
                {
                    const NTV2VPIDXferChars tc = vpid.GetTransferCharacteristics();
                    transfer = (tc == NTV2_VPID_TC_PQ)  ? 1
                             : (tc == NTV2_VPID_TC_HLG) ? 2 : 0;
                }
            }
        }
        else if (conn <= NTV2_CHANNEL2)
        {
            HDRRegValues drm;
            if (card->GetHDMIInDynamicRange(drm, conn))
                transfer = (drm.electroOpticalTransferFunction == 2) ? 1
                         : (drm.electroOpticalTransferFunction == 3) ? 2 : 0;
        }
    }
    const int previous = m_signalledTransfer.exchange(transfer, std::memory_order_relaxed);
    if (previous != transfer)
    {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "AJAInput: %s signals %s",
                      input::LabelUpper(inputNumber),
                      transfer == 1 ? "PQ (HDR)" : transfer == 2 ? "HLG (HDR)" : "SDR");
        FFGLLog::LogToHost(line);
    }
}

void AJACaptureSource::HandleNoFrameAvailable(
    CNTV2Card*                                   card,
    int                                          inputSourceRaw,
    std::chrono::steady_clock::time_point&       lastNoSignalRetry)
{
    // Bump idle-wakeup counter. This is the metric the operator
    // watches to spot "card alive but consumer/clock mismatch".
    m_idleWakeups.fetch_add(1, std::memory_order_relaxed);

    // While we are awake anyway, check the input format. The cable
    // might have come out, or the upstream may have switched
    // resolution: detecting it here avoids sitting in Running state
    // with no transfers happening. Note we probe the PHYSICAL input
    // source here, not the FrameStore — what we want to know is
    // whether the cable still has a signal locked.
    // v0.42: the format as configured (a quad-link input's 4K picture).
    (void)inputSourceRaw;
    const NTV2VideoFormat fmt = static_cast<NTV2VideoFormat>(
        ProbeLiveFormat(card, m_activeInputNumber));
    m_lastVideoFormatRaw.store(static_cast<int>(fmt),
                               std::memory_order_relaxed);
    // Keep the multi-format registry in step with the cable (v0.40).
    if (fmt == NTV2_FORMAT_UNKNOWN)
        MultiFormatRegistry::Withdraw(m_device.GetStatus().deviceIndex,
                                      m_frameStoreZeroBased);
    else
        MultiFormatRegistry::Publish(m_device.GetStatus().deviceIndex,
                                     m_frameStoreZeroBased,
                                     m_activeInputNumber,
                                     static_cast<int>(fmt));

    // Three outcomes:
    //   (a) no lock or undecodable      → NoSignal
    //   (b) lock + decoded + raster ==  → stay Running
    //   (c) lock + decoded + raster !=  → publish raster-change AND
    //                                     drop to NoSignal so the
    //                                     render thread takes over
    //                                     re-allocation
    //
    // (c) fires on either a dimension change OR a NTV2VideoFormat code
    // change — see the matching comment in TryConfigureChannel for the
    // 1080p30↔1080p60 case that motivates the format-code arm.
    CapturedFormat decoded;
    bool stillUsable    = false;
    bool rasterMismatch = false;
    if (fmt != NTV2_FORMAT_UNKNOWN &&
        TryDecodeNTV2Format(static_cast<int>(fmt), decoded))
    {
        const bool dimsOk =
            decoded.width  == m_width && decoded.height == m_height;
        const bool fmtOk =
            m_initialNtv2FormatRaw == 0 ||
            decoded.ntv2FormatRaw == m_initialNtv2FormatRaw;
        if (dimsOk && fmtOk)
            stillUsable = true;
        else
            rasterMismatch = true;
    }

    if (rasterMismatch)
    {
        // Publish the new geometry BEFORE the format-change flag so
        // the render thread's acquire-load on
        // ConsumePendingFormatChange will see both stores ordered
        // correctly. The flag is the gate; the geometry is the
        // payload.
        m_pendingPacked.store(
            (uint32_t(decoded.width)  << 16) |
             uint32_t(decoded.height & 0xFFFFu),
            std::memory_order_relaxed);
        m_pendingRawFormat.store(decoded.ntv2FormatRaw,
                                 std::memory_order_relaxed);
        m_pendingFormatChange.store(true, std::memory_order_release);
    }

    if (!stillUsable)
    {
        TeardownChannel();
        m_state.store(State::NoSignal, std::memory_order_release);
        lastNoSignalRetry = std::chrono::steady_clock::now();
    }
}

void AJACaptureSource::SampleSDIStatisticsIfDue(
    CNTV2Card*       card,
    bool             isSDI,
    bool             canSdiChecks,
    int              inputSourceRaw,
    uint32_t&        framesSinceSdiSample)
{
    // Once a second is enough to spot a cable that's losing lock or
    // accumulating CRC errors. HDMI inputs and cards without SDI
    // checks skip this entirely so the atomic state stays at its
    // "everything fine" defaults.
    constexpr uint32_t kSdiSampleInterval = 60;
    if (!isSDI || !canSdiChecks)
        return;
    if (++framesSinceSdiSample < kSdiSampleInterval)
        return;

    framesSinceSdiSample = 0;
    NTV2SDIInStatistics stats;
    if (!card->ReadSDIStatistics(stats))
        return;

    const NTV2InputSource inputSource =
        static_cast<NTV2InputSource>(inputSourceRaw);
    const UWord row = static_cast<UWord>(
        ::GetIndexForNTV2InputSource(inputSource));
    NTV2SDIInputStatus s;
    if (stats.GetSDIInputStatus(s, row))
    {
        m_sdiLocked.store(s.mLocked, std::memory_order_relaxed);
        m_sdiCrcErrors.store(
            uint32_t(s.mCRCTallyA) + uint32_t(s.mCRCTallyB),
            std::memory_order_relaxed);
        m_sdiUnlocks.store(s.mUnlockTally, std::memory_order_relaxed);
    }
}

void AJACaptureSource::CaptureLoop()
{
    ApplyThreadPriorityAndAffinity();

    // Activate the secondary GL context on this thread. The shared
    // context is identical to the one the host primary context belongs
    // to (Resolume's render context), so any GL objects we touch here
    // — most critically the texture ring's textures, PBOs and fences
    // — are visible to the render thread's primary context for
    // sampling. Without this, every gl* call invoked through
    // m_textureRing->Upload() silently drops on the floor (no current
    // context → glMapBufferRange returns null → memcpy never happens
    // → glTexSubImage2D would have been called with no PBO bound),
    // the texture stays at its glTexImage2D-allocated zero state,
    // the shader samples zeros and BT.709 limited yields a uniform
    // green G≈0.30 / R=B=0. That was the cause of the "verde uniforme"
    // failure during v0.34 hardware bring-up.
    //
    // FrameProducer applies the exact same pattern (and the comment
    // there explains the same gotcha); we mirror it here verbatim so
    // both producers behave identically with respect to context
    // affinity. If sharedContext is nullptr, the off-thread upload
    // path was never enabled (texture ring was not initialised),
    // m_textureRing is also nullptr, and the byte-queue fallback is
    // what runs — no GL on this thread either way.
    const bool sharedContextActive =
        (m_sharedContext != nullptr) &&
        m_sharedContext->ActivateOnCurrentThread();

    // Decide once for the lifetime of this capture session whether we
    // can have AJA's DMA engine write directly into the SharedTextureRing
    // PBOs. That requires:
    //  - we have a texture ring (off-thread upload path is wired up)
    //  - the ring was built on a driver that supports GL_ARB_buffer_storage,
    //    so the PBOs are persistently host-mapped
    // When both hold, m_transferBuffer is unused: every AutoCirculateTransfer
    // (or DMAReadFrame in low-latency mode) writes straight into the
    // writer-slot PBO, and Upload's "frameData == persistent pointer"
    // fast path means there is zero host-to-host copy on the way to GPU.
    // This is the biggest single Windows-side optimisation available
    // short of GPUDirect: it saves the ~2 ms (1080p UYVY) memcpy from
    // m_transferBuffer into the PBO that the classic path pays per frame.
    const bool persistentDMA =
        (m_textureRing != nullptr) &&
        m_textureRing->HasPersistentMapping();

    AUTOCIRCULATE_TRANSFER xfer;
    // Initial SetVideoBuffer with a placeholder. The real target pointer
    // is selected per iteration below (writer-slot PBO when persistentDMA
    // is on; m_transferBuffer otherwise) and rebound right before each
    // AutoCirculateTransfer. We seed with m_transferBuffer here even on
    // the persistent path so the field is non-null and well-defined; it
    // gets overwritten before any real transfer.
    xfer.SetVideoBuffer(reinterpret_cast<ULWord*>(m_transferBuffer.data()),
                        static_cast<ULWord>(m_frameBytes));

    // Page-lock the DMA destination buffer(s) for the lifetime of the
    // capture thread. The AJA driver's DMA engine has to either pin the
    // buffer itself (slow, done per-transfer) or use a kernel
    // scatter-gather list built from already-pinned host pages. By
    // calling DMABufferLock once up front we hand the driver the latter
    // path: it caches the page-table mapping and reuses it across every
    // AutoCirculateTransfer, shaving a measurable amount of per-frame
    // overhead and — more importantly — eliminating the per-frame jitter
    // that comes from the kernel walking the page tables on every
    // transfer.
    //
    // When persistentDMA is on, every PBO slot in the ring becomes a
    // legitimate DMA destination, so we lock all of them. The slot
    // rotation through writer/mid/reader walks all three over time;
    // not locking them all would leave some transfers paying the
    // per-call pinning cost.
    //
    // The matching DMABufferUnlock(s) at the bottom of this function
    // release the kernel resources back. We must keep the buffer(s)
    // alive (not resized, not freed) for as long as they are locked.
    // The texture ring is owned by AJAInputPlugin which outlives this
    // capture thread, and m_transferBuffer is owned by us, so both
    // are safe.
    //
    // The call is a no-op (with a returned false) on drivers that do
    // not support buffer-locking; we deliberately do not abort capture
    // if it fails. The performance hit is invisible in those cases.
    CNTV2Card* lockCard = m_device.Card();
    bool didLockSlots[ajainput::SharedTextureRing::SlotCount()] = {};

    // m_transferBuffer page lock (v0.40: tracked, lazy). It is the DMA
    // target on the fallback path (no persistent PBOs) AND in AC field
    // mode, where every field lands there before the scatter. Up to
    // v0.39 it was only locked on the fallback path, so field mode, the
    // path every interlaced UYVY source takes, made the driver pin and
    // unpin those pages on every transfer. Locked on first use and
    // re-locked if the buffer is ever re-allocated (TryConfigureChannel
    // resizes it when the pixel kind changes); one attempt per
    // buffer so a failing lock is not retried every field.
    ULWord* xferLockPtr       = nullptr;
    ULWord  xferLockBytes     = 0;
    bool    xferLockOk        = false;
    auto ensureTransferBufferLocked = [&](CNTV2Card* c)
    {
        ULWord* const ptr   = reinterpret_cast<ULWord*>(m_transferBuffer.data());
        const ULWord  bytes = static_cast<ULWord>(m_transferBuffer.size());
        if (c == nullptr || ptr == nullptr || bytes == 0)
            return;
        if (ptr == xferLockPtr && bytes == xferLockBytes)
            return;  // already attempted for this buffer
        if (xferLockOk)
            c->DMABufferUnlock(xferLockPtr, xferLockBytes);
        xferLockPtr   = ptr;
        xferLockBytes = bytes;
        xferLockOk    = c->DMABufferLock(ptr, bytes, /*inMap*/true,
                                         /*inRDMA*/false);
    };
    if (persistentDMA && lockCard != nullptr)
    {
        for (int i = 0;
             i < ajainput::SharedTextureRing::SlotCount();
             ++i)
        {
            void* ptr = m_textureRing->PersistentPtrForSlot(i);
            if (ptr != nullptr)
            {
                didLockSlots[i] = lockCard->DMABufferLock(
                    reinterpret_cast<ULWord*>(ptr),
                    static_cast<ULWord>(m_frameBytes),
                    /*inMap*/true,
                    /*inRDMA*/false);
            }
        }
    }
    else
    {
        ensureTransferBufferLocked(lockCard);
    }

    uint64_t lastDriverDropCount = 0;

    // v0.38: workaround for a Windows-only bug documented in the AJA
    // SDK (ntv2card.h docstring on WaitForInputVerticalInterrupt and
    // WaitForInputFieldID): on Windows, the wait uses an event handle
    // that is "only cleared by a previous call to Wait" — different
    // from Linux/macOS where the event is cleared before the wait.
    // The practical consequence with two plugin instances on the
    // same card is that an event signalled by ONE instance's vsync
    // can wake up BOTH instances' waits, even though only one channel
    // has a fresh frame to consume. The "loser" then sees
    // !HasAvailableInputFrame() and previously counted that as an
    // idle wakeup, even though the wait was spurious (the other
    // channel's vsync, not ours).
    //
    // The SDK documents the fix: read GetInputVerticalInterruptCount
    // before and after the wait. If the count did not advance for our
    // channel, the wait returned without our channel actually
    // experiencing a vsync — loop back and wait again without doing
    // any status checks or counting an idle wakeup. The driver bumps
    // the per-channel interrupt count atomically on the actual
    // hardware vsync, so this is a reliable arbiter between real and
    // spurious wakes.
    //
    // Initial value 0 means the first iteration's compare-and-skip
    // never triggers (lastVertInterruptCount=0, current>0 on first
    // real vsync ⇒ proceeds). Subsequent iterations track real
    // progress.
    ULWord lastVertInterruptCount = 0;

    // v0.40 backlog drain (AutoCirculate path). When a transfer leaves
    // more captured frames (or fields) in the driver ring, the next loop
    // iteration transfers again WITHOUT waiting for the next interrupt.
    // Before v0.40 the loop did exactly one transfer per interrupt, so a
    // single late wake-up (OS scheduling, a busy GPU driver) left the
    // capture one frame behind the input for the rest of the session:
    // one frame arrives and one is transferred per interrupt, and the
    // backlog never shrank. The drain count caps consecutive drains at
    // the ring size as a guard against a driver reporting a level that
    // never goes down.
    bool acDrainPending = false;
    int  acDrainCount   = 0;

    // SDI input statistics sampling state. Reading the stats registers
    // is cheap but not free, so we throttle to once per second
    // (~60 frames at 60 Hz, ~50 at 50 Hz; close enough either way). The
    // capability flag is captured once at loop start because it cannot
    // change for the life of an open device.
    const bool canSdiChecks =
        m_device.GetStatus().canDoSDIChecks;
    // Counter passed by reference to SampleSDIStatisticsIfDue; the
    // helper increments and resets it. Lives across iterations so
    // the periodic sampling cadence (~1 s at 60 Hz) is stable.
    uint32_t framesSinceSdiSample = 0;

    using clock = std::chrono::steady_clock;
    auto lastNoSignalRetry = clock::now();

    while (m_running.load(std::memory_order_acquire))
    {
        // v0.41: parked after giving the FrameStore up; the owner
        // rebuilds the capture when its clip is shown again.
        if (m_yielded.load(std::memory_order_acquire))
        {
            m_state.store(State::NoSignal, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (MaybeYield())
            continue;

        const State state = m_state.load(std::memory_order_acquire);

        switch (state)
        {
        case State::Idle:
        case State::Initializing:
        {
            // Tear down any previously active channel before re-init so a
            // user-driven SetInputNumber cleanly relinquishes the old
            // channel even if it was Running.
            if (m_activeInputNumber != 0)
                TeardownChannel();

            // Snapshot before TryConfigureChannel reads the targets; see
            // m_configGeneration.
            const std::uint32_t configGen =
                m_configGeneration.load(std::memory_order_acquire);

            if (TryConfigureChannel())
            {
                if (m_configGeneration.load(std::memory_order_acquire) !=
                    configGen)
                {
                    // A setting changed while we were configuring: go
                    // round again (the next pass tears this channel
                    // down and applies the new targets).
                    m_state.store(State::Initializing,
                                  std::memory_order_release);
                    break;
                }
                m_activeConfigGeneration = configGen;


                // v0.39 safety net. Every byte count downstream (the
                // persistent-PBO DMA target, Upload's memcpy, the field
                // accumulation buffer) assumes the sink was sized for
                // exactly the frame we capture. A mismatch means the
                // owner allocated the GL ring or queue for a different
                // pixel kind than the one this session resolved to
                // (e.g. 10-bit requested on a card without 10-bit), and
                // capturing anyway would DMA past the end of a mapped
                // PBO or over-read a host buffer. Refuse instead: the
                // owner re-creates the capture after re-sizing.
                const std::size_t sinkBytes = m_textureRing
                    ? m_textureRing->FrameBytes()
                    : m_queue.FrameBytes();
                if (sinkBytes != m_frameBytes)
                {
                    char line[192];
                    std::snprintf(line, sizeof(line),
                        "[AJAInput] capture sink size mismatch "
                        "(sink=%zu bytes, frame=%zu bytes); capture "
                        "stopped to avoid a buffer overrun.",
                        sinkBytes, m_frameBytes);
                    FFGLLog::LogToHost(line);
                    TeardownChannel();
                    m_state.store(State::Failed, std::memory_order_release);
                    break;
                }

                if (state == State::Initializing &&
                    m_recoveryCount.load(std::memory_order_relaxed) > 0)
                {
                    // Re-entry after NoSignal counts as a recovery; we
                    // pre-increment the counter on the NoSignal → Init
                    // transition below, so nothing to add here.
                }
                lastDriverDropCount = 0;
                // v0.38: reset spurious-wake tracking too. After a
                // re-init the per-channel interrupt counter may have
                // advanced while we were torn down; starting from
                // wherever it is now means our first Wait does a
                // GetInputVerticalInterruptCount, sees the value
                // (already non-zero), stores it, and lets the loop
                // run normally. The first iteration after re-init
                // skips the spurious-wake gate harmlessly.
                lastVertInterruptCount = 0;
                m_fieldFlagAnomalyLogged = false;
                acDrainPending = false;
                acDrainCount   = 0;
                m_state.store(State::Running, std::memory_order_release);
            }
            else
            {
                // v0.42: a failed configure holds no claim on a one-format
                // card's format, and leaves no TSI / quad mode on (set
                // before AutoCirculate init, which can still fail).
                MultiFormatRegistry::ReleaseCardFormat(
                    m_device.GetStatus().deviceIndex, m_frameStoreZeroBased);
                if (m_activeTopology != Topology::Single)
                {
                    ClearMultiFrameStoreMode(m_device.Card());
                    m_activeTopology = Topology::Single;
                }
                m_state.store(State::NoSignal, std::memory_order_release);
                lastNoSignalRetry = clock::now();
            }
            break;
        }

        case State::Running:
        {
            // v0.39: a setter that bumped the generation after the
            // Initializing pass re-checked it (but before Running was
            // stored) found the state still Initializing and skipped its
            // CAS; catch that here.
            if (m_configGeneration.load(std::memory_order_acquire) !=
                m_activeConfigGeneration)
            {
                m_state.store(State::Initializing, std::memory_order_release);
                break;
            }

            CNTV2Card* card = m_device.Card();
            if (!card)
            {
                m_state.store(State::Failed, std::memory_order_release);
                break;
            }

            NTV2Channel     physBNCChannel = NTV2_CHANNEL1;
            NTV2InputSource inputSource    = NTV2_INPUTSOURCE_SDI1;
            bool            isSDI          = true;
            if (!ResolveInputSelection(m_activeInputNumber,
                                       physBNCChannel, inputSource, isSDI))
            {
                // We should never reach Running with an invalid channel;
                // if we do, go back to Initializing so the state machine
                // re-derives the channel from the user-facing target.
                m_state.store(State::Initializing,
                              std::memory_order_release);
                break;
            }
            (void)physBNCChannel;
            (void)isSDI;
            NTV2Channel fsChannel = NTV2_CHANNEL1;
            FrameStoreZeroBasedToNTV2(m_frameStoreZeroBased, fsChannel);

            // Branch on the configured transfer mode. The atomic flag
            // is mirrored to a thread-local at every Initializing pass,
            // so this branch is stable for the duration of the capture
            // session; toggling the mode forces the loop back through
            // Initializing where the new mode takes effect.
            //
            // v0.37: dispatch on m_useACPath (the derived flag) rather
            // than m_activeLowLatencyMode (the user-facing toggle).
            // For interlaced UYVY sources the user's LL preference is
            // honoured by routing through AC field mode, which is
            // structurally faster than the manual LL path for that
            // content. See the m_useACPath block comment for the full
            // rationale.
            if (!m_useACPath)
            {
                // Low-latency manual ping-pong path. Mirrors the SDK's
                // ntv2llburn demo capture loop.
                //
                // WaitForInputFieldID returns false if the wait timed
                // out, which the SDK documents as "often due to the
                // FrameStore being configured for playout, or if its
                // input is not receiving a valid signal." We use it
                // as our NoSignal detector since there is no
                // AutoCirculate status to consult in this mode.
                if (!card->WaitForInputFieldID(NTV2_FIELD0, fsChannel))
                {
                    HandleNoFrameAvailable(card,
                                           static_cast<int>(inputSource),
                                           lastNoSignalRetry);
                    continue;
                }

                // v0.38: spurious-wake check, same Windows SDK quirk
                // as the AC path. WaitForInputFieldID's docstring
                // calls out the same bug (event-not-cleared-before-
                // wait on Windows). With multi-instance setups, the
                // wait can return without our channel's field having
                // arrived. Use the per-channel interrupt count as
                // arbiter: if it didn't advance, retry the wait.
                ULWord currentVertInterruptCount = 0;
                if (card->GetInputVerticalInterruptCount(
                        currentVertInterruptCount, fsChannel))
                {
                    if (currentVertInterruptCount == lastVertInterruptCount)
                        continue;
                    lastVertInterruptCount = currentVertInterruptCount;
                }

                if (!m_running.load(std::memory_order_acquire) ||
                    m_state.load(std::memory_order_acquire) != State::Running)
                    break;

                const auto producedAt = clock::now();

                // Pick the slot that the hardware finished writing at
                // this vblank: it is the one BEFORE the cycle slot in
                // the rotation. With N=2 this collapses to "the other
                // slot" (the old ^= 1 behavior). With N=3 it walks
                // through the rotation one position behind where the
                // hardware is currently writing, leaving one slot
                // idle as buffer slack.
                //
                // Latency is one frame interval in steady state for
                // any N in the supported range: every DMA reads a
                // slot that completed at the vblank we just woke
                // from. The price of larger N is not latency but
                // memory. Only the first iteration after priming reads
                // a never-written slot; it is not published (v0.40,
                // m_llWarmupSkip).
                const uint32_t llN = static_cast<uint32_t>(
                    std::min(m_activeRingFrameCount, 3));
                const uint32_t completedSlot =
                    (m_llCycleSlot + llN - 1u) % llN;
                m_currentInFrame = m_inputStartFrame + completedSlot;

                // Direct DMA read of the just-completed slot. No driver
                // ring, no AutoCirculateTransfer state machine; the
                // call blocks until the DMA engine finishes copying
                // the device frame buffer into our host buffer.
                //
                // captureTarget is the writer-slot PBO pointer when
                // persistent mapping is on (zero-copy: AJA writes
                // straight into GL-allocated host pinned memory),
                // m_transferBuffer.data() otherwise (classic path
                // with a subsequent memcpy in Upload). Reset every
                // iteration because the writer slot rotates after
                // each Upload.
                uint8_t* captureTarget = persistentDMA
                    ? static_cast<uint8_t*>(m_textureRing->WriterPersistentPtrForWrite())
                    : m_transferBuffer.data();
                if (captureTarget == nullptr)  // safety; should never trigger
                    captureTarget = m_transferBuffer.data();
                if (captureTarget == m_transferBuffer.data())
                    ensureTransferBufferLocked(card);

                // v0.39: pass the FrameStore. The 3-argument overload
                // converts the frame number with the card's base frame
                // size (ntv2dma.cpp), but AutoCirculate numbered our
                // slots in this FrameStore's frame size, which is 4x
                // larger for quad/TSI 4K rasters. The 4-argument
                // overload applies the quad multiplier; for HD/SD the
                // two are identical.
                if (!card->DMAReadFrame(
                        m_currentInFrame,
                        reinterpret_cast<ULWord*>(captureTarget),
                        static_cast<ULWord>(m_frameBytes),
                        fsChannel))
                {
                    // DMA failure on a healthy signal is an extremely
                    // rare bus condition. Treat it as a transient and
                    // fall back to NoSignal so the recovery path
                    // gives the device a moment to settle.
                    TeardownChannel();
                    m_state.store(State::NoSignal,
                                  std::memory_order_release);
                    lastNoSignalRetry = clock::now();
                    continue;
                }

                // Advance the cycle and tell the hardware which slot
                // to write into for the NEXT interval. The new slot
                // is two positions ahead of what we just DMA-read
                // (one position ahead of the slot the hardware is
                // currently writing). For N=2 this means SetInputFrame
                // back to the slot we just DMA'd (the rotation has
                // only two members); for N=3 it skips over the
                // currently-writing slot to keep the rotation moving
                // forward. The "idle" slot that exists for N=3 is
                // never both being-DMA'd and being-written at the
                // same time, which is what gives the cushion.
                //
                // This SetInputFrame is the linchpin of the rotation:
                // dropping it would make the hardware keep writing
                // to the same slot every frame and the DMA would
                // race with the active write.
                m_llCycleSlot = (m_llCycleSlot + 1u) % llN;
                card->SetInputFrame(fsChannel,
                                    m_inputStartFrame + m_llCycleSlot);

                // Read embedded SDI timecode directly from the card.
                // The AutoCirculate path pulled this from the
                // transfer status struct; here we go to the source.
                // GetRP188Data takes the SDI input channel directly
                // and returns the embedded timecode for that input;
                // unlike the frame-stamp variant it does not need an
                // NTV2TCIndex disambiguator because the card already
                // exposes one RP188 register per input.
                if (isSDI)
                {
                    NTV2_RP188 rp188;
                    if (card->GetRP188Data(physBNCChannel, rp188)
                        && rp188.IsValid())
                    {
                        CRP188 tc(rp188);
                        ULWord h = 0, m = 0, s = 0, f = 0;
                        tc.GetRP188Hrs(h);
                        tc.GetRP188Mins(m);
                        tc.GetRP188Secs(s);
                        tc.GetRP188Frms(f);
                        const uint32_t packed =
                            (uint32_t(h & 0xff) << 24) |
                            (uint32_t(m & 0xff) << 16) |
                            (uint32_t(s & 0xff) << 8)  |
                            (uint32_t(f & 0xff));
                        m_lastTimecodeHMSF.store(packed,
                                                 std::memory_order_relaxed);
                        m_lastTimecodeValid.store(true,
                                                  std::memory_order_relaxed);
                    }
                }

                // v0.40: the first DMA after (re)configuring reads a
                // slot the hardware never wrote this session (it was
                // latching the primed slot during that interval), which
                // showed up as one frame of stale card memory on every
                // start, channel switch or recovery. Skip publishing it.
                if (m_llWarmupSkip)
                {
                    m_llWarmupSkip = false;
                    break;
                }

                if (m_textureRing)
                    m_textureRing->Upload(captureTarget, producedAt);
                else
                    m_queue.WriteFrame(captureTarget, producedAt);

                m_framesCaptured.fetch_add(1, std::memory_order_relaxed);

                SampleSDIStatisticsIfDue(card, isSDI, canSdiChecks,
                                         static_cast<int>(inputSource),
                                         framesSinceSdiSample);
                if (++m_framesSinceTransferSample >= 60)
                {
                    m_framesSinceTransferSample = 0;
                    SampleSignalledTransfer(card, m_activeInputNumber);
                }
                break;
            }

            // Standard AutoCirculate path below.

            // v0.40: a pending drain skips the wait (and the spurious-
            // wake gate, which only makes sense right after a wait).
            const bool draining = acDrainPending;
            acDrainPending = false;
            // The first real wait after a drain may return at once on
            // Windows: an interrupt whose frame the drain already took
            // still left the event set (and advanced the interrupt
            // count, so the v0.38 gate lets it through). Finding the
            // ring empty then is expected, not an idle wake-up.
            const bool firstWaitAfterDrain = !draining && acDrainCount > 0;
            if (!draining)
                acDrainCount = 0;

            // Block on the FrameStore's vertical interrupt: lowest-latency
            // wake-up the SDK offers, aligned to the input clock once
            // routing is in place.
            const bool woke = draining
                ? false
                : card->WaitForInputVerticalInterrupt(fsChannel);

            // v0.38: spurious-wake check. See lastVertInterruptCount
            // declaration above for the full rationale (Windows-only
            // SDK quirk where multi-instance setups can cause one
            // instance's wait to wake the other). If the interrupt
            // count for our channel did not advance, this wake was
            // spurious — loop back to the wait without counting an
            // idle wakeup or doing any AC status work. On Linux/macOS
            // this branch never triggers because the SDK clears the
            // event before each wait, so it's free for those
            // platforms.
            //
            // v0.39: the gate only applies to a wait that returned
            // true. A false return is a timeout (ntv2card.h: "often
            // due to ... its input is not receiving a valid signal"),
            // and on a timeout the counter has not moved either; the
            // v0.38 gate turned that into an endless `continue`, so a
            // pulled cable never reached HandleNoFrameAvailable and
            // the instance sat in Running on a frozen frame. Timeouts
            // now fall through to the status check exactly as before
            // v0.38.
            ULWord currentVertInterruptCount = 0;
            if (woke &&
                card->GetInputVerticalInterruptCount(currentVertInterruptCount,
                                                     fsChannel))
            {
                if (currentVertInterruptCount == lastVertInterruptCount)
                    continue;  // spurious wake, retry
                lastVertInterruptCount = currentVertInterruptCount;
            }
            // (If GetInputVerticalInterruptCount itself failed, we
            // fall through to the normal path: better to potentially
            // over-count idle wakeups than to deadlock on a driver
            // hiccup.)

            if (!m_running.load(std::memory_order_acquire) ||
                m_state.load(std::memory_order_acquire) != State::Running)
                break;

            AUTOCIRCULATE_STATUS status;
            if (!card->AutoCirculateGetStatus(fsChannel, status))
                continue;

            // Defensive check: if AutoCirculate has somehow been stopped
            // out from under us (a buggy concurrent app, a driver
            // recovery cycle), the IsRunning() flag flips false. Without
            // this check we would spin reading "no frame available"
            // forever. The AJA capture demo includes this guard and we
            // follow suit.
            //
            // v0.40: STARTING is not a failure. AutoCirculate only starts
            // recording on a field-0 interrupt (ntv2autocirc.c, "start
            // capture on field 0"), so for interlaced sources the first
            // wake after AutoCirculateStart can land on field 1 with the
            // channel still STARTING; tearing down there bounced about
            // half of all interlaced session starts through NoSignal.
            // A wait that timed out while STARTING still goes through
            // the no-frame path, so a missing signal is detected
            // (HandleNoFrameAvailable probes the input and drops to
            // NoSignal when nothing is locked).
            // v0.40: someone re-initialised our FrameStore for playout
            // (AutoCirculate reports an output crosspoint). Never
            // transfer on it; report and go to NoSignal. TeardownChannel
            // leaves a foreign playout running.
            if (status.IsOutput())
            {
                NoteForeignUse(kForeignFrameStorePlayout,
                    "FrameStore was taken over for output by another "
                    "application while capturing");
                TeardownChannel();
                m_state.store(State::NoSignal, std::memory_order_release);
                lastNoSignalRetry = clock::now();
                continue;
            }
            if (status.IsStarting())
            {
                if (!woke && !draining)
                    HandleNoFrameAvailable(card,
                                           static_cast<int>(inputSource),
                                           lastNoSignalRetry);
                continue;
            }
            if (!status.IsRunning())
            {
                TeardownChannel();
                m_state.store(State::NoSignal,
                              std::memory_order_release);
                lastNoSignalRetry = clock::now();
                continue;
            }

            // Track driver-side drops monotonically.
            const uint64_t total = static_cast<uint64_t>(status.acFramesDropped);
            if (total > lastDriverDropCount)
            {
                m_driverDrops.fetch_add(total - lastDriverDropCount,
                                        std::memory_order_relaxed);
                lastDriverDropCount = total;
            }

            // v0.40: transfer as soon as ONE captured frame is ready.
            // HasAvailableInputFrame() is GetBufferLevel() > 1: it keeps
            // one complete frame in hand, so every transfer delivered
            // the frame that finished one interrupt EARLIER than the
            // newest (one frame of latency, one field in field mode).
            // The SDK documents why (ntv2publicinterface.h, at
            // HasAvailableInputFrame): at level 1 the firmware audio
            // system may not have written the last 512-byte chunk of
            // the frame's audio yet. Video and ANC are complete at the
            // interrupt. We never capture audio (AutoCirculate is
            // initialised with NTV2_AUDIOSYSTEM_INVALID), so level >= 1
            // is safe here; it is also what the LL path does (DMA of the
            // slot completed at the interrupt we woke from).
            if (status.GetBufferLevel() < 1)
            {
                // During a drain, an empty ring just means we caught up:
                // go back to waiting without counting an idle wake-up or
                // probing the input format.
                if (!draining && !(firstWaitAfterDrain && woke))
                    HandleNoFrameAvailable(card,
                                           static_cast<int>(inputSource),
                                           lastNoSignalRetry);
                continue;
            }

            // Rebind the AC transfer destination. There are two paths:
            //
            // FRAME MODE (m_fieldModeActive == false): exactly the
            //   v0.31 behaviour. The DMA target is the writer PBO
            //   when persistent mapping is on, or m_transferBuffer
            //   otherwise. The full frame is uploaded to the ring in
            //   a single Upload call.
            //
            // FIELD MODE (m_fieldModeActive == true, v0.32+): the DMA
            //   target is ALWAYS m_transferBuffer (sized for a full
            //   frame, but only the first half is written per
            //   transfer). The persistent-mapped fast path is bypassed
            //   here because the field rows arrive contiguously and
            //   we need to scatter them into every-other-line of the
            //   accumulator before uploading. After the transfer, the
            //   half-frame data is memcpy'd from m_transferBuffer
            //   into the appropriate row offsets of m_fieldAccumBuffer
            //   (offset 0 for top-field rows, +linePitch for bottom),
            //   and the FULL accumulator is then Upload'd to the ring
            //   along with field metadata. The ring publish happens
            //   per field, not per frame; the YADIF render path uses
            //   the FieldHint metadata to drive its history rotation
            //   on F0 arrivals only.
            uint8_t* captureTarget;
            ULWord   captureSize;
            if (m_fieldModeActive)
            {
                captureTarget = m_transferBuffer.data();
                ensureTransferBufferLocked(card);
                // One field = exactly half the woven-frame byte count.
                // The buffer is sized for a full frame so half always
                // fits; the second half is unused this transfer.
                captureSize   = static_cast<ULWord>(m_frameBytes / 2);
            }
            else
            {
                captureTarget = persistentDMA
                    ? static_cast<uint8_t*>(m_textureRing->WriterPersistentPtrForWrite())
                    : m_transferBuffer.data();
                if (captureTarget == nullptr)  // safety; should never trigger
                    captureTarget = m_transferBuffer.data();
                if (captureTarget == m_transferBuffer.data())
                    ensureTransferBufferLocked(card);
                captureSize   = static_cast<ULWord>(m_frameBytes);
            }
            xfer.SetVideoBuffer(reinterpret_cast<ULWord*>(captureTarget),
                                captureSize);

            const auto producedAt = clock::now();
            if (!card->AutoCirculateTransfer(fsChannel, xfer))
                continue;

            // v0.40: never publish the slot the hardware is still
            // writing. After a driver drop (ring overflow, or field
            // mode's "do not repeat the same field" rule) the driver
            // keeps recording into the active slot and marks it valid,
            // so a level >= 1 transfer (or a drain) can pick it. Its
            // content is mostly an older frame with the new one being
            // written over it. Skip it: no scatter, no sequence tick, no
            // publish, no drain. The driver marks it valid again when it
            // completes, and the next wake transfers it whole.
            if (xfer.acTransferStatus.acTransferFrame ==
                static_cast<LWord>(xfer.acTransferStatus.acFrameStamp.acCurrentFrame))
            {
                m_activeSlotSkips.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // First-frame diagnostic dump. After every successful
            // TryConfigureChannel, the per-session arm flag is set;
            // we consume it the first time a transfer succeeds, dump
            // the first 16 bytes of the host-side DMA destination in
            // hex, and disarm the flag so subsequent transfers are
            // not logged. Lets us see whether the device delivered
            // actual pixel data or all zeros, without instrumenting
            // the GL pipeline. Cheap (snprintf + LogToHost) so the
            // overhead on the live capture thread is negligible.
            // We read from captureTarget (the actual DMA destination
            // for this iteration) so the dump works in both the
            // persistent-mapped and fallback paths.
            if (m_armFirstFrameDump.exchange(false,
                                              std::memory_order_acq_rel)
                && captureTarget != nullptr && m_frameBytes > 0)
            {
                const std::size_t bytesToLog =
                    std::min<std::size_t>(16, m_frameBytes);
                char line[256];
                int written = std::snprintf(line, sizeof(line),
                    "AJAInput: first-frame DMA bytes (UYVY):");
                for (std::size_t i = 0; i < bytesToLog &&
                                          written < int(sizeof(line)) - 4;
                     ++i)
                {
                    written += std::snprintf(line + written,
                                              sizeof(line) - written,
                                              " %02x",
                                              static_cast<unsigned>(
                                                  captureTarget[i]));
                }
                FFGLLog::LogToHost(line);
            }

            // Pull the captured frame's embedded timecode out of the
            // transfer status, if RP188 was enabled at init time and
            // the input actually carries valid timecode. Some sources
            // (test pattern generators, switchers in certain modes,
            // SD-SDI from very old gear) do not embed RP188; in that
            // case GetInputTimeCode returns false and we leave the
            // last-known value in place. We decode the raw register
            // bits into BCD H/M/S/F here on the capture thread (the
            // SDK helper does the heavy lifting) and pack them into a
            // single 32-bit atomic so the render thread sees a fully
            // consistent timecode tuple even mid-publish.
            if (isSDI)
            {
                const NTV2TCIndex tcIndex =
                    (physBNCChannel == NTV2_CHANNEL2) ? NTV2_TCINDEX_SDI2 :
                    (physBNCChannel == NTV2_CHANNEL3) ? NTV2_TCINDEX_SDI3 :
                    (physBNCChannel == NTV2_CHANNEL4) ? NTV2_TCINDEX_SDI4 :
                                                       NTV2_TCINDEX_SDI1;
                NTV2_RP188 rp188;
                if (xfer.acTransferStatus.acFrameStamp
                        .GetInputTimeCode(rp188, tcIndex)
                    && rp188.IsValid())
                {
                    CRP188 tc(rp188);
                    ULWord h = 0, m = 0, s = 0, f = 0;
                    tc.GetRP188Hrs(h);
                    tc.GetRP188Mins(m);
                    tc.GetRP188Secs(s);
                    tc.GetRP188Frms(f);
                    const uint32_t packed =
                        (uint32_t(h & 0xff) << 24) |
                        (uint32_t(m & 0xff) << 16) |
                        (uint32_t(s & 0xff) << 8)  |
                        (uint32_t(f & 0xff));
                    m_lastTimecodeHMSF.store(packed,
                                             std::memory_order_relaxed);
                    m_lastTimecodeValid.store(true,
                                              std::memory_order_relaxed);
                }
            }

            if (m_textureRing)
            {
                if (m_fieldModeActive)
                {
                    // FIELD MODE upload (v0.32+).
                    //
                    // The AC transfer just filled the first
                    // (m_frameBytes/2) bytes of m_transferBuffer with
                    // ONE field's contiguous row data. AC reports
                    // which field via acPeerToPeerFlags:
                    //   AUTOCIRCULATE_FRAME_FIELD0 set  ⇒ first-in-time
                    //                                     (F0)
                    //   bit clear                       ⇒ second-in-time
                    //                                     (F1)
                    //
                    // F0 always begins a new source frame; F1 always
                    // continues the same source frame. The render-side
                    // YADIF history rotates when the source-frame-seq
                    // advances (gate is "did the source frame change"
                    // rather than "was this a frame-start publish";
                    // robust to lost F0 publishes).
                    //
                    // The row-offset map to top/bottom field rows
                    // depends on field order (TFF vs BFF):
                    //   TFF source (HD 1080i, PAL 576i): F0=top, F1=bottom
                    //   BFF source (NTSC 480i):          F0=bottom, F1=top
                    //
                    // Scatter: source contiguous rows[0..H/2) →
                    // destination rows[0, 2, 4, ...] (top) or
                    // [1, 3, 5, ...] (bottom). Implemented as a per-
                    // row memcpy with output stride = 2 * linePitch.
                    // ~2 MB per call at 1080 UYVY, ~120 MB/s at 60 Hz
                    // — well below any sensible memory-bandwidth
                    // budget on modern hardware.
                    const bool isF0 =
                        (xfer.acPeerToPeerFlags &
                         AUTOCIRCULATE_FRAME_FIELD0) != 0u;

                    // Diagnostic (v0.40): field mode expects the driver
                    // to tag every transfer FIELD0 or FIELD1. Anything
                    // else (e.g. FRAME_FULL) would be scattered as an F1
                    // and break the YADIF history; say so once per
                    // session so a log shows it.
                    if (!m_fieldFlagAnomalyLogged &&
                        (xfer.acPeerToPeerFlags &
                         (AUTOCIRCULATE_FRAME_FIELD0 |
                          AUTOCIRCULATE_FRAME_FIELD1)) == 0u)
                    {
                        m_fieldFlagAnomalyLogged = true;
                        char fl[160];
                        std::snprintf(fl, sizeof(fl),
                            "AJAInput: field mode transfer without a "
                            "field flag (acPeerToPeerFlags=0x%08X); "
                            "treated as F1",
                            static_cast<unsigned>(xfer.acPeerToPeerFlags));
                        FFGLLog::LogToHost(fl);
                    }
                    const bool isTopField = isF0
                        ? m_tffSource         // F0 is top in TFF, bottom in BFF
                        : !m_tffSource;       // F1 is the other one

                    // Tick the per-source-frame sequence: F0 starts a
                    // new source frame, F1 continues the same one.
                    // The first F1 after a session start (if AC began
                    // mid-frame and delivers F1 before any F0) sees
                    // m_captureSourceFrameSeq still at 0; publishing
                    // 0 leaves the renderer's last-rotated tracker
                    // at 0 too (no rotation, correct: we don't have
                    // a complete source frame yet).
                    if (isF0)
                        ++m_captureSourceFrameSeq;

                    const std::size_t halfBytes = m_frameBytes / 2;
                    const std::size_t numRows   = m_height / 2;
                    const std::size_t rowBytes  = halfBytes / (numRows ? numRows : 1);
                    const std::size_t rowStride = rowBytes;  // source is packed
                    const std::size_t dstStride = rowBytes * 2;  // every other line
                    const uint8_t*    src       = m_transferBuffer.data();
                    uint8_t*          dst       = m_fieldAccumBuffer.data()
                                                  + (isTopField ? 0 : rowBytes);
                    if (numRows > 0 && rowBytes > 0 &&
                        m_fieldAccumBuffer.size() >= m_frameBytes)
                    {
                        for (std::size_t r = 0; r < numRows; ++r)
                        {
                            std::memcpy(dst, src, rowBytes);
                            src += rowStride;
                            dst += dstStride;
                        }
                    }

                    const SharedTextureRing::FieldHint hint = isTopField
                        ? SharedTextureRing::FieldHint::FreshTop
                        : SharedTextureRing::FieldHint::FreshBottom;
                    m_textureRing->Upload(m_fieldAccumBuffer.data(),
                                          producedAt,
                                          hint,
                                          /*isFirstFieldOfFrame*/isF0,
                                          m_captureSourceFrameSeq);
                }
                else
                {
                    // FRAME MODE upload: the v0.31 path, unchanged
                    // semantically. We do pass the source-frame-seq
                    // so the renderer's rotation logic uses the same
                    // gate in both modes (sequence-based). Tick it
                    // here per frame.
                    ++m_captureSourceFrameSeq;
                    m_textureRing->Upload(captureTarget, producedAt,
                                          SharedTextureRing::FieldHint::FullFrame,
                                          /*isFirstFieldOfFrame*/false,
                                          m_captureSourceFrameSeq);
                }
            }
            else
            {
                m_queue.WriteFrame(captureTarget, producedAt);
            }

            // Count source FRAMES, not fields. In frame mode every
            // iteration is a frame; in field mode every other
            // iteration completes a frame (we tick on F0, which is
            // the first-arrived field — conventionally treating that
            // as the "frame-start" event). This keeps FramesCaptured()
            // semantically aligned with "captured source frames" so
            // downstream consumers (render fps stats, AJAInputPlugin's
            // field-tick reset) read a number that matches user
            // intuition. The field-rate work is observable through
            // SharedTextureRing::UploadedCount() instead.
            if (!m_fieldModeActive ||
                (xfer.acPeerToPeerFlags & AUTOCIRCULATE_FRAME_FIELD0))
            {
                m_framesCaptured.fetch_add(1, std::memory_order_relaxed);
            }

            SampleSDIStatisticsIfDue(card, isSDI, canSdiChecks,
                                     static_cast<int>(inputSource),
                                     framesSinceSdiSample);
            // v0.42: the HDR transfer the source signals (Colorimetry Auto).
            if (++m_framesSinceTransferSample >= 60)
            {
                m_framesSinceTransferSample = 0;
                SampleSignalledTransfer(card, m_activeInputNumber);
            }

            // Periodic readback: re-read the FrameStore configuration
            // and the live SDI receiver state every ~120 transfers
            // (~2 s at 60 Hz, ~4 s at 30 Hz). If another application
            // sharing the card has silently flipped a setting under
            // us (Resolume's built-in AJA integration acquires the
            // card at host startup, even when the user has not
            // configured AJA as an output, and tools like vMix do the
            // same — they each set kVRegApplicationCode/PID and may
            // continue to touch the card afterwards), the diag-
            // readback line will show the drift between what we
            // wrote and what is currently in effect.
            if (++m_periodicReadbackTick >= 120)
            {
                m_periodicReadbackTick = 0;
                EmitChannelReadback(card, "diag-readback");
            }

            // v0.40: more frames waiting in the driver ring? Transfer
            // them now instead of after the next interrupt (see
            // acDrainPending). The ring is latest-wins, so the renderer
            // simply picks up the newest one.
            const int drainCap = m_fieldModeActive
                ? m_activeRingFrameCount * 2
                : std::max(m_activeRingFrameCount, 3);
            if (xfer.acTransferStatus.acBufferLevel >= 1 &&
                acDrainCount < drainCap)
            {
                acDrainPending = true;
                ++acDrainCount;
                m_backlogDrains.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }

        case State::NoSignal:
        {
            // Don't hammer the driver. Sleep, then probe periodically.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!m_running.load(std::memory_order_acquire))
                break;

            const auto now = clock::now();
            if (now - lastNoSignalRetry < std::chrono::milliseconds(500))
                break;
            lastNoSignalRetry = now;

            CNTV2Card* card = m_device.Card();
            if (!card)
            {
                m_state.store(State::Failed, std::memory_order_release);
                break;
            }

            const int requested =
                m_targetInputNumber.load(std::memory_order_acquire);

            NTV2Channel     physBNCChannel = NTV2_CHANNEL1;
            NTV2InputSource inputSource    = NTV2_INPUTSOURCE_SDI1;
            bool            isSDI          = true;
            if (!ResolveInputSelection(requested,
                                       physBNCChannel, inputSource, isSDI))
            {
                m_state.store(State::Failed, std::memory_order_release);
                break;
            }
            // v0.40: a bidirectional connector in transmit mode never
            // shows a format here; let TryConfigureChannel decide
            // whether to switch it to receive or report whose output it
            // is.
            if (isSDI && ::NTV2DeviceHasBiDirectionalSDI(card->GetDeviceID()))
            {
                // v0.42: any connector of a quad-link input.
                bool transmitting = false;
                for (int k = 0; k < input::ConnectorCount(requested) && !transmitting; ++k)
                {
                    bool t = false;
                    if (card->GetSDITransmitEnable(
                            static_cast<NTV2Channel>(physBNCChannel + k), t) && t)
                        transmitting = true;
                }
                if (transmitting)
                {
                    m_state.store(State::Initializing, std::memory_order_release);
                    break;
                }
            }
            // We probe the physical SDI input directly; we don't need
            // routing or FrameStore state here, just "is the cable
            // carrying something we know how to capture yet?". Three
            // outcomes mirror the Running-state idle wakeup:
            //   (a) no lock or undecodable → stay in NoSignal.
            //   (b) lock + decoded + raster matches → recover.
            //   (c) lock + decoded + raster DIFFERS → publish a
            //       pending raster-change; the render thread will
            //       rebuild this capture instance at the new size.
            (void)inputSource;
            const NTV2VideoFormat fmt = static_cast<NTV2VideoFormat>(
                ProbeLiveFormat(card, requested));
            m_lastVideoFormatRaw.store(static_cast<int>(fmt),
                                       std::memory_order_relaxed);
            bool usable = false;
            if (fmt != NTV2_FORMAT_UNKNOWN)
            {
                CapturedFormat decoded;
                if (TryDecodeNTV2Format(static_cast<int>(fmt), decoded))
                {
                    if (decoded.width  == m_width &&
                        decoded.height == m_height)
                    {
                        usable = true;
                    }
                    else
                    {
                        m_pendingPacked.store(
                            (uint32_t(decoded.width)  << 16) |
                             uint32_t(decoded.height & 0xFFFFu),
                            std::memory_order_relaxed);
                        m_pendingRawFormat.store(decoded.ntv2FormatRaw,
                                                 std::memory_order_relaxed);
                        m_pendingFormatChange.store(
                            true, std::memory_order_release);
                    }
                }
            }
            if (usable)
            {
                // Real signal at the expected raster. Go back through
                // Initializing so AutoCirculate gets fully reconfigured.
                m_recoveryCount.fetch_add(1, std::memory_order_relaxed);
                m_state.store(State::Initializing,
                              std::memory_order_release);
            }
            break;
        }

        case State::Failed:
            // Terminal: idle the thread until Stop().
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            break;
        }
    }

    if (m_activeInputNumber != 0)
        TeardownChannel();

    // Release the page-locked DMA mapping created at the top of this
    // function. Done after TeardownChannel so the driver has finished
    // its last DMA operation before we tell it the memory may go away.
    // Safe to call even if the matching DMABufferLock failed; the
    // driver tolerates unlock-of-not-locked. We re-fetch the card
    // pointer because Stop() may have happened between top and now.
    // Release the page-locked DMA mapping(s) created at the top of
    // this function. Done after TeardownChannel so the driver has
    // finished its last DMA operation before we tell it the memory
    // may go away. Safe to call even if the matching DMABufferLock
    // failed; the driver tolerates unlock-of-not-locked. We re-fetch
    // the card pointer because Stop() may have happened between top
    // and now.
    //
    // Persistent mapping locked the PBO slot pointers; m_transferBuffer
    // is locked whenever it was used as a DMA target (fallback path,
    // field mode, safety fallbacks). Both can be locked at once.
    if (CNTV2Card* unlockCard = m_device.Card())
    {
        if (xferLockOk)
            unlockCard->DMABufferUnlock(xferLockPtr, xferLockBytes);

        if (persistentDMA)
        {
            for (int i = 0;
                 i < ajainput::SharedTextureRing::SlotCount();
                 ++i)
            {
                if (!didLockSlots[i])
                    continue;
                void* ptr = m_textureRing
                    ? m_textureRing->PersistentPtrForSlot(i)
                    : nullptr;
                if (ptr != nullptr)
                    unlockCard->DMABufferUnlock(
                        reinterpret_cast<ULWord*>(ptr),
                        static_cast<ULWord>(m_frameBytes));
            }
        }
    }

    // Release the secondary context from this thread before it exits,
    // so the SharedGLContext owner can safely tear it down later from
    // wherever the destructor runs. Mirrors FrameProducer's cleanup.
    if (sharedContextActive)
        m_sharedContext->DeactivateOnCurrentThread();

    // This capture no longer occupies its FrameStore (v0.40). After a
    // yield the FrameStore may already be another clip's: leave it.
    if (!m_yielded.load(std::memory_order_acquire))
        MultiFormatRegistry::Withdraw(m_device.GetStatus().deviceIndex,
                                      m_frameStoreZeroBased);

    RevertThreadScheduling();
}

void AJACaptureSource::RevertThreadScheduling()
{
#ifdef _WIN32
    // MMCSS registrations are per thread and must be reverted by the
    // thread that made them, before it exits.
    if (m_mmcssTask != nullptr && m_mmcssModule != nullptr)
    {
        using AvRevertFn = BOOL (WINAPI*)(HANDLE);
        auto avRevert = reinterpret_cast<AvRevertFn>(
            ::GetProcAddress(static_cast<HMODULE>(m_mmcssModule),
                             "AvRevertMmThreadCharacteristics"));
        if (avRevert)
            avRevert(static_cast<HANDLE>(m_mmcssTask));
    }
    if (m_mmcssModule != nullptr)
        ::FreeLibrary(static_cast<HMODULE>(m_mmcssModule));
    m_mmcssTask   = nullptr;
    m_mmcssModule = nullptr;
#endif
}

} // namespace ajainput
