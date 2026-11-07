// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    SharedTextureRing.cpp
 * @brief   Implementation of the triple-buffered shared texture ring.
 */

#include "SharedTextureRing.h"
#include "PixelFormat.h"

#include <cstring>

namespace ajainput
{

SharedTextureRing::~SharedTextureRing()
{
    Release();
}

bool SharedTextureRing::Initialise(std::size_t widthPixels,
                                   std::size_t heightPixels,
                                   PixelKind   kind)
{
    Release();

    if ((widthPixels & 1u) != 0u || widthPixels == 0u || heightPixels == 0u)
        return false;

    m_width      = widthPixels;
    m_height     = heightPixels;
    m_kind       = kind;
    m_frameBytes = FrameSize(kind, widthPixels, heightPixels);

    // Compute the GL texture's dimensions. For UYVY: half the source
    // width (one RGBA8 texel = one macropixel = 2 source pixels). For
    // V210: 4 RGBA8 texels per 6 source pixels of aligned width. See
    // PixelFormat.h::TextureWidthTexels for the exact formula.
    const GLsizei texWidth  =
        static_cast<GLsizei>(TextureWidthTexels(kind, m_width));
    const GLsizei texHeight = static_cast<GLsizei>(m_height);

    glGenTextures(kSize, m_textures);
    for (int i = 0; i < kSize; ++i)
    {
        if (m_textures[i] == 0)
        {
            Release();
            return false;
        }
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        // Both UYVY and V210 are uploaded as RGBA8 (4 bytes per texel).
        // The interpretation differs only at sample time in the shader.
        glTexImage2D(
            GL_TEXTURE_2D, 0,
            GL_RGBA8,
            texWidth, texHeight, 0,
            GL_RGBA, GL_UNSIGNED_BYTE,
            nullptr
        );
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenBuffers(kSize, m_pbos);
    for (int i = 0; i < kSize; ++i)
    {
        if (m_pbos[i] == 0)
        {
            Release();
            return false;
        }
    }

    // Prefer GL_ARB_buffer_storage (GL 4.4 / extension): immutable
    // backing store + persistent + coherent host mapping, so we can
    // write to the PBO from arbitrary points in the producer's pipeline
    // (including from AJA's DMA engine writing through the host pointer)
    // and dispatch the PBO→texture transfer with just glBindBuffer +
    // glTexSubImage2D. No orphan, no glMapBufferRange, no glUnmapBuffer
    // per frame. The classic streaming path (orphan + map + memcpy +
    // unmap on every Upload) costs ~1-2 ms per 1080p frame in profiling
    // even on a fast Windows NVIDIA driver; eliminating it is the
    // single biggest win available to us on Windows where GPUDirect
    // (true device-to-device DMA) is not exposed by AJA's driver.
    //
    // Coherent mapping is required because the producer writes from a
    // thread that is not necessarily the one that issues glTexSubImage2D
    // (well, in our setup they are the same — both run on the capture
    // thread under the secondary GL context — but coherent lets us not
    // emit explicit glMemoryBarrier or glFlushMappedBufferRange between
    // the host write and the GPU read). CLIENT_STORAGE_BIT asks the
    // driver to keep the backing in pinned host RAM rather than VRAM,
    // which is what we need for AJA's PCIe DMA destination.
    //
    // Fallback: if glBufferStorage / persistent mapping is not
    // available (very old drivers; the NVIDIA Windows driver has had
    // ARB_buffer_storage for years), we fall back to the classic
    // streaming path and live with the per-frame overhead.
    m_persistentMapping = false;
    const bool persistentSupported =
        (glewIsSupported("GL_ARB_buffer_storage") == GL_TRUE) &&
        (glBufferStorage != nullptr);

    if (persistentSupported)
    {
        const GLbitfield storageFlags =
            GL_MAP_WRITE_BIT |
            GL_MAP_PERSISTENT_BIT |
            GL_MAP_COHERENT_BIT |
            GL_CLIENT_STORAGE_BIT;
        const GLbitfield mapFlags =
            GL_MAP_WRITE_BIT |
            GL_MAP_PERSISTENT_BIT |
            GL_MAP_COHERENT_BIT;

        bool allMapped = true;
        for (int i = 0; i < kSize; ++i)
        {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[i]);
            glBufferStorage(GL_PIXEL_UNPACK_BUFFER,
                            static_cast<GLsizeiptr>(m_frameBytes),
                            nullptr,
                            storageFlags);
            m_persistentPtrs[i] = glMapBufferRange(
                GL_PIXEL_UNPACK_BUFFER,
                0,
                static_cast<GLsizeiptr>(m_frameBytes),
                mapFlags);
            if (m_persistentPtrs[i] == nullptr)
            {
                allMapped = false;
                break;
            }
            // Initialise the host-visible region to zeros so a consumer
            // that acquires before any Upload sees defined content
            // instead of whatever the driver left there. Matches the
            // contract of the classic glBufferData(nullptr,...) path
            // which the driver typically (not guaranteedly) zeros.
            std::memset(m_persistentPtrs[i], 0, m_frameBytes);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        if (allMapped)
        {
            m_persistentMapping = true;
        }
        else
        {
            // Partial persistence is unsupported (e.g. allocator
            // pressure). Drop everything and rebuild the PBOs as
            // classic streaming buffers.
            for (int i = 0; i < kSize; ++i)
                m_persistentPtrs[i] = nullptr;
            glDeleteBuffers(kSize, m_pbos);
            for (int i = 0; i < kSize; ++i) m_pbos[i] = 0;
            glGenBuffers(kSize, m_pbos);
        }
    }

    if (!m_persistentMapping)
    {
        // Classic streaming path. Each Upload will glBufferData(NULL)
        // to orphan, glMapBufferRange to acquire a pointer, memcpy,
        // glUnmapBuffer. Pre-size the storage here so the first frame
        // doesn't pay an extra reallocation.
        for (int i = 0; i < kSize; ++i)
        {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[i]);
            glBufferData(GL_PIXEL_UNPACK_BUFFER,
                         static_cast<GLsizeiptr>(m_frameBytes),
                         nullptr,
                         GL_STREAM_DRAW);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    // Initial index layout matches FrameQueue: writer owns slot 0, mid is
    // 1, reader holds 2. No fences exist yet; the first Acquire after the
    // first Upload will pick up the freshly published slot.
    m_writerIdx = 0;
    m_readerIdx = 2;
    m_mid.store(1, std::memory_order_relaxed);   // slot 1, not fresh

    for (int i = 0; i < kSize; ++i)
    {
        m_producedAt[i]    = {};
        m_publishedAt[i]   = {};
        m_sequences[i]     = 0;
        m_fences[i]        = nullptr;
        m_readFences[i]    = nullptr;
        m_fieldHints[i]    = FieldHint::FullFrame;
        m_isFirstFields[i] = false;
        m_srcFrameSeqs[i]  = 0;
    }
    m_sequenceCounter = 0;

    m_uploadedCount.store(0, std::memory_order_relaxed);
    m_repeatedCount.store(0, std::memory_order_relaxed);
    m_consumerMissedCount.store(0, std::memory_order_relaxed);

    return true;
}

void SharedTextureRing::Release()
{
    for (int i = 0; i < kSize; ++i)
    {
        if (m_fences[i] != nullptr)
        {
            glDeleteSync(m_fences[i]);
            m_fences[i] = nullptr;
        }
        if (m_readFences[i] != nullptr)
        {
            glDeleteSync(m_readFences[i]);
            m_readFences[i] = nullptr;
        }
    }

    bool anyTex = false;
    for (int i = 0; i < kSize; ++i) if (m_textures[i] != 0) { anyTex = true; break; }
    if (anyTex)
    {
        glDeleteTextures(kSize, m_textures);
        for (int i = 0; i < kSize; ++i) m_textures[i] = 0;
    }

    bool anyPBO = false;
    for (int i = 0; i < kSize; ++i) if (m_pbos[i] != 0) { anyPBO = true; break; }
    if (anyPBO)
    {
        // Unmap the persistent host pointers BEFORE glDeleteBuffers.
        // glDeleteBuffers on a still-mapped buffer is technically legal
        // (the GL spec defines the buffer as deleted but the pointer
        // remains valid until the next swap) but the simpler contract
        // is to keep mapping and lifetime aligned: unmap, then delete.
        // Skipped silently when persistent mapping was not used.
        if (m_persistentMapping)
        {
            for (int i = 0; i < kSize; ++i)
            {
                if (m_persistentPtrs[i] != nullptr)
                {
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[i]);
                    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                    m_persistentPtrs[i] = nullptr;
                }
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
        glDeleteBuffers(kSize, m_pbos);
        for (int i = 0; i < kSize; ++i) m_pbos[i] = 0;
    }
    m_persistentMapping = false;

    m_width            = 0;
    m_height           = 0;
    m_frameBytes       = 0;
    m_sequenceCounter  = 0;

    m_writerIdx = 0;
    m_readerIdx = 2;
    m_mid.store(1, std::memory_order_relaxed);   // slot 1, not fresh

    m_uploadedCount.store(0, std::memory_order_relaxed);
    m_repeatedCount.store(0, std::memory_order_relaxed);
    m_consumerMissedCount.store(0, std::memory_order_relaxed);
}

void SharedTextureRing::Upload(const uint8_t*                        frameData,
                               std::chrono::steady_clock::time_point producedAt,
                               FieldHint                             hint,
                               bool                                  isFirstFieldOfFrame,
                               std::uint64_t                         sourceFrameSequence)
{
    if (frameData == nullptr || m_frameBytes == 0 || m_textures[0] == 0)
        return;

    const int slot = m_writerIdx;

    // v0.39: write-after-read guard. The consumer's last commands that
    // read this slot's texture (history blits, weave draw) were queued in
    // Resolume's context, possibly several frames ago, and GL does not
    // order commands across contexts. Without this wait our
    // glTexSubImage2D below could overwrite the texture before those
    // reads executed on the GPU, which under GPU load shows up as a
    // later field inside a history frame. glWaitSync is a GPU-side wait:
    // the CPU work (memcpy into the PBO) is not delayed. The consumer
    // records the fence in EndRead and hands it over with the slot
    // through the m_mid exchange; we own it now and drop it here
    // (deletion is deferred by GL until the wait has completed).
    if (m_readFences[slot] != nullptr)
    {
        glWaitSync(m_readFences[slot], 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(m_readFences[slot]);
        m_readFences[slot] = nullptr;
    }

    if (m_persistentMapping)
    {
        // Persistent path. The PBO is permanently mapped to
        // m_persistentPtrs[slot]; no orphan, no map, no unmap.
        //
        // Two cases on the input data:
        //
        // 1. Caller passed exactly our persistent pointer for this slot
        //    (e.g. the AJA capture thread DMA-targeted it directly).
        //    The bytes are already in place; we only need to flush
        //    visibility to the GL command stream and dispatch the
        //    PBO→texture copy. With GL_MAP_COHERENT_BIT, the host
        //    writes are observable by subsequent GL commands on the
        //    same context without further synchronisation, so this
        //    is a zero-copy upload.
        //
        // 2. Caller passed an unrelated host buffer (e.g. the legacy
        //    inline-upload path that still maintains m_transferBuffer).
        //    We memcpy into the persistent pointer. Same coherency
        //    guarantee covers visibility of the memcpy.
        if (frameData != m_persistentPtrs[slot])
        {
            // The PBO is about to be overwritten by the CPU: make sure
            // the previous glTexSubImage2D that read it has run (v0.39).
            // When the caller DMA'd straight into the PBO it already
            // waited, through WriterPersistentPtrForWrite().
            WaitForSlotUploadIdle(slot);
            std::memcpy(m_persistentPtrs[slot], frameData, m_frameBytes);
        }

        const GLsizei texWidth  =
            static_cast<GLsizei>(TextureWidthTexels(m_kind, m_width));
        const GLsizei texHeight = static_cast<GLsizei>(m_height);

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[slot]);
        glBindTexture(GL_TEXTURE_2D, m_textures[slot]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(
            GL_TEXTURE_2D, 0,
            0, 0,
            texWidth, texHeight,
            GL_RGBA, GL_UNSIGNED_BYTE,
            nullptr  // offset into the bound PBO
        );
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    else
    {
        // Classic streaming path (driver lacks ARB_buffer_storage).
        // Orphan + map + memcpy + unmap + glTexSubImage2D per frame.

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbos[slot]);
        glBufferData(GL_PIXEL_UNPACK_BUFFER,
                     static_cast<GLsizeiptr>(m_frameBytes),
                     nullptr,
                     GL_STREAM_DRAW);

        void* mapped = glMapBufferRange(
            GL_PIXEL_UNPACK_BUFFER,
            0,
            static_cast<GLsizeiptr>(m_frameBytes),
            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT
        );

        if (mapped != nullptr)
        {
            std::memcpy(mapped, frameData, m_frameBytes);
            glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

            const GLsizei texWidth  =
                static_cast<GLsizei>(TextureWidthTexels(m_kind, m_width));
            const GLsizei texHeight = static_cast<GLsizei>(m_height);

            glBindTexture(GL_TEXTURE_2D, m_textures[slot]);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(
                GL_TEXTURE_2D, 0,
                0, 0,
                texWidth, texHeight,
                GL_RGBA, GL_UNSIGNED_BYTE,
                nullptr  // offset into the bound PBO
            );
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    // Delete the previous fence for this slot (if any) before recording
    // the new one. The triple-buffer invariant guarantees this slot is
    // not currently being read by the consumer, so any previous fence's
    // wait has already been issued and the fence object is safe to drop.
    if (m_fences[slot] != nullptr)
    {
        glDeleteSync(m_fences[slot]);
        m_fences[slot] = nullptr;
    }
    m_fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    m_producedAt[slot]  = producedAt;
    m_publishedAt[slot] = std::chrono::steady_clock::now();
    m_sequences[slot]   = ++m_sequenceCounter;
    m_fieldHints[slot]  = hint;
    // Only meaningful for field-mode publishes; harmless to record
    // unconditionally so the consumer can read it without branching.
    m_isFirstFields[slot] = isFirstFieldOfFrame;
    m_srcFrameSeqs[slot]  = sourceFrameSequence;

    // Push the just-queued commands so the GPU starts working on them
    // immediately rather than waiting for some other implicit trigger.
    // This is critical to keep the secondary context's commands from
    // being deferred by the driver, which would defeat the purpose of
    // off-thread upload.
    glFlush();

    // Publish: one atomic exchange swaps writer <-> mid AND sets the
    // fresh bit. If the previous mid still carried the fresh bit, the
    // consumer never picked that publication up: the analog of an
    // AutoCirculate "output drop", and the metric users care about.
    //
    // Index and fresh bit share one atomic on purpose (v0.39 fix). With
    // two separate atomics, a publish landing between the consumer's
    // "clear fresh" and "swap index" steps left the bit set after the
    // consumer already held the newest slot, so the NEXT Acquire handed
    // back an older slot marked fresh. In field mode that rotated an
    // older frame into the YADIF "next" history slot.
    const int previousMid = m_mid.exchange(slot | kFreshBit,
                                           std::memory_order_acq_rel);
    m_writerIdx           = previousMid & kIndexMask;
    if ((previousMid & kFreshBit) != 0)
        m_consumerMissedCount.fetch_add(1, std::memory_order_relaxed);

    m_uploadedCount.fetch_add(1, std::memory_order_relaxed);
}

SharedTextureRing::ReadResult SharedTextureRing::Acquire()
{
    ReadResult result;

    // If no upload has ever happened, return an empty result so the
    // consumer can fall back gracefully (typically skipping the draw).
    if (m_uploadedCount.load(std::memory_order_acquire) == 0)
        return result;

    // Only the consumer clears the fresh bit, so once the load sees it
    // set, the exchange is guaranteed to return a fresh slot (the newest
    // one if the producer published again in between).
    const bool hadFresh =
        (m_mid.load(std::memory_order_acquire) & kFreshBit) != 0;
    if (hadFresh)
    {
        const int previousMid = m_mid.exchange(m_readerIdx,
                                               std::memory_order_acq_rel);
        m_readerIdx           = previousMid & kIndexMask;
    }
    else
    {
        m_repeatedCount.fetch_add(1, std::memory_order_relaxed);
    }

    // Probe the fence's signaled state non-blockingly (timeout=0) to
    // gather a diagnostic statistic: was the upload already complete
    // in the GPU command queue when the render thread got to it, or
    // does the server-side wait below actually need to insert a
    // dependency that stalls the GPU? glClientWaitSync with timeout=0
    // returns GL_ALREADY_SIGNALED if the fence has retired, or
    // GL_TIMEOUT_EXPIRED if it has not. It does not block the CPU.
    // The probe is cheap (one syscall through the driver, no spin)
    // and surfaces information we currently fly blind on.
    //
    // After probing, we still issue the server-side wait below so
    // sampling is correct regardless of probe outcome.
    if (m_fences[m_readerIdx] != nullptr)
    {
        const GLenum probe = glClientWaitSync(
            m_fences[m_readerIdx],
            /*flags*/0,
            /*timeout ns*/0);
        result.fencePending =
            (probe != GL_ALREADY_SIGNALED) &&
            (probe != GL_CONDITION_SATISFIED);
    }

    // Server-side wait: queues a GPU-side dependency on the upload's
    // commands without blocking the CPU. Subsequent sampling of the
    // texture will not start until the upload has retired in the GPU's
    // command queue. GL_TIMEOUT_IGNORED is required for the server-side
    // form (the flags must be zero too).
    if (m_fences[m_readerIdx] != nullptr)
    {
        glWaitSync(m_fences[m_readerIdx], 0, GL_TIMEOUT_IGNORED);
    }

    result.texture     = m_textures[m_readerIdx];
    result.producedAt  = m_producedAt[m_readerIdx];
    result.publishedAt = m_publishedAt[m_readerIdx];
    result.sequence    = m_sequences[m_readerIdx];
    result.isFresh     = hadFresh;
    result.isValid     = true;
    result.fieldHint           = m_fieldHints   [m_readerIdx];
    result.isFirstFieldOfFrame = m_isFirstFields[m_readerIdx];
    result.sourceFrameSequence = m_srcFrameSeqs [m_readerIdx];
    return result;
}

void SharedTextureRing::WaitForSlotUploadIdle(int slot)
{
    // m_fences[slot] is the fence recorded after this producer's last
    // upload FROM this slot's PBO (already flushed at the end of that
    // Upload). Normally it signalled long ago and this returns at once;
    // under a GPU backlog it keeps a CPU write or a DMA from overwriting
    // PBO bytes a queued glTexSubImage2D still has to read. Bounded so a
    // driver hiccup can never stall the capture thread indefinitely.
    if (slot < 0 || slot >= kSize || m_fences[slot] == nullptr)
        return;
    constexpr GLuint64 kTimeoutNs = 20ull * 1000ull * 1000ull;  // 20 ms
    (void)glClientWaitSync(m_fences[slot], 0, kTimeoutNs);
}

void* SharedTextureRing::WriterPersistentPtrForWrite()
{
    if (!m_persistentMapping)
        return nullptr;
    WaitForSlotUploadIdle(m_writerIdx);
    return m_persistentPtrs[m_writerIdx];
}

void SharedTextureRing::EndRead()
{
    if (m_textures[0] == 0 || m_uploadedCount.load(std::memory_order_relaxed) == 0)
        return;

    // Replace the fence from an earlier frame that held the same slot
    // (repeats keep the reader slot); the producer never touches the
    // reader slot's fence, so this is race-free.
    GLsync& fence = m_readFences[m_readerIdx];
    if (fence != nullptr)
        glDeleteSync(fence);
    fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // Submit it now. A fence that sits unflushed in this context could
    // hold the producer's next upload into this slot until Resolume
    // flushes on its own.
    glFlush();
}

} // namespace ajainput
