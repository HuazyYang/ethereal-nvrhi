/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
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


#include "d3d12-backend.h"

#include <sstream>

namespace nvrhi::d3d12
{

    StaticDescriptorHeap::StaticDescriptorHeap(const Context& context)
        : m_Context(context)
    {
    }

    static const char* descriptorHeapTypeName(D3D12_DESCRIPTOR_HEAP_TYPE type)
    {
        switch (type)
        {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return "CBV_SRV_UAV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: return "SAMPLER";
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: return "RTV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: return "DSV";
        default: return "unknown";
        }
    }

    // The d3d12::DeviceDesc field that sizes a heap of this type.
    static const char* descriptorHeapSizeField(D3D12_DESCRIPTOR_HEAP_TYPE type)
    {
        switch (type)
        {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return "shaderResourceViewHeapSize";
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: return "samplerHeapSize";
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: return "renderTargetViewHeapSize";
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: return "depthStencilViewHeapSize";
        default: return "unknown";
        }
    }

    HRESULT StaticDescriptorHeap::createHeaps(uint32_t numDescriptors, bool shaderVisible,
        AutoPtr<ID3D12DescriptorHeap>& heap, AutoPtr<ID3D12DescriptorHeap>& shaderVisibleHeap) const
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = m_HeapType;
        heapDesc.NumDescriptors = numDescriptors;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        HRESULT hr = m_Context.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));

        if (FAILED(hr))
            return hr;

        if (shaderVisible)
        {
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

            hr = m_Context.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&shaderVisibleHeap));

            if (FAILED(hr))
                return hr;
        }

        return S_OK;
    }

    // One node per live range and one per free gap is at most one per descriptor, plus the node index the allocator
    // never hands out: with that many nodes the allocator cannot run out of them (ADR 0009, B.2).
    static uint32_t allocatorNodeCount(uint32_t numDescriptors)
    {
        return (numDescriptors >= 0xffffffffu - 2) ? 0xffffffffu : numDescriptors + 2;
    }

    HRESULT StaticDescriptorHeap::allocateResources(D3D12_DESCRIPTOR_HEAP_TYPE heapType, uint32_t numDescriptors, bool shaderVisible)
    {
        m_Heap = nullptr;
        m_ShaderVisibleHeap = nullptr;
        m_Allocator.reset();
        m_Ranges.clear();
        m_NumAllocatedDescriptors = 0;
        m_PeakAllocatedDescriptors = 0;
        m_PeakRanges = 0;

        m_HeapType = heapType;
        numDescriptors = std::max(1u, numDescriptors);

        HRESULT hr = createHeaps(numDescriptors, shaderVisible, m_Heap, m_ShaderVisibleHeap);

        if (FAILED(hr))
        {
            m_Heap = nullptr;
            m_ShaderVisibleHeap = nullptr;
            return hr;
        }

        m_NumDescriptors = numDescriptors;
        m_StartCpuHandle = m_Heap->GetCPUDescriptorHandleForHeapStart();
        if (m_ShaderVisibleHeap)
        {
            m_StartCpuHandleShaderVisible = m_ShaderVisibleHeap->GetCPUDescriptorHandleForHeapStart();
            m_StartGpuHandleShaderVisible = m_ShaderVisibleHeap->GetGPUDescriptorHandleForHeapStart();
        }
        m_Stride = m_Context.device->GetDescriptorHandleIncrementSize(heapType);
        m_Allocator.emplace(m_NumDescriptors, allocatorNodeCount(m_NumDescriptors));

        return S_OK;
    }

    static uint32_t nextPowerOf2(uint32_t v)
    {
        // https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2

        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v++;

        return v;
    }

    HRESULT StaticDescriptorHeap::Grow(uint32_t minRequiredSize)
    {
        uint32_t oldSize = m_NumDescriptors;
        uint32_t newSize = nextPowerOf2(minRequiredSize);

        bool const isShaderVisible = m_ShaderVisibleHeap != nullptr;

        if (isShaderVisible)
        {
            uint32_t const maxSize = (m_HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                ? D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 // Not a power of 2!
                : D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;

            newSize = std::min(newSize, maxSize);

            if (newSize < minRequiredSize)
                return E_OUTOFMEMORY;
        }

        // Replay the live ranges into a larger allocator (ADR 0009, B.3). A fresh allocator hands out consecutive
        // offsets from 0 while nothing has been freed, so allocating every live range and every gap between them in
        // offset order reproduces the old layout; the gaps are then freed and the tail beyond the old heap is free
        // already. With a node per descriptor the replay cannot fail.

        std::vector<std::pair<DescriptorIndex, uint32_t>> liveRanges; // (base, size)
        liveRanges.reserve(m_Ranges.size());
        for (const auto& [base, range] : m_Ranges)
            liveRanges.emplace_back(base, m_Allocator->allocationSize(range));
        std::sort(liveRanges.begin(), liveRanges.end());

        details::Allocator newAllocator(newSize, allocatorNodeCount(newSize));
        std::vector<DescriptorRange> replayedRanges; // parallel to liveRanges; applied to the side table on success
        replayedRanges.reserve(liveRanges.size());
        std::vector<DescriptorRange> gaps;
        DescriptorIndex cursor = 0;

        for (const auto& [base, size] : liveRanges)
        {
            if (base > cursor)
                gaps.push_back(newAllocator.allocate(base - cursor));

            const DescriptorRange replayed = newAllocator.allocate(size);
            assert(replayed.offset == base);
            replayedRanges.push_back(replayed);
            cursor = base + size;
        }

        for (const DescriptorRange& gap : gaps)
            newAllocator.free(gap);

        AutoPtr<ID3D12DescriptorHeap> newHeap;
        AutoPtr<ID3D12DescriptorHeap> newShaderVisibleHeap;

        HRESULT hr = createHeaps(newSize, isShaderVisible, newHeap, newShaderVisibleHeap);

        if (FAILED(hr))
            return hr; // nothing has changed

        for (size_t i = 0; i < liveRanges.size(); i++)
            m_Ranges[liveRanges[i].first] = replayedRanges[i];
        m_Allocator.emplace(std::move(newAllocator));

        // As on ethereal-dev: the old CPU heap is the source of both copies and is released at the end; the old
        // shader-visible heap lives on in the command lists that bound it (referencedNativeResources).
        AutoPtr<ID3D12DescriptorHeap> oldHeap = m_Heap;

        m_Heap = newHeap;
        m_ShaderVisibleHeap = newShaderVisibleHeap;
        m_NumDescriptors = newSize;
        m_StartCpuHandle = m_Heap->GetCPUDescriptorHandleForHeapStart();
        if (m_ShaderVisibleHeap)
        {
            m_StartCpuHandleShaderVisible = m_ShaderVisibleHeap->GetCPUDescriptorHandleForHeapStart();
            m_StartGpuHandleShaderVisible = m_ShaderVisibleHeap->GetGPUDescriptorHandleForHeapStart();
        }

        m_Context.device->CopyDescriptorsSimple(oldSize, m_StartCpuHandle, oldHeap->GetCPUDescriptorHandleForHeapStart(), m_HeapType);

        if (m_ShaderVisibleHeap != nullptr)
        {
            m_Context.device->CopyDescriptorsSimple(oldSize, m_StartCpuHandleShaderVisible, oldHeap->GetCPUDescriptorHandleForHeapStart(), m_HeapType);
        }

        return S_OK;
    }

    DescriptorRange StaticDescriptorHeap::allocateRangeLocked(uint32_t count)
    {
        if (count == 0 || !m_Allocator)
            return DescriptorRange{};

        DescriptorRange range = m_Allocator->allocate(count);

        if (range.offset == DescriptorRange::NO_SPACE)
        {
            // Out of space or too fragmented: grow as on ethereal-dev, to a free tail of at least `count` descriptors.
            if (FAILED(Grow(m_NumDescriptors + count)))
            {
                std::stringstream ss;
                ss << "Failed to grow the " << descriptorHeapTypeName(m_HeapType) << " descriptor heap ("
                    << m_NumDescriptors << " descriptors, " << m_NumAllocatedDescriptors << " in use in " << m_Ranges.size()
                    << " ranges) for a range of " << count << " descriptors.";
                m_Context.error(ss.str());
                return DescriptorRange{};
            }

            range = m_Allocator->allocate(count);
            assert(range.offset != DescriptorRange::NO_SPACE);
        }

        m_Ranges[range.offset] = range;
        m_NumAllocatedDescriptors += count;
        m_PeakAllocatedDescriptors = std::max(m_PeakAllocatedDescriptors, m_NumAllocatedDescriptors);
        m_PeakRanges = std::max(m_PeakRanges, uint32_t(m_Ranges.size()));

        return range;
    }

    void StaticDescriptorHeap::releaseRangeLocked(DescriptorRange range)
    {
        if (range.offset == DescriptorRange::NO_SPACE || !m_Allocator)
            return;

        const auto found = m_Ranges.find(range.offset);
        if (found == m_Ranges.end())
        {
            m_Context.error("Attempted to release an un-allocated descriptor range");
            return;
        }

        range = found->second; // the side table's copy is authoritative for the allocator node
        m_NumAllocatedDescriptors -= m_Allocator->allocationSize(range);
        m_Ranges.erase(found);
        m_Allocator->free(range);
    }

    DescriptorRange StaticDescriptorHeap::allocateRange(uint32_t count) noexcept
    {
        std::lock_guard lockGuard(m_Mutex);
        return allocateRangeLocked(count);
    }

    void StaticDescriptorHeap::releaseRange(DescriptorRange range) noexcept
    {
        std::lock_guard lockGuard(m_Mutex);
        releaseRangeLocked(range);
    }

    DescriptorIndex StaticDescriptorHeap::allocateDescriptors(uint32_t count) noexcept
    {
        return allocateRange(count).offset;
    }

    DescriptorIndex StaticDescriptorHeap::allocateDescriptor() noexcept
    {
        return allocateDescriptors(1);
    }

    void StaticDescriptorHeap::releaseDescriptors(DescriptorIndex baseIndex, uint32_t count) noexcept
    {
        if (count == 0 || baseIndex == c_InvalidDescriptorIndex)
            return;

        std::lock_guard lockGuard(m_Mutex);

        const auto found = m_Ranges.find(baseIndex);
        if (found == m_Ranges.end())
        {
            m_Context.error("Attempted to release an un-allocated descriptor range");
            return;
        }

        if (m_Allocator->allocationSize(found->second) != count)
        {
            // Ranges are released whole; a partial release would leave the rest of the range unaccounted for.
            std::stringstream ss;
            ss << "Attempted to release " << count << " descriptors of a range of " << m_Allocator->allocationSize(found->second)
                << " at index " << baseIndex << "; descriptor ranges are released whole.";
            m_Context.error(ss.str());
            return;
        }

        releaseRangeLocked(found->second);
    }

    void StaticDescriptorHeap::releaseDescriptor(DescriptorIndex index) noexcept
    {
        releaseDescriptors(index, 1);
    }

    void StaticDescriptorHeap::reportUsage() const
    {
        if (!m_Context.messageCallback || !m_Allocator)
            return;

        const details::StorageReport report = m_Allocator->storageReport();

        std::stringstream ss;
        ss << "D3D12 descriptor heap " << descriptorHeapTypeName(m_HeapType) << (m_ShaderVisibleHeap ? " (shader-visible)" : "")
            << ": size " << m_NumDescriptors << " (d3d12::DeviceDesc::" << descriptorHeapSizeField(m_HeapType) << ")"
            << ", peak used " << m_PeakAllocatedDescriptors << " descriptors in " << m_PeakRanges << " ranges"
            << ", in use " << m_NumAllocatedDescriptors << " descriptors in " << m_Ranges.size() << " ranges"
            << ", largest free block " << report.largestFreeRegion;
        m_Context.info(ss.str());
    }

    D3D12_CPU_DESCRIPTOR_HANDLE StaticDescriptorHeap::getCpuHandle(DescriptorIndex index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_StartCpuHandle;
        handle.ptr += index * m_Stride;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE StaticDescriptorHeap::getCpuHandleShaderVisible(DescriptorIndex index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_StartCpuHandleShaderVisible;
        handle.ptr += index * m_Stride;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE StaticDescriptorHeap::getGpuHandle(DescriptorIndex index)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = m_StartGpuHandleShaderVisible;
        handle.ptr += index * m_Stride;
        return handle;
    }

    ID3D12DescriptorHeap* StaticDescriptorHeap::getHeap() const noexcept
    {
        return m_Heap;
    }

    ID3D12DescriptorHeap* StaticDescriptorHeap::getShaderVisibleHeap() const noexcept
    {
        return m_ShaderVisibleHeap;
    }

    void StaticDescriptorHeap::copyToShaderVisibleHeap(DescriptorIndex index, uint32_t count)
    {
        m_Context.device->CopyDescriptorsSimple(count, getCpuHandleShaderVisible(index), getCpuHandle(index), m_HeapType);
    }

} // namespace nvrhi::d3d12
