// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    AJADevice.h
 * @brief   Wrapper around CNTV2Card for device discovery and lifetime
 *          management. Phase 6.4.1: enumeration, open, capability probing.
 *
 * This class encapsulates the conversation with the libajantv2 driver
 * required before any capture can happen:
 *   - Scan the host for AJA boards.
 *   - Open the first available device (or a specific one by index).
 *   - Query identity (model, serial, driver version) and capabilities
 *     (number of SDI inputs, supported input sources).
 *   - Expose a Status snapshot the plugin's UI layer can render.
 *
 * Capture path (AutoCirculate, transfers, callback thread) lives in a
 * future AJACaptureSource class; this header deliberately stops at
 * "device opened and described".
 *
 * Ownership rules: the underlying CNTV2Card holds a kernel-level claim on
 * the device while open. Construction does NOT open; call Open() explicitly
 * so the plugin can decide when to acquire the hardware. Close() and the
 * destructor release the claim. Open()/Close() are NOT thread-safe; serialise
 * them from the plugin's GL-init / GL-deinit path. The Status() getter is
 * safe to call concurrently with other reads but should also be sequenced
 * with Open/Close from the application side.
 */
#pragma once

#include "CaptureTopology.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward declaration: AJACaptureSource needs typed access to the underlying
// CNTV2Card object, but consumers that only inspect Status (e.g. the plugin's
// UI/overlay layer) do not need the full libajantv2 header tree pulled in.
class CNTV2Card;

namespace ajainput
{

/// @brief Reason an Open() call failed. Suitable for display to the user.
enum class AJAOpenStatus
{
    NotAttempted,   ///< Open() has not been called yet.
    Ok,             ///< Device is open and ready.
    NoDeviceFound,  ///< Scanner reported zero AJA devices on the host.
    DeviceBusy,     ///< A device exists but another process has it claimed.
    DriverMissing,  ///< The AJA driver is not installed or not loaded.
    OpenFailed,     ///< Unspecified failure from the driver layer.
};

/// @brief Convert the open status to a short human-readable string.
const char* ToString(AJAOpenStatus s) noexcept;

class AJADevice
{
public:
    /// @brief Snapshot of the opened device's identity and capabilities.
    struct Status
    {
        AJAOpenStatus  openStatus      = AJAOpenStatus::NotAttempted;
        uint32_t       deviceIndex     = 0;        ///< Zero-based card index.
        uint64_t       deviceId        = 0;        ///< NTV2DeviceID enum value.
        std::string    modelName;                  ///< e.g. "KONA X".
        std::string    displayName;                ///< Vendor display name.
        std::string    serialNumber;               ///< Hardware serial.
        std::string    driverVersion;              ///< Driver version string.
        std::string    deviceVersion;              ///< Firmware version string.

        // Counters.
        uint16_t       numVideoInputs  = 0;        ///< Count of SDI inputs.
        uint16_t       numVideoOutputs = 0;        ///< Count of SDI outputs.
        uint16_t       numHDMIInputs   = 0;        ///< Count of HDMI inputs.
        uint16_t       numHDMIOutputs  = 0;        ///< Count of HDMI outputs.
        uint16_t       numFrameStores  = 0;        ///< Count of internal DMA engines.
        uint16_t       numCSCs         = 0;        ///< Colorspace converter widgets.
        uint16_t       numDMAEngines   = 0;        ///< DMA engines on the card.
        uint32_t       activeMemoryMB  = 0;        ///< Reserved SDRAM size, in MB.
        uint16_t       hdmiVersion     = 0;        ///< HDMI chipset gen (4 or 5).

        // Capability flags.
        bool           canDoFBFYCbCr   = false;    ///< Card supports NTV2_FBF_8BIT_YCBCR.
        bool           canDoFBF10Bit   = false;    ///< Card supports NTV2_FBF_10BIT_YCBCR.
        bool           hasBiDirSDI     = false;    ///< SDI BNCs are configurable in/out.
        bool           canDoSDIChecks  = false;    ///< CRC/VPID/TRS error reporting available.
        bool           canDoSDI1       = false;    ///< Has NTV2_INPUTSOURCE_SDI1.
        bool           canDoSDI2       = false;
        bool           canDoSDI3       = false;
        bool           canDoSDI4       = false;
        /// v0.42: bit n set = input number n (InputId.h) exists on this
        /// card, quad-link inputs included when the card takes them.
        uint32_t       inputMask       = 0;
        uint16_t       numAnalogInputs = 0;        ///< Analog inputs (not captured).
        bool           can425Mux       = false;    ///< v0.42: has 425 (TSI) muxes.
        bool           isIP            = false;    ///< v0.42: SMPTE 2110 / 2022 card.
        /// v0.42: bit h-1 = HDMI h's input widget has quadrant / RGB
        /// outputs (CardCaps::hdmiQuadrantMask / hdmiRgbMask).
        uint32_t       hdmiQuadrantMask = 0;
        uint32_t       hdmiRgbMask      = 0;

        // Video format ceilings.
        bool           canDoSDVideo    = false;    ///< Can handle SD rasters.
        bool           canDoHDVideo    = false;    ///< Can handle HD rasters.
        bool           canDo2KVideo    = false;    ///< Can handle 2K rasters.
        bool           canDo4KVideo    = false;    ///< Can handle 4K/UHD rasters.
        bool           canDo8KVideo    = false;    ///< Can handle 8K rasters.
        bool           canDo12GSDI     = false;    ///< Has 6G/12G SDI connectors.
        bool           canDoHFRRGB     = false;    ///< 1080p RGB >50fps.

        // Routing / signal processing.
        bool           canDo12gRouting    = false; ///< 6G/12G routing crosspoints.
        bool           canDoMultiFormat   = false; ///< Per-FrameStore independent formats.
        bool           canDo3GLevelConv   = false; ///< 3G-SDI Level B → A conversion.
        bool           canDoRGBLevelA     = false; ///< RGB over 3G Level A.
        bool           canDoColorCorr     = false; ///< Has LUTs.
        bool           canDoEnhancedCSC   = false; ///< Has enhanced CSCs (HDR-ready).

        // Ancillary / timecode.
        bool           canDoCustomAnc     = false; ///< SDI ANC inserter/extractor.
        bool           canDoRP188         = false; ///< Timecode insert/extract.

        // GPU / streaming DMA — the GPUDirect-relevant flag.
        bool           canDoStreamingDMA  = false; ///< AJA's streaming DMA API.
        bool           canDoMSI           = false; ///< DMA Message Signaled Interrupts.

        // Multi-view / mosaic.
        bool           canDoHDMIMultiView = false; ///< 4 HD → 1 HDMI mosaic.
    };

    AJADevice();
    ~AJADevice();

    AJADevice(const AJADevice&)            = delete;
    AJADevice& operator=(const AJADevice&) = delete;

    /**
     * @brief Scan the host and open the device at the given zero-based index.
     *
     * Populates the internal Status snapshot regardless of outcome so the
     * caller can inspect it (and the UI can render the reason) even on
     * failure. Returns true only when the device is opened and ready.
     *
     * Calling Open() while already open is a no-op and returns true; close
     * explicitly first if you intend to switch device.
     */
    bool Open(uint32_t deviceIndex = 0);

    /// @brief Release the device. Safe to call multiple times.
    void Close();

    /// @brief True iff the device is open.
    bool IsOpen() const noexcept;

    /// @brief Return the latest Status snapshot. Always populated after the
    ///        first Open() attempt, including on failure.
    const Status& GetStatus() const noexcept { return m_status; }

    /// @brief v0.42: the capabilities CaptureTopology plans with (valid
    ///        after a successful Open()).
    CardCaps Caps() const noexcept;

    /// @brief Static helper: count AJA devices currently visible to the host
    ///        without opening any of them. Useful for hot-plug diagnostics.
    static uint32_t CountDevices();

    /// @brief Lightweight identity entry produced by EnumerateDevices.
    ///        Just enough to build a user-facing dropdown that names
    ///        each card by model and (optional) serial. The full
    ///        capabilities snapshot is only populated by Open() because
    ///        getting it requires holding the card open and probing
    ///        the device caps.
    struct DeviceEntry
    {
        uint32_t    index = 0;    ///< Zero-based card index for Open().
        std::string modelName;    ///< e.g. "KONA X".
        std::string serial;       ///< Hardware serial; may be empty.
        /// v0.42: false for output-only devices (T-TAP, T-TAP Pro).
        bool        canCapture = true;
        /// v0.42: the card's inputs, as Status::inputMask.
        uint32_t    inputMask  = 0;
    };

    /// @brief Static helper: enumerate every AJA card visible to the host
    ///        and return one DeviceEntry per card. Each entry is
    ///        sufficient to build a user-facing dropdown label such as
    ///        "0: KONA X (SN 12345)" without opening the card for use.
    ///        The scanner opens each card briefly to read its name and
    ///        serial, then closes it before returning, so concurrent
    ///        capture instances on a different card are unaffected.
    ///        Order matches the scanner's enumeration order: entry N
    ///        is the card that Open(N) would target.
    static std::vector<DeviceEntry> EnumerateDevices();

    /// @brief Access the underlying CNTV2Card. Returns nullptr if the device
    ///        is not open. The returned pointer is owned by this AJADevice
    ///        and is only valid as long as Close() has not been called.
    ///        Intended for use by AJACaptureSource and similar internal
    ///        components that need direct driver access.
    CNTV2Card* Card() noexcept;

    /**
     * @brief Query the input video format currently locked on a given
     *        input, without configuring or claiming the channel.
     *        v0.42: takes an input number (InputId.h): SDI 1..8, HDMI 1..4,
     *        or a quad-link input, whose format is the 4K picture its four
     *        links make (QuadLinkFormatFromLink on the first link).
     *
     * @param switchToReceive  v0.40: when true (the input this instance
     *         is going to capture), a bidirectional SDI connector that is
     *         currently transmitting is switched to receive first, unless
     *         its SDI output carries another application's playout (then
     *         it is left alone; v0.41, see SdiOutputCarriesForeignPlayout). When false (the informational probe of the
     *         other inputs at load), nothing on the card is changed: a
     *         transmitting connector is reported as kProbeIsOutput and the
     *         Level B to A conversion is not engaged. Up to v0.39 the
     *         load-time probe switched EVERY bidirectional connector to
     *         receive, which silently broke an SDI output another
     *         application (e.g. Resolume's own AJA output) had on it.
     *
     * @return Raw NTV2VideoFormat enum value (cast to int so callers
     *         that do not include libajantv2 headers can still handle
     *         it). Returns 0 (== NTV2_FORMAT_UNKNOWN) when no signal is
     *         present or the device is closed, kProbeIsOutput when the
     *         connector is (and stays) an output. Used at plugin InitGL
     *         time to negotiate raster dimensions before any GL resource
     *         is allocated.
     */
    static constexpr int kProbeIsOutput = -1;
    int ProbeInputFormat(int inputNumber = 1,
                         bool switchToReceive = true) noexcept;

    /**
     * @brief v0.41: true when the SDI output of the given connector is fed
     *        (through the routing matrix, possibly via CSC / mux widgets)
     *        by a FrameStore that is running an AutoCirculate playout.
     *        This plugin never plays out, so that is another application
     *        (Resolume's own AJA output, AJA ControlRoom, ...) using the
     *        connector as an output. v0.40 refused to switch ANY
     *        transmitting connector while any playout ran on the card,
     *        which also blocked an input whose connector merely sat in
     *        transmit mode while e.g. the HDMI output played.
     * @param bncZeroBased  connector / SDI output index (0 = SDI1).
     */
    bool SdiOutputCarriesForeignPlayout(int bncZeroBased) noexcept;

    /**
     * @brief v0.41: FrameStores (bit i = FrameStore i) whose output reaches
     *        the given SDI output widget, found by walking the routing
     *        matrix backwards a few widgets deep. 0 when nothing (or
     *        only black) feeds it.
     */
    uint32_t FrameStoresFeedingSdiOutput(int bncZeroBased) noexcept;

    /**
     * @brief v0.41: whether the card's crosspoint ROM allows routing the
     *        given input (an input number, InputId.h) straight into a
     *        FrameStore (an HDMI input also through the FrameStore's CSC).
     * @return 1 allowed, 0 not allowed, -1 the card cannot tell (no ROM,
     *         or a quad-link input, whose FrameStores are fixed).
     */
    int CanRouteInputToFrameStore(int inputNumber, int frameStoreZeroBased) noexcept;

    /**
     * @brief v0.40: bitmask of FrameStores (bit i = FrameStore i) whose
     *        AutoCirculate is not stopped: playouts only, or any use.
     *        Includes this process's own running captures; callers
     *        combine it with FrameStorePool, which already skips those.
     */
    uint32_t BusyFrameStores(bool playoutOnly) noexcept;

    /**
     * @brief Check whether the SDI input at the given 1-based channel
     *        is carrying a 3G-SDI Level B signal on the cable.
     *
     * The hardware sample point is taken BEFORE any internal B→A
     * conversion this class enables in Open(), so the answer reflects
     * what is physically on the wire. Useful as diagnostic information
     * to surface in logs ("the cable is Level B; we converted to A
     * internally"). Returns false when the device is closed, the
     * input is not SDI, or the input is not currently locked.
     * v0.42: takes an input number (the first link of a quad-link input).
     */
    bool ProbeIs3GbInput(int inputNumber = 1) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    Status                m_status;
};

} // namespace ajainput
