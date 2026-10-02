#ifndef NVRHI_CORE_TYPES_H
#define NVRHI_CORE_TYPES_H
#include <cstddef>
#include <cstdint>
#include <string.h>
#include <type_traits>

/// Unique identification structures
namespace nvrhi {

/// Unique interface identifier
struct GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];

    bool operator==(const GUID& rhs) const noexcept {
        return memcmp(&Data1, &rhs.Data1, sizeof(rhs)) == 0;
    }
    bool operator!=(const GUID& rhs) const noexcept { return !(*this == rhs); }
};

namespace details {
constexpr inline uint8_t ascii_to_hex(char c) { return c >= 97 ? c - 87 : c - 48; }

constexpr uint8_t str_to_uint8(const char* s) {
    return (ascii_to_hex(s[0]) << 4) | ascii_to_hex(s[1]);
}

constexpr uint16_t str_to_uint16(const char* s) {
    return (ascii_to_hex(s[0]) << 12) | (ascii_to_hex(s[1]) << 8) |
           (ascii_to_hex(s[2]) << 4) | ascii_to_hex(s[3]);
}

constexpr uint32_t str_to_uint32(const char* s) {
    return (ascii_to_hex(s[0]) << 28) | (ascii_to_hex(s[1]) << 24) |
           (ascii_to_hex(s[2]) << 20) | (ascii_to_hex(s[3]) << 16) |
           (ascii_to_hex(s[4]) << 12) | (ascii_to_hex(s[5]) << 8) |
           (ascii_to_hex(s[6]) << 4) | ascii_to_hex(s[7]);
}

constexpr GUID str_to_guid(const char* s) {
    return GUID{str_to_uint32(s),
                str_to_uint16(s + 9),
                str_to_uint16(s + 14),
                {str_to_uint8(s + 19), str_to_uint8(s + 21), str_to_uint8(s + 24),
                 str_to_uint8(s + 26), str_to_uint8(s + 28), str_to_uint8(s + 30),
                 str_to_uint8(s + 32), str_to_uint8(s + 34)}};
}
}  // namespace details

// GUID literals
namespace literals {
constexpr GUID operator"" _nvrhi_guid(const char* str, size_t /*N*/) {
    return details::str_to_guid(str);
}
}  // namespace literals

using namespace literals;

using FIID = GUID;
using FREFIID = const FIID&;
using FCLSID = GUID;
using FBOOL = int32_t;
using FLONG = int32_t;
using FRESULT = int32_t;

#define NVRHI_SUCCEEDED(hr) (((nvrhi::FRESULT)(hr)) >= 0)
#define NVRHI_FAILED(hr) (((nvrhi::FRESULT)(hr)) < 0)

// Expands to the IID of the pointee type of pInterface and pInterface as void**, e.g.
// pObject->QueryInterface(NVRHI_IID_PPV_ARGS(&spResult)).
#define NVRHI_IID_PPV_ARGS(pInterface) \
    ::nvrhi::uuid_of<std::decay_t<decltype(**(pInterface))>>(), (void**)(pInterface)

}  // namespace nvrhi

// NvrhiUUIDTraits maps an interface or class to its IID. It is global on purpose: NVRHI_IID and
// NVRHI_CCLSID/NVRHI_SCLSID specialize it from whatever namespace the interface is declared in, which
// MSVC accepts for a global template only (C2888 for a template in a named namespace).
#ifdef __clang__
// Since LLVM based code analysis tools does not recognize CWG727, we have to walk around
// this.
struct NvrhiUUIDTraits {
    template <typename Interface>
    static constexpr const nvrhi::GUID& uuid_of() {
        return Interface::this_uuid();
    }
};

#define NVRHI_IID(Interface, StrIID) \
    static constexpr nvrhi::GUID IID_##Interface = StrIID##_nvrhi_guid;

#define NVRHI_DECLARE_UUID_TRAITS(Interface) \
    friend struct ::NvrhiUUIDTraits;         \
    static constexpr const nvrhi::GUID& this_uuid() { return IID_##Interface; }

namespace nvrhi {
template <typename Interface>
constexpr const GUID& uuid_of(const Interface* = nullptr) {
    return ::NvrhiUUIDTraits::uuid_of<Interface>();
}
}  // namespace nvrhi

#else
// Note: NvrhiUUIDTraits must be declared in global namespace and specialized in any scope
template <typename Interface>
struct NvrhiUUIDTraits;

// Note: in order to specialization of NvrhiUUIDTraits in any namespace scope, Defect report CWG
// 727 must be used clang version must be greater or equal to 13.7.6.1
// The leading declaration puts Interface in the current namespace before the specialization names it:
// otherwise `struct Interface` would bind to a same-named interface of an enclosing namespace (or one
// brought in by a using-directive), e.g. to nvrhi::IDevice when declaring nvrhi::d3d12::IDevice.
#define NVRHI_IID(Interface, StrIID)                                              \
    struct Interface;                                                             \
    static constexpr nvrhi::GUID IID_##Interface = StrIID##_nvrhi_guid;           \
    template <>                                                                   \
    struct ::NvrhiUUIDTraits<struct Interface> {                                  \
        static constexpr const nvrhi::GUID& uuid_of() { return IID_##Interface; } \
    };

namespace nvrhi {
template <typename Interface>
constexpr const GUID& uuid_of(const Interface* = nullptr) {
    return ::NvrhiUUIDTraits<Interface>::uuid_of();
}
}  // namespace nvrhi

// For compatible with old version of gcc and clang
#define NVRHI_DECLARE_UUID_TRAITS(Interface)

#endif

namespace nvrhi::details {
// Records, inside an interface, the interface it derives from (see NVRHI_DECLARE_UUID_TRAITS_DERIVED).
// SelfType tells the interface's own declaration from one inherited from its parent.
template <typename Self, typename Parent>
struct QIInterfaceLink {
    using SelfType = Self;
    using ParentType = Parent;
};
}  // namespace nvrhi::details

// NVRHI_DECLARE_UUID_TRAITS for an interface that also answers QueryInterface for the IIDs of the
// interfaces it derives from. An object implementing Interface through ObjectImpl<...> (or listing it with
// NVRHI_IMPLEMENTS_INTERFACE_CHAIN in its own table) answers Interface, Parent, Parent's declared parent,
// and so on, each with the pointer to that interface:
//
//     NVRHI_IID(ITexture, "...")
//     struct ITexture : IRHIObject {
//         NVRHI_DECLARE_UUID_TRAITS_DERIVED(ITexture, IRHIObject)
//         ...
//     };
//
// Parent must be a base of Interface. An interface declared with plain NVRHI_DECLARE_UUID_TRAITS answers
// only its own IID (and IObject), whatever its parent declares.
#define NVRHI_DECLARE_UUID_TRAITS_DERIVED(Interface, Parent) \
    NVRHI_DECLARE_UUID_TRAITS(Interface)                     \
    using NvrhiQIInterfaceLink = ::nvrhi::details::QIInterfaceLink<Interface, Parent>;

namespace nvrhi {

/// Base interface for all dynamic objects in the engine
NVRHI_IID(IObject, "00000000-0000-0000-0000-000000000000")
struct IObject {
    NVRHI_DECLARE_UUID_TRAITS(IObject)
    /// Queries the specific interface.

    /// \param [in] IID - Unique identifier of the requested interface.
    /// \param [out] ppInterface - Memory address where the pointer to the requested
    /// interface will be written.
    ///                            If the interface is not supported, null pointer will
    ///                            be returned.
    /// \remark The method increments the number of strong references by 1. The
    /// interface must be
    ///         released by a call to Release() method when it is no longer needed.
    /// \remark With ppInterface == nullptr the method only reports whether the interface is
    ///         supported (FS_OK or FE_NOINTERFACE), without a pointer and without a reference.
    virtual FRESULT QueryInterface(FREFIID riid, void** ppInterface) = 0;

    /// Increments the number of strong references by 1.

    /// \remark This method is equivalent to GetReferenceCounters()->AddStrongRef().\n
    ///         The method is thread-safe and does not require explicit synchronization.
    /// \return The number of strong references after incrementing the counter.
    /// \note   In a multithreaded environment, the returned number may not be reliable
    ///         as other threads may simultaneously change the actual value of the
    ///         counter.
    virtual FLONG AddRef() = 0;

    /// Decrements the number of strong references by 1 and destroys the object when the
    /// counter reaches zero.

    /// \remark This method is equivalent to
    /// GetReferenceCounters()->ReleaseStrongRef().\n
    ///         The method is thread-safe and does not require explicit synchronization.
    /// \return The number of strong references after decrementing the counter.
    /// \note   In a multithreaded environment, the returned number may not be reliable
    ///         as other threads may simultaneously change the actual value of the
    ///         counter. The only reliable value is 0 as the object is destroyed when
    ///         the last strong reference is released.
    virtual FLONG Release() = 0;
};

NVRHI_IID(IWeakReference, "00000000-0000-0000-0000-000000000004")
struct IWeakReference : public IObject {
    NVRHI_DECLARE_UUID_TRAITS(IWeakReference)
    virtual FRESULT Resolve(FREFIID riid, void** ppv) = 0;

    virtual FLONG GetNumStrongRefs() const = 0;

    virtual FBOOL IsExpired() const = 0;
};

NVRHI_IID(IWeakReferenceSource, "00000000-0000-0000-0000-000000000005")
struct IWeakReferenceSource : public IObject {
    NVRHI_DECLARE_UUID_TRAITS(IWeakReferenceSource)
    /// Returns the weak reference of this object in *ppv with its reference counter
    /// incremented; the caller must Release() it. Does nothing if ppv is null.
    virtual void GetWeakReference(IWeakReference** ppv) = 0;
};

// Common Status Code
constexpr FRESULT FS_OK = 0;
constexpr FRESULT FE_GENERIC_ERROR = -1;
constexpr FRESULT FE_NOINTERFACE = -2;
constexpr FRESULT FE_NOT_IMPLEMENT = -3;
constexpr FRESULT FE_INVALID_ARGS = -4;
constexpr FRESULT FE_NOT_ALIVE_OBJECT = -4;
constexpr FRESULT FE_NOT_FOUND = -5;
constexpr FRESULT FE_WAIT_TIMEOUT = -6;

// ---- Type tests without RTTI (ADR 0006) ------------------------------------------------------------
// NVRHI is built without RTTI. Where code used to call dynamic_cast, it now calls QueryInterface with the
// IID of an interface, or with the class ID of an implementation class (NVRHI_CCLSID).
//
// QueryInterface(riid, nullptr) only reports whether the object answers riid. It returns no pointer and
// does not touch the reference count. Every interface table of the core object model accepts a null ppv.
namespace details {
// Answered by the base classes that own the reference count, and only while the object has strong
// references: ObjectImpl and WeakReferenceSourceImpl answer it themselves, and the delegating base classes
// forward it to their owner. QueryInterface(QIStrongRefProbeIID, nullptr) == FS_OK means that an
// AddRef/Release pair cannot destroy the object. The probe fails, without touching the reference count,
// while the object is being destroyed (strong count zero: its destructor, DestroyObject, a pre-destroy
// callback), while a WeakReferenceSourceImpl object is still being constructed, and for objects whose
// QueryInterface does not reach those base classes. The probe never returns a pointer: on success *ppv is
// null.
inline constexpr FIID QIStrongRefProbeIID = "474c2862-e881-40c2-bae4-dbc35600d2a6"_nvrhi_guid;

// The Debug check of checked_cast (<nvrhi/common/misc.h>): true when the object behind `from` answers
// uuid_of<T>() with exactly `to`, the static_cast of `from`.
//
// It never adds a reference to an object that may be dying, because QueryInterface adds one and the
// matching Release would destroy an object whose count was already zero a second time. So it asks the
// liveness probe first:
// - alive: QueryInterface(uuid_of<T>(), &pv), Release(), then compare pv with `to`;
// - not known to be alive: QueryInterface(uuid_of<T>(), nullptr). This checks the type only and touches
//   no reference count.
template <typename T, typename U>
bool QICastMatches(U* from, T* to) noexcept {
    using To = std::remove_cv_t<T>;
    using From = std::remove_cv_t<U>;
    static_assert(std::is_base_of_v<IObject, To> && std::is_base_of_v<IObject, From>,
                  "QICastMatches: both types must be nvrhi::IObject types");
    From* object = const_cast<From*>(from);
    const FIID& iid = uuid_of<To>();
    if (object->QueryInterface(QIStrongRefProbeIID, nullptr) != FS_OK)
        return object->QueryInterface(iid, nullptr) == FS_OK;

    void* pv = nullptr;
    if (object->QueryInterface(iid, &pv) != FS_OK) return false;
    object->Release();  // the reference QueryInterface added to this same object; the count stays >= 1
    return pv == static_cast<void*>(const_cast<To*>(to));
}
}  // namespace details

/// Binary data blob
// {F578FF0D-ABD2-4514-9D32-7CB454D4A73B}
NVRHI_IID(IDataBlob, "f578ff0d-abd2-4514-9d32-7cb454d4a73b")
struct IDataBlob : public IObject {
    /// Sets the size of the internal data buffer
    virtual void Resize(size_t NewSize) = 0;

    /// Returns the size of the internal data buffer
    virtual size_t GetSize() = 0;

    /// Returns the pointer to the internal data buffer
    virtual void* GetDataPtr() = 0;
};

// CreateBlob() and the other implementations: <nvrhi/core/DataBlob.h>

}  // namespace nvrhi

// Use nvrhi_guid literals
using namespace nvrhi::literals;


#endif /* NVRHI_CORE_TYPES_H */
