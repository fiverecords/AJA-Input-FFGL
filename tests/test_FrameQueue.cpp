// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Regression tests for FrameQueue. The queue is the synthetic path's
// only published-frame channel when off-thread upload is disabled, so
// its producer/consumer contract is what every test of the synthetic
// path indirectly relies on.
//
// Covered:
// - Initialise + WriteFrame + AcquireRead happy path.
// - Producer overrun -> dropped count increments.
// - Consumer poll without new frame -> repeated count.
// - ResetStats zeroes the counters.
// - Two-thread stress: every read marked fresh carries a strictly
//   newer frame, and its payload matches its sequence (v0.39
//   regression guard for the split fresh-flag / index handoff race).

#include "test_framework.h"
#include "FrameQueue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

constexpr std::size_t kFrameBytes = 64;  // arbitrarily small for tests

void FillBuffer(std::vector<uint8_t>& buf, uint8_t pattern)
{
    buf.assign(kFrameBytes, pattern);
}

}  // namespace

TEST_CASE(FrameQueue, InitAndWriteSingleFrame)
{
    ajainput::FrameQueue q;
    q.Initialise(kFrameBytes);
    std::vector<uint8_t> producerBuf;
    FillBuffer(producerBuf, 0xAA);

    EXPECT_EQ(q.ProducedCount(), 0u);
    EXPECT_FALSE(q.HasUnconsumedFrame());

    q.WriteFrame(producerBuf.data(), std::chrono::steady_clock::now());

    EXPECT_EQ(q.ProducedCount(), 1u);
    EXPECT_EQ(q.DroppedCount(),  0u);
    EXPECT_TRUE(q.HasUnconsumedFrame());

    auto read = q.AcquireRead();
    ASSERT_TRUE(read.isFresh);
    ASSERT_TRUE(read.data != nullptr);
    EXPECT_EQ(read.size, kFrameBytes);
    EXPECT_EQ(read.data[0], 0xAAu);
    EXPECT_FALSE(q.HasUnconsumedFrame());
    TEST_RETURN_OK;
}

TEST_CASE(FrameQueue, ProducerOverrunDropsOldFrame)
{
    ajainput::FrameQueue q;
    q.Initialise(kFrameBytes);
    std::vector<uint8_t> bufA, bufB;
    FillBuffer(bufA, 0x11);
    FillBuffer(bufB, 0x22);

    const auto t = std::chrono::steady_clock::now();
    q.WriteFrame(bufA.data(), t);
    // Second write before any AcquireRead -> the prior unconsumed
    // frame is overwritten and counted as a drop.
    q.WriteFrame(bufB.data(), t);

    EXPECT_EQ(q.ProducedCount(), 2u);
    EXPECT_EQ(q.DroppedCount(),  1u);

    // The consumer sees the newest frame, never the dropped one.
    auto read = q.AcquireRead();
    ASSERT_TRUE(read.isFresh);
    EXPECT_EQ(read.data[0], 0x22u);
    TEST_RETURN_OK;
}

TEST_CASE(FrameQueue, RepeatedRead)
{
    ajainput::FrameQueue q;
    q.Initialise(kFrameBytes);
    std::vector<uint8_t> buf;
    FillBuffer(buf, 0x55);

    q.WriteFrame(buf.data(), std::chrono::steady_clock::now());
    auto first = q.AcquireRead();
    ASSERT_TRUE(first.isFresh);

    // Same frame, second poll: should come back as not fresh (a
    // repeat), and repeated count increments.
    auto second = q.AcquireRead();
    EXPECT_FALSE(second.isFresh);
    EXPECT_EQ(q.RepeatedCount(), 1u);
    TEST_RETURN_OK;
}

TEST_CASE(FrameQueue, ResetStatsZeroesDropsAndRepeats)
{
    ajainput::FrameQueue q;
    q.Initialise(kFrameBytes);
    std::vector<uint8_t> buf;
    FillBuffer(buf, 0x77);

    // Generate a drop and a repeat.
    q.WriteFrame(buf.data(), std::chrono::steady_clock::now());
    q.WriteFrame(buf.data(), std::chrono::steady_clock::now()); // drop
    (void)q.AcquireRead();
    (void)q.AcquireRead();                                      // repeat

    EXPECT_NE(q.DroppedCount(),  0u);
    EXPECT_NE(q.RepeatedCount(), 0u);
    const uint64_t producedBeforeReset = q.ProducedCount();
    EXPECT_NE(producedBeforeReset, 0u);

    q.ResetStats();

    // Contract: ResetStats clears the user-visible drop and repeat
    // counters but deliberately preserves ProducedCount so the
    // sequence handshake between producer and consumer keeps
    // monotonically increasing across configuration changes.
    EXPECT_EQ(q.DroppedCount(),  0u);
    EXPECT_EQ(q.RepeatedCount(), 0u);
    EXPECT_EQ(q.ProducedCount(), producedBeforeReset);
    TEST_RETURN_OK;
}

TEST_CASE(FrameQueue, ConcurrentFreshReadsAreAlwaysNewer)
{
    // Producer publishes as fast as it can; the consumer polls in a
    // tight loop. Each frame carries its own sequence number in the
    // payload. A read marked fresh must hold a frame strictly newer
    // than the previous fresh read, and the payload must match the
    // slot's sequence (no torn or mismatched slot).
    //
    // With the pre-v0.39 handoff (fresh flag and ready index in two
    // separate atomics) roughly 1-2% of fresh reads on a multi-core
    // machine returned an OLDER frame. On a single-core runner the
    // race window is rarely hit, so this test can only fail, never
    // falsely pass, on the fixed code.
    ajainput::FrameQueue q;
    q.Initialise(kFrameBytes);

    std::atomic<bool> stop { false };
    std::thread producer([&]
    {
        std::vector<uint8_t> buf(kFrameBytes, 0);
        uint64_t n = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            ++n;
            std::memcpy(buf.data(), &n, sizeof n);
            q.WriteFrame(buf.data(), std::chrono::steady_clock::now());
        }
    });

    uint64_t lastSeq     = 0;
    uint64_t freshReads  = 0;
    uint64_t notNewer    = 0;
    uint64_t mismatched  = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 <
           std::chrono::milliseconds(400))
    {
        const auto r = q.AcquireRead();
        if (r.data == nullptr || !r.isFresh)
            continue;
        ++freshReads;
        uint64_t payload = 0;
        std::memcpy(&payload, r.data, sizeof payload);
        if (payload != r.sequence)
            ++mismatched;
        if (r.sequence <= lastSeq)
            ++notNewer;
        else
            lastSeq = r.sequence;
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();

    EXPECT_TRUE(freshReads > 0);
    EXPECT_EQ(notNewer,   uint64_t(0));
    EXPECT_EQ(mismatched, uint64_t(0));
    TEST_RETURN_OK;
}
