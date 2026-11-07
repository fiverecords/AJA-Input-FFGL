// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    FrameStorePool.cpp
 * @brief   Atomic bitmask backing the FrameStorePool arbiter.
 */

#include "FrameStorePool.h"

#include "InputId.h"

namespace ajainput
{
namespace
{

/// @brief Bitmask of claimed FrameStores per card. Bit N set means
///        FrameStore N is held by some capture. We allow up to 32
///        FrameStores per card, which comfortably covers every AJA card
///        in the current catalogue (largest is 8 on the Corvid 88).
std::atomic<uint32_t> g_claimedMask[FrameStorePool::kMaxDevices] = {};

std::atomic<uint32_t>& MaskFor(uint32_t deviceIndex) noexcept
{
    return g_claimedMask[deviceIndex < FrameStorePool::kMaxDevices
                             ? deviceIndex : FrameStorePool::kMaxDevices - 1];
}

} // namespace

int FrameStorePool::Claim(uint32_t deviceIndex, int maxFrameStores,
                          uint32_t avoidMask) noexcept
{
    std::atomic<uint32_t>& g = MaskFor(deviceIndex);
    if (maxFrameStores <= 0)
        return -1;
    if (maxFrameStores > 32)
        maxFrameStores = 32;

    // Lowest-first allocation: keeps FrameStore numbering predictable
    // for log readers ("first instance got 0, second got 1, third got 2"),
    // matching the driver's own preference for low-numbered channels.
    for (int i = 0; i < maxFrameStores; ++i)
    {
        const uint32_t mask = uint32_t(1) << i;
        if ((avoidMask & mask) != 0u)
            continue;
        uint32_t expected   = g.load(std::memory_order_relaxed);
        while ((expected & mask) == 0)
        {
            const uint32_t desired = expected | mask;
            if (g.compare_exchange_weak(
                    expected, desired,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                return i;
            }
            // expected was updated by compare_exchange_weak; the inner
            // loop re-checks whether the bit is still free.
        }
    }
    return -1;
}

bool FrameStorePool::TryClaim(uint32_t deviceIndex, int frameStoreIndex) noexcept
{
    if (frameStoreIndex < 0 || frameStoreIndex > 31)
        return false;
    const uint32_t mask = uint32_t(1) << frameStoreIndex;
    const uint32_t prev = MaskFor(deviceIndex).fetch_or(mask, std::memory_order_acq_rel);
    return (prev & mask) == 0u;
}

bool FrameStorePool::TryClaimMask(uint32_t deviceIndex, uint32_t mask) noexcept
{
    if (mask == 0u)
        return false;
    std::atomic<uint32_t>& g = MaskFor(deviceIndex);
    uint32_t expected = g.load(std::memory_order_relaxed);
    while ((expected & mask) == 0u)
    {
        if (g.compare_exchange_weak(expected, expected | mask,
                                    std::memory_order_acq_rel,
                                    std::memory_order_relaxed))
            return true;
    }
    return false;
}

void FrameStorePool::ReleaseMask(uint32_t deviceIndex, uint32_t mask) noexcept
{
    MaskFor(deviceIndex).fetch_and(~mask, std::memory_order_acq_rel);
}

int FrameStorePool::NaturalFrameStore(int inputNumber,
                                      int maxFrameStores,
                                      bool cardHasSdiInputs) noexcept
{
    int fs = -1;
    if (input::IsSdi(inputNumber))
        fs = input::SdiNumber(inputNumber) - 1;
    else if (input::IsHdmi(inputNumber) && !cardHasSdiInputs)
        fs = input::HdmiNumber(inputNumber) - 1;
    return (fs >= 0 && fs < maxFrameStores && fs < 32) ? fs : -1;
}

int FrameStorePool::PreferenceOrder(int inputNumber, int maxFrameStores,
                                    int* out, int outCapacity,
                                    bool cardHasSdiInputs, int numCSCs) noexcept
{
    if (out == nullptr || outCapacity <= 0 || maxFrameStores <= 0)
        return 0;
    if (maxFrameStores > 32)
        maxFrameStores = 32;
    int n = 0;
    const int natural = NaturalFrameStore(inputNumber, maxFrameStores,
                                          cardHasSdiInputs);
    if (natural >= 0)
    {
        out[n++] = natural;
        for (int i = 0; i < maxFrameStores && n < outCapacity; ++i)
            if (i != natural)
                out[n++] = i;
    }
    else if (input::IsHdmi(inputNumber))
    {
        for (int i = maxFrameStores - 1; i >= 0 && n < outCapacity; --i)
            out[n++] = i;
    }
    else
    {
        for (int i = 0; i < maxFrameStores && n < outCapacity; ++i)
            out[n++] = i;
    }
    // v0.42: HDMI tries the FrameStores with a CSC first (stable).
    if (input::IsHdmi(inputNumber) && numCSCs >= 0 && numCSCs < maxFrameStores)
    {
        int sorted[32] = {};
        int k = 0;
        for (int i = 0; i < n; ++i)
            if (out[i] < numCSCs)
                sorted[k++] = out[i];
        for (int i = 0; i < n; ++i)
            if (out[i] >= numCSCs)
                sorted[k++] = out[i];
        for (int i = 0; i < n; ++i)
            out[i] = sorted[i];
    }
    return n;
}

void FrameStorePool::Release(uint32_t deviceIndex, int frameStoreIndex) noexcept
{
    if (frameStoreIndex < 0 || frameStoreIndex > 31)
        return;
    const uint32_t mask = uint32_t(1) << frameStoreIndex;
    MaskFor(deviceIndex).fetch_and(~mask, std::memory_order_acq_rel);
}

uint32_t FrameStorePool::ClaimedMask(uint32_t deviceIndex) noexcept
{
    return MaskFor(deviceIndex).load(std::memory_order_acquire);
}

} // namespace ajainput
