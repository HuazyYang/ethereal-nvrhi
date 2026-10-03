#ifndef NVRHI_CORE_MEMORY_H
#define NVRHI_CORE_MEMORY_H
#include <nvrhi/core/export.h>
#include <type_traits>
#include <limits>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

#if defined(_MSC_VER)
#define NVRHI_LIKELY(x) (x)
#define NVRHI_UNLIKELY(x) (x)
#define NVRHI_DEBUG_BREAK() __debugbreak()
#else
#define NVRHI_LIKELY(x) __builtin_expect(!!(x), 1)
#define NVRHI_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define NVRHI_DEBUG_BREAK() ((void)0)
#endif

// NVRHI_DEBUG turns on the debug checks of the core headers (NVRHI_ASSERT, NVRHI_VERIFY, checked_cast). It
// follows NDEBUG, as assert() does, and _DEBUG, which only MSVC defines; define it to 0 or 1 to override.
// Modules built with different values instantiate different inline bodies: they stay private to each
// module (hidden visibility on ELF, see cmake/NvrhiCore.cmake).
#ifndef NVRHI_DEBUG
#if !defined(NDEBUG) || defined(_DEBUG)
#define NVRHI_DEBUG 1
#else
#define NVRHI_DEBUG 0
#endif
#endif

#if NVRHI_DEBUG

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
    virtual void* Allocate(size_t Size) noexcept = 0;

    /// Releases memory
    virtual void Free(void* Ptr) noexcept = 0;

    /// Allocates block of memory with specified alignment
    virtual void* AllocateAligned(size_t Size, size_t Alignment) noexcept = 0;

    /// Releases memory allocated with AllocateAligned
    virtual void FreeAligned(void* Ptr) noexcept = 0;
};

/// The process-wide default allocator, owned by nvrhi_core (src/core/memory.cpp). Every module that links
/// nvrhi_core gets the same instance, so a block allocated through it in one module (EXE, DLL, shared object)
/// may be freed through it in any other, whatever CRT each module links.
NVRHI_CORE_C_API IMemoryAllocator* nvrhiCoreGetDefaultMemAllocator() noexcept;

/// The allocator behind MAKE_RC_OBJ, UserAllocated and the default nvrhi containers.
inline IMemoryAllocator* GetDefaultMemAllocator() noexcept { return nvrhiCoreGetDefaultMemAllocator(); }

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

template <typename T, typename AllocatorType = IMemoryAllocator>
struct STDAllocator {
    using value_type = T;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using reference = value_type&;
    using const_reference = const value_type&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    // Two adapters are equal when they free through the same allocator instance. The default allocator is
    // one instance per process (nvrhi_core), so adapters over it compare equal in every module; adapters over
    // different custom allocators never do, so a std container does not swap or move-assign blocks between them.
    using is_always_equal = std::false_type;

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
    return &left.m_Allocator == &right.m_Allocator;
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
