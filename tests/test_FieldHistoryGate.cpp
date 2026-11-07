// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Tests for ajainput::FieldHistoryGate, the rotation / re-blit / repair
// decision used to feed the YADIF history from the texture ring.
//
// Covered:
// - The individual decisions (full frame, first field, second field,
//   repair only after a missed second field of the immediately previous
//   frame, repair parity, reset, capture-session restart).
// - A CPU model of the whole field-mode pipeline: capture accumulator,
//   latest-wins ring, three history slots. With the renderer missing
//   random publishes (never two in a row), the slot the renderer uses as
//   "cur" must always hold both fields of ONE source frame. The same
//   run with the repair disabled (the pre-v0.39 behaviour) must fail
//   that check, so the model really exercises the bug.

#include "test_framework.h"
#include "FieldHistoryGate.h"
#include "YadifSlots.h"

#include <array>
#include <cstdint>

namespace
{
using Gate = ajainput::FieldHistoryGate;
using PK   = Gate::PublishKind;

// One ring publish in the model: which source frame each row parity of
// the accumulator came from, plus the metadata the renderer sees.
struct Publish
{
    std::uint64_t evenRowsFrame = 0;   // top field for TFF
    std::uint64_t oddRowsFrame  = 0;   // bottom field for TFF
    PK            kind          = PK::FirstField;
    int           freshParity   = 0;
    std::uint64_t sourceSeq     = 0;
};

struct Slot
{
    std::uint64_t evenRowsFrame = 0;
    std::uint64_t oddRowsFrame  = 0;
};

// Deterministic LCG so the test is reproducible on every platform.
struct Lcg
{
    std::uint32_t state;
    explicit Lcg(std::uint32_t seed) : state(seed) {}
    std::uint32_t Next() { state = state * 1664525u + 1013904223u; return state >> 8; }
};

// Runs `fields` field publishes of a TFF source through the model and
// returns how many renders saw an inconsistent "cur" slot.
int RunPipelineModel(bool repairEnabled, int missPercent, int fields,
                     std::uint32_t seed, std::uint64_t* repairsOut)
{
    Gate                gate;
    ajainput::YadifSlots   slots;
    std::array<Slot, 3> history{};
    std::uint64_t       publishCounter = 0;

    // Capture side.
    std::uint64_t accEven = 0, accOdd = 0;
    std::uint64_t captureSeq = 0;

    Lcg  rng(seed);
    bool missedPrevious = false;
    int  inconsistent   = 0;

    Publish pending{};
    bool    havePending = false;

    for (int i = 0; i < fields; ++i)
    {
        // Capture thread: deliver the next field (strict F0/F1
        // alternation, TFF: F0 = even rows, F1 = odd rows).
        const bool isF0 = (i % 2) == 0;
        if (isF0)
        {
            ++captureSeq;
            accEven = captureSeq;
        }
        else
        {
            accOdd = captureSeq;
        }
        pending.evenRowsFrame = accEven;
        pending.oddRowsFrame  = accOdd;
        pending.kind          = isF0 ? PK::FirstField : PK::SecondField;
        pending.freshParity   = isF0 ? 0 : 1;
        pending.sourceSeq     = captureSeq;
        havePending           = true;   // latest-wins: overwrites any unread one

        // Renderer: acquires after this publish unless it "misses" it.
        // Never miss two publishes in a row.
        const bool miss = !missedPrevious &&
                          (static_cast<int>(rng.Next() % 100u) < missPercent);
        missedPrevious = miss;
        if (miss || !havePending)
            continue;

        const Publish p = pending;
        havePending = false;

        const auto d = gate.OnFreshPublish(p.kind, p.freshParity, p.sourceSeq,
                                           slots.framesBuffered, repairEnabled);
        if (d.repairOutgoing)
        {
            Slot& s = history[slots.nextSlot];
            if (d.repairRowParity == 0) s.evenRowsFrame = p.evenRowsFrame;
            else                        s.oddRowsFrame  = p.oddRowsFrame;
        }
        if (d.rotate)
        {
            const auto adv = slots.Advance(++publishCounter);
            history[adv.destinationSlot] = { p.evenRowsFrame, p.oddRowsFrame };
        }
        else
        {
            history[slots.nextSlot] = { p.evenRowsFrame, p.oddRowsFrame };
        }

        if (slots.framesBuffered >= 3)
        {
            const Slot& cur = history[slots.curSlot];
            if (cur.evenRowsFrame != cur.oddRowsFrame)
                ++inconsistent;
        }
    }

    if (repairsOut)
        *repairsOut = gate.repairs;
    return inconsistent;
}
}  // namespace

TEST_CASE(FieldHistoryGate, FullFrameAlwaysRotatesAndIsComplete)
{
    Gate g;
    auto d = g.OnFreshPublish(PK::FullFrame, 0, 1, 0);
    EXPECT_TRUE(d.rotate);
    EXPECT_FALSE(d.repairOutgoing);
    EXPECT_TRUE(g.nextSlotComplete);
    // Even the same sequence rotates in frame mode (one publish per frame).
    d = g.OnFreshPublish(PK::FullFrame, 0, 1, 1);
    EXPECT_TRUE(d.rotate);
    TEST_RETURN_OK;
}

TEST_CASE(FieldHistoryGate, FirstThenSecondFieldRotatesOnce)
{
    Gate g;
    auto d = g.OnFreshPublish(PK::FirstField, 0, 1, 0);
    EXPECT_TRUE(d.rotate);
    EXPECT_FALSE(g.nextSlotComplete);
    d = g.OnFreshPublish(PK::SecondField, 1, 1, 1);
    EXPECT_FALSE(d.rotate);
    EXPECT_FALSE(d.repairOutgoing);
    EXPECT_TRUE(g.nextSlotComplete);
    // Next frame's first field: no repair needed.
    d = g.OnFreshPublish(PK::FirstField, 0, 2, 2);
    EXPECT_TRUE(d.rotate);
    EXPECT_FALSE(d.repairOutgoing);
    EXPECT_EQ(g.repairs, uint64_t(0));
    TEST_RETURN_OK;
}

TEST_CASE(FieldHistoryGate, MissedSecondFieldIsRepairedWithOtherParity)
{
    Gate g;
    g.OnFreshPublish(PK::FirstField, 0, 1, 0);       // F0(1), F1(1) missed
    auto d = g.OnFreshPublish(PK::FirstField, 0, 2, 1);
    EXPECT_TRUE(d.repairOutgoing);
    EXPECT_EQ(d.repairRowParity, 1);                  // fresh top -> copy bottom
    EXPECT_TRUE(d.rotate);
    EXPECT_EQ(g.repairs, uint64_t(1));

    // Bottom-field-first source: fresh rows are odd, repair copies even.
    Gate b;
    b.OnFreshPublish(PK::FirstField, 1, 1, 0);
    d = b.OnFreshPublish(PK::FirstField, 1, 2, 1);
    EXPECT_TRUE(d.repairOutgoing);
    EXPECT_EQ(d.repairRowParity, 0);
    TEST_RETURN_OK;
}

TEST_CASE(FieldHistoryGate, NoRepairAfterLongerGapOrWhenDisabled)
{
    Gate g;
    g.OnFreshPublish(PK::FirstField, 0, 1, 0);
    // Sequence jumps by two: the other-parity rows belong to frame 2.
    auto d = g.OnFreshPublish(PK::FirstField, 0, 3, 1);
    EXPECT_FALSE(d.repairOutgoing);
    EXPECT_TRUE(d.rotate);

    Gate off;
    off.OnFreshPublish(PK::FirstField, 0, 1, 0);
    d = off.OnFreshPublish(PK::FirstField, 0, 2, 1, /*repairEnabled*/false);
    EXPECT_FALSE(d.repairOutgoing);

    // Nothing buffered yet: nothing to repair.
    Gate empty;
    empty.nextSlotComplete = false;
    d = empty.OnFreshPublish(PK::FirstField, 0, 1, 0);
    EXPECT_FALSE(d.repairOutgoing);
    TEST_RETURN_OK;
}

TEST_CASE(FieldHistoryGate, MissedFirstFieldRotatesCompleteSlot)
{
    Gate g;
    g.OnFreshPublish(PK::FirstField, 0, 1, 0);
    g.OnFreshPublish(PK::SecondField, 1, 1, 1);
    // F0(2) missed; F1(2) carries both fields of frame 2.
    auto d = g.OnFreshPublish(PK::SecondField, 1, 2, 1);
    EXPECT_TRUE(d.rotate);
    EXPECT_FALSE(d.repairOutgoing);
    EXPECT_TRUE(g.nextSlotComplete);
    TEST_RETURN_OK;
}

TEST_CASE(FieldHistoryGate, ResetForgetsSequenceButKeepsRepairCount)
{
    Gate g;
    g.OnFreshPublish(PK::FirstField, 0, 1, 0);
    g.OnFreshPublish(PK::FirstField, 0, 2, 1);
    EXPECT_EQ(g.repairs, uint64_t(1));
    g.Reset();
    EXPECT_EQ(g.lastSourceSequence, uint64_t(0));
    EXPECT_TRUE(g.nextSlotComplete);
    EXPECT_EQ(g.repairs, uint64_t(1));
    TEST_RETURN_OK;
}

TEST_CASE(FieldHistoryGate, SequenceGoingBackwardsResetsHistory)
{
    Gate g;
    g.OnFreshPublish(PK::FirstField, 0, 500, 3);
    // Capture restarted: the first publish can even be an F1 with seq 0.
    auto d = g.OnFreshPublish(PK::SecondField, 1, 0, 3);
    EXPECT_TRUE(d.resetHistory);
    EXPECT_FALSE(d.rotate);          // seq 0 == fresh lastSourceSequence
    EXPECT_FALSE(d.repairOutgoing);
    d = g.OnFreshPublish(PK::FirstField, 0, 1, 0);
    EXPECT_FALSE(d.resetHistory);
    EXPECT_TRUE(d.rotate);
    EXPECT_FALSE(d.repairOutgoing);
    // Frame mode restart as well.
    Gate f;
    f.OnFreshPublish(PK::FullFrame, 0, 900, 3);
    d = f.OnFreshPublish(PK::FullFrame, 0, 1, 3);
    EXPECT_TRUE(d.resetHistory);
    EXPECT_TRUE(d.rotate);
    TEST_RETURN_OK;
}

TEST_CASE(FieldHistoryGate, PipelineModelCurSlotAlwaysHoldsOneFrame)
{
    for (std::uint32_t seed = 1; seed <= 8; ++seed)
    {
        std::uint64_t repairs = 0;
        const int bad = RunPipelineModel(/*repair*/true, /*miss%*/25,
                                         /*fields*/4000, seed, &repairs);
        EXPECT_EQ(bad, 0);
        EXPECT_TRUE(repairs > 0);   // the run did hit missed F1 publishes
    }
    TEST_RETURN_OK;
}

TEST_CASE(FieldHistoryGate, PipelineModelWithoutRepairShowsStaleField)
{
    // Reproduces the pre-v0.39 behaviour: the model must catch it.
    const int bad = RunPipelineModel(/*repair*/false, /*miss%*/25,
                                     /*fields*/4000, /*seed*/1, nullptr);
    EXPECT_TRUE(bad > 0);
    TEST_RETURN_OK;
}
