// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Regression tests for ajainput::YadifSlots, the pure state machine that
// drives the three-slot rotation of YADIF history textures. The actual
// GL blits live in AJAInputPlugin::BlitRingFrameIntoHistory; only the
// integer bookkeeping is exercised here.
//
// Covered:
// - Initial state has the canonical {0, 1, 2} permutation, framesBuffered
//   = 0, lastBlittedSequence at its ~0ull sentinel.
// - Advance() rotates the indices consistently: the slot returned as
//   destinationSlot was the old prevSlot, and afterward is the new
//   nextSlot. prev/cur/next are always a permutation of {0, 1, 2}.
// - After three advances the slot occupied by sequence N is the
//   "next" slot at advance N; one earlier is "cur"; two earlier is
//   "prev". This is the invariant the render path samples against.
// - framesBuffered saturates at 3 (it gates yadif activation; the
//   render path samples three textures, so the count must not exceed
//   the array length).
// - Duplicate sequences are absorbed without rotating (idempotency
//   guard against the same ring frame being read twice).
// - Reset() restores the initial state regardless of prior history.

#include "test_framework.h"
#include "YadifSlots.h"

#include <cstdint>

namespace
{
// Helper: check that prevSlot, curSlot, nextSlot form a permutation
// of {0, 1, 2}. Any violation of this would mean two role textures
// alias the same physical slot, which the render path would surface
// as a corrupted "next" or "prev" sample.
bool IsValidPermutation(const ajainput::YadifSlots& s)
{
    const int a = s.prevSlot;
    const int b = s.curSlot;
    const int c = s.nextSlot;
    if (a < 0 || a > 2) return false;
    if (b < 0 || b > 2) return false;
    if (c < 0 || c > 2) return false;
    if (a == b || a == c || b == c) return false;
    return true;
}
}  // namespace

TEST_CASE(YadifSlots, InitialState)
{
    ajainput::YadifSlots s;
    EXPECT_EQ(s.prevSlot,       0);
    EXPECT_EQ(s.curSlot,        1);
    EXPECT_EQ(s.nextSlot,       2);
    EXPECT_EQ(s.framesBuffered, 0);
    EXPECT_EQ(s.lastBlittedSequence, ~uint64_t(0));
    EXPECT_TRUE(IsValidPermutation(s));
    TEST_RETURN_OK;
}

TEST_CASE(YadifSlots, FirstAdvanceWritesIntoOldPrev)
{
    ajainput::YadifSlots s;
    const auto r = s.Advance(100);

    EXPECT_TRUE(r.didAdvance);
    EXPECT_EQ(r.destinationSlot, 0);  // old prevSlot
    EXPECT_EQ(s.framesBuffered,  1);
    EXPECT_EQ(s.lastBlittedSequence, uint64_t(100));

    // After rotation the destination slot is the new "next".
    EXPECT_EQ(s.nextSlot, 0);
    EXPECT_EQ(s.curSlot,  2);
    EXPECT_EQ(s.prevSlot, 1);
    EXPECT_TRUE(IsValidPermutation(s));
    TEST_RETURN_OK;
}

TEST_CASE(YadifSlots, ThreeAdvancesProduceChronologicalOrdering)
{
    ajainput::YadifSlots s;

    // Simulate the contents of each slot externally so we can assert
    // that the indices in s correctly identify which sequence ended
    // up in which role.
    uint64_t slotContents[3] = {0, 0, 0};

    const auto r1 = s.Advance(100);
    ASSERT_TRUE(r1.didAdvance);
    slotContents[r1.destinationSlot] = 100;
    EXPECT_EQ(slotContents[s.nextSlot], uint64_t(100));

    const auto r2 = s.Advance(200);
    ASSERT_TRUE(r2.didAdvance);
    slotContents[r2.destinationSlot] = 200;
    EXPECT_EQ(slotContents[s.nextSlot], uint64_t(200));
    EXPECT_EQ(slotContents[s.curSlot],  uint64_t(100));

    const auto r3 = s.Advance(300);
    ASSERT_TRUE(r3.didAdvance);
    slotContents[r3.destinationSlot] = 300;
    EXPECT_EQ(slotContents[s.nextSlot], uint64_t(300));
    EXPECT_EQ(slotContents[s.curSlot],  uint64_t(200));
    EXPECT_EQ(slotContents[s.prevSlot], uint64_t(100));

    EXPECT_EQ(s.framesBuffered, 3);
    EXPECT_TRUE(IsValidPermutation(s));
    TEST_RETURN_OK;
}

TEST_CASE(YadifSlots, FramesBufferedSaturatesAtThree)
{
    ajainput::YadifSlots s;
    for (uint64_t i = 1; i <= 10; ++i)
    {
        const auto r = s.Advance(i);
        ASSERT_TRUE(r.didAdvance);
    }
    EXPECT_EQ(s.framesBuffered, 3);
    EXPECT_TRUE(IsValidPermutation(s));
    TEST_RETURN_OK;
}

TEST_CASE(YadifSlots, DuplicateSequenceIsNoOp)
{
    ajainput::YadifSlots s;
    const auto first  = s.Advance(42);
    ASSERT_TRUE(first.didAdvance);

    // Snapshot state, then call Advance with the same sequence.
    const int prevA = s.prevSlot;
    const int curA  = s.curSlot;
    const int nextA = s.nextSlot;
    const int bufA  = s.framesBuffered;

    const auto second = s.Advance(42);
    EXPECT_FALSE(second.didAdvance);
    EXPECT_EQ(second.destinationSlot, -1);
    EXPECT_EQ(s.prevSlot,       prevA);
    EXPECT_EQ(s.curSlot,        curA);
    EXPECT_EQ(s.nextSlot,       nextA);
    EXPECT_EQ(s.framesBuffered, bufA);
    TEST_RETURN_OK;
}

TEST_CASE(YadifSlots, AdvanceAfterDuplicateStillWorks)
{
    ajainput::YadifSlots s;
    (void)s.Advance(7);
    (void)s.Advance(7);   // dup, no-op
    const auto r = s.Advance(8);
    EXPECT_TRUE(r.didAdvance);
    EXPECT_EQ(s.framesBuffered, 2);
    EXPECT_EQ(s.lastBlittedSequence, uint64_t(8));
    EXPECT_TRUE(IsValidPermutation(s));
    TEST_RETURN_OK;
}

TEST_CASE(YadifSlots, ResetRestoresInitialState)
{
    ajainput::YadifSlots s;
    (void)s.Advance(1);
    (void)s.Advance(2);
    (void)s.Advance(3);
    (void)s.Advance(4);

    s.Reset();
    EXPECT_EQ(s.prevSlot,       0);
    EXPECT_EQ(s.curSlot,        1);
    EXPECT_EQ(s.nextSlot,       2);
    EXPECT_EQ(s.framesBuffered, 0);
    EXPECT_EQ(s.lastBlittedSequence, ~uint64_t(0));
    TEST_RETURN_OK;
}

TEST_CASE(YadifSlots, PermutationInvariantHoldsAcrossManyAdvances)
{
    ajainput::YadifSlots s;
    for (uint64_t i = 1; i <= 1000; ++i)
    {
        const auto r = s.Advance(i);
        ASSERT_TRUE(r.didAdvance);
        ASSERT_TRUE(IsValidPermutation(s));
        // destinationSlot must always be a valid slot index.
        ASSERT_TRUE(r.destinationSlot >= 0 && r.destinationSlot <= 2);
        // After advance, destinationSlot must be the new nextSlot
        // (the freshly-written frame plays the "next" role).
        ASSERT_TRUE(s.nextSlot == r.destinationSlot);
    }
    TEST_RETURN_OK;
}

TEST_CASE(YadifSlots, ResetClearsLastBlittedSequence)
{
    // Regression guard against a subtle bug: if Reset forgot to wipe
    // lastBlittedSequence, the first Advance after a format change
    // could no-op spuriously when the new capture stream happened to
    // hand out the same sequence number as the last absorbed frame
    // of the old stream.
    ajainput::YadifSlots s;
    (void)s.Advance(99);
    s.Reset();
    const auto r = s.Advance(99);  // same sequence the old stream had
    EXPECT_TRUE(r.didAdvance);
    EXPECT_EQ(s.framesBuffered, 1);
    TEST_RETURN_OK;
}

TEST_CASE(YadifSlots, OlderSequenceIsRejected)
{
    // An older publication arriving after a newer one (for example a
    // producer/consumer handoff glitch) must not rotate: it would put
    // an older frame in "next" and break the prev < cur < next order.
    ajainput::YadifSlots s;
    (void)s.Advance(10);
    (void)s.Advance(11);
    const int prevB = s.prevSlot;
    const int curB  = s.curSlot;
    const int nextB = s.nextSlot;

    const auto r = s.Advance(9);
    EXPECT_FALSE(r.didAdvance);
    EXPECT_EQ(r.destinationSlot, -1);
    EXPECT_EQ(s.prevSlot, prevB);
    EXPECT_EQ(s.curSlot,  curB);
    EXPECT_EQ(s.nextSlot, nextB);
    EXPECT_EQ(s.framesBuffered, 2);
    EXPECT_EQ(s.lastBlittedSequence, uint64_t(11));

    // A newer sequence after the rejected one still rotates normally.
    const auto r2 = s.Advance(12);
    EXPECT_TRUE(r2.didAdvance);
    EXPECT_EQ(s.framesBuffered, 3);
    EXPECT_TRUE(IsValidPermutation(s));
    TEST_RETURN_OK;
}

TEST_CASE(YadifSlots, ResetAllowsSequenceToRestartLower)
{
    // Ring re-initialisation restarts publication sequences at 1, and
    // is always paired with Reset(). After Reset a low sequence must be
    // accepted even if the previous stream had reached a higher one.
    ajainput::YadifSlots s;
    (void)s.Advance(500);
    s.Reset();
    const auto r = s.Advance(1);
    EXPECT_TRUE(r.didAdvance);
    EXPECT_EQ(s.framesBuffered, 1);
    TEST_RETURN_OK;
}
