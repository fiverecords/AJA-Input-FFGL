// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    SharedCapture.cpp
 * @brief   See SharedCapture.h.
 */

// windows.h first: FFGL.h (via SharedTextureRing.h) defines NOxxx macros
// that break the Windows headers the AJA SDK pulls in if they come later
// (same as AJACaptureSource.cpp).
#ifdef _WIN32
#  include <windows.h>
#endif

#include "SharedCapture.h"

#include "CaptureTopology.h"
#include "FrameStorePool.h"
#include "InputId.h"
#include "MultiFormatRegistry.h"

#include <cstdio>
#include <map>
#include <string>
#include <tuple>

namespace ajainput
{

namespace
{

using Clock = std::chrono::steady_clock;

int64_t NowMs(Clock::time_point t) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        t.time_since_epoch()).count();
}

const char* InputLabel(int inputNumber) noexcept
{
    return input::LabelUpper(inputNumber);
}

std::string FormatName(int raw)
{
    CapturedFormat f;
    if (raw != 0 && TryDecodeNTV2Format(raw, f) && !f.name.empty())
        return f.name;
    return "?";
}

// ----- registry ---------------------------------------------------------

struct Key
{
    uint32_t device;
    int      input;
    void*    context;
    bool operator<(const Key& o) const noexcept
    {
        return std::tie(device, input, context) <
               std::tie(o.device, o.input, o.context);
    }
};

std::mutex& RegistryMutex()
{
    static std::mutex m;
    return m;
}

std::map<Key, std::weak_ptr<SharedCapture>>& Registry()
{
    static std::map<Key, std::weak_ptr<SharedCapture>> r;
    return r;
}

constexpr float kSyntheticFps = 60.0f;

// Open card handles of destroyed captures, by device index, for the next
// capture of that card (render thread; mutex for safety).
std::mutex& SpareMutex()
{
    static std::mutex m;
    return m;
}

std::map<uint32_t, std::unique_ptr<AJADevice>>& Spares()
{
    static std::map<uint32_t, std::unique_ptr<AJADevice>> s;
    return s;
}

std::unique_ptr<AJADevice> TakeSpareOrNew(uint32_t deviceIndex, bool& outReused)
{
    std::lock_guard<std::mutex> lock(SpareMutex());
    auto& spares = Spares();
    const auto it = spares.find(deviceIndex);
    if (it != spares.end() && it->second && it->second->IsOpen())
    {
        std::unique_ptr<AJADevice> dev = std::move(it->second);
        spares.erase(it);
        outReused = true;
        return dev;
    }
    outReused = false;
    return std::make_unique<AJADevice>();
}

void ParkSpare(uint32_t deviceIndex, std::unique_ptr<AJADevice> dev)
{
    if (!dev || !dev->IsOpen())
        return;
    std::unique_ptr<AJADevice> displaced;
    {
        std::lock_guard<std::mutex> lock(SpareMutex());
        auto& slot = Spares()[deviceIndex];
        if (slot)
            displaced = std::move(dev);      // one spare per card is enough
        else
            slot = std::move(dev);
    }
    // displaced closes here, outside the lock.
}

} // namespace

void SharedCapture::CloseSpareDevices()
{
    std::map<uint32_t, std::unique_ptr<AJADevice>> closing;
    {
        std::lock_guard<std::mutex> lock(SpareMutex());
        closing.swap(Spares());
    }
    // Each AJADevice closes in its destructor (the last one of a card
    // hands the stream claim back and restores the task mode).
}

// =============================================================================
// Attach / lifetime
// =============================================================================

std::shared_ptr<SharedCapture> SharedCapture::Attach(uint32_t               deviceIndex,
                                                     int                    inputNumber,
                                                     const CaptureSettings& settingsIfNew,
                                                     bool&                  outCreated)
{
    outCreated = false;
    const Key key{ deviceIndex, inputNumber, SharedGLContext::CurrentHostContext() };

    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto& reg = Registry();
    for (auto it = reg.begin(); it != reg.end();)
    {
        if (it->second.expired())
            it = reg.erase(it);
        else
            ++it;
    }
    const auto found = reg.find(key);
    if (found != reg.end())
    {
        if (auto existing = found->second.lock())
            return existing;
    }
    else
    {
        // Same input in another GL context: say so, since those clips
        // cannot share textures and each gets its own capture.
        for (const auto& kv : reg)
            if (kv.first.device == deviceIndex && kv.first.input == inputNumber)
            {
                char line[200];
                std::snprintf(line, sizeof(line),
                              "AJAInput: %s is already captured for another "
                              "GL context; this clip gets a capture of its own",
                              InputLabel(inputNumber));
                FFGLLog::LogToHost(line);
                break;
            }
    }

    std::shared_ptr<SharedCapture> created(
        new SharedCapture(deviceIndex, inputNumber, settingsIfNew));
    reg[key] = created;
    outCreated = true;
    created->StartSource();
    return created;
}

SharedCapture::SharedCapture(uint32_t deviceIndex, int inputNumber,
                             const CaptureSettings& settings)
    : m_deviceIndex(deviceIndex),
      m_inputNumber(inputNumber),
      m_targetRingFrameCount(settings.ringFrameCount),
      m_targetLowLatency(settings.lowLatency),
      m_targetTenBit(settings.tenBit),
      m_reusedDevice(false),
      m_devicePtr(TakeSpareOrNew(deviceIndex, m_reusedDevice)),
      m_device(*m_devicePtr)
{
}

SharedCapture::~SharedCapture()
{
    // Writers first (they target the ring and the queue), then the GL
    // objects (render thread, host context current), then the card.
    StopCapture();
    ReleaseFrameStore();
    StopSyntheticProducer();
    m_ring.Release();
    m_uploader.Release();
    m_queue.Release();
    m_sharedContext.Release();
    // Leave the card open for the next capture of it (CloseSpareDevices).
    ParkSpare(m_deviceIndex, std::move(m_devicePtr));

    char line[160];
    std::snprintf(line, sizeof(line),
                  "AJAInput: %s capture closed (no clip uses it any more)",
                  InputLabel(m_inputNumber));
    FFGLLog::LogToHost(line);
}

// =============================================================================
// Setup
// =============================================================================

void SharedCapture::OpenDeviceAndProbe()
{
    bool decodedOk = false;
    const bool opened = m_reusedDevice ? m_device.IsOpen()
                                       : m_device.Open(m_deviceIndex);
    if (opened && m_reusedDevice)
    {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "AJAInput: %s reuses the open card handle (no reopen)",
                      InputLabel(m_inputNumber));
        FFGLLog::LogToHost(line);
    }
    if (opened && !m_reusedDevice)
    {
        const auto& s = m_device.GetStatus();
        char line[256];
        std::snprintf(line, sizeof(line),
                      "AJAInput: AJA device opened for %s, idx=%u model=%s "
                      "serial=%s driver=%s firmware=%s inputs=%u outputs=%u "
                      "framestores=%u",
                      InputLabel(m_inputNumber),
                      s.deviceIndex,
                      s.modelName.empty()    ? "?" : s.modelName.c_str(),
                      s.serialNumber.empty() ? "?" : s.serialNumber.c_str(),
                      s.driverVersion.empty()? "?" : s.driverVersion.c_str(),
                      s.deviceVersion.empty()? "?" : s.deviceVersion.c_str(),
                      static_cast<unsigned>(s.numVideoInputs),
                      static_cast<unsigned>(s.numVideoOutputs),
                      static_cast<unsigned>(s.numFrameStores));
        FFGLLog::LogToHost(line);

        // What the driver sees on every physical input (read-only, v0.40:
        // a connector another application uses as an output is reported,
        // not switched), and which FrameStores the crosspoint ROM lets
        // each reach (v0.41).
        // v0.42: every input the card has (SDI 1..8, HDMI 1..4); a
        // quad-link input is its SDI links, surveyed one by one.
        std::string routes = "AJAInput: routing ROM:";
        for (int k = 0; k < input::kMaxInputNumber; ++k)
        {
            const int inputNumber = input::kMenuOrder[k];
            if (input::IsQuad(inputNumber) || (s.inputMask & (1u << inputNumber)) == 0u)
                continue;
            const int raw = m_device.ProbeInputFormat(inputNumber, /*switchToReceive*/false);
            CapturedFormat decoded;
            const bool is3Gb = input::IsSdi(inputNumber) && m_device.ProbeIs3GbInput(inputNumber);
            if (raw == AJADevice::kProbeIsOutput)
                std::snprintf(line, sizeof(line),
                              "AJAInput: %s: connector is an SDI output (not probed)",
                              InputLabel(inputNumber));
            else if (raw == 0)
                std::snprintf(line, sizeof(line), "AJAInput: %s: no signal",
                              InputLabel(inputNumber));
            else if (TryDecodeNTV2Format(raw, decoded))
                std::snprintf(line, sizeof(line),
                              "AJAInput: %s: %s (%zux%zu @ %.3f Hz)%s",
                              InputLabel(inputNumber), decoded.name.c_str(),
                              decoded.width, decoded.height,
                              decoded.FramesPerSecond(),
                              is3Gb ? " [3G Level B on cable, auto-converted to A]" : "");
            else
                std::snprintf(line, sizeof(line),
                              "AJAInput: %s: locked but format unsupported "
                              "(NTV2VideoFormat=%d)",
                              InputLabel(inputNumber), raw);
            FFGLLog::LogToHost(line);

            // 1080p30 over 3G Level B: almost certainly 1080p60 dual-
            // stream HD mis-demuxed (1080p30 fits 1.5G).
            if (raw > 0 && is3Gb && decoded.width == 1920 && decoded.height == 1080 &&
                !decoded.isInterlaced && decoded.fpsNumerator == 30 &&
                decoded.fpsDenominator == 1)
            {
                std::snprintf(line, sizeof(line),
                    "AJAInput: WARN %s reports 1080p30 over 3G Level B; "
                    "cable likely carries 1080p60 dual-stream HD mis-demuxed. "
                    "Try Level A on the source.", InputLabel(inputNumber));
                FFGLLog::LogToHost(line);
            }

            routes += " ";
            routes += InputLabel(inputNumber);
            routes += " ->";
            bool any = false;
            for (int fs = 0; fs < static_cast<int>(s.numFrameStores) && fs < 8; ++fs)
            {
                const int can = m_device.CanRouteInputToFrameStore(inputNumber, fs);
                if (can < 0) { routes += " ?"; any = true; break; }
                if (can > 0) { routes += " FS" + std::to_string(fs + 1); any = true; }
            }
            if (!any)
                routes += " -";
            routes += ";";
        }
        FFGLLog::LogToHost(routes.c_str());
    }
    if (opened)
    {
        char line[256];
        // The input this capture is for: switching its connector to
        // receive is allowed (unless its SDI output carries another
        // application's playout).
        int raw = m_device.ProbeInputFormat(m_inputNumber, /*switchToReceive*/true);
        if (raw == AJADevice::kProbeIsOutput)
        {
            std::snprintf(line, sizeof(line),
                          "AJAInput: %s is an SDI output carrying another "
                          "application's playout; not switching it to input",
                          InputLabel(m_inputNumber));
            FFGLLog::LogToHost(line);
            raw = 0;
        }
        CapturedFormat probed;
        if (TryDecodeNTV2Format(raw, probed))
        {
            m_format  = probed;
            decodedOk = true;
            std::snprintf(line, sizeof(line),
                          "AJAInput: %s selected for capture; sized for %s",
                          InputLabel(m_inputNumber), m_format.name.c_str());
        }
        else
        {
            std::snprintf(line, sizeof(line),
                          "AJAInput: %s selected but no usable signal yet; "
                          "sized for the 720p60 default until it arrives",
                          InputLabel(m_inputNumber));
        }
        FFGLLog::LogToHost(line);
    }
    else
    {
        char line[200];
        std::snprintf(line, sizeof(line),
                      "AJAInput: AJA device not available (%s); running "
                      "synthetic generator for %s",
                      ToString(m_device.GetStatus().openStatus),
                      InputLabel(m_inputNumber));
        FFGLLog::LogToHost(line);
    }
    if (!decodedOk)
        m_format = SyntheticFallbackFormat();
}

PixelKind SharedCapture::WantedKind() const noexcept
{
    // v210 only from the card, only when asked for and supported; the
    // synthetic generator emits UYVY.
    const bool want = m_targetTenBit.load(std::memory_order_acquire);
    return (want && m_device.IsOpen() && m_device.GetStatus().canDoFBF10Bit)
               ? PixelKind::V210 : PixelKind::UYVY;
}

void SharedCapture::StartSource()
{
    OpenDeviceAndProbe();

    const PixelKind kind = WantedKind();
    m_allocatedKind = kind;
    if (!m_uploader.Initialise(m_format.width, m_format.height, kind))
        FFGLLog::LogToHost("AJAInput: texture uploader initialisation failed");

    // Off-thread upload: a secondary GL context sharing with the host's,
    // so the capture thread uploads straight into GL textures. Falls back
    // to the byte queue + render-thread upload when either step fails.
    m_useOffThreadUpload = false;
    if (m_sharedContext.Initialise())
    {
        if (m_ring.Initialise(m_format.width, m_format.height, kind))
        {
            m_useOffThreadUpload = true;
            FFGLLog::LogToHost(m_ring.HasPersistentMapping()
                ? "AJAInput: off-thread upload path active, zerocopy=on "
                  "(AJA DMA -> persistent-mapped PBO)"
                : "AJAInput: off-thread upload path active, zerocopy=off "
                  "(driver lacks GL_ARB_buffer_storage; streaming path)");
        }
        else
        {
            FFGLLog::LogToHost("AJAInput: shared texture ring allocation "
                               "failed; falling back to render-thread upload");
        }
    }
    if (!m_useOffThreadUpload)
        FFGLLog::LogToHost("AJAInput: using render-thread upload path");

    // Always initialised: the producers' backpressure code references it.
    m_queue.Initialise(FrameSize(kind, m_format.width, m_format.height));

    m_appliedSettingsGeneration = m_settingsGeneration.load(std::memory_order_acquire);
    if (m_device.IsOpen())
    {
        if (StartCapture() == StartResult::NoFrameStore)
        {
            char line[200];
            std::snprintf(line, sizeof(line),
                          "AJAInput: no AJA FrameStore free for %s (card has "
                          "%u, all used by other inputs); waiting for one",
                          InputLabel(m_inputNumber),
                          static_cast<unsigned>(m_device.GetStatus().numFrameStores));
            FFGLLog::LogToHost(line);
            m_waitingForFrameStore.store(true, std::memory_order_release);
            m_nextStartAttempt = {};
        }
    }
    else
    {
        StartSyntheticProducer();
    }
}

void SharedCapture::ReallocateSinks(const CapturedFormat& fmt, PixelKind kind)
{
    // Every writer is stopped by the caller. Initialise() releases and
    // re-creates at the new size.
    if (!m_uploader.Initialise(fmt.width, fmt.height, kind))
        FFGLLog::LogToHost("AJAInput: uploader re-init failed during "
                           "pipeline re-size");
    if (m_useOffThreadUpload &&
        !m_ring.Initialise(fmt.width, fmt.height, kind))
    {
        FFGLLog::LogToHost("AJAInput: texture ring re-init failed during "
                           "pipeline re-size; falling back to render-thread "
                           "upload");
        m_useOffThreadUpload = false;
    }
    m_queue.Initialise(FrameSize(kind, fmt.width, fmt.height));
    m_allocatedKind     = kind;
    m_lastQueueSequence = ~0ull;
}

void SharedCapture::Reconfigure(const CapturedFormat& fmt)
{
    StopCapture();
    const bool synthetic = (m_producer != nullptr);
    StopSyntheticProducer();

    ReallocateSinks(fmt, WantedKind());
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_format = fmt;
    }
    ++m_formatGeneration;

    if (m_device.IsOpen())
    {
        // v0.42: a 4K picture on a card without 12G routing needs other
        // FrameStores than an HD one (and the other way round): hand the
        // ones held back and claim what the new format needs.
        if (m_frameStoreIndex >= 0)
        {
            const FrameStorePlan plan = CurrentPlan();
            const uint32_t wanted = (plan.ok && plan.fixedMask != 0u)
                ? plan.fixedMask : 0u;
            const bool holdingSet = FrameStoreCount(m_frameStoreMask) > 1;
            if ((wanted != 0u && wanted != m_frameStoreMask) ||
                (wanted == 0u && holdingSet))
            {
                char set[16];
                FormatFrameStoreSet(m_frameStoreMask, set, sizeof(set));
                char msg[200];
                std::snprintf(msg, sizeof(msg),
                              "AJAInput: %s %s needs other FrameStores; "
                              "releasing %s",
                              InputLabel(m_inputNumber), fmt.name.c_str(), set);
                FFGLLog::LogToHost(msg);
                ReleaseFrameStore();
            }
        }
        if (m_frameStoreIndex >= 0)
            CreateAndStartCapture();
        else if (StartCapture() == StartResult::NoFrameStore)
        {
            // The capture had lent its FrameStore, never had one, or its
            // new format needs ones in use.
            m_waitingForFrameStore.store(true, std::memory_order_release);
            m_nextStartAttempt = {};
        }
    }
    else if (synthetic)
    {
        StartSyntheticProducer();
    }

    char line[200];
    std::snprintf(line, sizeof(line),
                  "AJAInput: %s pipeline re-allocated for %s (%zux%zu, %s)",
                  InputLabel(m_inputNumber), m_format.name.c_str(),
                  m_format.width, m_format.height,
                  m_allocatedKind == PixelKind::V210 ? "v210" : "uyvy");
    FFGLLog::LogToHost(line);
}

// =============================================================================
// Synthetic generator
// =============================================================================

void SharedCapture::StartSyntheticProducer()
{
    StopSyntheticProducer();
    // The producer picks its publish path (ring or queue) once at
    // construction, so it is rebuilt whenever that may have changed.
    auto producer = std::make_unique<FrameProducer>(
        m_queue, m_sharedContext,
        m_useOffThreadUpload ? &m_ring : nullptr,
        m_format.width, m_format.height, kSyntheticFps);
    // The dropdown value of the input (SDI1 = 0, SDI2 = 1, HDMI = 4)
    // picks the test pattern.
    producer->SetInputIndex(m_inputNumber - 1);
    producer->SetRingDepth(m_targetRingFrameCount.load(std::memory_order_acquire));
    m_captureStartedAt = Clock::now();
    producer->Start();
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_producer = std::move(producer);
    }

    char line[180];
    std::snprintf(line, sizeof(line),
                  "AJAInput: synthetic producer started for %s (%zux%zu, "
                  "%.1f fps, off-thread upload=%s)",
                  InputLabel(m_inputNumber), m_format.width, m_format.height,
                  kSyntheticFps, m_useOffThreadUpload ? "yes" : "no");
    FFGLLog::LogToHost(line);
}

void SharedCapture::StopSyntheticProducer()
{
    std::unique_ptr<FrameProducer> producer;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        producer = std::move(m_producer);
    }
    if (producer)
        producer->Stop();
}

// =============================================================================
// AJA capture and FrameStore
// =============================================================================

FrameStorePlan SharedCapture::CurrentPlan() const noexcept
{
    return PlanFrameStores(m_inputNumber, Is4KRaster(m_format), m_device.Caps());
}

int SharedCapture::ClaimFrameStore()
{
    // v0.41: pair the input with its natural FrameStore (SDI1 -> FS1,
    // SDI2 -> FS2), as AJA's own software and the card's default routing
    // do. Up to v0.40 the lowest free FrameStore was taken, so a lone
    // SDI2 clip, or a second clip, ran on a crossed route, and those were
    // the setups that never locked on the hardware. FrameStores the
    // card's crosspoint ROM cannot reach from this input are skipped.
    //
    // v0.40: keep clear of FrameStores another application is using.
    // Prefer one with AutoCirculate stopped; if every candidate is busy,
    // take over a capture (as before v0.40) but never a playout, which
    // would cut somebody's output (e.g. Resolume's own AJA output).
    //
    // v0.42: a 4K picture on a card without 12G routing needs a fixed
    // FrameStore pair or four (CaptureTopology.h), claimed all at once.
    // An input the plan rules out (e.g. 4K on one SDI cable of such a
    // card) still gets one FrameStore: its capture thread keeps probing
    // the cable and shows the reason, and re-plans when the format
    // changes.
    const int maxFs = static_cast<int>(m_device.GetStatus().numFrameStores);
    const uint32_t busy    = m_device.BusyFrameStores(/*playoutOnly*/false);
    const uint32_t playout = m_device.BusyFrameStores(/*playoutOnly*/true);
    const FrameStorePlan plan = CurrentPlan();
    char line[240];

    if (plan.ok && plan.fixedMask != 0u)
    {
        m_noRoutableFrameStore.store(false, std::memory_order_release);
        // Never a playout. A set is fixed, so there is no other one to
        // prefer: another application's capture on it is taken over (and
        // reported by the capture), as for a single FrameStore when every
        // one is busy.
        int fs = -1;
        if ((playout & plan.fixedMask) == 0u &&
            FrameStorePool::TryClaimMask(m_deviceIndex, plan.fixedMask))
        {
            fs = plan.firstFrameStore;
            m_frameStoreMask = plan.fixedMask;
        }
        const uint32_t ours    = FrameStorePool::ClaimedMask(m_deviceIndex);
        const uint32_t foreign = busy & ~ours;
        if (foreign != m_lastForeignLogged)
        {
            m_lastForeignLogged = foreign;
            if (foreign != 0u)
            {
                std::snprintf(line, sizeof(line),
                              "AJAInput: FrameStores in use by another application: "
                              "mask 0x%X (playout 0x%X)",
                              foreign, playout & ~ours);
                FFGLLog::LogToHost(line);
            }
        }
        if (fs >= 0)
        {
            m_lastForeignLogged = 0;
            char set[16];
            FormatFrameStoreSet(plan.fixedMask, set, sizeof(set));
            std::snprintf(line, sizeof(line),
                          "AJAInput: %s -> %s (%s 4K, experimental; card has %d)",
                          InputLabel(m_inputNumber), set,
                          ToString(plan.topology), maxFs);
            FFGLLog::LogToHost(line);
        }
        return fs;
    }

    const bool cardHasSdi = m_device.GetStatus().numVideoInputs > 0;
    int order[32] = {};
    const int n = FrameStorePool::PreferenceOrder(
        m_inputNumber, maxFs, order, 32, cardHasSdi,
        static_cast<int>(m_device.GetStatus().numCSCs));
    uint32_t unroutable = 0u;
    for (int i = 0; i < n; ++i)
        if (m_device.CanRouteInputToFrameStore(m_inputNumber, order[i]) == 0)
            unroutable |= (1u << order[i]);

    // Passes: free and idle; free and not playing out. A FrameStore the
    // ROM rules out is never taken: ApplySignalRoute validates against the
    // same ROM, so the capture would only loop on "routing failed" while
    // holding it. Without a ROM nothing is ruled out.
    // PreferenceOrder lists every FrameStore 0..n-1 once.
    const uint32_t candidates = (n >= 32) ? ~0u : ((1u << n) - 1u);
    const bool noRoute = (n > 0) && ((unroutable & candidates) == candidates);
    if (noRoute && !m_noRoutableFrameStore.load(std::memory_order_relaxed))
    {
        char msg[200];
        std::snprintf(msg, sizeof(msg),
                      "AJAInput: the card's routing ROM allows no route from "
                      "%s to any FrameStore; it cannot be captured",
                      InputLabel(m_inputNumber));
        FFGLLog::LogToHost(msg);
    }
    m_noRoutableFrameStore.store(noRoute, std::memory_order_release);
    const uint32_t avoid[2] = { busy | unroutable, playout | unroutable };
    int fs = -1;
    for (int pass = 0; pass < 2 && fs < 0; ++pass)
        for (int i = 0; i < n; ++i)
        {
            const uint32_t bit = 1u << order[i];
            if ((avoid[pass] & bit) == 0u &&
                FrameStorePool::TryClaim(m_deviceIndex, order[i]))
            {
                fs = order[i];
                break;
            }
        }
    if (fs >= 0)
        m_frameStoreMask = 1u << fs;

    const uint32_t ours    = FrameStorePool::ClaimedMask(m_deviceIndex);
    const uint32_t foreign = busy & ~ours;
    // Logged when it changes (a capture waiting for a FrameStore retries
    // every 100 ms).
    if (foreign != m_lastForeignLogged)
    {
        m_lastForeignLogged = foreign;
        if (foreign != 0u)
        {
            std::snprintf(line, sizeof(line),
                          "AJAInput: FrameStores in use by another application: "
                          "mask 0x%X (playout 0x%X)",
                          foreign, playout & ~ours);
            FFGLLog::LogToHost(line);
        }
    }
    const int natural = FrameStorePool::NaturalFrameStore(m_inputNumber, maxFs,
                                                          cardHasSdi);
    if (fs >= 0)
    {
        m_lastForeignLogged = 0;
        std::snprintf(line, sizeof(line),
                      "AJAInput: %s -> FrameStore %d (card has %d)%s%s",
                      InputLabel(m_inputNumber), fs + 1, maxFs,
                      (natural < 0)   ? "" :
                      (fs == natural) ? ", natural pairing"
                                      : ", crossed route: its own FrameStore is taken",
                      (unroutable != 0u) ? ", routing ROM excludes some FrameStores" : "");
        FFGLLog::LogToHost(line);
    }
    return fs;
}

void SharedCapture::ReleaseFrameStore()
{
    if (m_frameStoreIndex < 0)
        return;
    FrameStorePool::ReleaseMask(m_deviceIndex, m_frameStoreMask);
    m_frameStoreIndex = -1;
    m_frameStoreMask  = 0u;
}

SharedCapture::StartResult SharedCapture::StartCapture()
{
    m_frameStoreIndex = ClaimFrameStore();
    if (m_frameStoreIndex < 0)
        return StartResult::NoFrameStore;
    m_waitingForFrameStore.store(false, std::memory_order_release);
    CreateAndStartCapture();
    return StartResult::Started;
}

void SharedCapture::CreateAndStartCapture()
{
    auto capture = std::make_unique<AJACaptureSource>(
        m_device,
        m_queue,
        m_useOffThreadUpload ? &m_ring : nullptr,
        m_useOffThreadUpload ? &m_sharedContext : nullptr,
        m_format.width,
        m_format.height,
        m_frameStoreIndex,
        m_format.ntv2FormatRaw,
        m_targetRingFrameCount.load(std::memory_order_acquire),
        m_targetLowLatency.load(std::memory_order_acquire),
        // The kind the sinks are sized for, never another one: the
        // capture thread page-locks the persistent PBOs at its own frame
        // size. A pending 10-bit change re-sizes first (Update).
        m_allocatedKind,
        m_frameStoreMask);
    capture->SetInputNumber(m_inputNumber);
    const auto now = Clock::now();
    m_captureStartedAt = now;
    // Counts as shown from the start, so a fresh capture is not mistaken
    // for one whose clips left the screen (AJACaptureSource::MaybeYield).
    capture->NoteRendered(NowMs(now), MultiFormatRegistry::RenderTick());
    capture->Start();
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_capture = std::move(capture);
}

void SharedCapture::StopCapture()
{
    std::unique_ptr<AJACaptureSource> capture;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        capture = std::move(m_capture);
    }
    if (!capture)
        return;
    capture->Stop();
    // After the join: a capture that yielded has already released its
    // FrameStorePool claim (and the FrameStore may be another input's by
    // now), so only forget the index.
    if (capture->HasYielded())
    {
        m_frameStoreIndex = -1;
        m_frameStoreMask  = 0u;
    }
}

bool SharedCapture::Offline() const noexcept
{
    if (m_waitingForFrameStore.load(std::memory_order_acquire))
        return true;
    return m_capture != nullptr &&
           m_capture->GetState() != AJACaptureSource::State::Running;
}

void SharedCapture::HandleYieldWaitingAndPairing()
{
    if (!m_device.IsOpen())
        return;
    const auto     now   = Clock::now();
    const uint32_t dev   = m_device.GetStatus().deviceIndex;

    // The capture lent its FrameStore while none of its clips was on
    // screen (a clip that is wanted one): rebuild now that one is shown.
    if (m_capture && m_capture->HasYielded())
    {
        StopCapture();
        m_waitingForFrameStore.store(true, std::memory_order_release);
        m_nextStartAttempt = {};
        char line[160];
        std::snprintf(line, sizeof(line),
                      "AJAInput: %s shown again; its FrameStore was lent, "
                      "getting one back",
                      InputLabel(m_inputNumber));
        FFGLLog::LogToHost(line);
    }

    // Waiting for a FrameStore: try every 100 ms; say every time that a
    // clip on screen wants one, so a capture whose clips are not shown
    // lends its own.
    if (m_waitingForFrameStore.load(std::memory_order_acquire))
    {
        if (now < m_nextStartAttempt)
            return;
        m_nextStartAttempt = now + std::chrono::milliseconds(100);
        if (StartCapture() == StartResult::NoFrameStore)
        {
            // Ask inputs not on screen to lend theirs, unless no
            // FrameStore could take this input anyway (routing ROM):
            // then lending would only interrupt them for nothing.
            if (m_noRoutableFrameStore.load(std::memory_order_acquire))
                m_nextStartAttempt = now + std::chrono::seconds(2);
            else
            {
                // v0.42: a 4K set wants exactly its FrameStores, so only
                // captures holding one of them lend.
                const FrameStorePlan plan = CurrentPlan();
                MultiFormatRegistry::WantFrameStore(
                    dev, NowMs(now),
                    (plan.ok && plan.fixedMask != 0u) ? plan.fixedMask : ~0u);
            }
            return;
        }
        char set[16];
        FormatFrameStoreSet(m_frameStoreMask, set, sizeof(set));
        char line[160];
        std::snprintf(line, sizeof(line),
                      "AJAInput: %s capture started on %s",
                      InputLabel(m_inputNumber), set);
        FFGLLog::LogToHost(line);
        return;
    }

    if (!m_capture || m_frameStoreIndex < 0)
        return;

    // v0.41 natural pairing: SDI N belongs on FrameStore N. A capture
    // offline on a crossed route for 2 s moves there once it is free
    // (checked every 2 s: the busy check asks the driver about every
    // FrameStore). A picture on a crossed route is left alone.
    // v0.42: not for a 4K FrameStore set (fixed); HDMI N pairs with
    // FrameStore N on cards without SDI inputs (Kona HDMI).
    const int maxFs   = static_cast<int>(m_device.GetStatus().numFrameStores);
    const int natural = FrameStorePool::NaturalFrameStore(
        m_inputNumber, maxFs, m_device.GetStatus().numVideoInputs > 0);
    const bool running =
        m_capture->GetState() == AJACaptureSource::State::Running;
    if (natural < 0 || natural == m_frameStoreIndex || running ||
        FrameStoreCount(m_frameStoreMask) > 1)
    {
        m_offlineSince = {};
        return;
    }
    if (m_offlineSince == Clock::time_point{})
        m_offlineSince = now;
    if (now - m_offlineSince < std::chrono::seconds(2) || now < m_nextPairingCheck)
        return;
    m_nextPairingCheck = now + std::chrono::seconds(2);

    if ((m_device.BusyFrameStores(/*playoutOnly*/false) & (1u << natural)) != 0u)
        return;
    if (m_device.CanRouteInputToFrameStore(m_inputNumber, natural) == 0)
        return;
    if (!FrameStorePool::TryClaim(m_deviceIndex, natural))
        return;   // another input of ours has it

    const int previous = m_frameStoreIndex;
    StopCapture();
    ReleaseFrameStore();          // no-op if the capture yielded meanwhile
    m_frameStoreIndex = natural;
    m_frameStoreMask  = 1u << natural;
    m_offlineSince = {};
    CreateAndStartCapture();

    char line[200];
    std::snprintf(line, sizeof(line),
                  "AJAInput: %s moved from FrameStore %d to FrameStore %d "
                  "(its natural pair; capture was offline)",
                  InputLabel(m_inputNumber), previous + 1, natural + 1);
    FFGLLog::LogToHost(line);
}

void SharedCapture::ApplySettings()
{
    const uint64_t gen = m_settingsGeneration.load(std::memory_order_acquire);
    if (gen == m_appliedSettingsGeneration)
        return;
    m_appliedSettingsGeneration = gen;
    const int  ring = m_targetRingFrameCount.load(std::memory_order_acquire);
    const bool ll   = m_targetLowLatency.load(std::memory_order_acquire);
    if (m_capture)
    {
        // Both are no-ops when unchanged; a change re-arms AutoCirculate.
        m_capture->SetRingFrameCount(ring);
        m_capture->SetLowLatencyMode(ll);
    }
    if (m_producer)
    {
        m_producer->SetRingDepth(ring);
        m_producer->ResetStats();
    }
    // The 10-bit setting is applied by Update() through WantedKind().
}

void SharedCapture::Update()
{
    const auto now = Clock::now();

    // A clip of this input is being rendered (lending, MaybeYield).
    const uint64_t tick = MultiFormatRegistry::AdvanceRenderTick();
    if (m_capture)
        m_capture->NoteRendered(NowMs(now), tick);

    HandleYieldWaitingAndPairing();
    ApplySettings();

    // Pixel kind (10-bit setting) changed: re-size and restart.
    if (WantedKind() != m_allocatedKind)
    {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "AJAInput: %s pixel kind change %s -> %s; re-sizing "
                      "the upload pipeline",
                      InputLabel(m_inputNumber),
                      m_allocatedKind == PixelKind::V210 ? "v210" : "uyvy",
                      WantedKind() == PixelKind::V210 ? "v210" : "uyvy");
        FFGLLog::LogToHost(line);
        Reconfigure(m_format);
    }

    // The capture saw a different raster or rate on the cable: rebuild
    // the sinks at the new format (GL work, hence here).
    if (m_capture)
    {
        int newW = 0, newH = 0, newRaw = 0;
        if (m_capture->ConsumePendingFormatChange(newW, newH, newRaw))
        {
            CapturedFormat fmt;
            if (!TryDecodeNTV2Format(newRaw, fmt))
            {
                // Defensive: the capture decoded it before publishing.
                fmt = m_format;
                fmt.width         = static_cast<std::size_t>(newW);
                fmt.height        = static_cast<std::size_t>(newH);
                fmt.ntv2FormatRaw = newRaw;
            }
            Reconfigure(fmt);
        }
    }

    if (now >= m_nextRateClashCheck)
    {
        m_nextRateClashCheck = now + std::chrono::milliseconds(500);
        UpdateRateClash();
    }
}

void SharedCapture::UpdateRateClash()
{
    MultiFormatRegistry::Conflict c;
    const bool singleFormat = SingleFormatCard();
    if (m_capture && m_frameStoreIndex >= 0 && m_device.IsOpen())
        c = MultiFormatRegistry::FindConflict(m_device.GetStatus().deviceIndex,
                                              m_frameStoreIndex, singleFormat);
    const int newRaw   = c.found ? c.otherFormatRaw : 0;
    const int newInput = c.found ? c.otherInputNumber : 0;
    const int oldRaw   = m_rateClashFormatRaw.load(std::memory_order_relaxed);
    m_rateClashInput.store(newInput, std::memory_order_relaxed);
    m_rateClashFormatRaw.store(newRaw, std::memory_order_relaxed);
    if (newRaw == oldRaw)
        return;

    char line[320];
    if (newRaw != 0 && singleFormat)
    {
        // v0.42: a card without multi-format runs one format at a time.
        const int ownRaw = m_capture ? m_capture->LastVideoFormatRaw() : 0;
        std::snprintf(line, sizeof(line),
            "AJAInput: WARNING %s %s (FrameStore %d) and %s %s (FrameStore %d): "
            "this card runs one video format at a time; the input that came "
            "second waits until the formats match.",
            InputLabel(m_inputNumber), FormatName(ownRaw).c_str(),
            m_frameStoreIndex + 1, InputLabel(newInput),
            FormatName(newRaw).c_str(), c.otherFrameStore + 1);
    }
    else if (newRaw != 0)
    {
        const int ownRaw = m_capture ? m_capture->LastVideoFormatRaw() : 0;
        std::snprintf(line, sizeof(line),
            "AJAInput: WARNING frame rates not multi-format compatible: "
            "%s %s (FrameStore %d) vs %s %s (FrameStore %d) on the same card. "
            "AJA multi-format cards only run inputs of the same rate family "
            "together (29.97/59.94, 30/60, 25/50, 23.98, 24); expect stutter "
            "or dropped frames.",
            InputLabel(m_inputNumber), FormatName(ownRaw).c_str(),
            m_frameStoreIndex + 1, InputLabel(newInput),
            FormatName(newRaw).c_str(), c.otherFrameStore + 1);
    }
    else
    {
        std::snprintf(line, sizeof(line),
                      "AJAInput: %s frame-rate clash with the other input cleared",
                      InputLabel(m_inputNumber));
    }
    FFGLLog::LogToHost(line);
}

// =============================================================================
// Frames
// =============================================================================

SharedCapture::Frame SharedCapture::Acquire()
{
    Frame f;
    if (Offline())
        return f;

    if (m_useOffThreadUpload)
    {
        // Several clips may acquire in one host frame: a repeat returns the
        // same slot, a newer publish the newer one; the slot a clip read
        // stays fenced by its EndRead. Each clip judges freshness by
        // publishSequence itself (the ring's isFresh goes to the first
        // acquirer only).
        const SharedTextureRing::ReadResult res = m_ring.Acquire();
        f.needsEndRead = res.isValid;
        // A frame published before the current capture started (the ring
        // keeps the last picture across a restart) is not shown.
        if (res.isValid && res.producedAt >= m_captureStartedAt)
        {
            f.valid               = true;
            f.fromRing            = true;
            f.texture             = res.texture;
            f.fencePending        = res.fencePending;
            f.publishSequence     = res.sequence;
            f.fieldHint           = res.fieldHint;
            f.isFirstFieldOfFrame = res.isFirstFieldOfFrame;
            f.sourceFrameSequence = res.sourceFrameSequence;
            f.producedAt          = res.producedAt;
            f.publishedAt         = res.publishedAt;
        }
        return f;
    }

    // Byte queue: upload each new frame once, every clip samples the same
    // uploader texture (one GL context: later uploads are ordered after
    // the draws that read the previous one).
    const FrameQueue::ReadResult rd = m_queue.AcquireRead();
    if (rd.data != nullptr && rd.producedAt >= m_captureStartedAt)
    {
        if (rd.sequence != m_lastQueueSequence)
        {
            m_uploader.Upload(rd.data);
            m_lastQueueSequence = rd.sequence;
        }
        f.valid           = true;
        f.texture         = m_uploader.TextureId();
        f.publishSequence = rd.sequence;
        f.producedAt      = rd.producedAt;
        f.publishedAt     = rd.publishedAt;
    }
    return f;
}

void SharedCapture::EndRead()
{
    if (m_useOffThreadUpload)
        m_ring.EndRead();
}

uint64_t SharedCapture::CapturedFrameCount() const noexcept
{
    return m_capture ? m_capture->FramesCaptured() : m_ring.UploadedCount();
}

// =============================================================================
// Settings
// =============================================================================

CaptureSettings SharedCapture::Settings() const noexcept
{
    CaptureSettings s;
    s.ringFrameCount = m_targetRingFrameCount.load(std::memory_order_acquire);
    s.lowLatency     = m_targetLowLatency.load(std::memory_order_acquire);
    s.tenBit         = m_targetTenBit.load(std::memory_order_acquire);
    return s;
}

void SharedCapture::SetRingFrameCount(int ringFrameCount) noexcept
{
    if (ringFrameCount < 2) ringFrameCount = 2;
    if (ringFrameCount > 7) ringFrameCount = 7;
    if (m_targetRingFrameCount.exchange(ringFrameCount, std::memory_order_acq_rel) !=
        ringFrameCount)
        m_settingsGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void SharedCapture::SetLowLatency(bool enabled) noexcept
{
    if (m_targetLowLatency.exchange(enabled, std::memory_order_acq_rel) != enabled)
        m_settingsGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void SharedCapture::SetTenBit(bool enabled) noexcept
{
    if (m_targetTenBit.exchange(enabled, std::memory_order_acq_rel) != enabled)
        m_settingsGeneration.fetch_add(1, std::memory_order_acq_rel);
}

// =============================================================================
// Status
// =============================================================================

SharedCapture::Status SharedCapture::Snapshot() const
{
    Status st;
    st.inputNumber        = m_inputNumber;
    st.deviceOpen           = m_device.IsOpen();
    st.openStatus           = ToString(m_device.GetStatus().openStatus);
    st.waitingForFrameStore = m_waitingForFrameStore.load(std::memory_order_acquire);
    st.noRoutableFrameStore = m_noRoutableFrameStore.load(std::memory_order_acquire);
    st.rateClashFormatRaw   = m_rateClashFormatRaw.load(std::memory_order_relaxed);
    st.rateClashInput       = m_rateClashInput.load(std::memory_order_relaxed);
    st.clips                = m_clipCount.load(std::memory_order_acquire);
    st.singleFormatCard     = SingleFormatCard();

    std::lock_guard<std::mutex> lock(m_statusMutex);
    st.width  = m_format.width;
    st.height = m_format.height;
    st.fps    = m_format.FramesPerSecond();
    if (m_capture)
    {
        st.hasCapture    = true;
        st.state         = m_capture->GetState();
        st.yielded       = m_capture->HasYielded();
        st.frameStore    = m_capture->FrameStoreIndex();
        st.frameStoreMask = m_capture->FrameStoreMask();
        st.signalledTransfer = m_capture->SignalledTransfer();
        st.lastFormatRaw = m_capture->LastVideoFormatRaw();
        st.reason        = m_capture->StatusReason();
        st.foreignMask   = m_capture->RecentForeignUse();
        st.frames        = m_capture->FramesCaptured();
        st.drops         = m_capture->DriverDrops();
        st.recoveries    = m_capture->RecoveryCount();
        st.idleWakeups   = m_capture->IdleWakeups();
    }
    else if (m_producer)
    {
        st.synthetic = true;
        // Count at the sink the producer publishes to.
        st.frames = m_useOffThreadUpload ? m_ring.UploadedCount()
                                         : m_queue.ProducedCount();
        st.drops  = m_useOffThreadUpload ? m_ring.ConsumerMissedCount()
                                         : m_queue.DroppedCount();
    }
    return st;
}

} // namespace ajainput
