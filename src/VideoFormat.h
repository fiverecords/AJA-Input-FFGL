// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    VideoFormat.h
 * @brief   Pipeline-agnostic representation of an SDI input format.
 *
 * The capture path talks to libajantv2 with its NTV2VideoFormat enum, but
 * the GL/render side of the plugin doesn't need to know about that. This
 * module decodes an NTV2 format value into a plain struct (width, height,
 * frame rate, interlace flag) so the rest of the codebase can size GL
 * resources, set shader uniforms, and log human-readable names without
 * pulling AJA SDK headers everywhere.
 *
 * The decoder accepts the raw integer value of NTV2VideoFormat. Implementing
 * it as `int` lets this header be included from translation units that
 * don't link against libajantv2 (for instance the GL-only ones); the
 * implementation file converts via the typed enum internally.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ajainput
{

/// @brief Resolved description of a captured video format.
struct CapturedFormat
{
    std::size_t width            = 1280;
    std::size_t height           = 720;
    /// Frame rate expressed as a rational number to preserve 1001-based
    /// rates exactly (e.g. 60000/1001 for "59.94 Hz").
    uint32_t    fpsNumerator     = 60;
    uint32_t    fpsDenominator   = 1;
    bool        isInterlaced     = false;
    /// Raw NTV2VideoFormat enum value, for logging and round-tripping.
    int         ntv2FormatRaw    = 0;
    /// Human-readable name, e.g. "1080p59.94" or "720p60".
    std::string name;

    /// Convenience: frame rate as a real number, for diagnostic display.
    double FramesPerSecond() const noexcept
    {
        if (fpsDenominator == 0) return 0.0;
        return double(fpsNumerator) / double(fpsDenominator);
    }

    /// Convenience: bytes occupied by one full UYVY frame at this size.
    std::size_t UYVYBytes() const noexcept
    {
        // UYVY packs two pixels per 4 bytes (Y, Cb, Y, Cr).
        return width * height * 2;
    }
};

/**
 * @brief Decode an NTV2VideoFormat (raw int) into a CapturedFormat.
 *
 * @param  ntv2FormatRaw  Numeric value of the NTV2VideoFormat enum.
 * @param  out            Filled in on success. Untouched on failure.
 * @return True when the value corresponds to a recognised format AND the
 *         pipeline is prepared to ingest it (raster up to 4096x2160,
 *         progressive or interlaced). False on NTV2_FORMAT_UNKNOWN,
 *         quad-link / 8K rasters, or anything else outside the set
 *         declared in the implementation table.
 *
 * Interlaced formats are accepted and de-interlaced at render time
 * (YADIF on UYVY, bob on v210). Returning false from here is what drives
 * the AJACaptureSource state machine into NoSignal.
 */
bool TryDecodeNTV2Format(int ntv2FormatRaw, CapturedFormat& out);

/**
 * @brief Remap a 3G Level B NTV2VideoFormat value to its Level A equivalent.
 *
 * 3G-SDI Level B carries the picture as two 1.485 Gbps sub-streams
 * multiplexed at half wall-clock rate; the SDK reports the *wire* rate
 * (e.g. NTV2_FORMAT_1080p_6000_B decodes to NTV2_FRAMERATE_3000) which
 * misleads any code that asks "how fast is this signal really running".
 * The capture path always engages Level B→A conversion on the receiver
 * before locking, so what the FrameStore ends up programmed against is
 * the corresponding Level A format at the true display rate
 * (NTV2_FORMAT_1080p_6000_A → 60 Hz).
 *
 * This helper performs that 1-to-1 enum substitution. Pass any NTV2
 * format raw value; if it is a Level B variant the matching Level A
 * value is returned, otherwise the input is returned unchanged. Callers
 * that read GetInputVideoFormat() should funnel the result through this
 * function before configuring the FrameStore or decoding the timing.
 *
 * @param ntv2FormatRaw  Numeric value of the NTV2VideoFormat enum.
 * @return The Level A equivalent if Level B, else the value unchanged.
 */
int MapLevelBFormatToLevelA(int ntv2FormatRaw) noexcept;

/**
 * @brief v0.42: the 4K format a quad-link input carries, from the format
 *        its first link reports.
 *
 * A link with a 2160-line quad-link payload ID (2SI, SMPTE ST 425-5)
 * already reports the 4K picture (a 4x1920x1080 / 4x2048x1080 format,
 * from the VPID); it is returned as is, Level B folded to Level A.
 * Otherwise each link is a 1080p or 2K picture, mapped with the same
 * table as AJA's CNTV2DemoCommon::Get4KInputFormat (demos/
 * ntv2democommon.cpp): 1080p -> 4x1920x1080p (UHD), 2K 1080p ->
 * 4x2048x1080p (4K DCI), 1080psf -> 4x1920x1080psf. Level B link formats
 * are folded to Level A first.
 *
 * @return The quad-link 4K format, or 0 (NTV2_FORMAT_UNKNOWN) when the
 *         link format is not one a quad-link 4K picture is made of
 *         (interlaced, 720p, SD, a 3840 / 4096 single-link 4K format,
 *         unknown).
 */
int QuadLinkFormatFromLink(int linkFormatRaw) noexcept;

/// @return true for a 4K raster (2160 lines), the signals that need 12G
///         routing or several FrameStores.
inline bool Is4KRaster(const CapturedFormat& f) noexcept
{
    return f.height >= 2160;
}

/**
 * @brief Build a CapturedFormat for the synthetic-generator fallback.
 *
 * Returns a fixed 1280x720 progressive 60 Hz description, matching what
 * the SyntheticFrameGenerator produces. Used by AJAInputPlugin when no AJA
 * device is open so the GL pipeline has a real size to allocate against.
 */
CapturedFormat SyntheticFallbackFormat() noexcept;

} // namespace ajainput
