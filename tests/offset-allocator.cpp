/*
* OffsetAllocator tests: https://github.com/sebbbi/OffsetAllocator
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

// The upstream OffsetAllocator tests (offsetAllocatorTests.cpp, Catch2) on GoogleTest: the SmallFloat bin-size
// conversions, then the allocator (basic, allocate simple / merge trivial / reuse trivial / reuse complex / zero
// fragmentation). Every allocator case ends by allocating the whole space at offset 0: no fragmentation is left.
// src/common/offset-allocator.cpp is compiled into this executable (it is internal to nvrhi and not exported).

#include "../src/common/offset-allocator.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace nvrhi::details
{
    namespace SmallFloat
    {
        extern uint32_t uintToFloatRoundUp(uint32_t size);
        extern uint32_t uintToFloatRoundDown(uint32_t size);
        extern uint32_t floatToUint(uint32_t floatValue);
    }
}

namespace {

using nvrhi::details::Allocation;
using nvrhi::details::Allocator;
using nvrhi::details::StorageReport;
namespace SmallFloat = nvrhi::details::SmallFloat;

constexpr uint32_t c_Size = 1024 * 1024 * 256;

TEST(SmallFloat, UintToFloat)
{
    // Denorms, exp=1 and exp=2 + mantissa = 0 are all precise.
    // NOTE: Assuming 8 value (3 bit) mantissa.
    // If this test fails, please change this assumption!
    uint32_t preciseNumberCount = 17;
    for (uint32_t i = 0; i < preciseNumberCount; i++)
    {
        uint32_t roundUp = SmallFloat::uintToFloatRoundUp(i);
        uint32_t roundDown = SmallFloat::uintToFloatRoundDown(i);
        EXPECT_EQ(i, roundUp);
        EXPECT_EQ(i, roundDown);
    }

    // Test some random picked numbers
    struct NumberFloatUpDown
    {
        uint32_t number;
        uint32_t up;
        uint32_t down;
    };

    const NumberFloatUpDown testData[] = {
        {17, 17, 16},
        {118, 39, 38},
        {1024, 64, 64},
        {65536, 112, 112},
        {529445, 137, 136},
        {1048575, 144, 143},
    };

    for (const NumberFloatUpDown& v : testData)
    {
        uint32_t roundUp = SmallFloat::uintToFloatRoundUp(v.number);
        uint32_t roundDown = SmallFloat::uintToFloatRoundDown(v.number);
        EXPECT_EQ(roundUp, v.up) << "number " << v.number;
        EXPECT_EQ(roundDown, v.down) << "number " << v.number;
    }
}

TEST(SmallFloat, FloatToUint)
{
    // Denorms, exp=1 and exp=2 + mantissa = 0 are all precise.
    // NOTE: Assuming 8 value (3 bit) mantissa.
    // If this test fails, please change this assumption!
    uint32_t preciseNumberCount = 17;
    for (uint32_t i = 0; i < preciseNumberCount; i++)
    {
        uint32_t v = SmallFloat::floatToUint(i);
        EXPECT_EQ(i, v);
    }

    // Test that float->uint->float conversion is precise for all numbers
    // NOTE: Test values < 240. 240->4G = overflows 32 bit integer
    for (uint32_t i = 0; i < 240; i++)
    {
        uint32_t v = SmallFloat::floatToUint(i);
        uint32_t roundUp = SmallFloat::uintToFloatRoundUp(v);
        uint32_t roundDown = SmallFloat::uintToFloatRoundDown(v);
        EXPECT_EQ(i, roundUp);
        EXPECT_EQ(i, roundDown);
    }
}

TEST(OffsetAllocator, Basic)
{
    Allocator allocator(c_Size);
    Allocation a = allocator.allocate(1337);
    uint32_t offset = a.offset;
    EXPECT_EQ(offset, 0u);
    allocator.free(a);
}

// One 256 M allocator per case (upstream: one TEST_CASE, one SECTION per case).
class OffsetAllocatorAllocate : public ::testing::Test
{
protected:
    Allocator allocator{c_Size};

    // End: Validate that allocator has no fragmentation left. Should be 100% clean.
    void validateAll()
    {
        Allocation validateAll = allocator.allocate(c_Size);
        EXPECT_EQ(validateAll.offset, 0u);
        allocator.free(validateAll);
    }
};

TEST_F(OffsetAllocatorAllocate, Simple)
{
    // Free merges neighbor empty nodes. Next allocation should also have offset = 0
    Allocation a = allocator.allocate(0);
    EXPECT_EQ(a.offset, 0u);

    Allocation b = allocator.allocate(1);
    EXPECT_EQ(b.offset, 0u);

    Allocation c = allocator.allocate(123);
    EXPECT_EQ(c.offset, 1u);

    Allocation d = allocator.allocate(1234);
    EXPECT_EQ(d.offset, 124u);

    allocator.free(a);
    allocator.free(b);
    allocator.free(c);
    allocator.free(d);

    validateAll();
}

TEST_F(OffsetAllocatorAllocate, MergeTrivial)
{
    // Free merges neighbor empty nodes. Next allocation should also have offset = 0
    Allocation a = allocator.allocate(1337);
    EXPECT_EQ(a.offset, 0u);
    allocator.free(a);

    Allocation b = allocator.allocate(1337);
    EXPECT_EQ(b.offset, 0u);
    allocator.free(b);

    validateAll();
}

TEST_F(OffsetAllocatorAllocate, ReuseTrivial)
{
    // Allocator should reuse node freed by A since the allocation C fits in the same bin (using pow2 size to be sure)
    Allocation a = allocator.allocate(1024);
    EXPECT_EQ(a.offset, 0u);

    Allocation b = allocator.allocate(3456);
    EXPECT_EQ(b.offset, 1024u);

    allocator.free(a);

    Allocation c = allocator.allocate(1024);
    EXPECT_EQ(c.offset, 0u);

    allocator.free(c);
    allocator.free(b);

    validateAll();
}

TEST_F(OffsetAllocatorAllocate, ReuseComplex)
{
    // Allocator should not reuse node freed by A since the allocation C doesn't fits in the same bin
    // However node D and E fit there and should reuse node from A
    Allocation a = allocator.allocate(1024);
    EXPECT_EQ(a.offset, 0u);

    Allocation b = allocator.allocate(3456);
    EXPECT_EQ(b.offset, 1024u);

    allocator.free(a);

    Allocation c = allocator.allocate(2345);
    EXPECT_EQ(c.offset, 1024u + 3456u);

    Allocation d = allocator.allocate(456);
    EXPECT_EQ(d.offset, 0u);

    Allocation e = allocator.allocate(512);
    EXPECT_EQ(e.offset, 456u);

    StorageReport report = allocator.storageReport();
    EXPECT_EQ(report.totalFreeSpace, c_Size - 3456 - 2345 - 456 - 512);
    EXPECT_NE(report.largestFreeRegion, report.totalFreeSpace);

    allocator.free(c);
    allocator.free(d);
    allocator.free(b);
    allocator.free(e);

    validateAll();
}

TEST_F(OffsetAllocatorAllocate, ZeroFragmentation)
{
    // Allocate 256x 1MB. Should fit. Then free four random slots and reallocate four slots.
    // Plus free four contiguous slots an allocate 4x larger slot. All must be zero fragmentation!
    Allocation allocations[256];
    for (uint32_t i = 0; i < 256; i++)
    {
        allocations[i] = allocator.allocate(1024 * 1024);
        EXPECT_EQ(allocations[i].offset, i * 1024 * 1024);
    }

    StorageReport report = allocator.storageReport();
    EXPECT_EQ(report.totalFreeSpace, 0u);
    EXPECT_EQ(report.largestFreeRegion, 0u);

    // Free four random slots
    allocator.free(allocations[243]);
    allocator.free(allocations[5]);
    allocator.free(allocations[123]);
    allocator.free(allocations[95]);

    // Free four contiguous slot (allocator must merge)
    allocator.free(allocations[151]);
    allocator.free(allocations[152]);
    allocator.free(allocations[153]);
    allocator.free(allocations[154]);

    allocations[243] = allocator.allocate(1024 * 1024);
    allocations[5] = allocator.allocate(1024 * 1024);
    allocations[123] = allocator.allocate(1024 * 1024);
    allocations[95] = allocator.allocate(1024 * 1024);
    allocations[151] = allocator.allocate(1024 * 1024 * 4); // 4x larger
    EXPECT_NE(allocations[243].offset, Allocation::NO_SPACE);
    EXPECT_NE(allocations[5].offset, Allocation::NO_SPACE);
    EXPECT_NE(allocations[123].offset, Allocation::NO_SPACE);
    EXPECT_NE(allocations[95].offset, Allocation::NO_SPACE);
    EXPECT_NE(allocations[151].offset, Allocation::NO_SPACE);

    for (uint32_t i = 0; i < 256; i++)
    {
        if (i < 152 || i > 154)
            allocator.free(allocations[i]);
    }

    StorageReport report2 = allocator.storageReport();
    EXPECT_EQ(report2.totalFreeSpace, c_Size);
    EXPECT_EQ(report2.largestFreeRegion, c_Size);

    validateAll();
}

} // namespace
