// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    CaptureTopology.cpp
 * @brief   See CaptureTopology.h.
 */

#include "CaptureTopology.h"

#include "InputId.h"

#include <cstdio>

namespace ajainput
{

const char* ToString(Topology t) noexcept
{
    switch (t)
    {
        case Topology::Single:   return "single";
        case Topology::Tsi2:     return "2SI";
        case Topology::Squares4: return "squares";
    }
    return "?";
}

int FrameStoreCount(uint32_t mask) noexcept
{
    int n = 0;
    for (; mask != 0u; mask &= mask - 1u)
        ++n;
    return n;
}

int LowestFrameStore(uint32_t mask) noexcept
{
    for (int i = 0; i < 32; ++i)
        if (mask & (1u << i))
            return i;
    return -1;
}

namespace
{

uint32_t RangeMask(int first, int count) noexcept
{
    uint32_t m = 0u;
    for (int i = 0; i < count; ++i)
        if (first + i >= 0 && first + i < 32)
            m |= 1u << (first + i);
    return m;
}

FrameStorePlan Fail(const char* why) noexcept
{
    FrameStorePlan p;
    p.ok     = false;
    p.reason = why;
    return p;
}

FrameStorePlan SingleAny() noexcept
{
    FrameStorePlan p;
    p.ok       = true;
    p.topology = Topology::Single;
    return p;
}

FrameStorePlan Tsi2(int first, const CardCaps& caps) noexcept
{
    FrameStorePlan p;
    p.ok              = true;
    p.topology        = Topology::Tsi2;
    p.fixedMask       = RangeMask(first, 2);
    p.firstFrameStore = first;
    // Cards with 425 muxes have four of them. On four-FrameStore cards
    // pair (1+2) uses muxes 1+2 and pair (3+4) muxes 3+4; on the eight-
    // FrameStore Corvid 88, pair (5+6) uses muxes 3+4 (the SDK demo's
    // mux = fb / 2).
    const int fs = caps.numFrameStores > 4 ? caps.numFrameStores : 4;
    p.muxBase         = first * 4 / fs;
    return p;
}

FrameStorePlan Squares4(int first) noexcept
{
    FrameStorePlan p;
    p.ok              = true;
    p.topology        = Topology::Squares4;
    p.fixedMask       = RangeMask(first, 4);
    p.firstFrameStore = first;
    return p;
}

} // namespace

bool CardOffersQuadLink(int firstSdiOneBased, const CardCaps& caps) noexcept
{
    if (firstSdiOneBased != 1 && firstSdiOneBased != 5)
        return false;
    const int first = firstSdiOneBased - 1;
    if (caps.can12gRouting || !caps.can4K || caps.isIP)
        return false;
    if (caps.numSdiInputs < first + 4)
        return false;
    if (caps.can425Mux)
        return caps.numFrameStores >= first + 2;
    return caps.numFrameStores >= first + 4;
}

FrameStorePlan PlanFrameStores(int n, bool signalIs4K,
                               const CardCaps& caps) noexcept
{
    using namespace input;
    switch (KindOf(n))
    {
        case Kind::None:
            return Fail("invalid input");

        case Kind::SdiQuad:
        {
            const int firstSdi = SdiNumber(n);
            if (caps.can12gRouting)
                return Fail("this card takes 4K on one SDI cable: pick that SDI input");
            if (!CardOffersQuadLink(firstSdi, caps))
                return Fail("this card cannot capture quad-link 4K on these inputs");
            if (caps.can425Mux)
                return Tsi2(firstSdi - 1, caps);
            return Squares4(firstSdi - 1);
        }

        case Kind::Sdi:
            if (!signalIs4K || caps.can12gRouting)
                return SingleAny();
            return Fail("4K on one SDI cable needs a card with 12G routing "
                        "(use the quad-link input)");

        case Kind::Hdmi:
        {
            if (!signalIs4K || caps.can12gRouting)
                return SingleAny();
            const int hdmi = HdmiNumber(n);
            // TSI pairs: HDMI 1 -> FrameStores 1+2, HDMI 2 -> 3+4 (as AJA's
            // ntv2capture4k picks channel 1 / 3 for the Kona HDMI).
            const int first = (hdmi - 1) * 2;
            if (!caps.can425Mux || hdmi > 2 || caps.numFrameStores < first + 2 ||
                (caps.hdmiQuadrantMask & (1u << (hdmi - 1))) == 0u)
                return Fail("4K over HDMI is not supported on this card or input");
            return Tsi2(first, caps);
        }
    }
    return Fail("invalid input");
}

int TsiCscIndex(const FrameStorePlan& plan, int path, const CardCaps& caps) noexcept
{
    if (path < 0 || path > 3 || plan.firstFrameStore < 0)
        return -1;
    if (caps.numFrameStores > 0 && caps.numCSCs >= 2 * caps.numFrameStores)
        return plan.firstFrameStore + (path & 1) + 4 * (path >> 1);
    return 2 * plan.firstFrameStore + path;   // AJA's ntv2capture4k
}

void FormatFrameStoreSet(uint32_t mask, char* buf, std::size_t size) noexcept
{
    if (buf == nullptr || size == 0)
        return;
    const int count = FrameStoreCount(mask);
    const int low   = LowestFrameStore(mask);
    if (count == 0 || low < 0)
        std::snprintf(buf, size, "FS?");
    else if (count == 1)
        std::snprintf(buf, size, "FS%d", low + 1);
    else if (count == 2)
        std::snprintf(buf, size, "FS%d+%d", low + 1, low + 2);
    else
        std::snprintf(buf, size, "FS%d-%d", low + 1, low + count);
}

} // namespace ajainput
