// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    FrameQueue.cpp
 * @brief   Implementation of the lock-free triple-buffer frame queue.
 */

#include "FrameQueue.h"

#include <cstring>

namespace ajainput
{

void FrameQueue::Initialise(std::size_t frameBytes)
{
    Release();

    m_frameBytes = frameBytes;
    for (Slot& s : m_slots)
    {
        s.data.assign(frameBytes, 0);
        s.producedAt  = {};
        s.publishedAt = {};
        s.sequence    = 0;
    }

    m_writerIdx        = 0;
    m_readerIdx        = 2;
    m_mid.store(1, std::memory_order_relaxed);   // slot 1, not fresh
    m_producerSequence = 0;

    // Full counter reset; this is a fresh allocation, no in-flight frames
    // can exist. After this point, ResetStats may be called freely without
    // perturbing the producer/consumer handshake.
    m_producedCount.store(0, std::memory_order_relaxed);
    m_droppedCount.store(0,  std::memory_order_relaxed);
    m_repeatedCount.store(0, std::memory_order_relaxed);
}

void FrameQueue::ResetStats() noexcept
{
    // Only the user-visible drop and repeat counters are reset. The
    // produced-frame counter is preserved because AcquireRead uses it
    // as the "has anything ever been published" gate; zeroing it while
    // a frame is in flight would make the consumer skip a valid frame.
    m_droppedCount.store(0,  std::memory_order_relaxed);
    m_repeatedCount.store(0, std::memory_order_relaxed);
}

void FrameQueue::Release()
{
    for (Slot& s : m_slots)
    {
        s.data.clear();
        s.data.shrink_to_fit();
    }
    m_frameBytes = 0;
}

void FrameQueue::WriteFrame(const uint8_t*                        data,
                            std::chrono::steady_clock::time_point producedAt)
{
    if (m_frameBytes == 0 || data == nullptr)
        return;

    // Fill the slot the producer currently owns. Plain memory writes; the
    // release semantics on the subsequent exchanges publish them.
    Slot& slot       = m_slots[m_writerIdx];
    std::memcpy(slot.data.data(), data, m_frameBytes);
    slot.producedAt  = producedAt;
    slot.publishedAt = std::chrono::steady_clock::now();
    slot.sequence    = ++m_producerSequence;

    // Publish: swap the just-written slot into the ready position with
    // the fresh bit set, in ONE atomic operation. The previously ready
    // slot becomes the next writer slot. If the previous value still
    // carried the fresh bit, that frame was never consumed and is now
    // lost; count it as a drop.
    //
    // Index and fresh bit must live in the same atomic. With two
    // separate atomics (the pre-v0.39 layout) a publish landing between
    // the consumer's "clear fresh" and "swap index" steps left the bit
    // set while the consumer already held the newest slot, so the next
    // AcquireRead returned an OLDER slot marked fresh.
    const int previousMid = m_mid.exchange(m_writerIdx | kFreshBit,
                                           std::memory_order_acq_rel);
    m_writerIdx = previousMid & kIndexMask;
    if ((previousMid & kFreshBit) != 0)
        m_droppedCount.fetch_add(1, std::memory_order_relaxed);

    m_producedCount.fetch_add(1, std::memory_order_relaxed);
}

FrameQueue::ReadResult FrameQueue::AcquireRead()
{
    ReadResult result;

    // If no frame has been published yet, return an empty result. The
    // consumer treats this as "skip this render" or "show black". This
    // gate relies on m_producedCount being monotonically increasing past
    // zero once the first frame is published, and never being reset by
    // anything other than Initialise() (see ResetStats() for the
    // rationale).
    if (m_producedCount.load(std::memory_order_acquire) == 0)
        return result;

    // Try to grab a freshly published frame. Only the consumer clears
    // the fresh bit, so once the load observes it set, the exchange is
    // guaranteed to return a fresh slot; if the producer published again
    // in between, the exchange simply returns that newer slot. Otherwise
    // we keep the slot we already had (frame repeat).
    const bool hadFresh =
        (m_mid.load(std::memory_order_acquire) & kFreshBit) != 0;
    if (hadFresh)
    {
        const int previousMid = m_mid.exchange(m_readerIdx,
                                               std::memory_order_acq_rel);
        m_readerIdx = previousMid & kIndexMask;
    }
    else
    {
        m_repeatedCount.fetch_add(1, std::memory_order_relaxed);
    }

    const Slot& slot   = m_slots[m_readerIdx];
    result.data        = slot.data.data();
    result.size        = slot.data.size();
    result.producedAt  = slot.producedAt;
    result.publishedAt = slot.publishedAt;
    result.sequence    = slot.sequence;
    result.isFresh     = hadFresh;
    return result;
}

} // namespace ajainput
