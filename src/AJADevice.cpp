// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    AJADevice.cpp
 * @brief   Implementation of the device wrapper. Uses CNTV2DeviceScanner to
 *          locate boards and CNTV2Card for everything per-device.
 */

#include "AJADevice.h"
#include "InputNTV2.h"
#include "VideoFormat.h"

#include "ajabase/system/process.h"
#include "ntv2card.h"
#include "ntv2devicecapabilities.h"
#include "ntv2devicescanner.h"
#include "ntv2enums.h"
#include "ntv2signalrouter.h"
#include "ntv2utils.h"
#include <FFGLSDK.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace ajainput
{

const char* ToString(AJAOpenStatus s) noexcept
{
    switch (s)
    {
        case AJAOpenStatus::NotAttempted:   return "not attempted";
        case AJAOpenStatus::Ok:             return "ok";
        case AJAOpenStatus::NoDeviceFound:  return "no device";
        case AJAOpenStatus::DeviceBusy:     return "device busy";
        case AJAOpenStatus::DriverMissing:  return "driver missing";
        case AJAOpenStatus::OpenFailed:     return "open failed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Impl carries the actual CNTV2Card. Hidden behind a PIMPL so this class's
// public header does not transitively pull libajantv2 headers (and their
// platform-specific includes such as SetupAPI on Windows) into every
// translation unit that just wants to read the Status struct.
// ---------------------------------------------------------------------------
struct AJADevice::Impl
{
    CNTV2Card     card;
    /// True while this instance holds a reference in the process-wide
    /// claim registry (see ClaimTable below). The registry, not this
    /// instance, owns the saved task mode and the stream claim, so a
    /// second instance on the same card cannot undo them early.
    bool          holdsClaimRef    = false;
    uint32_t      claimIndex       = 0;
};

// AJA application signature. The driver records this (alongside the host
// PID) when AcquireStreamForApplication succeeds, so other AJA tooling
// can see which app currently owns the device. "KONX" reads as the
// product short name.
static constexpr ULWord kAppSignature = NTV2_FOURCC('A','J','I','N');

// ---------------------------------------------------------------------------
// Process-wide claim registry (v0.39).
//
// Every plugin instance owns its own CNTV2Card, but the stream claim
// (kVRegApplicationCode/PID) and the task mode are DEVICE-global
// registers. Before v0.39 each instance saved the task mode it found,
// acquired the stream and switched to OEM tasks on Open, and undid all
// of it on Close. With two instances on one card that went wrong:
//   - the second instance saved OEM (set by the first) as "original";
//   - closing the first instance called ReleaseStreamForApplication,
//     which forces the driver's acquire refcount to zero, and restored
//     STANDARD tasks while the second instance was still capturing
//     (on Windows STANDARD also changes SetVideoFormat's behaviour).
// The registry makes the first Open per device index take the claim and
// the last Close give it back. Guarded by a mutex because Resolume may
// construct and destroy instances on different threads.
// ---------------------------------------------------------------------------
namespace
{
struct DeviceClaim
{
    int          refs          = 0;
    NTV2TaskMode savedTaskMode = NTV2_STANDARD_TASKS;
    bool         acquired      = false;
};

std::mutex& ClaimMutex()
{
    static std::mutex m;
    return m;
}

std::map<uint32_t, DeviceClaim>& ClaimTable()
{
    static std::map<uint32_t, DeviceClaim> table;
    return table;
}

// v0.42: which HDMI input widgets have the quadrant outputs (4K through
// 425 muxes) and an RGB output, per the SDK's routing tables. The older
// widgets (Io 4K, KONA LHi, Io XT, ...) only have the YCbCr output.
void HdmiWidgetOutputs(NTV2DeviceID id, int numHdmi, uint32_t& outQuadrant,
                       uint32_t& outRgb)
{
    outQuadrant = 0u;
    outRgb      = 0u;
    for (int h = 0; h < numHdmi && h < 4; ++h)
    {
        const NTV2InputSource src =
            ::NTV2ChannelToInputSource(static_cast<NTV2Channel>(h), NTV2_IOKINDS_HDMI);
        NTV2WidgetID w = NTV2_WIDGET_INVALID;
        if (CNTV2SignalRouter::GetWidgetForOutput(
                ::GetInputSourceOutputXpt(src, false, /*RGB*/false, /*quadrant*/1), w, id))
            outQuadrant |= 1u << h;
        if (CNTV2SignalRouter::GetWidgetForOutput(
                ::GetInputSourceOutputXpt(src, false, /*RGB*/true, 0), w, id))
            outRgb |= 1u << h;
    }
}

// v0.42: the capabilities CaptureTopology plans with.
CardCaps CapsFor(CNTV2Card& card)
{
    auto& f = card.features();
    CardCaps c;
    c.numFrameStores = static_cast<int>(f.GetNumFrameStores());
    c.numSdiInputs   = static_cast<int>(f.GetNumVideoInputs());
    c.numHdmiInputs  = static_cast<int>(f.GetNumHDMIVideoInputs());
    c.numCSCs        = static_cast<int>(f.GetNumCSCs());
    c.can12gRouting  = f.CanDo12gRouting();
    c.can425Mux      = f.CanDo425Mux();
    c.can4K          = f.CanDo4KVideo();
    c.multiFormat    = f.CanDoMultiFormat();
    c.isIP           = f.CanDoIP();
    HdmiWidgetOutputs(card.GetDeviceID(), c.numHdmiInputs,
                      c.hdmiQuadrantMask, c.hdmiRgbMask);
    return c;
}

// v0.42: bit n = input number n (InputId.h) exists on the card.
uint32_t InputMaskFor(CNTV2Card& card, const CardCaps& caps)
{
    uint32_t mask = 0u;
    for (int n = 1; n <= input::kMaxInputNumber; ++n)
    {
        if (input::IsQuad(n))
        {
            if (CardOffersQuadLink(input::SdiNumber(n), caps))
                mask |= 1u << n;
            continue;
        }
        NTV2InputSource src = NTV2_INPUTSOURCE_INVALID;
        NTV2Channel     ch  = NTV2_CHANNEL1;
        bool            sdi = false;
        if (input::ToNTV2(n, src, ch, sdi) && card.features().CanDoInputSource(src))
            mask |= 1u << n;
    }
    return mask;
}
} // namespace

CardCaps AJADevice::Caps() const noexcept
{
    CardCaps c;
    c.numFrameStores = static_cast<int>(m_status.numFrameStores);
    c.numSdiInputs   = static_cast<int>(m_status.numVideoInputs);
    c.numHdmiInputs  = static_cast<int>(m_status.numHDMIInputs);
    c.numCSCs        = static_cast<int>(m_status.numCSCs);
    c.can12gRouting  = m_status.canDo12gRouting;
    c.can425Mux      = m_status.can425Mux;
    c.can4K          = m_status.canDo4KVideo;
    c.multiFormat    = m_status.canDoMultiFormat;
    c.isIP             = m_status.isIP;
    c.hdmiQuadrantMask = m_status.hdmiQuadrantMask;
    c.hdmiRgbMask      = m_status.hdmiRgbMask;
    return c;
}

AJADevice::AJADevice()
    : m_impl(std::make_unique<Impl>())
{
}

AJADevice::~AJADevice()
{
    Close();
}

bool AJADevice::IsOpen() const noexcept
{
    return m_impl && m_impl->card.IsOpen();
}

uint32_t AJADevice::CountDevices()
{
    // CNTV2DeviceScanner::GetDeviceAtIndex rescans on every call and opens
    // the device temporarily. We probe indices 0..N until we hit a miss to
    // get a count without holding any device open afterwards. The scan is
    // bounded by the number of AJA devices any reasonable host machine can
    // carry; we cap it defensively to keep the loop finite if the driver
    // ever started returning true unconditionally.
    constexpr uint32_t kProbeLimit = 16;
    uint32_t count = 0;
    for (uint32_t i = 0; i < kProbeLimit; ++i)
    {
        CNTV2Card probe;
        if (!CNTV2DeviceScanner::GetDeviceAtIndex(i, probe))
            break;
        probe.Close();
        ++count;
    }
    return count;
}

std::vector<AJADevice::DeviceEntry> AJADevice::EnumerateDevices()
{
    // Walk the same 0..N range as CountDevices, but for each card that
    // opens successfully we additionally read its model name and serial
    // and pack them into a DeviceEntry. The scanner opens each card
    // briefly to populate the CNTV2Card object's identity fields; the
    // local `probe` goes out of scope at the end of every iteration so
    // each card is closed before we move to the next, which keeps the
    // scan from holding open files on every AJA card on the host.
    //
    // A concurrent capture instance running against one of these cards
    // is unaffected: AJA's driver supports multiple opens of the same
    // physical device (it is how our own MultiFormat multi-instance
    // path works), and the scanner's read of GetModelName / serial is
    // a register peek with no side effects on the capture pipeline.
    constexpr uint32_t kProbeLimit = 16;
    std::vector<DeviceEntry> entries;
    for (uint32_t i = 0; i < kProbeLimit; ++i)
    {
        CNTV2Card probe;
        if (!CNTV2DeviceScanner::GetDeviceAtIndex(i, probe))
            break;
        if (!probe.IsOpen())
        {
            probe.Close();
            break;
        }
        DeviceEntry e;
        e.index     = i;
        e.modelName = probe.GetModelName();
        // GetSerialNumberString is the canonical API; it returns the
        // string form (e.g. "X12345") rather than the raw 64-bit
        // serial number. Empty serial is acceptable: some older or
        // engineering-sample cards do not have one programmed in.
        std::string serial;
        if (probe.GetSerialNumberString(serial))
            e.serial = serial;
        // v0.42: what the input menu offers and whether it can capture.
        e.canCapture = probe.features().CanDoCapture();
        e.inputMask  = InputMaskFor(probe, CapsFor(probe));
        probe.Close();
        entries.push_back(std::move(e));
    }
    return entries;
}

bool AJADevice::Open(uint32_t deviceIndex)
{
    if (IsOpen())
    {
        // Already open: refuse to change device implicitly. The caller can
        // Close() first if they want to retarget.
        return m_status.openStatus == AJAOpenStatus::Ok;
    }

    // Reset the status snapshot for this attempt. We populate it again as
    // soon as we have any information, including on failure.
    m_status              = Status{};
    m_status.deviceIndex  = deviceIndex;
    m_status.openStatus   = AJAOpenStatus::OpenFailed;

    const bool opened = CNTV2DeviceScanner::GetDeviceAtIndex(deviceIndex,
                                                             m_impl->card);
    if (!opened || !m_impl->card.IsOpen())
    {
        // Distinguish "no device at all" from "device exists but busy".
        // GetDeviceAtIndex does not differentiate, so we look at the scan
        // count: zero means the driver sees no boards, non-zero means at
        // least one is present and our open attempt got rejected.
        const uint32_t total = CountDevices();
        if (total == 0)
            m_status.openStatus = AJAOpenStatus::NoDeviceFound;
        else if (deviceIndex >= total)
            m_status.openStatus = AJAOpenStatus::NoDeviceFound;
        else
            m_status.openStatus = AJAOpenStatus::DeviceBusy;
        return false;
    }

    // Verify the card is actually ready for I/O. IsDeviceReady catches
    // cards in a half-initialised state (e.g. firmware still loading
    // after a fresh boot, kernel driver in the middle of a reset). The
    // matching error code is OpenFailed because the open call did not
    // produce a usable device handle.
    if (!m_impl->card.IsDeviceReady(false))
    {
        m_impl->card.Close();
        m_status.openStatus = AJAOpenStatus::OpenFailed;
        return false;
    }

    // A handful of AJA cards are output-only (some TTAP variants etc.).
    // Capture asks the capabilities object whether ANY video input
    // (SDI / HDMI / analog) is present; if not, we refuse the open with
    // OpenFailed so the user sees the synthetic fallback instead of a
    // silently broken capture path.
    //
    // While we have the DeviceCapabilities object handy, also verify
    // that the card supports the pixel format the shader expects
    // (8-bit UYVY 4:2:2). Every modern AJA card does, but a few legacy
    // models do not, and continuing on a card that cannot allocate the
    // FrameStore in UYVY would just hand us garbage frames.
    {
        DeviceCapabilities cap(m_impl->card);
        if (!cap.CanDoCapture())
        {
            m_impl->card.Close();
            m_status.openStatus = AJAOpenStatus::OpenFailed;
            return false;
        }
        if (!cap.CanDoFrameBufferFormat(NTV2_FBF_8BIT_YCBCR))
        {
            m_impl->card.Close();
            m_status.openStatus = AJAOpenStatus::OpenFailed;
            return false;
        }
    }

    // Save the device's current task mode so Close() can put the board
    // back the way we found it. NTV2_STANDARD_TASKS is what AJA Control
    // Room expects; we will switch to NTV2_OEM_TASKS for the duration
    // of our capture to prevent any retail service from racing us on
    // crosspoint or video-format settings.
    //
    // Acquire the device for our process. This is a co-operative claim
    // (the AJA driver records app signature + PID, but does not
    // physically lock other apps out). Combined with NTV2_OEM_TASKS
    // below, it tells any other AJA software that the device is in use
    // and ensures AJA Control Room's auto-configuration does not run
    // behind our back. Failure is logged but non-fatal.
    //
    // Only the first instance to open this device index does any of
    // this; later instances just take a reference (see ClaimTable).
    {
        std::lock_guard<std::mutex> lock(ClaimMutex());
        DeviceClaim& claim = ClaimTable()[deviceIndex];
        if (claim.refs == 0)
        {
            if (!m_impl->card.GetTaskMode(claim.savedTaskMode))
                claim.savedTaskMode = NTV2_STANDARD_TASKS;
            claim.acquired = m_impl->card.AcquireStreamForApplication(
                kAppSignature,
                static_cast<int32_t>(AJAProcess::GetPid()));
            // Switch to OEM task mode so AJA's retail services keep
            // their hands off this card while we drive it. Reverted by
            // the last Close() on this device index.
            m_impl->card.SetTaskMode(NTV2_OEM_TASKS);
        }
        else
        {
            // v0.40: later instances re-assert OEM tasks (a no-op when it
            // is already set), as every Open did before v0.39. If AJA's
            // retail services or another application switched the card
            // back to standard tasks meanwhile, the services would keep
            // re-applying their own routing and formats over ours.
            m_impl->card.SetTaskMode(NTV2_OEM_TASKS);
        }
        ++claim.refs;
        m_impl->holdsClaimRef = true;
        m_impl->claimIndex    = deviceIndex;
    }

    // Identity. Each of these can fail individually; we keep going and
    // leave the affected field empty rather than aborting the open.
    m_status.deviceId      = static_cast<uint64_t>(m_impl->card.GetDeviceID());
    m_status.modelName     = m_impl->card.GetModelName();
    m_status.displayName   = m_impl->card.GetDisplayName();
    m_status.driverVersion = m_impl->card.GetDriverVersionString();
    m_status.deviceVersion = m_impl->card.GetDeviceVersionString();
    {
        std::string serial;
        if (m_impl->card.GetSerialNumberString(serial))
            m_status.serialNumber = std::move(serial);
    }

    // Capabilities via the modern DeviceCapabilities accessor. We probe the
    // four lowest-numbered SDI input sources here; the Kona X exposes four,
    // and reading the flags up front lets the UI/log layer present an
    // unambiguous picture of what we can read from.
    auto& cap = m_impl->card.features();
    m_status.numVideoInputs  = cap.GetNumVideoInputs();
    m_status.numVideoOutputs = cap.GetNumVideoOutputs();
    m_status.numHDMIInputs   = cap.GetNumHDMIVideoInputs();
    m_status.numHDMIOutputs  = cap.GetNumHDMIVideoOutputs();
    m_status.numFrameStores  = cap.GetNumFrameStores();
    m_status.numCSCs         = cap.GetNumCSCs();
    m_status.numDMAEngines   = static_cast<uint16_t>(cap.GetNumDMAEngines());
    // Active memory is bytes; convert to MB for the human-readable log.
    m_status.activeMemoryMB  = static_cast<uint32_t>(
        cap.GetActiveMemorySize() / (1024u * 1024u));
    m_status.hdmiVersion     = static_cast<uint16_t>(cap.GetHDMIVersion());

    m_status.canDoFBFYCbCr   = cap.CanDoFrameBufferFormat(NTV2_FBF_8BIT_YCBCR);
    m_status.canDoFBF10Bit   = cap.CanDoFrameBufferFormat(NTV2_FBF_10BIT_YCBCR);
    m_status.hasBiDirSDI     = cap.HasBiDirectionalSDI();
    m_status.canDoSDIChecks  = cap.CanDoSDIErrorChecks();
    m_status.canDoSDI1       = cap.CanDoInputSource(NTV2_INPUTSOURCE_SDI1);
    m_status.canDoSDI2       = cap.CanDoInputSource(NTV2_INPUTSOURCE_SDI2);
    m_status.canDoSDI3       = cap.CanDoInputSource(NTV2_INPUTSOURCE_SDI3);
    m_status.canDoSDI4       = cap.CanDoInputSource(NTV2_INPUTSOURCE_SDI4);
    m_status.numAnalogInputs = cap.GetNumAnalogVideoInputs();
    m_status.can425Mux       = cap.CanDo425Mux();
    m_status.isIP            = cap.CanDoIP();
    HdmiWidgetOutputs(m_impl->card.GetDeviceID(),
                      static_cast<int>(cap.GetNumHDMIVideoInputs()),
                      m_status.hdmiQuadrantMask, m_status.hdmiRgbMask);

    m_status.canDoSDVideo    = cap.CanDoSDVideo();
    m_status.canDoHDVideo    = cap.CanDoHDVideo();
    m_status.canDo2KVideo    = cap.CanDo2KVideo();
    m_status.canDo4KVideo    = cap.CanDo4KVideo();
    m_status.canDo8KVideo    = cap.CanDo8KVideo();
    m_status.canDo12GSDI     = cap.CanDo12GSDI();
    m_status.canDoHFRRGB     = cap.CanDoHFRRGB();

    m_status.canDo12gRouting   = cap.CanDo12gRouting();
    m_status.canDoMultiFormat  = cap.CanDoMultiFormat();
    m_status.canDo3GLevelConv  = cap.CanDo3GLevelConversion();
    m_status.canDoRGBLevelA    = cap.CanDoRGBLevelAConversion();
    m_status.canDoColorCorr    = cap.CanDoColorCorrection();
    m_status.canDoEnhancedCSC  = cap.CanDoEnhancedCSC();

    m_status.canDoCustomAnc    = cap.CanDoCustomAnc();
    m_status.canDoRP188        = cap.CanDoRP188();

    m_status.canDoStreamingDMA = cap.CanDoStreamingDMA();
    m_status.canDoMSI          = cap.CanDoMSI();

    m_status.canDoHDMIMultiView = cap.CanDoHDMIMultiView();
    m_status.inputMask          = InputMaskFor(m_impl->card, Caps());

    // Dump the lot to the host log so the operator (and we) can see
    // exactly what this card claims to do the very first time it
    // appears. Every line is tagged [AJAInput:caps] so the operator
    // can grep the Resolume log without wading through unrelated
    // verbosity. The dump fires exactly once per device open; reading
    // it should be the first step in any troubleshooting session.
    auto yn = [](bool b) { return b ? "yes" : "no"; };
    char buf[512];

    std::snprintf(buf, sizeof(buf),
        "[AJAInput:caps] === %s (SN %s) ===",
        m_status.modelName.empty() ? "(unknown model)" : m_status.modelName.c_str(),
        m_status.serialNumber.empty() ? "?" : m_status.serialNumber.c_str());
    FFGLLog::LogToHost(buf);

    std::snprintf(buf, sizeof(buf),
        "[AJAInput:caps] firmware=%s driver=%s id=0x%llx",
        m_status.deviceVersion.empty() ? "?" : m_status.deviceVersion.c_str(),
        m_status.driverVersion.empty() ? "?" : m_status.driverVersion.c_str(),
        static_cast<unsigned long long>(m_status.deviceId));
    FFGLLog::LogToHost(buf);

    std::snprintf(buf, sizeof(buf),
        "[AJAInput:caps] connectors: SDI in=%u out=%u biDir=%s "
        "HDMI in=%u out=%u (gen %u) analog in=%u (not captured)",
        unsigned(m_status.numVideoInputs),
        unsigned(m_status.numVideoOutputs),
        yn(m_status.hasBiDirSDI),
        unsigned(m_status.numHDMIInputs),
        unsigned(m_status.numHDMIOutputs),
        unsigned(m_status.hdmiVersion),
        unsigned(m_status.numAnalogInputs));
    FFGLLog::LogToHost(buf);

    {
        // v0.42: the inputs this plugin can capture on the card.
        std::string ins = "[AJAInput:caps] inputs:";
        for (int k = 0; k < input::kMaxInputNumber; ++k)
        {
            const int n = input::kMenuOrder[k];
            if (m_status.inputMask & (1u << n))
            {
                ins += ' ';
                ins += input::LabelUpper(n);
            }
        }
        FFGLLog::LogToHost(ins.c_str());
    }

    std::snprintf(buf, sizeof(buf),
        "[AJAInput:caps] internals: FrameStores=%u CSCs=%u DMA=%u MSI=%s "
        "memory=%u MB",
        unsigned(m_status.numFrameStores),
        unsigned(m_status.numCSCs),
        unsigned(m_status.numDMAEngines),
        yn(m_status.canDoMSI),
        unsigned(m_status.activeMemoryMB));
    FFGLLog::LogToHost(buf);

    std::snprintf(buf, sizeof(buf),
        "[AJAInput:caps] rasters: SD=%s HD=%s 2K=%s 4K=%s 8K=%s "
        "12G-SDI=%s HFR-RGB=%s",
        yn(m_status.canDoSDVideo), yn(m_status.canDoHDVideo),
        yn(m_status.canDo2KVideo), yn(m_status.canDo4KVideo),
        yn(m_status.canDo8KVideo), yn(m_status.canDo12GSDI),
        yn(m_status.canDoHFRRGB));
    FFGLLog::LogToHost(buf);

    std::snprintf(buf, sizeof(buf),
        "[AJAInput:caps] pixfmt: 8bYCbCr=%s 10bYCbCr=%s",
        yn(m_status.canDoFBFYCbCr), yn(m_status.canDoFBF10Bit));
    FFGLLog::LogToHost(buf);

    std::snprintf(buf, sizeof(buf),
        "[AJAInput:caps] routing: 12g=%s 425mux=%s multiFmt=%s 3GLvlConv=%s "
        "RGBLvlA=%s LUTs=%s EnhCSC=%s",
        yn(m_status.canDo12gRouting), yn(m_status.can425Mux),
        yn(m_status.canDoMultiFormat),
        yn(m_status.canDo3GLevelConv), yn(m_status.canDoRGBLevelA),
        yn(m_status.canDoColorCorr), yn(m_status.canDoEnhancedCSC));
    FFGLLog::LogToHost(buf);

    std::snprintf(buf, sizeof(buf),
        "[AJAInput:caps] anc/timecode: customAnc=%s RP188=%s",
        yn(m_status.canDoCustomAnc), yn(m_status.canDoRP188));
    FFGLLog::LogToHost(buf);

    std::snprintf(buf, sizeof(buf),
        "[AJAInput:caps] diag: SDI-err-checks=%s HDMI-MultiView=%s",
        yn(m_status.canDoSDIChecks), yn(m_status.canDoHDMIMultiView));
    FFGLLog::LogToHost(buf);

    // Streaming DMA gets its own line because it is the GPUDirect
    // hook everyone asks about and the operator's eye will go here
    // first when latency is the question.
    std::snprintf(buf, sizeof(buf),
        "[AJAInput:caps] streamingDMA=%s "
        "(Linux-only path; not exposed by current Windows AJA driver)",
        yn(m_status.canDoStreamingDMA));
    FFGLLog::LogToHost(buf);

    FFGLLog::LogToHost("[AJAInput:caps] === end ===");


    // 3G-SDI Level B to Level A input conversion is NOT enabled here.
    // The SDK pattern (per the ntv2framegrabber and cudaapp demos) is
    // to enable it per-channel, conditional on GetSDIInput3GbPresent,
    // as part of the channel setup AFTER the SDI receiver has had time
    // to lock onto the cable. We do exactly that inside
    // AJACaptureSource::TryConfigureChannel. The main ntv2capture demo
    // does not enable it at all and still captures Level B inputs
    // reliably, suggesting the conversion is a defensive normalisation
    // rather than a hard requirement; we keep the call for robustness
    // but only where the SDK pattern places it (per-channel, after
    // bidir flip and VBI lock, conditional on the physical state of
    // the cable). Calling it unconditionally with `true` at open time
    // for every SDI BNC — as a previous iteration of this code did —
    // is not a pattern any SDK demo follows and the AJA SDK
    // documentation does not guarantee it is a safe no-op on cables
    // that are not Level B.

    // Enable independent per-channel format mode. AJA cards default to
    // bidir flip and VBI lock, conditional on the physical state of
    // the cable). Calling it unconditionally with `true` at open time
    // for every SDI BNC — as a previous iteration of this code did —
    // is not a pattern any SDK demo follows and the AJA SDK
    // documentation does not guarantee it is a safe no-op on cables
    // that are not Level B.

    // Enable independent per-channel format mode. AJA cards default to
    // a single shared clock across all FrameStores when this is off, so
    // switching the SDI Input parameter from a 1080p60 source to a
    // 720p50 one (or running another application on a different
    // channel) drags the rest of the card with it and produces the
    // classic "second channel won't lock" failure pattern. Multi-format
    // mode makes each FrameStore independent. Required for our
    // user-selectable SDI Input semantics even when only one channel
    // is being captured at a time, because the previous channel's
    // format may still be cached by the driver.
    //
    // The capability check is paranoid: every Kona X supports this and
    // SDK 17+ deprecates the uniformat mode entirely, but on the off
    // chance a future card lands here without the feature, we log it
    // and continue.
    if (cap.CanDoMultiFormat())
        m_impl->card.SetMultiFormatMode(true);

    // Set the device reference to its internal free-run oscillator.
    // The reference is what AJA's output channels lock to (genlock); we
    // do not have outputs, so the choice does not affect rendered
    // pixels. What it DOES affect is the behaviour when one input
    // loses signal: pinning the reference to a specific SDI input
    // (the default on some firmware) would cause other channels to
    // also lose lock if that SDI goes dark. Free-run isolates each
    // input from the others completely. With MultiFormat mode also
    // enabled, each channel runs on its own input-recovered clock and
    // signal loss on one cable does not propagate.
    m_impl->card.SetReference(NTV2_REFERENCE_FREERUN);

    m_status.openStatus = AJAOpenStatus::Ok;
    return true;
}

void AJADevice::Close()
{
    if (m_impl && m_impl->card.IsOpen())
    {
        // The last instance on this device restores the state the card
        // was in before the first one opened it: hand back the stream
        // claim (if we got one) and put the task mode back to whatever
        // AJA Control Room or the previous app had configured. Done
        // before the actual handle close so the AJA driver sees a clean
        // release sequence.
        if (m_impl->holdsClaimRef)
        {
            std::lock_guard<std::mutex> lock(ClaimMutex());
            auto it = ClaimTable().find(m_impl->claimIndex);
            if (it != ClaimTable().end() && --it->second.refs <= 0)
            {
                if (it->second.acquired)
                {
                    m_impl->card.ReleaseStreamForApplication(
                        kAppSignature,
                        static_cast<int32_t>(AJAProcess::GetPid()));
                }
                m_impl->card.SetTaskMode(it->second.savedTaskMode);
                ClaimTable().erase(it);
            }
            m_impl->holdsClaimRef = false;
        }

        m_impl->card.Close();
    }
    if (m_status.openStatus == AJAOpenStatus::Ok)
        m_status.openStatus = AJAOpenStatus::NotAttempted;
}

CNTV2Card* AJADevice::Card() noexcept
{
    if (!m_impl || !m_impl->card.IsOpen())
        return nullptr;
    return &m_impl->card;
}

uint32_t AJADevice::FrameStoresFeedingSdiOutput(int bncZeroBased) noexcept
{
    if (!m_impl || !m_impl->card.IsOpen() || bncZeroBased < 0 || bncZeroBased > 7)
        return 0u;
    CNTV2Card& card = m_impl->card;
    const int fsCount = std::min<int>(static_cast<int>(m_status.numFrameStores), 8);

    // Output crosspoints of every FrameStore (YUV / RGB, normal / 425).
    std::map<int, int> fsOfOutput;
    for (int fs = 0; fs < fsCount; ++fs)
    {
        const NTV2Channel ch = static_cast<NTV2Channel>(fs);
        for (int rgb = 0; rgb < 2; ++rgb)
            for (int is425 = 0; is425 < 2; ++is425)
            {
                const NTV2OutputXptID x =
                    ::GetFrameStoreOutputXptFromChannel(ch, rgb != 0, is425 != 0);
                if (x != NTV2_OUTPUT_CROSSPOINT_INVALID)
                    fsOfOutput[static_cast<int>(x)] = fs;
            }
    }

    // Breadth-first walk upstream from the SDI output's inputs (both data
    // streams), through at most a few widgets (CSC, LUT, mux, dual-link).
    std::vector<NTV2InputXptID> frontier;
    for (int ds2 = 0; ds2 < 2; ++ds2)
    {
        const NTV2InputXptID in =
            ::GetSDIOutputInputXpt(static_cast<NTV2Channel>(bncZeroBased), ds2 != 0);
        if (in != NTV2_INPUT_CROSSPOINT_INVALID)
            frontier.push_back(in);
    }
    std::set<int> seenOutputs;
    uint32_t mask = 0u;
    for (int depth = 0; depth < 4 && !frontier.empty(); ++depth)
    {
        std::vector<NTV2InputXptID> next;
        for (const NTV2InputXptID in : frontier)
        {
            NTV2OutputXptID out = NTV2_XptBlack;
            if (!card.GetConnectedOutput(in, out) || out == NTV2_XptBlack)
                continue;
            if (!seenOutputs.insert(static_cast<int>(out)).second)
                continue;
            const auto fsIt = fsOfOutput.find(static_cast<int>(out));
            if (fsIt != fsOfOutput.end())
            {
                mask |= (1u << fsIt->second);
                continue;
            }
            NTV2WidgetID widget = NTV2_WIDGET_INVALID;
            if (!CNTV2SignalRouter::GetWidgetForOutput(out, widget, card.GetDeviceID()))
                continue;
            NTV2InputXptIDSet widgetInputs;
            if (CNTV2SignalRouter::GetWidgetInputs(widget, widgetInputs))
                next.insert(next.end(), widgetInputs.begin(), widgetInputs.end());
        }
        frontier.swap(next);
    }
    return mask;
}

bool AJADevice::SdiOutputCarriesForeignPlayout(int bncZeroBased) noexcept
{
    const uint32_t playout = BusyFrameStores(/*playoutOnly*/true);
    if (playout == 0u)
        return false;
    return (FrameStoresFeedingSdiOutput(bncZeroBased) & playout) != 0u;
}

int AJADevice::CanRouteInputToFrameStore(int inputNumber,
                                         int frameStoreZeroBased) noexcept
{
    if (!m_impl || !m_impl->card.IsOpen() ||
        frameStoreZeroBased < 0 || frameStoreZeroBased > 7)
        return -1;
    // v0.42: a quad-link input takes a fixed FrameStore set through 425
    // muxes (CaptureTopology.h); its routing is checked when configured.
    if (input::IsQuad(inputNumber))
        return -1;
    NTV2InputSource source = NTV2_INPUTSOURCE_INVALID;
    NTV2Channel     conn   = NTV2_CHANNEL1;
    bool            isSdi  = false;
    if (!input::ToNTV2(inputNumber, source, conn, isSdi))
        return -1;
    const NTV2Channel     fs   = static_cast<NTV2Channel>(frameStoreZeroBased);
    const NTV2InputXptID  fsIn = ::GetFrameStoreInputXptFromChannel(fs);
    const NTV2OutputXptID src  = ::GetInputSourceOutputXpt(source, false, false);
    bool canConnect = false;
    if (!m_impl->card.CanConnect(fsIn, src, canConnect))
        return -1;   // no crosspoint ROM: the card cannot say
    if (canConnect)
        return 1;
    if (!isSdi)
    {
        // An RGB HDMI input reaches the FrameStore through its CSC.
        const NTV2InputXptID  cscIn  = ::GetCSCInputXptFromChannel(fs);
        const NTV2OutputXptID hdmiRgb = ::GetInputSourceOutputXpt(source, false, true);
        const NTV2OutputXptID cscYuv = ::GetCSCOutputXptFromChannel(fs, false, false);
        bool a = false, b = false;
        if (m_impl->card.CanConnect(cscIn, hdmiRgb, a) &&
            m_impl->card.CanConnect(fsIn, cscYuv, b) && a && b)
            return 1;
    }
    return 0;
}

uint32_t AJADevice::BusyFrameStores(bool playoutOnly) noexcept
{
    if (!m_impl || !m_impl->card.IsOpen())
        return 0u;
    uint32_t mask = 0u;
    const int count = static_cast<int>(m_status.numFrameStores);
    for (int i = 0; i < count && i < 8; ++i)
    {
        AUTOCIRCULATE_STATUS st;
        if (m_impl->card.AutoCirculateGetStatus(static_cast<NTV2Channel>(i), st) &&
            !st.IsStopped() && (!playoutOnly || st.IsOutput()))
            mask |= (1u << i);
    }
    // v0.42: on a card without 12G routing a FrameStore running in TSI or
    // quad (squares) mode drives its pair / group of four: a 4K picture.
    // AutoCirculate only runs on the first of them.
    if (mask != 0u && !m_status.canDo12gRouting)
    {
        uint32_t extra = 0u;
        for (int i = 0; i < count && i < 8; ++i)
        {
            if ((mask & (1u << i)) == 0u)
                continue;
            bool on = false;
            if (m_status.can425Mux &&
                m_impl->card.GetTsiFrameEnable(on, static_cast<NTV2Channel>(i)) && on)
                extra |= 3u << (i & ~1);
            on = false;
            if (m_status.canDo4KVideo &&
                m_impl->card.Get4kSquaresEnable(on, static_cast<NTV2Channel>(i)) && on)
                extra |= 0xFu << (i < 4 ? 0 : 4);
        }
        const uint32_t valid = (count >= 32) ? ~0u : ((1u << count) - 1u);
        mask |= extra & valid;
    }
    return mask;
}

int AJADevice::ProbeInputFormat(int inputNumber, bool switchToReceive) noexcept
{
    if (!m_impl || !m_impl->card.IsOpen())
        return 0;

    NTV2InputSource source = NTV2_INPUTSOURCE_SDI1;
    NTV2Channel     bnc    = NTV2_CHANNEL1;
    bool            isSDI  = true;
    if (!input::ToNTV2(inputNumber, source, bnc, isSDI))
        return 0;
    if (!m_impl->card.features().CanDoInputSource(source))
        return 0;

    // The SDI connector must be in "receive" mode for the input format
    // detector to lock. Flipping the bit here is idempotent and safe to
    // do even before AutoCirculate is configured; the user's capture
    // session will reconfigure as needed. HDMI inputs are unidirectional
    // so this step does not apply. v0.42: a quad-link input switches all
    // four of its connectors (each refused if it carries another
    // application's playout).
    if (isSDI && m_impl->card.features().HasBiDirectionalSDI())
    {
        const int links = input::ConnectorCount(inputNumber);
        for (int k = 0; k < links; ++k)
        {
            const NTV2Channel c = static_cast<NTV2Channel>(bnc + k);
            bool transmitting = false;
            m_impl->card.GetSDITransmitEnable(c, transmitting);
            if (!transmitting)
                continue;
            if (!switchToReceive ||
                SdiOutputCarriesForeignPlayout(static_cast<int>(c)))
                return kProbeIsOutput;
            m_impl->card.SetSDITransmitEnable(c, false);
        }
    }

    // 3G-SDI Level B handling. When the cable carries 3G Level B (two
    // 1.485 Gbps sub-streams multiplexed into one 2.97 Gbps link), the
    // SDI receiver's raw output is the dual-stream HD payload running
    // at half the wall-clock rate of the source. If we read
    // GetInputVideoFormat() in that state for a 1080p60 Level B source,
    // we get back 1080p30 — and then the rest of the pipeline configures
    // its FrameStore for 1080p30, which mismatches the actual 1080p60
    // data the receiver will deliver once Level B→A conversion is
    // engaged. The result is AutoCirculateTransfer never completing
    // (HasAvailableInputFrame() stays false because the FrameStore
    // raster does not match what the receiver writes), zero frames
    // captured, and the consumer sees an uninitialised texture — the
    // "gris uniforme" symptom that closes the Level B bring-up.
    //
    // Fix: detect 3Gb on the cable, engage the per-channel Level B→A
    // conversion BEFORE reading the format, give the receiver one VBI
    // period to apply the new mode, then read. The format we return
    // is now the post-conversion format (1080p60 Level A), which is
    // what TryConfigureChannel will program into the FrameStore. The
    // SetSDIInLevelBtoLevelAConversion call is idempotent and the
    // same call gets re-issued (with the same true value) in
    // TryConfigureChannel — that re-issue is harmless and keeps the
    // receiver-side state consistent across format-change reopens.
    //
    // The 50 ms sleep is the empirical settle time for the receiver
    // to apply the conversion register and re-lock on the converted
    // stream. AJA's own framegrabber demo never needs an explicit
    // sleep because it calls SetSDIInLevelBtoLevelAConversion AFTER
    // EnableChannel + crosspoint Connect + format set — those calls
    // collectively cost more than a VBI period. We do not have that
    // staircase of intermediate calls here, so we wait explicitly.
    // Only paid when 3Gb is actually present, so signal-free or
    // Level A cables do not see the cost.
    if (isSDI && switchToReceive)
    {
        // v0.42: every link of a quad-link input (one settle wait).
        bool anyConverted = false;
        const int links = input::ConnectorCount(inputNumber);
        for (int k = 0; k < links; ++k)
        {
            const NTV2Channel c = static_cast<NTV2Channel>(bnc + k);
            bool is3Gb = false;
            m_impl->card.GetSDIInput3GbPresent(is3Gb, c);
            if (is3Gb)
            {
                m_impl->card.SetSDIInLevelBtoLevelAConversion(
                    static_cast<UWord>(c), true);
                anyConverted = true;
            }
        }
        if (anyConverted)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const NTV2VideoFormat fmtRaw =
        m_impl->card.GetInputVideoFormat(source, false);

    // v0.42: a quad-link input carries the 4K picture its four 1080p / 2K
    // links make; 0 when the first link is not such a picture.
    if (input::IsQuad(inputNumber))
        return QuadLinkFormatFromLink(static_cast<int>(fmtRaw));

    // 3G Level B → Level A enum substitution. The receiver still reports
    // the Level B enum (because the VPID embedded in the cable says so)
    // even after the conversion above is engaged. Downstream code wants
    // the Level A equivalent so that GetNTV2FrameRateFromVideoFormat
    // returns the true display rate (60 Hz for 1080p_6000_B → 1080p_6000_A)
    // and so that SetVideoFormat on the FrameStore programs the raster
    // AutoCirculate will actually deliver. The remap is centralised in
    // ajainput::MapLevelBFormatToLevelA so the same logic applies in
    // TryDecodeNTV2Format and in the live-probe paths inside the
    // capture loop.
    return MapLevelBFormatToLevelA(static_cast<int>(fmtRaw));
}

bool AJADevice::ProbeIs3GbInput(int inputNumber) noexcept
{
    if (!m_impl || !m_impl->card.IsOpen())
        return false;

    // Only applicable to SDI. HDMI has no Level-A/Level-B concept.
    NTV2InputSource source  = NTV2_INPUTSOURCE_INVALID;
    NTV2Channel     channel = NTV2_CHANNEL1;
    bool            isSdi   = false;
    if (!input::ToNTV2(inputNumber, source, channel, isSdi) || !isSdi)
        return false;

    bool is3Gb = false;
    if (!m_impl->card.GetSDIInput3GbPresent(is3Gb, channel))
        return false;
    return is3Gb;
}

} // namespace ajainput
