// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    FrameProducer.cpp
 * @brief   Producer thread implementation with ring delay line.
 */

#include "FrameProducer.h"
#include "PixelFormat.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace ajainput
{

FrameProducer::FrameProducer(FrameQueue&        queue,
                             SharedGLContext&   sharedContext,
                             SharedTextureRing* textureRing,
                             std::size_t        widthPixels,
                             std::size_t        heightPixels,
                             float              targetFps)
    : m_queue(queue)
    , m_sharedContext(sharedContext)
    , m_textureRing(textureRing)
    , m_width(widthPixels)
    , m_height(heightPixels)
    , m_frameBytes(UYVYFrameSize(widthPixels, heightPixels))
    , m_targetFps(targetFps > 1.0f ? targetFps : 60.0f)
    , m_generator(widthPixels, heightPixels)
{
}

FrameProducer::~FrameProducer()
{
    Stop();
}

void FrameProducer::Start()
{
    if (m_running.exchange(true, std::memory_order_acq_rel))
        return;

    m_thread = std::thread(&FrameProducer::Run, this);
}

void FrameProducer::Stop()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel))
        return;

    if (m_thread.joinable())
        m_thread.join();
}

void FrameProducer::SetRingDepth(int depth) noexcept
{
    depth = std::clamp(depth, kMinRingDepth, kMaxRingDepth);
    m_targetRingDepth.store(depth, std::memory_order_relaxed);
}

void FrameProducer::ApplyThreadPriority()
{
#ifdef _WIN32
    // HIGHEST gives priority over normal application threads while leaving
    // the system responsive (mouse, audio). TIME_CRITICAL exists for cases
    // where this proves insufficient under load, but using it has system-
    // wide consequences and is reserved for later tuning if needed.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
}

void FrameProducer::Run()
{
    using clock = std::chrono::steady_clock;

    ApplyThreadPriority();

    // Attempt to acquire the shared GL context for this thread. Off-render
    // -thread upload is only viable when both the secondary context and a
    // texture ring are available; we evaluate the combination here once
    // and stick with the chosen path for the lifetime of the thread.
    const bool sharedContextActive = m_sharedContext.ActivateOnCurrentThread();
    const bool useOffThreadUpload  = sharedContextActive && (m_textureRing != nullptr);

    // v0.39 safety net. The generator always emits 8-bit UYVY; the sink
    // (texture ring or byte queue) copies its own frame size out of the
    // buffer we hand it. If the owner sized the sink for another pixel
    // kind (10-bit v210 is ~33% larger), publishing would over-read our
    // buffer. Publish nothing instead: the output stays black rather
    // than crashing the host.
    const std::size_t sinkBytes = useOffThreadUpload
        ? m_textureRing->FrameBytes()
        : m_queue.FrameBytes();
    const bool sinkSizeOk = (sinkBytes == m_frameBytes);

    const auto framePeriod = std::chrono::nanoseconds(
        static_cast<int64_t>(1.0e9 / m_targetFps));

    const auto startTime  = clock::now();
    auto       nextDeadline = startTime;

    // The delay line simulates AutoCirculate ring depth.
    std::vector<DelayLineEntry> delayLine(kMaxRingDepth);
    for (auto& e : delayLine)
        e.data.assign(m_frameBytes, 0);
    int delayHead = 0;
    int delaySize = 0;

    auto enqueueToDelayLine = [&](const uint8_t* data, clock::time_point producedAt) {
        const int ringDepth = m_targetRingDepth.load(std::memory_order_relaxed);

        if (delaySize >= ringDepth)
        {
            delayHead = (delayHead + 1) % kMaxRingDepth;
            --delaySize;
            m_ringDropCount.fetch_add(1, std::memory_order_relaxed);
        }

        const int tailIdx = (delayHead + delaySize) % kMaxRingDepth;
        std::memcpy(delayLine[tailIdx].data.data(), data, m_frameBytes);
        delayLine[tailIdx].producedAt = producedAt;
        ++delaySize;
    };

    // Publish strategy: ASAP, unconditional.
    //
    // We deliberately do NOT add any artificial latency here. The ring
    // depth parameter exists for the real-hardware path (it will be the
    // AutoCirculateInitForInput buffer count); in the simulator it does
    // not affect perceived latency, exactly as it does not in real AJA
    // capture. AutoCirculate's ring depth is a margin for capture-side
    // delays, not a source of latency on its own. The intrinsic capture
    // latency is just the SDI line time, which the simulator already
    // matches by running the generator at 60 Hz.
    //
    // Backpressure is also intentionally absent. Coupling the producer
    // to the consumer through HasUnconsumedFrame() created a phase-lock
    // between the 60 Hz generator and the ~100 Hz Resolume render that
    // could shift the system into a stable high-latency state after a
    // single perturbation. The triple-buffered texture ring guarantees
    // writer ≠ reader regardless of relative rates, so publishing
    // unconditionally is safe; any publication the consumer misses is
    // counted at the texture ring and is the simulator's analog of an
    // AutoCirculate output drop.
    auto tryPublishHead = [&]() {
        if (delaySize == 0)
            return;

        const DelayLineEntry& head = delayLine[delayHead];
        // Never publish a mis-sized frame (see sinkSizeOk above).
        if (sinkSizeOk)
        {
            if (useOffThreadUpload)
                m_textureRing->Upload(head.data.data(), head.producedAt);
            else
                m_queue.WriteFrame(head.data.data(), head.producedAt);
        }

        delayHead = (delayHead + 1) % kMaxRingDepth;
        --delaySize;
    };

    while (m_running.load(std::memory_order_acquire))
    {
        const auto now = clock::now();

        const float elapsedSeconds =
            std::chrono::duration<float>(now - startTime).count();
        const int   inputIdx    = m_inputIndex.load(std::memory_order_relaxed);
        const int   ringDepth   = m_targetRingDepth.load(std::memory_order_relaxed);

        m_generator.GenerateFrame(elapsedSeconds, inputIdx);

        // Shrink trim: if ring depth has been reduced below the current
        // pending count, drop excess frames immediately.
        while (delaySize > ringDepth)
        {
            delayHead = (delayHead + 1) % kMaxRingDepth;
            --delaySize;
            m_ringDropCount.fetch_add(1, std::memory_order_relaxed);
        }

        enqueueToDelayLine(m_generator.Data(), now);
        tryPublishHead();

        // Schedule the next deadline.
        nextDeadline += framePeriod;

        const auto afterWork = clock::now();
        if (nextDeadline < afterWork)
            nextDeadline = afterWork + framePeriod;

        std::this_thread::sleep_until(nextDeadline);
    }

    // Ensure the secondary context is released from this thread before it
    // exits, so the SharedGLContext owner can safely tear it down later.
    if (sharedContextActive)
        m_sharedContext.DeactivateOnCurrentThread();
}

} // namespace ajainput
