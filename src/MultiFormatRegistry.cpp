// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    MultiFormatRegistry.cpp
 * @brief   See MultiFormatRegistry.h.
 */

#include "MultiFormatRegistry.h"

#include "ntv2enums.h"
#include "ntv2utils.h"

#include <atomic>
#include <map>
#include <mutex>
#include <utility>

namespace ajainput
{

namespace
{
struct Entry
{
    int  inputNumber = 0;
    int  formatRaw     = 0;
    bool running       = false;  // v0.42: the card is programmed for formatRaw
};

struct Want
{
    int64_t  ms   = 0;
    uint32_t mask = 0u;
};

using Key = std::pair<uint32_t, int>;  // (device index, FrameStore)

std::mutex& RegistryMutex()
{
    static std::mutex m;
    return m;
}

std::map<Key, Entry>& RegistryTable()
{
    static std::map<Key, Entry> table;
    return table;
}

// (device index, 0) -> last WantFrameStore time (ms) and FrameStores.
std::map<Key, Want>& WantTable()
{
    static std::map<Key, Want> table;
    return table;
}
} // namespace

bool AreMultiFormatCompatible(int ntv2FormatRawA, int ntv2FormatRawB) noexcept
{
    const auto a = static_cast<NTV2VideoFormat>(ntv2FormatRawA);
    const auto b = static_cast<NTV2VideoFormat>(ntv2FormatRawB);
    if (a == NTV2_FORMAT_UNKNOWN || b == NTV2_FORMAT_UNKNOWN)
        return false;
    return ::IsMultiFormatCompatible(a, b);
}

void MultiFormatRegistry::Publish(uint32_t deviceIndex, int frameStore,
                                  int inputNumber, int ntv2FormatRaw)
{
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Entry& e = RegistryTable()[Key(deviceIndex, frameStore)];
    if (e.formatRaw != ntv2FormatRaw || e.inputNumber != inputNumber)
        e.running = false;
    e.inputNumber = inputNumber;
    e.formatRaw     = ntv2FormatRaw;
}

void MultiFormatRegistry::Withdraw(uint32_t deviceIndex, int frameStore)
{
    std::lock_guard<std::mutex> lock(RegistryMutex());
    RegistryTable().erase(Key(deviceIndex, frameStore));
}

MultiFormatRegistry::Conflict
MultiFormatRegistry::FindConflict(uint32_t deviceIndex, int frameStore,
                                  bool singleFormatCard)
{
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Conflict c;
    const auto& table = RegistryTable();
    const auto self = table.find(Key(deviceIndex, frameStore));
    if (self == table.end())
        return c;

    for (const auto& kv : table)
    {
        if (kv.first.first != deviceIndex || kv.first.second == frameStore)
            continue;
        const bool clash = singleFormatCard
            ? (self->second.formatRaw != kv.second.formatRaw &&
               self->second.formatRaw != 0 && kv.second.formatRaw != 0)
            : !AreMultiFormatCompatible(self->second.formatRaw, kv.second.formatRaw);
        if (clash)
        {
            c.found              = true;
            c.otherFrameStore    = kv.first.second;
            c.otherInputNumber = kv.second.inputNumber;
            c.otherFormatRaw     = kv.second.formatRaw;
            return c;
        }
    }
    return c;
}

bool MultiFormatRegistry::ClaimCardFormat(uint32_t deviceIndex, int frameStore,
                                          int inputNumber, int ntv2FormatRaw,
                                          Conflict* outOther)
{
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto& table = RegistryTable();
    for (const auto& kv : table)
    {
        if (kv.first.first != deviceIndex || kv.first.second == frameStore)
            continue;
        if (kv.second.running && kv.second.formatRaw != ntv2FormatRaw)
        {
            if (outOther != nullptr)
            {
                outOther->found              = true;
                outOther->otherFrameStore    = kv.first.second;
                outOther->otherInputNumber = kv.second.inputNumber;
                outOther->otherFormatRaw     = kv.second.formatRaw;
            }
            return false;
        }
    }
    Entry& e = table[Key(deviceIndex, frameStore)];
    e.inputNumber = inputNumber;
    e.formatRaw     = ntv2FormatRaw;
    e.running       = true;
    return true;
}

void MultiFormatRegistry::ReleaseCardFormat(uint32_t deviceIndex, int frameStore)
{
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto& table = RegistryTable();
    const auto it = table.find(Key(deviceIndex, frameStore));
    if (it != table.end())
        it->second.running = false;
}

void MultiFormatRegistry::WantFrameStore(uint32_t deviceIndex, int64_t nowMs,
                                         uint32_t wantedMask)
{
    constexpr int64_t kMergeMs = 300;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Want& w = WantTable()[Key(deviceIndex, 0)];
    if (w.mask != 0u && nowMs - w.ms <= kMergeMs)
        w.mask |= wantedMask;
    else
        w.mask = wantedMask;
    w.ms = nowMs;
}

bool MultiFormatRegistry::FrameStoreWantedSince(uint32_t deviceIndex,
                                                int64_t sinceMs)
{
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto& wants = WantTable();
    const auto found = wants.find(Key(deviceIndex, 0));
    return found != wants.end() && found->second.ms >= sinceMs;
}

namespace
{
std::atomic<uint64_t> g_renderTick{0};
} // namespace

uint64_t MultiFormatRegistry::AdvanceRenderTick() noexcept
{
    return g_renderTick.fetch_add(1, std::memory_order_acq_rel) + 1;
}

uint64_t MultiFormatRegistry::RenderTick() noexcept
{
    return g_renderTick.load(std::memory_order_acquire);
}

bool MultiFormatRegistry::ConsumeFrameStoreWant(uint32_t deviceIndex,
                                                int64_t sinceMs,
                                                uint32_t ownMask)
{
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto& wants = WantTable();
    const auto found = wants.find(Key(deviceIndex, 0));
    if (found == wants.end() || found->second.ms < sinceMs ||
        (found->second.mask & ownMask) == 0u)
        return false;
    wants.erase(found);
    return true;
}

void MultiFormatRegistry::Clear()
{
    std::lock_guard<std::mutex> lock(RegistryMutex());
    RegistryTable().clear();
    WantTable().clear();
}

} // namespace ajainput
