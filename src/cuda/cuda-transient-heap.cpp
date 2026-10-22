/*
* Copyright (c) 2026, ethereal contributors. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

// Transient memory: the upload and scratch memory of command list recordings (ADR 0011). The design follows the
// upload managers of the D3D12 and Vulkan backends (src/vulkan/vulkan-upload.cpp): a recording bump-allocates in
// chunks, and a chunk is reused only after the submission that used it has finished. Here the chunks are pooled
// by the device, and a submission returns its chunks to the pool when its event has signalled (Queue::retire).

#include "cuda-backend.h"

#include <algorithm>

namespace nvrhi::cuda
{
    namespace
    {
        constexpr size_t c_ChunkGranularity = 64 * 1024;

        size_t AlignUp(size_t value, size_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        void FreeChunk(TransientChunk* chunk)
        {
            if (chunk->kind == TransientMemory::Upload)
            {
                if (chunk->hostPtr)
                    g_Cuda.cuMemFreeHost(chunk->hostPtr);
            }
            else if (chunk->devicePtr)
            {
                g_Cuda.cuMemFree(chunk->devicePtr);
            }
            delete chunk;
        }
    }

    TransientChunk* TransientPool::acquire(TransientMemory kind, size_t minSize)
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            // The smallest free chunk of the kind that fits.
            auto best = m_Free.end();
            for (auto it = m_Free.begin(); it != m_Free.end(); ++it)
            {
                if ((*it)->kind == kind && (*it)->size >= minSize && (best == m_Free.end() || (*it)->size < (*best)->size))
                    best = it;
            }
            if (best != m_Free.end())
            {
                TransientChunk* chunk = *best;
                m_Free.erase(best);
                chunk->used = 0;
                return chunk;
            }
        }

        ContextScope scope(m_Context);
        auto chunk = new TransientChunk();
        chunk->kind = kind;
        chunk->size = AlignUp(std::max(minSize, c_ChunkGranularity), c_ChunkGranularity);

        bool ok;
        if (kind == TransientMemory::Upload)
        {
            void* host = nullptr;
            ok = NVRHI_CUDA_CHECK(m_Context, cuMemHostAlloc(&host, chunk->size, CU_MEMHOSTALLOC_DEVICEMAP | CU_MEMHOSTALLOC_PORTABLE));
            chunk->hostPtr = static_cast<uint8_t*>(host);
            ok = ok && NVRHI_CUDA_CHECK(m_Context, cuMemHostGetDevicePointer(&chunk->devicePtr, host, 0));
        }
        else
        {
            ok = NVRHI_CUDA_CHECK(m_Context, cuMemAlloc(&chunk->devicePtr, chunk->size));
        }

        if (!ok)
        {
            FreeChunk(chunk);
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(m_Mutex);
        m_All.push_back(chunk);
        return chunk;
    }

    void TransientPool::release(TransientChunk* chunk)
    {
        if (!chunk)
            return;
        std::lock_guard<std::mutex> lock(m_Mutex);
        chunk->used = 0;
        m_Free.push_back(chunk);
    }

    void TransientPool::destroyAll()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_All.empty())
            return;
        ContextScope scope(m_Context);
        for (TransientChunk* chunk : m_All)
            FreeChunk(chunk);
        m_All.clear();
        m_Free.clear();
    }

    bool TransientAllocator::allocate(TransientMemory kind, size_t size, size_t alignment, TransientAllocation& out)
    {
        out = TransientAllocation();
        alignment = std::max<size_t>(alignment, 16);
        size = std::max<size_t>(size, 1);

        TransientChunk*& current = m_Current[uint32_t(kind)];
        if (current)
        {
            const size_t offset = AlignUp(current->used, alignment);
            if (offset + size <= current->size)
            {
                current->used = offset + size;
                out.devicePtr = current->devicePtr + offset;
                out.hostPtr = current->hostPtr ? current->hostPtr + offset : nullptr;
                return true;
            }
        }

        TransientChunk* chunk = m_Pool.acquire(kind, std::max(size, m_ChunkSize));
        if (!chunk)
            return false;
        m_Chunks.push_back(chunk);
        current = chunk;
        chunk->used = size;
        out.devicePtr = chunk->devicePtr;
        out.hostPtr = chunk->hostPtr;
        return true;
    }

    void TransientAllocator::detachChunks(std::vector<TransientChunk*>& chunks)
    {
        chunks.insert(chunks.end(), m_Chunks.begin(), m_Chunks.end());
        m_Chunks.clear();
        m_Current = {};
    }

    void TransientAllocator::releaseAll()
    {
        for (TransientChunk* chunk : m_Chunks)
            m_Pool.release(chunk);
        m_Chunks.clear();
        m_Current = {};
    }
}
