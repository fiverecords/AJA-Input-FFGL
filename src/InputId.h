// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    InputId.h
 * @brief   The plugin's input numbers: which physical input a clip
 *          captures, independent of the card.
 *
 * Every layer of the plugin identifies an input by one integer, the
 * "input number". It is the Input parameter's option value + 1:
 *
 *   option value   input number   input
 *   0 .. 7         1 .. 8         SDI 1 .. SDI 8
 *   8 .. 11        9 .. 12        HDMI 1 .. HDMI 4
 *   12             13             SDI 1-4 quad-link 4K (experimental)
 *   13             14             SDI 5-8 quad-link 4K (experimental)
 *
 * Option values are stored in Resolume compositions: once published,
 * the numbering must not change (new inputs take the values after 13).
 *
 * A quad-link input is one 4K picture carried on four SDI cables, for
 * cards whose FrameStores cannot take 4K from one cable (no 12G
 * routing). Its "connector" is the first of the four.
 *
 * Header-only and free of AJA SDK headers, so the unit tests and the
 * UI layer can use it. The mapping to NTV2 enums lives in InputNTV2.h.
 */
#pragma once

namespace ajainput
{
namespace input
{

enum class Kind
{
    None,      ///< not a valid input number
    Sdi,       ///< one SDI connector
    Hdmi,      ///< one HDMI input
    SdiQuad    ///< four SDI connectors carrying one 4K picture
};

constexpr int kMaxInputNumber = 14;

constexpr Kind KindOf(int n) noexcept
{
    return (n >= 1 && n <= 8)   ? Kind::Sdi
         : (n >= 9 && n <= 12)  ? Kind::Hdmi
         : (n == 13 || n == 14) ? Kind::SdiQuad
                                : Kind::None;
}

constexpr bool IsValid(int n) noexcept { return KindOf(n) != Kind::None; }
constexpr bool IsSdi(int n) noexcept   { return KindOf(n) == Kind::Sdi; }
constexpr bool IsHdmi(int n) noexcept  { return KindOf(n) == Kind::Hdmi; }
constexpr bool IsQuad(int n) noexcept  { return KindOf(n) == Kind::SdiQuad; }
/// SDI single or quad-link: the connector(s) are SDI.
constexpr bool UsesSdi(int n) noexcept { return IsSdi(n) || IsQuad(n); }

/// @return SDI connector 1..8 of an SDI input, the first link (1 or 5)
///         of a quad-link input, 0 otherwise.
constexpr int SdiNumber(int n) noexcept
{
    return (n >= 1 && n <= 8) ? n
         : (n == 13)          ? 1
         : (n == 14)          ? 5
                              : 0;
}

/// @return HDMI input 1..4 of an HDMI input, 0 otherwise.
constexpr int HdmiNumber(int n) noexcept
{
    return (n >= 9 && n <= 12) ? n - 8 : 0;
}

/// @return Number of SDI connectors the input uses (4 for quad-link).
constexpr int ConnectorCount(int n) noexcept
{
    return IsQuad(n) ? 4 : (IsValid(n) ? 1 : 0);
}

/// @return Input number of SDI connector 1..8, or 0.
constexpr int FromSdi(int sdiOneBased) noexcept
{
    return (sdiOneBased >= 1 && sdiOneBased <= 8) ? sdiOneBased : 0;
}

/// @return Input number of HDMI input 1..4, or 0.
constexpr int FromHdmi(int hdmiOneBased) noexcept
{
    return (hdmiOneBased >= 1 && hdmiOneBased <= 4) ? hdmiOneBased + 8 : 0;
}

/// @return Input number of the quad-link input starting at SDI 1 or 5.
constexpr int QuadFromFirstSdi(int firstSdiOneBased) noexcept
{
    return firstSdiOneBased == 1 ? 13 : (firstSdiOneBased == 5 ? 14 : 0);
}

/// @return Input number for an Input parameter value; 0 when the value
///         names no input (e.g. a composition saved by a later version).
inline int FromOptionValue(float value) noexcept
{
    if (!(value > -0.5f) || value > static_cast<float>(kMaxInputNumber) - 0.5f)
        return 0;
    return static_cast<int>(value + 0.5f) + 1;
}

/// @return Input parameter value of an input number.
constexpr float ToOptionValue(int n) noexcept
{
    return static_cast<float>(n - 1);
}

/// Upper-case label for logs and the inspector: "SDI1".."SDI8",
/// "HDMI" (HDMI 1), "HDMI2".."HDMI4", "SDI1-4", "SDI5-8".
inline const char* LabelUpper(int n) noexcept
{
    static const char* const kLabels[kMaxInputNumber + 1] = {
        "input",
        "SDI1", "SDI2", "SDI3", "SDI4", "SDI5", "SDI6", "SDI7", "SDI8",
        "HDMI", "HDMI2", "HDMI3", "HDMI4",
        "SDI1-4", "SDI5-8" };
    return IsValid(n) ? kLabels[n] : kLabels[0];
}

/// Lower-case label for the overlay font.
inline const char* LabelLower(int n) noexcept
{
    static const char* const kLabels[kMaxInputNumber + 1] = {
        "input",
        "sdi1", "sdi2", "sdi3", "sdi4", "sdi5", "sdi6", "sdi7", "sdi8",
        "hdmi", "hdmi2", "hdmi3", "hdmi4",
        "sdi1-4", "sdi5-8" };
    return IsValid(n) ? kLabels[n] : kLabels[0];
}

/// Name in the Input dropdown. HDMI 1 reads "HDMI" unless the menu also
/// lists other HDMI inputs.
inline const char* MenuName(int n, bool severalHdmi) noexcept
{
    static const char* const kNames[kMaxInputNumber + 1] = {
        "?",
        "SDI 1", "SDI 2", "SDI 3", "SDI 4", "SDI 5", "SDI 6", "SDI 7", "SDI 8",
        "HDMI", "HDMI 2", "HDMI 3", "HDMI 4",
        "SDI 1-4 (4K)", "SDI 5-8 (4K)" };
    if (n == FromHdmi(1) && severalHdmi)
        return "HDMI 1";
    return IsValid(n) ? kNames[n] : kNames[0];
}

/// Order of the inputs in the dropdown: SDI 1..8, the quad-link inputs,
/// then HDMI 1..4.
constexpr int kMenuOrder[kMaxInputNumber] = {
    1, 2, 3, 4, 5, 6, 7, 8, 13, 14, 9, 10, 11, 12 };

} // namespace input
} // namespace ajainput
