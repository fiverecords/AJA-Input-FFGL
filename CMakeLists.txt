# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 Joaquin Villodre
# =============================================================================
#  AJA-Input-FFGL - FFGL Source plugin for AJA capture cards ("AJA Input")
#
#  Captures from AJA cards (AutoCirculate, frame or field mode, or
#  the manual Low Latency path) and hands frames to Resolume through an
#  off-thread GL upload ring. Falls back to a synthetic SMPTE bar pattern
#  when no card opens.
#
#  Build output: a shared library that Resolume Arena/Avenue 7.26+ loads as
#  a Source plugin.
#    - Windows: AJAInput.dll (primary target)
#    - Linux:   AJAInput.so  (CI and development builds)
#    - macOS:   AJAInput.dylib (untested)
#
#  Requirements:
#    - CMake 3.15 or newer
#    - C++17 toolchain (MSVC 2019+, Clang, or GCC)
#    - Resolume FFGL SDK cloned locally (https://github.com/resolume/ffgl)
#    - GLEW (recommended install path on Windows: vcpkg)
# =============================================================================

cmake_minimum_required(VERSION 3.15)

project(AJAInputFFGL
    VERSION 0.43.0
    DESCRIPTION "FFGL Source plugin for AJA capture cards (AJA Input)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------------------------------------------------------------
# MSVC runtime selection
# -----------------------------------------------------------------------------
# When building against a vcpkg static triplet (x64-windows-static), all the
# vcpkg-provided libraries are compiled with the static MSVC runtime (/MT).
# Our plugin and the FFGL static library must match, or the linker raises
# LNK2038 errors about RuntimeLibrary mismatch. Detect the triplet and set
# CMAKE_MSVC_RUNTIME_LIBRARY accordingly so every target in this build tree
# picks the right runtime.
if(MSVC)
    if(VCPKG_TARGET_TRIPLET MATCHES "static")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        message(STATUS "AJA-Input-FFGL: using static MSVC runtime (/MT) to match vcpkg static triplet")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        message(STATUS "AJA-Input-FFGL: using dynamic MSVC runtime (/MD)")
    endif()
endif()

# -----------------------------------------------------------------------------
# Locate the FFGL SDK
# -----------------------------------------------------------------------------
# By default we expect the resolume/ffgl repository as a sibling directory.
# Override with -DFFGL_ROOT=<path> on the command line if it lives elsewhere.
set(FFGL_ROOT "${CMAKE_SOURCE_DIR}/../ffgl"
    CACHE PATH "Path to the resolume/ffgl repository root")

if(NOT EXISTS "${FFGL_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found at '${FFGL_ROOT}'.\n"
        "Clone it with:\n"
        "  git clone https://github.com/resolume/ffgl.git\n"
        "and either place the resulting folder next to AJA-Input-FFGL, or pass "
        "-DFFGL_ROOT=<path> when invoking cmake."
    )
endif()

# We only want the ffgl-sdk library, not the example plugins shipped with it.
set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)

# GLEW is a transitive dependency of ffgl-sdk on every platform except macOS.
# Locating it in our own scope makes the GLEW::GLEW imported target visible
# both here and downstream of add_subdirectory(${FFGL_ROOT}).
if(NOT APPLE)
    find_package(GLEW REQUIRED)
endif()

add_subdirectory(${FFGL_ROOT} ${CMAKE_BINARY_DIR}/ffgl-build EXCLUDE_FROM_ALL)

# ffgl-sdk is built as a static archive. Embedding it into a shared library
# (our plugin) requires position-independent code on ELF platforms. On Windows
# this property has no effect; setting it unconditionally keeps the build
# consistent across platforms.
set_property(TARGET ffgl-sdk PROPERTY POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------
# Locate libajantv2 (AJA NTV2 SDK, open-source distribution)
# -----------------------------------------------------------------------------
# libajantv2 is required from Phase 6.4 onwards. We expect it as a sibling
# directory of AJA-Input-FFGL, named "libajantv2", checked out from AJA's public
# repository at the version tag the project targets (see docs/LATENCY_STRATEGY
# for the pinned tag). Override with -DAJANTV2_ROOT=<path> if it lives elsewhere.
#
# The library is added as a CMake subdirectory and linked statically. We turn
# off everything we do not need (demos, tests, tools, plugin loader, kernel
# driver on Linux) to keep configure and build times reasonable. The runtime
# library selection happens before add_subdirectory so that libajantv2 picks
# the same MSVC runtime as the rest of the project.
set(AJANTV2_ROOT "${CMAKE_SOURCE_DIR}/../libajantv2"
    CACHE PATH "Path to the libajantv2 (AJA NTV2 SDK) checkout")

if(NOT EXISTS "${AJANTV2_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "libajantv2 not found at '${AJANTV2_ROOT}'.\n"
        "Clone it with:\n"
        "  git clone --branch ntv2_18_0_0 https://github.com/aja-video/libajantv2.git\n"
        "and either place the resulting folder next to AJA-Input-FFGL, or pass "
        "-DAJANTV2_ROOT=<path> when invoking cmake."
    )
endif()

# Force libajantv2's build options before pulling in its CMakeLists. Setting
# them in the cache with FORCE makes sure we win over any defaults libajantv2
# might compute internally based on its own option() calls.
set(AJANTV2_BUILD_OPENSOURCE     ON  CACHE BOOL "" FORCE)
set(AJANTV2_BUILD_SHARED         OFF CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_DEMOS        ON  CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_DRIVER       ON  CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_TESTS        ON  CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_TOOLS        ON  CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_PLUGIN_LOAD  ON  CACHE BOOL "" FORCE)
set(AJA_DISABLE_QT               ON  CACHE BOOL "" FORCE)

add_subdirectory(${AJANTV2_ROOT} ${CMAKE_BINARY_DIR}/aja-build EXCLUDE_FROM_ALL)

# Ensure the AJA archive is PIC for the same reason as ffgl-sdk: it ends up
# embedded inside our shared library on ELF platforms.
if(TARGET ajantv2)
    set_property(TARGET ajantv2 PROPERTY POSITION_INDEPENDENT_CODE ON)

    # libajantv2 carries pieces of code (notably options_popt.cpp from a
    # vendored copy of popt) that pre-date modern C++ and trigger warnings
    # the upstream maintainers have not cleaned up. We silence the specific
    # warning numbers PRIVATE to the ajantv2 target so the build output of
    # our own sources still surfaces every warning we care about. These flags
    # are MSVC-only; GCC/Clang already accept the legacy constructs without
    # complaint at the warning levels we run with.
    if(MSVC)
        target_compile_options(ajantv2 PRIVATE
            /wd5033   # 'register' is no longer a supported storage class
            /wd4244   # conversion from 'X' to 'Y', possible loss of data
            /wd4267   # conversion from 'size_t' to 'X', possible loss of data
            /wd4996)  # use of deprecated function (libajantv2 internal)
    endif()
endif()

# -----------------------------------------------------------------------------
# The plugin shared library
# -----------------------------------------------------------------------------
add_library(AJAInput SHARED
    src/AJAInputPlugin.cpp
    src/AJAInputPlugin.h
    src/SyntheticFrameGenerator.cpp
    src/SyntheticFrameGenerator.h
    src/FrameQueue.cpp
    src/FrameQueue.h
    src/FrameProducer.cpp
    src/FrameProducer.h
    src/TextureUploader.cpp
    src/TextureUploader.h
    src/Overlay.cpp
    src/Overlay.h
    src/SharedGLContext.cpp
    src/SharedGLContext.h
    src/SharedTextureRing.cpp
    src/SharedTextureRing.h
    src/AJADevice.cpp
    src/AJADevice.h
    src/AJACaptureSource.cpp
    src/AJACaptureSource.h
    src/VideoFormat.cpp
    src/VideoFormat.h
    src/FrameStorePool.cpp
    src/FrameStorePool.h
    src/MultiFormatRegistry.cpp
    src/MultiFormatRegistry.h
    src/SharedCapture.cpp
    src/SharedCapture.h
    src/CaptureTopology.cpp
    src/CaptureTopology.h
    src/InputId.h
    src/InputNTV2.h
    src/PictureFit.h
    src/PixelFormat.h
    src/FieldHistoryGate.h
    src/YadifSlots.h
)

# Inherit the public include paths exposed by ffgl-sdk (source/lib/ tree).
# We mark them as SYSTEM so the compiler treats their headers as external
# code: their warnings are silenced while our own /W4 (or -Wall -Wextra) on
# project sources still applies. On MSVC this also activates /external:I
# semantics when combined with the /external:W0 flag below.
target_include_directories(AJAInput SYSTEM PRIVATE
    $<TARGET_PROPERTY:ffgl-sdk,INTERFACE_INCLUDE_DIRECTORIES>)

add_dependencies(AJAInput ffgl-sdk)

# Link transitive dependencies (OpenGL, GLEW) explicitly. We avoid propagating
# them via $<TARGET_PROPERTY:ffgl-sdk,INTERFACE_LINK_LIBRARIES> because the
# imported targets created inside the ffgl subdirectory are not always visible
# in our scope by generator-expression evaluation time.
find_package(OpenGL REQUIRED)
target_link_libraries(AJAInput PRIVATE OpenGL::GL)
if(NOT APPLE)
    target_link_libraries(AJAInput PRIVATE GLEW::GLEW)
endif()

# std::thread needs an explicit pthread link on POSIX systems. The Threads
# package selects the right flag (-pthread or equivalent) per platform; on
# MSVC it is a no-op.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(AJAInput PRIVATE Threads::Threads)

# libajantv2 is consumed as a static archive. Its headers are pulled in as
# SYSTEM so any warnings they emit do not pollute our /W4 (or -Wall -Wextra)
# build. The library propagates its own include paths and platform-specific
# link dependencies (setupapi etc. on Windows; rt, dl, udev on Linux) via its
# INTERFACE properties, so we do not have to enumerate them here.
target_link_libraries(AJAInput PRIVATE ajantv2)
get_target_property(_aja_include_dirs ajantv2 INTERFACE_INCLUDE_DIRECTORIES)
if(_aja_include_dirs)
    target_include_directories(AJAInput SYSTEM PRIVATE ${_aja_include_dirs})
endif()

# Force the linker to pull every object file of ffgl-sdk into our shared
# library. Without this the linker would discard objects whose symbols are
# not referenced from our own translation units, and that includes the only
# exported symbol Resolume looks for: plugMain.
if(MSVC)
    target_link_libraries(AJAInput PRIVATE ffgl-sdk)
    target_link_options(AJAInput PRIVATE
        "/WHOLEARCHIVE:$<TARGET_FILE_NAME:ffgl-sdk>")
    target_compile_options(AJAInput PRIVATE
        /W4 /permissive- /utf-8
        # Treat headers reached via SYSTEM include dirs as "external" and
        # silence their warnings. Required on MSVC because by default /W4
        # still reports warnings from headers brought in via -I/SYSTEM.
        /external:W0
        /external:anglebrackets)
    target_compile_definitions(AJAInput PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32
        _WINDOWS
        _USRDLL)
elseif(APPLE)
    target_link_options(AJAInput PRIVATE
        "-Wl,-force_load,$<TARGET_FILE:ffgl-sdk>")
else()
    # GCC and Clang on Linux: --whole-archive around the absolute path of the
    # archive avoids the deduplication CMake applies when the same library
    # appears more than once on the link line.
    target_link_options(AJAInput PRIVATE
        "LINKER:--whole-archive,$<TARGET_FILE:ffgl-sdk>,--no-whole-archive")
    target_compile_options(AJAInput PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Drop the "lib" prefix on Unix-like platforms so the binary name is just
# AJAInput.{so,dylib}, matching what Resolume expects to find.
set_target_properties(AJAInput PROPERTIES
    PREFIX ""
    OUTPUT_NAME "AJAInput"
)

message(STATUS "AJA-Input-FFGL: FFGL SDK at ${FFGL_ROOT}")
message(STATUS "AJA-Input-FFGL: plugin binary will be placed under ${CMAKE_BINARY_DIR}")


# =============================================================================
# Test target.
#
# Standalone executable that exercises the building blocks of the synthetic
# capture path (FrameQueue, FrameStorePool, VideoFormat) without requiring an
# AJA card. Built unconditionally alongside the plugin so a `cmake --build`
# always produces both, and registered with CTest so `ctest` (or the CI
# workflow) runs the suite.
#
# We deliberately skip tests for any code path that needs an active OpenGL
# context (FrameProducer, SharedTextureRing texture ops, AJAInputPlugin::InitGL,
# Overlay). Those would require headless GL on the CI box (EGL surfaceless or
# llvmpipe), which is feasible but not free; we will add a separate target
# with a guarded glfw/EGL dependency when the value justifies the
# infrastructure work.
# =============================================================================

enable_testing()

add_executable(AJAInputTests
    tests/test_main.cpp
    tests/test_FrameQueue.cpp
    tests/test_FrameStorePool.cpp
    tests/test_VideoFormat.cpp
    tests/test_YadifSlots.cpp
    tests/test_FieldHistoryGate.cpp
    tests/test_MultiFormatRegistry.cpp
    tests/test_SyntheticFrameGenerator.cpp
    tests/test_InputId.cpp
    tests/test_CaptureTopology.cpp
    tests/test_PictureFit.cpp
    src/FrameQueue.cpp
    src/FrameStorePool.cpp
    src/MultiFormatRegistry.cpp
    src/VideoFormat.cpp
    src/SyntheticFrameGenerator.cpp
    src/CaptureTopology.cpp
)

target_include_directories(AJAInputTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

# VideoFormat depends on the AJA SDK for the NTV2VideoFormat enum.
target_include_directories(AJAInputTests SYSTEM PRIVATE ${_aja_include_dirs})

# Link against the ajantv2 target to inherit its INTERFACE compile
# definitions (AJA_LINUX / AJA_MAC / AJA_WINDOWS, NTV2_*_REVISION, the
# ajaonly bits) which the SDK headers require to pick the right
# platform branch. The linker will discard the unused symbols; what we
# actually take from the SDK is just a handful of enum values.
target_link_libraries(AJAInputTests PRIVATE ajantv2 Threads::Threads)

set_target_properties(AJAInputTests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(AJAInputTests PRIVATE /W3)
else()
    target_compile_options(AJAInputTests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =============================================================================
# Optional offline FFGL host smoke test (Linux, EGL surfaceless + Mesa).
#
# Loads the built plugin through plugMain like a host, drives several clip
# instances on the synthetic generator and checks the v0.41 shared capture
# (tests/host/ffgl_host_smoke.cpp). Off by default: it needs EGL, GLEW and
# a Mesa driver with surfaceless support on the build machine.
# =============================================================================
option(AJAINPUT_BUILD_HOST_SMOKE "Build the offline FFGL host smoke test (Linux)" OFF)
if(AJAINPUT_BUILD_HOST_SMOKE AND UNIX AND NOT APPLE)
    find_library(AJAINPUT_EGL_LIB EGL REQUIRED)
    find_package(GLEW REQUIRED)
    add_executable(ffgl_host_smoke tests/host/ffgl_host_smoke.cpp)
    target_link_libraries(ffgl_host_smoke PRIVATE
        GLEW::GLEW ${AJAINPUT_EGL_LIB} OpenGL::GL ${CMAKE_DL_LIBS} Threads::Threads)
    set_target_properties(ffgl_host_smoke PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_dependencies(ffgl_host_smoke AJAInput)
    add_test(NAME AJAInput_host_smoke
             COMMAND ffgl_host_smoke $<TARGET_FILE:AJAInput>)
endif()

add_test(NAME AJAInput_unit_tests COMMAND AJAInputTests)
