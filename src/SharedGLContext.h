// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    SharedGLContext.h
 * @brief   Owner of an OpenGL context that shares resources with the host's
 *          primary context, intended to allow the producer thread to upload
 *          texture data without ferrying it through the render thread.
 *
 * This file contains only the lifecycle scaffolding (create, activate on a
 * thread, release). It does not yet move the upload path off the render
 * thread. The intent of this stage is solely to verify that a secondary
 * context can be created against Resolume's primary context on the user's
 * specific hardware and driver combination, before committing to the
 * larger architectural change that depends on it.
 *
 * Currently a no-op on non-Windows platforms; the wgl* APIs do not exist
 * elsewhere and the eventual integration target is Windows-only anyway.
 */
#pragma once

#include <FFGLSDK.h>

#include <cstdint>

namespace ajainput
{

/**
 * @class   SharedGLContext
 * @brief   Owns a secondary GL context for use by the producer thread.
 *
 * On Windows, the primary context (Resolume's) is captured in Initialise
 * and a sharing secondary context is created against it. The secondary
 * context can then be made current on any other thread via Activate.
 *
 * On other platforms, all methods report success without doing anything;
 * callers simply do not get the benefit of off-thread uploads but the
 * rest of the pipeline still works.
 */
class SharedGLContext
{
public:
    SharedGLContext() = default;
    ~SharedGLContext();

    SharedGLContext(const SharedGLContext&)            = delete;
    SharedGLContext& operator=(const SharedGLContext&) = delete;

    /**
     * @brief Capture the host's GL context and create a secondary that
     *        shares resources with it.
     *
     * Must be called from the render thread, while the host's context is
     * current. Logs diagnostic information via FFGLLog so success or
     * failure is visible in the host's log without needing a debugger.
     *
     * @return True if a secondary context was created. A return of false
     *         is informational, not fatal; the plugin must continue to
     *         work without an off-thread upload path.
     */
    bool Initialise();

    /**
     * @brief Make the secondary context current on the calling thread.
     *
     * Intended to be called once at the start of the producer thread's
     * Run() loop. Subsequent GL calls from that thread go through the
     * secondary context but operate on resources visible to the host
     * because of the share-list relationship.
     *
     * @return True if activation succeeded.
     */
    bool ActivateOnCurrentThread();

    /**
     * @brief Make the secondary context non-current on the calling thread.
     */
    void DeactivateOnCurrentThread();

    /**
     * @brief Tear down the secondary context. Safe to call multiple times.
     */
    void Release();

    /// @return True if a secondary context exists and is ready to activate.
    bool IsAvailable() const noexcept;

    /// v0.41: the GL context current on the calling thread (HGLRC on
    /// Windows), as an opaque identity; nullptr elsewhere. Used to share
    /// GL objects only between clips rendered in the same context.
    static void* CurrentHostContext() noexcept;

private:
#ifdef _WIN32
    // We deliberately type these as void* in the header to avoid pulling
    // <windows.h> in everywhere; the implementation casts back to HDC and
    // HGLRC. Using opaque pointers also keeps the API consistent across
    // platforms (other platforms simply have nullptr handles).
    void* m_primaryDC   = nullptr;  // HDC captured at Initialise time.
    void* m_primaryRC   = nullptr;  // HGLRC captured at Initialise time.
    void* m_secondaryRC = nullptr;  // HGLRC created by us.
#endif
};

} // namespace ajainput
