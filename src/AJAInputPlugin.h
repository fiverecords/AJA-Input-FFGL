// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    AJAInputPlugin.h
 * @brief   FFGL Source plugin for AJA capture cards ("AJA Input").
 *
 * One instance per Resolume clip. Since v0.41 the capture itself (card,
 * FrameStore, capture thread, texture ring, capture settings) lives in a
 * ajainput::SharedCapture per card input, shared by every clip of that input
 * the way Resolume's own live inputs are; see SharedCapture.h. The clip
 * keeps what is per clip: the YCbCr-to-RGB shader with its colour and
 * de-interlace settings, the YADIF history, and the stats overlay.
 */
#pragma once

#include "InputId.h"
#include "Overlay.h"
#include "PictureFit.h"
#include "PixelFormat.h"
#include "SharedCapture.h"
#include "SharedTextureRing.h"
#include "VideoFormat.h"
#include "FieldHistoryGate.h"
#include "YadifSlots.h"

#include <FFGLSDK.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

class AJAInputPlugin : public CFFGLPlugin
{
public:
    AJAInputPlugin();
    ~AJAInputPlugin() override = default;

    AJAInputPlugin(const AJAInputPlugin&)            = delete;
    AJAInputPlugin& operator=(const AJAInputPlugin&) = delete;

    FFResult InitGL(const FFGLViewportStruct* viewport) override;
    FFResult ProcessOpenGL(ProcessOpenGLStruct* processStruct) override;
    FFResult DeInitGL() override;

    FFResult SetFloatParameter(unsigned int index, float value) override;
    float    GetFloatParameter(unsigned int index) override;
    FFResult SetTextParameter(unsigned int index, const char* value) override;
    char*    GetTextParameter(unsigned int index) override;

    /// @brief Discrete option values exposed by PID_Colorimetry. The
    ///        numeric values double as dropdown indices visible in
    ///        Resolume and as the canonical "what is selected" enum
    ///        used throughout the colour-conversion code. "Auto"
    ///        picks BT.601 limited for SD (525i/625i) and BT.709
    ///        limited for everything HD and above, which matches what
    ///        every broadcast capture / playout pipeline does by
    ///        default and what camera and switcher firmware emit.
    ///
    /// Exposed publicly so the free helpers that turn a mode into
    /// shader uniforms (in AJAInputPlugin.cpp's anonymous namespace) can
    /// name it without friending the implementation.
    enum class ColorimetryMode : int
    {
        Auto            = 0,
        BT709Limited    = 1,
        BT709Full       = 2,
        BT601Limited    = 3,
        BT601Full       = 4,
        // HDR transfer functions over BT.2020 colour primaries. These
        // operate on the YCbCr sample the same way the SDR modes do,
        // but the shader interprets the normalised values as gamma-
        // encoded EOTF inputs and runs the inverse PQ / HLG transfer
        // before colour-space conversion. The result is tone-mapped
        // down to the SDR range Resolume's composite expects.
        //
        // Important caveat for v0.29.1: the device-side pixel format
        // is still 8-bit YCbCr (NTV2_FBF_8BIT_YCBCR / UYVY). PQ/HLG
        // sources in 8-bit clip highlights well below the format's
        // theoretical 10,000 / 1,000 nit peaks; a proper HDR pipeline
        // requires 10-bit capture (NTV2_FBF_10BIT_YCBCR / v210), which
        // is the scope of v0.29.2. These two modes are exposed now
        // because (a) some upstream gear does send PQ/HLG-encoded
        // 8-bit signal anyway and the shader needs to handle it
        // somehow other than treating it as Rec.709, and (b) the
        // shader architecture lands here so v0.29.2 only has to
        // change the device format, not the shader.
        PQ_BT2020       = 5,
        HLG_BT2020      = 6
    };

    /// @brief De-interlace method selection. Exposed at the FFGL
    ///        parameter surface as a simple on/off toggle since there
    ///        is only one active method (YADIF on UYVY, simple bob on
    ///        V210). This enum is kept so that the named values
    ///        document the meaning of InterlaceMode = 1 in the shader
    ///        and to make it easy to promote the parameter back to an
    ///        option dropdown if a third method is added later.
    enum class DeinterlaceMode : int
    {
        Weave           = 0,
        Yadif           = 1
    };

private:
    /// v0.41: attach to the shared capture of (Device, Input), creating
    /// it when this is the first clip of that input. Called from
    /// ProcessOpenGL only: the host applies a clip's saved parameter
    /// values after InitGL (FF_INSTANTIATE_GL sets the defaults and calls
    /// InitGL in one go), and clips that are never shown should not hold
    /// a capture. Render thread, host GL context current.
    void AttachSource();
    /// v0.41: let go of the shared capture; the last clip of an input
    /// destroys it (render thread, host GL context current).
    void DetachSource();
    /// v0.41: adopt the shared capture's format (re-sizing the per-clip
    /// YADIF history) and settings (mirrors + host value events).
    void SyncFromSource();
    /// v0.41: this clip's capture settings, from the parameter mirrors.
    ajainput::CaptureSettings CurrentSettings() const noexcept;
    /// v0.41: the Input parameter as an input number (InputId.h). v0.42:
    /// a value no input has (a composition from a later version) falls
    /// back to SDI 1, as Resolume does with an unknown option value.
    int InputNumber() const noexcept
    {
        const int n = ajainput::input::FromOptionValue(m_inputSource);
        return ajainput::input::IsValid(n) ? n : 1;
    }
    /// v0.41: the Device parameter as a scanner index.
    uint32_t DeviceIndexParam() const noexcept
    { return static_cast<uint32_t>(m_deviceIndex + 0.5f); }

    /// v0.42: what a clip shows while its input delivers no picture.
    enum class NoSignalMode : int
    {
        Black       = 0,
        Transparent = 1,
        HoldLast    = 2
    };
    NoSignalMode CurrentNoSignalMode() const noexcept
    {
        const int v = static_cast<int>(m_noSignalMode + 0.5f);
        return (v == 1) ? NoSignalMode::Transparent
             : (v == 2) ? NoSignalMode::HoldLast : NoSignalMode::Black;
    }
    /// v0.42 Hold Last Frame: copy what this clip just drew (the host's
    /// framebuffer) into the hold texture / paint it back. Render
    /// thread; both restore the framebuffer bindings and scissor state.
    void SaveHeldFrame();
    bool DrawHeldFrame();
    void ReleaseHeldFrame();

    /// v0.42 Resize: how the picture fills the clip (PictureFit.h).
    ajainput::ResizeMode CurrentResizeMode() const noexcept
    { return ajainput::ResizeModeFromOption(m_resizeMode); }
    /// v0.42 Resize: the rect of the host's viewport `host` the picture
    /// is drawn into, for the current format and Resize mode.
    ajainput::PixelRect PictureRectIn(const ajainput::PixelRect& host) const noexcept
    {
        return ajainput::PictureRect(CurrentResizeMode(),
                                  static_cast<int>(m_currentFormat.width),
                                  static_cast<int>(m_currentFormat.height),
                                  host, m_maxViewportDim);
    }

    /// @brief Allocate the three yadif history textures and the small
    ///        FBO used to blit captured frames into them. The textures
    ///        are sized to match the current capture raster, in the
    ///        same RGBA8 macropixel layout used by SharedTextureRing.
    ///        Idempotent for the GL side: if the existing allocation
    ///        already matches (w, h, kind) the textures are not
    ///        reallocated, but the slot bookkeeping (m_yadifSlots) is
    ///        always reset so the buffered history from a prior format
    ///        does not leak across a raster change.
    ///        Must be called from the render thread with the primary
    ///        GL context current.
    /// @return True on success, false on GL allocation failure.
    bool EnsureYadifHistory(std::size_t widthPixels,
                            std::size_t heightPixels,
                            ajainput::PixelKind kind);

    /// @brief Release the yadif history textures and blit FBO. Safe to
    ///        call from any state; resets history bookkeeping so that
    ///        the next EnsureYadifHistory starts fresh.
    void ReleaseYadifHistory();

    /// @brief Copy the most recently acquired ring texture into the
    ///        next history slot via FBO blit and rotate the prev/cur/
    ///        next slot indices forward. Called from ProcessOpenGL
    ///        each time SharedTextureRing::Acquire returns a fresh
    ///        frame (i.e. one whose sequence has not been blitted yet).
    /// @param sourceTexture  The ring's GL texture handle for the
    ///                       freshly acquired frame.
    /// @param sourceSequence Per-frame monotonically-increasing
    ///                       identifier from the ring, used to gate
    ///                       repeat blits via YadifSlots::Advance.
    void BlitRingFrameIntoHistory(GLuint sourceTexture, std::uint64_t sourceSequence);

    /// @brief v0.32 per-field path: blit the freshly-republished ring
    ///        texture into the CURRENT "next" yadif history slot
    ///        without rotating. Called for second-field-of-frame (F1)
    ///        ring publishes, where the YADIF rotation already
    ///        happened on the same frame's F0 publish. Safe to call
    ///        even when the history isn't allocated; the function
    ///        bails out cleanly.
    /// @param sourceTexture  The ring's GL texture handle for the
    ///                       freshly acquired field publish.
    void BlitRingFieldIntoCurrentNextSlot(GLuint sourceTexture);

    /// @brief Low-level helper that performs the actual FBO blit from
    ///        a ring source texture to a specific history slot index.
    ///        Saves and restores GL FBO bindings around the work so
    ///        the surrounding render code doesn't observe a transient
    ///        state change. Used by both BlitRingFrameIntoHistory
    ///        (which calls Advance first) and
    ///        BlitRingFieldIntoCurrentNextSlot (which targets the
    ///        existing next slot directly, no advance).
    void BlitRingTextureToHistorySlot(GLuint sourceTexture, int destinationSlot);

    /// @brief v0.39: copy only the rows of one parity (0 = even/top,
    ///        1 = odd/bottom) from a ring texture into a history slot.
    ///        Used to repair a slot whose second-field re-blit was
    ///        missed (see FieldHistoryGate.h). Saves and restores
    ///        the GL state it touches. No-op when the copy shader is
    ///        not available.
    void CopyRingRowsIntoHistorySlot(GLuint sourceTexture,
                                     int    destinationSlot,
                                     int    rowParity);

    /// @brief Translate the m_ringDepth mirror float (option dropdown
    ///        value 0..5 representing depths 2..7) into the integer
    ///        ring frame count that AutoCirculate and the synthetic
    ///        producer's delay line both consume. Bounded 2..7 even
    ///        if the mirror somehow drifts out of range.
    int CurrentRingFrameCount() const noexcept
    {
        int d = static_cast<int>(m_ringDepth + 0.5f) + 2;
        if (d < 2) d = 2;
        if (d > 7) d = 7;
        return d;
    }

    /// @brief Translate the m_lowLatencyMode mirror float (FFGL bool
    ///        toggle, 0/1) into a bool.
    bool CurrentLowLatencyMode() const noexcept
    {
        return m_lowLatencyMode >= 0.5f;
    }

    /// @brief Pixel kind of the frames this clip draws (the shared
    ///        capture's; UYVY before it is attached).
    ajainput::PixelKind SourceKind() const noexcept
    {
        return m_source ? m_source->Kind() : ajainput::PixelKind::UYVY;
    }

    // The order Resolume lists the parameters in. Compositions store
    // values by index: once published, new parameters go at the end.
    enum ParamID : unsigned int
    {
        PID_InputSource      = 0,
        PID_DeviceIndex      = 1,
        PID_FormatInfo       = 2,
        PID_Resize           = 3,
        PID_NoSignal         = 4,
        PID_Colorimetry      = 5,
        PID_Deinterlace      = 6,
        PID_LowLatencyMode   = 7,
        PID_RingDepth        = 8,
        PID_TenBitMode       = 9,
        PID_ShowStats        = 10,
        PID_StatsFrames      = 11,
        PID_StatsDrops       = 12,
        PID_StatsRecoveries  = 13,
        PID_StatsIdleWakeups = 14,
        PID_Count
    };

    // Parameter mirrors. v0.41: the ones other threads read or write
    // (setters, the host's GUI thread, SyncFromSource on the render
    // thread) are atomics.
    std::atomic<float> m_inputSource { 0.0f };
    float m_showStats      = 0.0f;
    std::atomic<float> m_ringDepth { 0.0f };
    std::atomic<float> m_deviceIndex { 0.0f };  // scanner index 0..15, dropdown
    float m_colorimetry    = 0.0f;  // ColorimetryMode dropdown
    std::atomic<float> m_lowLatencyMode { 0.0f };  // bool toggle, 0 = off (AutoCirculate)
    float m_deinterlace    = 0.0f;  // DeinterlaceMode dropdown
    std::atomic<float> m_tenBitMode { 0.0f };  // bool toggle, 0 = 8-bit UYVY, 1 = 10-bit v210
    float m_noSignalMode   = 0.0f;  // v0.42: NoSignalMode dropdown
    float m_resizeMode     = 0.0f;  // v0.42: ajainput::ResizeMode dropdown (Fit)
    // v0.42 Resize: the GL's largest viewport side (GL_MAX_VIEWPORT_DIMS),
    // read in InitGL; 0 until then.
    int   m_maxViewportDim = 0;

    // v0.42 Hold Last Frame: a copy of this clip's last drawn picture, at
    // the host viewport's size, kept for the input it came from.
    GLuint   m_holdTex      = 0;
    GLuint   m_holdFBO      = 0;
    int      m_holdW        = 0;
    int      m_holdH        = 0;
    bool     m_holdValid    = false;
    int      m_holdInput    = 0;
    uint32_t m_holdDevice   = 0;
    unsigned m_holdInternal = 0;      // GL internal format of the copy
    // A viewport size whose hold target came out incomplete: not retried
    // (or logged) every frame, only after a resize.
    int      m_holdFailedW  = 0;
    int      m_holdFailedH  = 0;
    // Ring Depth, Low Latency Mode and 10-bit are capture settings: since
    // v0.41 they belong to the shared capture of the input and apply to
    // every clip of it (as Resolume's own live inputs do). The mirrors
    // above follow the shared values (SyncFromSource).

    // Stable storage for the "Format" read-only text parameter. The
    // FFGL contract for GetTextParameter is that the returned pointer
    // remains valid until the next GetTextParameter call on the same
    // index, which means we cannot return a stack temporary. The
    // buffer is re-populated on every GetTextParameter call from the
    // capture path's atomically-published state (LastVideoFormatRaw,
    // GetState), so it is always up to date with what the operator
    // would see in the on-screen overlay. Size matches the longest
    // format string we might produce ("synthetic 1920x1080@59.94"
    // type strings fit comfortably in 64 bytes).
    char  m_formatInfoBuf[160] = "";

    // v0.41 shared capture of (Device, Input). Only the render thread
    // assigns it, under m_sourceMutex; the render thread reads it freely.
    // Other threads (text parameters, getters, setters) read it under
    // m_sourceMutex through SourceForParamsLocked() and never take a
    // strong copy: the last reference must be dropped on the render
    // thread, where the capture's GL objects can be deleted.
    std::shared_ptr<ajainput::SharedCapture> m_source;
    std::mutex                            m_sourceMutex;
    /// The attached capture if it is the one of the current Device /
    /// Input parameters, else nullptr (not attached yet, or a switch is
    /// pending until the clip is next rendered). m_sourceMutex held.
    ajainput::SharedCapture* SourceForParamsLocked() const noexcept
    {
        if (!m_source || m_source->DeviceIndex() != DeviceIndexParam() ||
            m_source->InputNumber() != InputNumber())
            return nullptr;
        return m_source.get();
    }
    // What this clip has adopted from m_source (render thread).
    uint64_t m_seenFormatGeneration   = 0;
    uint64_t m_seenSettingsGeneration = 0;
    bool     m_settingsEventsPending  = false;
    // Last ring publish this clip fed into its YADIF history: freshness is
    // per clip, since another clip of the input may acquire first.
    uint64_t m_lastRingSequence       = ~0ull;

    // Per-stat buffers for the four read-only counter parameters
    // (PID_StatsFrames, PID_StatsDrops, PID_StatsRecoveries,
    // PID_StatsIdleWakeups). Each parameter has its own buffer so
    // that Resolume's GUI thread can call GetTextParameter for all
    // four in succession without one buffer overwriting the result
    // of the previous call (the FFGL char* contract is "valid until
    // the next call on the SAME index"; using one shared buffer would
    // make any caller that captures more than one pointer at a time
    // see the wrong value). 32 bytes per buffer fits any printable
    // 64-bit counter with room for thousand separators if we ever
    // add them.
    char  m_statsFramesBuf[32]      = "0";
    char  m_statsDropsBuf[32]       = "0";
    char  m_statsRecoveriesBuf[32]  = "0";
    char  m_statsIdleWakeupsBuf[32] = "0";

    // Format of the frames this clip draws: a copy of the shared
    // capture's, refreshed when its FormatGeneration changes. Shader
    // uniforms, colorimetry Auto and the YADIF history follow it.
    ajainput::CapturedFormat m_currentFormat = ajainput::SyntheticFallbackFormat();

    ajainput::Overlay                 m_overlay;

    // OpenGL resources for the main render pass.
    ffglex::FFGLShader     m_shader;
    ffglex::FFGLScreenQuad m_quad;

    // Cached uniform locations.
    GLint m_uTexture        = -1;
    GLint m_uImageSize      = -1;
    GLint m_uColorNormalize = -1;  // (yOffset, yScale, cOffset, cScale)
    GLint m_uColorMatrix    = -1;  // (krCr, kgCb, kgCr, kbCb)
    GLint m_uHDRMode        = -1;  // 0 = SDR, 1 = PQ (ST.2084), 2 = HLG
    GLint m_uPixelFormatMode = -1; // 0 = 8-bit UYVY, 1 = 10-bit v210
    GLint m_uInterlaceMode  = -1;  // 0 = weave, 1 = yadif
    GLint m_uFieldParity    = -1;  // 0 = top field, 1 = bottom field
    GLint m_uPrevTexture    = -1;  // previous captured frame (yadif input)
    GLint m_uNextTexture    = -1;  // next captured frame    (yadif input)
    GLint m_uTopFieldFirst  = -1;  // 1 = top field first, 0 = bottom field first

    // YADIF history. Yadif (Yet Another DeInterlacing Filter) is a
    // motion-adaptive deinterlacer that combines an edge-directed spatial
    // predictor with a temporal predictor based on the previous, current,
    // and next interlaced frames. Real motion detection requires those
    // three frames; a single-frame heuristic (as the prior "motion
    // adaptive" iteration tried) cannot distinguish actual scene motion
    // from the structural alternation of fields, so it produces flicker
    // on fine static detail.
    //
    // The ring (SharedTextureRing) only guarantees one acquirable texture
    // at a time and reuses its three slots for the writer's next upload,
    // so we cannot safely hold three of the ring's GLuint handles at once.
    // Instead we keep three textures of our own, sized to the captured
    // raster, and on every new acquired frame we blit the ring's texture
    // into the oldest history slot via a small FBO + glBlitFramebuffer.
    // The cost is one GPU-side ~4 MB blit per arrived frame (sub-
    // millisecond on the 4080), and the gain is field-pure access to
    // three temporal samples for the yadif shader.
    //
    // Rendering uses history[m_yadifSlots.curSlot] (one frame behind the most
    // recently acquired one) so that the "next" frame is already
    // available, adding ~1/sourceRate of display latency relative to the
    // weave path (≈33 ms at 30 Hz captured / 1080i60).
    GLuint m_yadifHistoryTex[3]   = { 0, 0, 0 };
    // Two FBOs: one bound to GL_READ_FRAMEBUFFER with the ring's
    // texture attached as the blit source, the other bound to
    // GL_DRAW_FRAMEBUFFER with one of the history textures attached
    // as the blit destination. Two objects are required because
    // GL_READ_FRAMEBUFFER and GL_DRAW_FRAMEBUFFER are binding points,
    // not separate attachment storage — a single FBO bound to both
    // has only one COLOR_ATTACHMENT0 slot, so attaching source and
    // destination to "the same point" via the two targets is a
    // no-op (the second attach overwrites the first; the blit ends
    // up copying the destination onto itself).
    GLuint m_yadifBlitReadFBO     = 0;
    GLuint m_yadifBlitDrawFBO     = 0;
    // One-shot log gate for blit FBO completeness errors. Set to
    // true after the first incomplete-FBO log line of the current
    // allocation, cleared in EnsureYadifHistory so a re-allocation
    // re-arms the diagnostic. Prevents a per-frame error from
    // flooding the host log if the GL implementation rejects the
    // attachment combination for some reason.
    bool   m_yadifBlitCompleteLogged = false;
    ajainput::YadifSlots m_yadifSlots;     // pure rotation bookkeeping
    std::size_t m_yadifHistoryW   = 0;  // dimensions of the allocated history textures
    std::size_t m_yadifHistoryH   = 0;
    ajainput::PixelKind m_yadifHistoryKind = ajainput::PixelKind::UYVY;

    // Bob de-interlace field-parity tracking. The shader expects
    // FieldParity to alternate 0 ↔ 1 across consecutive render calls,
    // with each new captured frame starting at parity 0 (top field
    // for upper-field-first sources). Tying parity to a simple render
    // counter desynchronises against the capture stream whenever the
    // render rate is not exactly twice the capture rate — Resolume's
    // 60 Hz render against an interlaced 30 Hz source drifts in
    // practice, producing the "second hand jumps backwards once every
    // few seconds" motion artefact (top→bottom→top→bottom→top→...
    // is the desired temporal order; render drift turns it into
    // top→bottom→top→top→bottom→... which decodes as a backwards
    // step then a double-forward step).
    //
    // The fix is to reset the tick on every NEW captured frame and
    // alternate within the same captured frame. The captured frame
    // count is SharedCapture::CapturedFrameCount(): AJACaptureSource::
    // FramesCaptured() on the AJA path, the ring's upload count on the
    // synthetic path. Both increment monotonically per arrived frame.
    std::uint64_t m_lastCaptureFrameForField = ~0ull;
    std::uint64_t m_fieldTickIndex            = 0;

    // v0.32: last FieldHint observed on a fresh ring acquire. When the
    // ring is publishing per-field (FreshTop / FreshBottom), this
    // overrides the counter-based FieldParity logic above so that the
    // shader's FieldParity uniform always matches the freshly-written
    // field of the most recent publish. Reset to FullFrame on every
    // EnsureYadifHistory (which runs on raster changes) so a leftover
    // hint from a previous session doesn't influence the first render
    // of the next. When the value is FullFrame, the existing counter-
    // based logic stays in effect (used by frame-mode capture and
    // legacy paths).
    ajainput::SharedTextureRing::FieldHint m_lastFreshFieldHint =
        ajainput::SharedTextureRing::FieldHint::FullFrame;

    // Rotation gate for the YADIF history (v0.32 sequence-based
    // rotation, v0.39 missed-second-field repair). Rotates when a
    // publish carries a new source-frame sequence, re-blits the same
    // slot for the second field of the frame, and asks for a row-parity
    // repair when that second field was missed. Pure logic, unit-tested
    // in tests/test_FieldHistoryGate.cpp; see FieldHistoryGate.h for the
    // reasoning. Reset in EnsureYadifHistory.
    ajainput::FieldHistoryGate m_fieldGate;

    // Row-parity copy pass used by the repair (texelFetch + discard).
    // Compile failure only disables the repair.
    ffglex::FFGLShader m_fieldCopyShader;
    GLint              m_uFieldCopySource      = -1;
    GLint              m_uFieldCopyParity      = -1;

    // Consumer-side timing and frame-rate tracking.
    std::chrono::steady_clock::time_point m_fpsWindowStart;
    std::size_t                           m_fpsFrameCount = 0;
    float                                 m_fpsCurrent    = 0.0f;

    // Set to true at the end of InitGL and back to false at the start of
    // DeInitGL. Used to gate parameter handlers that have side effects
    // (opening the AJA device, starting a producer thread): when Resolume
    // applies saved parameter values immediately after constructing the
    // plugin but before calling InitGL, those handlers must NOT touch
    // the pipeline yet because the shared GL context, texture ring and
    // upload pipeline are not initialised. The handlers still update the
    // mirrored parameter floats so InitGL sees the right values when it
    // does the real allocation; the side effects are deferred to InitGL
    // itself, which does the same setup work and is the canonical entry
    // point for it.
    bool                                  m_initGLDone    = false;

    // Counter incremented on every FPS-recompute window (~0.5 s);
    // every 4 ticks (~2 s) the render path emits a one-line pipeline
    // diagnostic to the Resolume log. Cheap, bounded, and the fastest
    // way to confirm whether the producer is publishing and the
    // consumer is acquiring from the field.
    int                                   m_diagLogTick   = 0;

    // End-to-end latency tracking. On every render with a valid frame
    // we sample (now - producedAt) — the time from when the capture
    // thread copied the DMA buffer to when ProcessOpenGL bound the
    // texture for drawing. This is the latency the user perceives
    // (modulo Resolume's own render queue and the display chain
    // downstream, which add fixed offsets). Reset on each diag emit.
    double                                m_latencySumMs  = 0.0;
    double                                m_latencyMaxMs  = 0.0;
    std::size_t                           m_latencyCount  = 0;
    // Number of frames in the current diag window where Acquire saw a
    // still-pending upload fence (i.e. the PBO→VRAM transfer had not
    // retired on the GPU when the render thread got to the frame).
    // Emitted as a percentage alongside the latency stats.
    std::size_t                           m_fencePendingCount = 0;
};
