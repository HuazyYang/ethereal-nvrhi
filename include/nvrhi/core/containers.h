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

#pragma once

// Containers with a fixed layout, for the public API: their layout depends on neither the compiler's standard
// library nor its debug settings, so they may cross a module boundary (nvrhi.dll and its clients, MSVC and
// MinGW). Header only, modelled on EASTL (vector.h, string.h, fixed_vector.h), with no dependency on it.
//
// - No exceptions: running out of memory is reported through NVRHI_VERIFY.
// - No debug-only members, no [[no_unique_address]]: the sizes are asserted below.
// - Stateful allocators, with EASTL's mechanics. A container stores its allocator next to its pointers and
//   frees through it:
//   - copy construction copies the source's allocator; copy assignment keeps the target's own;
//   - move construction moves the allocator and steals the buffer;
//   - move assignment and swap exchange buffers only when the two allocators compare equal, and otherwise
//     move the elements one by one, each container into storage from its own allocator.
//   nvrhi::allocator (the default) frees through an IMemoryAllocator. By default that is the process-wide
//   allocator of nvrhi_core, the same in every module, so default-allocated containers compare equal
//   everywhere and may be freed by any module. A custom IMemoryAllocator must outlive every container that
//   uses it, copies included.

#include <nvrhi/core/memory.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <new>
#include <type_traits>
#include <utility>

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#error "nvrhi::string: the small-string layout assumes a little-endian target"
#endif

namespace nvrhi {

// ---- allocator ----------------------------------------------------------------------------------------------

/// The default container allocator: allocates and frees through an IMemoryAllocator, by default the
/// process-wide one (GetDefaultMemAllocator()). Allocators compare equal when they use the same
/// IMemoryAllocator.
class allocator {
 public:
    allocator() noexcept : m_impl(GetDefaultMemAllocator()) {}
    explicit allocator(IMemoryAllocator* impl) noexcept : m_impl(impl ? impl : GetDefaultMemAllocator()) {}

    void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) noexcept {
        void* p = alignment > alignof(std::max_align_t) ? m_impl->AllocateAligned(bytes, alignment)
                                                        : m_impl->Allocate(bytes);
        NVRHI_VERIFY(p != nullptr, "nvrhi::allocator: out of memory");
        return p;
    }

    void deallocate(void* p, size_t /*bytes*/, size_t alignment = alignof(std::max_align_t)) noexcept {
        if (!p) return;
        if (alignment > alignof(std::max_align_t))
            m_impl->FreeAligned(p);
        else
            m_impl->Free(p);
    }

    IMemoryAllocator* get_impl() const noexcept { return m_impl; }
    void set_impl(IMemoryAllocator* impl) noexcept { m_impl = impl ? impl : GetDefaultMemAllocator(); }

    friend bool operator==(const allocator& a, const allocator& b) noexcept { return a.m_impl == b.m_impl; }
    friend bool operator!=(const allocator& a, const allocator& b) noexcept { return a.m_impl != b.m_impl; }

 private:
    IMemoryAllocator* m_impl;
};

static_assert(sizeof(allocator) == sizeof(void*), "nvrhi::allocator is one pointer");

namespace details {

template <typename T>
void destroy_range(T* first, T* last) noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
        for (; first != last; ++first) first->~T();
    }
}

// Move-constructs [first, last) into the uninitialized, non-overlapping storage at dest; the source keeps
// its (moved-from) elements. Returns the end of the destination.
template <typename T>
T* uninitialized_move(T* first, T* last, T* dest) {
    if constexpr (std::is_trivially_copyable_v<T>) {
        const size_t n = size_t(last - first);
        if (n) std::memcpy(static_cast<void*>(dest), static_cast<const void*>(first), n * sizeof(T));
        return dest + n;
    } else {
        for (; first != last; ++first, ++dest) ::new (static_cast<void*>(dest)) T(std::move(*first));
        return dest;
    }
}

// Moves [first, last) into the uninitialized storage at dest and destroys the source elements.
template <typename T>
T* relocate(T* first, T* last, T* dest) {
    T* result = details::uninitialized_move(first, last, dest);
    details::destroy_range(first, last);
    return result;
}

// An iterator over n copies of one value: insert(pos, n, value) and assign(n, value) run the range code.
template <typename T>
struct repeat_iterator {
    const T* value;
    size_t index;

    const T& operator*() const noexcept { return *value; }
    repeat_iterator& operator++() noexcept { ++index; return *this; }
    bool operator==(const repeat_iterator& other) const noexcept { return index == other.index; }
    bool operator!=(const repeat_iterator& other) const noexcept { return index != other.index; }
};

// The number of elements in [first, last). Iterator ranges must be forward ranges (walked twice).
template <typename It>
size_t range_distance(It first, It last) {
    if constexpr (std::is_pointer_v<It>) {
        return size_t(last - first);
    } else {
        size_t n = 0;
        for (; first != last; ++first) ++n;
        return n;
    }
}

template <typename It>
using enable_if_iterator_t = std::enable_if_t<!std::is_integral_v<It>, int>;

}  // namespace details

// ---- vector -------------------------------------------------------------------------------------------------

/// A dynamic array (reference: EASTL vector). Layout: { T* begin; T* end; T* capacity; Allocator }.
/// Iterators are raw pointers.
template <typename T, typename Allocator = allocator>
class vector {
 public:
    using value_type = T;
    using allocator_type = Allocator;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;

    vector() noexcept : mAllocator() {}
    explicit vector(const allocator_type& alloc) noexcept : mAllocator(alloc) {}

    explicit vector(size_type n, const allocator_type& alloc = allocator_type()) : mAllocator(alloc) { resize(n); }

    vector(size_type n, const T& value, const allocator_type& alloc = allocator_type()) : mAllocator(alloc) {
        assign(n, value);
    }

    template <typename InputIt, details::enable_if_iterator_t<InputIt> = 0>
    vector(InputIt first, InputIt last, const allocator_type& alloc = allocator_type()) : mAllocator(alloc) {
        assign(first, last);
    }

    vector(std::initializer_list<T> il, const allocator_type& alloc = allocator_type()) : mAllocator(alloc) {
        assign(il.begin(), il.end());
    }

    // EASTL: the copy uses the source's allocator.
    vector(const vector& x) : mAllocator(x.mAllocator) { assign(x.mpBegin, x.mpEnd); }
    vector(const vector& x, const allocator_type& alloc) : mAllocator(alloc) { assign(x.mpBegin, x.mpEnd); }

    vector(vector&& x) noexcept : mAllocator(std::move(x.mAllocator)) { DoStealBuffer(x); }

    vector(vector&& x, const allocator_type& alloc) : mAllocator(alloc) {
        if (mAllocator == x.mAllocator)
            DoStealBuffer(x);
        else
            DoAssignMove(x);
    }

    ~vector() { DoFreeAll(); }

    // Copy assignment keeps this container's allocator (EASTL_ALLOCATOR_COPY_ENABLED == 0).
    vector& operator=(const vector& x) {
        if (this != &x) assign(x.mpBegin, x.mpEnd);
        return *this;
    }

    vector& operator=(vector&& x) {
        if (this != &x) {
            if (mAllocator == x.mAllocator) {
                DoFreeAll();
                DoStealBuffer(x);
            } else {
                DoAssignMove(x);
            }
        }
        return *this;
    }

    vector& operator=(std::initializer_list<T> il) {
        assign(il.begin(), il.end());
        return *this;
    }

    void assign(size_type n, const T& value) {
        const T copy(value);  // value may be an element of this vector
        DoAssignRange(details::repeat_iterator<T>{&copy, 0}, details::repeat_iterator<T>{&copy, n}, n);
    }

    template <typename InputIt, details::enable_if_iterator_t<InputIt> = 0>
    void assign(InputIt first, InputIt last) {
        DoAssignRange(first, last, details::range_distance(first, last));
    }

    void assign(std::initializer_list<T> il) { DoAssignRange(il.begin(), il.end(), il.size()); }

    const allocator_type& get_allocator() const noexcept { return mAllocator; }
    allocator_type& get_allocator() noexcept { return mAllocator; }

    // The allocator may change while the vector holds no memory (or to an equal allocator).
    void set_allocator(const allocator_type& alloc) {
        NVRHI_ASSERT(mpCapacity == mpBegin || mAllocator == alloc);
        mAllocator = alloc;
    }

    iterator begin() noexcept { return mpBegin; }
    const_iterator begin() const noexcept { return mpBegin; }
    const_iterator cbegin() const noexcept { return mpBegin; }
    iterator end() noexcept { return mpEnd; }
    const_iterator end() const noexcept { return mpEnd; }
    const_iterator cend() const noexcept { return mpEnd; }

    bool empty() const noexcept { return mpBegin == mpEnd; }
    size_type size() const noexcept { return size_type(mpEnd - mpBegin); }
    size_type capacity() const noexcept { return size_type(mpCapacity - mpBegin); }
    size_type max_size() const noexcept { return size_type(-1) / sizeof(T); }

    void reserve(size_type n) {
        if (n > capacity()) DoRealloc(n);
    }

    void resize(size_type n) {
        const size_type sz = size();
        if (n > sz) {
            if (n > capacity()) DoRealloc(DoGrowCapacity(n));
            for (T* p = mpEnd, *e = mpBegin + n; p != e; ++p) ::new (static_cast<void*>(p)) T();
            mpEnd = mpBegin + n;
        } else {
            details::destroy_range(mpBegin + n, mpEnd);
            mpEnd = mpBegin + n;
        }
    }

    void resize(size_type n, const T& value) {
        const size_type sz = size();
        if (n > sz)
            insert(mpEnd, n - sz, value);
        else
            erase(mpBegin + n, mpEnd);
    }

    void shrink_to_fit() {
        if (mpCapacity == mpEnd) return;
        if (mpBegin == mpEnd) {
            DoFreeAll();
            mpBegin = mpEnd = mpCapacity = nullptr;
        } else {
            DoRealloc(size());
        }
    }

    void clear() noexcept {
        details::destroy_range(mpBegin, mpEnd);
        mpEnd = mpBegin;
    }

    T* data() noexcept { return mpBegin; }
    const T* data() const noexcept { return mpBegin; }

    reference operator[](size_type i) noexcept {
        NVRHI_ASSERT(i < size());
        return mpBegin[i];
    }
    const_reference operator[](size_type i) const noexcept {
        NVRHI_ASSERT(i < size());
        return mpBegin[i];
    }

    // No exceptions: an index out of range asserts, as operator[] does.
    reference at(size_type i) noexcept { return (*this)[i]; }
    const_reference at(size_type i) const noexcept { return (*this)[i]; }

    reference front() noexcept { NVRHI_ASSERT(!empty()); return *mpBegin; }
    const_reference front() const noexcept { NVRHI_ASSERT(!empty()); return *mpBegin; }
    reference back() noexcept { NVRHI_ASSERT(!empty()); return mpEnd[-1]; }
    const_reference back() const noexcept { NVRHI_ASSERT(!empty()); return mpEnd[-1]; }

    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(std::move(value)); }

    template <typename... Args>
    reference emplace_back(Args&&... args) {
        if (mpEnd != mpCapacity) {
            ::new (static_cast<void*>(mpEnd)) T(std::forward<Args>(args)...);
            ++mpEnd;
        } else {
            DoReallocInsert(size(), std::forward<Args>(args)...);
        }
        return mpEnd[-1];
    }

    void pop_back() noexcept {
        NVRHI_ASSERT(!empty());
        --mpEnd;
        mpEnd->~T();
    }

    template <typename... Args>
    iterator emplace(const_iterator position, Args&&... args) {
        NVRHI_ASSERT(position >= mpBegin && position <= mpEnd);
        const size_type index = size_type(position - mpBegin);
        if (mpEnd == mpCapacity) {
            DoReallocInsert(index, std::forward<Args>(args)...);
        } else if (position == mpEnd) {
            ::new (static_cast<void*>(mpEnd)) T(std::forward<Args>(args)...);
            ++mpEnd;
        } else {
            T value(std::forward<Args>(args)...);  // first: the arguments may refer to elements
            T* pos = mpBegin + index;
            ::new (static_cast<void*>(mpEnd)) T(std::move(mpEnd[-1]));
            ++mpEnd;
            for (T* p = mpEnd - 2; p != pos; --p) *p = std::move(p[-1]);
            *pos = std::move(value);
        }
        return mpBegin + index;
    }

    iterator insert(const_iterator position, const T& value) { return emplace(position, value); }
    iterator insert(const_iterator position, T&& value) { return emplace(position, std::move(value)); }

    iterator insert(const_iterator position, size_type n, const T& value) {
        const T copy(value);  // value may be an element of this vector
        return DoInsertRange(position, details::repeat_iterator<T>{&copy, 0}, details::repeat_iterator<T>{&copy, n},
                             n);
    }

    template <typename InputIt, details::enable_if_iterator_t<InputIt> = 0>
    iterator insert(const_iterator position, InputIt first, InputIt last) {
        return DoInsertRange(position, first, last, details::range_distance(first, last));
    }

    iterator insert(const_iterator position, std::initializer_list<T> il) {
        return DoInsertRange(position, il.begin(), il.end(), il.size());
    }

    iterator erase(const_iterator position) {
        NVRHI_ASSERT(position >= mpBegin && position < mpEnd);
        T* pos = mpBegin + (position - mpBegin);
        for (T* p = pos + 1; p != mpEnd; ++p) p[-1] = std::move(*p);
        --mpEnd;
        mpEnd->~T();
        return pos;
    }

    iterator erase(const_iterator first, const_iterator last) {
        NVRHI_ASSERT(first >= mpBegin && first <= last && last <= mpEnd);
        T* dst = mpBegin + (first - mpBegin);
        T* src = mpBegin + (last - mpBegin);
        if (dst != src) {
            T* out = dst;
            for (; src != mpEnd; ++src, ++out) *out = std::move(*src);
            details::destroy_range(out, mpEnd);
            mpEnd = out;
        }
        return dst;
    }

    void swap(vector& x) {
        if (this == &x) return;
        if (mAllocator == x.mAllocator) {
            std::swap(mpBegin, x.mpBegin);
            std::swap(mpEnd, x.mpEnd);
            std::swap(mpCapacity, x.mpCapacity);
        } else {
            // Each container keeps its allocator and moves the other's elements into its own storage.
            vector temp(mAllocator);
            temp.DoAssignMove(*this);
            DoAssignMove(x);
            x.DoAssignMove(temp);
        }
    }

    friend bool operator==(const vector& a, const vector& b) {
        if (a.size() != b.size()) return false;
        for (size_type i = 0, n = a.size(); i < n; ++i)
            if (!(a.mpBegin[i] == b.mpBegin[i])) return false;
        return true;
    }
    friend bool operator!=(const vector& a, const vector& b) { return !(a == b); }

 protected:
    T* DoAllocate(size_type n) { return static_cast<T*>(mAllocator.allocate(n * sizeof(T), alignof(T))); }

    void DoFree(T* p, size_type n) noexcept {
        if (p) mAllocator.deallocate(p, n * sizeof(T), alignof(T));
    }

    // Destroys the elements and frees the buffer; the pointers are left dangling.
    void DoFreeAll() noexcept {
        details::destroy_range(mpBegin, mpEnd);
        DoFree(mpBegin, capacity());
    }

    void DoStealBuffer(vector& x) noexcept {
        mpBegin = x.mpBegin;
        mpEnd = x.mpEnd;
        mpCapacity = x.mpCapacity;
        x.mpBegin = x.mpEnd = x.mpCapacity = nullptr;
    }

    size_type DoGrowCapacity(size_type needed) const noexcept {
        const size_type cap = capacity();
        const size_type grown = cap ? 2 * cap : 1;
        return grown < needed ? needed : grown;
    }

    // Moves the elements into a new buffer of newCapacity (>= size()).
    void DoRealloc(size_type newCapacity) {
        T* newBegin = DoAllocate(newCapacity);
        T* newEnd = details::relocate(mpBegin, mpEnd, newBegin);
        DoFree(mpBegin, capacity());
        mpBegin = newBegin;
        mpEnd = newEnd;
        mpCapacity = newBegin + newCapacity;
    }

    // Inserts one element at index into a new buffer. The new element is constructed before the old ones
    // move, so the arguments may refer to elements of this vector.
    template <typename... Args>
    void DoReallocInsert(size_type index, Args&&... args) {
        const size_type newCapacity = DoGrowCapacity(size() + 1);
        T* newBegin = DoAllocate(newCapacity);
        ::new (static_cast<void*>(newBegin + index)) T(std::forward<Args>(args)...);
        details::relocate(mpBegin, mpBegin + index, newBegin);
        T* newEnd = details::relocate(mpBegin + index, mpEnd, newBegin + index + 1);
        DoFree(mpBegin, capacity());
        mpBegin = newBegin;
        mpEnd = newEnd;
        mpCapacity = newBegin + newCapacity;
    }

    template <typename It>
    void DoAssignRange(It first, It last, size_type n) {
        if (n > capacity()) {
            T* newBegin = DoAllocate(n);
            T* p = newBegin;
            for (; first != last; ++first, ++p) ::new (static_cast<void*>(p)) T(*first);
            DoFreeAll();
            mpBegin = newBegin;
            mpEnd = mpCapacity = newBegin + n;
            return;
        }
        T* p = mpBegin;
        for (; first != last && p != mpEnd; ++first, ++p) *p = *first;
        if (first != last) {
            for (; first != last; ++first, ++p) ::new (static_cast<void*>(p)) T(*first);
        } else {
            details::destroy_range(p, mpEnd);
        }
        mpEnd = p;
    }

    // Replaces the contents with x's elements, moved one by one into this vector's storage; x is left empty
    // with its buffer (and allocator) unchanged.
    void DoAssignMove(vector& x) {
        const size_type n = x.size();
        if (n > capacity()) {
            T* newBegin = DoAllocate(n);
            details::uninitialized_move(x.mpBegin, x.mpEnd, newBegin);
            DoFreeAll();
            mpBegin = newBegin;
            mpEnd = mpCapacity = newBegin + n;
        } else {
            T* p = mpBegin;
            T* src = x.mpBegin;
            for (; src != x.mpEnd && p != mpEnd; ++src, ++p) *p = std::move(*src);
            if (src != x.mpEnd)
                p = details::uninitialized_move(src, x.mpEnd, p);
            else
                details::destroy_range(p, mpEnd);
            mpEnd = p;
        }
        x.clear();
    }

    template <typename It>
    iterator DoInsertRange(const_iterator position, It first, It last, size_type n) {
        NVRHI_ASSERT(position >= mpBegin && position <= mpEnd);
        const size_type index = size_type(position - mpBegin);
        if (n == 0) return mpBegin + index;

        if (size() + n > capacity()) {
            const size_type newCapacity = DoGrowCapacity(size() + n);
            T* newBegin = DoAllocate(newCapacity);
            T* p = newBegin + index;
            for (; first != last; ++first, ++p) ::new (static_cast<void*>(p)) T(*first);
            details::relocate(mpBegin, mpBegin + index, newBegin);
            T* newEnd = details::relocate(mpBegin + index, mpEnd, p);
            DoFree(mpBegin, capacity());
            mpBegin = newBegin;
            mpEnd = newEnd;
            mpCapacity = newBegin + newCapacity;
            return mpBegin + index;
        }

        T* pos = mpBegin + index;
        T* oldEnd = mpEnd;
        const size_type after = size_type(oldEnd - pos);
        if (after > n) {
            // The last n elements move into uninitialized storage; the rest shift up; the range is assigned.
            mpEnd = details::uninitialized_move(oldEnd - n, oldEnd, oldEnd);
            for (T* p = oldEnd - n; p != pos;) {
                --p;
                p[n] = std::move(*p);
            }
            for (T* p = pos; first != last; ++first, ++p) *p = *first;
        } else {
            // The part of the range past the old end is constructed there, then the tail moves after it.
            It mid = first;
            for (size_type i = 0; i < after; ++i) ++mid;
            T* p = oldEnd;
            for (It it = mid; it != last; ++it, ++p) ::new (static_cast<void*>(p)) T(*it);
            mpEnd = details::uninitialized_move(pos, oldEnd, p);
            for (T* q = pos; first != mid; ++first, ++q) *q = *first;
        }
        return pos;
    }

    T* mpBegin = nullptr;
    T* mpEnd = nullptr;
    T* mpCapacity = nullptr;
    Allocator mAllocator;
};

static_assert(sizeof(vector<uint32_t>) == 4 * sizeof(void*), "nvrhi::vector: three pointers and the allocator");

template <typename T, typename Allocator>
void swap(vector<T, Allocator>& a, vector<T, Allocator>& b) {
    a.swap(b);
}

// ---- string -------------------------------------------------------------------------------------------------

class string;

namespace details {
// A string-like type: data() convertible to const char* and size(), e.g. std::string and std::string_view
// (accepted without including <string>).
template <typename S, typename = void>
struct is_string_like : std::false_type {};
template <typename S>
struct is_string_like<S, std::void_t<decltype(std::declval<const S&>().data()),
                                     decltype(std::declval<const S&>().size())>>
    : std::bool_constant<std::is_convertible_v<decltype(std::declval<const S&>().data()), const char*> &&
                         !std::is_same_v<std::remove_cv_t<S>, nvrhi::string>> {};
template <typename S>
inline constexpr bool is_string_like_v = is_string_like<S>::value;
template <typename S>
using enable_if_string_like_t = std::enable_if_t<is_string_like_v<S>, int>;
}  // namespace details

/// A char string (reference: EASTL basic_string<char>). Layout: a 24-byte union, then the allocator.
/// - Short strings (up to 23 chars) are stored inline. The last byte holds 23 - size, so it is zero (the
///   terminator) when the string is full.
/// - Longer strings use { char* begin; size_t size; size_t capacity | heap flag }. The heap flag is the top
///   bit of the capacity, which is the top bit of the union's last byte on a little-endian target.
class string {
 public:
    using value_type = char;
    using allocator_type = allocator;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using reference = char&;
    using const_reference = const char&;
    using pointer = char*;
    using const_pointer = const char*;
    using iterator = char*;
    using const_iterator = const char*;

    static constexpr size_type npos = size_type(-1);

    string() noexcept : mAllocator() { DoSetSSOSize(0); }
    explicit string(const allocator_type& alloc) noexcept : mAllocator(alloc) { DoSetSSOSize(0); }

    string(const char* s, const allocator_type& alloc = allocator_type()) : mAllocator(alloc) {
        DoSetSSOSize(0);
        assign(s);
    }
    string(const char* s, size_type n, const allocator_type& alloc = allocator_type()) : mAllocator(alloc) {
        DoSetSSOSize(0);
        assign(s, n);
    }
    string(size_type n, char c, const allocator_type& alloc = allocator_type()) : mAllocator(alloc) {
        DoSetSSOSize(0);
        assign(n, c);
    }

    // Implicit from any string-like type, e.g. std::string or std::string_view.
    template <typename S, details::enable_if_string_like_t<S> = 0>
    string(const S& s, const allocator_type& alloc = allocator_type()) : mAllocator(alloc) {
        DoSetSSOSize(0);
        assign(s.data(), size_type(s.size()));
    }

    // EASTL: the copy uses the source's allocator.
    string(const string& x) : mAllocator(x.mAllocator) {
        DoSetSSOSize(0);
        assign(x.data(), x.size());
    }
    string(const string& x, const allocator_type& alloc) : mAllocator(alloc) {
        DoSetSSOSize(0);
        assign(x.data(), x.size());
    }

    string(string&& x) noexcept : mAllocator(std::move(x.mAllocator)) {
        mRep = x.mRep;
        x.DoSetSSOSize(0);
    }
    string(string&& x, const allocator_type& alloc) : mAllocator(alloc) {
        DoSetSSOSize(0);
        *this = std::move(x);
    }

    ~string() { DoFreeHeap(); }

    // Copy assignment keeps this string's allocator.
    string& operator=(const string& x) {
        if (this != &x) assign(x.data(), x.size());
        return *this;
    }

    string& operator=(string&& x) {
        if (this != &x) {
            if (mAllocator == x.mAllocator) {
                DoFreeHeap();
                mRep = x.mRep;
                x.DoSetSSOSize(0);
            } else {
                assign(x.data(), x.size());
                x.clear();
            }
        }
        return *this;
    }

    string& operator=(const char* s) { return assign(s); }

    template <typename S, details::enable_if_string_like_t<S> = 0>
    string& operator=(const S& s) {
        return assign(s.data(), size_type(s.size()));
    }

    string& assign(const char* s, size_type n) {
        NVRHI_ASSERT(s || n == 0);
        if (n > capacity()) {
            char* p = DoAllocate(n);
            std::memcpy(p, s, n);
            p[n] = 0;
            DoFreeHeap();
            DoSetHeap(p, n, n);
        } else {
            if (n) std::memmove(DoData(), s, n);  // s may point into this string
            DoSetSize(n);
        }
        return *this;
    }
    string& assign(const char* s) { return assign(s, s ? std::strlen(s) : 0); }
    string& assign(const string& x) { return this == &x ? *this : assign(x.data(), x.size()); }
    string& assign(size_type n, char c) {
        clear();
        return append(n, c);
    }

    const allocator_type& get_allocator() const noexcept { return mAllocator; }
    allocator_type& get_allocator() noexcept { return mAllocator; }

    // The allocator may change while the string holds no heap memory (or to an equal allocator).
    void set_allocator(const allocator_type& alloc) {
        NVRHI_ASSERT(!DoIsHeap() || mAllocator == alloc);
        mAllocator = alloc;
    }

    const char* c_str() const noexcept { return DoData(); }
    const char* data() const noexcept { return DoData(); }
    char* data() noexcept { return DoData(); }

    size_type size() const noexcept { return DoIsHeap() ? mRep.heap.size : SSOCapacity - DoLastByte(); }
    size_type length() const noexcept { return size(); }
    bool empty() const noexcept { return size() == 0; }
    size_type capacity() const noexcept { return DoIsHeap() ? (mRep.heap.capacity & ~HeapFlag) : SSOCapacity; }
    size_type max_size() const noexcept { return (~HeapFlag) - 1; }

    void reserve(size_type n) {
        if (n > capacity()) DoGrowTo(n);
    }

    void resize(size_type n, char c = 0) {
        const size_type sz = size();
        if (n > sz)
            append(n - sz, c);
        else
            DoSetSize(n);
    }

    void shrink_to_fit() {
        if (!DoIsHeap()) return;
        const size_type sz = size();
        if (sz <= SSOCapacity) {
            char* p = mRep.heap.begin;
            const size_type cap = capacity();
            std::memcpy(mRep.sso.data, p, sz);
            DoSetSSOSize(sz);
            mAllocator.deallocate(p, cap + 1);
        } else if (capacity() > sz) {
            char* p = DoAllocate(sz);
            std::memcpy(p, mRep.heap.begin, sz + 1);
            DoFreeHeap();
            DoSetHeap(p, sz, sz);
        }
    }

    void clear() noexcept { DoSetSize(0); }

    iterator begin() noexcept { return DoData(); }
    const_iterator begin() const noexcept { return DoData(); }
    const_iterator cbegin() const noexcept { return DoData(); }
    iterator end() noexcept { return DoData() + size(); }
    const_iterator end() const noexcept { return DoData() + size(); }
    const_iterator cend() const noexcept { return DoData() + size(); }

    char& operator[](size_type i) noexcept {
        NVRHI_ASSERT(i <= size());
        return DoData()[i];
    }
    const char& operator[](size_type i) const noexcept {
        NVRHI_ASSERT(i <= size());
        return DoData()[i];
    }

    char& front() noexcept { NVRHI_ASSERT(!empty()); return DoData()[0]; }
    const char& front() const noexcept { NVRHI_ASSERT(!empty()); return DoData()[0]; }
    char& back() noexcept { NVRHI_ASSERT(!empty()); return DoData()[size() - 1]; }
    const char& back() const noexcept { NVRHI_ASSERT(!empty()); return DoData()[size() - 1]; }

    string& append(const char* s, size_type n) {
        NVRHI_ASSERT(s || n == 0);
        if (n == 0) return *this;
        const size_type sz = size();
        if (sz + n > capacity()) {
            // Copy before freeing: s may point into this string.
            const size_type newCapacity = DoGrowCapacity(sz + n);
            char* p = DoAllocate(newCapacity);
            std::memcpy(p, DoData(), sz);
            std::memcpy(p + sz, s, n);
            p[sz + n] = 0;
            DoFreeHeap();
            DoSetHeap(p, sz + n, newCapacity);
        } else {
            std::memmove(DoData() + sz, s, n);
            DoSetSize(sz + n);
        }
        return *this;
    }
    string& append(const char* s) { return append(s, s ? std::strlen(s) : 0); }
    string& append(const string& x) { return append(x.data(), x.size()); }
    string& append(size_type n, char c) {
        if (n == 0) return *this;
        const size_type sz = size();
        if (sz + n > capacity()) DoGrowTo(DoGrowCapacity(sz + n));
        std::memset(DoData() + sz, c, n);
        DoSetSize(sz + n);
        return *this;
    }
    template <typename S, details::enable_if_string_like_t<S> = 0>
    string& append(const S& s) {
        return append(s.data(), size_type(s.size()));
    }

    void push_back(char c) { append(size_type(1), c); }
    void pop_back() noexcept {
        NVRHI_ASSERT(!empty());
        DoSetSize(size() - 1);
    }

    string& operator+=(const string& x) { return append(x); }
    string& operator+=(const char* s) { return append(s); }
    string& operator+=(char c) { return append(size_type(1), c); }
    template <typename S, details::enable_if_string_like_t<S> = 0>
    string& operator+=(const S& s) {
        return append(s.data(), size_type(s.size()));
    }

    int compare(const char* s, size_type n) const noexcept {
        const size_type sz = size();
        const size_type common = sz < n ? sz : n;
        const int r = common ? std::memcmp(DoData(), s, common) : 0;
        if (r != 0) return r;
        return sz < n ? -1 : (sz > n ? 1 : 0);
    }
    int compare(const string& x) const noexcept { return compare(x.data(), x.size()); }
    int compare(const char* s) const noexcept { return compare(s, s ? std::strlen(s) : 0); }
    template <typename S, details::enable_if_string_like_t<S> = 0>
    int compare(const S& s) const noexcept {
        return compare(s.data(), size_type(s.size()));
    }

    size_type find(const char* s, size_type pos, size_type n) const noexcept {
        const size_type sz = size();
        if (pos > sz || n > sz - pos) return npos;
        const char* d = DoData();
        for (size_type i = pos, last = sz - n; i <= last; ++i)
            if (n == 0 || std::memcmp(d + i, s, n) == 0) return i;
        return npos;
    }
    size_type find(const string& x, size_type pos = 0) const noexcept { return find(x.data(), pos, x.size()); }
    size_type find(const char* s, size_type pos = 0) const noexcept { return find(s, pos, std::strlen(s)); }
    size_type find(char c, size_type pos = 0) const noexcept { return find(&c, pos, 1); }

    size_type rfind(const char* s, size_type pos, size_type n) const noexcept {
        const size_type sz = size();
        if (n > sz) return npos;
        size_type i = sz - n;
        if (pos < i) i = pos;
        const char* d = DoData();
        for (;; --i) {
            if (n == 0 || std::memcmp(d + i, s, n) == 0) return i;
            if (i == 0) return npos;
        }
    }
    size_type rfind(const string& x, size_type pos = npos) const noexcept { return rfind(x.data(), pos, x.size()); }
    size_type rfind(const char* s, size_type pos = npos) const noexcept { return rfind(s, pos, std::strlen(s)); }
    size_type rfind(char c, size_type pos = npos) const noexcept { return rfind(&c, pos, 1); }

    // The substring uses this string's allocator.
    string substr(size_type pos = 0, size_type n = npos) const {
        const size_type sz = size();
        NVRHI_ASSERT(pos <= sz);
        if (pos > sz) pos = sz;
        if (n > sz - pos) n = sz - pos;
        return string(DoData() + pos, n, mAllocator);
    }

    void swap(string& x) {
        if (this == &x) return;
        if (mAllocator == x.mAllocator) {
            const Layout temp = mRep;
            mRep = x.mRep;
            x.mRep = temp;
        } else {
            string temp(std::move(*this), mAllocator);
            *this = std::move(x);
            x = std::move(temp);
        }
    }

 private:
    struct HeapLayout {
        char* begin;
        size_type size;
        size_type capacity;  // excludes the terminator; the top bit is the heap flag
    };
    static constexpr size_type SSOCapacity = sizeof(HeapLayout) - 1;
    struct SSOLayout {
        char data[SSOCapacity + 1];  // the last byte holds SSOCapacity - size
    };
    union Layout {
        HeapLayout heap;
        SSOLayout sso;
    };
    static_assert(sizeof(HeapLayout) == sizeof(SSOLayout), "nvrhi::string: both layouts fill the union");
    static constexpr size_type HeapFlag = size_type(1) << (sizeof(size_type) * 8 - 1);

    unsigned char DoLastByte() const noexcept {
        return reinterpret_cast<const unsigned char*>(&mRep)[sizeof(Layout) - 1];
    }
    bool DoIsHeap() const noexcept { return (DoLastByte() & 0x80) != 0; }

    char* DoData() noexcept { return DoIsHeap() ? mRep.heap.begin : mRep.sso.data; }
    const char* DoData() const noexcept { return DoIsHeap() ? mRep.heap.begin : mRep.sso.data; }

    void DoSetSSOSize(size_type n) noexcept {
        NVRHI_ASSERT(n <= SSOCapacity);
        mRep.sso.data[SSOCapacity] = char(SSOCapacity - n);
        mRep.sso.data[n] = 0;
    }

    void DoSetHeap(char* p, size_type n, size_type cap) noexcept {
        mRep.heap.begin = p;
        mRep.heap.size = n;
        mRep.heap.capacity = cap | HeapFlag;
    }

    void DoSetSize(size_type n) noexcept {
        if (DoIsHeap()) {
            NVRHI_ASSERT(n <= capacity());
            mRep.heap.size = n;
            mRep.heap.begin[n] = 0;
        } else {
            DoSetSSOSize(n);
        }
    }

    // Room for cap chars and the terminator.
    char* DoAllocate(size_type cap) { return static_cast<char*>(mAllocator.allocate(cap + 1, 1)); }

    void DoFreeHeap() noexcept {
        if (DoIsHeap()) mAllocator.deallocate(mRep.heap.begin, capacity() + 1);
    }

    size_type DoGrowCapacity(size_type needed) const noexcept {
        const size_type grown = 2 * capacity();
        return grown < needed ? needed : grown;
    }

    void DoGrowTo(size_type cap) {
        const size_type sz = size();
        char* p = DoAllocate(cap);
        std::memcpy(p, DoData(), sz + 1);
        DoFreeHeap();
        DoSetHeap(p, sz, cap);
    }

    Layout mRep;
    allocator_type mAllocator;
};

static_assert(sizeof(string) == 4 * sizeof(void*), "nvrhi::string: a three-word union and the allocator");

inline void swap(string& a, string& b) { a.swap(b); }

inline bool operator==(const string& a, const string& b) noexcept {
    return a.size() == b.size() && a.compare(b) == 0;
}
inline bool operator!=(const string& a, const string& b) noexcept { return !(a == b); }
inline bool operator<(const string& a, const string& b) noexcept { return a.compare(b) < 0; }
inline bool operator>(const string& a, const string& b) noexcept { return a.compare(b) > 0; }
inline bool operator<=(const string& a, const string& b) noexcept { return a.compare(b) <= 0; }
inline bool operator>=(const string& a, const string& b) noexcept { return a.compare(b) >= 0; }

inline bool operator==(const string& a, const char* b) noexcept { return a.compare(b) == 0; }
inline bool operator==(const char* a, const string& b) noexcept { return b.compare(a) == 0; }
inline bool operator!=(const string& a, const char* b) noexcept { return a.compare(b) != 0; }
inline bool operator!=(const char* a, const string& b) noexcept { return b.compare(a) != 0; }
inline bool operator<(const string& a, const char* b) noexcept { return a.compare(b) < 0; }
inline bool operator<(const char* a, const string& b) noexcept { return b.compare(a) > 0; }
inline bool operator>(const string& a, const char* b) noexcept { return a.compare(b) > 0; }
inline bool operator>(const char* a, const string& b) noexcept { return b.compare(a) < 0; }
inline bool operator<=(const string& a, const char* b) noexcept { return a.compare(b) <= 0; }
inline bool operator<=(const char* a, const string& b) noexcept { return b.compare(a) >= 0; }
inline bool operator>=(const string& a, const char* b) noexcept { return a.compare(b) >= 0; }
inline bool operator>=(const char* a, const string& b) noexcept { return b.compare(a) <= 0; }

template <typename S, details::enable_if_string_like_t<S> = 0>
bool operator==(const string& a, const S& b) noexcept {
    return a.compare(b) == 0;
}
template <typename S, details::enable_if_string_like_t<S> = 0>
bool operator==(const S& a, const string& b) noexcept {
    return b.compare(a) == 0;
}
template <typename S, details::enable_if_string_like_t<S> = 0>
bool operator!=(const string& a, const S& b) noexcept {
    return a.compare(b) != 0;
}
template <typename S, details::enable_if_string_like_t<S> = 0>
bool operator!=(const S& a, const string& b) noexcept {
    return b.compare(a) != 0;
}
template <typename S, details::enable_if_string_like_t<S> = 0>
bool operator<(const string& a, const S& b) noexcept {
    return a.compare(b) < 0;
}
template <typename S, details::enable_if_string_like_t<S> = 0>
bool operator<(const S& a, const string& b) noexcept {
    return b.compare(a) > 0;
}

// Concatenation: the result uses the allocator of the string operand (the left one when both are strings).
inline string operator+(const string& a, const string& b) {
    string r(a.get_allocator());
    r.reserve(a.size() + b.size());
    r.append(a).append(b);
    return r;
}
inline string operator+(const string& a, const char* b) {
    const size_t n = b ? std::strlen(b) : 0;
    string r(a.get_allocator());
    r.reserve(a.size() + n);
    r.append(a).append(b, n);
    return r;
}
inline string operator+(const char* a, const string& b) {
    const size_t n = a ? std::strlen(a) : 0;
    string r(b.get_allocator());
    r.reserve(n + b.size());
    r.append(a, n).append(b);
    return r;
}
inline string operator+(const string& a, char b) {
    string r(a.get_allocator());
    r.reserve(a.size() + 1);
    r.append(a).push_back(b);
    return r;
}
inline string operator+(char a, const string& b) {
    string r(b.get_allocator());
    r.reserve(1 + b.size());
    r.push_back(a);
    r.append(b);
    return r;
}
inline string operator+(string&& a, const string& b) { return std::move(a.append(b)); }
inline string operator+(string&& a, const char* b) { return std::move(a.append(b)); }
inline string operator+(string&& a, char b) {
    a.push_back(b);
    return std::move(a);
}

// Stream output for any stream with write(const char*, count), e.g. std::ostream, without including <ostream>.
// Str is deduced, so that the operator takes nvrhi::string only, never something converted to it (a string
// literal would otherwise be ambiguous between this and std::operator<<(std::ostream&, const char*)).
template <typename OStream, typename Str, std::enable_if_t<std::is_same_v<Str, string>, int> = 0,
          typename = decltype(std::declval<OStream&>().write(std::declval<const char*>(), 1))>
OStream& operator<<(OStream& os, const Str& s) {
    os.write(s.data(), static_cast<long long>(s.size()));
    return os;
}

// ---- fixed_vector -------------------------------------------------------------------------------------------

namespace details {

// The allocator of fixed_vector's vector base (reference: EASTL fixed_vector_allocator). The inline buffer
// is never allocated or freed through it: the vector starts out pointing at the buffer, grows into the
// overflow allocator and frees only blocks outside the buffer. Every instance is unique (compares equal to
// itself only), so the vector base never exchanges buffers between two fixed_vectors.
template <typename OverflowAllocator, bool bEnableOverflow>
class fixed_vector_allocator {
 public:
    explicit fixed_vector_allocator(void* pPoolBegin) noexcept : mpPoolBegin(pPoolBegin), mOverflowAllocator() {}
    fixed_vector_allocator(void* pPoolBegin, const OverflowAllocator& overflow) noexcept
        : mpPoolBegin(pPoolBegin), mOverflowAllocator(overflow) {}

    void* allocate(size_t bytes, size_t alignment) noexcept {
        NVRHI_VERIFY(bEnableOverflow, "fixed_vector: the inline capacity is exceeded and overflow is disabled");
        return mOverflowAllocator.allocate(bytes, alignment);
    }

    void deallocate(void* p, size_t bytes, size_t alignment) noexcept {
        if (p != mpPoolBegin) mOverflowAllocator.deallocate(p, bytes, alignment);
    }

    friend bool operator==(const fixed_vector_allocator& a, const fixed_vector_allocator& b) noexcept {
        return &a == &b;
    }
    friend bool operator!=(const fixed_vector_allocator& a, const fixed_vector_allocator& b) noexcept {
        return &a != &b;
    }

    void* mpPoolBegin;
    OverflowAllocator mOverflowAllocator;
};

}  // namespace details

/// A vector with an inline buffer of N elements (reference: EASTL fixed_vector). With overflow enabled it
/// grows into the overflow allocator's heap when the buffer is full; with overflow disabled it asserts.
/// Copies and moves always start out in their own inline buffer: a move steals the source's heap block only
/// when the source has overflowed and both overflow allocators compare equal.
template <typename T, size_t N, bool bEnableOverflow = true, typename OverflowAllocator = allocator>
class fixed_vector : public vector<T, details::fixed_vector_allocator<OverflowAllocator, bEnableOverflow>> {
    static_assert(N > 0, "fixed_vector: N must not be zero");

 public:
    using fixed_allocator_type = details::fixed_vector_allocator<OverflowAllocator, bEnableOverflow>;
    using base_type = vector<T, fixed_allocator_type>;
    using overflow_allocator_type = OverflowAllocator;
    using typename base_type::const_iterator;
    using typename base_type::iterator;
    using typename base_type::size_type;
    using typename base_type::value_type;

    enum : size_t { kMaxSize = N };

    fixed_vector() : base_type(fixed_allocator_type(mBuffer)) { DoResetToBuffer(); }
    explicit fixed_vector(const overflow_allocator_type& overflow)
        : base_type(fixed_allocator_type(mBuffer, overflow)) {
        DoResetToBuffer();
    }
    explicit fixed_vector(size_type n) : fixed_vector() { this->resize(n); }
    fixed_vector(size_type n, const T& value) : fixed_vector() { this->assign(n, value); }

    template <typename InputIt, details::enable_if_iterator_t<InputIt> = 0>
    fixed_vector(InputIt first, InputIt last) : fixed_vector() {
        this->assign(first, last);
    }

    fixed_vector(std::initializer_list<T> il) : fixed_vector() { this->assign(il.begin(), il.end()); }

    // EASTL: the copy uses the source's overflow allocator.
    fixed_vector(const fixed_vector& x) : base_type(fixed_allocator_type(mBuffer, x.get_overflow_allocator())) {
        DoResetToBuffer();
        this->assign(x.begin(), x.end());
    }

    fixed_vector(fixed_vector&& x) : base_type(fixed_allocator_type(mBuffer, x.get_overflow_allocator())) {
        DoResetToBuffer();
        DoMoveFrom(x);
    }

    // The elements live in this class's buffer: destroy them here, before the buffer goes away, and leave the
    // vector base nothing to do.
    ~fixed_vector() {
        DoReleaseStorage();
        this->mpBegin = this->mpEnd = this->mpCapacity = nullptr;
    }

    fixed_vector& operator=(const fixed_vector& x) {
        if (this != &x) this->assign(x.begin(), x.end());
        return *this;
    }

    fixed_vector& operator=(fixed_vector&& x) {
        if (this != &x) {
            if (x.has_overflowed() && get_overflow_allocator() == x.get_overflow_allocator()) {
                DoReleaseStorage();
                DoResetToBuffer();
            } else {
                this->clear();
            }
            DoMoveFrom(x);
        }
        return *this;
    }

    fixed_vector& operator=(std::initializer_list<T> il) {
        this->assign(il.begin(), il.end());
        return *this;
    }

    void swap(fixed_vector& x) {
        if (this == &x) return;
        fixed_vector temp(std::move(*this));
        *this = std::move(x);
        x = std::move(temp);
    }

    // Moves the elements back into the inline buffer when they fit there, otherwise into an exact heap block.
    void shrink_to_fit() {
        if (!has_overflowed()) return;
        if (this->size() <= N) {
            T* oldBegin = this->mpBegin;
            const size_type oldCapacity = this->capacity();
            T* buffer = DoBuffer();
            this->mpEnd = details::relocate(this->mpBegin, this->mpEnd, buffer);
            this->mpBegin = buffer;
            this->mpCapacity = buffer + N;
            this->DoFree(oldBegin, oldCapacity);
        } else if (this->capacity() > this->size()) {
            this->DoRealloc(this->size());
        }
    }

    // True when the inline buffer is used up (the size reached N).
    bool full() const noexcept { return this->size() >= N; }
    // True when the elements live on the overflow heap rather than in the inline buffer.
    bool has_overflowed() const noexcept { return this->mpBegin != DoBuffer(); }
    size_type max_size() const noexcept { return bEnableOverflow ? base_type::max_size() : N; }

    const overflow_allocator_type& get_overflow_allocator() const noexcept {
        return this->mAllocator.mOverflowAllocator;
    }
    overflow_allocator_type& get_overflow_allocator() noexcept { return this->mAllocator.mOverflowAllocator; }

    // The overflow allocator may change while the vector holds no heap memory (or to an equal allocator).
    void set_overflow_allocator(const overflow_allocator_type& overflow) {
        NVRHI_ASSERT(!has_overflowed() || get_overflow_allocator() == overflow);
        this->mAllocator.mOverflowAllocator = overflow;
    }

 private:
    T* DoBuffer() noexcept { return reinterpret_cast<T*>(mBuffer); }
    const T* DoBuffer() const noexcept { return reinterpret_cast<const T*>(mBuffer); }

    void DoResetToBuffer() noexcept {
        this->mpBegin = this->mpEnd = DoBuffer();
        this->mpCapacity = DoBuffer() + N;
    }

    // Destroys the elements and frees the heap block, if any; the pointers are left dangling.
    void DoReleaseStorage() noexcept {
        details::destroy_range(this->mpBegin, this->mpEnd);
        if (has_overflowed()) this->DoFree(this->mpBegin, this->capacity());
    }

    // This vector is empty. Takes x's heap block if x has overflowed and the overflow allocators are equal
    // (then this vector is in its inline buffer), otherwise moves x's elements one by one; x is left empty.
    void DoMoveFrom(fixed_vector& x) {
        if (x.has_overflowed() && !has_overflowed() && get_overflow_allocator() == x.get_overflow_allocator()) {
            this->mpBegin = x.mpBegin;
            this->mpEnd = x.mpEnd;
            this->mpCapacity = x.mpCapacity;
            x.DoResetToBuffer();
        } else {
            this->reserve(x.size());
            this->mpEnd = details::uninitialized_move(x.mpBegin, x.mpEnd, this->mpBegin);
            x.clear();
        }
    }

    alignas(T) unsigned char mBuffer[N * sizeof(T)];
};

template <typename T, size_t N, bool bEnableOverflow, typename OverflowAllocator>
void swap(fixed_vector<T, N, bEnableOverflow, OverflowAllocator>& a,
          fixed_vector<T, N, bEnableOverflow, OverflowAllocator>& b) {
    a.swap(b);
}

static_assert(sizeof(fixed_vector<uint32_t, 4>) == 5 * sizeof(void*) + 4 * sizeof(uint32_t),
              "nvrhi::fixed_vector: the vector base, the pool pointer, the overflow allocator and the buffer");

// ---- static_vector ------------------------------------------------------------------------------------------

/// A vector with a capacity defined at compile time: all max_elements elements always exist (default
/// constructed), size() of them are in use. Out-of-capacity use asserts.
template <typename T, uint32_t _max_elements>
struct static_vector {
    static_assert(_max_elements > 0, "static_vector: the capacity must not be zero");
    enum { max_elements = _max_elements };

    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;
    // xxxnsubtil: reverse iterators not implemented

    static_vector() : m_elements(), m_size(0) {}

    static_vector(size_t size) : m_elements(), m_size(size) { NVRHI_ASSERT(size <= max_elements); }

    static_vector(std::initializer_list<T> il) : m_elements(), m_size(0) {
        for (const T& i : il) push_back(i);
    }

    // Checked against the capacity, like std::array::at (without the exception).
    reference at(size_type pos) noexcept {
        NVRHI_ASSERT(pos < max_elements);
        return m_elements[pos];
    }
    const_reference at(size_type pos) const noexcept {
        NVRHI_ASSERT(pos < max_elements);
        return m_elements[pos];
    }

    reference operator[](size_type pos) noexcept {
        NVRHI_ASSERT(pos < m_size);
        return m_elements[pos];
    }
    const_reference operator[](size_type pos) const noexcept {
        NVRHI_ASSERT(pos < m_size);
        return m_elements[pos];
    }

    reference front() noexcept { return m_elements[0]; }
    const_reference front() const noexcept { return m_elements[0]; }

    reference back() noexcept { return m_elements[m_size - 1]; }
    const_reference back() const noexcept { return m_elements[m_size - 1]; }

    T* data() noexcept { return m_elements; }
    const T* data() const noexcept { return m_elements; }

    iterator begin() noexcept { return m_elements; }
    const_iterator begin() const noexcept { return m_elements; }
    const_iterator cbegin() const noexcept { return m_elements; }

    iterator end() noexcept { return m_elements + m_size; }
    const_iterator end() const noexcept { return m_elements + m_size; }
    const_iterator cend() const noexcept { return m_elements + m_size; }

    bool empty() const noexcept { return m_size == 0; }
    size_t size() const noexcept { return m_size; }
    constexpr size_t max_size() const noexcept { return max_elements; }

    void fill(const T& value) noexcept {
        for (T& e : m_elements) e = value;
        m_size = max_elements;
    }

    void swap(static_vector& other) noexcept {
        for (size_type i = 0; i < max_elements; ++i) std::swap(m_elements[i], other.m_elements[i]);
        std::swap(m_size, other.m_size);
    }

    void push_back(const T& value) noexcept {
        NVRHI_ASSERT(m_size < max_elements);
        m_elements[m_size] = value;
        m_size++;
    }

    void push_back(T&& value) noexcept {
        NVRHI_ASSERT(m_size < max_elements);
        m_elements[m_size] = std::move(value);
        m_size++;
    }

    void pop_back() noexcept {
        NVRHI_ASSERT(m_size > 0);
        m_size--;
    }

    // Elements dropped or added are reset to T{}.
    void resize(size_type new_size) noexcept {
        NVRHI_ASSERT(new_size <= max_elements);

        if (m_size > new_size) {
            for (size_type i = new_size; i < m_size; i++) m_elements[i] = T{};
        } else {
            for (size_type i = m_size; i < new_size; i++) m_elements[i] = T{};
        }

        m_size = new_size;
    }

    reference emplace_back() noexcept {
        NVRHI_ASSERT(m_size < max_elements);
        ++m_size;
        back() = T{};
        return back();
    }

 private:
    T m_elements[_max_elements];
    size_type m_size;
};

static_assert(sizeof(static_vector<uint32_t, 4>) == 4 * sizeof(uint32_t) + sizeof(size_t),
              "nvrhi::static_vector: the elements and the size");

}  // namespace nvrhi

namespace std {
template <>
struct hash<nvrhi::string> {
    size_t operator()(const nvrhi::string& s) const noexcept {
        // FNV-1a
        uint64_t h = 14695981039346656037ull;
        for (char c : s) {
            h ^= uint64_t(static_cast<unsigned char>(c));
            h *= 1099511628211ull;
        }
        return size_t(h);
    }
};
}  // namespace std
