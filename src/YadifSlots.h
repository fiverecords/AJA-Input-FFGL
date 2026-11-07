// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Pure state machine for YADIF's three-slot ring of history textures.
//
// The yadif deinterlacer needs access to three temporally adjacent
// captured frames (prev, cur, next) to do its motion-adaptive
// reconstruction. AJAInputPlugin holds three GL textures of its own
// (separately from the SharedTextureRing, whose slots cannot be safely
// retained for more than one frame) and rotates which texture plays
// which role as new frames arrive. This struct holds just the
// integer bookkeeping for that rotation, decoupled from the GL
// resources themselves so that the rotation invariants can be
// exercised by unit tests without an OpenGL context.
//
// State semantics:
// - prevSlot, curSlot, nextSlot index into the owner's three-element
//   array of GL textures. They are always a permutation of {0, 1, 2}.
// - framesBuffered counts how many distinct captured frames have been
//   absorbed since the last Reset. Saturates at 3. The owner gates
//   the yadif sample path on framesBuffered >= 3 so that all three
//   role textures contain valid data before they are sampled.
// - lastBlittedSequence records the per-frame monotonically-increasing
//   identifier of the most recently absorbed frame; Advance() is
//   idempotent against duplicate sequences, so a render that re-acquires
//   the same ring slot does not double-rotate.
//
// Rotation rule (see Advance below): the incoming frame overwrites the
// slot that currently plays the "prev" role; after the rotation that
// slot becomes the new "next", which is the invariant the rendering
// code relies on (CurTexture samples the slot one frame behind the most
// recently arrived one, so PrevTexture and NextTexture both straddle
// it in time).

#pragma once

#include <cstdint>

namespace ajainput
{

struct YadifSlots
{
    int prevSlot = 0;  // slot containing the "previous" frame
    int curSlot  = 1;  // slot containing the "current"  frame (rendered)
    int nextSlot = 2;  // slot containing the "next"     frame (most recent arrival)
    int framesBuffered = 0;                       // 0..3, saturates
    std::uint64_t lastBlittedSequence = ~0ull;    // sentinel = "nothing yet"

    /// @brief Reset to initial state. Use after history reallocation
    ///        or whenever the buffered history is known to be invalid
    ///        (format change, source switch). Idempotent.
    void Reset() noexcept
    {
        prevSlot = 0;
        curSlot  = 1;
        nextSlot = 2;
        framesBuffered = 0;
        lastBlittedSequence = ~0ull;
    }

    /// @brief Outcome of an Advance() call.
    struct AdvanceResult
    {
        bool didAdvance;        ///< False when the call was a no-op
                                ///  (duplicate sequence).
        int  destinationSlot;   ///< When didAdvance is true, the slot
                                ///  index the caller should write the
                                ///  incoming frame into. When false,
                                ///  -1 (do not write anything).
    };

    /// @brief Advance the rotation for a freshly-arrived captured
    ///        frame identified by sourceSequence. The caller is
    ///        expected to write the incoming frame's data into
    ///        result.destinationSlot AFTER this call returns (the
    ///        indices have already been updated, and the destination
    ///        slot is the one that now plays the "next" role).
    ///
    ///        Idempotent against duplicate sequences: if Advance is
    ///        called twice with the same sourceSequence, the second
    ///        call returns {false, -1} without touching state. A
    ///        sequence OLDER than the last one accepted is rejected the
    ///        same way: rotating an older frame into "next" would put
    ///        the three history frames out of temporal order. Sequences
    ///        restart from 1 only together with a history Reset() (ring
    ///        re-initialisation always re-runs EnsureYadifHistory).
    ///        framesBuffered saturates at 3 (after which further
    ///        advances rotate but no longer increment it).
    AdvanceResult Advance(std::uint64_t sourceSequence) noexcept
    {
        if (lastBlittedSequence != ~0ull &&
            sourceSequence <= lastBlittedSequence)
            return {false, -1};

        // Rotation: incoming frame overwrites the old prev slot, which
        // then becomes the new next. Old cur becomes new prev, old
        // next becomes new cur.
        //
        //   before:  prev=A    cur=B    next=C        (C is newest)
        //   after :  prev=B    cur=C    next=(incoming, written to A's slot)
        const int incomingSlot = prevSlot;
        const int newPrevSlot  = curSlot;
        const int newCurSlot   = nextSlot;
        const int newNextSlot  = incomingSlot;

        prevSlot = newPrevSlot;
        curSlot  = newCurSlot;
        nextSlot = newNextSlot;

        if (framesBuffered < 3)
            ++framesBuffered;

        lastBlittedSequence = sourceSequence;
        return {true, incomingSlot};
    }
};

}  // namespace ajainput
