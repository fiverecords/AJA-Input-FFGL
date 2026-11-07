// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    FrameStorePool.h
 * @brief   Process-wide arbiter for AJA FrameStore claims.
 *
 * AJA cards expose a small fixed number of FrameStores (the hardware
 * DMA engines that AutoCirculate drives) — for example the Kona X base
 * model has 2, the Kona X 4CH has 4. A FrameStore can only host ONE
 * active AutoCirculate session at a time, regardless of which physical
 * SDI / HDMI input feeds it. That means when several plugin instances
 * coexist in the same Resolume composition (capturing different SDI
 * inputs of the same card), each instance needs its own FrameStore.
 *
 * The pool tracks claims across all plugin instances within the same
 * process, per card (v0.41: card 2's FrameStore 1 is not card 1's). Each
 * capture asks for a free FrameStore at startup and releases it at
 * teardown. The card-level capabilities tell us how
 * many FrameStores exist; the pool refuses to hand out indices beyond
 * that.
 *
 * Implementation: lock-free atomic bitmask. Cheap on uncontended paths
 * (the common case for plugin lifecycle) and correct under any thread
 * combination.
 *
 * Cross-process coordination is NOT this class's job. The AJA driver
 * enforces exclusivity at card-open time, and a Kona X opened by
 * Resolume cannot be opened concurrently by another application, so
 * the only place where multiple FrameStores need arbitration is
 * within a single host process — which is exactly what this class
 * covers.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace ajainput
{

class FrameStorePool
{
public:
    /**
     * @brief Claim the lowest-numbered FrameStore that is currently free
     *        and within the device's reported capability.
     *
     * @param  deviceIndex     Card (scanner index); each card has its own
     *                         FrameStores. Indices >= kMaxDevices share
     *                         the last slot.
     * @param  maxFrameStores  Total FrameStores the underlying device
     *                         exposes (1..32). Values above the card's
     *                         hardware limit are not allocated even if
     *                         the bit slot is free; this mirrors the
     *                         driver's behaviour and saves us a wasted
     *                         AutoCirculate init that would fail
     *                         immediately.
     * @return Zero-based FrameStore index on success, or -1 if every
     *         FrameStore up to maxFrameStores is already claimed.
     */
    static int Claim(uint32_t deviceIndex, int maxFrameStores) noexcept
    { return Claim(deviceIndex, maxFrameStores, 0u); }

    static constexpr uint32_t kMaxDevices = 16;

    /**
     * @brief v0.40: same, skipping the FrameStores set in avoidMask (bit i
     *        = FrameStore i). Used to keep clear of FrameStores another
     *        application is using (see AJADevice::ForeignBusyFrameStores).
     */
    static int Claim(uint32_t deviceIndex, int maxFrameStores, uint32_t avoidMask) noexcept;

    /**
     * @brief v0.41: claim one specific FrameStore.
     * @return true if it was free and is now ours.
     */
    static bool TryClaim(uint32_t deviceIndex, int frameStoreIndex) noexcept;

    /**
     * @brief v0.42: claim every FrameStore in mask, or none of them.
     *        Used by 4K captures that need a FrameStore pair (TSI) or
     *        four of them (squares); see CaptureTopology.h.
     * @return true if all were free and are now ours.
     */
    static bool TryClaimMask(uint32_t deviceIndex, uint32_t mask) noexcept;

    /// @brief v0.42: release every FrameStore in mask. Idempotent.
    static void ReleaseMask(uint32_t deviceIndex, uint32_t mask) noexcept;

    /**
     * @brief v0.41: the FrameStore an input is paired with by default on
     *        AJA cards: SDI N -> FrameStore N (zero-based N-1). v0.42: on
     *        cards without SDI inputs (Kona HDMI) HDMI N -> FrameStore N
     *        likewise; elsewhere HDMI has none. Quad-link inputs use a
     *        fixed set instead (CaptureTopology.h) and have none here.
     *        Inputs beyond the FrameStore count have none.
     * @param  inputNumber     input number (InputId.h)
     * @param  cardHasSdiInputs  false for HDMI-only cards
     * @return zero-based index, or -1.
     */
    static int NaturalFrameStore(int inputNumber, int maxFrameStores,
                                 bool cardHasSdiInputs = true) noexcept;

    /**
     * @brief v0.41: order in which a capture of the given input tries the
     *        FrameStores. An input with a natural FrameStore: that one
     *        first, then the others lowest-first. HDMI on a card with SDI
     *        inputs: highest-first, which leaves the low FrameStores to
     *        the SDI inputs (their natural pairs).
     *        v0.42: an HDMI input on a card with fewer CSCs than
     *        FrameStores (KONA XM: 3 for 4) tries the FrameStores that
     *        have a CSC (CSC N belongs to FrameStore N) first, in that
     *        order, since an RGB source needs one.
     * @param  out          receives zero-based indices
     * @param  outCapacity  size of out
     * @param  numCSCs      the card's CSC count; negative: do not care
     * @return number of indices written (min(maxFrameStores, 32, outCapacity))
     */
    static int PreferenceOrder(int inputNumber, int maxFrameStores,
                               int* out, int outCapacity,
                               bool cardHasSdiInputs = true,
                               int numCSCs = -1) noexcept;

    /**
     * @brief Release a previously claimed FrameStore back into the pool.
     *        Idempotent; releasing an unclaimed index is a no-op.
     */
    static void Release(uint32_t deviceIndex, int frameStoreIndex) noexcept;

    /// @brief Snapshot of currently claimed FrameStores (bitmask).
    ///        For diagnostic logging only; not race-free in any
    ///        meaningful sense.
    static uint32_t ClaimedMask(uint32_t deviceIndex) noexcept;
};

} // namespace ajainput
