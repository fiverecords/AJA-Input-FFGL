// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Tests for ajainput::MultiFormatRegistry and AreMultiFormatCompatible: the
// frame-rate family rule AJA multi-format cards follow, and the per-card
// conflict lookup each plugin instance uses to warn the operator.

#include "test_framework.h"
#include "MultiFormatRegistry.h"

#include "ntv2enums.h"

namespace
{
int F(NTV2VideoFormat f) { return static_cast<int>(f); }
}

TEST_CASE(MultiFormat, SameFamilyIsCompatibleWhateverTheRaster)
{
    // 29.97 / 59.94 family: interlaced 1080, 720p and 1080p mix freely.
    EXPECT_TRUE(ajainput::AreMultiFormatCompatible(F(NTV2_FORMAT_1080i_5994),
                                                F(NTV2_FORMAT_720p_5994)));
    EXPECT_TRUE(ajainput::AreMultiFormatCompatible(F(NTV2_FORMAT_1080i_5994),
                                                F(NTV2_FORMAT_1080p_5994_A)));
    EXPECT_TRUE(ajainput::AreMultiFormatCompatible(F(NTV2_FORMAT_525_5994),
                                                F(NTV2_FORMAT_1080p_2997)));
    // 25 / 50 family.
    EXPECT_TRUE(ajainput::AreMultiFormatCompatible(F(NTV2_FORMAT_1080i_5000),
                                                F(NTV2_FORMAT_720p_5000)));
    EXPECT_TRUE(ajainput::AreMultiFormatCompatible(F(NTV2_FORMAT_625_5000),
                                                F(NTV2_FORMAT_1080p_2500)));
    // 30 / 60 family, and 4K with HD.
    EXPECT_TRUE(ajainput::AreMultiFormatCompatible(F(NTV2_FORMAT_1080p_6000_A),
                                                F(NTV2_FORMAT_3840x2160p_3000)));
    TEST_RETURN_OK;
}

TEST_CASE(MultiFormat, DifferentFamiliesAreIncompatible)
{
    EXPECT_FALSE(ajainput::AreMultiFormatCompatible(F(NTV2_FORMAT_1080i_5000),
                                                 F(NTV2_FORMAT_1080p_5994_A)));
    EXPECT_FALSE(ajainput::AreMultiFormatCompatible(F(NTV2_FORMAT_720p_5994),
                                                 F(NTV2_FORMAT_720p_6000)));
    // 23.98 is its own family, not part of 29.97/59.94.
    EXPECT_FALSE(ajainput::AreMultiFormatCompatible(F(NTV2_FORMAT_1080p_2398),
                                                 F(NTV2_FORMAT_1080p_5994_A)));
    EXPECT_FALSE(ajainput::AreMultiFormatCompatible(F(NTV2_FORMAT_1080p_2400),
                                                 F(NTV2_FORMAT_1080p_2500)));
    // Unknown never counts as compatible.
    EXPECT_FALSE(ajainput::AreMultiFormatCompatible(F(NTV2_FORMAT_UNKNOWN),
                                                 F(NTV2_FORMAT_1080p_2500)));
    TEST_RETURN_OK;
}

TEST_CASE(MultiFormat, RegistryReportsConflictToBothInstances)
{
    using R = ajainput::MultiFormatRegistry;
    R::Clear();
    R::Publish(0, 0, 1, F(NTV2_FORMAT_1080i_5000));
    EXPECT_FALSE(R::FindConflict(0, 0).found);   // alone on the card

    R::Publish(0, 1, 2, F(NTV2_FORMAT_1080p_5994_A));
    const auto a = R::FindConflict(0, 0);
    const auto b = R::FindConflict(0, 1);
    EXPECT_TRUE(a.found);
    EXPECT_EQ(a.otherFrameStore, 1);
    EXPECT_EQ(a.otherInputNumber, 2);
    EXPECT_EQ(a.otherFormatRaw, F(NTV2_FORMAT_1080p_5994_A));
    EXPECT_TRUE(b.found);
    EXPECT_EQ(b.otherFrameStore, 0);
    EXPECT_EQ(b.otherInputNumber, 1);

    // The second input switches to a compatible rate: conflict gone.
    R::Publish(0, 1, 2, F(NTV2_FORMAT_720p_5000));
    EXPECT_FALSE(R::FindConflict(0, 0).found);
    EXPECT_FALSE(R::FindConflict(0, 1).found);
    R::Clear();
    TEST_RETURN_OK;
}

TEST_CASE(MultiFormat, RegistryIgnoresOtherCardsAndWithdrawn)
{
    using R = ajainput::MultiFormatRegistry;
    R::Clear();
    R::Publish(0, 0, 1, F(NTV2_FORMAT_1080i_5000));
    R::Publish(1, 1, 1, F(NTV2_FORMAT_1080p_5994_A));  // another card
    EXPECT_FALSE(R::FindConflict(0, 0).found);

    R::Publish(0, 1, 2, F(NTV2_FORMAT_1080p_5994_A));
    EXPECT_TRUE(R::FindConflict(0, 0).found);
    R::Withdraw(0, 1);                                   // no signal / closed
    EXPECT_FALSE(R::FindConflict(0, 0).found);

    // Nothing published for this FrameStore: no conflict to report.
    EXPECT_FALSE(R::FindConflict(0, 3).found);
    R::Clear();
    TEST_RETURN_OK;
}

// v0.41: FrameStore requests expire by time and are per card.
TEST_CASE(MultiFormat, FrameStoreWantsArePerCardAndTimed)
{
    using R = ajainput::MultiFormatRegistry;
    R::Clear();
    EXPECT_FALSE(R::FrameStoreWantedSince(0, 0));
    R::WantFrameStore(0, 2000);
    EXPECT_TRUE(R::FrameStoreWantedSince(0, 1800));
    EXPECT_FALSE(R::FrameStoreWantedSince(0, 2001));  // too old
    EXPECT_FALSE(R::FrameStoreWantedSince(1, 0));     // other card
    R::Clear();
    EXPECT_FALSE(R::FrameStoreWantedSince(0, 0));
    TEST_RETURN_OK;
}

// v0.41: one FrameStore request makes one capture yield.
TEST_CASE(MultiFormat, FrameStoreWantIsConsumedOnce)
{
    using R = ajainput::MultiFormatRegistry;
    R::Clear();
    R::WantFrameStore(0, 1000);
    EXPECT_FALSE(R::ConsumeFrameStoreWant(0, 1001));  // too old: kept
    EXPECT_TRUE(R::ConsumeFrameStoreWant(0, 900));
    EXPECT_FALSE(R::ConsumeFrameStoreWant(0, 900));   // already taken
    EXPECT_FALSE(R::FrameStoreWantedSince(0, 0));
    R::Clear();
    TEST_RETURN_OK;
}

// v0.42: a card without multi-format runs one format; the second input
// with another format waits instead of reprogramming the card.
TEST_CASE(MultiFormatRegistry, OneFormatCardsKeepTheFirstFormat)
{
    using ajainput::MultiFormatRegistry;
    MultiFormatRegistry::Clear();
    const int f1080i50 = static_cast<int>(NTV2_FORMAT_1080i_5000);
    const int f720p50  = static_cast<int>(NTV2_FORMAT_720p_5000);

    MultiFormatRegistry::Publish(3, 0, 1, f1080i50);
    EXPECT_TRUE(MultiFormatRegistry::ClaimCardFormat(3, 0, 1, f1080i50));
    MultiFormatRegistry::Publish(3, 1, 2, f720p50);
    MultiFormatRegistry::Conflict other;
    EXPECT_FALSE(MultiFormatRegistry::ClaimCardFormat(3, 1, 2, f720p50, &other));
    EXPECT_TRUE(other.found);
    EXPECT_EQ(other.otherFrameStore, 0);
    EXPECT_EQ(other.otherInputNumber, 1);
    EXPECT_EQ(other.otherFormatRaw, f1080i50);
    // Same format: both run.
    EXPECT_TRUE(MultiFormatRegistry::ClaimCardFormat(3, 1, 2, f1080i50));
    // Any difference is a clash on such a card, even one multi-format
    // cards accept (same rate family).
    MultiFormatRegistry::Publish(3, 1, 2, f720p50);
    EXPECT_TRUE(MultiFormatRegistry::FindConflict(3, 1, /*singleFormatCard*/true).found);
    EXPECT_FALSE(MultiFormatRegistry::FindConflict(3, 1, false).found);
    // The first input stops: the card's format is free.
    MultiFormatRegistry::ReleaseCardFormat(3, 0);
    EXPECT_TRUE(MultiFormatRegistry::ClaimCardFormat(3, 1, 2, f720p50));
    // A new format published by a running capture drops its claim.
    MultiFormatRegistry::Publish(3, 1, 2, f1080i50);
    EXPECT_TRUE(MultiFormatRegistry::ClaimCardFormat(3, 0, 1, f720p50));
    MultiFormatRegistry::Clear();
    TEST_RETURN_OK;
}

// v0.42: a request for a FrameStore set is only consumed by a capture
// holding one of them.
TEST_CASE(MultiFormatRegistry, FrameStoreWantsNameTheFrameStores)
{
    using ajainput::MultiFormatRegistry;
    MultiFormatRegistry::Clear();
    MultiFormatRegistry::WantFrameStore(4, 1000, 0x3u);
    EXPECT_FALSE(MultiFormatRegistry::ConsumeFrameStoreWant(4, 900, 0x4u));
    EXPECT_TRUE(MultiFormatRegistry::ConsumeFrameStoreWant(4, 900, 0x2u));
    EXPECT_FALSE(MultiFormatRegistry::ConsumeFrameStoreWant(4, 900, 0x2u));
    // Requests close together add up.
    MultiFormatRegistry::WantFrameStore(4, 2000, 0x1u);
    MultiFormatRegistry::WantFrameStore(4, 2100, 0x8u);
    EXPECT_TRUE(MultiFormatRegistry::ConsumeFrameStoreWant(4, 2000, 0x1u));
    // ... far apart they do not.
    MultiFormatRegistry::WantFrameStore(4, 3000, 0x1u);
    MultiFormatRegistry::WantFrameStore(4, 4000, 0x8u);
    EXPECT_FALSE(MultiFormatRegistry::ConsumeFrameStoreWant(4, 3000, 0x1u));
    EXPECT_TRUE(MultiFormatRegistry::ConsumeFrameStoreWant(4, 3000, 0x8u));
    // Default: any FrameStore helps.
    MultiFormatRegistry::WantFrameStore(4, 5000);
    EXPECT_TRUE(MultiFormatRegistry::ConsumeFrameStoreWant(4, 5000, 0x10u));
    MultiFormatRegistry::Clear();
    TEST_RETURN_OK;
}
