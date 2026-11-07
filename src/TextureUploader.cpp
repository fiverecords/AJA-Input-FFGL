// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    TextureUploader.cpp
 * @brief   PBO double-buffer implementation of the UYVY texture uploader.
 */

#include "TextureUploader.h"
#include "PixelFormat.h"

#include <cstring>

namespace ajainput
{

TextureUploader::~TextureUploader()
{
    Release();
}

bool TextureUploader::Initialise(std::size_t widthPixels,
                                 std::size_t heightPixels,
                                 PixelKind   kind)
{
    Release();

    if ((widthPixels & 1u) != 0u || widthPixels == 0u || heightPixels == 0u)
        return false;

    m_width      = widthPixels;
    m_height     = heightPixels;
    m_kind       = kind;
    m_bufferSize = FrameSize(kind, m_width, m_height);

    const GLsizei texWidth  =
        static_cast<GLsizei>(TextureWidthTexels(kind, m_width));
    const GLsizei texHeight = static_cast<GLsizei>(m_height);

    glGenTextures(1, &m_texture);
    if (m_texture == 0)
    {
        Release();
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, m_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D, 0,
        GL_RGBA8,
        texWidth, texHeight, 0,
        GL_RGBA, GL_UNSIGNED_BYTE,
        nullptr
    );
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenBuffers(static_cast<GLsizei>(m_pbos.size()), m_pbos.data());
    for (GLuint pbo : m_pbos)
    {
        if (pbo == 0)
        {
            Release();
            return false;
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
        glBufferData(GL_PIXEL_UNPACK_BUFFER,
                     static_cast<GLsizeiptr>(m_bufferSize),
                     nullptr,
                     GL_STREAM_DRAW);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    m_currentPBO  = 0;
    return true;
}

void TextureUploader::Upload(const uint8_t* data)
{
    if (m_texture == 0 || data == nullptr || m_bufferSize == 0)
        return;

    const GLuint pbo = m_pbos[m_currentPBO];
    m_currentPBO = (m_currentPBO + 1u) % kPBORingSize;

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);

    // Orphan the buffer to avoid stalls waiting on the GPU's previous read.
    glBufferData(GL_PIXEL_UNPACK_BUFFER,
                 static_cast<GLsizeiptr>(m_bufferSize),
                 nullptr,
                 GL_STREAM_DRAW);

    void* mapped = glMapBufferRange(
        GL_PIXEL_UNPACK_BUFFER,
        0,
        static_cast<GLsizeiptr>(m_bufferSize),
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT
    );

    if (mapped != nullptr)
    {
        std::memcpy(mapped, data, m_bufferSize);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

        const GLsizei texWidth  =
            static_cast<GLsizei>(TextureWidthTexels(m_kind, m_width));
        const GLsizei texHeight = static_cast<GLsizei>(m_height);

        glBindTexture(GL_TEXTURE_2D, m_texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(
            GL_TEXTURE_2D, 0,
            0, 0,
            texWidth, texHeight,
            GL_RGBA, GL_UNSIGNED_BYTE,
            nullptr  // offset into the bound PBO.
        );
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void TextureUploader::Release()
{
    if (m_texture != 0)
    {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }

    bool anyPBO = false;
    for (GLuint& pbo : m_pbos) if (pbo != 0) { anyPBO = true; break; }
    if (anyPBO)
    {
        glDeleteBuffers(static_cast<GLsizei>(m_pbos.size()), m_pbos.data());
        for (GLuint& pbo : m_pbos) pbo = 0;
    }

    m_width       = 0;
    m_height      = 0;
    m_bufferSize  = 0;
    m_currentPBO  = 0;
}

} // namespace ajainput
