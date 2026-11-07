// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    MultiFormatRegistry.h
 * @brief   Process-wide record of the video format each capture runs on
 *          each card, used to warn when two inputs on one card use frame
 *          rates that AJA multi-format devices cannot run together.
 *
 * AJA multi-format cards (the Kona X among them) accept a different
 * video format on every FrameStore, but only when the frame rates belong
 * to the same "family". The rule is the SDK's IsMultiFormatCompatible
 * (ntv2utils.cpp): 14.98/29.97/59.94/119.88, 15/30/60/120, 23.98/47.95,
 * 24/48 and 25/50 are the families. Raster size does not matter. Mixing
 * families (e.g. 1080i50 on one input and 1080p59.94 on the other) is
 * reported by users of other software as stutter and dropped frames.
 *
 * Each AJACaptureSource publishes its format when a capture session
 * starts and withdraws it on teardown; each plugin instance asks for a
 * conflict involving its own FrameStore. Thread-safe (one mutex); all
 * calls are short and happen at session start/stop or a few times per
 * second.
 */
#pragma once

#include <cstdint>

namespace ajainput
{

/// @return true when two NTV2VideoFormat values (raw ints) may run at the
///         same time on one multi-format card. Thin wrapper over the
///         SDK's IsMultiFormatCompatible; false if either is unknown.
bool AreMultiFormatCompatible(int ntv2FormatRawA, int ntv2FormatRawB) noexcept;

class MultiFormatRegistry
{
public:
    struct Conflict
    {
        bool found              = false;
        int  otherFrameStore    = -1;  ///< zero-based
        int  otherInputNumber = 0;   ///< input number (InputId.h)
        int  otherFormatRaw     = 0;   ///< NTV2VideoFormat of the other capture
    };

    /// Record (or replace) the format a capture's input carries. Keeps the
    /// entry's "running" mark (ClaimCardFormat) only while the format is
    /// the same.
    static void Publish(uint32_t deviceIndex, int frameStore,
                        int inputNumber, int ntv2FormatRaw);

    /// Forget a capture (session stopped, no signal, instance closed).
    static void Withdraw(uint32_t deviceIndex, int frameStore);

    /// First other capture on the same card whose format is not
    /// multi-format compatible with this one's. No conflict when this
    /// FrameStore has nothing published. v0.42: on a card without
    /// multi-format (singleFormatCard) any other format is a conflict.
    static Conflict FindConflict(uint32_t deviceIndex, int frameStore,
                                 bool singleFormatCard = false);

    // ----- v0.42: cards without multi-format ---------------------------
    //
    // On those cards (Kona 1, Kona LHi, Kona 3G, Corvid 24, ...) the SDK
    // programs every video format into channel 1 (CNTV2Card::SetVideoFormat
    // uses channel 1 when multi-format is not active), so all FrameStores
    // run one format. A capture must not reprogram it under another one.

    /**
     * @brief Atomically: unless another capture on the card runs a
     *        different format, record this one as running ntv2FormatRaw.
     * @param outOther  filled with the capture in the way on failure
     * @return true if this capture may configure the card for the format.
     */
    static bool ClaimCardFormat(uint32_t deviceIndex, int frameStore,
                                int inputNumber, int ntv2FormatRaw,
                                Conflict* outOther = nullptr);

    /// Clear the running mark (capture torn down); the format entry stays.
    static void ReleaseCardFormat(uint32_t deviceIndex, int frameStore);

    // ----- v0.41: lending FrameStores to the inputs being shown --------
    //
    // An input whose clips are on screen but that has no FrameStore
    // (every one is taken) says so here every ~100 ms. A capture none of
    // whose clips has been rendered for a moment gives its FrameStore up
    // when one is wanted (AJACaptureSource::MaybeYield), so inputs whose
    // clips sit in the deck do not starve the one on screen.

    /// An input on screen needs a FrameStore. v0.42: wantedMask names the
    /// FrameStores that would help (a 4K capture needs a fixed pair or
    /// four); requests within 300 ms of each other add up.
    static void WantFrameStore(uint32_t deviceIndex, int64_t nowMs,
                               uint32_t wantedMask = ~0u);
    /// Whether WantFrameStore was called at or after sinceMs.
    static bool FrameStoreWantedSince(uint32_t deviceIndex, int64_t sinceMs);
    /// Like FrameStoreWantedSince, and forget the want when true, so one
    /// request makes one capture give its FrameStore up, not all of them.
    /// v0.42: only when the request wants one of ownMask's FrameStores.
    static bool ConsumeFrameStoreWant(uint32_t deviceIndex, int64_t sinceMs,
                                      uint32_t ownMask = ~0u);

    /// Counts clip renders process-wide (every SharedCapture::Update).
    /// Lets a capture tell "my clips are not on screen" (others keep
    /// rendering) from "the render thread stalled" (nobody renders).
    /// Lock-free; any thread.
    static uint64_t AdvanceRenderTick() noexcept;
    static uint64_t RenderTick() noexcept;

    /// Test helper: drop every entry (formats and FrameStore requests).
    static void Clear();
};

} // namespace ajainput
