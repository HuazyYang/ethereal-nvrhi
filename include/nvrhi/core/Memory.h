#ifndef NVRHI_CORE_MEMORY_H
#define NVRHI_CORE_MEMORY_H
#include <type_traits>
#include <limits>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
#include <malloc.h>
#endif

#if defined(_MSC_VER)
#define NVRHI_LIKELY(x) (x)
#define NVRHI_UNLIKELY(x) (x)
#define NVRHI_DEBUG_BREAK() __debugbreak()
#else
#define NVRHI_LIKELY(x) __builtin_expect(!!(x), 1)
#define NVRHI_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define NVRHI_DEBUG_BREAK() ((void)0)
#endif

#ifdef _DEBUG

// #define NVRHI_DUMP_ALIVE_OBJECTS

#define NVRHI_ASSERT(expr) assert(expr)

#define NVRHI_VERIFY(Expr, ...)  \
    do {                         \
        if (!(Expr)) {           \
            NVRHI_DEBUG_BREAK(); \
            assert(0);           \
        }                        \
    } while (false)

#else

// #define NVRHI_DUMP_ALIVE_OBJECTS

#define NVRHI_ASSERT(expr) ((void)0)

// clang-format off
#    define NVRHI_VERIFY(...)do{}while(false)
// clang-format on

#endif

namespace nvrhi {
struct IMemoryAllocator {
    /// Allocates block of memory
    virtual void* Allocate(size_t Size) = 0;

    /// Releases memory
    virtual void Free(void* Ptr) = 0;

    /// Allocates block of memory with specified alignment
    virtual void* AllocateAligned(size_t Size, size_t Alignment) = 0;

    /// Releases memory allocated with AllocateAligned
    virtual void FreeAligned(void* Ptr) = 0;
};

namespace details {
#if defined(__ANDROID__) && __ANDROID_API__ < 28
// No aligned_alloc: over-allocate and keep the malloc pointer just below the aligned block.
inline void* AlignedMalloc(size_t Size, size_t Alignment) {
    constexpr size_t PointerSize = sizeof(void*);
    const size_t AdjustedAlignment = Alignment > PointerSize ? Alignment : PointerSize;

    void* Pointer = std::malloc(Size + AdjustedAlignment + PointerSize);
    if (!Pointer) return nullptr;
    const uintptr_t Aligned = (reinterpret_cast<uintptr_t>(Pointer) + PointerSize + AdjustedAlignment - 1) &
                              ~uintptr_t(AdjustedAlignment - 1);
    reinterpret_cast<void**>(Aligned)[-1] = Pointer;
    return reinterpret_cast<void*>(Aligned);
}

inline void AlignedFree(void* Ptr) {
    if (Ptr) std::free(reinterpret_cast<void**>(Ptr)[-1]);
}
#elif defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
inline void* AlignedMalloc(size_t Size, size_t Alignment) { return _aligned_malloc(Size, Alignment); }
inline void AlignedFree(void* Ptr) { _aligned_free(Ptr); }
#else
inline void* AlignedMalloc(size_t Size, size_t Alignment) {
    // aligned_alloc wants the size to be a multiple of the alignment
    return std::aligned_alloc(Alignment, (Size + Alignment - 1) & ~(Alignment - 1));
}
inline void AlignedFree(void* Ptr) { std::free(Ptr); }
#endif
}  // namespace details

/// The allocator behind MAKE_RC_OBJ, header only and stateless: all instances are interchangeable, so each
/// module (EXE, DLL, shared object) may own its own copy. Memory is always freed by code of the module that
/// allocated it: an RC object is destroyed through its ObjectWrapper, whose vtable is instantiated where
/// MakeNewRCObj created the object. So this holds even when every DLL links its own static CRT heap.
class DefaultMemoryAllocator final : public IMemoryAllocator {
 public:
    constexpr DefaultMemoryAllocator() noexcept = default;

    /// Allocates block of memory
    void* Allocate(size_t Size) override {
        NVRHI_VERIFY(Size > 0);
        return std::malloc(Size);
    }

    /// Releases memory
    void Free(void* Ptr) override { std::free(Ptr); }

    /// Allocates block of memory with specified alignment
    void* AllocateAligned(size_t Size, size_t Alignment) override {
        NVRHI_VERIFY(Size > 0 && Alignment > 0 && (Alignment & (Alignment - 1)) == 0);
        return details::AlignedMalloc(Size, Alignment);
    }

    /// Releases memory allocated with AllocateAligned
    void FreeAligned(void* Ptr) override { details::AlignedFree(Ptr); }

 private:
    DefaultMemoryAllocator(const DefaultMemoryAllocator&) = delete;
    DefaultMemoryAllocator(DefaultMemoryAllocator&&) = delete;
    DefaultMemoryAllocator& operator=(const DefaultMemoryAllocator&) = delete;
    DefaultMemoryAllocator& operator=(DefaultMemoryAllocator&&) = delete;
};

/// One instance per module on Windows (one per process on ELF); either is fine for a stateless allocator.
inline DefaultMemoryAllocator* GetDefaultMemAllocator() noexcept {
    static DefaultMemoryAllocator Allocator;  // constexpr ctor: constant-initialized, no guard
    return &Allocator;
}

struct NvrhiNewOverload {};

template <typename AllocatorType, typename Tp>
void DeleteObject(AllocatorType* pAllocator, Tp* p) {
    if (p) {
        p->~Tp();
        pAllocator->Free(p);
    }
}

// std::allocator adapter

template <typename T>
typename std::enable_if<std::is_destructible<T>::value, void>::type Destruct(T* ptr) {
    ptr->~T();
}

template <typename T>
typename std::enable_if<!std::is_destructible<T>::value, void>::type Destruct(T* ptr) {}

template <typename T, typename AllocatorType = DefaultMemoryAllocator>
struct STDAllocator {
    using value_type = T;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using reference = value_type&;
    using const_reference = const value_type&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    // The default allocator is stateless: any instance frees blocks of any other. A container shared
    // between modules should use IMemoryAllocator instead, so that it frees through the owner's vtable.
    using is_always_equal = std::is_same<AllocatorType, DefaultMemoryAllocator>;

    STDAllocator(AllocatorType& Allocator) noexcept : m_Allocator{Allocator} {}

    template <class U>
    STDAllocator(const STDAllocator<U, AllocatorType>& other) noexcept
        : m_Allocator{other.m_Allocator} {}

    template <class U>
    STDAllocator(STDAllocator<U, AllocatorType>&& other) noexcept
        : m_Allocator{other.m_Allocator} {}

    template <class U>
    STDAllocator& operator=(STDAllocator<U, AllocatorType>&& other) noexcept {
        NVRHI_VERIFY(&m_Allocator == &other.m_Allocator, "Inconsistent allocators");
        return *this;
    }

    template <class U>
    struct rebind {
        typedef STDAllocator<U, AllocatorType> other;
    };

    T* allocate(std::size_t count) {
        return reinterpret_cast<T*>(
            m_Allocator.AllocateAligned(count * sizeof(T), alignof(T)));
    }

    pointer address(reference r) { return &r; }
    const_pointer address(const_reference r) { return &r; }

    void deallocate(T* p, std::size_t count) { m_Allocator.FreeAligned(p); }

    inline size_type max_size() const {
        return (std::numeric_limits<size_type>::max)() / sizeof(T);
    }

    //    construction/destruction
    template <class U, class... Args>
    void construct(U* p, Args&&... args) {
        ::new (p) U(std::forward<Args>(args)...);
    }

    inline void destroy(pointer p) { Destruct(p); }

    AllocatorType& m_Allocator;
};

template <class T, class U, class A>
bool operator==(const STDAllocator<T, A>& left, const STDAllocator<U, A>& right) noexcept {
    return STDAllocator<T, A>::is_always_equal::value || &left.m_Allocator == &right.m_Allocator;
}

template <class T, class U, class A>
bool operator!=(const STDAllocator<T, A>& left, const STDAllocator<U, A>& right) {
    return !(left == right);
}

}  // namespace nvrhi

// inline void* operator new(size_t, nvrhi::NvrhiNewOverload, void* where) { return where; }
// inline void operator delete(void*, nvrhi::NvrhiNewOverload, void*) {
// }  // This is only required so we can use the symmetrical new()
// #define NVRHI_NEW(Allocator, Ty) \
//     new (nvrhi::NvrhiNewOverload, Allocator.Allocate(sizeof(Ty))) Ty
// #define NVRHI_NEW0(Ty)            \
//     new (nvrhi::NvrhiNewOverload, \
//          nvrhi::GetDefaultMemAllocator()->Allocate(sizeof(Ty))) Ty
// #define NVRHI_DELETE(Allocator, p) nvrhi::DeleteObject(&Allocator, p)
// #define NVRHI_DELETE0(p) nvrhi::DeleteObject(nvrhi::GetDefaultMemAllocator(), p)


#endif /* NVRHI_CORE_MEMORY_H */
