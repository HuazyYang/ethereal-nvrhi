/*
* OffsetAllocator: https://github.com/sebbbi/OffsetAllocator
*
* MIT License
*
* Copyright (c) 2023 Sebastian Aaltonen
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

// Sebastian Aaltonen's OffsetAllocator (2023), as upstream except for the namespace (nvrhi::details), the
// <cstdint> types, the asserts (nvrhi's, see offset-allocator.cpp) and C++17 aggregate initialisation in place
// of designated initializers. Allocates ranges of a linear address space in O(1): two-level power-of-two bins
// (32 top bins of 8 leaf bins each, bin sizes on a 3-bit-mantissa float curve) over a node list, with free
// neighbours merged on free. Offsets are unitless: the caller picks the unit (bytes, descriptors, ...).
// The 16-bit node index mode (USE_16_BIT_NODE_INDICES) is off: node indices are 32-bit.

#pragma once

#include <cstdint>

namespace nvrhi::details
{
    // 16 bit offsets mode will halve the metadata storage cost
    // But it only supports up to 65536 maximum allocation count
#ifdef USE_16_BIT_NODE_INDICES
    typedef uint16_t NodeIndex;
#else
    typedef uint32_t NodeIndex;
#endif

    static constexpr uint32_t NUM_TOP_BINS = 32;
    static constexpr uint32_t BINS_PER_LEAF = 8;
    static constexpr uint32_t TOP_BINS_INDEX_SHIFT = 3;
    static constexpr uint32_t LEAF_BINS_INDEX_MASK = 0x7;
    static constexpr uint32_t NUM_LEAF_BINS = NUM_TOP_BINS * BINS_PER_LEAF;

    struct Allocation
    {
        static constexpr uint32_t NO_SPACE = 0xffffffff;

        uint32_t offset = NO_SPACE;
        NodeIndex metadata = NO_SPACE; // internal: node index
    };

    struct StorageReport
    {
        uint32_t totalFreeSpace;
        uint32_t largestFreeRegion;
    };

    struct StorageReportFull
    {
        struct Region
        {
            uint32_t size;
            uint32_t count;
        };

        Region freeRegions[NUM_LEAF_BINS];
    };

    class Allocator
    {
    public:
        Allocator(uint32_t size, uint32_t maxAllocs = 128 * 1024);
        Allocator(Allocator &&other);
        ~Allocator();
        void reset();

        Allocation allocate(uint32_t size);
        void free(Allocation allocation);

        uint32_t allocationSize(Allocation allocation) const;
        StorageReport storageReport() const;
        StorageReportFull storageReportFull() const;

    private:
        uint32_t insertNodeIntoBin(uint32_t size, uint32_t dataOffset);
        void removeNodeFromBin(uint32_t nodeIndex);

        struct Node
        {
            static constexpr NodeIndex unused = 0xffffffff;

            uint32_t dataOffset = 0;
            uint32_t dataSize = 0;
            NodeIndex binListPrev = unused;
            NodeIndex binListNext = unused;
            NodeIndex neighborPrev = unused;
            NodeIndex neighborNext = unused;
            bool used = false; // TODO: Merge as bit flag
        };

        uint32_t m_size;
        uint32_t m_maxAllocs;
        uint32_t m_freeStorage;

        uint32_t m_usedBinsTop;
        uint8_t m_usedBins[NUM_TOP_BINS];
        NodeIndex m_binIndices[NUM_LEAF_BINS];

        Node* m_nodes;
        NodeIndex* m_freeNodes;
        uint32_t m_freeOffset;
    };
}
