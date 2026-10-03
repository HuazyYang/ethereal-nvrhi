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

// nvrhi::vector / string / fixed_vector / static_vector (include/nvrhi/core/containers.h): element lifetimes,
// the small-string boundary, the inline buffer of fixed_vector and EASTL's allocator mechanics.

#include <nvrhi/core/containers.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

void* AlignedMalloc(size_t size, size_t alignment) {
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    return std::aligned_alloc(alignment, (size + alignment - 1) & ~(alignment - 1));
#endif
}

void AlignedFree(void* p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}

// Counts allocations and checks that every block it frees is one it allocated.
class CountingAllocator final : public nvrhi::IMemoryAllocator {
 public:
    void* Allocate(size_t size) noexcept override { return Track(std::malloc(size)); }
    void Free(void* p) noexcept override {
        if (Untrack(p)) std::free(p);
    }
    void* AllocateAligned(size_t size, size_t alignment) noexcept override {
        return Track(AlignedMalloc(size, alignment));
    }
    void FreeAligned(void* p) noexcept override {
        if (Untrack(p)) AlignedFree(p);
    }

    int allocations = 0;
    int frees = 0;
    int foreignFrees = 0;  // blocks this allocator never allocated
    size_t live() const { return m_live.size(); }

 private:
    void* Track(void* p) {
        if (p) {
            ++allocations;
            m_live.insert(p);
        }
        return p;
    }
    bool Untrack(void* p) {
        if (!p) return false;
        if (m_live.erase(p) == 0) {
            ++foreignFrees;
            return false;
        }
        ++frees;
        return true;
    }
    std::set<void*> m_live;
};

// A non-trivial element: counts constructions and destructions.
struct Tracked {
    static int constructed;
    static int destroyed;
    static int alive() { return constructed - destroyed; }
    static void reset() { constructed = destroyed = 0; }

    int value = 0;
    int* heap = nullptr;  // owned: copies must deep-copy, moves must steal

    Tracked() : heap(new int(0)) { ++constructed; }
    Tracked(int v) : value(v), heap(new int(v)) { ++constructed; }
    Tracked(const Tracked& o) : value(o.value), heap(new int(*o.heap)) { ++constructed; }
    Tracked(Tracked&& o) noexcept : value(o.value), heap(o.heap) {
        o.heap = nullptr;
        o.value = -1;
        ++constructed;
    }
    Tracked& operator=(const Tracked& o) {
        if (this != &o) {
            delete heap;
            value = o.value;
            heap = new int(*o.heap);
        }
        return *this;
    }
    Tracked& operator=(Tracked&& o) noexcept {
        if (this != &o) {
            delete heap;
            value = o.value;
            heap = o.heap;
            o.heap = nullptr;
            o.value = -1;
        }
        return *this;
    }
    ~Tracked() {
        delete heap;
        ++destroyed;
    }
    bool operator==(const Tracked& o) const { return value == o.value; }
};
int Tracked::constructed = 0;
int Tracked::destroyed = 0;

template <typename V>
std::vector<int> Values(const V& v) {
    std::vector<int> out;
    for (const auto& e : v) out.push_back(int(e.value));
    return out;
}

template <typename V>
std::vector<int> Ints(const V& v) {
    return std::vector<int>(v.begin(), v.end());
}

}  // namespace

// ---- sizes --------------------------------------------------------------------------------------------------

TEST(Containers, Sizes) {
    EXPECT_EQ(sizeof(nvrhi::allocator), sizeof(void*));
    EXPECT_EQ(sizeof(nvrhi::vector<int>), 4 * sizeof(void*));
    EXPECT_EQ(sizeof(nvrhi::vector<Tracked>), 4 * sizeof(void*));
    EXPECT_EQ(sizeof(nvrhi::string), 4 * sizeof(void*));
    EXPECT_EQ(sizeof(nvrhi::static_vector<uint32_t, 8>), 8 * sizeof(uint32_t) + sizeof(size_t));
    EXPECT_EQ(sizeof(nvrhi::fixed_vector<uint64_t, 3>), 5 * sizeof(void*) + 3 * sizeof(uint64_t));
}

// ---- vector -------------------------------------------------------------------------------------------------

TEST(Vector, ElementLifetimes) {
    Tracked::reset();
    {
        nvrhi::vector<Tracked> v;
        for (int i = 0; i < 10; ++i) v.push_back(Tracked(i));
        v.emplace_back(10);
        EXPECT_EQ(v.size(), 11u);
        EXPECT_EQ(Tracked::alive(), 11);

        v.insert(v.begin() + 2, Tracked(100));            // middle, within capacity or not
        v.insert(v.end(), {Tracked(200), Tracked(201)});   // ViewTracer.cpp: insert(end(), {...})
        v.insert(v.begin(), 2, Tracked(7));
        EXPECT_EQ(Values(v), (std::vector<int>{7, 7, 0, 1, 100, 2, 3, 4, 5, 6, 7, 8, 9, 10, 200, 201}));

        v.erase(v.begin() + 1);
        v.erase(v.begin() + 3, v.begin() + 6);
        EXPECT_EQ(Values(v), (std::vector<int>{7, 0, 1, 4, 5, 6, 7, 8, 9, 10, 200, 201}));
        EXPECT_EQ(Tracked::alive(), int(v.size()));

        v.resize(3);
        EXPECT_EQ(Values(v), (std::vector<int>{7, 0, 1}));
        v.resize(5, Tracked(42));
        EXPECT_EQ(Values(v), (std::vector<int>{7, 0, 1, 42, 42}));
        v.resize(6);
        EXPECT_EQ(v.back().value, 0);
        EXPECT_EQ(Tracked::alive(), 6);

        v.reserve(100);
        EXPECT_GE(v.capacity(), 100u);
        v.shrink_to_fit();
        EXPECT_EQ(v.capacity(), v.size());

        v.pop_back();
        EXPECT_EQ(Tracked::alive(), 5);

        nvrhi::vector<Tracked> copy(v);
        EXPECT_EQ(copy, v);
        EXPECT_NE(*copy[0].heap, -1);
        EXPECT_NE(copy[0].heap, v[0].heap);  // deep copies

        nvrhi::vector<Tracked> moved(std::move(copy));
        EXPECT_TRUE(copy.empty());
        EXPECT_EQ(moved, v);

        v.clear();
        EXPECT_TRUE(v.empty());
        EXPECT_EQ(Tracked::alive(), 5);  // moved
    }
    EXPECT_EQ(Tracked::alive(), 0);
}

TEST(Vector, InsertRanges) {
    nvrhi::vector<int> v{1, 2, 3, 4, 5};
    v.reserve(20);
    const int extra[] = {10, 11, 12};
    v.insert(v.begin() + 1, std::begin(extra), std::end(extra));  // fewer elements after pos than inserted? no
    EXPECT_EQ(Ints(v), (std::vector<int>{1, 10, 11, 12, 2, 3, 4, 5}));
    v.insert(v.end() - 1, std::begin(extra), std::end(extra));  // more inserted than elements after pos
    EXPECT_EQ(Ints(v), (std::vector<int>{1, 10, 11, 12, 2, 3, 4, 10, 11, 12, 5}));
    std::set<int> s{-3, -2, -1};  // a non-pointer forward iterator
    v.insert(v.begin(), s.begin(), s.end());
    EXPECT_EQ(v[0], -3);
    EXPECT_EQ(v.size(), 14u);

    v.assign(3, 9);
    EXPECT_EQ(Ints(v), (std::vector<int>{9, 9, 9}));
    v = {4, 5};
    EXPECT_EQ(Ints(v), (std::vector<int>{4, 5}));
    v.assign(s.begin(), s.end());
    EXPECT_EQ(Ints(v), (std::vector<int>{-3, -2, -1}));
}

TEST(Vector, SelfReferencingInsert) {
    Tracked::reset();
    {
        nvrhi::vector<Tracked> v;
        v.push_back(Tracked(1));
        v.shrink_to_fit();
        for (int i = 0; i < 6; ++i) v.push_back(v[0]);  // reallocates while referring to an element
        v.insert(v.begin(), v.back());
        v.emplace(v.begin() + 1, v[0]);
        v.insert(v.begin(), 3, v[1]);
        for (const Tracked& t : v) EXPECT_EQ(t.value, 1);
        EXPECT_EQ(v.size(), 12u);
    }
    EXPECT_EQ(Tracked::alive(), 0);
}

TEST(Vector, AllocatorSemantics) {
    CountingAllocator A, B;
    Tracked::reset();
    {
        nvrhi::vector<Tracked> a((nvrhi::allocator(&A)));
        for (int i = 0; i < 5; ++i) a.emplace_back(i);
        EXPECT_GT(A.allocations, 0);

        // Copy construction carries the source's allocator.
        nvrhi::vector<Tracked> copy(a);
        EXPECT_EQ(copy.get_allocator().get_impl(), &A);

        // Copy assignment keeps the target's allocator.
        nvrhi::vector<Tracked> b((nvrhi::allocator(&B)));
        b = a;
        EXPECT_EQ(b.get_allocator().get_impl(), &B);
        EXPECT_EQ(Values(b), Values(a));
        EXPECT_GT(B.allocations, 0);

        // Move construction moves the allocator and steals the buffer.
        const Tracked* buffer = copy.data();
        nvrhi::vector<Tracked> stolen(std::move(copy));
        EXPECT_EQ(stolen.data(), buffer);
        EXPECT_EQ(stolen.get_allocator().get_impl(), &A);

        // Move assignment, equal allocators: the buffer moves.
        nvrhi::vector<Tracked> a2((nvrhi::allocator(&A)));
        a2.emplace_back(99);
        buffer = stolen.data();
        a2 = std::move(stolen);
        EXPECT_EQ(a2.data(), buffer);
        EXPECT_EQ(a2.size(), 5u);

        // Move assignment, unequal allocators: element by element into the target's own storage.
        nvrhi::vector<Tracked> b2((nvrhi::allocator(&B)));
        buffer = a2.data();
        b2 = std::move(a2);
        EXPECT_NE(b2.data(), buffer);
        EXPECT_EQ(b2.get_allocator().get_impl(), &B);
        EXPECT_EQ(Values(b2), (std::vector<int>{0, 1, 2, 3, 4}));
        EXPECT_TRUE(a2.empty());
        EXPECT_EQ(a2.get_allocator().get_impl(), &A);

        // swap, equal allocators: buffers are exchanged.
        nvrhi::vector<Tracked> a3((nvrhi::allocator(&A)));
        a3.emplace_back(7);
        const Tracked* aBuffer = a.data();
        const Tracked* a3Buffer = a3.data();
        a.swap(a3);
        EXPECT_EQ(a.data(), a3Buffer);
        EXPECT_EQ(a3.data(), aBuffer);

        // swap, unequal allocators: contents are exchanged, each keeps its allocator.
        a.swap(b2);
        EXPECT_EQ(Values(a), (std::vector<int>{0, 1, 2, 3, 4}));
        EXPECT_EQ(Values(b2), (std::vector<int>{7}));
        EXPECT_EQ(a.get_allocator().get_impl(), &A);
        EXPECT_EQ(b2.get_allocator().get_impl(), &B);

        // Move construction with another allocator.
        nvrhi::vector<Tracked> b3(std::move(a), nvrhi::allocator(&B));
        EXPECT_EQ(b3.get_allocator().get_impl(), &B);
        EXPECT_EQ(b3.size(), 5u);

        // A default allocator is the process-wide one.
        nvrhi::vector<int> d1, d2;
        EXPECT_EQ(d1.get_allocator(), d2.get_allocator());
        EXPECT_EQ(d1.get_allocator().get_impl(), nvrhi::GetDefaultMemAllocator());
    }
    EXPECT_EQ(Tracked::alive(), 0);
    EXPECT_EQ(A.live(), 0u);
    EXPECT_EQ(B.live(), 0u);
    EXPECT_EQ(A.allocations, A.frees);
    EXPECT_EQ(B.allocations, B.frees);
    EXPECT_EQ(A.foreignFrees, 0);
    EXPECT_EQ(B.foreignFrees, 0);
}

TEST(Vector, OverAlignedElements) {
    struct alignas(64) Wide {
        float v[16];
    };
    CountingAllocator A;
    {
        nvrhi::vector<Wide> v((nvrhi::allocator(&A)));
        for (int i = 0; i < 9; ++i) v.push_back(Wide{});
        for (const Wide& w : v) EXPECT_EQ(reinterpret_cast<uintptr_t>(&w) % 64, 0u);
    }
    EXPECT_EQ(A.live(), 0u);
    EXPECT_EQ(A.foreignFrees, 0);
}

// ---- string -------------------------------------------------------------------------------------------------

TEST(String, SmallStringBoundary) {
    CountingAllocator A;
    {
        const std::string s23(23, 'a');
        const std::string s24(24, 'b');

        nvrhi::string a(s23.c_str(), nvrhi::allocator(&A));
        EXPECT_EQ(a.size(), 23u);
        EXPECT_EQ(a.capacity(), 23u);
        EXPECT_EQ(A.allocations, 0);  // inline
        EXPECT_EQ(std::strlen(a.c_str()), 23u);
        EXPECT_EQ(a, s23);

        nvrhi::string b(s24.c_str(), nvrhi::allocator(&A));
        EXPECT_EQ(b.size(), 24u);
        EXPECT_EQ(A.allocations, 1);  // heap
        EXPECT_EQ(std::strlen(b.c_str()), 24u);
        EXPECT_EQ(b, s24);

        a.push_back('a');  // 23 -> 24: moves to the heap
        EXPECT_EQ(A.allocations, 2);
        EXPECT_EQ(a.size(), 24u);
        EXPECT_EQ(std::string(a.c_str()), std::string(24, 'a'));

        a.resize(5);
        a.shrink_to_fit();  // back inline
        EXPECT_EQ(A.live(), 1u);
        EXPECT_EQ(a, "aaaaa");
        EXPECT_EQ(a.capacity(), 23u);

        nvrhi::string empty;
        EXPECT_TRUE(empty.empty());
        EXPECT_STREQ(empty.c_str(), "");
    }
    EXPECT_EQ(A.live(), 0u);
    EXPECT_EQ(A.foreignFrees, 0);
}

TEST(String, Operations) {
    nvrhi::string s = "Hello";
    s += ", ";
    s += std::string("world");
    s += '!';
    EXPECT_EQ(s, "Hello, world!");
    EXPECT_EQ(s.length(), 13u);

    // StringLike: std::string and std::string_view, without nvrhi including <string>.
    const std::string stdString = "texture";
    nvrhi::string fromStd = stdString;
    nvrhi::string fromView = std::string_view("texture");
    EXPECT_EQ(fromStd, stdString);
    EXPECT_EQ(stdString, fromStd);
    EXPECT_EQ(fromView, fromStd);
    fromStd = std::string("buffer");
    EXPECT_EQ(fromStd, "buffer");

    EXPECT_EQ(s.find("world"), 7u);
    EXPECT_EQ(s.find('o'), 4u);
    EXPECT_EQ(s.rfind('o'), 8u);
    EXPECT_EQ(s.find("xyz"), nvrhi::string::npos);
    EXPECT_EQ(s.substr(7, 5), "world");
    EXPECT_EQ(s.substr(7), "world!");

    EXPECT_LT(nvrhi::string("abc"), nvrhi::string("abd"));
    EXPECT_LT(nvrhi::string("ab"), "abc");
    EXPECT_GT("b", nvrhi::string("abc"));
    EXPECT_NE(nvrhi::string("abc"), "abcd");

    const nvrhi::string a = "debug";
    EXPECT_EQ(a + "Name", "debugName");
    EXPECT_EQ("my" + a, "mydebug");
    EXPECT_EQ(a + nvrhi::string("Name"), "debugName");
    EXPECT_EQ(a + '!', "debug!");

    std::ostringstream os;
    os << "[" << a << "]";
    EXPECT_EQ(os.str(), "[debug]");

    std::unordered_set<nvrhi::string> set;
    set.insert("one");
    set.insert(nvrhi::string("one"));
    set.insert("two");
    EXPECT_EQ(set.size(), 2u);
    EXPECT_EQ(std::hash<nvrhi::string>()(nvrhi::string("x")), std::hash<nvrhi::string>()(nvrhi::string("x")));

    // Appending a part of itself, across the move to the heap.
    nvrhi::string self = "0123456789";
    self.append(self.c_str(), self.size());
    self.append(self.c_str(), self.size());
    EXPECT_EQ(self, "0123456789012345678901234567890123456789");
    self.assign(self.c_str() + 30, 10);
    EXPECT_EQ(self, "0123456789");
}

TEST(String, AllocatorSemantics) {
    CountingAllocator A, B;
    {
        const char* text = "a string that does not fit in the inline buffer";
        nvrhi::string a(text, nvrhi::allocator(&A));

        nvrhi::string copy(a);
        EXPECT_EQ(copy.get_allocator().get_impl(), &A);

        nvrhi::string b((nvrhi::allocator(&B)));
        b = a;
        EXPECT_EQ(b.get_allocator().get_impl(), &B);
        EXPECT_EQ(b, text);

        const char* buffer = copy.data();
        nvrhi::string stolen(std::move(copy));
        EXPECT_EQ(stolen.data(), buffer);
        EXPECT_TRUE(copy.empty());

        nvrhi::string a2((nvrhi::allocator(&A)));
        a2 = std::move(stolen);  // equal: the buffer moves
        EXPECT_EQ(a2.data(), buffer);

        nvrhi::string b2((nvrhi::allocator(&B)));
        b2 = std::move(a2);  // unequal: copied into B, a2 emptied
        EXPECT_NE(b2.data(), buffer);
        EXPECT_EQ(b2, text);
        EXPECT_TRUE(a2.empty());

        a.swap(b2);  // unequal: contents exchanged, allocators kept
        EXPECT_EQ(a.get_allocator().get_impl(), &A);
        EXPECT_EQ(b2.get_allocator().get_impl(), &B);
        EXPECT_EQ(a, text);
    }
    EXPECT_EQ(A.live(), 0u);
    EXPECT_EQ(B.live(), 0u);
    EXPECT_EQ(A.foreignFrees, 0);
    EXPECT_EQ(B.foreignFrees, 0);
}

// ---- fixed_vector -------------------------------------------------------------------------------------------

TEST(FixedVector, InlineBufferAndOverflow) {
    CountingAllocator A;
    Tracked::reset();
    {
        using FV = nvrhi::fixed_vector<Tracked, 4>;
        FV v((nvrhi::allocator(&A)));
        for (int i = 0; i < 4; ++i) v.emplace_back(i);
        EXPECT_TRUE(v.full());
        EXPECT_FALSE(v.has_overflowed());
        EXPECT_EQ(A.allocations, 0);

        v.emplace_back(4);  // into the heap
        EXPECT_TRUE(v.has_overflowed());
        EXPECT_EQ(A.allocations, 1);
        EXPECT_EQ(Values(v), (std::vector<int>{0, 1, 2, 3, 4}));

        // A copy starts in its own inline buffer; this one overflows into the copied overflow allocator.
        FV copy(v);
        EXPECT_TRUE(copy.has_overflowed());
        EXPECT_EQ(copy.get_overflow_allocator().get_impl(), &A);
        EXPECT_EQ(Values(copy), Values(v));

        // A move from an overflowed vector steals its heap block; the source returns to its buffer.
        const Tracked* heap = v.data();
        FV moved(std::move(v));
        EXPECT_EQ(moved.data(), heap);
        EXPECT_TRUE(v.empty());
        EXPECT_FALSE(v.has_overflowed());

        // Back from the heap into the inline buffer.
        moved.pop_back();
        moved.shrink_to_fit();
        EXPECT_FALSE(moved.has_overflowed());
        EXPECT_EQ(Values(moved), (std::vector<int>{0, 1, 2, 3}));

        // A move from an inline vector moves the elements into the target's own buffer.
        FV inlineMoved(std::move(moved));
        EXPECT_FALSE(inlineMoved.has_overflowed());
        EXPECT_NE(inlineMoved.data(), moved.data());
        EXPECT_EQ(Values(inlineMoved), (std::vector<int>{0, 1, 2, 3}));

        // Copy assignment of a small vector into an overflowed one, then swap.
        copy = inlineMoved;
        EXPECT_EQ(Values(copy), (std::vector<int>{0, 1, 2, 3}));
        FV other;
        other.emplace_back(9);
        other.swap(copy);
        EXPECT_EQ(Values(other), (std::vector<int>{0, 1, 2, 3}));
        EXPECT_EQ(Values(copy), (std::vector<int>{9}));

        // Move assignment from an overflowed vector with an equal overflow allocator steals the block.
        FV big((nvrhi::allocator(&A)));
        for (int i = 0; i < 6; ++i) big.emplace_back(i);
        const Tracked* bigHeap = big.data();
        FV target((nvrhi::allocator(&A)));
        target.emplace_back(1);
        target = std::move(big);
        EXPECT_EQ(target.data(), bigHeap);
        EXPECT_EQ(target.size(), 6u);
        EXPECT_FALSE(big.has_overflowed());
    }
    EXPECT_EQ(Tracked::alive(), 0);
    EXPECT_EQ(A.live(), 0u);
    EXPECT_EQ(A.foreignFrees, 0);
}

TEST(FixedVector, OverflowDisabled) {
    nvrhi::fixed_vector<int, 3, false> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    EXPECT_TRUE(v.full());
    EXPECT_EQ(v.max_size(), 3u);
    EXPECT_FALSE(v.has_overflowed());
    v.erase(v.begin());
    v.insert(v.begin(), 0);
    EXPECT_EQ(Ints(v), (std::vector<int>{0, 2, 3}));
    nvrhi::fixed_vector<int, 3, false> copy = v;
    EXPECT_EQ(Ints(copy), Ints(v));
    EXPECT_NE(copy.data(), v.data());
}

// ---- static_vector ------------------------------------------------------------------------------------------

TEST(StaticVector, Api) {
    nvrhi::static_vector<int, 4> v;
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.max_size(), 4u);
    v.push_back(1);
    v.push_back(2);
    v.emplace_back() = 3;
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v.back(), 3);
    EXPECT_EQ(v.front(), 1);
    EXPECT_EQ(Ints(v), (std::vector<int>{1, 2, 3}));
    v.pop_back();
    EXPECT_EQ(v.size(), 2u);
    v.resize(4);
    EXPECT_EQ(Ints(v), (std::vector<int>{1, 2, 0, 0}));
    v.resize(1);
    v.resize(2);
    EXPECT_EQ(v[1], 0);  // dropped elements are reset

    nvrhi::static_vector<int, 4> il = {5, 6};
    il.swap(v);
    EXPECT_EQ(Ints(v), (std::vector<int>{5, 6}));
    EXPECT_EQ(Ints(il), (std::vector<int>{1, 0}));

    nvrhi::static_vector<int, 4> filled;
    filled.fill(7);
    EXPECT_EQ(Ints(filled), (std::vector<int>{7, 7, 7, 7}));
    EXPECT_EQ(int(nvrhi::static_vector<int, 4>::max_elements), 4);

    nvrhi::static_vector<int, 4> sized(3);
    EXPECT_EQ(Ints(sized), (std::vector<int>{0, 0, 0}));

    int sum = 0;
    for (int x : filled) sum += x;
    EXPECT_EQ(sum, 28);
}
