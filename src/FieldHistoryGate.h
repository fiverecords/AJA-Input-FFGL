// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Pure decision logic for feeding the YADIF history from the texture
// ring, extracted from AJAInputPlugin so it can be unit-tested without an
// OpenGL context.
//
// Background
// ----------
// In field mode the capture thread publishes one ring frame per FIELD.
// Each publish carries the whole capture accumulator: the field that
// just arrived plus the other field's rows from the last time that
// field arrived. For a TFF source the publishes for source frame N are
//   F0(N): top rows = F0(N), bottom rows = F1(N-1)   (first field)
//   F1(N): top rows = F0(N), bottom rows = F1(N)     (second field)
// and both carry sourceSequence N.
//
// The renderer keeps three history slots (YadifSlots). A publish with a
// NEW sourceSequence rotates the slots and copies the ring frame into
// the new "next" slot; a publish with the SAME sequence (the second
// field) re-copies the ring frame into that same slot, completing it.
//
// The ring is latest-wins: when two publishes land between two renders,
// the renderer only sees the second. Missing a first-field publish is
// harmless (the second field's publish carries both fields). Missing a
// second-field publish is not: the slot keeps the previous frame's
// second field, and once it becomes "cur" every second-field output
// keeps those stale rows while the interpolated rows come from newer
// fields, which shows as combing on motion only, on every other output
// field.
//
// The repair: when the first-field publish of frame N+1 arrives and the
// outgoing slot (frame N) is still incomplete, the publish's
// other-parity rows ARE F1(N) (the accumulator only replaces a field's
// rows when that field arrives, and AutoCirculate delivers fields in
// strict F0/F1 alternation). Copying just those rows into the outgoing
// slot before rotating completes it. After a longer gap (sequence jumps
// by more than one) those rows belong to a later frame, so no repair.

#pragma once

#include <cstdint>

namespace ajainput
{

struct FieldHistoryGate
{
    /// What a fresh ring publish is.
    enum class PublishKind
    {
        FullFrame,    ///< frame-mode capture: one publish per source frame
        FirstField,   ///< field mode, first field of a source frame (F0)
        SecondField,  ///< field mode, second field of a source frame (F1)
    };

    struct Decision
    {
        /// The source sequence went backwards: the capture session was
        /// restarted (input, ring depth or mode change, reopen) and the
        /// buffered history belongs to the old session. The caller must
        /// YadifSlots::Reset() before acting on the rest.
        bool resetHistory    = false;
        /// Copy rows of `repairRowParity` (0 = even/top, 1 = odd/bottom)
        /// from the publish into the CURRENT next slot. Done before the
        /// rotation below.
        bool repairOutgoing  = false;
        int  repairRowParity = -1;
        /// true: rotate (YadifSlots::Advance) and copy the whole publish
        /// into the new next slot. false: re-copy the whole publish into
        /// the current next slot (second field of the same frame).
        bool rotate          = false;
    };

    /// Source-frame sequence the history last rotated for.
    std::uint64_t lastSourceSequence = 0;
    /// False between a rotation on a first-field publish and the
    /// second-field publish that completes that slot.
    bool          nextSlotComplete   = true;
    /// Repairs performed since construction (diagnostics only; not
    /// cleared by Reset).
    std::uint64_t repairs            = 0;

    void Reset() noexcept
    {
        lastSourceSequence = 0;
        nextSlotComplete   = true;
    }

    /// @param kind            what the publish is
    /// @param freshRowParity  row parity of the field that just arrived
    ///                        (0 = even/top, 1 = odd/bottom); ignored
    ///                        for FullFrame
    /// @param sourceSequence  the publish's source-frame sequence
    /// @param framesBuffered  YadifSlots::framesBuffered before the call
    ///                        (treated as 0 when resetHistory is set)
    /// @param repairEnabled   false disables the repair (used when the
    ///                        copy shader is not available, and by tests
    ///                        to reproduce the pre-v0.39 behaviour)
    Decision OnFreshPublish(PublishKind   kind,
                            int           freshRowParity,
                            std::uint64_t sourceSequence,
                            int           framesBuffered,
                            bool          repairEnabled = true) noexcept
    {
        Decision d;
        if (sourceSequence < lastSourceSequence)
        {
            d.resetHistory = true;
            Reset();
            framesBuffered = 0;
        }

        if (kind != PublishKind::FullFrame &&
            sourceSequence == lastSourceSequence)
        {
            // Second field of the frame we already rotated for.
            d.rotate         = false;
            nextSlotComplete = true;
            return d;
        }

        if (repairEnabled &&
            kind == PublishKind::FirstField &&
            !nextSlotComplete &&
            framesBuffered >= 1 &&
            sourceSequence == lastSourceSequence + 1)
        {
            d.repairOutgoing  = true;
            d.repairRowParity = (freshRowParity & 1) ^ 1;
            ++repairs;
        }

        d.rotate           = true;
        lastSourceSequence = sourceSequence;
        // A slot filled from a FullFrame publish, or from a second-field
        // publish whose first field was missed (the accumulator already
        // holds both fields of this frame), is complete; one filled from
        // a first-field publish waits for its second field.
        nextSlotComplete   = (kind != PublishKind::FirstField);
        return d;
    }
};

} // namespace ajainput
