// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    CaptureTopology.h
 * @brief   v0.42: which FrameStores a capture needs, given the input, whether
 *          the signal is 4K, and what the card can do.
 *
 * Most captures use one FrameStore fed straight from one input widget:
 * SD / HD / 3G on every card, and 4K on cards with 12G routing (Kona X,
 * Kona 5 with its 4x12G firmware, Corvid 44 12G), whose FrameStores take
 * a 4K raster from one cable.
 *
 * Cards without 12G routing build a 4K picture from four streams, the way
 * AJA's ntv2capture4k demo routes it (CNTV2DemoCommon::GetInputRouting4K):
 *   - Tsi2: two-sample interleave. Four streams (four SDI links, or the
 *     four quadrant outputs of an HDMI input) go through two 425 muxes
 *     into a FrameStore pair (1+2, 3+4, 5+6), switched to TSI mode.
 *   - Squares4: square division. Each SDI link carries one quadrant into
 *     its own FrameStore (1-4 or 5-8), the four switched to quad mode.
 * Cards with 425 muxes take Tsi2 (the common quad-link standard); cards
 * without them (Corvid 24, Kona 3G Quad) take Squares4. Both are
 * experimental: untested on hardware.
 *
 * Pure logic, unit-tested (tests/test_CaptureTopology.cpp).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace ajainput
{

/// The card capabilities the plan depends on (from AJADevice::Status).
struct CardCaps
{
    int  numFrameStores = 0;
    int  numSdiInputs   = 0;
    int  numHdmiInputs  = 0;
    int  numCSCs        = 0;
    bool can12gRouting  = false;
    bool can425Mux      = false;
    bool can4K          = false;
    bool multiFormat    = true;
    /// SMPTE 2110 / 2022 card: its "SDI" inputs are network streams.
    bool isIP           = false;
    /// Bit h-1: HDMI h's input widget has the four quadrant outputs a 4K
    /// picture through 425 muxes needs (KONA HDMI, Io 4K Plus; not the
    /// older widgets, e.g. Io 4K), per the SDK's routing tables.
    uint32_t hdmiQuadrantMask = 0;
    /// Bit h-1: HDMI h's input widget has an RGB output (older widgets
    /// only have the YCbCr one).
    uint32_t hdmiRgbMask      = 0;
};

enum class Topology : int
{
    Single   = 0,  ///< one FrameStore, one input widget
    Tsi2     = 1,  ///< 4K through two 425 muxes into a FrameStore pair
    Squares4 = 2   ///< 4K as four quadrants into four FrameStores
};

const char* ToString(Topology t) noexcept;

struct FrameStorePlan
{
    bool        ok              = false;
    Topology    topology        = Topology::Single;
    /// FrameStores the plan needs exactly (bit i = FrameStore i); 0 means
    /// any one FrameStore (the pool's preference order picks it).
    uint32_t    fixedMask       = 0;
    /// AutoCirculate channel of a fixed set (its lowest FrameStore).
    int         firstFrameStore = -1;
    /// Tsi2: first of the two 425 muxes.
    int         muxBase         = 0;
    /// Why the input cannot be captured (string literal) when !ok.
    const char* reason          = nullptr;
};

/**
 * @brief Tsi2 from an RGB HDMI input: the CSC (zero-based) that converts
 *        quadrant stream path (0..3) of the plan.
 *
 * AJA's demo uses CSCs 1-4 for HDMI 1 (CSC = path). On a card with two
 * CSCs per FrameStore (KONA HDMI: 8 for 4) the pair's own CSCs and the
 * ones four above them are used instead (HDMI 1: 1, 2, 5, 6; HDMI 2: 3,
 * 4, 7, 8), so a 4K picture never takes the CSC of a FrameStore an HD
 * RGB capture of another input uses (single captures use the CSC of
 * their FrameStore).
 */
int TsiCscIndex(const FrameStorePlan& plan, int path, const CardCaps& caps) noexcept;

/**
 * @param inputNumber  input number (InputId.h)
 * @param signalIs4K   the picture is 4K (3840/4096 x 2160); for a
 *                     quad-link input always true
 */
FrameStorePlan PlanFrameStores(int inputNumber, bool signalIs4K,
                               const CardCaps& caps) noexcept;

/// Whether the card can take a quad-link input whose first link is SDI 1
/// or SDI 5: those four SDI inputs exist, the card does 4K, has no 12G
/// routing (such cards take 4K on one cable), is not an IP card, and has
/// either 425 muxes and the FrameStore pair, or the four FrameStores.
bool CardOffersQuadLink(int firstSdiOneBased, const CardCaps& caps) noexcept;

/// "FS1", "FS1+2", "FS1-4" (or "FS?") into buf.
void FormatFrameStoreSet(uint32_t mask, char* buf, std::size_t size) noexcept;

/// @return number of bits set.
int FrameStoreCount(uint32_t mask) noexcept;

/// @return lowest FrameStore in the mask, or -1.
int LowestFrameStore(uint32_t mask) noexcept;

} // namespace ajainput
