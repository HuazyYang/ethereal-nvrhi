#ifndef NVRHI_CORE_AUTOPTR_H
#define NVRHI_CORE_AUTOPTR_H
#include <nvrhi/core/types.h>
#include <nvrhi/core/memory.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace nvrhi {

template <typename T>
class WeakPtr;

// The main advantage of AutoPtr over the std::shared_ptr is that you can
// attach the same raw pointer to different smart pointers.
//
// For instance, the following code will crash since p will be released twice:
//
// auto *p = new char;
// std::shared_ptr<char> pTmp1(p);
// std::shared_ptr<char> pTmp2(p);
// ...

// This code, in contrast, works perfectly fine:
//
// ObjectImpl *pRawPtr(new ObjectImpl);
// AutoPtr<ObjectImpl> pSmartPtr1(pRawPtr);
// AutoPtr<ObjectImpl> pSmartPtr2(pRawPtr);
// ...

// Other advantage is that weak pointers remain valid until the
// object is alive, even if all smart pointers were destroyed:
//
// WeakPtr<ObjectImpl> pWeakPtr(pSmartPtr1);
// pSmartPtr1.Release();
// auto pSmartPtr3 = pWeakPtr.Lock();
// ..

// Weak pointers can also be attached directly to the object:
// WeakPtr<ObjectImpl> pWeakPtr(pRawPtr);
//

namespace details {

// T should be the AutoPtr<T> or a derived type of it, not just the interface
template <typename T>
class AutoPtrRef {
    using InterfaceType = typename T::InterfaceType;

 public:
    AutoPtrRef(T* ptr) throw() { this->ptr_ = ptr; }

    // Conversion operators
    operator void**() const throw() {
        return reinterpret_cast<void**>(this->ptr_->ReleaseAndGetAddressOf());
    }

    // This is our operator AutoPtr<U> (or the latest derived class from AutoPtr (e.g.
    // WeakRef))
    operator T*() throw() {
        *this->ptr_ = nullptr;
        return this->ptr_;
    }

    // We define operator InterfaceType**() here instead of on ComPtrRefBase<T>, since
    // if InterfaceType is IObject or IInspectable, having it on the base will collide.
    operator InterfaceType**() throw() { return this->ptr_->ReleaseAndGetAddressOf(); }

    // This is used for NVRHI_IID_PPV_ARGS in order to do uuid_of(**(ppType)).
    // It does not need to clear  ptr_ at this point, it is done at
    // IID_PPV_ARGS_Helper(AutoPtrRef) later in this file.
    InterfaceType* operator*() throw() { return this->ptr_->Get(); }

    // Explicit functions
    InterfaceType* const* GetAddressOf() const throw() {
        return this->ptr_->GetAddressOf();
    }

    InterfaceType** ReleaseAndGetAddressOf() throw() {
        return this->ptr_->ReleaseAndGetAddressOf();
    }

 private:
    T* ptr_;
};

template <typename Ty1, typename Ty2, bool>
struct CompressedPair;

struct ZeroThenVariadicArgs {};
struct OneThenVariadicArgs {};

template <typename Ty1, typename Ty2, bool = std::is_empty_v<Ty1> && !std::is_final_v<Ty1>>
struct CompressedPair : private Ty1 {
    Ty2 val2;

    template <typename... Args>
    constexpr explicit CompressedPair(ZeroThenVariadicArgs, Args&&... args) noexcept
        : Ty1(), val2(std::forward<Args>(args)...) {}

    template <typename Tp1, typename... Args>
    constexpr explicit CompressedPair(OneThenVariadicArgs, Tp1&& val1,
                                      Args&&... args) noexcept
        : Ty1(std::forward<Tp1>(val1)), val2(std::forward<Args>(args)...) {}

    constexpr Ty1& GetFirst() noexcept { return *this; }
    constexpr const Ty1& GetFirst() const noexcept { return *this; }
};

template <typename Ty1, typename Ty2>
struct CompressedPair<Ty1, Ty2, false> {
    Ty1 val1;
    Ty2 val2;

    template <typename... Args>
    constexpr explicit CompressedPair(ZeroThenVariadicArgs, Args&&... args) noexcept
        : val1(), val2(std::forward<Args>(args)...) {}

    template <typename Tp1, typename... Args>
    constexpr explicit CompressedPair(OneThenVariadicArgs, Tp1&& val1,
                                      Args&&... args) noexcept
        : val1(std::forward<Tp1>(val1)), val2(std::forward<Args>(args)...) {}

    constexpr Ty1& GetFirst() noexcept { return val1; }
    constexpr const Ty1& GetFirst() const noexcept { return val1; }
};

template <class Kty, bool Enabled>
struct ConditionalEnabledHash {
    [[nodiscard]] size_t operator()(const Kty& key_val) const {
        return std::hash<Kty>::do_hash(key_val);
    }
};
template <class Kty>
struct ConditionalEnabledHash<Kty, false> {
    ConditionalEnabledHash() = delete;
    ConditionalEnabledHash(const ConditionalEnabledHash&) = delete;
    ConditionalEnabledHash(ConditionalEnabledHash&&) = delete;
    ConditionalEnabledHash& operator=(const ConditionalEnabledHash&) = delete;
    ConditionalEnabledHash& operator=(ConditionalEnabledHash&&) = delete;
};

template <bool Test, typename T = void>
using EnableIf = std::enable_if<Test, T>;

template <typename Tp, typename Ty>
using IsConvertible = std::is_convertible<Tp, Ty>;

template <typename Base, typename Derived>
using IsBaseOf = std::is_base_of<Base, Derived>;

template <typename Tp, typename Ty>
using IsSame = std::is_same<Tp, Ty>;

using nullptr_t = std::nullptr_t;

using BoolType = bool;

template <typename T>
struct WeakRefTypeTrait {
    // SFINEA overload when implement of IWeakReferenceSource is visible to compiling unit.
    template <typename Tp>
    static typename Tp::WeakRefImplType* GetWeakRef(typename Tp::WeakRefImplType*);
    // SFINEA overload when only IWeakReference is used.
    template <typename Tp>
    static IWeakReference* GetWeakRef(Tp*);

    using Type = T;
    using WeakRefType = typename std::remove_pointer<decltype(GetWeakRef<T>(0))>::type;
};

}  // namespace details

template <typename T>
FRESULT AsWeak(T* p, WeakPtr<T>* pWeak) throw();

template <typename T>
class AutoPtr {
 public:
    typedef T InterfaceType;

 protected:
    InterfaceType* ptr_;
    template <class U>
    friend class AutoPtr;

    void InternalAddRef() const throw() {
        if (ptr_ != nullptr) {
            ptr_->AddRef();
        }
    }

    FLONG InternalRelease() throw() {
        FLONG ref = 0;
        T* temp = ptr_;

        if (temp != nullptr) {
            ptr_ = nullptr;
            ref = temp->Release();
        }

        return ref;
    }

 public:
#pragma region constructors
    AutoPtr() throw() : ptr_(nullptr) {}

    AutoPtr(details::nullptr_t) throw() : ptr_(nullptr) {}

    template <class U>
    AutoPtr(U* other) throw() : ptr_(other) {
        InternalAddRef();
    }

    AutoPtr(const AutoPtr& other) throw() : ptr_(other.ptr_) { InternalAddRef(); }

    // copy constructor that allows to instantiate class when U* is convertible to T*
    template <class U>
    AutoPtr(const AutoPtr<U>& other,
            typename details::EnableIf<details::IsConvertible<U*, T*>::value,
                                       void*>::type* = 0) throw()
        : ptr_(other.ptr_) {
        InternalAddRef();
    }

    AutoPtr(AutoPtr&& other) throw() : ptr_(nullptr) {
        if (this != reinterpret_cast<AutoPtr*>(&reinterpret_cast<unsigned char&>(other))) {
            Swap(other);
        }
    }

    // Move constructor that allows instantiation of a class when U* is convertible to T*
    template <class U>
    AutoPtr(AutoPtr<U>&& other,
            typename details::EnableIf<details::IsConvertible<U*, T*>::value,
                                       void*>::type* = 0) throw()
        : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
#pragma endregion

#pragma region destructor
    ~AutoPtr() throw() { InternalRelease(); }
#pragma endregion

#pragma region assignment
    AutoPtr& operator=(details::nullptr_t) throw() {
        InternalRelease();
        return *this;
    }

    AutoPtr& operator=(T* other) throw() {
        if (ptr_ != other) {
            AutoPtr(other).Swap(*this);
        }
        return *this;
    }

    template <typename U>
    AutoPtr& operator=(U* other) throw() {
        AutoPtr(other).Swap(*this);
        return *this;
    }

    AutoPtr& operator=(const AutoPtr& other) throw() {
        if (ptr_ != other.ptr_) {
            AutoPtr(other).Swap(*this);
        }
        return *this;
    }

    template <class U>
    AutoPtr& operator=(const AutoPtr<U>& other) throw() {
        AutoPtr(other).Swap(*this);
        return *this;
    }

    AutoPtr& operator=(AutoPtr&& other) throw() {
        AutoPtr(static_cast<AutoPtr&&>(other)).Swap(*this);
        return *this;
    }

    template <class U>
    AutoPtr& operator=(AutoPtr<U>&& other) throw() {
        AutoPtr(static_cast<AutoPtr<U>&&>(other)).Swap(*this);
        return *this;
    }
#pragma endregion

#pragma region modifiers
    void Swap(AutoPtr&& r) throw() {
        T* tmp = ptr_;
        ptr_ = r.ptr_;
        r.ptr_ = tmp;
    }

    void Swap(AutoPtr& r) throw() {
        T* tmp = ptr_;
        ptr_ = r.ptr_;
        r.ptr_ = tmp;
    }
#pragma endregion

    explicit operator details::BoolType() const noexcept { return ptr_ != nullptr; }

    T* Get() const throw() { return ptr_; }

    operator T*() const noexcept { return ptr_; }

    InterfaceType* operator->() const throw() { return ptr_; }

    details::AutoPtrRef<AutoPtr<T>> operator&() throw() {
        return details::AutoPtrRef<AutoPtr<T>>(this);
    }

    const details::AutoPtrRef<const AutoPtr<T>> operator&() const throw() {
        return details::AutoPtrRef<const AutoPtr<T>>(this);
    }

    T* const* GetAddressOf() const throw() { return &ptr_; }

    T** GetAddressOf() throw() { return &ptr_; }

    T** ReleaseAndGetAddressOf() throw() {
        InternalRelease();
        return &ptr_;
    }

    T* Detach() throw() {
        T* ptr = ptr_;
        ptr_ = nullptr;
        return ptr;
    }

    void Attach(InterfaceType* other) throw() {
        if (ptr_ != nullptr) {
            auto ref = ptr_->Release();
            (void)ref;
            // Attaching to the same object only works if duplicate references are being
            // coalesced. Otherwise re-attaching will cause the pointer to be released and
            // may cause a crash on a subsequent dereference.
            NVRHI_ASSERT(ref != 0 || ptr_ != other);
        }

        ptr_ = other;
    }

    unsigned long Reset() { return InternalRelease(); }

    // Previously, unsafe behavior could be triggered when 'this' is AutoPtr<IInspectable>
    // or AutoPtr<IObject> and CopyTo is used to copy to another type U. The user will use
    // operator& to convert the destination into a AutoPtrRef, which can then implicit cast
    // to IInspectable** and IObject**. If this overload of CopyTo is not present, it will
    // implicitly cast to IInspectable or IObject and match CopyTo(InterfaceType**) instead.
    // A valid polymoprhic downcast requires run-time type checking via QueryInterface, so
    // CopyTo(InterfaceType**) will break type safety. This overload matches AutoPtrRef
    // before the implicit cast takes place, preventing the unsafe downcast.
    template <typename U>
    FRESULT CopyTo(details::AutoPtrRef<AutoPtr<U>> ptr,
                   typename details::EnableIf<details::IsSame<T, IObject>::value &&
                                                  !details::IsSame<U*, T*>::value,
                                              void*>::type* = 0) const throw() {
        return ptr_->QueryInterface(uuid_of<U>(), ptr);
    }

    FRESULT CopyTo(InterfaceType** ptr) const throw() {
        InternalAddRef();
        *ptr = ptr_;
        return FS_OK;
    }

    FRESULT CopyTo(FREFIID riid, void** ptr) const throw() {
        return ptr_->QueryInterface(riid, ptr);
    }

    template <typename U>
    FRESULT CopyTo(U** ptr) const throw() {
        return ptr_->QueryInterface(uuid_of<U>(), reinterpret_cast<void**>(ptr));
    }

    // query for U interface
    template <typename U>
    FRESULT As(details::AutoPtrRef<AutoPtr<U>> p) const throw() {
        return ptr_->QueryInterface(uuid_of<U>(), p);
    }

    // query for U interface
    template <typename U>
    FRESULT As(AutoPtr<U>* p) const throw() {
        return ptr_->QueryInterface(uuid_of<U>(),
                                    reinterpret_cast<void**>(p->ReleaseAndGetAddressOf()));
    }

    // query for riid interface and return as IObject
    FRESULT AsIID(FREFIID riid, AutoPtr<IObject>* p) const throw() {
        return ptr_->QueryInterface(riid,
                                    reinterpret_cast<void**>(p->ReleaseAndGetAddressOf()));
    }

    FRESULT AsWeak(WeakPtr<T>* pWeakRef) const throw() {
        return nvrhi::AsWeak(ptr_, pWeakRef);
    }
};  // AutoPtr

/// Implementation of weak pointers
template <typename T>
class WeakPtr {
 public:
    using WeakRefType = typename details::WeakRefTypeTrait<T>::WeakRefType;

    WeakPtr() noexcept {}

    explicit WeakPtr(T* pObj) noexcept : m_pWeakRef{nullptr}, m_pObject{pObj} {
        if (m_pObject) m_pWeakRef = AcquireWeakRef(m_pObject);
    }

    ~WeakPtr() { Reset(); }

    WeakPtr(const WeakPtr& WeakPtr) noexcept
        : m_pWeakRef{WeakPtr.m_pWeakRef}, m_pObject{WeakPtr.m_pObject} {
        if (m_pWeakRef) m_pWeakRef->AddRef();
    }

    WeakPtr(WeakPtr&& other) noexcept
        : m_pWeakRef{other.m_pWeakRef}, m_pObject{other.m_pObject} {
        other.m_pWeakRef = nullptr;
        other.m_pObject = nullptr;
    }

    explicit WeakPtr(AutoPtr<T>& AutoPtr) noexcept
        : m_pWeakRef{AutoPtr ? AcquireWeakRef(AutoPtr.Get()) : nullptr},
          m_pObject{static_cast<T*>(AutoPtr.Get())} {}

    WeakPtr& operator=(const WeakPtr& WeakPtr) noexcept {
        if (*this == WeakPtr) return *this;

        Reset();
        m_pObject = WeakPtr.m_pObject;
        m_pWeakRef = WeakPtr.m_pWeakRef;
        if (m_pWeakRef) m_pWeakRef->AddRef();
        return *this;
    }

    WeakPtr& operator=(T* pObj) noexcept { return operator=(WeakPtr(pObj)); }

    WeakPtr& operator=(WeakPtr&& other) noexcept {
        if (*this == other) return *this;

        Reset();
        m_pObject = other.m_pObject;
        m_pWeakRef = other.m_pWeakRef;
        other.m_pWeakRef = nullptr;
        other.m_pObject = nullptr;
        return *this;
    }

    WeakPtr& operator=(AutoPtr<T>& AutoPtr) noexcept {
        Reset();
        m_pObject = AutoPtr.Get();
        m_pWeakRef = m_pObject ? AcquireWeakRef(m_pObject) : nullptr;
        return *this;
    }

    void Reset() noexcept {
        if (m_pWeakRef) m_pWeakRef->Release();
        m_pWeakRef = nullptr;
        m_pObject = nullptr;
    }

    /// \note This method may not be reliable in a multithreaded environment.
    ///       However, when false is returned, the strong pointer created from
    ///       this weak pointer will reliably be empty.
    bool IsValid() const noexcept {
        return m_pObject != nullptr && m_pWeakRef != nullptr &&
               m_pWeakRef->GetNumStrongRefs() > 0;
    }

    /// Returns a raw pointer to the managed object.
    /// \note The object may or may not be alive.
    ///       Use Lock() to atomically obtain a strong reference.
    T* UnsafeRawPtr() noexcept { return m_pObject; }
    const T* UnsafeRawPtr() const noexcept { return m_pObject; }

    /// Obtains a strong reference to the object
    AutoPtr<T> Lock() const {
        AutoPtr<T> spObj;
        if (m_pWeakRef) {
            // Try to obtain a pointer to the owner object.
            // spOwner is only used to keep the object
            // alive while obtaining strong reference from
            // the raw pointer m_pObject
            AutoPtr<IObject> spOwner;
            m_pWeakRef->Resolve(NVRHI_IID_PPV_ARGS(&spOwner));
            if (spOwner) {
                // If owner is alive, we can use our RAW pointer to
                // create strong reference
                spObj = m_pObject;
            }
            // else {
            //     // Owner object has been destroyed. There is no reason
            //     // to keep this weak reference anymore
            //     Reset();
            // }
        }
        return spObj;
    }

    bool operator==(const WeakPtr& Ptr) const noexcept {
        return m_pWeakRef == Ptr.m_pWeakRef;
    }
    bool operator!=(const WeakPtr& Ptr) const noexcept {
        return m_pWeakRef != Ptr.m_pWeakRef;
    }

 protected:
    static WeakRefType* WeakRefTypeCast(IWeakReference* p) {
        return static_cast<WeakRefType*>(p);
    }
    // Returns the weak reference of pObj with its reference counter incremented.
    static WeakRefType* AcquireWeakRef(T* pObj) {
        IWeakReference* pWeakRef = nullptr;
        pObj->GetWeakReference(&pWeakRef);
        return WeakRefTypeCast(pWeakRef);
    }
    WeakRefType* m_pWeakRef = nullptr;
    // We need to store raw pointer to object itself,
    // because if the object is owned by another object,
    // m_pRefCounters->QueryObject(&pObj) will return
    // a pointer to the owner, which is not what we need.
    T* m_pObject = nullptr;
};

template <typename T>
AutoPtr<T> TakeOver(T* p) noexcept {
    AutoPtr<T> ret;
    *ret.GetAddressOf() = p;
    return ret;
}

template <typename T>
FRESULT AsWeak(T* p, WeakPtr<T>* pWeak) throw() {
    static_assert(!details::IsSame<IWeakReference, T>::value,
                  "Cannot get IWeakReference object to IWeakReference.");
    AutoPtr<IWeakReferenceSource> refSource;

    FRESULT hr = p->QueryInterface(NVRHI_IID_PPV_ARGS(refSource.GetAddressOf()));
    if (NVRHI_FAILED(hr)) {
        return hr;
    }

    AutoPtr<IWeakReference> weakref;
    refSource->GetWeakReference(weakref.GetAddressOf());
    if (!weakref) {
        return FE_NOT_IMPLEMENT;
    }

    *pWeak = WeakPtr<T>(p);
    return FS_OK;
}

// Comparison operators - don't compare COM object identity
template <class T, class U>
bool operator==(const AutoPtr<T>& a, const AutoPtr<U>& b) throw() {
    static_assert(details::IsBaseOf<T, U>::value || details::IsBaseOf<U, T>::value,
                  "'T' and 'U' pointers must be comparable");
    return a.Get() == b.Get();
}

template <class T>
bool operator==(const AutoPtr<T>& a, details::nullptr_t) throw() {
    return a.Get() == nullptr;
}

template <class T>
bool operator==(details::nullptr_t, const AutoPtr<T>& a) throw() {
    return a.Get() == nullptr;
}

template <class T, class U>
bool operator!=(const AutoPtr<T>& a, const AutoPtr<U>& b) throw() {
    static_assert(details::IsBaseOf<T, U>::value || details::IsBaseOf<U, T>::value,
                  "'T' and 'U' pointers must be comparable");
    return a.Get() != b.Get();
}

template <class T>
bool operator!=(const AutoPtr<T>& a, details::nullptr_t) throw() {
    return a.Get() != nullptr;
}

template <class T>
bool operator!=(details::nullptr_t, const AutoPtr<T>& a) throw() {
    return a.Get() != nullptr;
}

template <class T, class U>
bool operator<(const AutoPtr<T>& a, const AutoPtr<U>& b) throw() {
    static_assert(details::IsBaseOf<T, U>::value || details::IsBaseOf<U, T>::value,
                  "'T' and 'U' pointers must be comparable");
    return a.Get() < b.Get();
}

//// details::AutoPtrRef comparisons
template <class T, class U>
bool operator==(const details::AutoPtrRef<AutoPtr<T>>& a,
                const details::AutoPtrRef<AutoPtr<U>>& b) throw() {
    static_assert(details::IsBaseOf<T, U>::value || details::IsBaseOf<U, T>::value,
                  "'T' and 'U' pointers must be comparable");
    return a.GetAddressOf() == b.GetAddressOf();
}

template <class T>
bool operator==(const details::AutoPtrRef<AutoPtr<T>>& a, details::nullptr_t) throw() {
    return a.GetAddressOf() == nullptr;
}

template <class T>
bool operator==(details::nullptr_t, const details::AutoPtrRef<AutoPtr<T>>& a) throw() {
    return a.GetAddressOf() == nullptr;
}

template <class T>
bool operator==(const details::AutoPtrRef<AutoPtr<T>>& a, void* b) throw() {
    return a.GetAddressOf() == b;
}

template <class T>
bool operator==(void* b, const details::AutoPtrRef<AutoPtr<T>>& a) throw() {
    return a.GetAddressOf() == b;
}

template <class T, class U>
bool operator!=(const details::AutoPtrRef<AutoPtr<T>>& a,
                const details::AutoPtrRef<AutoPtr<U>>& b) throw() {
    static_assert(details::IsBaseOf<T, U>::value || details::IsBaseOf<U, T>::value,
                  "'T' and 'U' pointers must be comparable");
    return a.GetAddressOf() != b.GetAddressOf();
}

template <class T>
bool operator!=(const details::AutoPtrRef<AutoPtr<T>>& a, details::nullptr_t) throw() {
    return a.GetAddressOf() != nullptr;
}

template <class T>
bool operator!=(details::nullptr_t, const details::AutoPtrRef<AutoPtr<T>>& a) throw() {
    return a.GetAddressOf() != nullptr;
}

template <class T>
bool operator!=(const details::AutoPtrRef<AutoPtr<T>>& a, void* b) throw() {
    return a.GetAddressOf() != b;
}

template <class T>
bool operator!=(void* b, const details::AutoPtrRef<AutoPtr<T>>& a) throw() {
    return a.GetAddressOf() != b;
}

template <class T, class U>
bool operator<(const details::AutoPtrRef<AutoPtr<T>>& a,
               const details::AutoPtrRef<AutoPtr<U>>& b) throw() {
    static_assert(details::IsBaseOf<T, U>::value || details::IsBaseOf<U, T>::value,
                  "'T' and 'U' pointers must be comparable");
    return a.GetAddressOf() < b.GetAddressOf();
}

template <typename Ty>
struct DefaultDeleter {
    constexpr DefaultDeleter() noexcept = default;
    template <class Ty2,
              typename std::enable_if<std::is_convertible<Ty2*, Ty*>::value, int>::type = 0>
    DefaultDeleter(const DefaultDeleter<Ty2>&) noexcept {}

    void operator()(Ty* ptr) const noexcept {
        static_assert(0 < sizeof(Ty), "can't delete an incomplete type");
        delete ptr;
    }
};

template <typename Ty>
struct DefaultDeleter<Ty[]> {
    constexpr DefaultDeleter() noexcept = default;
    template <class Ty2,
              typename std::enable_if<std::is_convertible<Ty2, Ty*>::value, int>::type = 0>
    DefaultDeleter(const DefaultDeleter<Ty2>&) noexcept {}

    template <class Ty2,
              std::enable_if_t<std::is_convertible_v<Ty2 (*)[], Ty (*)[]>, int> = 0>
    void operator()(Ty2* ptr) const noexcept {
        static_assert(0 < sizeof(Ty2), "can't delete an incomplete type");
        delete[] ptr;
    }
};

namespace details {

template <typename Ty, class Dx_noref, class = void>
struct GetDeleterPointerType {
    using type = Ty*;
};

template <typename Ty, typename Dx_noref>
struct GetDeleterPointerType<Ty, Dx_noref, std::void_t<typename Dx_noref::pointer>> {
    using type = typename Dx_noref::pointer;
};

template <typename Dx2>
using MonoPtrEnableDefault =
    std::enable_if_t<std::conjunction_v<std::negation<std::is_pointer<Dx2>>,
                                        std::is_default_constructible<Dx2>>,
                     int>;
}  // namespace details

template <typename Ty, typename Dx = DefaultDeleter<Ty>>
class MonoPtr {
 public:
    using pointer = typename details::GetDeleterPointerType<Ty, std::remove_reference_t<Dx>>::type;
    using element_type = Ty;
    using deleter_type = Dx;

 public:
#pragma region constructors
    template <typename Dx2 = Dx, details::MonoPtrEnableDefault<Dx2> = 0>
    MonoPtr() noexcept : pair_(details::ZeroThenVariadicArgs{}) {}

    template <typename Dx2 = Dx, details::MonoPtrEnableDefault<Dx2> = 0>
    MonoPtr(details::nullptr_t) noexcept : pair_(details::ZeroThenVariadicArgs{}) {}

    template <typename Dx2 = Dx, details::MonoPtrEnableDefault<Dx2> = 0>
    explicit MonoPtr(pointer ptr) noexcept : pair_(details::ZeroThenVariadicArgs{}, ptr) {}

    template <typename Dx2 = Dx,
              std::enable_if_t<std::is_convertible_v<Dx2, const Dx2&>, int> = 0>
    MonoPtr(pointer ptr, const Dx& dt) noexcept
        : pair_(details::OneThenVariadicArgs{}, dt, ptr) {}

    template <typename Dx2 = Dx,
              std::enable_if_t<std::conjunction_v<std::negation<std::is_reference<Dx2>>,
                                                  std::is_convertible<Dx2, Dx2>>,
                               int> = 0>
    MonoPtr(pointer ptr, Dx&& dt) noexcept
        : pair_(details::OneThenVariadicArgs{}, std::move(dt), ptr) {}

    template <
        typename Dx2 = Dx,
        std::enable_if_t<
            std::conjunction_v<std::is_reference<Dx2>,
                               std::is_constructible<Dx2, std::remove_reference_t<Dx2>>>,
            int> = 0>
    MonoPtr(pointer, std::remove_reference_t<Dx>&&) = delete;

    template <typename Dx2 = Dx,
              std::enable_if_t<std::is_move_constructible_v<Dx2>, int> = 0>
    MonoPtr(MonoPtr&& other)
        : pair_(details::OneThenVariadicArgs{}, std::forward<Dx>(other.GetDeleter()), other.Detach()) {}

    template <typename Ty2, typename Dx2,
              std::enable_if_t<
                  std::conjunction_v<
                      std::negation<std::is_array<Ty2>>,
                      std::is_convertible<typename MonoPtr<Ty2, Dx2>::pointer, pointer>,
                      std::conditional_t<std::is_reference_v<Dx>, std::is_same<Dx2, Dx>,
                                         std::is_convertible<Dx2, Dx>>>,
                  int> = 0>
    MonoPtr(MonoPtr<Ty2, Dx2>&& other)
        : pair_(details::OneThenVariadicArgs{}, std::forward<Dx2>(other.GetDeleter()), other.Detach()) {}

    MonoPtr(const MonoPtr&) = delete;
    MonoPtr& operator=(const MonoPtr&) = delete;
#pragma endregion constructors

#pragma region destructor
    ~MonoPtr() noexcept {
        if (pair_.val2) pair_.GetFirst()(pair_.val2);
    }
#pragma endregion destructor

#pragma region assignment
    MonoPtr& operator=(details::nullptr_t) throw() {
        Reset();
        return *this;
    }

    template <typename Ty2, typename Dx2,
              std::enable_if_t<
                  std::conjunction_v<
                      std::negation<std::is_array<Ty2>>, std::is_assignable<Dx&, Dx2>,
                      std::is_convertible<typename MonoPtr<Ty2, Dx2>::pointer, pointer>>,
                  int> = 0>
    MonoPtr& operator=(MonoPtr<Ty2, Dx2>&& other) throw() {
        Reset(other.Detach());
        pair_.GetFirst() = std::forward<Dx2>(other.pair_.GetFirst());
        return *this;
    }

    template <typename Dx2 = Dx,
              std::enable_if_t<std::is_move_constructible_v<Dx2>, int> = 0>
    MonoPtr& operator=(MonoPtr&& other) throw() {
        if (this != std::addressof(other)) {
            Reset(other.Detach());
            pair_.GetFirst() = std::forward<Dx>(other.pair_.GetFirst());
        }
        return *this;
    }
#pragma endregion assignment

#pragma region modifiers
    void Swap(MonoPtr&& other) throw() {
        std::swap(pair_.val2, other.pair_.val2);
        std::swap(pair_.GetFirst(), other.pair_.GetFirst());
    }

    void Reset(pointer ptr = nullptr) noexcept {
        pointer old = std::exchange(pair_.val2, ptr);
        if (old) pair_.GetFirst()(old);
    }

    pointer Detach() noexcept { return std::exchange(pair_.val2, nullptr); }

#pragma endregion modifiers

    explicit operator details::BoolType() const throw() {
        return static_cast<details::BoolType>(pair_.val2);
    }

    pointer Get() const throw() { return pair_.val2; }

    operator pointer() const throw() { return pair_.val2; }

    pointer operator->() const throw() { return pair_.val2; }

    std::add_lvalue_reference_t<Ty> operator*() const noexcept { return *pair_.val2; }

 private:
    Dx& GetDeleter() noexcept { return pair_.GetFirst(); }
    const Dx& GetDeleter() const noexcept { return pair_.GetFirst(); }

    template <class U, class UDx>
    friend class MonoPtr;
    details::CompressedPair<Dx, pointer> pair_;
};

template <class Ty, class Dx>
class MonoPtr<Ty[], Dx> {
 public:
    using pointer =
        typename details::GetDeleterPointerType<Ty, std::remove_reference_t<Dx>>::type;
    using element_type = Ty;
    using deleter_type = Dx;

    template <typename Tp>
    using EnableCtorReset = std::enable_if_t<
        std::is_same_v<Tp, pointer>       //
        || std::is_same_v<Tp, std::nullptr_t>  //
        || (std::is_same_v<pointer, Ty*>  //
            && std::is_pointer_v<Tp>      //
            && std::is_convertible_v<std::remove_reference_t<Tp> (*)[], Ty (*)[]>), int>;
#pragma region constructors

    template <typename Dx2 = Dx, details::MonoPtrEnableDefault<Dx2> = 0>
    constexpr MonoPtr() noexcept : pair_(details::ZeroThenVariadicArgs{}) {}

    template <typename Dx2 = Dx, details::MonoPtrEnableDefault<Dx2> = 0>
    constexpr MonoPtr(details::nullptr_t) noexcept : pair_(details::ZeroThenVariadicArgs{}) {}

    template<typename Tp, class Dx2 = Dx, EnableCtorReset<Tp> = 0, details::MonoPtrEnableDefault<Dx2> = 0> 
    explicit MonoPtr(Tp ptr) : pair_(details::ZeroThenVariadicArgs{}, ptr) {}

    template <typename Tp, typename Dx2 = Dx, EnableCtorReset<Tp> = 0,
              std::enable_if_t<std::is_convertible_v<Dx2, const Dx2&>, int> = 0>
    MonoPtr(Tp ptr, const Dx& dt) noexcept : pair_(details::OneThenVariadicArgs{}, dt, ptr) {}

    template <typename Tp, typename Dx2 = Dx, EnableCtorReset<Tp> = 0,
              std::enable_if_t<std::conjunction_v<std::negation<std::is_reference<Dx2>>,
                                                  std::is_constructible<Dx2, Dx2>>,
                               int> = 0>
    MonoPtr(Tp ptr, Dx&& dt) noexcept
        : pair_(details::OneThenVariadicArgs{}, std::move(dt), ptr) {}

    template <
        typename Tp,
        typename Dx2 = Dx,
        EnableCtorReset<Tp> = 0,
        std::enable_if_t<
            std::conjunction_v<std::is_reference<Dx2>,
                               std::is_constructible<Dx2, std::remove_reference_t<Dx2>>>,
            int> = 0>
    MonoPtr(pointer ptr, std::remove_reference_t<Dx>&&) = delete;

    template <typename Dx2 = Dx,
              std::enable_if_t<std::is_move_constructible_v<Dx2>, int> = 0>
    MonoPtr(MonoPtr&& other) noexcept
        : pair_(details::OneThenVariadicArgs{}, std::forward<Dx>(other.GetDeleter()), other.Detach()) {}

    MonoPtr(const MonoPtr&) = delete;
    MonoPtr& operator=(const MonoPtr&) = delete;

#pragma endregion constructors

#pragma region destructor
    ~MonoPtr() noexcept {
        if (pair_.val2) pair_.GetFirst()(pair_.val2);
    }
#pragma endregion destructor

#pragma region assignment
    MonoPtr& operator=(details::nullptr_t) noexcept {
        Reset();
        return *this;
    }

    template <class Dx2 = Dx, std::enable_if_t<std::is_move_constructible_v<Dx2>, int> = 0>
    MonoPtr& operator=(MonoPtr&& other) noexcept {
        if (this != std::addressof(other)) {
            Reset(other.Detach());
            pair_.GetFirst() = std::move(other.pair_.GetFirst());
        }
        return *this;
    }

#pragma endregion assignment

#pragma region modifiers
    pointer Detach() noexcept { return std::exchange(pair_.val2, nullptr); }

    void Reset(pointer ptr = nullptr) noexcept {
        pointer old = std::exchange(pair_.val2, ptr);
        if (old) {
            pair_.GetFirst()(old);
        }
    }

    void Swap(MonoPtr& other) noexcept {
        std::swap(pair_.val2, other.pair_.val2);
        std::swap(pair_.GetFirst(), other.pair_.GetFirst());
    }
#pragma endregion modifiers

    explicit operator details::BoolType() const noexcept {
        return static_cast<details::BoolType>(pair_.val2);
    }

    pointer Get() const noexcept { return pair_.val2; }

    operator pointer() const throw() { return pair_.val2; }

    Ty& operator[](size_t idx) const noexcept { return pair_.val2[idx]; }

 private:
    Dx& GetDeleter() noexcept { return pair_.GetFirst(); }
    const Dx& GetDeleter() const noexcept { return pair_.GetFirst(); }

    template <class U, class UDx>
    friend class MonoPtr;
    details::CompressedPair<Dx, pointer> pair_;
};

template <typename Ty, typename... Types, std::enable_if_t<!std::is_array_v<Ty>, int> = 0>
MonoPtr<Ty> MakeMono(Types&&... args) {
    return MonoPtr<Ty>(new Ty(std::forward<Types>(args)...));
}

template <typename Ty,
          std::enable_if_t<std::is_array_v<Ty> && std::extent_v<Ty> == 0, int> = 0>
MonoPtr<Ty> MakeMono(const size_t size) {
    using ElemType = std::remove_extent_t<Ty>;
    return MonoPtr<Ty>(new ElemType[size]());
}

template <typename Ty, typename... Types, std::enable_if_t<std::extent_v<Ty> != 0, int> = 0>
void MakeMono(Types&&...) = delete;

template <class Ty1, class Dx1, class Ty2, class Dx2>
bool operator==(const MonoPtr<Ty1, Dx1>& a, const MonoPtr<Ty2, Dx2>& b) throw() {
    return a.Get() == b.Get();
}

template <class Ty, class Dx>
bool operator==(const MonoPtr<Ty, Dx>& a, details::nullptr_t) throw() {
    return !a.Get();
}

template <class Ty, class Dx>
bool operator==(details::nullptr_t, const MonoPtr<Ty, Dx>& a) throw() {
    return !a.Get();
}

template <class Ty1, class Dx1, class Ty2, class Dx2>
bool operator!=(const MonoPtr<Ty1, Dx1>& a, const MonoPtr<Ty2, Dx2>& b) throw() {
    return a.Get() != b.Get();
}

template <class Ty, class Dx>
bool operator!=(const MonoPtr<Ty, Dx>& a, details::nullptr_t) throw() {
    return !(a.Get() == nullptr);
}

template <class Ty, class Dx>
bool operator!=(details::nullptr_t, const MonoPtr<Ty, Dx>& a) throw() {
    return !(a.Get() == nullptr);
}

template <class Ty1, class Dx1, class Ty2, class Dx2>
bool operator<(const MonoPtr<Ty1, Dx1>& a, const MonoPtr<Ty2, Dx2>& b) throw() {
    return a.Get() < b.Get();
}

}  // namespace nvrhi

// Overloaded global function to provide to IID_PPV_ARGS that support details::AutoPtrRef
template <typename T>
void** IID_PPV_ARGS_Helper(nvrhi::details::AutoPtrRef<T> pp) throw() {
    return pp;
}

// std namespace adapters
namespace std {

template <typename Ty>
struct hash<nvrhi::AutoPtr<Ty>> {

    [[nodiscard]] size_t operator()(const nvrhi::AutoPtr<Ty> &key_val) const noexcept {
        return hash<typename nvrhi::AutoPtr<Ty>::InterfaceType*>()(key_val.Get());
    }
};

template<typename Ty>
struct hash<nvrhi::WeakPtr<Ty>> {
    [[nodiscard]] size_t operator()(const nvrhi::WeakPtr<Ty> &key_val) const noexcept {
        return hash<const Ty*>{}(key_val.UnsafeRawPtr());
    }
};

template <typename Ty, typename Dx>
struct hash<nvrhi::MonoPtr<Ty, Dx>>: public nvrhi::details::ConditionalEnabledHash<nvrhi::MonoPtr<Ty, Dx>,
    std::is_default_constructible_v<hash<typename nvrhi::MonoPtr<Ty, Dx>::pointer>>> {
    static size_t do_hash(const nvrhi::MonoPtr<Ty, Dx> &key_val) noexcept {
        return hash<typename nvrhi::MonoPtr<Ty, Dx>::pointer>{}(key_val.Get());
    }
};

}


#endif /* NVRHI_CORE_AUTOPTR_H */
