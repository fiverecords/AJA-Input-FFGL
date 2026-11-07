// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    PixelFormat.h
 * @brief   Pixel format definitions for the capture pipeline.
 *
 * The plugin's GPU upload path consumes packed YCbCr 4:2:2 buffers. UYVY is
 * the entry-level format used here because it matches one of the formats the
 * AJA NTV2 SDK can deliver via AutoCirculate (NTV2_FBF_8BIT_YCBCR), and
 * because every common SDI/HDMI raster size lays out cleanly in it.
 *
 * UYVY byte layout (4 bytes encode 2 horizontal pixels):
 *
 *     Byte 0   Byte 1   Byte 2   Byte 3
 *       U        Y0       V        Y1
 *      (Cb)   (luma 0)  (Cr)   (luma 1)
 *
 * Both pixels share the chroma samples (U, V), each carries its own luma.
 * On the GPU the buffer is sampled as a half-width RGBA8 texture: each
 * texel of 4 bytes corresponds to one "macropixel" (two image pixels). The
 * fragment shader picks Y0 or Y1 depending on the horizontal subpixel
 * position, and combines it with the shared U/V to produce the output
 * color.
 *
 * SDI/HDMI broadcast signals carry "limited range" YCbCr: Y in [16, 235]
 * and CbCr in [16, 240] for 8-bit. Conversion to full-range RGB requires
 * re-scaling these to [0, 1] before applying the colorimetry matrix. The
 * shader handles this; consumers of the buffer do not need to.
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace ajainput
{

/**
 * @brief Pixel format kind used by the capture pipeline.
 *
 * `UYVY` (8 bits per sample, 4:2:2) is the default and is what the
 * plugin has used since v0.18. The host buffer carries 4 bytes per
 * macropixel (one macropixel = 2 image pixels), and the GPU uploads
 * it as a half-width RGBA8 texture.
 *
 * `V210` (10 bits per sample, 4:2:2 packed) was added in v0.29.2 as
 * an opt-in for HDR sources that need the full code range. Three
 * 10-bit samples are packed into each 32-bit word with 2 bits of
 * zero padding; six image pixels (= 12 samples in UYVY order) fit
 * into 4 words (16 bytes). The GPU uploads it as RGBA8 too (each
 * RGBA8 texel = one 32-bit word's bytes), and the fragment shader
 * does the unpack via bit shifts. The capture-side AJA pixel format
 * is NTV2_FBF_10BIT_YCBCR.
 */
enum class PixelKind : int
{
    UYVY = 0,
    V210 = 1
};

/**
 * @brief Bytes per macropixel in UYVY (one macropixel = two image pixels).
 */
constexpr std::size_t kUYVYBytesPerMacropixel = 4;

/**
 * @brief Compute the byte size of an UYVY frame buffer.
 * @param widthPixels   Frame width in image pixels (must be even).
 * @param heightPixels  Frame height in image pixels.
 */
inline std::size_t UYVYFrameSize(std::size_t widthPixels, std::size_t heightPixels)
{
    // Two image pixels per macropixel, four bytes per macropixel: net 2 bytes/pixel.
    return widthPixels * heightPixels * 2;
}

/**
 * @brief Compute the byte size of a v210 frame buffer.
 *
 * v210 packs 6 image pixels (12 YUV samples in UYVY order) into 4
 * words = 16 bytes. The width is rounded up to a multiple of 48
 * pixels per the SMPTE alignment rule, which is also what AJA's
 * frame buffer allocator does internally. For 1920x1080 the result
 * is 5,529,600 bytes (vs 4,147,200 for 8-bit UYVY, a 33% increase).
 *
 * The (width + 47) / 48 * 48 rounding ensures any rasters we ever
 * touch land on the same alignment AJA expects, even if width is
 * not a multiple of 6.
 *
 * @param widthPixels   Frame width in image pixels.
 * @param heightPixels  Frame height in image pixels.
 */
inline std::size_t V210FrameSize(std::size_t widthPixels, std::size_t heightPixels)
{
    const std::size_t alignedWidth = (widthPixels + 47) / 48 * 48;
    // 6 pixels per 16 bytes  → 16/6 bytes per pixel (= 2.667).
    // Multiply by 8 then divide by 3 stays integer: alignedWidth * 8 / 3.
    return (alignedWidth * 8 / 3) * heightPixels;
}

/**
 * @brief Bytes per image scanline for a given pixel kind.
 *
 * The host transfer buffer is sized via FrameSize(); the GPU
 * texture upload uses pitch information to know how wide the
 * uploaded RGBA8 texture must be (UYVY: width/2 texels per row;
 * V210: alignedWidth/6 * 4 texels per row, since one group of 6
 * pixels = 4 words = 4 RGBA8 texels).
 */
inline std::size_t FrameSize(PixelKind kind,
                             std::size_t widthPixels,
                             std::size_t heightPixels)
{
    return (kind == PixelKind::V210)
        ? V210FrameSize(widthPixels, heightPixels)
        : UYVYFrameSize(widthPixels, heightPixels);
}

/**
 * @brief Width of the RGBA8 GL texture that holds one row of source
 *        pixels in the given kind.
 *
 * - UYVY: 2 source pixels per RGBA8 texel → texture width = width/2.
 * - V210: 4 words (= 4 RGBA8 texels) per 6 source pixels →
 *         texture width = alignedWidth / 6 * 4.
 */
inline std::size_t TextureWidthTexels(PixelKind kind, std::size_t widthPixels)
{
    if (kind == PixelKind::V210)
    {
        const std::size_t alignedWidth = (widthPixels + 47) / 48 * 48;
        return alignedWidth / 6 * 4;
    }
    return widthPixels / 2;
}

/**
 * @brief Build a UYVY macropixel from two adjacent image pixels' YCbCr values.
 *
 * The Cb/Cr inputs apply to both pixels (4:2:2 subsampling). All values are
 * expected to be limited-range, 8-bit (Y in [16,235], CbCr in [16,240]).
 *
 * @param y0  Luma for the left pixel.
 * @param y1  Luma for the right pixel.
 * @param cb  Shared blue-difference chroma.
 * @param cr  Shared red-difference chroma.
 * @return Packed 32-bit macropixel, byte order U Y0 V Y1 (little-endian).
 */
inline uint32_t PackUYVY(uint8_t y0, uint8_t y1, uint8_t cb, uint8_t cr)
{
    return  static_cast<uint32_t>(cb)
         | (static_cast<uint32_t>(y0) << 8)
         | (static_cast<uint32_t>(cr) << 16)
         | (static_cast<uint32_t>(y1) << 24);
}

} // namespace ajainput
