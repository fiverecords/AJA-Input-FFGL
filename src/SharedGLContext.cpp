// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    SharedGLContext.cpp
 * @brief   Windows-only implementation of the secondary GL context owner.
 */

#include "SharedGLContext.h"

#include <cstdio>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <GL/gl.h>
// wglCreateContextAttribsARB and the WGL_CONTEXT_* token constants are not
// in the standard windows headers; they ship as part of the WGL_ARB_create
// _context extension. We declare them locally to avoid pulling in a third
// party header just for two function prototypes.
#  ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#    define WGL_CONTEXT_MAJOR_VERSION_ARB          0x2091
#    define WGL_CONTEXT_MINOR_VERSION_ARB          0x2092
#    define WGL_CONTEXT_PROFILE_MASK_ARB           0x9126
#    define WGL_CONTEXT_CORE_PROFILE_BIT_ARB       0x00000001
#    define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#  endif

typedef HGLRC (WINAPI* PFNWGLCREATECONTEXTATTRIBSARBPROC)(
    HDC hDC, HGLRC hShareContext, const int* attribList);
#endif

namespace ajainput
{

SharedGLContext::~SharedGLContext()
{
    Release();
}

bool SharedGLContext::IsAvailable() const noexcept
{
#ifdef _WIN32
    return m_secondaryRC != nullptr;
#else
    return false;
#endif
}

#ifdef _WIN32

namespace
{

void LogfHost(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    FFGLLog::LogToHost(buf);
}

} // namespace

bool SharedGLContext::Initialise()
{
    Release();

    // The host's context must be current on this thread when Initialise is
    // called. If it is not, we cannot capture it or create a shared one.
    HDC   primaryDC = wglGetCurrentDC();
    HGLRC primaryRC = wglGetCurrentContext();
    if (primaryDC == nullptr || primaryRC == nullptr)
    {
        FFGLLog::LogToHost("AJAInput: no current GL context at SharedGLContext::Initialise");
        return false;
    }

    // Log the renderer we are running against; useful diagnostic data when
    // shared-context behaviour differs across drivers.
    const GLubyte* vendor   = glGetString(GL_VENDOR);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version  = glGetString(GL_VERSION);
    LogfHost("AJAInput: GL vendor=%s renderer=%s version=%s",
             vendor   ? reinterpret_cast<const char*>(vendor)   : "?",
             renderer ? reinterpret_cast<const char*>(renderer) : "?",
             version  ? reinterpret_cast<const char*>(version)  : "?");

    // Look up the extension entry point. It is exposed through wglGetProcAddress
    // because it is part of WGL_ARB_create_context, not core WGL.
    auto wglCreateContextAttribsARB =
        reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
            wglGetProcAddress("wglCreateContextAttribsARB"));

    HGLRC secondaryRC = nullptr;
    if (wglCreateContextAttribsARB != nullptr)
    {
        // Match Resolume's 4.1 core profile (this is the version reported
        // by Resolume in the user's logs: "4.1.0 NVIDIA 596.36"). Using
        // the same profile and version simplifies the sharing semantics.
        const int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
            WGL_CONTEXT_MINOR_VERSION_ARB, 1,
            WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        secondaryRC = wglCreateContextAttribsARB(primaryDC, primaryRC, attribs);
        if (secondaryRC == nullptr)
        {
            const DWORD err = ::GetLastError();
            LogfHost("AJAInput: wglCreateContextAttribsARB failed, err=0x%08lx",
                     static_cast<unsigned long>(err));
        }
    }
    else
    {
        FFGLLog::LogToHost("AJAInput: wglCreateContextAttribsARB not available");
    }

    // Fallback: the legacy wglCreateContext + wglShareLists path. Less
    // precise about profile but widely supported.
    if (secondaryRC == nullptr)
    {
        secondaryRC = wglCreateContext(primaryDC);
        if (secondaryRC != nullptr)
        {
            if (!wglShareLists(primaryRC, secondaryRC))
            {
                const DWORD err = ::GetLastError();
                LogfHost("AJAInput: wglShareLists failed, err=0x%08lx",
                         static_cast<unsigned long>(err));
                wglDeleteContext(secondaryRC);
                secondaryRC = nullptr;
            }
            else
            {
                FFGLLog::LogToHost("AJAInput: secondary GL context created via legacy path");
            }
        }
        else
        {
            const DWORD err = ::GetLastError();
            LogfHost("AJAInput: wglCreateContext fallback failed, err=0x%08lx",
                     static_cast<unsigned long>(err));
        }
    }
    else
    {
        FFGLLog::LogToHost("AJAInput: secondary GL context created via ARB path");
    }

    if (secondaryRC == nullptr)
    {
        FFGLLog::LogToHost("AJAInput: secondary GL context creation failed");
        return false;
    }

    m_primaryDC   = primaryDC;
    m_primaryRC   = primaryRC;
    m_secondaryRC = secondaryRC;
    return true;
}

bool SharedGLContext::ActivateOnCurrentThread()
{
    if (m_primaryDC == nullptr || m_secondaryRC == nullptr)
    {
        FFGLLog::LogToHost("AJAInput: cannot activate secondary context, not initialised");
        return false;
    }

    if (!wglMakeCurrent(static_cast<HDC>(m_primaryDC),
                        static_cast<HGLRC>(m_secondaryRC)))
    {
        const DWORD err = ::GetLastError();
        LogfHost("AJAInput: wglMakeCurrent for secondary context failed, err=0x%08lx",
                 static_cast<unsigned long>(err));
        return false;
    }

    FFGLLog::LogToHost("AJAInput: producer thread acquired secondary GL context");
    return true;
}

void SharedGLContext::DeactivateOnCurrentThread()
{
    wglMakeCurrent(nullptr, nullptr);
}

void SharedGLContext::Release()
{
    if (m_secondaryRC != nullptr)
    {
        // Defensive: ensure the secondary context is not current on the
        // calling thread before we delete it.
        if (wglGetCurrentContext() == static_cast<HGLRC>(m_secondaryRC))
            wglMakeCurrent(nullptr, nullptr);

        wglDeleteContext(static_cast<HGLRC>(m_secondaryRC));
        m_secondaryRC = nullptr;
    }
    m_primaryDC = nullptr;
    m_primaryRC = nullptr;
}

void* SharedGLContext::CurrentHostContext() noexcept
{
    return static_cast<void*>(wglGetCurrentContext());
}

#else // not _WIN32

void* SharedGLContext::CurrentHostContext() noexcept { return nullptr; }
bool SharedGLContext::Initialise()              { return false; }
bool SharedGLContext::ActivateOnCurrentThread() { return false; }
void SharedGLContext::DeactivateOnCurrentThread() {}
void SharedGLContext::Release()                 {}

#endif

} // namespace ajainput
