// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Regression tests for VideoFormat::TryDecodeNTV2Format. Every other
// piece of capture logic — raster-change detection, format probe in
// HandleNoFrameAvailable, the Format read-only inspector text — runs
// the input video format through this decoder, so wrong answers here
// cascade everywhere.
//
// Covered:
// - A handful of known supported formats decode to the expected
//   width / height / interlaced.
// - NTV2_FORMAT_UNKNOWN returns false without writing to the output.
// - An interlaced format decodes successfully (the pipeline accepts
//   it; bob de-interlacing handles the rest at shader time).
//
// We deliberately do not enumerate every supported format the SDK
// knows about; that would lock us to a specific SDK version. The
// asserts here pick the formats the plugin actually negotiates in
// production.

#include "test_framework.h"
#include "VideoFormat.h"

#include "ntv2vpid.h"
#include "PixelFormat.h"

#include "ntv2enums.h"

#include <string>

TEST_CASE(VideoFormat, Decode1080p59_94)
{
    ajainput::CapturedFormat fmt;
    const bool ok = ajainput::TryDecodeNTV2Format(
        static_cast<int>(NTV2_FORMAT_1080p_5994_A), fmt);

    ASSERT_TRUE(ok);
    EXPECT_EQ(fmt.width,        std::size_t(1920));
    EXPECT_EQ(fmt.height,       std::size_t(1080));
    EXPECT_FALSE(fmt.isInterlaced);
    EXPECT_TRUE(fmt.FramesPerSecond() > 59.0);
    EXPECT_TRUE(fmt.FramesPerSecond() < 60.0);
    TEST_RETURN_OK;
}

TEST_CASE(VideoFormat, Decode720p60)
{
    ajainput::CapturedFormat fmt;
    const bool ok = ajainput::TryDecodeNTV2Format(
        static_cast<int>(NTV2_FORMAT_720p_6000), fmt);

    ASSERT_TRUE(ok);
    EXPECT_EQ(fmt.width,  std::size_t(1280));
    EXPECT_EQ(fmt.height, std::size_t(720));
    EXPECT_FALSE(fmt.isInterlaced);
    TEST_RETURN_OK;
}

TEST_CASE(VideoFormat, DecodeUnknownReturnsFalse)
{
    ajainput::CapturedFormat fmt;
    fmt.width  = 0xDEAD;
    fmt.height = 0xBEEF;

    const bool ok = ajainput::TryDecodeNTV2Format(
        static_cast<int>(NTV2_FORMAT_UNKNOWN), fmt);
    EXPECT_FALSE(ok);

    // On failure, the helper documents the output is left untouched.
    EXPECT_EQ(fmt.width,  std::size_t(0xDEAD));
    EXPECT_EQ(fmt.height, std::size_t(0xBEEF));
    TEST_RETURN_OK;
}

TEST_CASE(VideoFormat, Decode1080i5994Interlaced)
{
    ajainput::CapturedFormat fmt;
    const bool ok = ajainput::TryDecodeNTV2Format(
        static_cast<int>(NTV2_FORMAT_1080i_5994), fmt);

    ASSERT_TRUE(ok);
    EXPECT_EQ(fmt.width,  std::size_t(1920));
    EXPECT_EQ(fmt.height, std::size_t(1080));
    EXPECT_TRUE(fmt.isInterlaced);
    TEST_RETURN_OK;
}

TEST_CASE(VideoFormat, UYVYBytesMatchesRaster)
{
    ajainput::CapturedFormat fmt;
    fmt.width  = 1920;
    fmt.height = 1080;
    // UYVY = 2 bytes per pixel.
    EXPECT_EQ(fmt.UYVYBytes(),
              std::size_t(1920) * std::size_t(1080) * std::size_t(2));
    TEST_RETURN_OK;
}

// ---------------------------------------------------------------------
// v0.29.2 — V210 packed 10-bit YCbCr math. These tests pin down the
// constants the capture-side buffer sizing and texture-width allocation
// both rely on. Numbers from the SMPTE v210 spec: 6 image pixels pack
// into 4 words (16 bytes); rows align on a 48-pixel boundary.
// ---------------------------------------------------------------------

TEST_CASE(PixelFormat, V210FrameSize1080p)
{
    // 1920 is already a multiple of 48 (1920 / 48 = 40), so the aligned
    // width is the same. 1920 * 8 / 3 = 5120 bytes/row × 1080 rows.
    EXPECT_EQ(ajainput::V210FrameSize(1920, 1080), std::size_t(5'529'600));
    TEST_RETURN_OK;
}

TEST_CASE(PixelFormat, V210FrameSize720p)
{
    // 1280 / 48 = 26.666..., so aligned width = 27 * 48 = 1296. Bytes
    // per row = 1296 * 8 / 3 = 3456. Rows = 720.
    EXPECT_EQ(ajainput::V210FrameSize(1280, 720),
              std::size_t(3456) * std::size_t(720));
    TEST_RETURN_OK;
}

TEST_CASE(PixelFormat, V210IsLargerThanUYVY)
{
    // V210 is always larger than UYVY for the same raster: 10-bit
    // packed is roughly 33% more bytes per pixel than 8-bit packed.
    EXPECT_TRUE(ajainput::V210FrameSize(1920, 1080)
                > ajainput::UYVYFrameSize(1920, 1080));
    TEST_RETURN_OK;
}

TEST_CASE(PixelFormat, TextureWidthTexels1080pUYVY)
{
    // UYVY: 2 image pixels per RGBA8 texel.
    EXPECT_EQ(ajainput::TextureWidthTexels(ajainput::PixelKind::UYVY, 1920),
              std::size_t(960));
    TEST_RETURN_OK;
}

TEST_CASE(PixelFormat, TextureWidthTexels1080pV210)
{
    // V210: 4 words per group of 6 pixels. 1920 / 6 * 4 = 1280.
    EXPECT_EQ(ajainput::TextureWidthTexels(ajainput::PixelKind::V210, 1920),
              std::size_t(1280));
    TEST_RETURN_OK;
}

TEST_CASE(PixelFormat, FrameSizeDispatches)
{
    // FrameSize() must agree with the per-kind helpers.
    EXPECT_EQ(ajainput::FrameSize(ajainput::PixelKind::UYVY, 1920, 1080),
              ajainput::UYVYFrameSize(1920, 1080));
    EXPECT_EQ(ajainput::FrameSize(ajainput::PixelKind::V210, 1920, 1080),
              ajainput::V210FrameSize(1920, 1080));
    TEST_RETURN_OK;
}

// v0.40: printed names, which the inspector shows verbatim.
TEST_CASE(VideoFormat, PrettyNames)
{
    struct Case { NTV2VideoFormat fmt; const char* name; };
    const Case cases[] = {
        { NTV2_FORMAT_1080p_5994_A,     "1080p59.94" },
        { NTV2_FORMAT_1080p_6000_A,     "1080p60" },
        { NTV2_FORMAT_720p_5000,        "720p50" },
        { NTV2_FORMAT_1080i_5994,       "1080i59.94" },
        { NTV2_FORMAT_1080i_5000,       "1080i50" },
        { NTV2_FORMAT_525_5994,         "525i59.94" },
        { NTV2_FORMAT_625_5000,         "625i50" },
        { NTV2_FORMAT_1080p_2K_2400,    "DCI 1080p24" },
        { NTV2_FORMAT_3840x2160p_5000,  "2160p50" },
        { NTV2_FORMAT_4096x2160p_2400,  "DCI 2160p24" },
        { NTV2_FORMAT_525psf_2997,      "525p29.97" },
    };
    for (const Case& c : cases)
    {
        ajainput::CapturedFormat f;
        EXPECT_TRUE(ajainput::TryDecodeNTV2Format(static_cast<int>(c.fmt), f));
        EXPECT_EQ(f.name, std::string(c.name));
    }
    TEST_RETURN_OK;
}

// v0.42: a quad-link input's 4K picture from the format its links report
// (CNTV2DemoCommon::Get4KInputFormat).
TEST_CASE(VideoFormat, QuadLinkFormatFromLink)
{
    using ajainput::QuadLinkFormatFromLink;
    EXPECT_EQ(QuadLinkFormatFromLink(NTV2_FORMAT_1080p_5000_A),
              static_cast<int>(NTV2_FORMAT_4x1920x1080p_5000));
    EXPECT_EQ(QuadLinkFormatFromLink(NTV2_FORMAT_1080p_5994_B),   // Level B link
              static_cast<int>(NTV2_FORMAT_4x1920x1080p_5994));
    EXPECT_EQ(QuadLinkFormatFromLink(NTV2_FORMAT_1080p_2K_2400),
              static_cast<int>(NTV2_FORMAT_4x2048x1080p_2400));
    EXPECT_EQ(QuadLinkFormatFromLink(NTV2_FORMAT_1080psf_2398),
              static_cast<int>(NTV2_FORMAT_4x1920x1080psf_2398));
    EXPECT_EQ(QuadLinkFormatFromLink(NTV2_FORMAT_1080i_5000), 0);
    EXPECT_EQ(QuadLinkFormatFromLink(NTV2_FORMAT_720p_5000), 0);
    EXPECT_EQ(QuadLinkFormatFromLink(NTV2_FORMAT_UNKNOWN), 0);
    // Single-link 4K formats are not quad-link pictures.
    EXPECT_EQ(QuadLinkFormatFromLink(NTV2_FORMAT_3840x2160p_5000), 0);

    // A 2SI link with a payload ID already reports the 4K picture (the
    // SDK returns the VPID's format): it passes through, Level B to A.
    EXPECT_EQ(QuadLinkFormatFromLink(NTV2_FORMAT_4x1920x1080p_5000),
              static_cast<int>(NTV2_FORMAT_4x1920x1080p_5000));
    EXPECT_EQ(QuadLinkFormatFromLink(NTV2_FORMAT_4x2048x1080p_2400),
              static_cast<int>(NTV2_FORMAT_4x2048x1080p_2400));
    EXPECT_EQ(QuadLinkFormatFromLink(NTV2_FORMAT_4x1920x1080p_5994_B),
              static_cast<int>(NTV2_FORMAT_4x1920x1080p_5994));
    {
        // Quad-link 2SI 2160p50 Level A payload ID, link 1.
        const CNTV2VPID vpid(0x97C98005);
        EXPECT_TRUE(vpid.IsValid());
        EXPECT_TRUE(vpid.IsStandardTwoSampleInterleave());
        EXPECT_EQ(static_cast<int>(vpid.GetVideoFormat()),
                  static_cast<int>(NTV2_FORMAT_4x1920x1080p_5000));
        EXPECT_EQ(QuadLinkFormatFromLink(vpid.GetVideoFormat()),
                  static_cast<int>(NTV2_FORMAT_4x1920x1080p_5000));
    }

    // The 4K picture decodes to a UHD / DCI raster.
    ajainput::CapturedFormat f;
    EXPECT_TRUE(ajainput::TryDecodeNTV2Format(
        QuadLinkFormatFromLink(NTV2_FORMAT_1080p_5000_A), f));
    EXPECT_EQ(f.width, static_cast<std::size_t>(3840));
    EXPECT_EQ(f.height, static_cast<std::size_t>(2160));
    EXPECT_TRUE(ajainput::Is4KRaster(f));
    TEST_RETURN_OK;
}
