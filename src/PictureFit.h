// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    PictureFit.h
 * @brief   v0.42 Resize: where in the clip the input's picture is drawn.
 *
 * An FFGL source cannot choose its own size: the host hands it a viewport
 * (in Resolume, the clip's size, which for a source is the composition's)
 * and the plugin draws into it. Up to v0.41 the picture was stretched over
 * the whole viewport, so an input whose aspect differs from the
 * composition's (SD 4:3, DCI 4K, a 16:10 composition) came out deformed.
 * The Resize option keeps the input's aspect, with the names and meanings
 * of Resolume's own clip Resize:
 *   - Fit:      the whole picture, as large as fits; transparent bars
 *               where it does not reach (default).
 *   - Fill:     the picture covers the clip, keeping its aspect; what
 *               overflows is cut.
 *   - Stretch:  the picture is stretched over the clip (up to v0.41).
 *   - Original: the picture at its own size, centred, not scaled.
 *
 * SD rasters (720 x 486 / 576) have non-square pixels: they are shown at
 * 4:3 (anamorphic 16:9 SD: use Stretch on a 16:9 composition).
 *
 * Pure logic, unit-tested (tests/test_PictureFit.cpp).
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ajainput
{

enum class ResizeMode : int
{
    Fit      = 0,
    Fill     = 1,
    Stretch  = 2,
    Original = 3
};

/// The Resize option value as a mode; a value no option has: Fit.
inline ResizeMode ResizeModeFromOption(float value) noexcept
{
    if (!(value >= -0.5f && value < 3.5f))   // also NaN
        return ResizeMode::Fit;
    switch (static_cast<int>(std::lround(value)))
    {
        case 1:  return ResizeMode::Fill;
        case 2:  return ResizeMode::Stretch;
        case 3:  return ResizeMode::Original;
        default: return ResizeMode::Fit;
    }
}

/// A rectangle in framebuffer pixels (GL viewport / scissor convention:
/// x, y of the lower-left corner).
struct PixelRect
{
    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;
};

inline bool operator==(const PixelRect& a, const PixelRect& b) noexcept
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
inline bool operator!=(const PixelRect& a, const PixelRect& b) noexcept
{
    return !(a == b);
}

/// SD raster (720 x 486 / 576 and smaller): non-square pixels, shown 4:3.
inline bool IsStandardDefinition(int width, int height) noexcept
{
    return width > 0 && height > 0 && width <= 720 && height <= 576;
}

/// The width the picture is shown at, for its height: its own width, or
/// 4:3 of its height for SD.
inline double DisplayWidth(int width, int height) noexcept
{
    return IsStandardDefinition(width, height)
               ? static_cast<double>(height) * 4.0 / 3.0
               : static_cast<double>(width);
}

/// Largest picture rect side when the GL's own limit is unknown.
constexpr int kDefaultMaxViewportDim = 16384;

/**
 * @brief The rect, in framebuffer pixels, the picture is drawn into.
 *
 * @param mode           Resize mode.
 * @param pictureWidth   captured raster (pixels); <= 0: unknown
 * @param pictureHeight  captured raster (lines);  <= 0: unknown
 * @param viewport       the host's viewport for this clip
 * @param maxDim         largest viewport side the GL takes
 *                       (GL_MAX_VIEWPORT_DIMS); <= 0: 16384. A larger
 *                       rect is shrunk evenly to fit (no distortion).
 *
 * Centred in the viewport; may extend beyond it (Fill, Original), the
 * caller cuts the draw to the viewport. Stretch, an unknown picture size
 * or an empty viewport give the viewport itself, and so does a rect
 * within one pixel of it in both directions (rounding would otherwise
 * leave a one-pixel bar).
 */
inline PixelRect PictureRect(ResizeMode mode, int pictureWidth, int pictureHeight,
                             const PixelRect& viewport, int maxDim = 0) noexcept
{
    if (mode == ResizeMode::Stretch || viewport.width <= 0 || viewport.height <= 0 ||
        pictureWidth <= 0 || pictureHeight <= 0)
        return viewport;

    const double pw = DisplayWidth(pictureWidth, pictureHeight);
    const double ph = static_cast<double>(pictureHeight);
    const double sx = static_cast<double>(viewport.width)  / pw;
    const double sy = static_cast<double>(viewport.height) / ph;
    double scale = 1.0;                                   // Original
    if (mode == ResizeMode::Fit)
        scale = std::min(sx, sy);
    else if (mode == ResizeMode::Fill)
        scale = std::max(sx, sy);

    double w = pw * scale;
    double h = ph * scale;
    const double limit = static_cast<double>(maxDim > 0 ? maxDim : kDefaultMaxViewportDim);
    if (w > limit || h > limit)
    {
        const double k = std::min(limit / w, limit / h);
        w *= k;
        h *= k;
    }

    const int iw = std::max(1, static_cast<int>(std::lround(w)));
    const int ih = std::max(1, static_cast<int>(std::lround(h)));
    if (std::abs(iw - viewport.width) <= 1 && std::abs(ih - viewport.height) <= 1)
        return viewport;

    PixelRect r;
    r.width  = iw;
    r.height = ih;
    r.x = viewport.x + static_cast<int>(std::floor((viewport.width  - iw) / 2.0));
    r.y = viewport.y + static_cast<int>(std::floor((viewport.height - ih) / 2.0));
    return r;
}

/// Whether r covers every pixel of area.
inline bool Covers(const PixelRect& r, const PixelRect& area) noexcept
{
    return r.x <= area.x && r.y <= area.y &&
           r.x + r.width  >= area.x + area.width &&
           r.y + r.height >= area.y + area.height;
}

/// The overlap of a and b (width / height 0 when they do not overlap).
inline PixelRect Intersect(const PixelRect& a, const PixelRect& b) noexcept
{
    const int x0 = std::max(a.x, b.x);
    const int y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.width,  b.x + b.width);
    const int y1 = std::min(a.y + a.height, b.y + b.height);
    PixelRect r;
    r.x      = x0;
    r.y      = y0;
    r.width  = std::max(0, x1 - x0);
    r.height = std::max(0, y1 - y0);
    return r;
}

} // namespace ajainput
