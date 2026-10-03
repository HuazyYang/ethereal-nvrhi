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

// The process-wide default allocator (nvrhiCoreGetDefaultMemAllocator).

#include <nvrhi/core/memory.h>
#include <cstdlib>
#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
#include <malloc.h>
#endif

namespace nvrhi {
namespace {

#if defined(__ANDROID__) && __ANDROID_API__ < 28
// No aligned_alloc: over-allocate and keep the malloc pointer just below the aligned block.
void* AlignedMalloc(size_t Size, size_t Alignment) noexcept {
    constexpr size_t PointerSize = sizeof(void*);
    const size_t AdjustedAlignment = Alignment > PointerSize ? Alignment : PointerSize;

    void* Pointer = std::malloc(Size + AdjustedAlignment + PointerSize);
    if (!Pointer) return nullptr;
    const uintptr_t Aligned = (reinterpret_cast<uintptr_t>(Pointer) + PointerSize + AdjustedAlignment - 1) &
                              ~uintptr_t(AdjustedAlignment - 1);
    reinterpret_cast<void**>(Aligned)[-1] = Pointer;
    return reinterpret_cast<void*>(Aligned);
}

void AlignedFree(void* Ptr) noexcept {
    if (Ptr) std::free(reinterpret_cast<void**>(Ptr)[-1]);
}
#elif defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
void* AlignedMalloc(size_t Size, size_t Alignment) noexcept { return _aligned_malloc(Size, Alignment); }
void AlignedFree(void* Ptr) noexcept { _aligned_free(Ptr); }
#else
void* AlignedMalloc(size_t Size, size_t Alignment) noexcept {
    // aligned_alloc wants the size to be a multiple of the alignment
    return std::aligned_alloc(Alignment, (Size + Alignment - 1) & ~(Alignment - 1));
}
void AlignedFree(void* Ptr) noexcept { std::free(Ptr); }
#endif

/// Stateless, on the CRT heap of nvrhi_core. There is one instance per process: every module reaches it
/// through nvrhiCoreGetDefaultMemAllocator(), so memory allocated through it in one module may be freed
/// through it in any other.
class DefaultMemoryAllocator final : public IMemoryAllocator {
 public:
    constexpr DefaultMemoryAllocator() noexcept = default;

    void* Allocate(size_t Size) noexcept override {
        NVRHI_VERIFY(Size > 0);
        return std::malloc(Size);
    }

    void Free(void* Ptr) noexcept override { std::free(Ptr); }

    void* AllocateAligned(size_t Size, size_t Alignment) noexcept override {
        NVRHI_VERIFY(Size > 0 && Alignment > 0 && (Alignment & (Alignment - 1)) == 0);
        return AlignedMalloc(Size, Alignment);
    }

    void FreeAligned(void* Ptr) noexcept override { AlignedFree(Ptr); }

    DefaultMemoryAllocator(const DefaultMemoryAllocator&) = delete;
    DefaultMemoryAllocator(DefaultMemoryAllocator&&) = delete;
    DefaultMemoryAllocator& operator=(const DefaultMemoryAllocator&) = delete;
    DefaultMemoryAllocator& operator=(DefaultMemoryAllocator&&) = delete;
};

// constexpr constructor: constant-initialized, so it is usable from any other module's static initializers.
DefaultMemoryAllocator g_DefaultAllocator;

}  // namespace

NVRHI_CORE_C_API IMemoryAllocator* nvrhiCoreGetDefaultMemAllocator() noexcept { return &g_DefaultAllocator; }

}  // namespace nvrhi
