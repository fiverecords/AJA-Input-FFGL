// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    FrameProducer.h
 * @brief   Background thread that drives the synthetic frame generator and
 *          models the latency contribution of an AutoCirculate-style ring.
 *
 * Ring depth (2..7) is accepted for parity with the AutoCirculate path
 * but does not add latency: each generated frame is queued and
 * published in the same pass, so the internal delay line never holds
 * more than one frame (the publish strategy comment in FrameProducer.cpp
 * explains why; AutoCirculate's ring depth is a margin for capture-side
 * delays, not a latency source). The only effect of lowering the depth
 * below the pending count is the drop counter.
 *
 * The producer thread runs at the highest practical OS priority on Windows
 * to closely approximate what a capture thread will need in production.
 * It does not use TIME_CRITICAL by default to keep the system responsive.
 */
#pragma once

#include "FrameQueue.h"
#include "SharedGLContext.h"
#include "SharedTextureRing.h"
#include "SyntheticFrameGenerator.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <random>
#include <thread>
#include <vector>

namespace ajainput
{

class FrameProducer
{
public:
    /// Maximum ring depth this simulator supports, matching the upper bound
    /// historically exposed by the native AJA Resolume integration.
    static constexpr int kMaxRingDepth = 7;
    static constexpr int kMinRingDepth = 2;

    /**
     * @param queue          CPU-side fallback queue; must outlive this
     *                       producer. Used when the shared GL context is
     *                       not available.
     * @param sharedContext  Secondary GL context. The producer activates
     *                       it on its thread at startup.
     * @param textureRing    Optional shared texture ring; if non-null and
     *                       the shared context is available, the producer
     *                       uploads frames directly into GL textures and
     *                       the byte queue is bypassed. If null, the
     *                       byte queue is used unconditionally.
     * @param widthPixels    Frame width.
     * @param heightPixels   Frame height.
     * @param targetFps      Production rate in frames per second.
     */
    FrameProducer(FrameQueue&        queue,
                  SharedGLContext&   sharedContext,
                  SharedTextureRing* textureRing,
                  std::size_t        widthPixels,
                  std::size_t        heightPixels,
                  float              targetFps);

    ~FrameProducer();

    FrameProducer(const FrameProducer&)            = delete;
    FrameProducer& operator=(const FrameProducer&) = delete;

    void Start();
    void Stop();

    void SetInputIndex(int idx) noexcept
    { m_inputIndex.store(idx, std::memory_order_relaxed); }

    /// Ring depth in [kMinRingDepth, kMaxRingDepth]. Out-of-range values are
    /// clamped. Applied at the start of the next iteration of the producer.
    void SetRingDepth(int depth) noexcept;

    /// @return Currently active ring depth as observed by the producer.
    int RingDepth() const noexcept
    { return m_targetRingDepth.load(std::memory_order_relaxed); }

    /// @return Frames lost at the producer-side ring (the simulator's
    ///         equivalent of AutoCirculate's input-side frame drops when
    ///         the ring fills because the capture thread cannot keep up).
    uint64_t RingDropCount() const noexcept
    { return m_ringDropCount.load(std::memory_order_relaxed); }

    /// @brief Zero the ring drop counter. Typically called when ring depth
    ///        is changed to provide a clean test interval.
    void ResetStats() noexcept
    { m_ringDropCount.store(0, std::memory_order_relaxed); }

    bool IsRunning() const noexcept
    { return m_running.load(std::memory_order_acquire); }

private:
    void Run();
    void ApplyThreadPriority();

    struct DelayLineEntry
    {
        std::vector<uint8_t>                  data;
        std::chrono::steady_clock::time_point producedAt;
    };

    FrameQueue&             m_queue;
    SharedGLContext&        m_sharedContext;
    SharedTextureRing*      m_textureRing;
    std::size_t             m_width;
    std::size_t             m_height;
    std::size_t             m_frameBytes;
    float                   m_targetFps;

    SyntheticFrameGenerator m_generator;
    std::thread             m_thread;
    std::atomic<bool>       m_running       { false };
    std::atomic<int>        m_inputIndex    { 0 };

    // Ring depth, read by the producer thread on each iteration.
    std::atomic<int>        m_targetRingDepth { kMinRingDepth };

    // Producer-side drop counter (frames lost at the simulated ring).
    std::atomic<uint64_t>   m_ringDropCount   { 0 };
};

} // namespace ajainput
