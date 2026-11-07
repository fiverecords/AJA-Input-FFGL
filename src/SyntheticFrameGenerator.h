// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    SyntheticFrameGenerator.h
 * @brief   Produces synthetic UYVY frames for development and validation.
 *
 * Until the AJA capture pipeline is integrated, this component supplies the
 * plugin with frames in the exact format the real capture path will deliver
 * (UYVY 4:2:2, 8-bit, limited range). The generated content is a SMPTE-like
 * color bar pattern with an animated indicator strip in the lower portion;
 * the visual result rendered by the plugin should be indistinguishable from
 * what a hardware source feeding the same pattern would produce, modulo
 * minor differences in interpolation.
 *
 * This generator is intentionally CPU-only and trivially understandable. It
 * is not a long-term production component, but a stable, well-known frame
 * source against which the rest of the pipeline (texture upload, color
 * conversion shader, latency measurement) can be developed and validated.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ajainput
{

/**
 * @class   SyntheticFrameGenerator
 * @brief   Generates UYVY frames at a fixed resolution.
 *
 * A single instance owns one frame-sized buffer that is rewritten on every
 * call to GenerateFrame. The buffer is exposed to the caller by const
 * reference; ownership remains with the generator and the data remains valid
 * until the next call to GenerateFrame on the same instance.
 */
class SyntheticFrameGenerator
{
public:
    /**
     * @param widthPixels   Frame width in image pixels (must be even).
     * @param heightPixels  Frame height in image pixels.
     */
    SyntheticFrameGenerator(std::size_t widthPixels, std::size_t heightPixels);

    /**
     * @brief Render the next frame into the internal buffer.
     * @param timeSeconds  Elapsed time used to animate the indicator strip.
     * @param inputIndex   Simulated input selector (0=SDI1, 1=SDI2, 2=HDMI1)
     *                     used only to color the indicator strip until the
     *                     hardware path is integrated.
     */
    void GenerateFrame(float timeSeconds, int inputIndex);

    /// @return Pointer to the UYVY frame buffer.
    const uint8_t* Data() const noexcept { return m_buffer.data(); }

    /// @return Size of the buffer in bytes.
    std::size_t SizeBytes() const noexcept { return m_buffer.size(); }

    std::size_t Width()  const noexcept { return m_width; }
    std::size_t Height() const noexcept { return m_height; }

private:
    std::size_t          m_width  = 0;
    std::size_t          m_height = 0;
    std::vector<uint8_t> m_buffer;
};

} // namespace ajainput
