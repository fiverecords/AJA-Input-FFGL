// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Regression tests for SyntheticFrameGenerator. The generator is the
// fallback path used when no AJA hardware is available; it produces
// an SMPTE 75% colour-bar test pattern in UYVY 4:2:2 layout.
//
// These tests verify INVARIANTS rather than re-deriving the BT.709
// limited-range arithmetic: the conversion math is itself the
// implementation under test, so checking expected bytes byte-for-byte
// would just be a tautology. Instead we check structural properties:
//
//   - Frame buffer is the size UYVY layout demands.
//   - Odd widths are rounded down to even (chroma sub-sampling
//     requires pairs of pixels).
//   - The bottom 25% of the frame is plain black (Y=16, U=128, V=128).
//   - The seven SMPTE bars in the top 75% have monotonically
//     descending luma (white > yellow > cyan > green > magenta >
//     red > blue) — a direct consequence of dropping R, then G,
//     then both, from the limited-range definition.
//   - The white bar has neutral chroma (no colour cast).
//   - The macropixel byte order is U, Y0, V, Y1 (confirmed via
//     the position of luma vs chroma within the macropixel).
//
// If a future refactor breaks any of these properties (the most likely
// cause being a byte-order regression in PackUYVY or a row-stride
// computation that drifts), at least one test fails clearly.

#include "test_framework.h"
#include "SyntheticFrameGenerator.h"
#include "PixelFormat.h"

#include <cstdint>

namespace
{

// Sample a single UYVY macropixel at (row, macropixelX). Returns the
// four bytes the generator wrote: [U, Y0, V, Y1].
struct Macropixel { uint8_t u, y0, v, y1; };

Macropixel SampleMacropixel(const ajainput::SyntheticFrameGenerator& gen,
                            std::size_t row,
                            std::size_t macropixelX)
{
    const std::size_t bytesPerRow = gen.Width() * 2u;  // UYVY = 2 bytes per pixel
    const uint8_t* rowPtr = gen.Data() + row * bytesPerRow;
    const uint8_t* mp = rowPtr + macropixelX * ajainput::kUYVYBytesPerMacropixel;
    return Macropixel{ mp[0], mp[1], mp[2], mp[3] };
}

// Centre macropixel of a given bar index (0..6) in a frame whose width
// is exactly divisible by 14 (7 bars × 2 pixels per macropixel). Picked
// to avoid bar-boundary aliasing inside the sample.
std::size_t BarCentreMacropixel(std::size_t width, int barIndex)
{
    const std::size_t macropixelsPerBar = width / (7u * 2u);
    return static_cast<std::size_t>(barIndex) * macropixelsPerBar
           + macropixelsPerBar / 2u;
}

}  // namespace

TEST_CASE(SyntheticFrameGenerator, ConstructsFrameOfExpectedUYVYSize)
{
    ajainput::SyntheticFrameGenerator gen(1920, 1080);
    // UYVY: 2 bytes per pixel.
    EXPECT_EQ(gen.SizeBytes(), static_cast<std::size_t>(1920 * 1080 * 2));
    EXPECT_EQ(gen.Width(),  static_cast<std::size_t>(1920));
    EXPECT_EQ(gen.Height(), static_cast<std::size_t>(1080));
    TEST_RETURN_OK;
}

TEST_CASE(SyntheticFrameGenerator, OddWidthRoundsDownToEven)
{
    // 1921 is invalid for UYVY (odd pixel count). The generator rounds
    // down silently rather than throw — production code validates
    // upstream — but the buffer must still be UYVY-compatible.
    ajainput::SyntheticFrameGenerator gen(1921, 1080);
    EXPECT_EQ(gen.Width(), static_cast<std::size_t>(1920));
    EXPECT_EQ(gen.SizeBytes(), static_cast<std::size_t>(1920 * 1080 * 2));
    TEST_RETURN_OK;
}

TEST_CASE(SyntheticFrameGenerator, BottomRegionIsPlainBlack)
{
    // 100 rows: top 75 are bars, bottom 25 are black. Sample row 90
    // (inside the black region) at multiple X positions to catch any
    // row-stride drift that would shift the bars into the bottom band.
    ajainput::SyntheticFrameGenerator gen(1400, 100);  // 14-divisible width
    gen.GenerateFrame(/*t=*/0.0f, /*input=*/0);

    for (std::size_t mx : {std::size_t{0},
                           std::size_t{100},
                           std::size_t{349},
                           std::size_t{500}})  // span the whole row width
    {
        const auto m = SampleMacropixel(gen, /*row=*/90, mx);
        EXPECT_EQ(static_cast<int>(m.u),  128);  // chroma neutral
        EXPECT_EQ(static_cast<int>(m.y0),  16);  // BT.709 limited black
        EXPECT_EQ(static_cast<int>(m.v),  128);
        EXPECT_EQ(static_cast<int>(m.y1),  16);
    }
    TEST_RETURN_OK;
}

TEST_CASE(SyntheticFrameGenerator, BarsRegionHasMonotonicallyDescendingLuma)
{
    // The SMPTE 75% bars are: white, yellow, cyan, green, magenta, red,
    // blue. Under BT.709 luma weights (R=0.2126, G=0.7152, B=0.0722),
    // dropping channels from the all-bright "white" produces a strictly
    // descending luma sequence. This is the structural test: if a
    // future change reorders bars or breaks the YCbCr conversion's
    // luma channel, this test fires immediately.
    ajainput::SyntheticFrameGenerator gen(1400, 100);
    gen.GenerateFrame(/*t=*/0.0f, /*input=*/0);

    // Sample row 10 — well inside the top 75% bars region.
    uint8_t prevY = 255;  // start strictly above white's Y
    for (int bar = 0; bar < 7; ++bar)
    {
        const std::size_t mx = BarCentreMacropixel(gen.Width(), bar);
        const auto m = SampleMacropixel(gen, /*row=*/10, mx);
        EXPECT_TRUE(m.y0 < prevY);  // strictly descending
        EXPECT_TRUE(m.y1 < prevY);
        prevY = m.y0;
    }
    TEST_RETURN_OK;
}

TEST_CASE(SyntheticFrameGenerator, WhiteBarHasNeutralChromaAndHighLuma)
{
    // White bar (index 0): R=G=B=0.75, so chroma must be ±0 (cb=cr=128),
    // and luma must be high (well above mid-grey). We don't pin the
    // exact value to avoid coupling the test to the rounding choices
    // in the BT.709 quantiser, just check the sign properties.
    ajainput::SyntheticFrameGenerator gen(1400, 100);
    gen.GenerateFrame(/*t=*/0.0f, /*input=*/0);

    const std::size_t mx = BarCentreMacropixel(gen.Width(), 0);
    const auto m = SampleMacropixel(gen, /*row=*/10, mx);

    EXPECT_EQ(static_cast<int>(m.u),  128);  // neutral cb
    EXPECT_EQ(static_cast<int>(m.v),  128);  // neutral cr
    EXPECT_TRUE(m.y0 > 128);                  // luma above mid-grey
    EXPECT_TRUE(m.y1 > 128);
    TEST_RETURN_OK;
}

TEST_CASE(SyntheticFrameGenerator, BlackBarMatchesBT709LimitedSpec)
{
    // Spec: BT.709 limited-range black is exactly Y=16, U=V=128. The
    // bottom region uses kBlack which is defined to those values.
    // Test serves as a regression guard against accidental
    // "full-range" black (Y=0) being introduced.
    ajainput::SyntheticFrameGenerator gen(1400, 100);
    gen.GenerateFrame(/*t=*/0.0f, /*input=*/0);

    // Bottom region (rows >= 75 in a 100-tall frame).
    const auto m = SampleMacropixel(gen, /*row=*/80, /*mx=*/350);
    EXPECT_EQ(static_cast<int>(m.y0),  16);
    EXPECT_EQ(static_cast<int>(m.y1),  16);
    EXPECT_EQ(static_cast<int>(m.u),  128);
    EXPECT_EQ(static_cast<int>(m.v),  128);
    TEST_RETURN_OK;
}

TEST_CASE(SyntheticFrameGenerator, MacropixelByteOrderIsUYVY)
{
    // PackUYVY documents byte order U Y0 V Y1 (little-endian). A byte-
    // swap regression (e.g. accidentally producing YUYV/YUY2) would
    // put the luma values in bytes [0] and [2] instead of [1] and [3].
    // We detect it by checking that the bytes most likely to differ
    // between adjacent macropixels are the luma bytes (positions 1
    // and 3), not the chroma bytes (positions 0 and 2). The test
    // uses two adjacent macropixels at a bar boundary — same bar so
    // chroma is identical, same luma so all four bytes match within
    // a bar. Pick two macropixels from DIFFERENT bars (yellow then
    // cyan); chroma changes radically and luma changes too. Then
    // verify the U/V bytes (positions 0,2) match what GenerateFrame
    // wrote to those positions vs Y bytes (positions 1,3).
    //
    // Concrete check: sample white (R=G=B=0.75) at one position and
    // blue (R=G=0, B=0.75) at another. For white: Y high, U=V=128.
    // For blue: Y low, U high, V low. The luma bytes (positions 1
    // and 3) should match these expectations; if byte order were
    // YUY2 the values would land in the wrong slots.
    ajainput::SyntheticFrameGenerator gen(1400, 100);
    gen.GenerateFrame(/*t=*/0.0f, /*input=*/0);

    const auto white = SampleMacropixel(gen,
                                        /*row=*/10,
                                        BarCentreMacropixel(gen.Width(), 0));
    const auto blue  = SampleMacropixel(gen,
                                        /*row=*/10,
                                        BarCentreMacropixel(gen.Width(), 6));

    // Position 1 (Y0) and position 3 (Y1) must hold luma => differ
    // sharply between white and blue (white luma >> blue luma).
    EXPECT_TRUE(white.y0 > blue.y0);
    EXPECT_TRUE(white.y1 > blue.y1);
    EXPECT_TRUE(white.y0 - blue.y0 > 50);  // huge luma gap, not noise

    // Position 0 (U) must hold chroma-blue => higher for blue than for
    // white. Position 2 (V) must hold chroma-red => lower for blue
    // than for white (blue has zero red component).
    EXPECT_TRUE(blue.u > white.u);
    EXPECT_TRUE(blue.v < white.v);
    TEST_RETURN_OK;
}
