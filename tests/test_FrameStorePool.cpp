// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Regression tests for FrameStorePool. The pool is a process-global
// allocator of FrameStore slot indices, so the multi-instance plugin
// contract (each AJAInputPlugin claims one FrameStore at InitGL and
// releases at DeInitGL) hinges on it returning unique indices and
// not leaking claims.
//
// Covered:
// - Claim returns the lowest free index.
// - Concurrent claims return distinct indices.
// - Release frees the slot for re-use.
// - Releasing an unclaimed index is harmless.
// - Claim respects the maxFrameStores ceiling.

#include "test_framework.h"
#include "FrameStorePool.h"
#include "InputId.h"

namespace
{

// Each test should start with a clean pool. Since the pool is a
// process-global, we explicitly release every index we claim within a
// test before returning; failing to do so would leak state into the
// next test in the run sequence. A reset() helper would be cleaner,
// but introducing one only for tests would also be a way to corrupt
// the pool in production code, so we keep the discipline manual.
void ReleaseRange(int from, int to)
{
    for (int i = from; i <= to; ++i)
        ajainput::FrameStorePool::Release(0, i);
}

}  // namespace

TEST_CASE(FrameStorePool, ClaimReturnsLowestFreeIndex)
{
    const int a = ajainput::FrameStorePool::Claim(0, 4);
    EXPECT_EQ(a, 0);

    const int b = ajainput::FrameStorePool::Claim(0, 4);
    EXPECT_EQ(b, 1);

    const int c = ajainput::FrameStorePool::Claim(0, 4);
    EXPECT_EQ(c, 2);

    ReleaseRange(0, 7);  // cleanup
    TEST_RETURN_OK;
}

TEST_CASE(FrameStorePool, ReleaseFreesSlot)
{
    const int a = ajainput::FrameStorePool::Claim(0, 4);
    const int b = ajainput::FrameStorePool::Claim(0, 4);
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);

    ajainput::FrameStorePool::Release(0, a);

    // Next claim should reuse slot 0, not jump to 2.
    const int c = ajainput::FrameStorePool::Claim(0, 4);
    EXPECT_EQ(c, 0);

    ReleaseRange(0, 7);  // cleanup
    TEST_RETURN_OK;
}

TEST_CASE(FrameStorePool, ClaimRespectsMaxCeiling)
{
    // Card with only 2 FrameStores: the third claim must fail
    // even though slots 2..31 are free in the bitmask.
    const int a = ajainput::FrameStorePool::Claim(0, 2);
    const int b = ajainput::FrameStorePool::Claim(0, 2);
    const int c = ajainput::FrameStorePool::Claim(0, 2);

    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);
    EXPECT_EQ(c, -1);

    ReleaseRange(0, 7);  // cleanup
    TEST_RETURN_OK;
}

TEST_CASE(FrameStorePool, ReleaseUnclaimedIsNoOp)
{
    // No claims outstanding. Releasing arbitrary indices must not
    // corrupt the mask, so a subsequent Claim still returns 0.
    ajainput::FrameStorePool::Release(0, 0);
    ajainput::FrameStorePool::Release(0, 5);
    ajainput::FrameStorePool::Release(0, -1);  // out of range
    ajainput::FrameStorePool::Release(0, 100); // out of range

    const int a = ajainput::FrameStorePool::Claim(0, 4);
    EXPECT_EQ(a, 0);

    ReleaseRange(0, 7);  // cleanup
    TEST_RETURN_OK;
}

// v0.40: FrameStores another application uses are skipped.
TEST_CASE(FrameStorePool, ClaimSkipsAvoidedFrameStores)
{
    using ajainput::FrameStorePool;
    const int a = FrameStorePool::Claim(0, 4, 0x1u);   // avoid FS0
    EXPECT_EQ(a, 1);
    const int b = FrameStorePool::Claim(0, 4, 0x1u | 0x4u);  // avoid FS0, FS2
    EXPECT_EQ(b, 3);
    const int c = FrameStorePool::Claim(0, 4, 0xFu);   // everything avoided
    EXPECT_EQ(c, -1);
    FrameStorePool::Release(0, a);
    FrameStorePool::Release(0, b);
    TEST_RETURN_OK;
}

// v0.41: one specific FrameStore; a second claim of it fails.
TEST_CASE(FrameStorePool, TryClaimTakesOnlyThatFrameStore)
{
    using ajainput::FrameStorePool;
    EXPECT_TRUE(FrameStorePool::TryClaim(0, 1));
    EXPECT_FALSE(FrameStorePool::TryClaim(0, 1));
    EXPECT_EQ(FrameStorePool::Claim(0, 4), 0);      // lowest free is still 0
    EXPECT_FALSE(FrameStorePool::TryClaim(0, -1));
    EXPECT_FALSE(FrameStorePool::TryClaim(0, 32));
    ReleaseRange(0, 7);
    EXPECT_TRUE(FrameStorePool::TryClaim(0, 1));
    ReleaseRange(0, 7);
    TEST_RETURN_OK;
}

// v0.41: SDI N pairs with FrameStore N (the card's default wiring).
TEST_CASE(FrameStorePool, PreferenceOrderPairsSdiWithItsFrameStore)
{
    using ajainput::FrameStorePool;
    int o[8] = {};
    EXPECT_EQ(FrameStorePool::NaturalFrameStore(1, 2), 0);
    EXPECT_EQ(FrameStorePool::NaturalFrameStore(2, 2), 1);
    EXPECT_EQ(FrameStorePool::NaturalFrameStore(3, 2), -1);   // no FS3 on a 2-FS card
    EXPECT_EQ(FrameStorePool::NaturalFrameStore(ajainput::input::FromHdmi(1), 4), -1);   // HDMI: no fixed pair

    EXPECT_EQ(FrameStorePool::PreferenceOrder(2, 2, o, 8), 2);
    EXPECT_EQ(o[0], 1);
    EXPECT_EQ(o[1], 0);

    EXPECT_EQ(FrameStorePool::PreferenceOrder(3, 4, o, 8), 4);
    EXPECT_EQ(o[0], 2);
    EXPECT_EQ(o[1], 0);
    EXPECT_EQ(o[2], 1);
    EXPECT_EQ(o[3], 3);

    // HDMI takes the highest first, leaving FS1/FS2 to SDI1/SDI2.
    EXPECT_EQ(FrameStorePool::PreferenceOrder(ajainput::input::FromHdmi(1), 2, o, 8), 2);
    EXPECT_EQ(o[0], 1);
    EXPECT_EQ(o[1], 0);

    // SDI3 on a 2-FS card: plain lowest-first.
    EXPECT_EQ(FrameStorePool::PreferenceOrder(3, 2, o, 8), 2);
    EXPECT_EQ(o[0], 0);
    EXPECT_EQ(o[1], 1);

    EXPECT_EQ(FrameStorePool::PreferenceOrder(1, 4, o, 2), 2);  // capacity
    EXPECT_EQ(FrameStorePool::PreferenceOrder(1, 0, o, 8), 0);
    TEST_RETURN_OK;
}

// v0.41: each card has its own FrameStores.
TEST_CASE(FrameStorePool, CardsHaveSeparateFrameStores)
{
    using ajainput::FrameStorePool;
    EXPECT_TRUE(FrameStorePool::TryClaim(0, 0));
    EXPECT_TRUE(FrameStorePool::TryClaim(1, 0));      // card 2's FS1 is free
    EXPECT_FALSE(FrameStorePool::TryClaim(1, 0));
    EXPECT_EQ(FrameStorePool::ClaimedMask(0), 1u);
    EXPECT_EQ(FrameStorePool::ClaimedMask(1), 1u);
    FrameStorePool::Release(1, 0);
    EXPECT_EQ(FrameStorePool::ClaimedMask(1), 0u);
    EXPECT_EQ(FrameStorePool::ClaimedMask(0), 1u);    // card 1 unaffected
    FrameStorePool::Release(0, 0);
    TEST_RETURN_OK;
}

// v0.42: 4K captures claim a FrameStore pair or four, all or none.
TEST_CASE(FrameStorePool, MaskClaimsAreAllOrNothing)
{
    using ajainput::FrameStorePool;
    EXPECT_TRUE(FrameStorePool::TryClaim(2, 1));           // FS2 taken
    EXPECT_FALSE(FrameStorePool::TryClaimMask(2, 0x3u));   // pair 1+2 needs FS2
    EXPECT_EQ(FrameStorePool::ClaimedMask(2), 0x2u);       // FS1 not left claimed
    FrameStorePool::Release(2, 1);
    EXPECT_TRUE(FrameStorePool::TryClaimMask(2, 0x3u));
    EXPECT_FALSE(FrameStorePool::TryClaim(2, 0));
    EXPECT_FALSE(FrameStorePool::TryClaimMask(2, 0xFu));   // squares overlaps
    EXPECT_TRUE(FrameStorePool::TryClaimMask(2, 0xCu));    // pair 3+4 is free
    EXPECT_EQ(FrameStorePool::ClaimedMask(2), 0xFu);
    FrameStorePool::ReleaseMask(2, 0x3u);
    EXPECT_EQ(FrameStorePool::ClaimedMask(2), 0xCu);
    FrameStorePool::ReleaseMask(2, 0xCu);
    EXPECT_EQ(FrameStorePool::ClaimedMask(2), 0u);
    EXPECT_FALSE(FrameStorePool::TryClaimMask(2, 0u));     // empty set
    TEST_RETURN_OK;
}

// v0.42: SDI 5..8 pair with FrameStores 5..8; HDMI N with FrameStore N on
// a card without SDI inputs (Kona HDMI).
TEST_CASE(FrameStorePool, PairingCoversEveryInput)
{
    using ajainput::FrameStorePool;
    namespace in = ajainput::input;
    int o[8] = {};
    EXPECT_EQ(FrameStorePool::NaturalFrameStore(in::FromSdi(5), 8), 4);
    EXPECT_EQ(FrameStorePool::NaturalFrameStore(in::FromSdi(8), 8), 7);
    EXPECT_EQ(FrameStorePool::NaturalFrameStore(in::FromSdi(5), 4), -1);
    EXPECT_EQ(FrameStorePool::NaturalFrameStore(in::FromHdmi(2), 4), -1);
    EXPECT_EQ(FrameStorePool::NaturalFrameStore(in::FromHdmi(2), 4, false), 1);
    EXPECT_EQ(FrameStorePool::NaturalFrameStore(in::FromHdmi(4), 4, false), 3);
    EXPECT_EQ(FrameStorePool::NaturalFrameStore(in::QuadFromFirstSdi(1), 4), -1);

    // HDMI 3 on a Kona HDMI: its own FrameStore first.
    EXPECT_EQ(FrameStorePool::PreferenceOrder(in::FromHdmi(3), 4, o, 8, false), 4);
    EXPECT_EQ(o[0], 2);
    EXPECT_EQ(o[1], 0);
    // HDMI 2 on a card with SDI inputs: highest first, like HDMI 1.
    EXPECT_EQ(FrameStorePool::PreferenceOrder(in::FromHdmi(2), 4, o, 8), 4);
    EXPECT_EQ(o[0], 3);
    EXPECT_EQ(o[3], 0);
    // KONA XM: 4 FrameStores, CSCs 1-3 only; HDMI tries FrameStores with a
    // CSC first (an RGB source needs one), still highest first.
    EXPECT_EQ(FrameStorePool::PreferenceOrder(in::FromHdmi(1), 4, o, 8, true, 3), 4);
    EXPECT_EQ(o[0], 2);
    EXPECT_EQ(o[1], 1);
    EXPECT_EQ(o[2], 0);
    EXPECT_EQ(o[3], 3);
    // KONA X (3 CSCs, 2 FrameStores): unchanged.
    EXPECT_EQ(FrameStorePool::PreferenceOrder(in::FromHdmi(1), 2, o, 8, true, 3), 2);
    EXPECT_EQ(o[0], 1);
    EXPECT_EQ(o[1], 0);
    // SDI is not affected.
    EXPECT_EQ(FrameStorePool::PreferenceOrder(in::FromSdi(4), 4, o, 8, true, 3), 4);
    EXPECT_EQ(o[0], 3);
    TEST_RETURN_OK;
}
