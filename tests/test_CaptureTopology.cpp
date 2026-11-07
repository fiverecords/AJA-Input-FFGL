// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// v0.42 FrameStore plans (src/CaptureTopology.h) for the card families,
// with capabilities as the AJA SDK reports them.

#include "test_framework.h"
#include "CaptureTopology.h"
#include "InputId.h"

#include <cstring>

namespace in = ajainput::input;
using ajainput::CardCaps;
using ajainput::PlanFrameStores;
using ajainput::Topology;

namespace
{
CardCaps KonaX()
{
    CardCaps c;
    c.numFrameStores = 2; c.numSdiInputs = 2; c.numHdmiInputs = 1; c.numCSCs = 3;
    c.can12gRouting = true; c.can425Mux = false; c.can4K = true;
    return c;
}
CardCaps Kona4()
{
    CardCaps c;
    c.numFrameStores = 4; c.numSdiInputs = 4; c.numHdmiInputs = 0; c.numCSCs = 5;
    c.can12gRouting = false; c.can425Mux = true; c.can4K = true;
    return c;
}
CardCaps Corvid88()
{
    CardCaps c;
    c.numFrameStores = 8; c.numSdiInputs = 8; c.numCSCs = 8;
    c.can12gRouting = false; c.can425Mux = true; c.can4K = true;
    return c;
}
CardCaps Corvid24()
{
    CardCaps c;
    c.numFrameStores = 4; c.numSdiInputs = 4; c.numCSCs = 4;
    c.can12gRouting = false; c.can425Mux = false; c.can4K = true;
    c.multiFormat = false;
    return c;
}
CardCaps KonaHdmi()
{
    CardCaps c;
    c.numFrameStores = 4; c.numSdiInputs = 0; c.numHdmiInputs = 4; c.numCSCs = 8;
    c.can12gRouting = false; c.can425Mux = true; c.can4K = true;
    c.hdmiQuadrantMask = 0xFu; c.hdmiRgbMask = 0xFu;
    return c;
}
CardCaps Io4K()     // older HDMI widget: no quadrant / RGB outputs
{
    CardCaps c;
    c.numFrameStores = 4; c.numSdiInputs = 4; c.numHdmiInputs = 1; c.numCSCs = 5;
    c.can12gRouting = false; c.can425Mux = true; c.can4K = true;
    return c;
}
CardCaps Io4KPlus()
{
    CardCaps c = Io4K();
    c.hdmiQuadrantMask = 0x1u; c.hdmiRgbMask = 0x1u;
    return c;
}
CardCaps Kona1()
{
    CardCaps c;
    c.numFrameStores = 2; c.numSdiInputs = 1; c.numCSCs = 2;
    c.can12gRouting = false; c.can425Mux = false; c.can4K = false;
    c.multiFormat = false;
    return c;
}
} // namespace

TEST_CASE(CaptureTopology, HdAndTwelveGTakeOneFrameStore)
{
    auto p = PlanFrameStores(in::FromSdi(1), false, KonaX());
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(static_cast<int>(p.topology), static_cast<int>(Topology::Single));
    EXPECT_EQ(p.fixedMask, 0u);
    // 4K on one cable of a 12G-routing card: one FrameStore, as before.
    p = PlanFrameStores(in::FromSdi(2), true, KonaX());
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.fixedMask, 0u);
    p = PlanFrameStores(in::FromHdmi(1), true, KonaX());
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.fixedMask, 0u);
    // HD on a card without 12G routing.
    p = PlanFrameStores(in::FromSdi(3), false, Kona4());
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.fixedMask, 0u);
    TEST_RETURN_OK;
}

TEST_CASE(CaptureTopology, FourKOnOneCableNeedsTwelveGRouting)
{
    auto p = PlanFrameStores(in::FromSdi(1), true, Kona4());
    EXPECT_FALSE(p.ok);
    EXPECT_TRUE(p.reason != nullptr);
    // Quad-link on a 12G-routing card is not offered.
    p = PlanFrameStores(in::QuadFromFirstSdi(1), true, KonaX());
    EXPECT_FALSE(p.ok);
    EXPECT_FALSE(ajainput::CardOffersQuadLink(1, KonaX()));
    TEST_RETURN_OK;
}

TEST_CASE(CaptureTopology, QuadLinkPlans)
{
    // 425 muxes: 2SI into FrameStores 1+2 (muxes 1+2).
    auto p = PlanFrameStores(in::QuadFromFirstSdi(1), true, Kona4());
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(static_cast<int>(p.topology), static_cast<int>(Topology::Tsi2));
    EXPECT_EQ(p.fixedMask, 0x3u);
    EXPECT_EQ(p.firstFrameStore, 0);
    EXPECT_EQ(p.muxBase, 0);
    // Signal flag does not matter for a quad-link input.
    EXPECT_TRUE(PlanFrameStores(in::QuadFromFirstSdi(1), false, Kona4()).ok);
    // SDI 5-8 needs eight SDI inputs.
    EXPECT_FALSE(PlanFrameStores(in::QuadFromFirstSdi(5), true, Kona4()).ok);
    // Corvid 88: SDI 5-8 into FrameStores 5+6 through muxes 3+4.
    p = PlanFrameStores(in::QuadFromFirstSdi(5), true, Corvid88());
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.fixedMask, 0x30u);
    EXPECT_EQ(p.firstFrameStore, 4);
    EXPECT_EQ(p.muxBase, 2);
    // No 425 muxes: square division into the four FrameStores.
    p = PlanFrameStores(in::QuadFromFirstSdi(1), true, Corvid24());
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(static_cast<int>(p.topology), static_cast<int>(Topology::Squares4));
    EXPECT_EQ(p.fixedMask, 0xFu);
    // A card without 4K.
    EXPECT_FALSE(ajainput::CardOffersQuadLink(1, Kona1()));
    EXPECT_FALSE(PlanFrameStores(in::QuadFromFirstSdi(1), true, Kona1()).ok);
    TEST_RETURN_OK;
}

TEST_CASE(CaptureTopology, FourKHdmiThroughMuxes)
{
    // Kona HDMI: HDMI 1 -> FrameStores 1+2, HDMI 2 -> 3+4 (muxes 3+4).
    auto p = PlanFrameStores(in::FromHdmi(1), true, KonaHdmi());
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(static_cast<int>(p.topology), static_cast<int>(Topology::Tsi2));
    EXPECT_EQ(p.fixedMask, 0x3u);
    // RGB: CSCs 1, 2, 5, 6 (zero-based 0, 1, 4, 5): never CSC 3 / 4, which
    // HD RGB captures on FrameStores 3 / 4 use.
    EXPECT_EQ(ajainput::TsiCscIndex(p, 0, KonaHdmi()), 0);
    EXPECT_EQ(ajainput::TsiCscIndex(p, 1, KonaHdmi()), 1);
    EXPECT_EQ(ajainput::TsiCscIndex(p, 2, KonaHdmi()), 4);
    EXPECT_EQ(ajainput::TsiCscIndex(p, 3, KonaHdmi()), 5);
    p = PlanFrameStores(in::FromHdmi(2), true, KonaHdmi());
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.fixedMask, 0xCu);
    EXPECT_EQ(p.firstFrameStore, 2);
    EXPECT_EQ(p.muxBase, 2);
    EXPECT_EQ(ajainput::TsiCscIndex(p, 0, KonaHdmi()), 2);
    EXPECT_EQ(ajainput::TsiCscIndex(p, 3, KonaHdmi()), 7);
    // HDMI 3 and 4 are HD inputs.
    EXPECT_FALSE(PlanFrameStores(in::FromHdmi(3), true, KonaHdmi()).ok);
    // HD over HDMI: one FrameStore.
    p = PlanFrameStores(in::FromHdmi(3), false, KonaHdmi());
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.fixedMask, 0u);
    // Io 4K Plus: HDMI 1 4K on FrameStores 1+2, CSCs 1-4 (AJA's demo).
    p = PlanFrameStores(in::FromHdmi(1), true, Io4KPlus());
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(ajainput::TsiCscIndex(p, 0, Io4KPlus()), 0);
    EXPECT_EQ(ajainput::TsiCscIndex(p, 3, Io4KPlus()), 3);
    // Io 4K: its HDMI input widget has no quadrant outputs.
    EXPECT_FALSE(PlanFrameStores(in::FromHdmi(1), true, Io4K()).ok);
    EXPECT_TRUE(PlanFrameStores(in::FromHdmi(1), false, Io4K()).ok);
    TEST_RETURN_OK;
}

TEST_CASE(CaptureTopology, IpCardsTakeNoQuadLink)
{
    CardCaps ip = Kona4();
    ip.isIP = true;
    EXPECT_FALSE(ajainput::CardOffersQuadLink(1, ip));
    EXPECT_FALSE(PlanFrameStores(in::QuadFromFirstSdi(1), true, ip).ok);
    EXPECT_TRUE(PlanFrameStores(in::FromSdi(1), false, ip).ok);
    TEST_RETURN_OK;
}

TEST_CASE(CaptureTopology, FrameStoreSetNames)
{
    char b[16];
    ajainput::FormatFrameStoreSet(0x1u, b, sizeof(b));
    EXPECT_TRUE(std::strcmp(b, "FS1") == 0);
    ajainput::FormatFrameStoreSet(0x30u, b, sizeof(b));
    EXPECT_TRUE(std::strcmp(b, "FS5+6") == 0);
    ajainput::FormatFrameStoreSet(0xF0u, b, sizeof(b));
    EXPECT_TRUE(std::strcmp(b, "FS5-8") == 0);
    ajainput::FormatFrameStoreSet(0u, b, sizeof(b));
    EXPECT_TRUE(std::strcmp(b, "FS?") == 0);
    EXPECT_EQ(ajainput::FrameStoreCount(0xCu), 2);
    EXPECT_EQ(ajainput::LowestFrameStore(0xCu), 2);
    EXPECT_EQ(ajainput::LowestFrameStore(0u), -1);
    EXPECT_TRUE(std::strcmp(ajainput::ToString(Topology::Tsi2), "2SI") == 0);
    TEST_RETURN_OK;
}
