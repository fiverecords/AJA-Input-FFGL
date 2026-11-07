// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    TextureUploader.h
 * @brief   Asynchronous GL texture uploader for UYVY frame buffers.
 *
 * The uploader manages a single 2D texture and a ring of Pixel Buffer Objects
 * (PBOs) used to transfer UYVY frames from CPU memory into GPU memory without
 * blocking the render thread. The pattern is the canonical "orphan-and-fill"
 * double-buffer: while the GPU is consuming the upload issued the previous
 * frame, the CPU writes the next frame into an alternate PBO. Neither side
 * waits for the other.
 *
 * Memory layout: the underlying GL texture is allocated as
 *
 *     width  = imagePixelWidth / 2
 *     format = GL_RGBA8
 *
 * with each RGBA8 texel representing one four-byte UYVY macropixel (two
 * image pixels). The fragment shader reconstructs per-pixel luma from the
 * U Y0 V Y1 channel layout and applies the BT.709 inverse matrix.
 *
 * Latency statistics: every Upload() call is timed (CPU-side wall time
 * from entry to return). The most recent samples are retained in a fixed
 * sliding window; GetStats() returns the current average and maximum over
 * that window and is safe to call from the render thread at any time.
 */
#pragma once

#include <FFGLSDK.h>
#include "PixelFormat.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ajainput
{

class TextureUploader
{
public:
    /// @brief Number of PBOs in the ring. Two is sufficient for full overlap.
    static constexpr std::size_t kPBORingSize = 2;

    TextureUploader() = default;
    ~TextureUploader();

    TextureUploader(const TextureUploader&)            = delete;
    TextureUploader& operator=(const TextureUploader&) = delete;

    /**
     * @brief Allocate the underlying GL texture and the PBO ring.
     * @param widthPixels   Frame width in image pixels (must be even).
     * @param heightPixels  Frame height in image pixels.
     * @param kind          Pixel format of the source bytes. Defaults to
     *                      UYVY for backward compatibility; V210 selects
     *                      the wider RGBA8 texture and the larger PBO
     *                      stride. The shader is responsible for the
     *                      sample-time unpack of V210 bytes.
     * @return True on success.
     */
    bool Initialise(std::size_t widthPixels,
                    std::size_t heightPixels,
                    PixelKind   kind = PixelKind::UYVY);

    /**
     * @brief Upload a fresh frame buffer into the GL texture via the
     *        next PBO.
     * @param data  Pointer to a frame buffer of exactly
     *              FrameSize(kind, widthPixels, heightPixels) bytes,
     *              where `kind` is the value passed to Initialise.
     */
    void Upload(const uint8_t* data);

    /// @brief Release GL resources.
    void Release();

    /// @return Raw GL texture name, or 0 if not initialised.
    GLuint TextureId() const noexcept { return m_texture; }

    std::size_t Width()  const noexcept { return m_width; }
    std::size_t Height() const noexcept { return m_height; }

private:
    GLuint                              m_texture    = 0;
    std::array<GLuint, kPBORingSize>    m_pbos       = {};
    std::size_t                         m_currentPBO = 0;
    std::size_t                         m_width      = 0;
    std::size_t                         m_height     = 0;
    std::size_t                         m_bufferSize = 0;
    PixelKind                           m_kind       = PixelKind::UYVY;
};

} // namespace ajainput
