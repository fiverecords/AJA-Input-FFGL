// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    Overlay.h
 * @brief   Lightweight GL text overlay for development diagnostics.
 *
 * Renders short ASCII strings on top of the plugin output using a small
 * embedded 5x7 bitmap font. The font supports digits, a period, space, the
 * lowercase letters needed to spell "avg", "max", "ms" and "fps", and the
 * uppercase letters needed for "FPS". Anything outside that set renders as
 * a blank glyph.
 *
 * The overlay is intended purely as a development aid and is hidden behind
 * a plugin parameter so end users never see it in production.
 *
 * Rendering pipeline: a single R8 texture holds all glyphs side-by-side as
 * a horizontal atlas. For each character in the input string, a textured
 * quad is appended to a per-frame vertex buffer; the whole string is then
 * drawn with one glDrawArrays call. The fragment shader discards texels
 * below a threshold so the overlay composites correctly without depending
 * on the host's blending state.
 */
#pragma once

#include <FFGLSDK.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ajainput
{

class Overlay
{
public:
    Overlay() = default;
    ~Overlay();

    Overlay(const Overlay&)            = delete;
    Overlay& operator=(const Overlay&) = delete;

    /// @brief Compile shader, build the font atlas texture, allocate buffers.
    bool Initialise();

    /**
     * @brief Render a string at the given top-left position in NDC space.
     *
     * NDC coordinates: x in [-1, 1] running left-to-right, y in [-1, 1]
     * running bottom-to-top. Position (-1, 1) is the top-left corner of the
     * output. A typical diagnostic location is around (-0.97, 0.95).
     *
     * @param text          The string to render. Characters outside the
     *                      supported set render as blanks.
     * @param ndcX          NDC x of the string's top-left corner.
     * @param ndcY          NDC y of the string's top-left corner.
     * @param glyphHeight   Height of one glyph in NDC units (typical: 0.04).
     * @param colorRGB      Glyph color, RGB in [0, 1].
     */
    void Render(const std::string& text,
                float              ndcX,
                float              ndcY,
                float              glyphHeight,
                const float        colorRGB[3]);

    /// @brief Free GL resources.
    void Release();

private:
    void EnsureVertexCapacity(std::size_t characters);

    GLuint m_atlasTex   = 0;
    GLuint m_program    = 0;
    GLuint m_vao        = 0;
    GLuint m_vbo        = 0;

    GLint  m_uAtlas     = -1;
    GLint  m_uColor     = -1;

    std::size_t m_vboCapacityBytes = 0;

    // Per-call vertex staging, kept to avoid a heap allocation per
    // Render call.
    std::vector<float> m_vertexScratch;
};

} // namespace ajainput
