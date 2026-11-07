// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    SyntheticFrameGenerator.cpp
 * @brief   Implementation of the development-time UYVY frame source.
 *
 * The generator emits a SMPTE-style 75% color-bar pattern with an animated
 * indicator strip in the bottom quarter. All pixel data is produced in
 * UYVY 4:2:2 with BT.709 limited-range colorimetry, matching what the AJA
 * capture path is expected to deliver in production.
 *
 * Performance is intentionally not optimised: this code runs at most a few
 * hundred times per second on a single thread and is in service of a
 * developer feedback loop, not a release-quality hot path. Clarity of the
 * pixel-format work takes priority.
 */

#include "SyntheticFrameGenerator.h"
#include "PixelFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ajainput
{
namespace
{

/// @brief Clamp a float to [0, 1].
inline float Saturate(float v)
{
    return std::max(0.0f, std::min(1.0f, v));
}

/// @brief One quantised, limited-range 8-bit YCbCr triplet.
struct YCbCr8
{
    uint8_t y;
    uint8_t cb;
    uint8_t cr;
};

/**
 * @brief Convert non-linear R'G'B' in [0,1] to 8-bit BT.709 limited-range YCbCr.
 *
 * The matrix below is the standard ITU-R BT.709 luma/chroma definition.
 * The output is then offset and scaled into the 8-bit limited-range studio
 * encoding used by SDI/HDMI: Y in [16, 235], CbCr in [16, 240].
 */
YCbCr8 RgbToYCbCr_BT709_Limited(float r, float g, float b)
{
    r = Saturate(r);
    g = Saturate(g);
    b = Saturate(b);

    const float y  =  0.2126f * r + 0.7152f * g + 0.0722f * b;
    const float cb = -0.1146f * r - 0.3854f * g + 0.5000f * b;
    const float cr =  0.5000f * r - 0.4542f * g - 0.0458f * b;

    auto quantiseY = [](float v) -> uint8_t {
        const float scaled = 16.0f + 219.0f * v;
        return static_cast<uint8_t>(std::lround(std::clamp(scaled, 0.0f, 255.0f)));
    };
    auto quantiseC = [](float v) -> uint8_t {
        const float scaled = 128.0f + 224.0f * v;
        return static_cast<uint8_t>(std::lround(std::clamp(scaled, 0.0f, 255.0f)));
    };

    return YCbCr8{
        quantiseY(y),
        quantiseC(cb),
        quantiseC(cr)
    };
}

/// @brief Linear RGB definitions of the seven SMPTE 75% color bars.
constexpr std::array<std::array<float, 3>, 7> kBarRGB = { {
    {0.75f, 0.75f, 0.75f},  // White
    {0.75f, 0.75f, 0.00f},  // Yellow
    {0.00f, 0.75f, 0.75f},  // Cyan
    {0.00f, 0.75f, 0.00f},  // Green
    {0.75f, 0.00f, 0.75f},  // Magenta
    {0.75f, 0.00f, 0.00f},  // Red
    {0.00f, 0.00f, 0.75f}   // Blue
} };

constexpr YCbCr8 kBlack = { 16, 128, 128 };

} // namespace

// ============================================================================

SyntheticFrameGenerator::SyntheticFrameGenerator(std::size_t widthPixels,
                                                 std::size_t heightPixels)
    : m_width(widthPixels)
    , m_height(heightPixels)
{
    // UYVY requires an even pixel width because two pixels share each chroma
    // pair. Round down silently rather than throw; production code should
    // validate this upstream.
    if (m_width & 1u)
        m_width -= 1u;

    m_buffer.assign(UYVYFrameSize(m_width, m_height), 0);
}

void SyntheticFrameGenerator::GenerateFrame(float timeSeconds,
                                            int   inputIndex)
{
    // Animation hooks are reserved for future use; current pattern is
    // static colour bars on top, black on bottom.
    (void)timeSeconds;
    (void)inputIndex;

    // Precompute the YCbCr value for each of the seven bars. Doing this once
    // per frame (rather than per pixel) keeps the inner loop tight.
    std::array<YCbCr8, 7> barYCbCr{};
    for (std::size_t i = 0; i < kBarRGB.size(); ++i)
    {
        const auto& rgb = kBarRGB[i];
        barYCbCr[i] = RgbToYCbCr_BT709_Limited(rgb[0], rgb[1], rgb[2]);
    }

    // Geometry: top 75% of frame is the bar pattern, bottom 25% is black.
    const std::size_t barsBottomRow = (m_height * 3u) / 4u;

    // Iterate row by row. Memory layout is top-to-bottom (row 0 = top), which
    // matches what the AJA capture path will deliver. The shader is in charge
    // of flipping for OpenGL's default bottom-left texture origin.
    const std::size_t macropixelsPerRow = m_width / 2u;
    const std::size_t bytesPerRow       = macropixelsPerRow * kUYVYBytesPerMacropixel;

    for (std::size_t row = 0; row < m_height; ++row)
    {
        const bool inBars = (row < barsBottomRow);
        uint8_t* rowPtr = m_buffer.data() + row * bytesPerRow;

        for (std::size_t mp = 0; mp < macropixelsPerRow; ++mp)
        {
            // Image-space x coordinates of the two pixels in this macropixel.
            const std::size_t x0 = mp * 2u;
            const std::size_t x1 = x0 + 1u;

            YCbCr8 c0 = kBlack;
            YCbCr8 c1 = kBlack;

            if (inBars)
            {
                // Pick the bar each pixel belongs to. Both x0 and x1 will
                // usually map to the same bar; only at bar boundaries do
                // they differ, and even then the result is visually correct.
                const std::size_t bar0 = std::min<std::size_t>(6u, (x0 * 7u) / m_width);
                const std::size_t bar1 = std::min<std::size_t>(6u, (x1 * 7u) / m_width);
                c0 = barYCbCr[bar0];
                c1 = barYCbCr[bar1];
            }

            // 4:2:2 subsampling: the two pixels share one chroma pair. We
            // average the chromas to avoid a hard hue shift at colour
            // boundaries, then keep each pixel's own luma.
            const uint8_t cb = static_cast<uint8_t>((c0.cb + c1.cb) >> 1);
            const uint8_t cr = static_cast<uint8_t>((c0.cr + c1.cr) >> 1);

            const uint32_t mp32 = PackUYVY(c0.y, c1.y, cb, cr);
            uint8_t* dst = rowPtr + mp * kUYVYBytesPerMacropixel;
            dst[0] = static_cast<uint8_t>( mp32        & 0xFFu);
            dst[1] = static_cast<uint8_t>((mp32 >>  8) & 0xFFu);
            dst[2] = static_cast<uint8_t>((mp32 >> 16) & 0xFFu);
            dst[3] = static_cast<uint8_t>((mp32 >> 24) & 0xFFu);
        }
    }
}

} // namespace ajainput
