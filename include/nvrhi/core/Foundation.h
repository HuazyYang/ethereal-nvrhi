#ifndef NVRHI_CORE_FOUNDATION_H
#define NVRHI_CORE_FOUNDATION_H
#include <nvrhi/core/Threading.h>
#include <nvrhi/core/Types.h>
#include <nvrhi/core/Memory.h>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

// packing reference control block(WeakReferenceImpl) and object memory together
// so as to optimize memory allocation and cache missing.
// Default memory allocation strategy is packing them all together.
#ifndef NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
#define NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT 1
#endif

#ifdef __clang__
#define NVRHI_CCLSID(Class, StrCLSID) \
    static constexpr nvrhi::GUID IID_##Class = StrCLSID##_nvrhi_guid;
#define NVRHI_SCLSID(Class, StrCLSID) \
    static constexpr nvrhi::GUID IID_##Class = StrCLSID##_nvrhi_guid;
#else
#define NVRHI_CCLSID(Class, StrCLSID)                                          \
    static constexpr nvrhi::GUID IID_##Class = StrCLSID##_nvrhi_guid;      \
    template <>                                                                  \
    struct ::NvrhiUUIDTraits<class Class> {                                           \
        static constexpr const nvrhi::GUID& uuid_of() { return IID_##Class; } \
    };
#define NVRHI_SCLSID(Class, StrCLSID)                                         \
    static constexpr nvrhi::GUID IID_##Class = StrCLSID##_nvrhi_guid;         \
    template <>                                                               \
    struct ::NvrhiUUIDTraits<struct Class> {                                        \
        static constexpr const nvrhi::GUID& uuid_of() { return IID_##Class; } \
    };
#endif

#define NVRHI_BEGIN_INTERFACE_TABLE(ClassName)                                     \
    nvrhi::FRESULT ClassName::QueryInterface(nvrhi::FREFIID riid, void** ppv) { \
        typedef ClassName _ITCls;                                                     \
        static const nvrhi::details::INTERFACE_ENTRY inttable[] = {
#define NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ClassName)                            \
    nvrhi::FRESULT QueryInterface(nvrhi::FREFIID riid, void** ppv) override { \
        typedef ClassName _ITCls;                                                   \
        static const nvrhi::details::INTERFACE_ENTRY inttable[] = {
#define NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE(ClassName)                     \
    nvrhi::FRESULT ClassName::NonDelegatingQueryInterface(nvrhi::FREFIID riid, \
                                                             void** ppv) {           \
        typedef ClassName _ITCls;                                                    \
        static const nvrhi::details::INTERFACE_ENTRY inttable[] = {
#define NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(ClassName)               \
    nvrhi::FRESULT NonDelegatingQueryInterface(nvrhi::FREFIID riid, void** ppv) \
        override {                                                                    \
        typedef ClassName _ITCls;                                                     \
        static const nvrhi::details::INTERFACE_ENTRY inttable[] = {
#define NVRHI_IMPLEMENTS_ROUTE_PARENT(...) \
    {nullptr, &nvrhi::details::QIEntryFinder<__VA_ARGS__, false>, NVRHI_BASE_OFFSET(_ITCls, __VA_ARGS__)},

#define NVRHI_IMPLEMENTS_ROUTE_MEMBER(Member)                                        \
    {nullptr,                                                                           \
     &nvrhi::details::RouteMemberQueryInterface<::std::decay<decltype(Member)>::type>, \
     uint32_t(size_t(::std::addressof(this->Member)) -                                    \
              size_t(this))}, /* Note: we can not use offsetof here */

#define NVRHI_IMPLEMENTS_INTERFACE(Itf) \
    {&nvrhi::details::QIIIDOf<Itf>, NVRHI_ENTRY_IS_OFFSET, NVRHI_BASE_OFFSET(_ITCls, Itf)},

// Itf and the interfaces it derives from, as declared with NVRHI_DECLARE_UUID_TRAITS_DERIVED.
#define NVRHI_IMPLEMENTS_INTERFACE_CHAIN(Itf)                                                       \
    {&nvrhi::details::QIIIDOf<void>, &nvrhi::details::QIEntryFinder<nvrhi::details::QIChain<Itf>, false>, \
     NVRHI_BASE_OFFSET(_ITCls, Itf)},

#define NVRHI_IMPLEMENTS_INTERFACE_AS(req, Itf) \
    {&nvrhi::details::QIIIDOf<req>, NVRHI_ENTRY_IS_OFFSET, NVRHI_BASE_OFFSET(_ITCls, Itf)},

// The class's own class ID, answered with the class itself. The entry AddRefs through the class, so it does
// not depend on where the class's IObject lies in its layout.
#define NVRHI_IMPLEMENTS_CLASS(Class) \
    {&nvrhi::details::QIIIDOf<Class>, &nvrhi::details::QISelfEntry<Class>, 0},

// Class ID of an implementation class, in two parts. The class answers QueryInterface for its class ID;
// checked_cast verifies that in Debug builds, and it replaces dynamic_cast (ADR 0006):
//
//     NVRHI_CLASS_CLSID(Texture, "...")            // namespace scope, before the class
//     class Texture : public ObjectImpl<ITexture>
//     {
//     public:
//         NVRHI_CLASS_INTERFACE_TABLE(Texture)     // in a public section
//         ...
//     };
//
// NVRHI_CLASS_CLSID declares the class (NVRHI_CCLSID does not) and gives it the class ID. A struct uses
// NVRHI_SCLSID after its own forward declaration.
#define NVRHI_CLASS_CLSID(Class, StrCLSID) \
    class Class;                           \
    NVRHI_CCLSID(Class, StrCLSID)

// The class's uuid traits and an interface table that answers its class ID and routes everything else to
// its ObjectImpl / WeakReferenceSourceImpl base. A class that needs more entries writes the table out and
// lists NVRHI_IMPLEMENTS_CLASS(Class) in it.
#define NVRHI_CLASS_INTERFACE_TABLE(Class)    \
    NVRHI_DECLARE_UUID_TRAITS(Class)          \
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Class) \
    NVRHI_IMPLEMENTS_CLASS(Class)             \
    NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()

#define NVRHI_END_INTERFACE_TABLE()                                                 \
    { 0, (nvrhi::details::INTERFACE_FINDER)0, 0 }                                   \
    }                                                                                  \
    ;                                                                                  \
    return nvrhi::details::InterfaceTableQueryInterface(this, inttable, riid, ppv); \
    }

// Ends a table of a class derived from ObjectImpl<...> (or WeakReferenceSourceImpl / DelegatingObjectImpl /
// DelegatingWeakReferenceSourceImpl). What the table does not answer is routed to that base class: its
// interfaces, then, in declaration order, every base that implements QueryInterface (direct, non-virtual
// calls). IObject always goes to the base class, so identity is the base's.
// A class derived from another implementation Foo derives from ObjectImpl<Foo> (not from Foo directly):
// otherwise the route skips Foo's own table and goes straight to Foo's base class.
#define NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT() NVRHI_QI_END_ROUTE_TABLE_(false)

// Non-delegating side of an aggregated object (DelegatingObjectImpl / DelegatingWeakReferenceSourceImpl):
// bases are asked through their NonDelegatingQueryInterface; IObject stays with the owner.
#define NVRHI_END_NON_DELEGATING_INTERFACE_TABLE_ROUTE_PARENT() NVRHI_QI_END_ROUTE_TABLE_(true)

#define NVRHI_QI_END_ROUTE_TABLE_(ND)                                                \
    {nullptr, &nvrhi::details::QIRouteToCore<_ITCls, ND>, 0},                      \
    { 0, (nvrhi::details::INTERFACE_FINDER)0, 0 }                                 \
    }                                                                              \
    ;                                                                              \
    return nvrhi::details::QIRouteTableQueryInterface(this, inttable, riid, ppv); \
    }

#define NVRHI_DECLARE_INTERFACE_TABLE() \
    nvrhi::FRESULT QueryInterface(const nvrhi::FIID& riid, void** ppv) override;

namespace nvrhi {

template <class AllocatorType = IMemoryAllocator>
class MakeNewRCObj;

template <typename... Bases>
class WeakReferenceSourceImpl;

namespace details {
// Which base class family owns an object's reference count. The delegating ones answer their own
// interfaces through NonDelegatingQueryInterface.
enum class QILifetime : unsigned char { Object, Weak, Delegating, DelegatingWeak };
constexpr bool QIIsDelegating(QILifetime k) { return k >= QILifetime::Delegating; }

// The four base classes: root layer (owns the reference count) or pass-through layer (its single base
// already does). Defined further below.
template <QILifetime K, typename... Bases> class QIRootLayer;
template <QILifetime K, typename... Bases> class QIPassThroughLayer;

// The only name the four base classes put in user classes (as QITraits): the family that owns the
// reference count, and the layer itself.
template <QILifetime K, typename C>
struct QITraits {
    static constexpr QILifetime Lifetime = K;
    using Core = C;
};

typedef FRESULT (*INTERFACE_FINDER)(void* pThis, uint32_t data, FREFIID riid, void** ppv);

// One copy of each IID with vague (COMDAT) linkage and constant initialization: the same address in
// every translation unit, so interface tables in inline and template functions stay constant data.
// (IID_X from NVRHI_IID has internal linkage; MSVC guards a table that takes its address.)
template <typename Itf>
inline constexpr FIID QIIIDOf = uuid_of<Itf>();
// Key of an entry whose finder is asked for any IID (a mixin in a root table).
template <>
inline constexpr FIID QIIIDOf<void> = {};

struct INTERFACE_ENTRY {
    const FIID* pIID;
    INTERFACE_FINDER pfnFinder;
    uint32_t data;
};

template <typename QIB, bool ND>
FRESULT QIEntryFinder(void* pThis, uint32_t offset, FREFIID riid, void** ppv);

// ---- Interface chains (NVRHI_DECLARE_UUID_TRAITS_DERIVED) --------------------------------------------
// The parent an interface declares for itself, or void (none, or only one inherited from its parent).
template <typename Itf, typename = void>
struct QIParentOf {
    using type = void;
};
template <typename Itf>
struct QIParentOf<Itf, std::void_t<typename Itf::NvrhiQIInterfaceLink>> {
    using Link = typename Itf::NvrhiQIInterfaceLink;
    using type = std::conditional_t<std::is_same_v<typename Link::SelfType, Itf>, typename Link::ParentType, void>;
};
template <typename Itf>
inline constexpr bool QIHasParent = !std::is_void_v<typename QIParentOf<Itf>::type>;

// QIEntryFinder<QIChain<Itf>, ND>: the entry of an interface base that answers its whole chain.
template <typename Itf>
struct QIChain {};
template <typename T>
inline constexpr bool QIIsChain = false;
template <typename Itf>
inline constexpr bool QIIsChain<QIChain<Itf>> = true;

// The pointer to the interface of Itf's chain that riid names, or null. Each step is a static_cast, so a
// parent need not be the first base of its child.
template <typename Itf>
void* QIChainCast(Itf* p, FREFIID riid) {
    if (riid == QIIIDOf<Itf>) return p;
    using Parent = typename QIParentOf<Itf>::type;
    if constexpr (std::is_void_v<Parent>) {
        return nullptr;
    } else {
        static_assert(std::is_base_of_v<Parent, Itf>,
                      "NVRHI_DECLARE_UUID_TRAITS_DERIVED(Interface, Parent): Parent must be a base of Interface");
        return details::QIChainCast<Parent>(static_cast<Parent*>(p), riid);
    }
}

template <typename Itf>
FRESULT QIChainEntry(void* pItf, QIChain<Itf>, FREFIID riid, void** ppv) {
    Itf* p = static_cast<Itf*>(pItf);
    void* pv = details::QIChainCast<Itf>(p, riid);
    if (!pv) return FE_NOINTERFACE;
    if (ppv) {
        *ppv = pv;
        p->AddRef();
    }
    return FS_OK;
}

// NVRHI_IMPLEMENTS_CLASS: the object itself, as Cls.
template <typename Cls>
FRESULT QISelfEntry(void* pThis, uint32_t, FREFIID, void** ppv) {
    if (ppv) {
        Cls* p = static_cast<Cls*>(pThis);
        *ppv = p;
        p->AddRef();
    }
    return FS_OK;
}

// An offset entry. A real finder (QIEntryFinder below), so every table entry is an address constant.
#define NVRHI_ENTRY_IS_OFFSET (&nvrhi::details::QIEntryFinder<void, false>)

inline FRESULT InterfaceTableQueryInterface(void* pThis, const INTERFACE_ENTRY* pTable, FREFIID riid, void** ppv) {
    if (riid == IID_IObject) {
        // first entry must be an offset
        if (ppv) {
            *ppv = static_cast<char*>(pThis) + pTable->data;
            static_cast<IObject*>(*ppv)->AddRef();
        }
        return FS_OK;
    }

    FRESULT hr = FE_NOINTERFACE;
    while (pTable->pfnFinder) {
        if (!pTable->pIID || pTable->pIID == &QIIIDOf<void> || riid == *pTable->pIID) {
            if (pTable->pfnFinder == NVRHI_ENTRY_IS_OFFSET) {
                if (ppv) {
                    *ppv = static_cast<char*>(pThis) + pTable->data;
                    static_cast<IObject*>(*ppv)->AddRef();
                }
                hr = FS_OK;
                break;
            }
            hr = pTable->pfnFinder(pThis, pTable->data, riid, ppv);
            if (hr == FS_OK) break;
        }
        pTable++;
    }
    if (hr != FS_OK && ppv) *ppv = nullptr;
    return hr;
}

// The same walk for a table ended by ..._ROUTE_PARENT(); IObject goes to its last entry (the route).
inline FRESULT QIRouteTableQueryInterface(void* pThis, const INTERFACE_ENTRY* pTable, FREFIID riid, void** ppv) {
    if (riid == IID_IObject) {  // identity belongs to the base class: ask the route entry, the last one
        while (pTable[1].pfnFinder) pTable++;
        return pTable->pfnFinder(pThis, pTable->data, riid, ppv);
    }
    return details::InterfaceTableQueryInterface(pThis, pTable, riid, ppv);
}
#define NVRHI_ENTRY_ROUTE_BASECLASS uint32_t(-1)
// The base is variadic so that template bases with commas (ObjectImpl<Foo, IA>) pass through macros.
#define NVRHI_BASE_OFFSET(ClassName, ...)                            \
    uint32_t(reinterpret_cast<char*>(static_cast<__VA_ARGS__*>(      \
                 reinterpret_cast<ClassName*>(sizeof(ClassName)))) - \
             reinterpret_cast<char*>(sizeof(ClassName)))

// ---- Routing to parent classes ----------------------------------------------------------------------
// A qualified call Base::QueryInterface(...) does not dispatch: it binds to the declaration that name
// lookup finds from Base. &Base::QueryInterface is a pointer to member *of the declaring class*, so the
// deduced class tells what the call binds to:
//   IObject          only the root pure virtual: an interface, nothing to call (would not link)
//   any other class  an implementation (Base's own, or an ancestor's such as ObjectImpl<...>)
//   not deducible    ambiguous (two implementing bases), not public, or an overload set with a template
// This relies on nvrhi's rule that interfaces do not re-declare QueryInterface.
template <typename C> C* QIDeclarer(FRESULT (C::*)(FREFIID, void**));

enum : int { QINone, QIInterface, QIImpl, QIBad };
template <typename B, typename = void>
inline constexpr int QIKind = std::is_base_of_v<IObject, B> ? QIBad : QINone;
template <typename B>
inline constexpr int QIKind<B, std::void_t<decltype(details::QIDeclarer(&B::QueryInterface))>> =
    std::is_same_v<decltype(details::QIDeclarer(&B::QueryInterface)), IObject*> ? QIInterface : QIImpl;

template <typename B, typename = void> inline constexpr bool QIHasNonDelegating = false;
template <typename B>
inline constexpr bool QIHasNonDelegating<B, std::void_t<decltype(details::QIDeclarer(&B::NonDelegatingQueryInterface))>> = true;

// Table finder for the base at `offset`, shared by every class that lists it: a direct (qualified,
// non-virtual) call to its implementation, or nothing. B = void is the offset entry (the walker handles it
// inline; a call does the same).
template <typename QIB, bool ND>
FRESULT QIEntryFinder(void* pThis, uint32_t offset, FREFIID riid, void** ppv) {
    static_assert(ND || QIKind<QIB> != QIBad,
                  "Route parent: this base's QueryInterface is ambiguous (two implementing bases), not "
                  "public, or overloaded with a function template");
    if constexpr (std::is_void_v<QIB>) {
        if (ppv) {
            *ppv = static_cast<char*>(pThis) + offset;
            static_cast<IObject*>(*ppv)->AddRef();
        }
        return FS_OK;
    } else if constexpr (QIIsChain<QIB>) {
        return details::QIChainEntry(static_cast<char*>(pThis) + offset, QIB{}, riid, ppv);
    } else if constexpr (ND && QIHasNonDelegating<QIB>)
        return reinterpret_cast<QIB*>(static_cast<char*>(pThis) + offset)->QIB::NonDelegatingQueryInterface(riid, ppv);
    else if constexpr (!ND && QIKind<QIB> == QIImpl)
        return reinterpret_cast<QIB*>(static_cast<char*>(pThis) + offset)->QIB::QueryInterface(riid, ppv);
    else
        return FE_NOINTERFACE;
}

// ---- Root layer: one table, the shared walker -------------------------------------------------------
// Entry per base: an interface by offset, an interface with a declared parent (keyed on QIIIDOf<void>: any
// IID) through QIEntryFinder<QIChain<B>> (its IID and its ancestors' IIDs, same object), any other base
// (also keyed on any IID) through QIEntryFinder<B>. All are picked by type, not by ?: or constexpr pointer
// variables, which MSVC does not always fold: the table must stay constant data (no thread-safe
// initialization guard).
template <typename B> inline constexpr bool QIIsInterface = QIKind<B> == QIInterface;
template <typename B> inline constexpr bool QIIsPlainInterface = QIIsInterface<B> && !QIHasParent<B>;
template <typename B>
using QIRootEntryFinderOf =
    std::conditional_t<QIIsInterface<B>, std::conditional_t<QIHasParent<B>, QIChain<B>, void>, B>;

// Identity first (IObject through the first base), then the bases in declaration order.
template <bool ND, typename Root, typename B0, typename... Bs>
FRESULT QIQueryRoot(void* self, FREFIID riid, void** ppv) {
#define NVRHI_QI_ROOT_ENTRY_(B)                                                 \
    {&QIIIDOf<std::conditional_t<QIIsPlainInterface<B>, B, void>>,               \
     &QIEntryFinder<QIRootEntryFinderOf<B>, ND && !QIIsInterface<B>>,            \
     NVRHI_BASE_OFFSET(Root, B)}
    static const INTERFACE_ENTRY table[] = {
        {&QIIIDOf<IObject>, NVRHI_ENTRY_IS_OFFSET,
         uint32_t(reinterpret_cast<char*>(static_cast<IObject*>(static_cast<B0*>(reinterpret_cast<Root*>(sizeof(Root))))) -
                  reinterpret_cast<char*>(sizeof(Root)))},
        NVRHI_QI_ROOT_ENTRY_(B0), NVRHI_QI_ROOT_ENTRY_(Bs)..., {nullptr, (INTERFACE_FINDER)0, 0}};
#undef NVRHI_QI_ROOT_ENTRY_
    if constexpr (ND) {
        if (riid == IID_IObject) {  // the owner's
            if (ppv) *ppv = nullptr;
            return FE_NOINTERFACE;
        }
    }
    return details::InterfaceTableQueryInterface(self, table, riid, ppv);
}

template <typename B, typename = void> inline constexpr bool QIOwnsRefCount = false;
template <typename B> inline constexpr bool QIOwnsRefCount<B, std::void_t<typename B::QITraits>> = true;

template <QILifetime K, typename... Bases>
constexpr bool QICheckRootBases() {
    static_assert(sizeof...(Bases) > 0 && (std::is_base_of_v<IObject, Bases> && ...),
                  "ObjectImpl<Bases...>: list interfaces and classes implementing QueryInterface; "
                  "inherit helper classes directly");
    static_assert(!(QIOwnsRefCount<Bases> || ...),
                  "A base that already owns a reference count must be the only one: ObjectImpl<Base>");
    static_assert(((QIKind<Bases> != QIBad) && ...),
                  "Route parent: this base's QueryInterface is ambiguous (two implementing bases), not "
                  "public, or overloaded with a function template");
    static_assert(K == QILifetime::Object || K == QILifetime::Delegating ||
                      (std::is_base_of_v<IWeakReferenceSource, Bases> || ...),
                  "WeakReferenceSourceImpl<Bases...>: list IWeakReferenceSource (or an interface derived from it)");
    return true;
}

// The route entry that ..._ROUTE_PARENT() appends: a qualified call into the layer the table's class
// derives from (root: its table; pass-through: the base's QueryInterface).
template <typename Cls, bool ND>
FRESULT QIRouteToCore(void* pThis, uint32_t, FREFIID riid, void** ppv) {
    using QIClsTraits = typename Cls::QITraits;
    static_assert(ND || !details::QIIsDelegating(QIClsTraits::Lifetime),
                  "ROUTE_PARENT(): a delegating class answers through NonDelegatingQueryInterface; use "
                  "NVRHI_BEGIN/END_NON_DELEGATING_INTERFACE_TABLE...");
    static_assert(!ND || details::QIIsDelegating(QIClsTraits::Lifetime),
                  "NON_DELEGATING_..._ROUTE_PARENT(): only for DelegatingObjectImpl / DelegatingWeakReferenceSourceImpl");
    if constexpr (ND)
        return static_cast<Cls*>(pThis)->QIClsTraits::Core::NonDelegatingQueryInterface(riid, ppv);
    else
        return static_cast<Cls*>(pThis)->QIClsTraits::Core::QueryInterface(riid, ppv);
}

// NVRHI_IMPLEMENTS_ROUTE_MEMBER: the aggregated member (an object or a pointer to one).
template <typename QIT>
FRESULT RouteMemberQueryInterface(void* pThis, uint32_t data, FREFIID riid, void** ppv) {
    QIT& member = *reinterpret_cast<QIT*>(static_cast<char*>(pThis) + data);
    if constexpr (std::is_pointer_v<QIT>)
        return details::RouteMemberQueryInterface<std::remove_pointer_t<QIT>>(member, 0, riid, ppv);
    else
        return member.QIT::NonDelegatingQueryInterface(riid, ppv);
}

class ObjectWrapperBase {
 public:
    virtual void DestroyObject() = 0;
    virtual FRESULT QueryInterface(const FIID& iid, void** ppInterface) = 0;
    virtual void DeletePackedStorage(void* pWeakRef) noexcept = 0;
};

template <typename ObjectType, typename AllocatorType>
class ObjectWrapper : public ObjectWrapperBase {
 public:
    ObjectWrapper(ObjectType* pObject, AllocatorType* pAllocator) noexcept
        : m_pObject{pObject}, m_pAllocator{pAllocator} {}
    virtual void DestroyObject() override final {
        if (m_pAllocator) {
            m_pObject->~ObjectType();
            m_pAllocator->Free(m_pObject);
        } else {
            delete m_pObject;
        }
    }
    virtual FRESULT QueryInterface(const FIID& iid, void** ppInterface) override final {
        return m_pObject->QueryInterface(iid, ppInterface);
    }

    void DeletePackedStorage(void* /*pWeakRef*/) noexcept final {}

 private:
    // It is crucially important that the type of the pointer
    // is ObjectType and not IObject, since the latter
    // does not have virtual dtor.
    ObjectType* const m_pObject;
    AllocatorType* const m_pAllocator;
};

template <typename ObjectType, typename AllocatorType>
class PackedObjectWrapper : public ObjectWrapperBase {
 public:
    PackedObjectWrapper(ObjectType* pObject, AllocatorType* pAllocator) noexcept
        : m_pObject{pObject}, m_pAllocator{pAllocator} {}
    virtual void DestroyObject() override final {
        m_pObject->~ObjectType();
    }
    virtual FRESULT QueryInterface(const FIID& iid, void** ppInterface) override final {
        return m_pObject->QueryInterface(iid, ppInterface);
    }

    virtual void DeletePackedStorage(void* pWeakRef) noexcept final;

 private:
    // It is crucially important that the type of the pointer
    // is ObjectType and not IObject, since the latter
    // does not have virtual dtor.
    ObjectType* const m_pObject;
    AllocatorType* const m_pAllocator;
};

struct ObjectWrapperStorage {
    // The layout of every ObjectWrapper / PackedObjectWrapper (a vptr and two pointers), without
    // instantiating one for IObject (whose DestroyObject would delete through a non-virtual dtor).
    struct ObjectWrapperStub : ObjectWrapperBase {
        void* pObject;
        void* pAllocator;
    };
    static constexpr size_t ObjectWrapperBufferSize =
        sizeof(ObjectWrapperStub) / sizeof(size_t);
    alignas(ObjectWrapperStub) size_t val[ObjectWrapperBufferSize];
};

template<typename ...Itfs>
struct IsWeakReferenceSource;

template <typename Itf>
struct IsWeakReferenceSource<Itf> {
    static constexpr bool value =
        std::is_base_of<IWeakReferenceSource, Itf>::value ||
        std::is_same<IWeakReferenceSource, Itf>::value;
};

template<typename ... Itfs>
struct IsWeakReferenceSource {
    static constexpr bool value = (IsWeakReferenceSource<Itfs>::value || ...);
};

template <typename TInterface>
struct WeakRefTypeTrait;

}  // namespace details

class UserAllocated {
 protected:
    template <typename AllocatorType>
    friend class MakeNewRCObj;

    template <typename ObjectType, typename AllocatorType>
    friend class details::PackedObjectWrapper;
    template <typename ObjectType, typename AllocatorType>
    friend class details::ObjectWrapper;

    friend class DefaultMemoryAllocator;
    void operator delete(void* ptr) { GetDefaultMemAllocator()->Free(ptr); }

    template <typename AllocatorType>
    void operator delete(void* ptr, AllocatorType* Allocator) {
        return Allocator->Free(ptr);
    }

    void* operator new(size_t Size) { return GetDefaultMemAllocator()->Allocate(Size); }

    template <typename AllocatorType>
    void* operator new(size_t Size, AllocatorType* Allocator) {
        return Allocator->Allocate(Size);
    }
};

namespace details {

// This class controls the lifetime of a refcounted object
class WeakReferenceImpl final : public IWeakReference, public UserAllocated {
 public:
    FLONG AddRef() override final { return AddWeakRef(); }

    FLONG Release() override final { return ReleaseWeakRef(); }

    FRESULT QueryInterface(FREFIID riid, void** ppv) override final {
        if (riid == IID_IObject || riid == IID_IWeakReference) {
            if (ppv) {  // a null ppv only asks whether the interface is supported
                *ppv = this;
                this->AddRef();
            }
            return FS_OK;
        }
        if (ppv) *ppv = nullptr;
        return FE_NOINTERFACE;
    }

    FRESULT Resolve(FREFIID riid, void** ppv) override final {
        return QueryObject(riid, ppv);
    }

    FLONG AddStrongRef() {
        NVRHI_VERIFY(m_ObjectState.load() == ObjectState::Alive,
                        "Attempting to increment strong reference counter for a destroyed "
                        "or not initialized object!");
        NVRHI_VERIFY(
            m_ObjectWrapperBuffer.val[0] != 0 && m_ObjectWrapperBuffer.val[1] != 0,
            "Object wrapper is not initialized");
        return m_NumStrongReferences.fetch_add(+1, std::memory_order_relaxed) + 1;
    }

    template <class TPreObjectDestroy>
    FLONG ReleaseStrongRef(TPreObjectDestroy&& PreObjectDestroy) {
        NVRHI_VERIFY(m_ObjectState.load() == ObjectState::Alive,
                        "Attempting to decrement strong reference counter for an object "
                        "that is not alive");
        NVRHI_VERIFY(
            m_ObjectWrapperBuffer.val[0] != 0 && m_ObjectWrapperBuffer.val[1] != 0,
            "Object wrapper is not initialized");

        // Decrement strong reference counter without acquiring the lock.
        const auto RefCount = m_NumStrongReferences.fetch_add(-1) - 1;
        NVRHI_VERIFY(RefCount >= 0, "Inconsistent call to ReleaseStrongRef()");
        if (RefCount == 0) {
            PreObjectDestroy();
            TryDestroyObject();
        }

        return RefCount;
    }

    FLONG ReleaseStrongRef() {
        return ReleaseStrongRef([]() {});
    }

    FLONG AddWeakRef() {
        return m_NumWeakReferences.fetch_add(+1, std::memory_order_relaxed) + 1;
    }

    FLONG ReleaseWeakRef() {
        // Lock-free. m_NumWeakReferences includes one implicit reference owned by all
        // strong references together (see m_NumWeakReferences), so it can only reach zero
        // after TryDestroyObject() has destroyed the object and released that reference.
        //
        // Whoever decrements the counter to zero destroys the control block. For every
        // other caller the decrement is the last access to <this>, so no thread can touch
        // the control block after it is freed. acq_rel: the release half publishes this
        // thread's prior writes (e.g. object destruction), the acquire half makes all of
        // them visible to the thread that destroys the control block.
        //
        // Weak references created and released while the object is being constructed
        // (A ==sp==> B ---wp---> A, B.ctor throws) never drive the counter to zero either,
        // because the implicit reference is held from the very beginning.
        const auto NumWeakReferences =
            m_NumWeakReferences.fetch_add(-1, std::memory_order_acq_rel) - 1;
        NVRHI_VERIFY(NumWeakReferences >= 0, "Inconsistent call to ReleaseWeakRef()");

        if (NumWeakReferences == 0) {
            NVRHI_VERIFY(m_NumStrongReferences.load() == 0 &&
                             m_ObjectState.load() == ObjectState::Destroyed,
                         "The implicit weak reference must only be released after the "
                         "object is destroyed");
#if !NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
            NVRHI_VERIFY(
                m_ObjectWrapperBuffer.val[0] == 0 && m_ObjectWrapperBuffer.val[1] == 0,
                "Object wrapper must be null");
#endif
            SelfDestroy();
        }
        return NumWeakReferences;
    }

    FRESULT
    QueryObject(FREFIID riid, void** ppv) {
        if (m_ObjectState.load() != ObjectState::Alive)
            return FE_NOT_ALIVE_OBJECT;  // Early exit

        FRESULT hr = FS_OK;
        if (ppv) *ppv = nullptr;

        // It is essential to INCREMENT REF COUNTER while HOLDING THE LOCK to make sure that
        // StrongRefCnt > 1 guarantees that the object is alive.

        // If other thread started deleting the object in ReleaseStrongRef(), then
        // m_NumStrongReferences==0 We must make sure only one thread is allowed to
        // increment the counter to guarantee that if StrongRefCnt > 1, there is at least
        // one real strong reference left. Otherwise the following scenario may occur:
        //
        //                                      m_NumStrongReferences == 1
        //
        //    Thread 1 - ReleaseStrongRef()    |    Thread 2 - QueryObject()       | Thread
        //    3 - QueryObject()
        //                                     |                                   |
        //  - Decrement m_NumStrongReferences  | -Increment m_NumStrongReferences  |
        //  -Increment m_NumStrongReferences
        //  - Read RefCount == 0               | -Read StrongRefCnt==1             | -Read
        //  StrongRefCnt==2
        //    Destroy the object               |                                   | -Return
        //    reference to the soon
        //                                     |                                   |  to
        //                                     expire object
        //
        SpinLockGuard Guard{m_Lock};

        const auto StrongRefCnt = m_NumStrongReferences.fetch_add(+1) + 1;

        // Checking if m_ObjectState == ObjectState::Alive only is not reliable:
        //
        //           This thread                    |          Another thread
        //                                          |
        //   1. Acquire the lock                    |
        //                                          |    1. Decrement m_NumStrongReferences
        //   2. Increment m_NumStrongReferences     |    2. Test RefCount==0
        //   3. Read StrongRefCnt == 1              |    3. Start destroying the object
        //      m_ObjectState == ObjectState::Alive |
        //   4. DO NOT return the reference to      |    4. Wait for the lock, m_ObjectState
        //   == ObjectState::Alive
        //      the object                          |
        //   5. Decrement m_NumStrongReferences     |
        //                                          |    5. Destroy the object

        if (m_ObjectState == ObjectState::Alive && StrongRefCnt > 1) {
            NVRHI_VERIFY(
                m_ObjectWrapperBuffer.val[0] != 0 && m_ObjectWrapperBuffer.val[1] != 0,
                "Object wrapper is not initialized");
            // QueryInterface() must not lock the object, or a deadlock happens.
            // The only other method that locks the object is ReleaseStrongRef()
            // (via TryDestroyObject()), which is never called by QueryInterface()
            auto* pWrapper =
                reinterpret_cast<details::ObjectWrapperBase*>(&m_ObjectWrapperBuffer);
            hr = pWrapper->QueryInterface(riid, ppv);
        }
        m_NumStrongReferences.fetch_add(-1);

        return hr;
    }

    FLONG GetNumStrongRefs() const override { return m_NumStrongReferences.load(); }

    FBOOL IsExpired() const override {
        return !(m_NumStrongReferences.load() > 0 &&
                 m_ObjectState.load() == ObjectState::Alive);
    }

    // FLONG GetNumWeakRefs() const { return m_NumWeakReferences.load(); }

 private:
    template <QILifetime, typename...>
    friend class QIRootLayer;
    template <typename ObjectType, typename AllocatorType>
    friend class details::PackedObjectWrapper;
    template <typename AllocatorType>
    friend class nvrhi::MakeNewRCObj;

    WeakReferenceImpl() noexcept {}

    template <typename ObjectType, typename AllocatorType>
    void Attach(ObjectType* pObject, AllocatorType* pAllocator) noexcept {
        NVRHI_VERIFY(m_ObjectState.load() == ObjectState::NotInitialized,
                        "Object has already been attached");
#if NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
        static_assert(sizeof(details::ObjectWrapper<ObjectType, AllocatorType>) ==
                          sizeof(m_ObjectWrapperBuffer),
                      "Unexpected object wrapper size");

        new (&m_ObjectWrapperBuffer)
            details::PackedObjectWrapper<ObjectType, AllocatorType>{pObject, pAllocator};
        m_ObjectState.store(ObjectState::Alive);
#else
        static_assert(sizeof(details::ObjectWrapper<ObjectType, AllocatorType>) ==
                          sizeof(m_ObjectWrapperBuffer),
                      "Unexpected object wrapper size");

        new (&m_ObjectWrapperBuffer)
            details::ObjectWrapper<ObjectType, AllocatorType>{pObject, pAllocator};
        m_ObjectState.store(ObjectState::Alive);
#endif
    }

    void TryDestroyObject() {
        // clang-format off
        // Since RefCount==0, there are no more strong references and the only place
        // where strong ref counter can be incremented is from QueryObject().

        // If several threads were allowed to get to this point, there would
        // be serious risk that <this> had already been destroyed and m_LockFlag expired.
        // Consider the following scenario:
        //                                      |
        //             This thread              |             Another thread
        //                                      |
        //                      m_NumStrongReferences == 1
        //                      m_NumWeakReferences == 1
        //                                      |
        // 1. Decrement m_NumStrongReferences   |
        //    Read RefCount==0, no lock acquired|
        //                                      |   1. Run QueryObject()
        //                                      |      - acquire the lock
        //                                      |      - increment m_NumStrongReferences
        //                                      |      - release the lock
        //                                      |
        //                                      |   2. Run ReleaseWeakRef()
        //                                      |      - decrement m_NumWeakReferences
        //                                      |
        //                                      |   3. Run ReleaseStrongRef()
        //                                      |      - decrement m_NumStrongReferences
        //                                      |      - read RefCount==0
        //
        //         Both threads will get to this point. The first one will destroy <this>
        //         The second one will read expired m_LockFlag

        //  IT IS CRUCIALLY IMPORTANT TO ASSURE THAT ONLY ONE THREAD WILL EVER
        //  EXECUTE THIS CODE

        // The solution is to atomically increment strong ref counter in QueryObject().
        // There are two possible scenarios depending on who first increments the counter:


        //                                                     Scenario I
        //
        //             This thread              |     Another thread - QueryObject()        |  One more thread - QueryObject()
        //                                      |                                           |
        //                        m_NumStrongReferences == 1                                |
        //                                      |                                           |
        //                                      |   1. Acquire the lock                     |
        // 1. Decrement mlNumStrongReferences   |                                           |   1. Wait for the lock
        // 2. Read RefCount==0                  |   2. Increment m_NumStrongReferences      |
        // 3. Start destroying the object       |   3. Read StrongRefCnt == 1               |
        // 4. Wait for the lock                 |   4. DO NOT return the reference          |
        //                                      |      to the object                        |
        //                                      |   5. Decrement m_NumStrongReferences      |
        // _  _  _  _  _  _  _  _  _  _  _  _  _|   6. Release the lock _  _  _  _  _  _  _ |_  _  _  _  _  _  _  _  _  _  _  _  _  _
        //                                      |                                           |   2. Acquire the lock
        //                                      |                                           |   3. Increment m_NumStrongReferences
        //                                      |                                           |   4. Read StrongRefCnt == 1
        //                                      |                                           |   5. DO NOT return the reference
        //                                      |                                           |      to the object
        //                                      |                                           |   6. Decrement m_NumStrongReferences
        //  _  _  _  _  _  _  _  _  _  _  _  _  | _  _  _  _  _  _  _  _  _  _  _  _  _  _  | _ 7. Release the lock _  _  _  _  _  _
        // 5. Acquire the lock                  |                                           |
        //   - m_NumStrongReferences==0         |                                           |
        // 6. DESTROY the object                |                                           |
        //                                      |                                           |

        //  QueryObject() MUST BE SERIALIZED for this to work properly!


        //                                   Scenario II
        //
        //             This thread              |     Another thread - QueryObject()
        //                                      |
        //                       m_NumStrongReferences == 1
        //                                      |
        //                                      |   1. Acquire the lock
        //                                      |   2. Increment m_NumStrongReferences
        // 1. Decrement m_NumStrongReferences   |
        // 2. Read RefCount>0                   |
        // 3. DO NOT destroy the object         |   3. Read StrongRefCnt > 1 (while m_NumStrongReferences == 1)
        //                                      |   4. Return the reference to the object
        //                                      |       - Increment m_NumStrongReferences
        //                                      |   5. Decrement m_NumStrongReferences
        // clang-format on
#ifdef _DEBUG
        {
            auto NumStrongRefs = m_NumStrongReferences.load();
            NVRHI_VERIFY(NumStrongRefs == 0 || NumStrongRefs == 1,
                            "Num strong references (", NumStrongRefs,
                            ") is expected to be 0 or 1");
        }
#endif

        // Acquire the lock.
        std::unique_lock<SpinLock> Guard{m_Lock};

        // QueryObject() first acquires the lock, and only then increments and
        // decrements the ref counter. If it reads 1 after incrementing the counter,
        // it does not return the reference to the object and decrements the counter.
        // If we acquired the lock, QueryObject() will not start until we are done
        NVRHI_VERIFY(m_NumStrongReferences.load() == 0 &&
                        m_ObjectState.load() == ObjectState::Alive);

        // Extra caution
        if (m_NumStrongReferences.load() == 0 &&
            m_ObjectState.load() == ObjectState::Alive) {
            NVRHI_VERIFY(
                m_ObjectWrapperBuffer.val[0] != 0 && m_ObjectWrapperBuffer.val[1] != 0,
                "Object wrapper is not initialized");
            // We cannot destroy the object while reference counters are locked as this will
            // cause a deadlock in cases like this:
            //
            //    A ==sp==> B ---wp---> A
            //
            //    RefCounters_A.Lock();
            //    delete A{
            //      A.~dtor(){
            //          B.~dtor(){
            //              wpA.Lock() -> QueryObject(){
            //                  RefCounters_A.Lock(); // Deadlock
            //

#if NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
            auto* const pWrapper =
                reinterpret_cast<details::ObjectWrapperBase*>(&m_ObjectWrapperBuffer);
#else
            // So we copy the object wrapper and destroy the object after unlocking the
            // reference counters
            details::ObjectWrapperStorage ObjectWrapperBufferCopy;
            memcpy(&ObjectWrapperBufferCopy, &m_ObjectWrapperBuffer,
                   sizeof(m_ObjectWrapperBuffer));
            memset(&m_ObjectWrapperBuffer, 0, sizeof(m_ObjectWrapperBuffer));

            auto* pWrapper =
                reinterpret_cast<details::ObjectWrapperBase*>(&ObjectWrapperBufferCopy);
#endif

            // Note that this is the only place where m_ObjectState is
            // modified after the ref counters object has been created.
            // QueryObject() checks the state under the lock, so after this store no
            // new strong reference can be handed out.
            m_ObjectState.store(ObjectState::Destroyed);

            // We must explicitly unlock the object now to avoid deadlocks: the object
            // dtor may resolve weak references to this very object (QueryObject()).
            Guard.unlock();

            // <this> stays alive while the object is being destroyed: this thread still
            // owns the implicit weak reference, so m_NumWeakReferences >= 1 no matter
            // which weak references the dtor (or other threads) release meanwhile.
            pWrapper->DestroyObject();

            // Release the implicit weak reference. This is the last access to <this> on
            // this path.
            //
            // Fast path (as in libstdc++'s shared_ptr): if only the implicit reference
            // is left, nobody else can hold or obtain a reference any more (strong == 0,
            // state == Destroyed, no external weak references), so the atomic RMW can be
            // skipped. The acquire load pairs with the release half of a concurrent
            // ReleaseWeakRef() that took the counter from 2 to 1; that thread no longer
            // touches <this> after its decrement.
            if (m_NumWeakReferences.load(std::memory_order_acquire) == 1) {
                m_NumWeakReferences.store(0, std::memory_order_relaxed);
                SelfDestroy();
            } else {
                ReleaseWeakRef();
            }
        }
    }

    void SelfDestroy() {

#if NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
        auto ObjWrappStorageCopy = m_ObjectWrapperBuffer;
        auto pWrapper = reinterpret_cast<details::ObjectWrapperBase*>(&ObjWrappStorageCopy);
        pWrapper->DeletePackedStorage(this);
#else
        delete this;
#endif
    }

    ~WeakReferenceImpl() {
        NVRHI_VERIFY(
            m_NumStrongReferences.load() == 0 && m_NumWeakReferences.load() == 0,
            "There exist outstanding references to the object being destroyed");
    }

    // No copies/moves
    // clang-format off
    WeakReferenceImpl             (const WeakReferenceImpl&)  = delete;
    WeakReferenceImpl             (      WeakReferenceImpl&&) = delete;
    WeakReferenceImpl& operator = (const WeakReferenceImpl&)  = delete;
    WeakReferenceImpl& operator = (      WeakReferenceImpl&&) = delete;
    // clang-format on

    std::atomic<FLONG> m_NumStrongReferences{1};
    // External weak references + 1. The extra (implicit) reference is owned collectively
    // by all strong references and is released by TryDestroyObject() after the object
    // is destroyed. Whoever decrements this counter to zero destroys the control block.
    std::atomic<FLONG> m_NumWeakReferences{1};

    // Serializes QueryObject() against TryDestroyObject() only. It is never used to
    // decide the lifetime of the control block.
    SpinLock m_Lock;

    enum class ObjectState : uint32_t { NotInitialized = 0, Alive = 1, Destroyed = 2 };
    std::atomic<ObjectState> m_ObjectState{ObjectState::NotInitialized};

    ObjectWrapperStorage m_ObjectWrapperBuffer{};
};

template<typename ObjectType, typename AllocatorType>
void PackedObjectWrapper<ObjectType, AllocatorType>::DeletePackedStorage(void *pWeakRef) noexcept {
    reinterpret_cast<WeakReferenceImpl*>(pWeakRef)->~WeakReferenceImpl();
    if (m_pAllocator) {
        m_pAllocator->Free(m_pObject);
    } else
        delete (const ObjectType*)m_pObject;
}

// ---- Root layers: the base class owns the reference count -------------------------------------------
// Bases are the interfaces the class implements (IObject or interfaces derived from it) and mixins that
// implement QueryInterface with their own table, answered in declaration order by one table (QIQueryRoot).

template <typename... QIBases>
class QIRootLayer<QILifetime::Object, QIBases...> : public QIBases..., protected UserAllocated {
    static_assert(QICheckRootBases<QILifetime::Object, QIBases...>());

 public:
    using QITraits = nvrhi::details::QITraits<QILifetime::Object, QIRootLayer>;

    QIRootLayer() {}

    FRESULT QueryInterface(FREFIID riid, void** ppv) override {
        const FRESULT hr = QIQueryRoot<false, QIRootLayer, QIBases...>(static_cast<void*>(this), riid, ppv);
        if (hr != FS_OK && riid == QIStrongRefProbeIID)  // the liveness probe, see Types.h
            return m_NumStrongReferences.load() > 0 ? FS_OK : FE_NOT_ALIVE_OBJECT;
        return hr;
    }

    FLONG AddRef() override final {
        FLONG RefCount = m_NumStrongReferences.fetch_add(+1, std::memory_order_relaxed) + 1;
        return RefCount;
    }

    FLONG Release() override final {
        FLONG RefCount = m_NumStrongReferences.fetch_add(-1) - 1;
        if (RefCount == 0) DestroyObject();
        return RefCount;
    }

    void DestroyObject() {
        auto ObjWrapperStorageCopy = m_ObjWrapperStorage;
        auto pWrapper = reinterpret_cast<ObjectWrapperBase*>(&ObjWrapperStorageCopy);
        pWrapper->DestroyObject();
    }

 private:
    template <typename AllocatorType>
    friend class nvrhi::MakeNewRCObj;
    template <typename ObjectType, typename AllocatorType>
    friend class ObjectWrapper;

    template <typename ObjectType, typename AllocatorType>
    void Attach(ObjectType* pObject, AllocatorType* pAllocator) throw() {
        static_assert(sizeof(ObjectWrapper<ObjectType, AllocatorType>) == sizeof(m_ObjWrapperStorage),
                      "Unexpected object wrapper size");
        new (&m_ObjWrapperStorage) ObjectWrapper<ObjectType, AllocatorType>{pObject, pAllocator};
    }

    QIRootLayer(const QIRootLayer&) = delete;
    QIRootLayer(QIRootLayer&&) = delete;
    QIRootLayer& operator=(const QIRootLayer&) = delete;
    QIRootLayer& operator=(QIRootLayer&&) = delete;

    std::atomic<FLONG> m_NumStrongReferences{1};
    ObjectWrapperStorage m_ObjWrapperStorage{};
};

template <typename... QIBases>
class QIRootLayer<QILifetime::Weak, QIBases...> : public QIBases..., protected UserAllocated {
    static_assert(QICheckRootBases<QILifetime::Weak, QIBases...>());

 public:
    using QITraits = nvrhi::details::QITraits<QILifetime::Weak, QIRootLayer>;

#if NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
    // Constructor with weak reference syntax
    QIRootLayer() noexcept { ::new (&m_Storage.WeakRef) WeakReferenceImpl{}; }
#else
    QIRootLayer() noexcept {
        m_Storage.pWeakRef = (WeakReferenceImpl*)(*(uintptr_t*)((uint8_t*)this + offsetof(QIRootLayer, m_Storage)));
    }
#endif

    // Virtual destructor makes sure all derived classes can be destroyed
    // through the pointer to the base class
    virtual ~QIRootLayer() {
        // m_pWeakRef stays valid while the dtor runs when the object is destroyed via
        // ReleaseStrongRef(): TryDestroyObject() holds the implicit weak reference until
        // the dtor returns.
    }

    inline virtual FLONG AddRef() override final {
        // Since type of m_pWeakRef is WeakReference,
        // this call will not be virtual and should be inlined
        return GetWeakReferenceImpl()->AddStrongRef();
    }

    // Not final: derived classes may override it to run code before destruction
    // (see Release(TPreObjectDestroy&&) below).
    inline virtual FLONG Release() override {
        // Since type of m_pWeakRef is WeakReference,
        // this call will not be virtual and should be inlined
        return GetWeakReferenceImpl()->ReleaseStrongRef();
    }

    template <class TPreObjectDestroy>
    inline FLONG Release(TPreObjectDestroy&& PreObjectDestroy) {
        return GetWeakReferenceImpl()->ReleaseStrongRef(std::forward<TPreObjectDestroy>(PreObjectDestroy));
    }

    FRESULT QueryInterface(FREFIID riid, void** ppv) override {
        const FRESULT hr = QIQueryRoot<false, QIRootLayer, QIBases...>(static_cast<void*>(this), riid, ppv);
        if (hr != FS_OK && riid == QIStrongRefProbeIID)  // the liveness probe, see Types.h
            return GetWeakReferenceImpl()->IsExpired() ? FE_NOT_ALIVE_OBJECT : FS_OK;
        return hr;
    }

    void GetWeakReference(IWeakReference** ppv) override final {
        if (ppv) {
            auto pWeakRef = GetWeakReferenceImpl();
            pWeakRef->AddRef();
            *ppv = pWeakRef;
        }
    }

    WeakReferenceImpl* GetWeakReferenceImpl() {
#if NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
        return &m_Storage.WeakRef;
#else
        return m_Storage.pWeakRef;
#endif
    }

 protected:
    template <typename ObjectType, typename AllocatorType>
    friend class PackedObjectWrapper;
    template <typename ObjectType, typename AllocatorType>
    friend class ObjectWrapper;
    template <typename AllocatorType>
    friend class nvrhi::MakeNewRCObj;

    friend class WeakReferenceImpl;

    template <typename ObjectType>
    friend struct WeakRefTypeTrait;  // Used for get implement object type of IWeakReference.

    using WeakRefImplType = WeakReferenceImpl;

 private:
    QIRootLayer(const QIRootLayer&) = delete;
    QIRootLayer(QIRootLayer&&) = delete;
    QIRootLayer& operator=(const QIRootLayer&) = delete;
    QIRootLayer& operator=(QIRootLayer&&) = delete;

    template <typename ObjectType, typename AllocatorType>
    void Attach(ObjectType* pObject, AllocatorType* pAllocator) noexcept {
#if NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
        m_Storage.WeakRef.template Attach<ObjectType, AllocatorType>(pObject, pAllocator);
#else
        m_Storage.pWeakRef->template Attach<ObjectType, AllocatorType>(pObject, pAllocator);
#endif
    }

    // Note that the type of the reference counters is WeakReference,
    // not IWeakReference. This avoids virtual calls from
    // AddRef() and Release() methods
    union WeakReferenceImplStorage {
        WeakReferenceImplStorage() {}
        ~WeakReferenceImplStorage() {}
#if NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
        WeakReferenceImpl WeakRef;
#else
        WeakReferenceImpl* pWeakRef;
#endif
    } m_Storage;
};

template <typename... QIBases>
class QIRootLayer<QILifetime::Delegating, QIBases...> : public QIBases..., protected UserAllocated {
    static_assert(QICheckRootBases<QILifetime::Delegating, QIBases...>());

 public:
    using QITraits = nvrhi::details::QITraits<QILifetime::Delegating, QIRootLayer>;

    QIRootLayer(IObject* pOwner) : m_pOwner(pOwner) {}

    FLONG AddRef() override final { return m_pOwner->AddRef(); }

    FLONG Release() override final { return m_pOwner->Release(); }

    FRESULT QueryInterface(FREFIID riid, void** ppv) override { return m_pOwner->QueryInterface(riid, ppv); }

    virtual FRESULT NonDelegatingQueryInterface(FREFIID riid, void** ppv) {
        return QIQueryRoot<true, QIRootLayer, QIBases...>(static_cast<void*>(this), riid, ppv);
    }

    void DestroyObject() {
        auto ObjWrapperStorageCopy = m_ObjWrapperStorage;
        auto pWrapper = reinterpret_cast<ObjectWrapperBase*>(&ObjWrapperStorageCopy);
        pWrapper->DestroyObject();
    }

 protected:
    template <typename ObjectType, typename AllocatorType>
    friend class ObjectWrapper;

    IObject* m_pOwner;

 private:
    template <typename AllocatorType>
    friend class nvrhi::MakeNewRCObj;

    template <typename ObjectType, typename AllocatorType>
    void Attach(ObjectType* pObject, AllocatorType* pAllocator) throw() {
        static_assert(sizeof(ObjectWrapper<ObjectType, AllocatorType>) == sizeof(m_ObjWrapperStorage),
                      "Unexpected object wrapper size");
        new (&m_ObjWrapperStorage) ObjectWrapper<ObjectType, AllocatorType>{pObject, pAllocator};
    }

    QIRootLayer(const QIRootLayer&) = delete;
    QIRootLayer(QIRootLayer&&) = delete;
    QIRootLayer& operator=(const QIRootLayer&) = delete;
    QIRootLayer& operator=(QIRootLayer&&) = delete;

    ObjectWrapperStorage m_ObjWrapperStorage{};
};

template <typename... QIBases>
class QIRootLayer<QILifetime::DelegatingWeak, QIBases...> : public QIBases..., protected UserAllocated {
    static_assert(QICheckRootBases<QILifetime::DelegatingWeak, QIBases...>());

 public:
    using QITraits = nvrhi::details::QITraits<QILifetime::DelegatingWeak, QIRootLayer>;

    QIRootLayer(IWeakReferenceSource* pOwner) : m_pOwner(pOwner) {}

    FLONG AddRef() override final { return m_pOwner->AddRef(); }

    FLONG Release() override final { return m_pOwner->Release(); }

    FRESULT QueryInterface(FREFIID riid, void** ppv) override { return m_pOwner->QueryInterface(riid, ppv); }

    void GetWeakReference(IWeakReference** ppv) override { return m_pOwner->GetWeakReference(ppv); }

    virtual FRESULT NonDelegatingQueryInterface(FREFIID riid, void** ppv) {
        return QIQueryRoot<true, QIRootLayer, QIBases...>(static_cast<void*>(this), riid, ppv);
    }

    void DestroyObject() {
        auto ObjWrapperStorageCopy = m_ObjWrapperStorage;
        auto pWrapper = reinterpret_cast<ObjectWrapperBase*>(&ObjWrapperStorageCopy);
        pWrapper->DestroyObject();
    }

 protected:
    template <typename ObjectType, typename AllocatorType>
    friend class ObjectWrapper;

    IWeakReferenceSource* m_pOwner;

 private:
    template <typename AllocatorType>
    friend class nvrhi::MakeNewRCObj;

    template <typename ObjectType, typename AllocatorType>
    void Attach(ObjectType* pObject, AllocatorType* pAllocator) throw() {
        static_assert(sizeof(ObjectWrapper<ObjectType, AllocatorType>) == sizeof(m_ObjWrapperStorage),
                      "Unexpected object wrapper size");
        new (&m_ObjWrapperStorage) ObjectWrapper<ObjectType, AllocatorType>{pObject, pAllocator};
    }

    QIRootLayer(const QIRootLayer&) = delete;
    QIRootLayer(QIRootLayer&&) = delete;
    QIRootLayer& operator=(const QIRootLayer&) = delete;
    QIRootLayer& operator=(QIRootLayer&&) = delete;

    ObjectWrapperStorage m_ObjWrapperStorage{};
};

// ---- Pass-through layer: the single base already owns the reference count -----------------------------
// Bar : ObjectImpl<Foo> where Foo derives from ObjectImpl<...>: no second reference count, no
// new vptr. ROUTE_PARENT() in Bar calls Foo's QueryInterface (or NonDelegatingQueryInterface).
template <QILifetime QIK, typename QIB0>
class QIPassThroughLayer<QIK, QIB0> : public QIB0 {
    static_assert(QIB0::QITraits::Lifetime == QIK,
                  "The base that owns the reference count must be of the same family (ObjectImpl / "
                  "WeakReferenceSourceImpl / DelegatingObjectImpl / DelegatingWeakReferenceSourceImpl)");

 public:
    using QITraits = nvrhi::details::QITraits<QIK, QIPassThroughLayer>;
    using QIB0::QIB0;
};

template <QILifetime QIK, typename... QIBases>
using QILayer = std::conditional_t<sizeof...(QIBases) == 1 && (QIOwnsRefCount<QIBases> && ...),
                                 QIPassThroughLayer<QIK, QIBases...>, QIRootLayer<QIK, QIBases...>>;

}  // namespace details

/// Base classes of reference-counted objects. Bases are the class's interfaces (IObject or interfaces
/// derived from it) and classes implementing QueryInterface (mixins), or - for a class derived from an
/// existing implementation - that implementation alone:
///
///     class Foo : public ObjectImpl<IA, IB> { ... };    // owns the reference count
///     class Bar : public ObjectImpl<Foo> { ... };        // routes to Foo, adds no reference count
///
/// A class that ends its interface table with NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT() routes everything
/// its table does not answer to these bases.
// In the bodies below, nvrhi::details rather than details: MSVC also searches the (user) bases for names.
template <typename... QIBases>
class ObjectImpl : public details::QILayer<details::QILifetime::Object, QIBases...> {
    using QILayer = nvrhi::details::QILayer<nvrhi::details::QILifetime::Object, QIBases...>;

 public:
    using QILayer::QILayer;
};

/// Like ObjectImpl, with weak references: one of the bases is IWeakReferenceSource (or derived from it).
template <typename... QIBases>
class WeakReferenceSourceImpl : public details::QILayer<details::QILifetime::Weak, QIBases...> {
    using QILayer = nvrhi::details::QILayer<nvrhi::details::QILifetime::Weak, QIBases...>;

 public:
    using QILayer::QILayer;
};

/// An aggregated object: AddRef/Release/QueryInterface go to the owner; the owner reaches this object's
/// own interfaces through NonDelegatingQueryInterface.
template <typename... QIBases>
class DelegatingObjectImpl : public details::QILayer<details::QILifetime::Delegating, QIBases...> {
    using QILayer = nvrhi::details::QILayer<nvrhi::details::QILifetime::Delegating, QIBases...>;

 public:
    using QILayer::QILayer;
};

template <typename... QIBases>
class DelegatingWeakReferenceSourceImpl : public details::QILayer<details::QILifetime::DelegatingWeak, QIBases...> {
    using QILayer = nvrhi::details::QILayer<nvrhi::details::QILifetime::DelegatingWeak, QIBases...>;

 public:
    using QILayer::QILayer;
};

template <typename... Itfs>
using RuntimeClass = std::conditional_t<details::IsWeakReferenceSource<Itfs...>::value,
                                        WeakReferenceSourceImpl<Itfs...>, ObjectImpl<Itfs...>>;

template <typename... Itfs>
using RuntimeProxyClass =
    std::conditional_t<details::IsWeakReferenceSource<Itfs...>::value, DelegatingWeakReferenceSourceImpl<Itfs...>,
                       DelegatingObjectImpl<Itfs...>>;

template <typename AllocatorType>
class MakeNewRCObj {
 public:
    MakeNewRCObj(AllocatorType* Allocator) noexcept : m_pAllocator{Allocator} {}

    // clang-format off
    MakeNewRCObj           (const MakeNewRCObj&)  = delete;
    MakeNewRCObj           (      MakeNewRCObj&&) = delete;
    MakeNewRCObj& operator=(const MakeNewRCObj&)  = delete;
    MakeNewRCObj& operator=(      MakeNewRCObj&&) = delete;
    // clang-format on

    template <typename ObjectType, typename... CtorArgTypes>
    ObjectType* RcNew(CtorArgTypes&&... CtorArgs) const {
        return RcNewImpl<ObjectType>(0, std::forward<CtorArgTypes>(CtorArgs)...);
    }

    template <typename ObjectType, typename OwnerType, typename... CtorArgTypes>
    ObjectType* RcNewDelegating(OwnerType* pOwner, CtorArgTypes&&... CtorArgs) const {
        return RcNewDelegatingImpl<ObjectType>(pOwner,
                                               std::forward<CtorArgTypes>(CtorArgs)...);
    }

 private:
    template <typename ObjectType>
    struct ObjectTypeStorage : public UserAllocated {
        using StorageType =
            typename std::aligned_storage<sizeof(ObjectType), alignof(ObjectType)>::type;
        StorageType Storage;
    };

    // SFINEA overload for IWeakReferenceSoure kind object type
    template <typename Tp, typename... CtorArgTypes>
    Tp* RcNewImpl(
        typename std::enable_if<details::IsWeakReferenceSource<Tp>::value, int>::type,
        CtorArgTypes&&... CtorArgs) const {
#if NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
        Tp* pObj = nullptr;
        details::WeakReferenceImpl* pWeakRef = nullptr;

        try {
            if (m_pAllocator)
                pObj = new (m_pAllocator) Tp(std::forward<CtorArgTypes>(CtorArgs)...);
            else
                pObj = new Tp(std::forward<CtorArgTypes>(CtorArgs)...);

            pWeakRef = pObj->GetWeakReferenceImpl();
            pWeakRef->Attach(pObj, m_pAllocator);
        } catch (...) {
            if (pWeakRef) {
                pWeakRef->m_NumStrongReferences = 0;
                pWeakRef->m_NumWeakReferences = 0;  // drop the implicit weak reference
                // Obviously, control block is initialized.
                if (m_pAllocator) {
                    pWeakRef->~WeakReferenceImpl();
                    m_pAllocator->Free(pObj);
                } else
                    delete pObj;
            }
            throw;
        }

        return pObj;
#else
        using MyObjectStorage = ObjectTypeStorage<Tp>;

        details::WeakReferenceImpl* pWeakRef = nullptr;
        MyObjectStorage* pMem = nullptr;
        Tp *pObj = nullptr;
        try {
           pWeakRef = new details::WeakReferenceImpl;

            if(m_pAllocator)
                pMem = new (m_pAllocator)MyObjectStorage;
            else
                pMem = new MyObjectStorage;

            ((Tp *)pMem)->m_Storage.pWeakRef = pWeakRef;
            pObj = ::new (pMem) Tp(std::forward<CtorArgTypes>(CtorArgs)...);

            pWeakRef->Attach<Tp, AllocatorType>(pObj, m_pAllocator);
        } catch (...) {
            if(pWeakRef) {
                pWeakRef->m_NumStrongReferences = 0;
                pWeakRef->m_NumWeakReferences = 0;  // drop the implicit weak reference
                pWeakRef->SelfDestroy();
            }

            if(pMem) {
                if(m_pAllocator)
                    m_pAllocator->Free(pMem);
                else
                    delete pMem;
            }

            throw;
        }

        return pObj;
#endif
    }

    // SFINEA overload for IObject (but non-IWeakReferenceSource) kind object type
    template <typename Tp, typename... CtorArgTypes>
    Tp* RcNewImpl(
        typename std::enable_if<!details::IsWeakReferenceSource<Tp>::value, int>::type,
        CtorArgTypes&&... CtorArgs) const {
        Tp* pObj = nullptr;
        try {
            // Operators new and delete of RefCountedObject are private and only accessible
            // by methods of MakeNewRCObj
            if (m_pAllocator)
                pObj = new (m_pAllocator) Tp{std::forward<CtorArgTypes>(CtorArgs)...};
            else
                pObj = new Tp{std::forward<CtorArgTypes>(CtorArgs)...};

            pObj->template Attach<Tp, AllocatorType>(pObj, m_pAllocator);
        } catch (...) {
            throw;
        }

        return pObj;
    }

    template <typename ObjectType, typename OwnerType, typename... CtorArgTypes,
              std::enable_if_t<std::is_base_of_v<IObject, OwnerType>, int> = 0>
    ObjectType* RcNewDelegatingImpl(OwnerType* pOwner, CtorArgTypes&&... CtorArgs) const {

        ObjectType* pObj = nullptr;
        try {
            // Operators new and delete of RefCountedObject are private and only accessible
            // by methods of MakeNewRCObj
            if (m_pAllocator)
                pObj = new (m_pAllocator)
                    ObjectType{pOwner, std::forward<CtorArgTypes>(CtorArgs)...};
            else
                pObj = new ObjectType{pOwner, std::forward<CtorArgTypes>(CtorArgs)...};

            pObj->template Attach<ObjectType, AllocatorType>(pObj, m_pAllocator);
        } catch (...) {
            throw;
        }

        return pObj;
    }

    AllocatorType* m_pAllocator;
};

#define MAKE_GENERIC_RC_OBJ(Allocator, Type, ...)                                        \
    nvrhi::MakeNewRCObj<typename std::remove_reference<decltype(Allocator)>::type>( \
        Allocator)                                                                  \
        .RcNew<Type>
#define MAKE_GENERIC_RC_OBJ_TR(Allocator, Type, ...) \
    nvrhi::TakeOver(MAKE_GENERIC_RC_OBJ(Allocator, Type, ##__VA_ARGS__))

#define MAKE_RC_OBJ(Type, ...)                                                           \
    nvrhi::MakeNewRCObj<nvrhi::DefaultMemoryAllocator>(nvrhi::GetDefaultMemAllocator()) \
        .RcNew<Type>(__VA_ARGS__)
#define MAKE_RC_OBJ_PTR(Type, ...) nvrhi::TakeOver(MAKE_RC_OBJ(Type, ##__VA_ARGS__))

#define MAKE_GENERIC_RC_DELEGATING(Allocator, Type, ...)                            \
    nvrhi::MakeNewRCObj<typename std::remove_reference<decltype(Allocator)>::type>( \
        Allocator)                                                                  \
        .RcNewDelegating<Type>(__VA_ARGS__)
#define MAKE_GENERIC_RC_DELEGATING_PTR(Allocator, Type, ...) \
    nvrhi::TakeOver(MAKE_GENERIC_RC_DELEGATING(Allocator, Type, ##__VA_ARGS__))

#define MAKE_RC_DELEGATING(Type, ...)                                                   \
    nvrhi::MakeNewRCObj<nvrhi::DefaultMemoryAllocator>(nvrhi::GetDefaultMemAllocator()) \
        .RcNewDelegating<Type>(__VA_ARGS__)
#define MAKE_RC_DELEGATING_PTR(Type, ...) \
    nvrhi::TakeOver(MAKE_RC_DELEGATING(Type, ##__VA_ARGS__))

template <typename T>
void SafeAddRef(T* p) {
    if (p) p->AddRef();
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

}  // namespace nvrhi


#endif /* NVRHI_CORE_FOUNDATION_H */
