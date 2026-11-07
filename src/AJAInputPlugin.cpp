// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    AJAInputPlugin.cpp
 * @brief   Implementation of the FFGL Source plugin for AJA capture
 *          cards ("AJA Input" in Resolume).
 *
 * One instance per Resolume clip. Frames come from the ajainput::SharedCapture
 * of the clip's (Device, Input), shared by every clip of that input
 * (v0.41, see SharedCapture.h): a capture thread (AJA AutoCirculate, the
 * manual low-latency path, or the synthetic generator when no card opens)
 * publishes into a triple-buffered texture ring through a shared GL
 * context, or into a byte queue uploaded on the render thread.
 *
 * The render thread, driven by Resolume's ProcessOpenGL calls, acquires
 * the newest frame, feeds the clip's YADIF history for interlaced
 * sources, and draws it with the YCbCr-to-RGB fragment shader using the
 * clip's own colorimetry and de-interlace settings.
 */

#include "AJAInputPlugin.h"

#include "CaptureTopology.h"
#include "InputId.h"
#include "PixelFormat.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace ffglex;

// =============================================================================
// Colorimetry resolution.
// =============================================================================

namespace
{

/// @brief 4-vec packing for the shader's `ColorNormalize` uniform:
///        (yOffset, yScale, cOffset, cScale).
struct ColorNormalize
{
    float yOffset;
    float yScale;
    float cOffset;
    float cScale;
};

/// @brief 4-vec packing for the shader's `ColorMatrix` uniform:
///        (krCr, kgCb, kgCr, kbCb).
struct ColorMatrix
{
    float krCr;
    float kgCb;
    float kgCr;
    float kbCb;
};

/// @brief Resolve "Auto" to a concrete colorimetry mode based on the
///        active format. SD (525-line and 625-line) gets BT.601
///        limited; everything else gets BT.709 limited. Anything HD
///        and above defaults to BT.709 to match how broadcast cameras
///        and switchers describe their output.
///
/// v0.42: an HDR transfer the source signals (SDI payload ID, HDMI
/// Dynamic Range and Mastering InfoFrame) wins: 1 = PQ, 2 = HLG, both
/// over BT.2020 primaries.
AJAInputPlugin::ColorimetryMode ResolveAuto(const ajainput::CapturedFormat& fmt,
                                         int signalledTransfer) noexcept
{
    if (signalledTransfer == 1)
        return AJAInputPlugin::ColorimetryMode::PQ_BT2020;
    if (signalledTransfer == 2)
        return AJAInputPlugin::ColorimetryMode::HLG_BT2020;
    // 525-line (NTSC) is height==486 active; 625-line (PAL) is
    // height==576 active. Anything taller is HD or above. We are
    // generous with the threshold (≤ 576) because some SD formats
    // present slightly different active heights in different drivers.
    if (fmt.height <= 576)
        return AJAInputPlugin::ColorimetryMode::BT601Limited;
    return AJAInputPlugin::ColorimetryMode::BT709Limited;
}

/// @brief Build the (yOffset, yScale, cOffset, cScale) tuple for the
///        chosen quantisation range AND bit depth. The shader receives
///        the Y / Cb / Cr code values as scalars in their native code
///        space (0..255 for UYVY, 0..1023 for V210) and applies the
///        offset+scale identically; this helper just provides the
///        right code-space tuple.
///
/// For 8-bit (UYVY):
///   limited range: Y in 16..235 (219 codes), CbCr in 16..240 around 128.
///   full range:    Y in 0..255, CbCr in 0..255 around 128.
///
/// For 10-bit (V210): the same code positions scaled by 4×.
///   limited range: Y in 64..940 (876 codes), CbCr in 64..960 around 512.
///   full range:    Y in 0..1023, CbCr in 0..1023 around 512.
///
/// PQ and HLG signals on SDI are by convention narrow-range (sample
/// codes occupy the 64..940 / 64..960 region in 10-bit, which maps to
/// 16..235 / 16..240 in 8-bit). They are NOT full-range. So PQ/HLG
/// fall into the "narrow range" branch alongside the SDR
/// limited-range modes.
ColorNormalize NormalizeForMode(AJAInputPlugin::ColorimetryMode m,
                                ajainput::PixelKind k) noexcept
{
    const bool isFull =
        (m == AJAInputPlugin::ColorimetryMode::BT709Full) ||
        (m == AJAInputPlugin::ColorimetryMode::BT601Full);
    const bool is10Bit = (k == ajainput::PixelKind::V210);

    ColorNormalize n;
    if (is10Bit)
    {
        n.cOffset = 512.0f;
        if (isFull)
        {
            n.yOffset = 0.0f;
            n.yScale  = 1.0f / 1023.0f;
            n.cScale  = 1.0f / 1023.0f;
        }
        else
        {
            n.yOffset = 64.0f;
            n.yScale  = 1.0f / 876.0f;
            n.cScale  = 1.0f / 896.0f;
        }
    }
    else
    {
        n.cOffset = 128.0f;
        if (isFull)
        {
            n.yOffset = 0.0f;
            n.yScale  = 1.0f / 255.0f;
            n.cScale  = 1.0f / 255.0f;
        }
        else
        {
            n.yOffset = 16.0f;
            n.yScale  = 1.0f / 219.0f;
            n.cScale  = 1.0f / 224.0f;
        }
    }
    return n;
}

/// @brief Per-coefficient matrix for the chosen primaries.
///   - BT.709 (1.5748 / -0.1873 / -0.4681 / 1.8556) — HD/4K SDR convention.
///   - BT.601 (1.4020 / -0.3441 / -0.7141 / 1.7720) — SD SDR convention.
///   - BT.2020 (1.4746 / -0.1646 / -0.5714 / 1.8814) — HDR / wide-gamut
///     for both PQ and HLG.
ColorMatrix MatrixForMode(AJAInputPlugin::ColorimetryMode m) noexcept
{
    const bool is601 =
        (m == AJAInputPlugin::ColorimetryMode::BT601Limited) ||
        (m == AJAInputPlugin::ColorimetryMode::BT601Full);
    const bool isHDR =
        (m == AJAInputPlugin::ColorimetryMode::PQ_BT2020) ||
        (m == AJAInputPlugin::ColorimetryMode::HLG_BT2020);
    ColorMatrix mat;
    if (isHDR)
    {
        // BT.2020 Y'CbCr → R'G'B' matrix (non-constant-luminance).
        // Coefficients derived from kR=0.2627, kG=0.6780, kB=0.0593.
        mat.krCr =  1.4746f;
        mat.kgCb = -0.1646f;
        mat.kgCr = -0.5714f;
        mat.kbCb =  1.8814f;
    }
    else if (is601)
    {
        mat.krCr =  1.4020f;
        mat.kgCb = -0.3441f;
        mat.kgCr = -0.7141f;
        mat.kbCb =  1.7720f;
    }
    else
    {
        mat.krCr =  1.5748f;
        mat.kgCb = -0.1873f;
        mat.kgCr = -0.4681f;
        mat.kbCb =  1.8556f;
    }
    return mat;
}

/// @brief Translate the colorimetry mode to the HDR-mode shader uniform.
///        0 = SDR (linear from gamma-encoded sample, no extra processing).
///        1 = PQ (ST.2084): apply inverse PQ EOTF, tone-map to display,
///            convert BT.2020 → BT.709.
///        2 = HLG (ITU-R BT.2100): apply inverse HLG OETF + system gamma,
///            tone-map, convert BT.2020 → BT.709.
int HDRModeFor(AJAInputPlugin::ColorimetryMode m) noexcept
{
    switch (m)
    {
        case AJAInputPlugin::ColorimetryMode::PQ_BT2020:  return 1;
        case AJAInputPlugin::ColorimetryMode::HLG_BT2020: return 2;
        default:                                       return 0;
    }
}

} // namespace

// =============================================================================
// Plugin registration with the FFGL host.
// =============================================================================

// Single source of truth for the plugin version: feeds both the FFGL
// registration below and the overlay's version line.
static constexpr int kPluginVersionMajor = 0;
static constexpr int kPluginVersionMinor = 43;

// The unique ID is what the host keys saved compositions on: it must not
// change once published.
static CFFGLPluginInfo PluginInfo(
    PluginFactory< AJAInputPlugin >,
    "AJIN",
    "AJA Input",
    2, 1,
    kPluginVersionMajor, kPluginVersionMinor,
    FF_SOURCE,
    "Low-latency live input from AJA capture cards (KONA, Corvid, Io).",
    "AJA-Input-FFGL (c) 2026 Joaquin Villodre, LGPL-2.1-or-later"
);

// =============================================================================
// Input labels (v0.40, used by the multi-format rate warning).
// =============================================================================

/// Upper-case label for the inspector and log ("SDI1".."SDI8", "HDMI",
/// "HDMI2".."HDMI4", "SDI1-4"; InputId.h).
static const char* InputLabelUpper(int inputNumber) noexcept
{
    return ajainput::input::LabelUpper(inputNumber);
}

/// Lower-case label for the overlay.
static const char* InputLabelLower(int inputNumber) noexcept
{
    return ajainput::input::LabelLower(inputNumber);
}

/// v0.42: "fs1", "fs1+2", "fs1-4" for the overlay.
static void FrameStoreSetLower(uint32_t mask, char* buf, std::size_t size) noexcept
{
    ajainput::FormatFrameStoreSet(mask, buf, size);
    for (char* c = buf; *c != '\0'; ++c)
        if (*c >= 'A' && *c <= 'Z')
            *c = static_cast<char>(*c - 'A' + 'a');
}

/// v0.40: text for AJACaptureSource::RecentForeignUse() bits, most
/// important first; nullptr when nothing is reported. The lower-case
/// variant only uses characters the overlay font has.
static const char* ForeignUseText(uint32_t mask, bool upperCase) noexcept
{
    using C = ajainput::AJACaptureSource;
    if (mask & C::kForeignSdiOutput)
        return upperCase ? "input connector is an SDI output of another app"
                         : "input is an sdi output of another app";
    if (mask & C::kForeignFrameStorePlayout)
        return upperCase ? "FrameStore used for output by another app"
                         : "framestore used for output by another app";
    if (mask & C::kForeignTaskMode)
        return upperCase ? "AJA services were controlling the card"
                         : "aja services were controlling the card";
    if (mask & C::kForeignFrameStoreCapture)
        return upperCase ? "FrameStore was capturing for another app"
                         : "framestore was capturing for another app";
    return nullptr;
}

/// Short format name for a raw NTV2VideoFormat, or "?".
static std::string FormatNameForRaw(int raw)
{
    ajainput::CapturedFormat f;
    if (raw != 0 && ajainput::TryDecodeNTV2Format(raw, f) && !f.name.empty())
        return f.name;
    return "?";
}

// =============================================================================
// Shaders.
// =============================================================================

static const char vertexShaderCode[] = R"(#version 410 core
layout(location = 0) in vec4 vPosition;
layout(location = 1) in vec2 vUV;
out vec2 uv;
void main() {
    gl_Position = vPosition;
    uv = vUV;
}
)";

// v0.39 field-row copy pass (see AJAInputPlugin::CopyRingRowsIntoHistorySlot).
// Drawn into a history slot at the slot's own size, so gl_FragCoord maps
// 1:1 onto destination texels and texelFetch reads the same texel from
// the source; rows of the other parity are left untouched.
static const char fieldCopyFragmentShaderCode[] = R"(#version 410 core
out vec4 fragColor;
uniform sampler2D Source;
uniform int RowParity;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    if ((p.y & 1) != RowParity)
        discard;
    fragColor = texelFetch(Source, p, 0);
}
)";

static const char fragmentShaderCode[] = R"(#version 410 core
in  vec2 uv;
out vec4 fragColor;

// Source captured frames. CurTexture is the frame whose row layout the
// shader is reconstructing this draw call; PrevTexture and NextTexture
// are the previous and next captured frames in temporal order, used by
// the yadif deinterlacer. For progressive sources or weave mode, only
// CurTexture is sampled; the other two units are bound to the same
// texture as CurTexture by the C++ side so that any sample from them
// produces benign data (their reads are also gated by the InterlaceMode
// branch).
uniform sampler2D CurTexture;
uniform sampler2D PrevTexture;
uniform sampler2D NextTexture;
uniform vec2      ImageSize;

// Colorimetry. The Y, Cb, Cr components arrive from the UYVY texture as
// 8-bit values in the 0..1 range (one fragment of an RGBA8 macropixel
// per sample). We multiply back up to 0..255, subtract the digital
// zero point, scale to the full normalised range, and apply the
// per-coefficient YCbCr -> R'G'B' matrix.
//
// Two uniforms pack everything:
//   ColorNormalize = (yOffset, yScale, cOffset, cScale)
//     limited range: yOffset=16,  yScale=1/219, cOffset=128, cScale=1/224
//     full range:    yOffset=0,   yScale=1/255, cOffset=128, cScale=1/255
//   ColorMatrix    = (krCr, kgCb, kgCr, kbCb)
//     BT.709: ( 1.5748, -0.1873, -0.4681,  1.8556)
//     BT.601: ( 1.4020, -0.3441, -0.7141,  1.7720)
uniform vec4      ColorNormalize;
uniform vec4      ColorMatrix;

// HDR transfer function selector.
//   0 = SDR (no extra processing; the sampled RGB is treated as display-
//       ready Rec.709 / Rec.601 R'G'B' and clamped to [0, 1]).
//   1 = PQ (ST.2084): the sampled values are interpreted as gamma-encoded
//       BT.2020 R'G'B'; the inverse PQ EOTF is applied to recover
//       display-referred linear light (0..1 normalised to a 10,000-nit
//       peak), then the result is tone-mapped to the SDR range and the
//       BT.2020 → BT.709 colour conversion matrix is applied.
//   2 = HLG (ITU-R BT.2100): the sampled values are interpreted as the
//       hybrid log-gamma transfer; the inverse HLG OETF is applied to
//       recover a scene-referred 0..12 normalised value; then the same
//       tone-map + gamut conversion as PQ.
//
// Caveat reminder for 8-bit input: PQ specifically maps 8-bit code 235
// to ~93 nits. Anything brighter than that on the original source is
// already clipped at the cable. The HDR-capable code path here is
// architectural prep for 10-bit capture (v0.29.2); against an actual
// 8-bit PQ signal the highlights look compressed but at least the
// midtones map closer to what the operator's eyes expect than would
// treating the whole signal as Rec.709.
uniform int       HDRMode;

// De-interlace mode. The capture path delivers interlaced sources as
// a woven frame (top and bottom fields interleaved line-by-line in a
// single full-height raster), which produces classic combing on motion.
// Mode 0 keeps the raw weave - correct for progressive sources, wrong
// for interlaced but cheap. Mode 1 is YADIF (Yet Another DeInterlacing
// Filter), a motion-adaptive deinterlacer combining an edge-directed
// spatial predictor (best diagonal coherence between the two same-
// field rows immediately above and below the reconstructed row) with
// a temporal predictor (plausible-motion clamp derived from prev/cur/
// next frames). The renderer alternates which field is preserved on
// successive ProcessOpenGL calls via FieldParity, doubling the
// effective frame rate against the source - same temporal behaviour
// as plain bob, but the spatial reconstruction is dramatically better
// on fine static detail and the temporal clamp suppresses the inter-
// line flicker that single-frame heuristics cannot. The cost is one
// additional frame of latency (we render the second-most-recent frame
// so the "next" temporal sample is available) and ~26 texture lookups
// per reconstructed pixel.
//
// Mode 2 (v0.40, "Low Latency" toggle on interlaced UYVY) is the same
// algorithm without lookahead: it shows the newest field and fills the
// missing rows from the field before it, one source frame less latency
// than mode 1 (see yadif_temporal_past).
//
// Implementation derived from FFmpeg's vf_yadif_cuda.cu (Copyright
// (c) 2018 Philip Langdale) and libplacebo's GLSL port, both LGPL-2.1.
//
// `FieldParity` is 0 (preserve top field rows = even rows) or 1
// (preserve bottom field rows = odd rows). The C++ side toggles it
// every render call. `TopFieldFirst` is 1 when the source is top-
// field-first (HD 1080i default) and 0 for bottom-field-first (NTSC
// 480i default); it controls which of prev/cur/next is sampled at
// the same-field temporal offsets.
uniform int       InterlaceMode;
uniform int       FieldParity;
uniform int       TopFieldFirst;

// Source pixel kind.
//   0 = 8-bit UYVY (one RGBA8 texel = one macropixel = two pixels).
//       The original code path; latency and quality unchanged from
//       v0.29.1 and prior.
//   1 = 10-bit packed v210 (4 RGBA8 texels = 4 32-bit words = 6 pixels).
//       Each 32-bit word carries 3 × 10-bit samples (LSB-first, with
//       2 bits of zero padding at the top); the shader reconstructs
//       each word from the texel's 4 bytes, then masks out the
//       relevant 10-bit slice depending on the pixel's position
//       within its 6-pixel group. ColorNormalize must be passed in
//       10-bit code space (yOffset=64 for limited-range, scale=1/876
//       etc.) — the C++ side selects the right tuple automatically
//       when this mode is non-zero.
//
//       EXPERIMENTAL: pending hardware validation. If the operator's
//       Kona X uses a different byte/bit order for
//       NTV2_FBF_10BIT_YCBCR than standard SMPTE v210, the image will
//       appear colour-shifted or striped horizontally. Workaround:
//       leave the "10-bit capture" UI toggle off.
uniform int       PixelFormatMode;

// =====================================================================
// HDR helper functions. These are no-ops when HDRMode == 0; the optimizer
// elides them on the SDR code path because the branch is constant per
// draw call.
// =====================================================================

// Inverse PQ EOTF (ST.2084). Input is normalised 0..1 (the gamma-encoded
// signal); output is normalised linear-light 0..1 where 1.0 corresponds
// to the 10,000-nit reference peak.
vec3 pq_inverse_eotf(vec3 E) {
    const float m1 = 0.1593017578125;   // 2610 / 4096 / 4
    const float m2 = 78.84375;          // 2523 / 4096 * 128
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 Ep   = pow(max(E, 0.0), vec3(1.0 / m2));
    vec3 num  = max(Ep - c1, 0.0);
    vec3 den  = max(c2 - c3 * Ep, 1e-6);
    return pow(num / den, vec3(1.0 / m1));
}

// Inverse HLG OETF (ITU-R BT.2100). Input is normalised 0..1; output
// is scene-referred linear 0..1 representing 0..12 normalised.
vec3 hlg_inverse_oetf(vec3 E) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 lo = (E * E) / 3.0;
    vec3 hi = (exp((E - c) / a) + b) / 12.0;
    return mix(lo, hi, step(0.5, E));
}

// BT.2020 → BT.709 colour conversion in linear light. The result may
// contain negative values for colours that lie outside the BT.709
// gamut; the caller clamps before output. Matrix from BT.2087.
vec3 bt2020_to_bt709(vec3 c) {
    return vec3(
         1.6605 * c.r - 0.5876 * c.g - 0.0728 * c.b,
        -0.1246 * c.r + 1.1329 * c.g - 0.0083 * c.b,
        -0.0182 * c.r - 0.1006 * c.g + 1.1187 * c.b
    );
}

// Simple Reinhard tone map. Scales each channel by a single ratio
// derived from the maximum component, preserving hue at the cost of
// some saturation loss in saturated highlights. Adequate for a first
// pass; v0.30+ can swap in something more elaborate (BT.2390 or ACES)
// without disturbing anything else here.
vec3 tone_map_reinhard(vec3 c) {
    float L  = max(max(c.r, c.g), c.b);
    float Ld = L / (1.0 + L);
    return c * (Ld / max(L, 1e-6));
}

// Approximate sRGB encoding via pow(L, 1/2.2). The exact piecewise sRGB
// curve is more correct in the deep shadows but the difference is
// invisible against typical Resolume composite output, and the pow
// form is one shader instruction instead of three.
vec3 srgb_encode(vec3 L) {
    return pow(max(L, 0.0), vec3(1.0 / 2.2));
}

// =====================================================================
// YADIF deinterlace helpers. Ported from FFmpeg's vf_yadif.c (Copyright
// (C) 2006-2011 Michael Niedermayer, 2010 James Darnley) and
// vf_yadif_cuda.cu (Copyright (C) 2018 Philip Langdale), by way of
// libplacebo's GLSL version (src/shaders/deinterlacing.c); all three are
// LGPL-2.1-or-later, as this project (THIRD_PARTY_NOTICES.md). The
// algorithm operates vector-wise on RGBA8 macropixels (one texel = one
// UYVY pair = two image pixels in horizontal), so each of the four
// channels (U, Y0, V, Y1) independently selects its best spatial edge
// direction and is independently clamped to its plausible temporal
// range. Spatial
// offsets are expressed in macropixel units (±1..±3 macropixels =
// ±2..±6 image pixels), a slightly coarser horizontal window than
// per-pixel yadif but indistinguishable for HD interlaced sources
// where the structure that bob/yadif is trying to recover is the
// VERTICAL line resolution.
// =====================================================================

vec4 yadif_spatial(int xMacro, int yo) {
    int W  = int(ImageSize.x * 0.5);
    int H  = int(ImageSize.y);
    int yU = clamp(yo - 1, 0, H - 1);
    int yD = clamp(yo + 1, 0, H - 1);

    // Horizontal clamping for ±3 macropixel reach.
    int xm3 = clamp(xMacro - 3, 0, W - 1);
    int xm2 = clamp(xMacro - 2, 0, W - 1);
    int xm1 = clamp(xMacro - 1, 0, W - 1);
    int xp0 = clamp(xMacro    , 0, W - 1);
    int xp1 = clamp(xMacro + 1, 0, W - 1);
    int xp2 = clamp(xMacro + 2, 0, W - 1);
    int xp3 = clamp(xMacro + 3, 0, W - 1);

    // Seven samples on the row above (a..g), seven on the row below
    // (h..n). Mirrored naming from the reference CUDA source.
    vec4 sa = texelFetch(CurTexture, ivec2(xm3, yU), 0);
    vec4 sb = texelFetch(CurTexture, ivec2(xm2, yU), 0);
    vec4 sc = texelFetch(CurTexture, ivec2(xm1, yU), 0);
    vec4 sd = texelFetch(CurTexture, ivec2(xp0, yU), 0);
    vec4 se = texelFetch(CurTexture, ivec2(xp1, yU), 0);
    vec4 sf = texelFetch(CurTexture, ivec2(xp2, yU), 0);
    vec4 sg = texelFetch(CurTexture, ivec2(xp3, yU), 0);

    vec4 sh = texelFetch(CurTexture, ivec2(xm3, yD), 0);
    vec4 si = texelFetch(CurTexture, ivec2(xm2, yD), 0);
    vec4 sj = texelFetch(CurTexture, ivec2(xm1, yD), 0);
    vec4 sk = texelFetch(CurTexture, ivec2(xp0, yD), 0);
    vec4 sl = texelFetch(CurTexture, ivec2(xp1, yD), 0);
    vec4 sm = texelFetch(CurTexture, ivec2(xp2, yD), 0);
    vec4 sn = texelFetch(CurTexture, ivec2(xp3, yD), 0);

    // Baseline: vertical average, score over a three-tap horizontal
    // band centred on (d, k).
    vec4 spatial_pred  = (sd + sk) * 0.5;
    vec4 spatial_score = abs(sc - sj) + abs(sd - sk) + abs(se - sl);

    // Left diagonal level 1: candidate (c+l)/2, scored over (b..d, k..m).
    vec4  L1_score = abs(sb - sk) + abs(sc - sl) + abs(sd - sm);
    bvec4 L1_mask  = lessThan(L1_score, spatial_score);
    spatial_pred   = mix(spatial_pred,  (sc + sl) * 0.5, L1_mask);
    spatial_score  = mix(spatial_score, L1_score,        L1_mask);

    // Left diagonal level 2: candidate (b+m)/2. The reference is
    // nested inside the L1 conditional, so this only fires when L1
    // already won. We replicate the per-channel "AND" via a float
    // multiply on the mask vectors (GLSL has no `&&` for bvec).
    vec4  L2_score    = abs(sa - sl) + abs(sb - sm) + abs(sc - sn);
    vec4  L2_inner    = vec4(lessThan(L2_score, spatial_score));
    bvec4 L2_mask     = bvec4(L2_inner * vec4(L1_mask));
    spatial_pred   = mix(spatial_pred,  (sb + sm) * 0.5, L2_mask);
    spatial_score  = mix(spatial_score, L2_score,        L2_mask);

    // Right diagonal level 1: candidate (e+j)/2.
    vec4  R1_score = abs(sd - si) + abs(se - sj) + abs(sf - sk);
    bvec4 R1_mask  = lessThan(R1_score, spatial_score);
    spatial_pred   = mix(spatial_pred,  (se + sj) * 0.5, R1_mask);
    spatial_score  = mix(spatial_score, R1_score,        R1_mask);

    // Right diagonal level 2: candidate (f+i)/2, nested in R1.
    vec4  R2_score    = abs(se - sh) + abs(sf - si) + abs(sg - sj);
    vec4  R2_inner    = vec4(lessThan(R2_score, spatial_score));
    bvec4 R2_mask     = bvec4(R2_inner * vec4(R1_mask));
    spatial_pred   = mix(spatial_pred,  (sf + si) * 0.5, R2_mask);

    return spatial_pred;
}
)" R"(
vec4 yadif_temporal(vec4 spatial_pred,
                    int  xMacro,
                    int  yo,
                    int  parity,
                    int  tff)
{
    int W   = int(ImageSize.x * 0.5);
    int H   = int(ImageSize.y);
    int x   = clamp(xMacro, 0, W - 1);
    int yU  = clamp(yo - 1, 0, H - 1);
    int yD  = clamp(yo + 1, 0, H - 1);
    int y0_ = clamp(yo    , 0, H - 1);
    int yU2 = clamp(yo - 2, 0, H - 1);
    int yD2 = clamp(yo + 2, 0, H - 1);

    // Temporal field-order. The reference defines:
    //   is_second_field = !(parity ^ tff)
    // which evaluates to true when we are reconstructing the field
    // that arrives SECOND in time within the current frame. In that
    // case, the "previous-in-time" same-field is the OTHER field of
    // the current frame (prev1 = cur), and the "next-in-time" same-
    // field is the matching field of the NEXT frame (next1 = next).
    // Otherwise we are reconstructing the first-in-time field, and
    // prev1 = prev / next1 = cur.
    bool is_second_field = ((parity ^ tff) == 0);

    // Always-frame-bound samples: A, B come from prev (= "prev2"),
    // K, L come from next (= "next2"). F, G come from cur. These six
    // are at offsets (x, y-1) and (x, y+1).
    vec4 A = texelFetch(PrevTexture, ivec2(x, yU), 0);
    vec4 B = texelFetch(PrevTexture, ivec2(x, yD), 0);
    vec4 K = texelFetch(NextTexture, ivec2(x, yU), 0);
    vec4 L = texelFetch(NextTexture, ivec2(x, yD), 0);
    vec4 F = texelFetch(CurTexture,  ivec2(x, yU), 0);
    vec4 G = texelFetch(CurTexture,  ivec2(x, yD), 0);

    // Frame-conditional samples: C, D, E from prev1 at offsets
    // (x, y-2), (x, y), (x, y+2); H, I, J from next1 at the same
    // offsets. Branching on is_second_field is uniform across the
    // fragment (no warp divergence) because parity and tff are
    // uniforms.
    vec4 C, D, E, Hs, Is, J;
    if (is_second_field) {
        C  = texelFetch(CurTexture,  ivec2(x, yU2), 0);
        D  = texelFetch(CurTexture,  ivec2(x, y0_), 0);
        E  = texelFetch(CurTexture,  ivec2(x, yD2), 0);
        Hs = texelFetch(NextTexture, ivec2(x, yU2), 0);
        Is = texelFetch(NextTexture, ivec2(x, y0_), 0);
        J  = texelFetch(NextTexture, ivec2(x, yD2), 0);
    } else {
        C  = texelFetch(PrevTexture, ivec2(x, yU2), 0);
        D  = texelFetch(PrevTexture, ivec2(x, y0_), 0);
        E  = texelFetch(PrevTexture, ivec2(x, yD2), 0);
        Hs = texelFetch(CurTexture,  ivec2(x, yU2), 0);
        Is = texelFetch(CurTexture,  ivec2(x, y0_), 0);
        J  = texelFetch(CurTexture,  ivec2(x, yD2), 0);
    }

    // Plausible-motion bracket. p2 is the temporal average of the
    // two same-row samples (D and I, drawn from prev1 and next1):
    // this is the value spatial_pred is clamped around. p1, p3 are
    // the cur frame's two adjacent same-field samples; p0, p4 are
    // the temporal averages at the rows above and below.
    vec4 p0 = (C + Hs) * 0.5;
    vec4 p1 = F;
    vec4 p2 = (D + Is) * 0.5;
    vec4 p3 = G;
    vec4 p4 = (E + J)  * 0.5;

    // Temporal difference estimate. tdiff0 from prev1/next1, tdiff1
    // from prev/cur, tdiff2 from cur/next. The max is the conservative
    // motion bound; bracket-expansion below widens it when the local
    // intensity profile suggests a fast vertical edge.
    vec4 tdiff0 = abs(D - Is);
    vec4 tdiff1 = (abs(A - F) + abs(B - G)) * 0.5;
    vec4 tdiff2 = (abs(K - F) + abs(G - L)) * 0.5;
    vec4 diff   = max(max(tdiff0, tdiff1), tdiff2);

    // Bracket adjustment ("skip_spatial_check = false" path of the
    // reference). Always non-negative because diff starts as max of
    // non-negative quantities, and the max3(diff, mini, -maxi) below
    // can only grow it.
    vec4 maxi = max(max(p2 - p3, p2 - p1), min(p0 - p1, p4 - p3));
    vec4 mini = min(min(p2 - p3, p2 - p1), max(p0 - p1, p4 - p3));
    diff = max(max(diff, mini), -maxi);

    return clamp(spatial_pred, p2 - diff, p2 + diff);
}

// v0.40 low-latency variant (InterlaceMode == 2): YADIF without
// lookahead. Mode 1 shows the field one source frame behind the newest
// one, because its temporal predictor needs the field AFTER the one it
// reconstructs. Mode 2 instead shows the NEWEST field and fills the
// missing rows from the field just before it, gated by the same
// motion test (same-parity differences one frame apart) and the same
// bracket clamp; static detail is woven exactly as in mode 1, moving
// areas fall back to the edge-directed spatial prediction. The price
// is a temporal predictor centred half a field earlier (slightly more
// trailing on slow motion) and motion detected from the past only.
//
// Texture roles in this mode (bound by the C++ side):
//   CurTexture  = N, the newest history frame. Its rows of the output
//                 parity are the field being shown; its other rows are
//                 the field that arrived just before it (for the first
//                 field of a frame, the capture accumulator still holds
//                 the previous frame's second field there).
//   PrevTexture = the frame one source frame older than N.
//   NextTexture = the frame two source frames older than N.
vec4 yadif_temporal_past(vec4 spatial_pred,
                         int  xMacro,
                         int  yo,
                         int  parity,
                         int  tff)
{
    int W   = int(ImageSize.x * 0.5);
    int H   = int(ImageSize.y);
    int x   = clamp(xMacro, 0, W - 1);
    int yU  = clamp(yo - 1, 0, H - 1);
    int yD  = clamp(yo + 1, 0, H - 1);
    int y0_ = clamp(yo    , 0, H - 1);
    int yU2 = clamp(yo - 2, 0, H - 1);
    int yD2 = clamp(yo + 2, 0, H - 1);

    bool is_second_field = ((parity ^ tff) == 0);

    // Output-parity rows above/below: newest (F, G) and one frame
    // earlier (A, B).
    vec4 F = texelFetch(CurTexture,  ivec2(x, yU), 0);
    vec4 G = texelFetch(CurTexture,  ivec2(x, yD), 0);
    vec4 A = texelFetch(PrevTexture, ivec2(x, yU), 0);
    vec4 B = texelFetch(PrevTexture, ivec2(x, yD), 0);

    // Missing-parity rows of the field just before the output field.
    vec4 D  = texelFetch(CurTexture, ivec2(x, y0_), 0);
    vec4 C2 = texelFetch(CurTexture, ivec2(x, yU2), 0);
    vec4 E2 = texelFetch(CurTexture, ivec2(x, yD2), 0);

    // The same field one source frame earlier. For a second-field
    // output, D is this frame's first field and its predecessor sits
    // in PrevTexture; for a first-field output, D is the previous
    // frame's second field (also what PrevTexture holds), so its
    // predecessor is two frames back.
    vec4 X;
    if (is_second_field)
        X = texelFetch(PrevTexture, ivec2(x, y0_), 0);
    else
        X = texelFetch(NextTexture, ivec2(x, y0_), 0);

    vec4 p0 = C2;
    vec4 p1 = F;
    vec4 p2 = D;
    vec4 p3 = G;
    vec4 p4 = E2;

    // Same convention as yadif_temporal (tdiff0 not halved).
    vec4 tdiff0 = abs(D - X);
    vec4 tdiff1 = (abs(A - F) + abs(B - G)) * 0.5;
    vec4 diff   = max(tdiff0, tdiff1);

    vec4 maxi = max(max(p2 - p3, p2 - p1), min(p0 - p1, p4 - p3));
    vec4 mini = min(min(p2 - p3, p2 - p1), max(p0 - p1, p4 - p3));
    diff = max(max(diff, mini), -maxi);

    return clamp(spatial_pred, p2 - diff, p2 + diff);
}

vec4 yadif_macropixel(int xMacro, int yo, int parity, int tff) {
    vec4 sp = yadif_spatial(xMacro, yo);
    if (InterlaceMode == 2)
        return yadif_temporal_past(sp, xMacro, yo, parity, tff);
    return yadif_temporal(sp, xMacro, yo, parity, tff);
}

void main() {
    vec2 sampleUV = vec2(uv.x, 1.0 - uv.y);

    // Deinterlace strategy. The capture path delivers interlaced sources
    // as a woven buffer where rows alternate between top-field and
    // bottom-field content captured 1/60 s apart (UFF). Two strategies
    // are available:
    //
    //   InterlaceMode == 0 (weave): pass-through of the woven buffer.
    //     Correct for progressive sources, shows classic combing on
    //     motion for interlaced sources.
    //
    //   InterlaceMode == 1 (yadif): YADIF reconstruction for UYVY 8-bit
    //     sources, with simple bob fallback for V210 10-bit. The yadif
    //     path is implemented by yadif_macropixel() above and is the
    //     full motion-adaptive algorithm: edge-directed spatial
    //     predictor + plausible-motion temporal clamp using three
    //     captured frames. The bob fallback for V210 keeps the same
    //     field-pure interpolation as the legacy bob mode because the
    //     V210 byte-unpack does not map cleanly onto yadif's macropixel-
    //     coord sample pattern (each V210 fragment depends on a 4-texel
    //     group, not a single texel). Lifting that limitation is on the
    //     roadmap once the 10-bit path is hardware-validated.
    //
    // Both modes alternate FieldParity (0=preserve top rows / 1=preserve
    // bottom rows) on successive ProcessOpenGL calls so that each field
    // is shown at its own temporal instant, recovering the source's
    // 60 Hz motion.

    // Field-pure interpolation setup. Only used by the V210 path when
    // InterlaceMode == 1; the UYVY path uses yadif instead and ignores
    // these variables. Kept at no-op defaults so the InterlaceMode == 0
    // and PixelFormatMode == 0 progressive/weave path remains a single
    // bilinear sample.
    float gBobMix       = 0.0;
    float gBobUVBelowY  = sampleUV.y;
    float gBobUVAboveY  = sampleUV.y;
    int   gBobRowSnap   = 0;
    if (InterlaceMode == 1 && PixelFormatMode == 1) {
        float y_out_px     = sampleUV.y * ImageSize.y;
        float field_offset = (FieldParity == 0) ? 0.0 : 1.0;
        // Position of y_out_px in the current field's row-index space.
        // For top field: 0, 1, 2 = woven rows 0, 2, 4. For bottom: 0,
        // 1, 2 = woven rows 1, 3, 5.
        float field_pos    = (y_out_px - field_offset) * 0.5;
        float k_below      = floor(field_pos);
        gBobMix            = field_pos - k_below;          // 0..1
        float row_below    = 2.0 * k_below + field_offset;  // woven row index
        float row_above    = row_below + 2.0;
        // Edge handling. When one of the two same-field rows lies
        // outside the raster, collapse the sample pair to the valid
        // row instead of letting a generic clamp pull in a row from
        // the OPPOSITE field, which would taint the field-pure
        // sample with cross-field data at the edges.
        if (row_below < 0.0) {
            row_below = row_above;
            gBobMix   = 0.0;
        }
        if (row_above > ImageSize.y - 1.0) {
            row_above = row_below;
            gBobMix   = 0.0;
        }
        row_below = clamp(row_below, 0.0, ImageSize.y - 1.0);
        row_above = clamp(row_above, 0.0, ImageSize.y - 1.0);
        gBobUVBelowY = (row_below + 0.5) / ImageSize.y;
        gBobUVAboveY = (row_above + 0.5) / ImageSize.y;
        // For V210's nearest-row path: snap to the closer of the two.
        gBobRowSnap = int((gBobMix < 0.5) ? row_below : row_above);
    }
)" R"(
    // The three values yCode / cbCode / crCode are filled below in
    // their NATIVE code-space:
    //   - UYVY (8-bit): values in [0, 255]
    //   - V210 (10-bit): values in [0, 1023]
    // The ColorNormalize uniform is already populated by the C++ side
    // with the matching code-space tuple, so the rest of the shader
    // does not need to know the bit depth.
    float yCode, cbCode, crCode;

    if (PixelFormatMode == 0) {
        // --- UYVY (8-bit) macropixel path -----------------------
        // The texture is half-width: each RGBA8 texel holds one UYVY
        // macropixel = two image pixels.
        float imgX       = sampleUV.x * ImageSize.x;
        float macroX     = imgX * 0.5;
        float macroIdx   = floor(macroX);
        bool  rightPixel = fract(macroX) >= 0.5;

        int W = int(ImageSize.x * 0.5);
        int H = int(ImageSize.y);
        int xMacro = clamp(int(macroIdx), 0, W - 1);
        int yo     = clamp(int(sampleUV.y * ImageSize.y), 0, H - 1);

        vec4 mp;
        if (InterlaceMode == 1 || InterlaceMode == 2) {
            // YADIF path. Rows whose parity matches FieldParity are
            // taken straight from CurTexture (the "preserved" field of
            // this render). Other rows are reconstructed by
            // yadif_macropixel which combines an edge-directed spatial
            // predictor with a temporal clamp using prev/cur/next.
            if ((yo & 1) == FieldParity) {
                // Row parity matches ⇒ this row IS the preserved field
                // at this parity; pass through the cur sample. (Bitwise
                // test: integer division of a negative operand, as the
                // pre-v0.39 form did for yo = 0, is undefined in GLSL.)
                mp = texelFetch(CurTexture, ivec2(xMacro, yo), 0);
            } else {
                mp = yadif_macropixel(xMacro, yo, FieldParity, TopFieldFirst);
            }
        } else {
            // Weave / progressive: single bilinear sample of cur.
            float texU = (macroIdx + 0.5) / float(W);
            mp = texture(CurTexture, vec2(texU, sampleUV.y));
        }

        float u  = mp.r;
        float y0 = mp.g;
        float v  = mp.b;
        float y1 = mp.a;

        // The * 255.0 converts the sampler2D's [0,1] reading back to
        // the original 8-bit code value. ColorNormalize is in 8-bit
        // code space, so we end up doing (code - offset) * scale.
        float yNorm = rightPixel ? y1 : y0;
        yCode  = yNorm * 255.0;
        cbCode = u     * 255.0;
        crCode = v     * 255.0;
    } else {
        // --- V210 (10-bit packed) unpack ------------------------
        // Six image pixels are packed into 4 × 32-bit words = 16 bytes
        // = 4 RGBA8 texels. Bit layout per SMPTE standard v210
        // (LSB-first 10-bit samples, 2 bits of zero padding at top):
        //
        //   Word 0: bits[ 0: 9]=Cb0  bits[10:19]=Y0  bits[20:29]=Cr0
        //   Word 1: bits[ 0: 9]=Y1   bits[10:19]=Cb1 bits[20:29]=Y2
        //   Word 2: bits[ 0: 9]=Cr1  bits[10:19]=Y3  bits[20:29]=Cb2
        //   Word 3: bits[ 0: 9]=Y4   bits[10:19]=Cr2 bits[20:29]=Y5
        //
        // Pixel 0,1 share Cb0/Cr0; pixels 2,3 share Cb1/Cr1; pixels
        // 4,5 share Cb2/Cr2 (4:2:2 subsampling). Each RGBA8 texel
        // upload is interpreted as LITTLE-ENDIAN: texel.r = bits[0:7]
        // (lowest byte of the 32-bit word), texel.g = bits[8:15], and
        // so on. This matches AJA's DMA on Windows/x86_64 hosts; if
        // the operator's machine is big-endian (unlikely in 2026)
        // the byte recombination flips and the image will appear
        // colour-shifted.
        float imgX = sampleUV.x * ImageSize.x;
        int   col       = int(floor(imgX));
        int   groupIdx  = col / 6;
        int   inGroupIx = col - groupIdx * 6;  // = col % 6
        int   pairIdx   = inGroupIx / 2;        // 0, 1, 2

        // texelFetch wants integer pixel coords. The row choice
        // depends on whether bob deinterlace is active. With bob off
        // we use the standard nearest-row sampling at the floored
        // sampleUV.y, matching the progressive code path. With bob on
        // we snap to the nearest scanline of the *current* field
        // (gBobRowSnap, computed in the field-sample block earlier);
        // this preserves strict field separation under the bob
        // alternation and avoids both the vertical jitter and the
        // accidental cross-field leakage the previous formulation had.
        // V210 cannot do bilinear-style interpolation between two
        // same-field rows here because the byte unpack assumes a
        // single contiguous group of four texels per macropixel;
        // doing two reads + manual mix would mean unpacking two
        // independent 6-pixel groups every fragment, which is enough
        // extra work that we save it for a possible v0.30 cleanup.
        int row;
        if (InterlaceMode == 1) {
            row = gBobRowSnap;
        } else {
            row = int(sampleUV.y * ImageSize.y);
        }
        if (row < 0) row = 0;
        if (row >= int(ImageSize.y)) row = int(ImageSize.y) - 1;
        int colBase = groupIdx * 4;

        // Read the four texels making up this 6-pixel group.
        ivec2 c0 = ivec2(colBase + 0, row);
        ivec2 c1 = ivec2(colBase + 1, row);
        ivec2 c2 = ivec2(colBase + 2, row);
        ivec2 c3 = ivec2(colBase + 3, row);

        // 255*texel rounds to the original byte value. The +0.5
        // floor-correction handles the float-to-int truncation that
        // would otherwise turn e.g. 254.998 → 254.
        uvec4 b0 = uvec4(texelFetch(CurTexture, c0, 0) * 255.0 + 0.5);
        uvec4 b1 = uvec4(texelFetch(CurTexture, c1, 0) * 255.0 + 0.5);
        uvec4 b2 = uvec4(texelFetch(CurTexture, c2, 0) * 255.0 + 0.5);
        uvec4 b3 = uvec4(texelFetch(CurTexture, c3, 0) * 255.0 + 0.5);

        uint w0 = b0.r | (b0.g << 8) | (b0.b << 16) | (b0.a << 24);
        uint w1 = b1.r | (b1.g << 8) | (b1.b << 16) | (b1.a << 24);
        uint w2 = b2.r | (b2.g << 8) | (b2.b << 16) | (b2.a << 24);
        uint w3 = b3.r | (b3.g << 8) | (b3.b << 16) | (b3.a << 24);

        const uint M = 0x3FFu;

        // Luma is unique per pixel. Six pixels, six slots.
        uint yU;
        if      (inGroupIx == 0) yU = (w0 >> 10) & M;
        else if (inGroupIx == 1) yU =  w1        & M;
        else if (inGroupIx == 2) yU = (w1 >> 20) & M;
        else if (inGroupIx == 3) yU = (w2 >> 10) & M;
        else if (inGroupIx == 4) yU =  w3        & M;
        else                     yU = (w3 >> 20) & M;

        // Chroma is shared per pair: pixels 0/1, 2/3, 4/5.
        uint cbU, crU;
        if (pairIdx == 0) {
            cbU =  w0        & M;
            crU = (w0 >> 20) & M;
        } else if (pairIdx == 1) {
            cbU = (w1 >> 10) & M;
            crU =  w2        & M;
        } else {
            cbU = (w2 >> 20) & M;
            crU = (w3 >> 10) & M;
        }

        yCode  = float(yU);
        cbCode = float(cbU);
        crCode = float(crU);
    }

    // De-quantise to studio-range floats, then apply the YCbCr->R'G'B'
    // matrix. ColorNormalize is in the right code-space for the active
    // pixel kind, so this branch is bit-depth-agnostic.
    float yL  = (yCode  - ColorNormalize.x) * ColorNormalize.y;
    float cbL = (cbCode - ColorNormalize.z) * ColorNormalize.w;
    float crL = (crCode - ColorNormalize.z) * ColorNormalize.w;

    float r = yL + ColorMatrix.x * crL;
    float g = yL + ColorMatrix.y * cbL + ColorMatrix.z * crL;
    float b = yL + ColorMatrix.w * cbL;

    vec3 rgb;
    if (HDRMode == 0) {
        // SDR path: the matrix output is already display-ready R'G'B'.
        // Clamp into [0, 1] and we're done.
        rgb = clamp(vec3(r, g, b), 0.0, 1.0);
    } else {
        // HDR path: the matrix output is gamma-encoded BT.2020 R'G'B'.
        // Apply the inverse transfer function, tone-map, gamut-convert,
        // and re-encode for output. Allow the matrix output to exceed
        // [0, 1] since the encoded HDR signal frequently does.
        vec3 enc = vec3(r, g, b);
        vec3 lin = (HDRMode == 1) ? pq_inverse_eotf(enc)
                                  : hlg_inverse_oetf(enc);
        lin      = tone_map_reinhard(lin);
        lin      = bt2020_to_bt709(lin);
        lin      = max(lin, 0.0);  // clamp out-of-gamut negative values
        rgb      = srgb_encode(lin);
    }
    fragColor = vec4(rgb, 1.0);
}
)";

// =============================================================================
// Construction.
// =============================================================================

AJAInputPlugin::AJAInputPlugin()
{
    SetMinInputs(0);
    SetMaxInputs(0);

    // One scan of the AJA cards for both menus below.
    const auto detected = ajainput::AJADevice::EnumerateDevices();

    // Physical input source: the inputs of the AJA cards on this machine
    // (all of them: the Device parameter may pick any card), in the order
    // SDI 1..8, quad-link 4K inputs, HDMI 1..4.
    //
    // The element values are the plugin's input numbers minus one
    // (InputId.h) and are stored in compositions, so they never change.
    // A composition saved with an input this machine's cards do not have
    // falls back to the first entry (Resolume's behaviour for an unknown
    // option value). Without any AJA card the menu offers SDI 1, SDI 2
    // and HDMI (the clip then shows the synthetic test pattern).
    {
        uint32_t inputs = 0u;
        for (const auto& d : detected)
            if (d.canCapture)
                inputs |= d.inputMask;
        namespace in = ajainput::input;
        if (inputs == 0u)
            inputs = (1u << in::FromSdi(1)) | (1u << in::FromSdi(2)) |
                     (1u << in::FromHdmi(1));
        const bool severalHdmi =
            (inputs & ((1u << in::FromHdmi(2)) | (1u << in::FromHdmi(3)) |
                       (1u << in::FromHdmi(4)))) != 0u;
        int items[ajainput::input::kMaxInputNumber] = {};
        unsigned int count = 0;
        for (int k = 0; k < ajainput::input::kMaxInputNumber; ++k)
        {
            const int n = ajainput::input::kMenuOrder[k];
            if (inputs & (1u << n))
                items[count++] = n;
        }
        SetOptionParamInfo(PID_InputSource, "Input", count,
                           ajainput::input::ToOptionValue(items[0]));
        for (unsigned int i = 0; i < count; ++i)
            SetParamElementInfo(PID_InputSource, i,
                                ajainput::input::MenuName(items[i], severalHdmi),
                                ajainput::input::ToOptionValue(items[i]));
    }

    SetParamInfo(PID_ShowStats,   "Show Stats",   FF_TYPE_BOOLEAN,    false);

    // Ring Depth: AutoCirculate ring frame count (and the synthetic
    // producer's simulated equivalent). Lower = lower end-to-end
    // latency; higher = more buffer against momentary consumer
    // stalls. The SDK documents this trade-off explicitly: "Fewer
    // frames reduces latency, but increases the likelihood of frame
    // drops." Default 2 = the lowest value AutoCirculate accepts =
    // the latency-first choice. A user who observes occasional
    // drops under load (Resolume composite spikes, Windows scheduler
    // hiccups, GPU contention with other plugins) raises this to 3
    // or 4. Above 5 only really makes sense for very busy multi-
    // instance setups; the SDK demo uses 7 as a defensive default
    // because it targets unknown machines.
    SetOptionParamInfo(PID_RingDepth, "Ring Depth", 6, 0.0f);
    SetParamElementInfo(PID_RingDepth, 0, "2", 0.0f);
    SetParamElementInfo(PID_RingDepth, 1, "3", 1.0f);
    SetParamElementInfo(PID_RingDepth, 2, "4", 2.0f);
    SetParamElementInfo(PID_RingDepth, 3, "5", 3.0f);
    SetParamElementInfo(PID_RingDepth, 4, "6", 4.0f);
    SetParamElementInfo(PID_RingDepth, 5, "7", 5.0f);

    // Device dropdown: enumerate every AJA card visible to the host and
    // present each one by model and serial number. The previous build
    // exposed four hard-coded entries labelled "0", "1", "2", "3"
    // (zero-based PCIe slot index), which (a) did not help the user
    // verify that the plugin actually recognises their hardware, and
    // (b) showed three slots that nobody can fill on a typical desktop
    // (most installations have a single AJA card). With the
    // enumerated list, opening the dropdown becomes the simplest
    // possible "is my card detected?" diagnostic.
    //
    // The element value passed to SetParamElementInfo is the scanner-
    // index of the card; this is the same integer that gets fed to
    // AJADevice::Open() later, so a selection straight from the
    // dropdown goes to the correct card without any remapping.
    //
    // v0.42: output-only devices (T-TAP, T-TAP Pro) are left out; they
    // keep their scanner index, so the values of the others do not move.
    //
    // Edge case: when no AJA card is present, we fall back to a single
    // placeholder entry. A zero-element option dropdown would make
    // some FFGL hosts misbehave (the parameter renders as an empty
    // box, or the host falls back to a free-form slider), and we
    // would lose the visual "no hardware detected" feedback the
    // populated dropdown provides today.
    {
        std::vector<const ajainput::AJADevice::DeviceEntry*> cards;
        for (const auto& d : detected)
        {
            if (d.canCapture)
            {
                cards.push_back(&d);
                continue;
            }
            char line[160];
            std::snprintf(line, sizeof(line),
                          "AJAInput: %u: %s is an output-only device; not "
                          "offered as an input", d.index, d.modelName.c_str());
            FFGLLog::LogToHost(line);
        }
        if (cards.empty())
        {
            SetOptionParamInfo(PID_DeviceIndex, "Device", 1, 0.0f);
            SetParamElementInfo(PID_DeviceIndex, 0,
                                detected.empty() ? "(no AJA detected)"
                                                 : "(no AJA input card)",
                                0.0f);
        }
        else
        {
            SetOptionParamInfo(PID_DeviceIndex, "Device",
                               static_cast<unsigned int>(cards.size()),
                               static_cast<float>(cards[0]->index));
            for (std::size_t i = 0; i < cards.size(); ++i)
            {
                // Compose a label like "0: KONA X (SN X12345)" or
                // "0: KONA X" when no serial was programmed. We trim
                // the model name a touch so the label fits the narrow
                // dropdown column in Resolume's parameter inspector.
                char label[64];
                if (!cards[i]->serial.empty())
                {
                    std::snprintf(label, sizeof(label),
                                  "%u: %s (SN %s)",
                                  cards[i]->index,
                                  cards[i]->modelName.c_str(),
                                  cards[i]->serial.c_str());
                }
                else
                {
                    std::snprintf(label, sizeof(label),
                                  "%u: %s",
                                  cards[i]->index,
                                  cards[i]->modelName.c_str());
                }
                SetParamElementInfo(
                    PID_DeviceIndex,
                    static_cast<unsigned int>(i),
                    label,
                    static_cast<float>(cards[i]->index));
            }
        }
    }

    // Colorimetry: which YCbCr -> RGB matrix and quantisation range the
    // shader applies. "Auto" picks BT.601 limited for SD (525i / 625i)
    // and BT.709 limited for everything HD or above; this is the right
    // default for broadcast camera and switcher output and matches the
    // SMPTE convention. The explicit options exist for sources that
    // signal a non-default colour space, computer / graphics-card HDMI
    // (typically full-range RGB which gets CSC'd to full-range YCbCr
    // upstream of the FrameStore), and for diagnostic A/B against
    // other capture tools.
    SetOptionParamInfo(PID_Colorimetry, "Colorimetry", 7, 0.0f);
    SetParamElementInfo(PID_Colorimetry, 0, "Auto",            0.0f);
    SetParamElementInfo(PID_Colorimetry, 1, "BT.709 limited",  1.0f);
    SetParamElementInfo(PID_Colorimetry, 2, "BT.709 full",     2.0f);
    SetParamElementInfo(PID_Colorimetry, 3, "BT.601 limited",  3.0f);
    SetParamElementInfo(PID_Colorimetry, 4, "BT.601 full",     4.0f);
    // HDR transfer functions over BT.2020 primaries. The shader runs
    // the inverse EOTF (PQ ST.2084 or HLG ITU-R BT.2100) on the
    // decoded RGB, tone-maps the result down to the SDR range
    // Resolume's composite expects, and re-encodes for output. See
    // ColorimetryMode in AJAInputPlugin.h for the 8-bit limitation
    // caveat that will be addressed in v0.29.2.
    SetParamElementInfo(PID_Colorimetry, 5, "PQ (ST.2084)",    5.0f);
    SetParamElementInfo(PID_Colorimetry, 6, "HLG",             6.0f);

    // De-interlace toggle. Off (default) renders the captured frame as
    // a single woven raster - correct for progressive sources, leaves
    // visible "combing" on motion for interlaced ones. On enables Bob
    // de-interlace in the shader: only the top field's scanlines are
    // sampled, linearly interpolated back up to full raster height.
    // Halves vertical resolution but eliminates combing entirely.
    //
    // Exposed as a boolean because there is only one user-visible
    // mode today (off = Weave / on = YADIF for UYVY, simple bob for
    // V210). The shader silently force-clamps InterlaceMode to Weave
    // on progressive sources (the render path detects
    // !m_currentFormat.isInterlaced and forces the uniform to 0
    // regardless of this toggle), so leaving it on permanently is
    // safe. The DeinterlaceMode enum in AJAInputPlugin.h documents the
    // canonical option indices if this is ever promoted back to an
    // option dropdown.
    SetParamInfo(PID_Deinterlace, "Deinterlace", FF_TYPE_BOOLEAN, false);

    // Low Latency Mode: opt-in toggle that switches the AJA capture
    // path from AutoCirculate to the SDK's documented low-latency
    // pattern (ntv2llburn). When on, the driver-side ring is
    // bypassed and frame transfers happen via direct DMAReadFrame
    // calls against two device frame slots manually ping-ponged with
    // SetInputFrame. End-to-end capture latency drops from
    // ~1-2 frames variable to ~1 frame deterministic. Trade-off:
    // less tolerance to consumer stalls - if the render thread
    // cannot keep pace with the SDI clock, frames are silently
    // overwritten at the next vblank rather than queued. Default
    // off so the robust AutoCirculate path remains the out-of-the-
    // box experience; users chasing the lowest possible latency on
    // a workstation with deterministic scheduling turn this on.
    //
    // The toggle takes effect on the next capture session re-init:
    // changing it forces the capture loop back through Initializing
    // identically to a channel switch, with a brief NoSignal gap of
    // a couple of frames while the device re-arms. Live toggling
    // during playback is safe.
    SetParamInfo(PID_LowLatencyMode,
                 "Low Latency Mode",
                 FF_TYPE_BOOLEAN,
                 false);

    // Format: read-only text field that mirrors what the on-screen
    // overlay shows ("1080p59.94", "720p60", "no signal", etc.).
    // FFGL has no native concept of read-only text, so we expose it
    // as FF_TYPE_TEXT and ignore SetTextParameter for this index;
    // the value the operator sees is always the live state read
    // from the capture path's atomic counters at the moment of the
    // GetTextParameter call. Resolume will render this as a small
    // text field in the inspector, which the operator can ignore
    // when editing (Resolume re-polls GetTextParameter as soon as
    // focus leaves the field, restoring our value). The default
    // value below is what shows before the capture has produced
    // any state, e.g. while InitGL is still running.
    SetParamInfo(PID_FormatInfo,
                 "Format",
                 FF_TYPE_TEXT,
                 "—");

    // Live capture stats as four read-only text parameters. Same
    // pattern as PID_FormatInfo: FF_TYPE_TEXT, SetTextParameter is a
    // no-op, GetTextParameter composes the value on demand from the
    // atomic counters owned by the capture or producer. Resolume
    // polls these on its GUI thread independently of ProcessOpenGL,
    // so they stay in sync with what the on-screen overlay shows
    // without needing the overlay enabled.
    //
    // Stat → source on AJA path → source on synthetic path:
    //   Frames        AJACaptureSource::FramesCaptured     queue.ProducedCount
    //   Drops         AJACaptureSource::DriverDrops        queue.DroppedCount
    //   Recoveries    AJACaptureSource::RecoveryCount      n/a (shows 0)
    //   Idle wakeups  AJACaptureSource::IdleWakeups        n/a (shows 0)
    //
    // Recoveries and Idle wakeups have no synthetic-path equivalent
    // because the synthetic producer does not have a state machine
    // that loses signal or wakes up empty. Showing 0 there is
    // deliberate, not a bug: a synthetic session genuinely has zero
    // of those events.
    SetParamInfo(PID_StatsFrames,      "Stats / Frames",       FF_TYPE_TEXT, "0");
    SetParamInfo(PID_StatsDrops,       "Stats / Drops",        FF_TYPE_TEXT, "0");
    SetParamInfo(PID_StatsRecoveries,  "Stats / Recoveries",   FF_TYPE_TEXT, "0");
    SetParamInfo(PID_StatsIdleWakeups, "Stats / Idle wakeups", FF_TYPE_TEXT, "0");

    // 10-bit packed v210 capture toggle. Default off (8-bit UYVY,
    // backward-compatible with v0.29.1 and prior). When the user
    // turns it on AND the active card reports
    // canDoFBF10Bit=yes in the v0.25 capabilities dump, the AJA
    // capture path switches to NTV2_FBF_10BIT_YCBCR and the host
    // DMA buffer / GL texture / shader unpack all follow. If the
    // card cannot do 10-bit, the toggle is silently ignored and a
    // line is logged so the operator knows.
    //
    // Tied to a reopen of the AJA device: switching the toggle
    // mid-stream tears down and re-creates the capture (same
    // contract as a device-index change). A few hundred ms of
    // black is expected during the swap.
    //
    // EXPERIMENTAL until validated on actual hardware. The v210
    // bit-packing layout in NTV2_FBF_10BIT_YCBCR is assumed to be
    // standard SMPTE (3 × 10-bit samples per 32-bit word, LSB-first);
    // if the operator's card uses a different byte order, the
    // image will appear colour-tinted or horizontally striped. The
    // workaround is to leave this toggle off.
    SetParamInfo(PID_TenBitMode,
                 "10-bit capture (v210)",
                 FF_TYPE_BOOLEAN,
                 0.0f);

    // v0.42: what the clip shows while its input has no picture (no
    // signal, starting up, waiting for a FrameStore). Black is what the
    // clip showed before (a near-black grey); Transparent lets the layers
    // below through; Hold Last Frame keeps the clip's last picture of
    // this input on screen, as a vision mixer would.
    SetOptionParamInfo(PID_NoSignal, "No Signal", 3, 0.0f);
    SetParamElementInfo(PID_NoSignal, 0, "Black",           0.0f);
    SetParamElementInfo(PID_NoSignal, 1, "Transparent",     1.0f);
    SetParamElementInfo(PID_NoSignal, 2, "Hold Last Frame", 2.0f);

    // v0.42: how the input's picture fills the clip. An FFGL source cannot
    // set its own size (Resolume renders it at the clip's size, the
    // composition's), so up to v0.41 a picture of another aspect was
    // stretched, as Stretch still does. Fit (default) keeps the aspect with
    // transparent bars, Fill covers the clip and cuts the overflow,
    // Original shows the picture at its own size. Same names and meanings
    // as Resolume's clip Resize; see PictureFit.h.
    SetOptionParamInfo(PID_Resize, "Resize", 4, 0.0f);
    SetParamElementInfo(PID_Resize, 0, "Fit",      0.0f);
    SetParamElementInfo(PID_Resize, 1, "Fill",     1.0f);
    SetParamElementInfo(PID_Resize, 2, "Stretch",  2.0f);
    SetParamElementInfo(PID_Resize, 3, "Original", 3.0f);

    FFGLLog::LogToHost("AJAInput: plugin instance created");
}

// =============================================================================
// OpenGL lifecycle.
// =============================================================================

FFResult AJAInputPlugin::InitGL(const FFGLViewportStruct* viewport)
{
    if (!m_shader.Compile(vertexShaderCode, fragmentShaderCode))
    {
        FFGLLog::LogToHost("AJAInput: shader compilation failed");
        DeInitGL();
        return FF_FAIL;
    }

    if (!m_quad.Initialise())
    {
        FFGLLog::LogToHost("AJAInput: screen quad initialisation failed");
        DeInitGL();
        return FF_FAIL;
    }

    // Optional: the field-row copy pass that repairs YADIF history after
    // a missed second-field publish. Failure is logged and only disables
    // the repair.
    if (m_fieldCopyShader.Compile(vertexShaderCode, fieldCopyFragmentShaderCode))
    {
        m_uFieldCopySource = m_fieldCopyShader.FindUniform("Source");
        m_uFieldCopyParity = m_fieldCopyShader.FindUniform("RowParity");
    }
    else
    {
        FFGLLog::LogToHost("AJAInput: field copy shader compilation "
                           "failed; YADIF field repair disabled");
    }

    if (!m_overlay.Initialise())
    {
        FFGLLog::LogToHost("AJAInput: overlay initialisation failed");
        DeInitGL();
        return FF_FAIL;
    }

    // v0.41: the clip attaches to the shared capture of its (Device,
    // Input) on its first ProcessOpenGL, not here: FF_INSTANTIATE_GL sets
    // the parameter defaults and calls InitGL in one go, so a saved
    // composition's Input / Device / capture settings only arrive after
    // InitGL, and a clip that is never shown should not hold a capture.

    {
        ScopedShaderBinding bind(m_shader.GetGLID());
        m_uTexture        = m_shader.FindUniform("CurTexture");
        m_uPrevTexture    = m_shader.FindUniform("PrevTexture");
        m_uNextTexture    = m_shader.FindUniform("NextTexture");
        m_uImageSize      = m_shader.FindUniform("ImageSize");
        m_uColorNormalize  = m_shader.FindUniform("ColorNormalize");
        m_uColorMatrix     = m_shader.FindUniform("ColorMatrix");
        m_uHDRMode         = m_shader.FindUniform("HDRMode");
        m_uPixelFormatMode = m_shader.FindUniform("PixelFormatMode");
        m_uInterlaceMode   = m_shader.FindUniform("InterlaceMode");
        m_uFieldParity     = m_shader.FindUniform("FieldParity");
        m_uTopFieldFirst   = m_shader.FindUniform("TopFieldFirst");

        // Static texture-unit assignments. CurTexture is on unit 0,
        // PrevTexture on unit 1, NextTexture on unit 2. The render loop
        // binds the actual textures via glActiveTexture / glBindTexture
        // before each draw, but the uniform→unit mapping never changes.
        if (m_uTexture     >= 0) glUniform1i(m_uTexture,     0);
        if (m_uPrevTexture >= 0) glUniform1i(m_uPrevTexture, 1);
        if (m_uNextTexture >= 0) glUniform1i(m_uNextTexture, 2);
    }

    m_fpsWindowStart = std::chrono::steady_clock::now();
    m_fpsFrameCount  = 0;
    m_fpsCurrent     = 0.0f;

    // v0.42 Resize: a Fill / Original picture rect may be larger than the
    // clip; it must stay within what glViewport takes.
    {
        GLint dims[2] = { 0, 0 };
        glGetIntegerv(GL_MAX_VIEWPORT_DIMS, dims);
        m_maxViewportDim = std::max(0, std::min(dims[0], dims[1]));
    }

    FFGLLog::LogToHost("AJAInput: InitGL completed");
    m_initGLDone = true;
    return CFFGLPlugin::InitGL(viewport);
}

// =============================================================================
// v0.42 Resize: drawing into the picture's rect of the clip.
// =============================================================================

namespace
{
/// The host's viewport for this clip: what a full-clip draw fills.
ajainput::PixelRect CurrentGLViewport() noexcept
{
    GLint v[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, v);
    ajainput::PixelRect r;
    r.x      = v[0];
    r.y      = v[1];
    r.width  = v[2];
    r.height = v[3];
    return r;
}

/// Clears and draws limited to rects of the clip. Saves the viewport, the
/// scissor test and box, and the clear colour, and restores them on scope
/// exit. Nothing is written outside the clip: the host's viewport, cut to
/// the host's scissor box when the host had the scissor test on (as a
/// full-clip draw would be).
class ClipAreaScope
{
public:
    explicit ClipAreaScope(const ajainput::PixelRect& hostViewport) noexcept
        : m_viewport(hostViewport), m_clip(hostViewport)
    {
        m_scissor = glIsEnabled(GL_SCISSOR_TEST);
        glGetIntegerv(GL_SCISSOR_BOX, m_scissorBox);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, m_clearColor);
        if (m_scissor)
        {
            ajainput::PixelRect box;
            box.x      = m_scissorBox[0];
            box.y      = m_scissorBox[1];
            box.width  = m_scissorBox[2];
            box.height = m_scissorBox[3];
            m_clip = ajainput::Intersect(m_clip, box);
        }
        glEnable(GL_SCISSOR_TEST);
    }
    ~ClipAreaScope()
    {
        glViewport(m_viewport.x, m_viewport.y, m_viewport.width, m_viewport.height);
        glScissor(m_scissorBox[0], m_scissorBox[1], m_scissorBox[2], m_scissorBox[3]);
        if (!m_scissor)
            glDisable(GL_SCISSOR_TEST);
        glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
    }
    ClipAreaScope(const ClipAreaScope&)            = delete;
    ClipAreaScope& operator=(const ClipAreaScope&) = delete;

    /// Clear r, within the clip, to a colour.
    void Clear(const ajainput::PixelRect& r,
               float red, float green, float blue, float alpha) const noexcept
    {
        const ajainput::PixelRect a = ajainput::Intersect(m_clip, r);
        if (a.width <= 0 || a.height <= 0)
            return;
        glScissor(a.x, a.y, a.width, a.height);
        glClearColor(red, green, blue, alpha);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    /// Point the following draws at r, cut to the clip.
    void Target(const ajainput::PixelRect& r) const noexcept
    {
        glScissor(m_clip.x, m_clip.y, m_clip.width, m_clip.height);
        glViewport(r.x, r.y, r.width, r.height);
    }

private:
    ajainput::PixelRect m_viewport;
    ajainput::PixelRect m_clip;
    GLboolean        m_scissor       = GL_FALSE;
    GLint            m_scissorBox[4] = { 0, 0, 0, 0 };
    GLfloat          m_clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};
} // namespace

FFResult AJAInputPlugin::ProcessOpenGL(ProcessOpenGLStruct* /*processStruct*/)
{
    // v0.41: first frame, or Input / Device changed: attach to that
    // input's shared capture. The old one closes if this was its last
    // clip, freeing its FrameStore first; its open card handle is reused
    // by the new capture instead of reopening the card.
    if (!m_source ||
        m_source->DeviceIndex()   != DeviceIndexParam() ||
        m_source->InputNumber() != InputNumber())
    {
        DetachSource();
        AttachSource();
        ajainput::SharedCapture::CloseSpareDevices();
    }
    // v0.42: a held picture belongs to the input it came from.
    if (m_holdValid &&
        (m_holdInput != InputNumber() || m_holdDevice != DeviceIndexParam()))
        m_holdValid = false;

    // Shared-capture housekeeping (marks the input as on screen, applies
    // setting changes, re-sizes for a new format, FrameStore lending and
    // pairing), then adopt its format and settings for this clip.
    if (m_source)
    {
        m_source->Update();
        SyncFromSource();
    }
    if (m_settingsEventsPending)
    {
        // Another clip of this input changed a capture setting (or this
        // clip joined an input whose settings differ): make the host
        // re-read the values so the inspector shows the shared ones.
        m_settingsEventsPending = false;
        RaiseParamEvent(PID_RingDepth,      FF_EVENT_FLAG_VALUE);
        RaiseParamEvent(PID_LowLatencyMode, FF_EVENT_FLAG_VALUE);
        RaiseParamEvent(PID_TenBitMode,     FF_EVENT_FLAG_VALUE);
    }

    // -------------------------------------------------------------------
    // Source-state branch. If the capture exists but is not running (no
    // signal, configuring, waiting for a FrameStore), do not sample stale
    // texture content: show what the No Signal parameter asks for (v0.42:
    // black, transparent, or the last picture; up to v0.41 a neutral dark
    // gray) and let the overlay surface the cause, the equivalent of a
    // broadcast capture app's "NO SIGNAL" screen.
    // -------------------------------------------------------------------
    ajainput::AJACaptureSource* const cap = m_source ? m_source->Capture() : nullptr;
    const bool ajaActiveButOffline = !m_source || m_source->Offline();

    // -------------------------------------------------------------------
    // 1. Acquire the most recent frame from the shared capture (texture
    //    ring, or the byte queue uploaded once per new frame).
    // -------------------------------------------------------------------
    GLuint                                 textureToBind = 0;
    std::chrono::steady_clock::time_point  producedAt    = {};
    std::chrono::steady_clock::time_point  publishedAt   = {};
    bool                                   haveFrame     = false;
    // Only set true when the off-thread Acquire path reported the
    // upload fence as still pending. Counted per-frame for the diag log.
    bool                                   fencePendingThisFrame = false;
    // True when this frame acquired from the texture ring; EndRead must
    // then be called after the last command that reads the slot.
    bool                                   ringAcquired = false;

    if (!ajaActiveButOffline)
    {
        const ajainput::SharedCapture::Frame fr = m_source->Acquire();
        ringAcquired = fr.needsEndRead;
        if (fr.valid)
        {
            textureToBind = fr.texture;
            producedAt    = fr.producedAt;
            publishedAt   = fr.publishedAt;
            fencePendingThisFrame = fr.fencePending;
            haveFrame     = true;

            // Feed the YADIF history, on FRESH publishes only (a repeat
            // acquire must not re-blit or rotate). v0.41: freshness is
            // judged per clip from the publish sequence, because another
            // clip of the same input may have acquired this publish first.
            //
            // The rotation gate is the source-frame sequence carried by
            // the ring metadata, not isFirstFieldOfFrame:
            //   - FullFrame publish (frame-mode capture): rotate and blit
            //     the whole ring texture into the new "next".
            //   - Field publish with a NEW sequence: rotate and blit.
            //     Normally the first field (F0); if F0 was missed the F1
            //     publish rotates instead, and it already carries both
            //     fields of the frame.
            //   - Field publish with the SAME sequence (F1 of the frame
            //     we rotated for): re-blit into the same "next".
            //   - v0.39: if that F1 was missed, the next F0 publish
            //     carries it in its other-parity rows; copy those into
            //     the outgoing slot before rotating.
            //   - v0.39: a sequence going backwards (capture restarted)
            //     resets the history.
            // The decisions live in ajainput::FieldHistoryGate
            // (FieldHistoryGate.h, unit-tested).
            const bool fresh = fr.fromRing && fr.publishSequence != m_lastRingSequence;
            if (fr.fromRing)
                m_lastRingSequence = fr.publishSequence;
            if (fresh)
            {
                using FH = ajainput::SharedTextureRing::FieldHint;
                // Remember the hint for the FieldParity override below. A
                // non-fresh acquire (re-render of the same publish) leaves
                // it unchanged so the shader keeps painting the same field.
                m_lastFreshFieldHint = fr.fieldHint;

                using PK = ajainput::FieldHistoryGate::PublishKind;
                const PK kind =
                    (fr.fieldHint == FH::FullFrame) ? PK::FullFrame :
                    fr.isFirstFieldOfFrame          ? PK::FirstField :
                                                      PK::SecondField;
                const int freshRowParity =
                    (fr.fieldHint == FH::FreshBottom) ? 1 : 0;
                const auto decision = m_fieldGate.OnFreshPublish(
                    kind, freshRowParity, fr.sourceFrameSequence,
                    m_yadifSlots.framesBuffered,
                    /*repairEnabled*/m_fieldCopyShader.GetGLID() != 0);

                if (decision.resetHistory)
                {
                    // Capture session restarted: drop the old session's
                    // frames instead of mixing them in.
                    m_yadifSlots.Reset();
                }
                if (decision.repairOutgoing)
                {
                    // Complete the outgoing next slot with the second
                    // field it missed, before it rotates into "cur".
                    CopyRingRowsIntoHistorySlot(fr.texture,
                                                m_yadifSlots.nextSlot,
                                                decision.repairRowParity);
                }
                if (decision.rotate)
                    BlitRingFrameIntoHistory(fr.texture, fr.publishSequence);
                else
                    BlitRingFieldIntoCurrentNextSlot(fr.texture);
            }
        }
    }

    // -------------------------------------------------------------------
    // 2. Render. Either the YCbCr shader on the captured texture, or a
    //    plain dark gray fill for the no-signal case.
    // -------------------------------------------------------------------
    if (haveFrame)
    {
        // Sample end-to-end latency: time from DMA-copy completion (when
        // the capture thread filled m_transferBuffer and called Upload)
        // to right now (just before binding the texture for this draw).
        // Excludes Resolume's own render queue and the GPU display
        // chain, which are downstream fixed offsets we can't observe
        // from here. Aggregated and emitted by the diag log below.
        //
        // NOTE: when the regular yadif path (shader mode 1) renders the
        // second-most-recent captured frame (so that "next" is
        // available), the displayed field is one source frame older
        // than producedAt reports, so the stat under-reports by that
        // ~1/sourceRate (≈33 ms at 1080i59.94). The low-latency mode
        // (shader mode 2, v0.40) shows the newest field and has no such
        // offset. Documenting here
        // rather than complicating the latency calculation with
        // history-slot-aware producedAt tracking; the absolute number
        // is useful mainly to spot regressions over time.
        const auto latencyDuration =
            std::chrono::steady_clock::now() - producedAt;
        const double latencyMs =
            std::chrono::duration<double, std::milli>(
                latencyDuration).count();
        if (latencyMs >= 0.0 && latencyMs < 1000.0)
        {
            m_latencySumMs += latencyMs;
            m_latencyCount += 1;
            if (latencyMs > m_latencyMaxMs)
                m_latencyMaxMs = latencyMs;
        }
        if (fencePendingThisFrame)
            ++m_fencePendingCount;

        ScopedShaderBinding shaderBind(m_shader.GetGLID());

        // Decide whether the yadif sample path is active for this
        // draw. Conditions: (a) deinterlace toggle is on, (b) the
        // source is interlaced, (c) the pixel kind is UYVY (V210 is
        // not supported yet, see shader comment), (d) the history is
        // fully buffered (we have at least 3 captured frames). Until
        // all four are true we fall back to plain weave by binding
        // the ring's texture directly and letting InterlaceMode == 0
        // win in the shader, regardless of the user toggle.
        const ajainput::PixelKind kindNow = SourceKind();
        const bool yadifPath =
            (m_deinterlace > 0.5f) &&
            m_currentFormat.isInterlaced &&
            (kindNow == ajainput::PixelKind::UYVY) &&
            (m_source && m_source->OffThreadUpload()) &&
            (m_yadifSlots.framesBuffered >= 3) &&
            (m_yadifHistoryTex[0] != 0);

        // Bind the three sampler units. With yadif on, the "current"
        // frame is one capture period behind the most recently
        // arrived one (so that PrevTexture and NextTexture both have
        // valid data straddling it in time). Without yadif, all three
        // units get the same texture so the shader's other sample
        // sites still produce something benign if they execute
        // (they should not — InterlaceMode == 0 in that case — but
        // defensive binding avoids UB on driver implementations that
        // sample unconditionally).
        //
        // v0.40: with Low Latency on, field-mode interlaced sources use
        // the past-only YADIF variant (shader InterlaceMode 2), which
        // shows the NEWEST history frame and needs the two older ones.
        // It relies on the per-field publish layout (the newest frame's
        // other rows hold the field just before the output field), so
        // frame-mode publishes keep the regular YADIF.
        const bool lowLatencyDeint =
            yadifPath &&
            CurrentLowLatencyMode() &&
            (m_lastFreshFieldHint !=
             ajainput::SharedTextureRing::FieldHint::FullFrame);

        GLuint curTex, prevTex, nextTex;
        if (yadifPath && lowLatencyDeint)
        {
            curTex  = m_yadifHistoryTex[m_yadifSlots.nextSlot];  // newest
            prevTex = m_yadifHistoryTex[m_yadifSlots.curSlot];   // 1 frame older
            nextTex = m_yadifHistoryTex[m_yadifSlots.prevSlot];  // 2 frames older
        }
        else if (yadifPath)
        {
            curTex  = m_yadifHistoryTex[m_yadifSlots.curSlot];
            prevTex = m_yadifHistoryTex[m_yadifSlots.prevSlot];
            nextTex = m_yadifHistoryTex[m_yadifSlots.nextSlot];
        }
        else
        {
            curTex = prevTex = nextTex = textureToBind;
        }

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, prevTex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, nextTex);
        // Bind unit 0 last so it's the current active unit when the
        // ScopedXXX wrappers below kick in, matching the prior code
        // path's expectations.
        glActiveTexture(GL_TEXTURE0);
        Scoped2DTextureBinding textureBind(curTex);

        glUniform2f(m_uImageSize,
                    static_cast<float>(m_currentFormat.width),
                    static_cast<float>(m_currentFormat.height));

        // Colorimetry: resolve "Auto" against the current format, then
        // pack the normalisation tuple and the matrix into their
        // respective uniforms. Done per-frame because the format can
        // change live (a different camera resolution coming in via the
        // raster-change reallocation path), and because the user can
        // flip the dropdown at any time.
        const int colorIdx = static_cast<int>(m_colorimetry + 0.5f);
        ColorimetryMode mode = static_cast<ColorimetryMode>(colorIdx);
        if (mode == ColorimetryMode::Auto)
            mode = ResolveAuto(m_currentFormat,
                               m_source ? m_source->SignalledTransfer() : 0);
        // Resolve the live pixel kind here so the shader receives a
        // ColorNormalize tuple in the correct code-space (0..255 for
        // UYVY; 0..1023 for V210). The active pixel kind is the one the
        // shared capture's texture pipeline is sized for.
        const ajainput::PixelKind kind = kindNow;
        const ColorNormalize norm = NormalizeForMode(mode, kind);
        const ColorMatrix    mat  = MatrixForMode(mode);
        glUniform4f(m_uColorNormalize,
                    norm.yOffset, norm.yScale,
                    norm.cOffset, norm.cScale);
        glUniform4f(m_uColorMatrix,
                    mat.krCr, mat.kgCb, mat.kgCr, mat.kbCb);
        // HDR mode flag derived from the same resolved colorimetry.
        // The shader branches on this value; SDR modes (Auto, BT.709,
        // BT.601) resolve to 0 here, PQ resolves to 1, HLG to 2.
        glUniform1i(m_uHDRMode, HDRModeFor(mode));
        // Pixel-format flag for the shader's macropixel reconstruction
        // branch. 0 = UYVY (existing path); 1 = V210 (v0.29.2 unpack).
        glUniform1i(m_uPixelFormatMode,
                    (kind == ajainput::PixelKind::V210) ? 1 : 0);

        // De-interlace toggle. Mode 1 in the shader is YADIF for UYVY
        // and simple bob for V210. The yadif path needs three fully
        // buffered captured frames in m_yadifHistory; until the
        // history fills (the first ~3 captured frames after InitGL,
        // or after a raster change) we keep Mode 0 (Weave) on UYVY
        // to avoid sampling yet-uninitialised history textures. The
        // V210 path never reads the history so it can engage Mode 1
        // (simple bob) immediately. Mode 0 is forced on progressive
        // sources, so the user can leave the toggle on as a "make
        // interlaced cameras look right" preset without disturbing
        // the look of progressive material.
        const bool deintOn = (m_deinterlace > 0.5f);
        const bool wantsDeint = deintOn && m_currentFormat.isInterlaced;
        const bool deintReady =
            (kindNow == ajainput::PixelKind::V210) || yadifPath;
        const int interlaceUniform =
            !(wantsDeint && deintReady) ? 0 :
            lowLatencyDeint             ? 2 : 1;
        glUniform1i(m_uInterlaceMode, interlaceUniform);

        // Top-field-first temporal order. HD interlaced (1080i25/29.97/
        // 30 and 1080i50/59.94/60 derived variants) is TFF; NTSC-class
        // 480i is BFF. We detect BFF by the same raster-height test
        // used below for the parity swap. The shader's temporal
        // predictor uses TopFieldFirst to choose which of prev/cur/
        // next supplies each temporal sample.
        const bool isLowerFieldFirst =
            m_currentFormat.isInterlaced &&
            (m_currentFormat.height <= 486);
        if (m_uTopFieldFirst >= 0)
        {
            glUniform1i(m_uTopFieldFirst, isLowerFieldFirst ? 0 : 1);
        }

        // Field parity. Two paths.
        //
        // FIELD-MODE publishes (v0.32+): the ring carries explicit
        //   metadata about which field rows were freshly written. We
        //   map that directly to FieldParity: FreshTop → 0 (preserve
        //   top rows, interpolate bottom), FreshBottom → 1 (preserve
        //   bottom rows, interpolate top). No TFF/BFF swap is needed
        //   here — the hint is about which ROWS are fresh, not about
        //   time order. m_lastFreshFieldHint is updated on every fresh
        //   ring acquire and persists across non-fresh ProcessOpenGL
        //   calls (re-renders) so the shader keeps painting the same
        //   field consistently between publishes.
        //
        // FRAME-MODE / legacy (m_lastFreshFieldHint == FullFrame): the
        //   counter-based scheme from v0.21. Bound to the live
        //   captured-frame counter so each new arrival of a woven
        //   interlaced frame restarts the tick at 0 (top field,
        //   temporally earlier under UFF). Without that anchoring,
        //   the tick would simply free-run against the render thread
        //   and slowly drift against the capture rate, since
        //   Resolume's render cadence is rarely an exact 2× of an
        //   interlaced source's 30 Hz / 25 Hz / 29.97 Hz frame rate.
        //   The drift symptom is "second hand jumps backwards once
        //   every few seconds" — what looks like a temporal stutter
        //   is really top→bottom→top→top→bottom→... where the
        //   duplicated tick shows the same field of a new frame after
        //   the previous frame's complement was never displayed.
        //
        // The swap-bit XOR is taken from `isLowerFieldFirst`, computed
        // a few lines above for the TopFieldFirst uniform.
        int fieldParityUniform;
        using FH = ajainput::SharedTextureRing::FieldHint;
        if (m_lastFreshFieldHint == FH::FreshTop)
        {
            fieldParityUniform = 0;
        }
        else if (m_lastFreshFieldHint == FH::FreshBottom)
        {
            fieldParityUniform = 1;
        }
        else
        {
            const uint64_t currentCaptureFrame =
                m_source ? m_source->CapturedFrameCount() : 0ull;
            if (currentCaptureFrame != m_lastCaptureFrameForField)
            {
                m_fieldTickIndex            = 0;
                m_lastCaptureFrameForField  = currentCaptureFrame;
            }
            else
            {
                ++m_fieldTickIndex;
            }

            const unsigned int swapBit = isLowerFieldFirst ? 1u : 0u;
            fieldParityUniform = static_cast<int>(
                (m_fieldTickIndex & 1u) ^ swapBit);
        }
        glUniform1i(m_uFieldParity, fieldParityUniform);

        // v0.42 Resize: the picture keeps the input's aspect in a clip of
        // another shape (PictureFit.h). When its rect is the clip (Stretch,
        // or the same aspect) this is the full-clip draw of v0.41.
        const ajainput::PixelRect host    = CurrentGLViewport();
        const ajainput::PixelRect picture = PictureRectIn(host);
        if (picture == host)
        {
            m_quad.Draw();
        }
        else
        {
            ClipAreaScope area(host);
            if (!ajainput::Covers(picture, host))
                area.Clear(host, 0.0f, 0.0f, 0.0f, 0.0f);   // transparent bars
            area.Target(picture);
            m_quad.Draw();
        }

        // Leave units 1 and 2 unbound as we found them (unit 0 is
        // restored by Scoped2DTextureBinding). FFGL hosts expect the
        // default texture state back.
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);

        // v0.42: keep a copy for Hold Last Frame (the picture only; the
        // stats overlay is drawn after this).
        if (CurrentNoSignalMode() == NoSignalMode::HoldLast)
            SaveHeldFrame();
        else if (m_holdTex != 0)
            ReleaseHeldFrame();   // not holding: give the copy back
    }
    else
    {
        // Either AJA is between states or no frame has arrived yet.
        // v0.42: the No Signal parameter decides what the clip shows.
        // Black covers the picture's area (Resize: the bars stay
        // transparent, as while the input is live), Transparent the clip.
        const NoSignalMode mode = CurrentNoSignalMode();
        if (!(mode == NoSignalMode::HoldLast && DrawHeldFrame()))
        {
            const ajainput::PixelRect host    = CurrentGLViewport();
            const ajainput::PixelRect picture =
                (mode == NoSignalMode::Transparent) ? host : PictureRectIn(host);
            if (picture == host)
            {
                glClearColor(0.0f, 0.0f, 0.0f,
                             mode == NoSignalMode::Transparent ? 0.0f : 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            else
            {
                ClipAreaScope area(host);
                if (!ajainput::Covers(picture, host))
                    area.Clear(host, 0.0f, 0.0f, 0.0f, 0.0f);
                area.Clear(picture, 0.0f, 0.0f, 0.0f, 1.0f);
            }
        }
    }

    // -------------------------------------------------------------------
    // 3. Update consumer FPS counter.
    // -------------------------------------------------------------------
    ++m_fpsFrameCount;
    const auto now    = std::chrono::steady_clock::now();
    const auto window = std::chrono::duration_cast<std::chrono::microseconds>(
                            now - m_fpsWindowStart).count();
    if (window >= 500000) // recompute roughly every half second
    {
        m_fpsCurrent = static_cast<float>(m_fpsFrameCount) *
                       (1.0e6f / static_cast<float>(window));
        m_fpsWindowStart = now;
        m_fpsFrameCount  = 0;

        // Push a value-changed event for each live-readout parameter
        // so the host re-polls GetTextParameter and the inspector
        // reflects the current counter values. FFGL hosts (Resolume
        // in particular) cache parameter values and only re-poll on
        // explicit events, focus changes, or rare periodic timers.
        // Without this, the Frames / Drops / Format fields show their
        // construction-time defaults forever even though our
        // GetTextParameter implementation already returns the live
        // counters. 500 ms cadence keeps the UI feeling responsive
        // without flooding the host's event loop (5 events per tick
        // × 2 ticks per second = 10 events/sec total, which is
        // trivial).
        RaiseParamEvent(PID_FormatInfo,       FF_EVENT_FLAG_VALUE);
        RaiseParamEvent(PID_StatsFrames,      FF_EVENT_FLAG_VALUE);
        RaiseParamEvent(PID_StatsDrops,       FF_EVENT_FLAG_VALUE);
        RaiseParamEvent(PID_StatsRecoveries,  FF_EVENT_FLAG_VALUE);
        RaiseParamEvent(PID_StatsIdleWakeups, FF_EVENT_FLAG_VALUE);

        // Periodic pipeline diagnostic. Surfaces in the Resolume log
        // every ~2 seconds (one in four FPS-recompute windows) and
        // gives a fast read on whether the producer is publishing,
        // whether the consumer is acquiring, and whether the upload
        // path is what we expect. Pin to the bare minimum so the
        // log stays readable; users can correlate with the FPS
        // counter on the overlay.
        ++m_diagLogTick;
        if (m_diagLogTick >= 4)
        {
            m_diagLogTick = 0;

            // Compute aggregate latency for the window and reset for
            // the next one. Average is the steady-state perceived
            // latency; max catches any single-frame spikes that an
            // average would mask. Both excluded from this log emission
            // when no frames were sampled (e.g. NoSignal the whole
            // window) — we print "n/a" instead of dividing by zero.
            char latencyBuf[80];
            if (m_latencyCount > 0)
            {
                const double avgMs =
                    m_latencySumMs / static_cast<double>(m_latencyCount);
                // Percentage of frames whose upload fence was still
                // unsignaled at Acquire time. High values mean the
                // PCIe transfer is our actual bottleneck and would
                // justify a pipelining attack. Low values (typical
                // when persistent zero-copy is on) mean the GPU is
                // not the limiter.
                const double pendPct = 100.0 *
                    static_cast<double>(m_fencePendingCount) /
                    static_cast<double>(m_latencyCount);
                std::snprintf(latencyBuf, sizeof(latencyBuf),
                              "avg=%.1fms max=%.1fms n=%zu fencePend=%.0f%%",
                              avgMs, m_latencyMaxMs,
                              m_latencyCount, pendPct);
            }
            else
            {
                std::snprintf(latencyBuf, sizeof(latencyBuf),
                              "avg=n/a max=n/a n=0 fencePend=n/a");
            }
            m_latencySumMs = 0.0;
            m_latencyMaxMs = 0.0;
            m_latencyCount = 0;
            m_fencePendingCount = 0;

            // Capture-thread mode and ring depth, queried directly from
            // the live AJACaptureSource. Lets us correlate latency
            // numbers with the exact path that produced them, and lets
            // us verify in one line that toggling Low Latency Mode in
            // the inspector actually flipped the capture thread to the
            // LL path (and similarly for Ring Depth changes).
            char modeBuf[112];
            if (cap)
            {
                std::snprintf(modeBuf, sizeof(modeBuf),
                              "mode=%s ringDepth=%d drains=%llu "
                              "activeSkips=%llu",
                              cap->IsLowLatencyModeRequested() ? "LL" : "AC",
                              cap->RingFrameCountRequested(),
                              static_cast<unsigned long long>(
                                  cap->BacklogDrains()),
                              static_cast<unsigned long long>(
                                  cap->ActiveSlotSkips()));
            }
            else
            {
                std::snprintf(modeBuf, sizeof(modeBuf),
                              "mode=synthetic");
            }

            const bool offThread = m_source && m_source->OffThreadUpload();
            char dline[460];
            std::snprintf(dline, sizeof(dline),
                          "AJAInput: diag %s fps=%.1f haveFrame=%d "
                          "path=%s%s %s latency=%s "
                          "ringUploaded=%llu ringMissed=%llu "
                          "fieldRepairs=%llu "
                          "queueProduced=%llu queueDropped=%llu "
                          "ajaCapture=%s clips=%d",
                          m_source ? InputLabelUpper(m_source->InputNumber()) : "-",
                          m_fpsCurrent,
                          haveFrame ? 1 : 0,
                          offThread ? "off-thread" : "inline",
                          (offThread && m_source->Ring().HasPersistentMapping())
                              ? "+zerocopy" : "",
                          modeBuf,
                          latencyBuf,
                          static_cast<unsigned long long>(
                              m_source ? m_source->Ring().UploadedCount() : 0ull),
                          static_cast<unsigned long long>(
                              m_source ? m_source->Ring().ConsumerMissedCount() : 0ull),
                          static_cast<unsigned long long>(
                              m_fieldGate.repairs),
                          static_cast<unsigned long long>(
                              m_source ? m_source->Queue().ProducedCount() : 0ull),
                          static_cast<unsigned long long>(
                              m_source ? m_source->Queue().DroppedCount() : 0ull),
                          cap ? "yes" : "no",
                          m_source ? m_source->Clips() : 0);
            FFGLLog::LogToHost(dline);
        }
    }

    // -------------------------------------------------------------------
    // 4. Diagnostic overlay.
    // -------------------------------------------------------------------
    if (m_showStats > 0.5f)
    {
        const bool ajaActive = (cap != nullptr);

        // Frame age / queue wait are only meaningful when we actually
        // rendered captured content this frame. If the texture was not
        // sampled (no signal, no frame yet), suppress these to avoid
        // confusing huge numbers based on stale timestamps.
        const double frameAge = haveFrame
            ? std::chrono::duration<double, std::milli>(now - producedAt).count()
            : 0.0;
        const double queueWait = haveFrame
            ? std::chrono::duration<double, std::milli>(now - publishedAt).count()
            : 0.0;

        // Synth-only diagnostic: the configured ring depth. Consumer
        // misses: publishes overwritten before any clip of the input
        // rendered them (shared ring) or dropped queue frames.
        const int      ringDepth = CurrentRingFrameCount();
        const uint64_t drops = !m_source ? 0ull
            : (m_source->OffThreadUpload()
                   ? m_source->Ring().ConsumerMissedCount()
                   : m_source->Queue().DroppedCount());

        // ----- Per-line text. Layout differs based on source -----
        char lineVer[40];
        char line1[64], line2[64], line3[64];
        char line4[64], line5[64], line6[64], line7[64];

        // The version label gets a " [LL]" suffix when the AJA path is
        // running in low-latency mode. This is the cheapest way to
        // make the active transfer mode visible at a glance without
        // adding a dedicated overlay line; the suffix only appears
        // when the toggle is on AND the AJA capture is alive, so
        // synthetic and AutoCirculate sessions stay free of it. A
        // "10b" suffix follows for the v0.29.2 10-bit capture path.
        //
        // The number comes from kPluginVersionMajor/Minor, the same
        // constants the FFGL PluginInfo registration uses.
        const bool tenBit = (SourceKind() == ajainput::PixelKind::V210);
        const char* suffix = "";
        if (CurrentLowLatencyMode() && cap && tenBit)
            suffix = " [LL 10b]";
        else if (CurrentLowLatencyMode() && cap)
            suffix = " [LL]";
        else if (cap && tenBit)
            suffix = " [10b]";
        std::snprintf(lineVer, sizeof(lineVer), "v%d.%d%s",
                      kPluginVersionMajor, kPluginVersionMinor, suffix);

        if (ajaActive)
        {
            // Capture-specific lines replace the upload-latency lines
            // (which are zero in off-thread mode anyway) and the
            // ring-depth line (the simulator's delay line is bypassed
            // when AJA owns the source).
            //
            // line1: AJA state + FrameStore index.
            // line2: timecode HH:MM:SS:FF if the input carries valid
            //        RP188; otherwise the running capture count, which
            //        is the next most useful single number when no TC
            //        is embedded.
            // line6: driver-side drops, augmented with CRC error count
            //        when SDI error reporting is active. When the SDI
            //        receiver loses lock the line switches to a hard
            //        "no lock" indicator instead so it is impossible to
            //        miss visually.
            const ajainput::AJACaptureSource::State st = cap->GetState();
            // v0.41: input, 1-based FrameStore, and how many clips share
            // the capture.
            const int clips = m_source ? m_source->Clips() : 0;
            char fsSet[16];
            FrameStoreSetLower(cap->FrameStoreMask(), fsSet, sizeof(fsSet));
            if (clips > 1)
                std::snprintf(line1, sizeof(line1), "aja %s %s %s x%d",
                              ajainput::AJACaptureSource::ToString(st),
                              InputLabelLower(cap->SelectedInputNumber()),
                              fsSet, clips);
            else
                std::snprintf(line1, sizeof(line1), "aja %s %s %s",
                              ajainput::AJACaptureSource::ToString(st),
                              InputLabelLower(cap->SelectedInputNumber()),
                              fsSet);

            if (cap->HasTimecode())
            {
                const uint32_t tc = cap->LastTimecodeHMSF();
                const unsigned h = (tc >> 24) & 0xff;
                const unsigned m = (tc >> 16) & 0xff;
                const unsigned s = (tc >>  8) & 0xff;
                const unsigned f = (tc      ) & 0xff;
                std::snprintf(line2, sizeof(line2),
                              "tc %02u:%02u:%02u:%02u", h, m, s, f);
            }
            else
            {
                std::snprintf(line2, sizeof(line2), "caps %llu",
                              static_cast<unsigned long long>(
                                  cap->FramesCaptured()));
            }

            if (!cap->SdiLocked())
            {
                std::snprintf(line6, sizeof(line6), "sdi no lock");
            }
            else
            {
                const uint32_t crc = cap->SdiCrcErrors();
                if (crc > 0)
                    std::snprintf(line6, sizeof(line6),
                                  "drv %llu crc %u",
                                  static_cast<unsigned long long>(
                                      cap->DriverDrops()),
                                  static_cast<unsigned>(crc));
                else
                    std::snprintf(line6, sizeof(line6), "drv %llu",
                                  static_cast<unsigned long long>(
                                      cap->DriverDrops()));
            }
        }
        else
        {
            // Synthetic-mode overlay does not have a "state" or
            // "timecode" line, so line1 and line2 are left blank.
            // The downstream Render() call is a no-op on empty
            // strings (the overlay renderer guards against zero-
            // length input), which produces a small vertical gap
            // above the FPS line in synthetic mode. Acceptable
            // cost for keeping the line numbering consistent
            // between AJA and synthetic layouts.
            line1[0] = '\0';
            line2[0] = '\0';
            std::snprintf(line6, sizeof(line6), "ring %d", ringDepth);
        }

        std::snprintf(line3, sizeof(line3), "FPS %.1f", m_fpsCurrent);
        std::snprintf(line4, sizeof(line4), "age %.1f ms", frameAge);
        std::snprintf(line5, sizeof(line5), "wait %.1f ms", queueWait);
        std::snprintf(line7, sizeof(line7), "drops %llu",
                      static_cast<unsigned long long>(drops));

        // ----- Colours ---------------------------------------------------
        // The AJA state line colour mirrors the broadcast world: green for
        // running, red for any not-receiving state, yellow during the
        // brief init handshake.
        float ajaStateColor[3] = { 0.2f, 1.0f, 0.4f };
        if (ajaActive)
        {
            switch (cap->GetState())
            {
                case ajainput::AJACaptureSource::State::Running:
                    ajaStateColor[0] = 0.2f; ajaStateColor[1] = 1.0f; ajaStateColor[2] = 0.4f;
                    break;
                case ajainput::AJACaptureSource::State::Initializing:
                    ajaStateColor[0] = 1.0f; ajaStateColor[1] = 0.85f; ajaStateColor[2] = 0.2f;
                    break;
                case ajainput::AJACaptureSource::State::NoSignal:
                case ajainput::AJACaptureSource::State::Failed:
                case ajainput::AJACaptureSource::State::Idle:
                    ajaStateColor[0] = 1.0f; ajaStateColor[1] = 0.3f; ajaStateColor[2] = 0.3f;
                    break;
            }
        }

        float ageColor[3] = { 0.2f, 1.0f, 0.4f };
        if (frameAge > 40.0)      { ageColor[0] = 1.0f; ageColor[1] = 0.3f; ageColor[2] = 0.3f; }
        else if (frameAge > 20.0) { ageColor[0] = 1.0f; ageColor[1] = 0.85f; ageColor[2] = 0.2f; }

        float waitColor[3] = { 0.2f, 1.0f, 0.4f };
        if (queueWait > 15.0)     { waitColor[0] = 1.0f; waitColor[1] = 0.3f; waitColor[2] = 0.3f; }
        else if (queueWait > 5.0) { waitColor[0] = 1.0f; waitColor[1] = 0.85f; waitColor[2] = 0.2f; }

        float dropColor[3] = { 0.2f, 1.0f, 0.4f };
        if (drops > 0) { dropColor[0] = 1.0f; dropColor[1] = 0.3f; dropColor[2] = 0.3f; }

        const float white[3] = { 1.0f, 1.0f, 1.0f };
        const float versionColor[3] = { 0.6f, 0.85f, 1.0f };

        const float h        = 0.05f;
        const float lineStep = h * 1.3f;
        const float x0       = -0.97f;
        const float y0       = 0.95f;

        // First line: version (always). Then the source-dependent block.
        // For AJA mode, line1 is the state colour; synthetic mode leaves
        // line1 blank so the colour is irrelevant — pass white as a
        // harmless default.
        const float* l1Color = ajaActive ? ajaStateColor : white;
        const float* l6Color = white;  // ring or drv: neutral

        m_overlay.Render(lineVer, x0, y0,                   h, versionColor);
        m_overlay.Render(line1,   x0, y0 - lineStep,        h, l1Color);
        m_overlay.Render(line2,   x0, y0 - lineStep * 2.0f, h, white);
        m_overlay.Render(line3,   x0, y0 - lineStep * 3.0f, h, white);
        m_overlay.Render(line4,   x0, y0 - lineStep * 4.0f, h, ageColor);
        m_overlay.Render(line5,   x0, y0 - lineStep * 5.0f, h, waitColor);
        m_overlay.Render(line6,   x0, y0 - lineStep * 6.0f, h, l6Color);
        m_overlay.Render(line7,   x0, y0 - lineStep * 7.0f, h, dropColor);

        // Red warning lines below the stats, each only while it lasts.
        const float red[3] = { 1.0f, 0.3f, 0.3f };
        float warnRow = 8.0f;

        // v0.40: frame-rate family clash with the other input on this
        // card.
        const int clashRaw = m_source ? m_source->RateClashFormatRaw() : 0;
        if (ajaActive && clashRaw != 0)
        {
            char line8[64];
            std::snprintf(line8, sizeof(line8),
                          m_source->SingleFormatCard() ? "format clash %s %s"
                                                       : "rate clash %s %s",
                          InputLabelLower(m_source->RateClashInput()),
                          FormatNameForRaw(clashRaw).c_str());
            m_overlay.Render(line8, x0, y0 - lineStep * warnRow, h, red);
            warnRow += 1.0f;
        }

        // v0.40: connector / FrameStore used by another application.
        if (ajaActive)
        {
            const char* foreign = ForeignUseText(
                cap->RecentForeignUse(), /*upperCase*/false);
            if (foreign != nullptr)
            {
                m_overlay.Render(foreign, x0, y0 - lineStep * warnRow, h, red);
                warnRow += 1.0f;
            }
        }

        // v0.41: every FrameStore is used by inputs on screen.
        if (m_source && m_source->WaitingForFrameStore())
        {
            char lineWait[96];
            std::snprintf(lineWait, sizeof(lineWait),
                          m_source->NoRoutableFrameStore()
                              ? "card cannot route %s to a framestore"
                              : "%s waiting for a free framestore",
                          InputLabelLower(m_source->InputNumber()));
            m_overlay.Render(lineWait, x0, y0 - lineStep * warnRow, h, red);
            warnRow += 1.0f;
        }

        // v0.41: why the capture is not running (routing failed,
        // AutoCirculate init failed, ...), so the cause is on screen.
        if (ajaActive &&
            cap->GetState() != ajainput::AJACaptureSource::State::Running)
        {
            const char* reason = cap->StatusReason();
            if (reason != nullptr)
                m_overlay.Render(reason, x0, y0 - lineStep * warnRow, h, red);
        }
    }

    // Every read of the acquired ring slot (history blits, repair pass,
    // weave draw) has been queued by now: let the producer know when the
    // GPU is done with it (v0.39 write-after-read guard).
    if (ringAcquired && m_source)
        m_source->EndRead();

    return FF_SUCCESS;
}

FFResult AJAInputPlugin::DeInitGL()
{
    // Mark the GL lifecycle as torn down BEFORE doing anything else, so
    // any concurrent SetFloatParameter callback that races with DeInitGL
    // takes the "not initialised" branch.
    m_initGLDone = false;

    // v0.41: let go of the shared capture. When this was the last clip of
    // its input, the capture thread is joined, the FrameStore returned
    // and the ring and card released right here (host context current).
    DetachSource();
    ajainput::SharedCapture::CloseSpareDevices();

    ReleaseYadifHistory();
    ReleaseHeldFrame();
    m_overlay.Release();
    m_shader.FreeGLResources();
    m_fieldCopyShader.FreeGLResources();
    m_uFieldCopySource = m_uFieldCopyParity = -1;
    m_quad.Release();

    m_uTexture = m_uImageSize = -1;
    m_uColorNormalize = m_uColorMatrix = m_uInterlaceMode = -1;
    m_uFieldParity = m_uHDRMode = m_uPixelFormatMode = -1;
    m_uPrevTexture = m_uNextTexture = m_uTopFieldFirst = -1;

    FFGLLog::LogToHost("AJAInput: DeInitGL completed");
    return FF_SUCCESS;
}

// =============================================================================
// Shared capture (v0.41).
// =============================================================================

ajainput::CaptureSettings AJAInputPlugin::CurrentSettings() const noexcept
{
    ajainput::CaptureSettings s;
    s.ringFrameCount = CurrentRingFrameCount();
    s.lowLatency     = CurrentLowLatencyMode();
    s.tenBit         = (m_tenBitMode >= 0.5f);
    return s;
}

void AJAInputPlugin::AttachSource()
{
    bool created = false;
    std::shared_ptr<ajainput::SharedCapture> src = ajainput::SharedCapture::Attach(
        DeviceIndexParam(), InputNumber(), CurrentSettings(), created);
    src->ClipAttached();
    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        m_source = src;
        // A new capture started with the settings read before Attach,
        // which can take a while (card open, probe). A setter that ran
        // meanwhile found no capture and only updated the mirror; push
        // the mirrors again under the lock, after which setters forward.
        if (created)
        {
            src->SetRingFrameCount(CurrentRingFrameCount());
            src->SetLowLatency(CurrentLowLatencyMode());
            src->SetTenBit(m_tenBitMode >= 0.5f);
        }
    }

    // Size the per-clip state for the capture's format on the next sync,
    // and adopt its settings: a clip joining an input takes the input's
    // capture settings, as every clip of a Resolume live input shares them.
    m_seenFormatGeneration   = 0;
    m_seenSettingsGeneration = created ? src->SettingsGeneration() : 0;
    m_lastRingSequence       = ~0ull;
    SyncFromSource();

    char line[200];
    std::snprintf(line, sizeof(line),
                  "AJAInput: clip attached to the %s capture (%s, %d "
                  "clip%s on it)",
                  InputLabelUpper(src->InputNumber()),
                  created ? "new" : "shared",
                  src->Clips(), src->Clips() == 1 ? "" : "s");
    FFGLLog::LogToHost(line);
}

void AJAInputPlugin::DetachSource()
{
    // Moved out under the lock (another thread may be reading through it),
    // released outside it: when this was the input's last clip the
    // capture is destroyed here, on the render thread.
    std::shared_ptr<ajainput::SharedCapture> old;
    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        old = std::move(m_source);
        m_source.reset();
    }
    if (!old)
        return;
    old->ClipDetached();
    old.reset();
}

void AJAInputPlugin::SyncFromSource()
{
    if (!m_source)
        return;

    if (m_source->FormatGeneration() != m_seenFormatGeneration)
    {
        m_seenFormatGeneration = m_source->FormatGeneration();
        m_currentFormat = m_source->Format();
        // YADIF needs the ring's texture layout (off-thread path).
        if (m_source->OffThreadUpload())
            EnsureYadifHistory(m_currentFormat.width, m_currentFormat.height,
                               m_source->Kind());
        else
            ReleaseYadifHistory();
        m_lastRingSequence = ~0ull;
    }

    const uint64_t gen = m_source->SettingsGeneration();
    if (gen != m_seenSettingsGeneration)
    {
        m_seenSettingsGeneration = gen;
        const ajainput::CaptureSettings st = m_source->Settings();
        m_ringDepth      = static_cast<float>(st.ringFrameCount - 2);
        m_lowLatencyMode = st.lowLatency ? 1.0f : 0.0f;
        m_tenBitMode     = st.tenBit ? 1.0f : 0.0f;
        m_settingsEventsPending = true;
    }
}

// =============================================================================
// YADIF history textures
// =============================================================================
//
// Three GL textures plus two FBOs. Each texture mirrors the layout of
// the SharedTextureRing's slots (same width, height, and pixel kind), so
// a blit from the ring's current slot into one of the history textures
// is a straight 1:1 copy. The ring texture is attached to the read FBO
// and the destination history texture to the draw FBO for each blit
// (see the m_yadifBlitReadFBO declaration for why one FBO is not
// enough). The v0.39 field repair draws into the draw FBO with the
// row-parity copy shader instead of blitting.
//
// Cost per arrived ring frame: one glBlitFramebuffer at ring-slot
// resolution (1920×540 RGBA8 for 1080i UYVY half-width = ~4 MB), which
// completes in well under a millisecond on any modern GPU.

bool AJAInputPlugin::EnsureYadifHistory(std::size_t widthPixels,
                                     std::size_t heightPixels,
                                     ajainput::PixelKind kind)
{
    if (m_yadifHistoryTex[0] != 0 &&
        m_yadifHistoryW == widthPixels &&
        m_yadifHistoryH == heightPixels &&
        m_yadifHistoryKind == kind)
    {
        // The GL allocation is already at the right dimensions, so
        // we skip the texture realloc. But the slot bookkeeping must
        // still reset: callers reach Ensure on a format change
        // (SyncFromSource), and the buffered history is from
        // the previous format. If we kept framesBuffered >= 3 from
        // before, the very first render after the change would sample
        // textures from the OLD format as the yadif prev/next inputs.
        // Self-heals in 3 captures, but the resulting one-render
        // glitch is avoidable for the cost of two integer stores.
        m_yadifSlots.Reset();
        // Clear the v0.32 per-field FieldParity cache so the next
        // session's first render falls back to the counter-based
        // path until a new fresh ring publish arrives with a hint.
        m_lastFreshFieldHint =
            ajainput::SharedTextureRing::FieldHint::FullFrame;
        m_fieldGate.Reset();
        return true;
    }

    ReleaseYadifHistory();

    // Match the macropixel layout of SharedTextureRing: UYVY uses
    // half-width RGBA8 (one texel = one UYVY pair = two image pixels).
    // V210 uses a different texel-per-image-pixel ratio but for now
    // yadif is only enabled on the UYVY path, so we allocate at the
    // UYVY layout regardless of kind. The V210 path never reads the
    // history textures so their content is irrelevant.
    const GLsizei texW = static_cast<GLsizei>(widthPixels / 2);
    const GLsizei texH = static_cast<GLsizei>(heightPixels);

    glGenTextures(3, m_yadifHistoryTex);
    for (int i = 0; i < 3; ++i)
    {
        glBindTexture(GL_TEXTURE_2D, m_yadifHistoryTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     texW, texH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    // Two FBOs: see the field declaration comment for why.
    glGenFramebuffers(1, &m_yadifBlitReadFBO);
    glGenFramebuffers(1, &m_yadifBlitDrawFBO);
    // Don't attach anything yet; BlitRingFrameIntoHistory will rebind
    // attachments per blit.

    m_yadifHistoryW    = widthPixels;
    m_yadifHistoryH    = heightPixels;
    m_yadifHistoryKind = kind;
    m_yadifSlots.Reset();
    m_lastFreshFieldHint =
        ajainput::SharedTextureRing::FieldHint::FullFrame;
    m_fieldGate.Reset();
    m_yadifBlitCompleteLogged = false;

    const GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "AJAInput: yadif history allocation failed, GL error 0x%X",
                      err);
        FFGLLog::LogToHost(line);
        ReleaseYadifHistory();
        return false;
    }

    char line[160];
    std::snprintf(line, sizeof(line),
                  "AJAInput: yadif history allocated (%dx%d RGBA8, 3 slots)",
                  texW, texH);
    FFGLLog::LogToHost(line);
    return true;
}

void AJAInputPlugin::ReleaseYadifHistory()
{
    if (m_yadifHistoryTex[0] != 0)
    {
        glDeleteTextures(3, m_yadifHistoryTex);
        m_yadifHistoryTex[0] = m_yadifHistoryTex[1] = m_yadifHistoryTex[2] = 0;
    }
    if (m_yadifBlitReadFBO != 0)
    {
        glDeleteFramebuffers(1, &m_yadifBlitReadFBO);
        m_yadifBlitReadFBO = 0;
    }
    if (m_yadifBlitDrawFBO != 0)
    {
        glDeleteFramebuffers(1, &m_yadifBlitDrawFBO);
        m_yadifBlitDrawFBO = 0;
    }
    m_yadifHistoryW = 0;
    m_yadifHistoryH = 0;
    m_yadifSlots.Reset();
}

void AJAInputPlugin::BlitRingFrameIntoHistory(GLuint sourceTexture,
                                            std::uint64_t sourceSequence)
{
    if (m_yadifBlitReadFBO == 0 || m_yadifBlitDrawFBO == 0 ||
        m_yadifHistoryTex[0] == 0)
        return;  // history not allocated yet
    if (sourceTexture == 0)
        return;  // nothing to copy
    // The history textures are allocated at the UYVY macropixel layout
    // (width/2 RGBA8 texels). In V210 mode the ring's textures have a
    // different layout (alignedWidth/6*4 texels), AND the render path
    // is gated on UYVY anyway — the V210 path uses simple bob and
    // never samples the history. So the blit is pure waste in V210
    // mode (~3 MB/frame at 1080 with no consumer). Skip it.
    if (m_yadifHistoryKind != ajainput::PixelKind::UYVY)
        return;

    // Pure rotation: returns the destination slot and updates the
    // prev/cur/next indices. Idempotent against duplicate sequences,
    // so re-acquiring the same ring frame is a no-op.
    const auto adv = m_yadifSlots.Advance(sourceSequence);
    if (!adv.didAdvance)
        return;
    BlitRingTextureToHistorySlot(sourceTexture, adv.destinationSlot);
}

void AJAInputPlugin::BlitRingFieldIntoCurrentNextSlot(GLuint sourceTexture)
{
    // v0.32 per-field path. Called when the ring publishes the SECOND
    // field of a source frame (F1 in AC terminology) — i.e. NOT a
    // frame-start. We do NOT call YadifSlots::Advance; the rotation
    // was already done on this frame's F0 arrival. The blit just
    // copies the freshly-republished ring texture into the SAME
    // "next" slot, which now contains both fields refreshed (the F0
    // half was written on the previous F0-publish blit, the F1 half
    // was written into the ring by the capture-thread scatter, and
    // this blit propagates the updated ring texture into the history
    // slot).
    if (m_yadifBlitReadFBO == 0 || m_yadifBlitDrawFBO == 0 ||
        m_yadifHistoryTex[0] == 0 || sourceTexture == 0)
        return;
    if (m_yadifHistoryKind != ajainput::PixelKind::UYVY)
        return;
    // m_yadifSlots.framesBuffered must be >= 1 for nextSlot to be
    // meaningful (Reset leaves slots in a defined state but no
    // frames have been buffered, so the renderer also needs >=3 to
    // sample; using nextSlot here just before the first F0 is
    // harmless but produces no visible output anyway).
    BlitRingTextureToHistorySlot(sourceTexture, m_yadifSlots.nextSlot);
}

void AJAInputPlugin::BlitRingTextureToHistorySlot(GLuint sourceTexture,
                                                int destinationSlot)
{
    if (destinationSlot < 0 || destinationSlot >= 3)
        return;
    const int incomingSlot = destinationSlot;

    const GLsizei texW = static_cast<GLsizei>(m_yadifHistoryW / 2);
    const GLsizei texH = static_cast<GLsizei>(m_yadifHistoryH);

    // Save current FBO bindings so the surrounding render code does
    // not see a transient state change. FFGL host code does not
    // reliably restore FBOs between plugins, so we restore by hand.
    GLint prevReadFBO = 0, prevDrawFBO = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);

    // SRC FBO: source ring texture attached as COLOR_ATTACHMENT0,
    // bound to GL_READ_FRAMEBUFFER. DST FBO: chosen history slot
    // attached as COLOR_ATTACHMENT0, bound to GL_DRAW_FRAMEBUFFER.
    // Two distinct FBO objects are required — see the field
    // declaration comment in the header for why a single FBO with
    // separate read/draw bindings does not work.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_yadifBlitReadFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, sourceTexture, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_yadifBlitDrawFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_yadifHistoryTex[incomingSlot], 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);

    // Defensive completeness check. Surface incomplete-FBO errors
    // exactly once per ProcessOpenGL call rather than letting them
    // hide behind a silent blit failure. The 'once' is gated by
    // m_yadifBlitCompleteLogged so a per-frame error does not flood
    // the log; we re-arm on each EnsureYadifHistory.
    const GLenum readStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    const GLenum drawStatus = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if ((readStatus != GL_FRAMEBUFFER_COMPLETE ||
         drawStatus != GL_FRAMEBUFFER_COMPLETE) &&
        !m_yadifBlitCompleteLogged)
    {
        char line[200];
        std::snprintf(line, sizeof(line),
                      "AJAInput: yadif blit FBO incomplete "
                      "(read=0x%X draw=0x%X); deinterlace path will "
                      "show black/green output until the next "
                      "EnsureYadifHistory",
                      readStatus, drawStatus);
        FFGLLog::LogToHost(line);
        m_yadifBlitCompleteLogged = true;
    }

    // Same dimensions on both ends; GL_NEAREST avoids any filter
    // ambiguity since the formats are identical RGBA8. glBlitFramebuffer
    // honours the scissor test, and nothing guarantees the host left it
    // off, so disable it around the blit (v0.39).
    const GLboolean scissorOn = glIsEnabled(GL_SCISSOR_TEST);
    if (scissorOn)
        glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, texW, texH,
                      0, 0, texW, texH,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissorOn)
        glEnable(GL_SCISSOR_TEST);

    // Detach to allow the textures to be freed cleanly later, and
    // restore the previously-bound framebuffers.
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevReadFBO));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDrawFBO));
}

void AJAInputPlugin::CopyRingRowsIntoHistorySlot(GLuint sourceTexture,
                                              int    destinationSlot,
                                              int    rowParity)
{
    if (m_fieldCopyShader.GetGLID() == 0 || m_yadifBlitDrawFBO == 0 ||
        m_yadifHistoryTex[0] == 0 || sourceTexture == 0)
        return;
    if (destinationSlot < 0 || destinationSlot >= 3)
        return;
    if (m_yadifHistoryKind != ajainput::PixelKind::UYVY)
        return;

    const GLsizei texW = static_cast<GLsizei>(m_yadifHistoryW / 2);
    const GLsizei texH = static_cast<GLsizei>(m_yadifHistoryH);

    // Save everything this pass changes. The FFGL host expects its
    // state back, and the main draw that follows relies on it too.
    GLint prevDrawFBO = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);
    GLint prevViewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    const GLboolean blendOn   = glIsEnabled(GL_BLEND);
    const GLboolean scissorOn = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean depthOn   = glIsEnabled(GL_DEPTH_TEST);
    // UYVY texels carry Y1 in alpha: a host-left colour mask must not
    // drop any channel of the copy.
    GLboolean colorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_yadifBlitDrawFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_yadifHistoryTex[destinationSlot], 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, texW, texH);
    if (blendOn)   glDisable(GL_BLEND);
    if (scissorOn) glDisable(GL_SCISSOR_TEST);
    if (depthOn)   glDisable(GL_DEPTH_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
    {
        ScopedShaderBinding bind(m_fieldCopyShader.GetGLID());
        glUniform1i(m_uFieldCopySource, 0);
        glUniform1i(m_uFieldCopyParity, rowParity & 1);
        glActiveTexture(GL_TEXTURE0);
        Scoped2DTextureBinding textureBind(sourceTexture);
        m_quad.Draw();
    }

    if (blendOn)   glEnable(GL_BLEND);
    if (scissorOn) glEnable(GL_SCISSOR_TEST);
    if (depthOn)   glEnable(GL_DEPTH_TEST);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glViewport(prevViewport[0], prevViewport[1],
               prevViewport[2], prevViewport[3]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDrawFBO));
}

// =============================================================================
// Hold Last Frame (v0.42).
// =============================================================================
//
// The clip copies what it just drew (the host framebuffer's viewport) into
// a texture of its own each frame while No Signal is Hold Last Frame, and
// blits it back while the input has no picture. Copying the drawn result,
// not the captured frame, keeps it independent of the capture's texture
// ring (reallocated on a format change) and of de-interlacing.

namespace
{
/// Saves the framebuffer bindings and the scissor test (which clips
/// blits) and restores them on scope exit.
struct BlitStateGuard
{
    GLint     readFbo = 0;
    GLint     drawFbo = 0;
    GLboolean scissor = GL_FALSE;
    BlitStateGuard()
    {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        if (scissor)
            glDisable(GL_SCISSOR_TEST);
    }
    ~BlitStateGuard()
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
        if (scissor)
            glEnable(GL_SCISSOR_TEST);
    }
    BlitStateGuard(const BlitStateGuard&)            = delete;
    BlitStateGuard& operator=(const BlitStateGuard&) = delete;
};
} // namespace

void AJAInputPlugin::SaveHeldFrame()
{
    const int w = static_cast<int>(currentViewport.width);
    const int h = static_cast<int>(currentViewport.height);
    if (w <= 0 || h <= 0)
    {
        m_holdValid = false;
        return;
    }
    if (w == m_holdFailedW && h == m_holdFailedH)
        return;   // target incomplete at this size; retried after a resize

    // Keep the host's precision: a 16-bit (half float) copy when the
    // host framebuffer has more than 8 bits per channel (Resolume's
    // 16-bit compositions).
    GLenum internal = GL_RGBA8;
    {
        GLint drawFbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        if (drawFbo != 0)
        {
            GLint type = GL_NONE;
            glGetFramebufferAttachmentParameteriv(
                GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
            GLint redBits = 8;
            if (type != GL_NONE)
                glGetFramebufferAttachmentParameteriv(
                    GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                    GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &redBits);
            if (redBits > 8)
                internal = GL_RGBA16F;
        }
    }

    if (m_holdTex == 0 || m_holdFBO == 0 || m_holdW != w || m_holdH != h ||
        m_holdInternal != static_cast<unsigned>(internal))
    {
        ReleaseHeldFrame();
        GLint prevTex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        glGenTextures(1, &m_holdTex);
        glBindTexture(GL_TEXTURE_2D, m_holdTex);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
        glGenFramebuffers(1, &m_holdFBO);
        bool complete = false;
        {
            BlitStateGuard guard;
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_holdFBO);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, m_holdTex, 0);
            complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
                       GL_FRAMEBUFFER_COMPLETE;
        }
        if (!complete)
        {
            FFGLLog::LogToHost("AJAInput: Hold Last Frame target incomplete; "
                               "not holding at this clip size");
            ReleaseHeldFrame();
            m_holdFailedW = w;
            m_holdFailedH = h;
            return;
        }
        m_holdW        = w;
        m_holdH        = h;
        m_holdInternal = static_cast<unsigned>(internal);
        m_holdFailedW  = 0;
        m_holdFailedH  = 0;
    }

    const int x = static_cast<int>(currentViewport.x);
    const int y = static_cast<int>(currentViewport.y);
    BlitStateGuard guard;
    // The host's framebuffer is the one bound for drawing.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(guard.drawFbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_holdFBO);
    glBlitFramebuffer(x, y, x + w, y + h, 0, 0, w, h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    m_holdValid  = true;
    m_holdInput  = InputNumber();
    m_holdDevice = DeviceIndexParam();
}

bool AJAInputPlugin::DrawHeldFrame()
{
    if (!m_holdValid || m_holdFBO == 0)
        return false;
    const int w = static_cast<int>(currentViewport.width);
    const int h = static_cast<int>(currentViewport.height);
    if (w != m_holdW || h != m_holdH)
    {
        m_holdValid = false;   // the host resized the clip
        return false;
    }
    const int x = static_cast<int>(currentViewport.x);
    const int y = static_cast<int>(currentViewport.y);
    BlitStateGuard guard;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_holdFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(guard.drawFbo));
    glBlitFramebuffer(0, 0, w, h, x, y, x + w, y + h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    return true;
}

void AJAInputPlugin::ReleaseHeldFrame()
{
    if (m_holdFBO != 0)
    {
        glDeleteFramebuffers(1, &m_holdFBO);
        m_holdFBO = 0;
    }
    if (m_holdTex != 0)
    {
        glDeleteTextures(1, &m_holdTex);
        m_holdTex = 0;
    }
    m_holdW        = 0;
    m_holdH        = 0;
    m_holdInternal = 0;
    m_holdValid    = false;
}

// =============================================================================
// Parameter access.
// =============================================================================

FFResult AJAInputPlugin::SetFloatParameter(unsigned int index, float value)
{
    switch (static_cast<ParamID>(index))
    {
        case PID_InputSource:
        {
            // v0.41: a different input is a different shared capture; the
            // render thread re-attaches the next time it renders this
            // clip (the old capture closes there if this was its last
            // clip). Until then the clip reports its own values.
            m_inputSource = value;
            return FF_SUCCESS;
        }

        case PID_ShowStats:
            m_showStats = value;
            return FF_SUCCESS;

        case PID_RingDepth:
        {
            // Option index 0..5 = ring depth 2..7. v0.41: a capture
            // setting of the input, shared by all its clips; the shared
            // capture applies it on the render thread (AutoCirculate ring
            // frame count, re-armed on change; the synthetic producer's
            // delay line). Before the clip is attached the mirror is all
            // there is, and a new capture starts with it.
            // The value forwarded comes from `value`, not the mirror, which
            // the render thread may be overwriting (SyncFromSource).
            m_ringDepth = value;
            {
                std::lock_guard<std::mutex> lock(m_sourceMutex);
                if (ajainput::SharedCapture* src = SourceForParamsLocked())
                    src->SetRingFrameCount(static_cast<int>(value + 0.5f) + 2);
            }
            return FF_SUCCESS;
        }

        case PID_DeviceIndex:
        {
            // v0.41: another card is another shared capture; the render
            // thread re-attaches the next time it renders this clip. A
            // host re-sending the same value changes nothing.
            // v0.42: any scanner index the pool tracks (0..15).
            m_deviceIndex = std::max(0.0f, std::min(15.0f, value));
            return FF_SUCCESS;
        }

        case PID_NoSignal:
            // v0.42: render-thread only, read every frame.
            m_noSignalMode = value;
            return FF_SUCCESS;

        case PID_Resize:
            // v0.42: as No Signal, read every frame.
            m_resizeMode = value;
            return FF_SUCCESS;

        case PID_Colorimetry:
            // Pure shader-uniform update. The new value is picked up on
            // the next ProcessOpenGL call when the uniforms are
            // re-uploaded; no pipeline tear-down is needed.
            m_colorimetry = value;
            return FF_SUCCESS;

        case PID_Deinterlace:
            // As above: the shader resolves the deinterlace uniform
            // per-frame against this mirror and the current format's
            // isInterlaced flag, so a single mirror update suffices.
            m_deinterlace = value;
            return FF_SUCCESS;

        case PID_LowLatencyMode:
            // Mirror the toggle then forward the resolved bool to the
            // AJA capture instance. Same pattern as channel and ring-
            // depth changes: the setter forces the capture loop back
            // through Initializing on its next iteration, which
            // momentarily flashes NoSignal and then re-arms the
            // alternative transfer path. The synthetic producer is
            // unaffected; mode toggling only applies to the AJA
            // path.
            // v0.41: shared by every clip of the input (see Ring Depth).
            m_lowLatencyMode = value;
            {
                std::lock_guard<std::mutex> lock(m_sourceMutex);
                if (ajainput::SharedCapture* src = SourceForParamsLocked())
                    src->SetLowLatency(value >= 0.5f);
            }
            return FF_SUCCESS;

        case PID_TenBitMode:
            // Toggling the pixel kind changes the FrameStore format, the
            // capture byte count AND the GL texture layout, so the
            // capture must be restarted with re-sized sinks. That needs
            // the GL context, so the shared capture does it in its next
            // Update() on the render thread (v0.41: for every clip of
            // the input; each clip re-sizes its YADIF history when it
            // sees the new format generation).
            m_tenBitMode = value;
            {
                std::lock_guard<std::mutex> lock(m_sourceMutex);
                if (ajainput::SharedCapture* src = SourceForParamsLocked())
                    src->SetTenBit(value >= 0.5f);
            }
            return FF_SUCCESS;

        default:
            return FF_FAIL;
    }
}

float AJAInputPlugin::GetFloatParameter(unsigned int index)
{
    // v0.41: the capture settings are the input's. Once attached, report
    // the shared value straight from the capture, so a clip that is not
    // being rendered (its mirror only follows in SyncFromSource) still
    // shows, and saves, what its input really uses. Any thread.
    switch (static_cast<ParamID>(index))
    {
        case PID_RingDepth:
        case PID_LowLatencyMode:
        case PID_TenBitMode:
        {
            std::lock_guard<std::mutex> lock(m_sourceMutex);
            if (ajainput::SharedCapture* src = SourceForParamsLocked())
            {
                const ajainput::CaptureSettings st = src->Settings();
                if (index == PID_RingDepth)
                    return static_cast<float>(st.ringFrameCount - 2);
                if (index == PID_LowLatencyMode)
                    return st.lowLatency ? 1.0f : 0.0f;
                return st.tenBit ? 1.0f : 0.0f;
            }
            break;
        }
        default:
            break;
    }

    switch (static_cast<ParamID>(index))
    {
        case PID_InputSource:    return m_inputSource;
        case PID_ShowStats:      return m_showStats;
        case PID_RingDepth:      return m_ringDepth;
        case PID_DeviceIndex:    return m_deviceIndex;
        case PID_Colorimetry:    return m_colorimetry;
        case PID_Deinterlace:    return m_deinterlace;
        case PID_LowLatencyMode: return m_lowLatencyMode;
        case PID_TenBitMode:     return m_tenBitMode;
        case PID_NoSignal:       return m_noSignalMode;
        case PID_Resize:         return m_resizeMode;
        default:                return 0.0f;
    }
}

FFResult AJAInputPlugin::SetTextParameter(unsigned int index, const char* /*value*/)
{
    // All text parameters we expose are semantically read-only.
    // Resolume calls SetTextParameter when the user types into the
    // inspector field, and also during composition load with the
    // value that was saved last time. In both cases we silently
    // ignore the incoming string and let the next GetTextParameter
    // return our actual live state; the operator's edit reverts as
    // soon as focus leaves the field.
    switch (static_cast<ParamID>(index))
    {
        case PID_FormatInfo:
        case PID_StatsFrames:
        case PID_StatsDrops:
        case PID_StatsRecoveries:
        case PID_StatsIdleWakeups:
            return FF_SUCCESS;
        default:
            return FF_FAIL;
    }
}

char* AJAInputPlugin::GetTextParameter(unsigned int index)
{
    // Called on the host's GUI thread: everything comes from the shared
    // capture's thread-safe snapshot, taken under the clip's lock (lock
    // order: clip, then the capture's status mutex).
    ajainput::SharedCapture::Status st;
    bool attached = false;
    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        if (const ajainput::SharedCapture* src = SourceForParamsLocked())
        {
            st = src->Snapshot();
            attached = true;
        }
    }

    // Stats params: format the counter into the per-param buffer.
    // Recoveries and idle wakeups only exist on the AJA capture state
    // machine; synthetic sessions genuinely have zero of them.
    switch (static_cast<ParamID>(index))
    {
        case PID_StatsFrames:
            std::snprintf(m_statsFramesBuf, sizeof(m_statsFramesBuf),
                          "%llu", static_cast<unsigned long long>(st.frames));
            return m_statsFramesBuf;
        case PID_StatsDrops:
            std::snprintf(m_statsDropsBuf, sizeof(m_statsDropsBuf),
                          "%llu", static_cast<unsigned long long>(st.drops));
            return m_statsDropsBuf;
        case PID_StatsRecoveries:
            std::snprintf(m_statsRecoveriesBuf, sizeof(m_statsRecoveriesBuf),
                          "%llu", static_cast<unsigned long long>(st.recoveries));
            return m_statsRecoveriesBuf;
        case PID_StatsIdleWakeups:
            std::snprintf(m_statsIdleWakeupsBuf, sizeof(m_statsIdleWakeupsBuf),
                          "%llu", static_cast<unsigned long long>(st.idleWakeups));
            return m_statsIdleWakeupsBuf;
        case PID_FormatInfo:
            break;
        default:
            return nullptr;
    }

    // PID_FormatInfo: the same diagnostic the overlay shows.
    //   capture running            -> "1080p59.94 | SDI1 FS1"
    //   no lock                    -> "no signal | SDI1 FS1"
    //   configure failed           -> the reason ("routing failed", ...)
    //   no FrameStore free         -> "SDI2 waiting for a free FrameStore"
    //   FrameStore lent            -> "idle (FrameStore lent ...)"
    //   synthetic / no device      -> "synthetic WxH@fps"
    // plus "| N clips" when the input's capture is shared, and the
    // frame-rate clash / other-application warnings.
    if (!attached)
    {
        // Not rendered since it was created or its Input / Device changed.
        std::snprintf(m_formatInfoBuf, sizeof(m_formatInfoBuf),
                      "%s idle (starts when shown)", InputLabelUpper(InputNumber()));
        return m_formatInfoBuf;
    }
    if (st.synthetic)
    {
        std::snprintf(m_formatInfoBuf, sizeof(m_formatInfoBuf),
                      "synthetic %dx%d@%.2f",
                      static_cast<int>(st.width), static_cast<int>(st.height),
                      st.fps);
    }
    else if (st.waitingForFrameStore)
    {
        std::snprintf(m_formatInfoBuf, sizeof(m_formatInfoBuf),
                      st.noRoutableFrameStore
                          ? "card cannot route %s to a FrameStore"
                          : "%s waiting for a free FrameStore",
                      InputLabelUpper(st.inputNumber));
    }
    else if (!st.hasCapture)
    {
        std::snprintf(m_formatInfoBuf, sizeof(m_formatInfoBuf),
                      "%s", st.deviceOpen ? "starting" : "no device");
    }
    else if (st.yielded)
    {
        // Not on screen; its FrameStore went to an input that is.
        std::snprintf(m_formatInfoBuf, sizeof(m_formatInfoBuf),
                      "idle (FrameStore lent to a clip on screen)");
    }
    else if (st.state == ajainput::AJACaptureSource::State::Running)
    {
        ajainput::CapturedFormat decoded;
        if (st.lastFormatRaw != 0 &&
            ajainput::TryDecodeNTV2Format(st.lastFormatRaw, decoded) &&
            !decoded.name.empty())
            std::snprintf(m_formatInfoBuf, sizeof(m_formatInfoBuf),
                          "%s", decoded.name.c_str());
        else
            std::snprintf(m_formatInfoBuf, sizeof(m_formatInfoBuf),
                          "%dx%d (raw fmt %d)",
                          static_cast<int>(st.width), static_cast<int>(st.height),
                          st.lastFormatRaw);
    }
    else if (st.state == ajainput::AJACaptureSource::State::NoSignal ||
             (st.state == ajainput::AJACaptureSource::State::Initializing &&
              st.reason != nullptr))
    {
        // The configure failure reason when there is one; a retry
        // (Initializing) keeps showing it instead of flickering to "init".
        std::snprintf(m_formatInfoBuf, sizeof(m_formatInfoBuf),
                      "%s", st.reason != nullptr ? st.reason : "no signal");
    }
    else
    {
        std::snprintf(m_formatInfoBuf, sizeof(m_formatInfoBuf),
                      "%s", ajainput::AJACaptureSource::ToString(st.state));
    }

    auto append = [this](const char* fmt, auto... args)
    {
        const std::size_t used = std::strlen(m_formatInfoBuf);
        if (used + 1 < sizeof(m_formatInfoBuf))
            std::snprintf(m_formatInfoBuf + used, sizeof(m_formatInfoBuf) - used,
                          fmt, args...);
    };

    // Which input and FrameStore(s), and how many clips share the capture.
    if (st.hasCapture && !st.yielded && st.frameStore >= 0)
    {
        char set[16];
        ajainput::FormatFrameStoreSet(st.frameStoreMask != 0u
                                       ? st.frameStoreMask
                                       : (1u << st.frameStore),
                                   set, sizeof(set));
        append(" | %s %s", InputLabelUpper(st.inputNumber), set);
    }
    if (st.clips > 1)
        append(" | %d clips", st.clips);
    // v0.42: an HDR transfer the source signals (Colorimetry Auto
    // follows it).
    if (st.hasCapture && st.signalledTransfer != 0)
        append(" | %s", st.signalledTransfer == 1 ? "PQ" : "HLG");

    // Frame rate the card cannot mix with the other input's; appended in
    // every state, a clash can be why an input does not come up. v0.42:
    // on a card without multi-format any other format clashes.
    if (st.rateClashFormatRaw != 0)
        append(st.singleFormatCard ? " ! format clash: %s %s"
                                   : " ! rate clash: %s %s",
               InputLabelUpper(st.rateClashInput),
               FormatNameForRaw(st.rateClashFormatRaw).c_str());

    // Another application (or the AJA services) using this input's
    // connector or FrameStore.
    if (const char* foreign = ForeignUseText(st.foreignMask, /*upperCase*/true))
        append(" ! %s", foreign);

    return m_formatInfoBuf;
}
