// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    Overlay.cpp
 * @brief   Implementation of the diagnostic text overlay.
 */

#include "Overlay.h"

#include <array>
#include <cstring>
#include <vector>

namespace ajainput
{
namespace
{

// ---------------------------------------------------------------------------
// Embedded 5x7 bitmap font.
//
// Each supported character is represented by 7 bytes: one row of pixels per
// byte, top row first. Bits are packed with bit 4 = leftmost pixel and bit 0
// = rightmost. A set bit means "draw this pixel".
// ---------------------------------------------------------------------------

constexpr int kGlyphWidth  = 5;
constexpr int kGlyphHeight = 7;

struct Glyph
{
    char    ch;
    uint8_t rows[kGlyphHeight];
};

constexpr Glyph kFont[] = {
    { '0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E} },
    { '1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E} },
    { '2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F} },
    { '3', {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E} },
    { '4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02} },
    { '5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E} },
    { '6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E} },
    { '7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08} },
    { '8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E} },
    { '9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C} },
    { '.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04} },
    { ' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00} },
    { 'a', {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F} },
    { 'v', {0x00, 0x00, 0x11, 0x11, 0x0A, 0x0A, 0x04} },
    { 'g', {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x0E} },
    { 'm', {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11} },
    { 'x', {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11} },
    { 's', {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E} },
    { 'F', {0x1F, 0x10, 0x10, 0x1C, 0x10, 0x10, 0x10} },
    { 'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10} },
    { 'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E} },
    { 'e', {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E} },
    { 'r', {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10} },
    { 'i', {0x04, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04} },
    { 'n', {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11} },
    { 'd', {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F} },
    { 'o', {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E} },
    { 'p', {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10} },
    { 'w', {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A} },
    { 't', {0x08, 0x08, 0x1C, 0x08, 0x08, 0x08, 0x06} },
    { 'j', {0x04, 0x00, 0x04, 0x04, 0x04, 0x04, 0x18} },
    { 'l', {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E} },
    { 'u', {0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0F} },
    { 'f', {0x06, 0x08, 0x1C, 0x08, 0x08, 0x08, 0x08} },
    { 'c', {0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E} },
    { 'k', {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12} },
    { 'b', {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E} },
    { 'h', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11} },
    { 'q', {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01} },
    { 'y', {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E} },
    { ':', {0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00} },
    { '/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10} },
    // Used by the version line suffixes "[LL]" / "[10b]" and by the
    // "?" fallback of the capture state name. Missing before v0.39,
    // so those suffixes rendered as blanks.
    { 'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F} },
    { '[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E} },
    { ']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E} },
    { '?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04} },
    // v0.41: the rest of the capitals and some punctuation, so status
    // texts ("AutoCirculate init failed", "input used by another
    // clip (fs1)", "SDI2 -> FS2") render instead of leaving blanks.
    { 'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11} },
    { 'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E} },
    { 'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E} },
    { 'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C} },
    { 'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F} },
    { 'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F} },
    { 'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11} },
    { 'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E} },
    { 'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C} },
    { 'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11} },
    { 'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11} },
    { 'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11} },
    { 'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E} },
    { 'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D} },
    { 'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11} },
    { 'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04} },
    { 'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E} },
    { 'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04} },
    { 'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A} },
    { 'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11} },
    { 'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04} },
    { 'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F} },
    { 'z', {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F} },
    { '-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00} },
    { '>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08} },
    { '<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02} },
    { '(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02} },
    { ')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08} },
    { ',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08} },
    { '!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04} },
    { '\'', {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00} },
    { '+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00} },
    { '=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00} },
    { '%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03} },
    { '_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F} },
    { '|', {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04} },
};

constexpr int kGlyphCount = static_cast<int>(sizeof(kFont) / sizeof(kFont[0]));

/// @brief Find a glyph's atlas slot index, or -1 if the character is unsupported.
int GlyphIndex(char c)
{
    for (int i = 0; i < kGlyphCount; ++i)
        if (kFont[i].ch == c) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Atlas geometry: glyphs laid out side by side, no padding. Total atlas
// width = kGlyphCount * kGlyphWidth, height = kGlyphHeight.
// ---------------------------------------------------------------------------

constexpr int kAtlasWidth  = kGlyphCount * kGlyphWidth;
constexpr int kAtlasHeight = kGlyphHeight;

void BuildAtlasPixels(std::vector<uint8_t>& outPixels)
{
    outPixels.assign(static_cast<std::size_t>(kAtlasWidth * kAtlasHeight), 0);
    for (int g = 0; g < kGlyphCount; ++g)
    {
        const auto& rows = kFont[g].rows;
        for (int row = 0; row < kGlyphHeight; ++row)
        {
            const uint8_t bits = rows[row];
            for (int col = 0; col < kGlyphWidth; ++col)
            {
                const bool on = ((bits >> (kGlyphWidth - 1 - col)) & 1u) != 0u;
                const int  x  = g * kGlyphWidth + col;
                const int  y  = row;
                outPixels[static_cast<std::size_t>(y * kAtlasWidth + x)] = on ? 0xFFu : 0x00u;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Shaders.
// ---------------------------------------------------------------------------

const char kVertexShader[] = R"(#version 410 core
layout(location = 0) in vec2 vPosition;  // NDC position
layout(location = 1) in vec2 vUV;        // atlas UV
out vec2 uv;
void main() {
    gl_Position = vec4(vPosition, 0.0, 1.0);
    uv = vUV;
}
)";

const char kFragmentShader[] = R"(#version 410 core
in  vec2 uv;
out vec4 fragColor;
uniform sampler2D Atlas;
uniform vec3      OverlayColor;
void main() {
    float a = texture(Atlas, uv).r;
    if (a < 0.5) discard;
    fragColor = vec4(OverlayColor, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Local shader-program helper (we don't reuse ffglex::FFGLShader because we
// want full control over attribute layout and a self-contained life cycle).
// ---------------------------------------------------------------------------

GLuint CompileShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint LinkProgram(GLuint vs, GLuint fs)
{
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

} // namespace

// ===========================================================================

Overlay::~Overlay()
{
    Release();
}

bool Overlay::Initialise()
{
    Release();

    // Compile shaders.
    const GLuint vs = CompileShader(GL_VERTEX_SHADER,   kVertexShader);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vs == 0 || fs == 0)
    {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }

    m_program = LinkProgram(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (m_program == 0)
        return false;

    m_uAtlas = glGetUniformLocation(m_program, "Atlas");
    m_uColor = glGetUniformLocation(m_program, "OverlayColor");

    // Build and upload the font atlas.
    std::vector<uint8_t> pixels;
    BuildAtlasPixels(pixels);

    glGenTextures(1, &m_atlasTex);
    if (m_atlasTex == 0) { Release(); return false; }

    glBindTexture(GL_TEXTURE_2D, m_atlasTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D, 0,
        GL_R8,
        kAtlasWidth, kAtlasHeight, 0,
        GL_RED, GL_UNSIGNED_BYTE,
        pixels.data()
    );
    glBindTexture(GL_TEXTURE_2D, 0);

    // VAO + VBO for the dynamic per-frame vertex stream.
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    if (m_vao == 0 || m_vbo == 0) { Release(); return false; }

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    // Attribute 0: position (vec2). Attribute 1: uv (vec2). Stride = 4 floats.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    return true;
}

void Overlay::EnsureVertexCapacity(std::size_t characters)
{
    // Six vertices per character (two triangles), four floats per vertex.
    const std::size_t needed = characters * 6u * 4u * sizeof(float);
    if (needed <= m_vboCapacityBytes)
        return;

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(needed),
                 nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_vboCapacityBytes = needed;
}

void Overlay::Render(const std::string& text,
                     float              ndcX,
                     float              ndcY,
                     float              glyphHeight,
                     const float        colorRGB[3])
{
    if (m_program == 0 || m_atlasTex == 0 || text.empty())
        return;

    // Aspect-correct glyph width: keep the 5:7 source ratio.
    // (Cells are square in NDC unless we correct here.)
    const float glyphWidth   = glyphHeight * (5.0f / 7.0f);
    const float glyphSpacing = glyphWidth * 1.15f;

    // Reused across calls (v0.40): Render runs several times per frame
    // on the render thread, and a fresh vector each time was a heap
    // allocation per line. clear() keeps the capacity.
    std::vector<float>& verts = m_vertexScratch;
    verts.clear();
    verts.reserve(text.size() * 6u * 4u);

    const float atlasW = static_cast<float>(kAtlasWidth);

    float cursorX = ndcX;
    const float topY    = ndcY;
    const float bottomY = ndcY - glyphHeight;

    for (char c : text)
    {
        const int g = GlyphIndex(c);
        if (g >= 0)
        {
            const float left   = cursorX;
            const float right  = cursorX + glyphWidth;
            const float u0     = (g * kGlyphWidth)            / atlasW;
            const float u1     = ((g + 1) * kGlyphWidth)      / atlasW;
            // V coordinates: glTexImage2D treats the first row of pixel data
            // as the bottom row of the resulting texture (GL bottom-left
            // origin). Our atlas was written with row 0 = top of glyph, so
            // the top of the glyph lives at texture v=0, the bottom at v=1.
            // Therefore the top vertex of the screen quad samples v=0 and
            // the bottom vertex samples v=1.
            const float v0     = 0.0f;
            const float v1     = 1.0f;

            // Two triangles per glyph quad.
            const float quad[6 * 4] = {
                left,  topY,    u0, v0,
                right, topY,    u1, v0,
                left,  bottomY, u0, v1,

                left,  bottomY, u0, v1,
                right, topY,    u1, v0,
                right, bottomY, u1, v1,
            };
            verts.insert(verts.end(), std::begin(quad), std::end(quad));
        }
        cursorX += glyphSpacing;
    }

    if (verts.empty())
        return;

    EnsureVertexCapacity(verts.size() / 24u);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                    verts.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glUseProgram(m_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_atlasTex);
    if (m_uAtlas >= 0) glUniform1i(m_uAtlas, 0);
    if (m_uColor >= 0) glUniform3f(m_uColor, colorRGB[0], colorRGB[1], colorRGB[2]);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 4));
    glBindVertexArray(0);

    // Restore default state to comply with FFGL host expectations.
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void Overlay::Release()
{
    if (m_vbo)         { glDeleteBuffers(1, &m_vbo);          m_vbo = 0; }
    if (m_vao)         { glDeleteVertexArrays(1, &m_vao);     m_vao = 0; }
    if (m_atlasTex)    { glDeleteTextures(1, &m_atlasTex);    m_atlasTex = 0; }
    if (m_program)     { glDeleteProgram(m_program);          m_program = 0; }
    m_uAtlas = m_uColor = -1;
    m_vboCapacityBytes = 0;
}

} // namespace ajainput
