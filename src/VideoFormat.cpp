// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    VideoFormat.cpp
 * @brief   Decoder from NTV2VideoFormat to a pipeline-agnostic description.
 */

#include "VideoFormat.h"

#include "ntv2enums.h"
#include "ntv2utils.h"

#include <cstdio>

namespace ajainput
{
namespace
{

/// @brief Frame-rate decoder. Maps the NTV2FrameRate enum to a rational
///        (numerator, denominator) pair so 1001-based rates survive
///        unmangled.
bool DecodeFrameRate(NTV2FrameRate rate,
                     uint32_t&     outNumerator,
                     uint32_t&     outDenominator) noexcept
{
    switch (rate)
    {
        case NTV2_FRAMERATE_2398:  outNumerator = 24000; outDenominator = 1001; return true;
        case NTV2_FRAMERATE_2400:  outNumerator = 24;    outDenominator = 1;    return true;
        case NTV2_FRAMERATE_2500:  outNumerator = 25;    outDenominator = 1;    return true;
        case NTV2_FRAMERATE_2997:  outNumerator = 30000; outDenominator = 1001; return true;
        case NTV2_FRAMERATE_3000:  outNumerator = 30;    outDenominator = 1;    return true;
        case NTV2_FRAMERATE_4795:  outNumerator = 48000; outDenominator = 1001; return true;
        case NTV2_FRAMERATE_4800:  outNumerator = 48;    outDenominator = 1;    return true;
        case NTV2_FRAMERATE_5000:  outNumerator = 50;    outDenominator = 1;    return true;
        case NTV2_FRAMERATE_5994:  outNumerator = 60000; outDenominator = 1001; return true;
        case NTV2_FRAMERATE_6000:  outNumerator = 60;    outDenominator = 1;    return true;
        default:                                                                return false;
    }
}

/// @brief Build a short, human-readable identifier in the style broadcast
///        engineers expect ("1080p59.94", "720p60", "1080i59.94", etc.).
///        The name is purely informational; everything functional comes
///        from the typed fields of CapturedFormat. The width is implied
///        by the height in the broadcast vocabulary so it is not part of
///        the printed label.
///
///        Interlaced formats are labelled by their FIELD rate (the
///        SMPTE convention: "1080i59.94" means 59.94 fields/sec), which
///        is twice the frame rate that AJA reports from
///        GetNTV2FrameRateFromVideoFormat. The doubling is applied here
///        so the printed label matches what cameras and broadcast
///        engineers actually call the format. The numeric Hz reported
///        elsewhere through CapturedFormat::FramesPerSecond() remains
///        the AutoCirculate buffer delivery rate.
///
///        v0.40: the DCI rasters (2048x1080, 4096x2160) get a "DCI "
///        prefix so they are not shown with the same label as the
///        1920/3840 HD/UHD rasters, and SD uses the line-count names the
///        rest of the docs use (525 for the 486-line NTSC raster, 625 for
///        the 576-line PAL raster).
std::string FormatPrettyName(std::size_t width,
                             std::size_t height,
                             bool        isInterlaced,
                             uint32_t    fpsNumerator,
                             uint32_t    fpsDenominator) noexcept
{
    char buf[40];
    const char p = isInterlaced ? 'i' : 'p';

    const bool dci = (width == 2048 && height == 1080) ||
                     (width == 4096 && height == 2160);
    const char* prefix = dci ? "DCI " : "";
    std::size_t lines  = height;
    if (height == 486) lines = 525;   // NTSC raster, interlaced or PsF
    if (height == 576) lines = 625;   // PAL raster, interlaced or PsF

    const uint32_t labelNum = isInterlaced ? fpsNumerator * 2 : fpsNumerator;
    const uint32_t labelDen = fpsDenominator;
    const double labelFps = (labelDen > 0)
                            ? double(labelNum) / double(labelDen)
                            : 0.0;

    if (labelDen == 1)
        std::snprintf(buf, sizeof(buf), "%s%zu%c%u", prefix, lines, p, labelNum);
    else
        std::snprintf(buf, sizeof(buf), "%s%zu%c%.2f", prefix, lines, p, labelFps);
    return std::string(buf);
}

} // namespace

int MapLevelBFormatToLevelA(int ntv2FormatRaw) noexcept
{
    const NTV2VideoFormat fmt = static_cast<NTV2VideoFormat>(ntv2FormatRaw);
    switch (fmt)
    {
        // HD 1080p Level B → Level A
        case NTV2_FORMAT_1080p_5000_B:      return static_cast<int>(NTV2_FORMAT_1080p_5000_A);
        case NTV2_FORMAT_1080p_5994_B:      return static_cast<int>(NTV2_FORMAT_1080p_5994_A);
        case NTV2_FORMAT_1080p_6000_B:      return static_cast<int>(NTV2_FORMAT_1080p_6000_A);
        // 2K 1080p Level B → Level A
        case NTV2_FORMAT_1080p_2K_4795_B:   return static_cast<int>(NTV2_FORMAT_1080p_2K_4795_A);
        case NTV2_FORMAT_1080p_2K_4800_B:   return static_cast<int>(NTV2_FORMAT_1080p_2K_4800_A);
        case NTV2_FORMAT_1080p_2K_5000_B:   return static_cast<int>(NTV2_FORMAT_1080p_2K_5000_A);
        case NTV2_FORMAT_1080p_2K_5994_B:   return static_cast<int>(NTV2_FORMAT_1080p_2K_5994_A);
        case NTV2_FORMAT_1080p_2K_6000_B:   return static_cast<int>(NTV2_FORMAT_1080p_2K_6000_A);
        // Higher rasters (3840x2160_*_B, 4096x2160_*_B, 4x1920x1080_*_B,
        // 4x2048x1080_*_B, etc.) are also Level B variants per
        // NTV2_VIDEO_FORMAT_IS_B but Kona X reaches them via quad/12G
        // plumbing not yet wired in the capture path, so leave them
        // unchanged and let the downstream consumer reject them rather
        // than silently misroute.
        default:                            return ntv2FormatRaw;
    }
}

int QuadLinkFormatFromLink(int linkFormatRaw) noexcept
{
    // A link whose payload ID names a 2160-line quad-link standard (SMPTE
    // ST 425-5, i.e. any compliant 2SI source) already reports the 4K
    // picture: CNTV2Card::GetSDIInputVideoFormat returns the VPID's own
    // format (ntv2register.cpp), a 4x1920x1080 / 4x2048x1080 one. Pass
    // those through, Level B folded to Level A as for the links below.
    const auto raw = static_cast<NTV2VideoFormat>(linkFormatRaw);
    switch (raw)
    {
        case NTV2_FORMAT_4x1920x1080p_5000_B: return NTV2_FORMAT_4x1920x1080p_5000;
        case NTV2_FORMAT_4x1920x1080p_5994_B: return NTV2_FORMAT_4x1920x1080p_5994;
        case NTV2_FORMAT_4x1920x1080p_6000_B: return NTV2_FORMAT_4x1920x1080p_6000;
        case NTV2_FORMAT_4x2048x1080p_5000_B: return NTV2_FORMAT_4x2048x1080p_5000;
        case NTV2_FORMAT_4x2048x1080p_5994_B: return NTV2_FORMAT_4x2048x1080p_5994;
        case NTV2_FORMAT_4x2048x1080p_6000_B: return NTV2_FORMAT_4x2048x1080p_6000;
        case NTV2_FORMAT_4x2048x1080p_4795_B: return NTV2_FORMAT_4x2048x1080p_4795;
        case NTV2_FORMAT_4x2048x1080p_4800_B: return NTV2_FORMAT_4x2048x1080p_4800;
        default: break;
    }
    if (raw >= NTV2_FORMAT_FIRST_4K_DEF_FORMAT && raw < NTV2_FORMAT_END_4K_DEF_FORMATS)
        return linkFormatRaw;

    // A link without a payload ID (or a square-division source, whose
    // links are plain 1080p pictures) reports its own format:
    // CNTV2DemoCommon::Get4KInputFormat, after folding Level B to A (its
    // table lists both spellings).
    switch (static_cast<NTV2VideoFormat>(MapLevelBFormatToLevelA(linkFormatRaw)))
    {
        case NTV2_FORMAT_1080psf_2398:     return NTV2_FORMAT_4x1920x1080psf_2398;
        case NTV2_FORMAT_1080psf_2400:     return NTV2_FORMAT_4x1920x1080psf_2400;
        case NTV2_FORMAT_1080p_2398:       return NTV2_FORMAT_4x1920x1080p_2398;
        case NTV2_FORMAT_1080p_2400:       return NTV2_FORMAT_4x1920x1080p_2400;
        case NTV2_FORMAT_1080p_2500:       return NTV2_FORMAT_4x1920x1080p_2500;
        case NTV2_FORMAT_1080p_2997:       return NTV2_FORMAT_4x1920x1080p_2997;
        case NTV2_FORMAT_1080p_3000:       return NTV2_FORMAT_4x1920x1080p_3000;
        case NTV2_FORMAT_1080p_5000_A:     return NTV2_FORMAT_4x1920x1080p_5000;
        case NTV2_FORMAT_1080p_5994_A:     return NTV2_FORMAT_4x1920x1080p_5994;
        case NTV2_FORMAT_1080p_6000_A:     return NTV2_FORMAT_4x1920x1080p_6000;
        case NTV2_FORMAT_1080p_2K_2398:    return NTV2_FORMAT_4x2048x1080p_2398;
        case NTV2_FORMAT_1080p_2K_2400:    return NTV2_FORMAT_4x2048x1080p_2400;
        case NTV2_FORMAT_1080p_2K_2500:    return NTV2_FORMAT_4x2048x1080p_2500;
        case NTV2_FORMAT_1080p_2K_2997:    return NTV2_FORMAT_4x2048x1080p_2997;
        case NTV2_FORMAT_1080p_2K_3000:    return NTV2_FORMAT_4x2048x1080p_3000;
        case NTV2_FORMAT_1080p_2K_5000_A:  return NTV2_FORMAT_4x2048x1080p_5000;
        case NTV2_FORMAT_1080p_2K_5994_A:  return NTV2_FORMAT_4x2048x1080p_5994;
        case NTV2_FORMAT_1080p_2K_6000_A:  return NTV2_FORMAT_4x2048x1080p_6000;
        default:                           return NTV2_FORMAT_UNKNOWN;
    }
}

bool TryDecodeNTV2Format(int ntv2FormatRaw, CapturedFormat& out)
{
    // Normalise any 3G Level B value to its Level A equivalent before
    // reading rate/raster from the SDK. This is the single point in the
    // pipeline that handles the Level B "wire rate vs display rate"
    // discrepancy: NTV2_FORMAT_1080p_6000_B reports NTV2_FRAMERATE_3000
    // because Level B's wire is 30 Hz dual-stream, but the capture
    // receiver delivers 60 Hz progressive after the Level B→A conversion
    // ProbeInputFormat / TryConfigureChannel engage. By remapping here
    // we hand every caller a Level A enum, whose GetNTV2FrameRateFromVideoFormat
    // returns the true display rate. See VideoFormat.h::MapLevelBFormatToLevelA
    // for the rationale.
    const int normalisedRaw = MapLevelBFormatToLevelA(ntv2FormatRaw);
    const NTV2VideoFormat fmt = static_cast<NTV2VideoFormat>(normalisedRaw);
    if (fmt == NTV2_FORMAT_UNKNOWN)
        return false;

    // Active raster width/height come straight from the SDK; this is the
    // single source of truth for the pixel grid we will allocate
    // textures, PBOs and host buffers against.
    const ULWord width  = ::GetDisplayWidth(fmt);
    const ULWord height = ::GetDisplayHeight(fmt);
    if (width == 0 || height == 0)
        return false;

    const NTV2FrameRate rate = ::GetNTV2FrameRateFromVideoFormat(fmt);
    uint32_t fpsNum = 0, fpsDen = 0;
    if (!DecodeFrameRate(rate, fpsNum, fpsDen))
        return false;

    const bool isProgressive = ::IsProgressivePicture(fmt);

    // Pipeline limits: up to and including 4K DCI (4096x2160). 8K
    // (anything beyond that, e.g. 4x3840x2160 quad-link or
    // 7680x4320 single-link) is excluded for v0.15 to keep host
    // memory predictable and because the channel-bonding setup quad-
    // link 8K requires is not yet wired in the capture path.
    //
    // Interlaced formats are accepted. The capture delivers them either
    // as woven frames or, on the UYVY path, field by field (v0.32 AC
    // field mode) into a woven accumulator; the render shader
    // de-interlaces (YADIF on UYVY since v0.35, bob on v210).
    if (width > 4096 || height > 2160)
        return false;

    out.width          = static_cast<std::size_t>(width);
    out.height         = static_cast<std::size_t>(height);
    out.fpsNumerator   = fpsNum;
    out.fpsDenominator = fpsDen;
    out.isInterlaced   = !isProgressive;
    out.ntv2FormatRaw  = normalisedRaw;
    out.name           = FormatPrettyName(out.width,
                                          out.height,
                                          out.isInterlaced,
                                          out.fpsNumerator,
                                          out.fpsDenominator);
    return true;
}

CapturedFormat SyntheticFallbackFormat() noexcept
{
    CapturedFormat f;
    f.width          = 1280;
    f.height         = 720;
    f.fpsNumerator   = 60;
    f.fpsDenominator = 1;
    f.isInterlaced   = false;
    f.ntv2FormatRaw  = static_cast<int>(NTV2_FORMAT_720p_6000);
    f.name           = "720p60";
    return f;
}

} // namespace ajainput
