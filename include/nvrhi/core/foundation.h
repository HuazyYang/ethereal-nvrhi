#ifndef NVRHI_CORE_FOUNDATION_H
#define NVRHI_CORE_FOUNDATION_H
#include <nvrhi/core/threading.h>
#include <nvrhi/core/types.h>
#include <nvrhi/core/memory.h>
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

// ---- Interface tables (ADR 0007) --------------------------------------------------------------------
// Every concrete class writes its QueryInterface out as an explicit table. The base classes (ObjectImpl,
// WeakReferenceSourceImpl and the delegating ones) own the reference count but implement no QueryInterface
// (the delegating ones forward QueryInterface to their owner and leave NonDelegatingQueryInterface pure),
// so a class that neither writes a table nor inherits one stays abstract and MAKE_RC_OBJ does not compile
// for it.
//
//     NVRHI_CLASS_CLSID(Texture, "...")                    // namespace scope: declares the class, class ID
//     class Texture : public ObjectImpl<ITexture>, public TextureStateExtension
//     {
//     public:
//         NVRHI_DECLARE_UUID_TRAITS(Texture)
//         NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Texture)
//         NVRHI_IMPLEMENTS_INTERFACE(ITexture)             // first: an offset entry, the object's IObject
//         NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)           // every ancestor interface is listed too
//         NVRHI_IMPLEMENTS_CLASS(Texture)                  // the class ID
//         NVRHI_END_INTERFACE_TABLE()
//         ...
//     };
//
// Rules:
// - The table lists every interface the class implements, ancestors included (an interface answers only
//   the IIDs listed for it; nothing is derived from the inheritance graph), and the class ID if the class
//   has one. A weak-referenceable object lists IWeakReferenceSource.
// - IObject is not listed: the walker answers it with the FIRST entry, which must therefore be an offset
//   entry (NVRHI_IMPLEMENTS_INTERFACE / NVRHI_IMPLEMENTS_INTERFACE_AS) whose pointer is a valid IObject*,
//   the object's identity. A class that routes to a parent or a member still starts with an interface of
//   its own, and a derived class starts with the same interface as its parent's table, so that the
//   identity does not change between the two.
// - Entries are tried in order. NVRHI_IMPLEMENTS_ROUTE_PARENT(Base) asks Base's table (a direct call);
//   NVRHI_IMPLEMENTS_ROUTE_MEMBER(m) asks an aggregated member's non-delegating table.
// - An aggregated object (DelegatingObjectImpl / DelegatingWeakReferenceSourceImpl) writes its table with
//   NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE...; that table refuses IObject (the identity is the owner's).
// - NVRHI_END_INTERFACE_TABLE() ends both kinds. On a miss in a QueryInterface table it answers the
//   liveness probe (details::QIStrongRefProbeIID, types.h) by asking the base class that owns the reference
//   count (NvrhiQIAnswerProbe); a non-delegating table does not answer the probe (the owner's table does).
//
// The tables are constant data: every entry is an address constant (the IIDs through details::QIIIDOf,
// the finders are functions), so MSVC emits no thread-safe initialization guard for them. Keep it so:
// no ?: or constexpr pointer variables in entries, which MSVC does not always fold.
//
// Per-class check: a class inherits its base's table, so a derived class that adds an interface or a class
// ID and forgets its own table would still compile. MakeNewRCObj (every MAKE_RC_OBJ / MAKE_GENERIC_RC_OBJ /
// MAKE_RC_DELEGATING path) therefore requires each class it creates to state its table itself
// (details::QIDeclaresOwnTable): with a table macro in its body (NVRHI_BEGIN_..._INLINE, or
// NVRHI_DECLARE_..._INTERFACE_TABLE() for an out-of-line table), or, when it adds no interface and no class
// ID, with
//
//     class PerspectiveCamera : public SceneCamera
//     {
//         NVRHI_INHERIT_INTERFACE_TABLE()                  // SceneCamera's table is correct for this class
//         ...
//     };
//
// A class that writes its QueryInterface by hand (no table macro) also passes. Objects that are not
// created through MakeNewRCObj (by-value members, stack instances) are not checked.
//
// Each of these macros declares NvrhiQITableClass() (a non-delegating table: NvrhiQINonDelegatingTableClass()),
// a member function whose return type, decltype(this), names the class without spelling it (so it also works
// in class templates, and in NVRHI_DECLARE_..._INTERFACE_TABLE() / NVRHI_INHERIT_INTERFACE_TABLE(), which
// take no class name). A derived class that writes nothing finds its base's, which names the base. The macro
// also makes details::QITableAccess a friend, so the check reads the member in a private or protected
// section too; the macros do not change the access of the members that follow them.
#define NVRHI_QI_TABLE_CLASS_(Member)              \
    friend struct ::nvrhi::details::QITableAccess; \
    auto Member()->decltype(this) { return this; }

#define NVRHI_BEGIN_INTERFACE_TABLE(ClassName)                                     \
    nvrhi::FRESULT ClassName::QueryInterface(nvrhi::FREFIID riid, void** ppv) {   \
        typedef ClassName _ITCls;                                                  \
        constexpr bool _ITNonDelegating = false;                                   \
        static const nvrhi::details::INTERFACE_ENTRY inttable[] = {
#define NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ClassName)                              \
    NVRHI_QI_TABLE_CLASS_(NvrhiQITableClass)                                       \
    nvrhi::FRESULT QueryInterface(nvrhi::FREFIID riid, void** ppv) override {      \
        typedef ClassName _ITCls;                                                  \
        constexpr bool _ITNonDelegating = false;                                   \
        static const nvrhi::details::INTERFACE_ENTRY inttable[] = {
#define NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE(ClassName)                      \
    nvrhi::FRESULT ClassName::NonDelegatingQueryInterface(nvrhi::FREFIID riid,     \
                                                          void** ppv) {            \
        typedef ClassName _ITCls;                                                  \
        constexpr bool _ITNonDelegating = true;                                    \
        static const nvrhi::details::INTERFACE_ENTRY inttable[] = {
#define NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(ClassName)               \
    NVRHI_QI_TABLE_CLASS_(NvrhiQINonDelegatingTableClass)                          \
    nvrhi::FRESULT NonDelegatingQueryInterface(nvrhi::FREFIID riid, void** ppv)    \
        override {                                                                 \
        typedef ClassName _ITCls;                                                  \
        constexpr bool _ITNonDelegating = true;                                    \
        static const nvrhi::details::INTERFACE_ENTRY inttable[] = {

// Asks the table of Base, a base class that implements QueryInterface (in a non-delegating table: its
// NonDelegatingQueryInterface), with a direct, non-virtual call. Base cannot be an interface or a class of
// the ObjectImpl family, which implement no QueryInterface: list their interfaces instead. Variadic so that
// template bases with commas (Foo<A, B>) pass through the macro.
#define NVRHI_IMPLEMENTS_ROUTE_PARENT(...)                                                       \
    {nullptr, &nvrhi::details::RouteParentQueryInterface<_ITCls, _ITNonDelegating, __VA_ARGS__>, \
     NVRHI_BASE_OFFSET(_ITCls, __VA_ARGS__)},

// Asks the non-delegating table of an aggregated member (an object or a pointer to one).
#define NVRHI_IMPLEMENTS_ROUTE_MEMBER(Member)                                        \
    {nullptr,                                                                           \
     &nvrhi::details::RouteMemberQueryInterface<::std::decay<decltype(Member)>::type>, \
     uint32_t(size_t(::std::addressof(this->Member)) -                                    \
              size_t(this))}, /* Note: we can not use offsetof here */

// The IID of Itf, answered with the class's Itf base: an offset entry. Itf derives from IObject and is an
// unambiguous base of the class; otherwise use NVRHI_IMPLEMENTS_INTERFACE_AS.
#define NVRHI_IMPLEMENTS_INTERFACE(Itf) \
    {&nvrhi::details::QIIIDOf<Itf>, NVRHI_ENTRY_IS_OFFSET, NVRHI_BASE_OFFSET(_ITCls, Itf)},

// The IID of req, answered with the class's Itf base (e.g. an ancestor that is a base through several
// paths, answered through one of them).
#define NVRHI_IMPLEMENTS_INTERFACE_AS(req, Itf) \
    {&nvrhi::details::QIIIDOf<req>, NVRHI_ENTRY_IS_OFFSET, NVRHI_BASE_OFFSET(_ITCls, Itf)},

// The class ID of Class (the class itself, or an implementation class it derives from), answered with the
// Class base. The entry AddRefs through Class, so it does not depend on where the class's IObject lies in its
// layout (a helper base may come first, e.g. vulkan Texture : MemoryResource, ObjectImpl<ITexture>). Not an
// offset entry: never the first entry of a table.
#define NVRHI_IMPLEMENTS_CLASS(Class) \
    {&nvrhi::details::QIIIDOf<Class>, &nvrhi::details::QISelfEntry<Class>, NVRHI_BASE_OFFSET(_ITCls, Class)},

// Class ID of an implementation class. The class answers QueryInterface for it (NVRHI_IMPLEMENTS_CLASS in
// its table); checked_cast verifies that in Debug builds, and it replaces dynamic_cast (ADR 0006):
//
//     NVRHI_CLASS_CLSID(Texture, "...")            // namespace scope, before the class
//     class Texture : public ObjectImpl<ITexture>
//     {
//     public:
//         NVRHI_DECLARE_UUID_TRAITS(Texture)
//         NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Texture)
//         NVRHI_IMPLEMENTS_INTERFACE(ITexture)
//         NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
//         NVRHI_IMPLEMENTS_CLASS(Texture)
//         NVRHI_END_INTERFACE_TABLE()
//         ...
//     };
//
// NVRHI_CLASS_CLSID declares the class (NVRHI_CCLSID does not) and gives it the class ID. A struct uses
// NVRHI_SCLSID after its own forward declaration.
#define NVRHI_CLASS_CLSID(Class, StrCLSID) \
    class Class;                           \
    NVRHI_CCLSID(Class, StrCLSID)

#define NVRHI_END_INTERFACE_TABLE()                                                            \
    { 0, (nvrhi::details::INTERFACE_FINDER)0, 0 }                                              \
    }                                                                                             \
    ;                                                                                             \
    return nvrhi::details::QITableQueryInterface<_ITNonDelegating>(this, inttable, riid, ppv); \
    }

// In the class, for a table written out of line with NVRHI_BEGIN_(NON_DELEGATING_)INTERFACE_TABLE(Class).
#define NVRHI_DECLARE_INTERFACE_TABLE()       \
    NVRHI_QI_TABLE_CLASS_(NvrhiQITableClass) \
    nvrhi::FRESULT QueryInterface(const nvrhi::FIID& riid, void** ppv) override;
#define NVRHI_DECLARE_NON_DELEGATING_INTERFACE_TABLE()     \
    NVRHI_QI_TABLE_CLASS_(NvrhiQINonDelegatingTableClass) \
    nvrhi::FRESULT NonDelegatingQueryInterface(const nvrhi::FIID& riid, void** ppv) override;

// In a class that adds no interface and no class ID to its base: the base's table is correct for it (see
// "Per-class check" above). The static_assert rejects it in a class that has no table to inherit (checked
// where the class is defined; for a class template, where MakeNewRCObj creates it).
#define NVRHI_INHERIT_INTERFACE_TABLE()                                                                     \
    NVRHI_QI_TABLE_CLASS_(NvrhiQITableClass)                                                                \
    void NvrhiQICheckInheritedTable() {                                                                     \
        static_assert(::nvrhi::details::QIInheritsTable<::std::remove_pointer_t<decltype(this)>>,           \
                      "NVRHI_INHERIT_INTERFACE_TABLE(): no base class declares an interface table to inherit; " \
                      "write the class's own table (NVRHI_BEGIN/END_INTERFACE_TABLE)");                     \
    }

namespace nvrhi {

template <class AllocatorType = IMemoryAllocator>
class MakeNewRCObj;

template <typename... Bases>
class WeakReferenceSourceImpl;

namespace details {
typedef FRESULT (*INTERFACE_FINDER)(void* pThis, uint32_t data, FREFIID riid, void** ppv);

// One copy of each IID with vague (COMDAT) linkage and constant initialization: the same address in
// every translation unit, so interface tables in inline and template functions stay constant data.
// (IID_X from NVRHI_IID has internal linkage; MSVC guards a table that takes its address.)
template <typename Itf>
inline constexpr FIID QIIIDOf = uuid_of<Itf>();

struct INTERFACE_ENTRY {
    const FIID* pIID;
    INTERFACE_FINDER pfnFinder;
    uint32_t data;
};

// NVRHI_IMPLEMENTS_CLASS: the object, as its Cls base at pThis + offset.
template <typename Cls>
FRESULT QISelfEntry(void* pThis, uint32_t offset, FREFIID, void** ppv) {
    if (ppv) {
        Cls* p = reinterpret_cast<Cls*>(static_cast<char*>(pThis) + offset);
        *ppv = p;
        p->AddRef();
    }
    return FS_OK;
}

// The marker of an offset entry, and the IObject answer: the base at pThis + offset, an IObject at that
// address. A real function, so that every table entry is an address constant; the walker recognizes offset
// entries by this address.
inline FRESULT QIOffsetEntry(void* pThis, uint32_t offset, FREFIID, void** ppv) {
    if (ppv) {
        *ppv = static_cast<char*>(pThis) + offset;
        static_cast<IObject*>(*ppv)->AddRef();
    }
    return FS_OK;
}
#define NVRHI_ENTRY_IS_OFFSET (&nvrhi::details::QIOffsetEntry)

// The base is variadic so that template bases with commas (Foo<A, B>) pass through macros.
#define NVRHI_BASE_OFFSET(ClassName, ...)                            \
    uint32_t(reinterpret_cast<char*>(static_cast<__VA_ARGS__*>(      \
                 reinterpret_cast<ClassName*>(sizeof(ClassName)))) - \
             reinterpret_cast<char*>(sizeof(ClassName)))

// The walk over a table. IObject: the first entry, which must be an offset entry (see the rules above).
// An offset entry returns its base and adds the reference through that identity, so the base need not
// start with its IObject (an interface whose first base is not an IObject). A null ppv only asks whether
// riid is answered: no pointer, no reference.
inline FRESULT InterfaceTableQueryInterface(void* pThis, const INTERFACE_ENTRY* pTable, FREFIID riid, void** ppv) {
    NVRHI_VERIFY(pTable->pfnFinder == NVRHI_ENTRY_IS_OFFSET,
                 "The first entry of an interface table must be NVRHI_IMPLEMENTS_INTERFACE: it answers IObject");
    const uint32_t identity = pTable->data;
    if (riid == IID_IObject) return details::QIOffsetEntry(pThis, identity, riid, ppv);

    FRESULT hr = FE_NOINTERFACE;
    while (pTable->pfnFinder) {
        if (!pTable->pIID || riid == *pTable->pIID) {
            if (pTable->pfnFinder == NVRHI_ENTRY_IS_OFFSET) {
                if (ppv) {
                    *ppv = static_cast<char*>(pThis) + pTable->data;
                    reinterpret_cast<IObject*>(static_cast<char*>(pThis) + identity)->AddRef();
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

// The liveness probe for a class whose table missed it: the base class that owns the reference count
// answers through its non-virtual NvrhiQIAnswerProbe(). A class without one (a mixin with its own table,
// whose host class's table asks the owner itself) does not answer.
template <typename Cls>
auto QIAnswerProbe(Cls* self, int) -> decltype(self->NvrhiQIAnswerProbe()) {
    return self->NvrhiQIAnswerProbe();
}
template <typename Cls>
FRESULT QIAnswerProbe(Cls*, long) {
    return FE_NOINTERFACE;
}

// NVRHI_END_INTERFACE_TABLE(): the walk, then what no entry answers. A non-delegating table refuses
// IObject (the identity is the owner's) and leaves the probe to the owner's table: answering it here would
// ask the owner, whose table may route back here through NVRHI_IMPLEMENTS_ROUTE_MEMBER.
template <bool ND, typename Cls>
FRESULT QITableQueryInterface(Cls* self, const INTERFACE_ENTRY* pTable, FREFIID riid, void** ppv) {
    if constexpr (ND) {
        if (riid == IID_IObject) {
            if (ppv) *ppv = nullptr;
            return FE_NOINTERFACE;
        }
        return details::InterfaceTableQueryInterface(static_cast<void*>(self), pTable, riid, ppv);
    } else {
        const FRESULT hr = details::InterfaceTableQueryInterface(static_cast<void*>(self), pTable, riid, ppv);
        if (hr != FS_OK && riid == QIStrongRefProbeIID) return details::QIAnswerProbe(self, 0);
        return hr;
    }
}

// ---- Routing entries --------------------------------------------------------------------------------
// A qualified call Base::QueryInterface(...) does not dispatch: it binds to the declaration that name
// lookup finds from Base. &Base::QueryInterface is a pointer to member *of the declaring class*: IObject
// when Base has nothing but the root pure virtual (an interface, or a class of the ObjectImpl family), and
// then the call would not link. This relies on nvrhi's rule that interfaces do not re-declare
// QueryInterface.
template <typename C> C* QIDeclarer(FRESULT (C::*)(FREFIID, void**));

// ---- Per-class table check --------------------------------------------------------------------------
// A friend of every class whose body has a table macro (NVRHI_QI_TABLE_CLASS_), so these reach members in
// private and protected sections. Each answers void when the member is missing, ambiguous or inaccessible.
struct QITableAccess {
    // The object wrappers (weak references) call QueryInterface through here: a virtual call, which also
    // reaches a table written in a private or protected section.
    template <typename T>
    static FRESULT QueryInterface(T* p, FREFIID riid, void** ppv) {
        return p->QueryInterface(riid, ppv);
    }

    // The class named by NvrhiQITableClass() / NvrhiQINonDelegatingTableClass(): T*, or a base's.
    template <typename T>
    static auto TableClass(int) -> decltype(std::declval<T&>().NvrhiQITableClass());
    template <typename T>
    static void TableClass(long);
    template <typename T>
    static auto NonDelegatingTableClass(int) -> decltype(std::declval<T&>().NvrhiQINonDelegatingTableClass());
    template <typename T>
    static void NonDelegatingTableClass(long);

    // The class that declares T's QueryInterface / NonDelegatingQueryInterface (C* for a declaring class C).
    template <typename T>
    static auto QueryInterfaceDeclarer(int) -> decltype(details::QIDeclarer(&T::QueryInterface));
    template <typename T>
    static void QueryInterfaceDeclarer(long);
    template <typename T>
    static auto NonDelegatingDeclarer(int) -> decltype(details::QIDeclarer(&T::NonDelegatingQueryInterface));
    template <typename T>
    static void NonDelegatingDeclarer(long);
};

// True when T's own body states its interface table: a table macro, or NVRHI_INHERIT_INTERFACE_TABLE().
// False for a class that merely inherits a base's table without saying so.
template <typename T>
inline constexpr bool QIDeclaresOwnTable =
    std::is_same_v<decltype(QITableAccess::TableClass<T>(0)), T*> ||
    std::is_same_v<decltype(QITableAccess::NonDelegatingTableClass<T>(0)), T*>;

// True when T itself declares QueryInterface or NonDelegatingQueryInterface: a table (inline or out of line),
// or a hand-written implementation.
template <typename T>
inline constexpr bool QIDeclaresQueryInterface =
    std::is_same_v<decltype(QITableAccess::QueryInterfaceDeclarer<T>(0)), T*> ||
    std::is_same_v<decltype(QITableAccess::NonDelegatingDeclarer<T>(0)), T*>;

template <typename C>
inline constexpr bool QIDeclarerHasTable = std::is_class_v<C> && QIDeclaresOwnTable<C>;

// NVRHI_INHERIT_INTERFACE_TABLE(): T answers QueryInterface (or, aggregated, NonDelegatingQueryInterface)
// with a table that a base class declared.
template <typename T>
inline constexpr bool QIInheritsTable =
    (!std::is_same_v<decltype(QITableAccess::QueryInterfaceDeclarer<T>(0)), T*> &&
     QIDeclarerHasTable<std::remove_pointer_t<decltype(QITableAccess::QueryInterfaceDeclarer<T>(0))>>) ||
    (!std::is_same_v<decltype(QITableAccess::NonDelegatingDeclarer<T>(0)), T*> &&
     QIDeclarerHasTable<std::remove_pointer_t<decltype(QITableAccess::NonDelegatingDeclarer<T>(0))>>);

// NVRHI_IMPLEMENTS_ROUTE_PARENT: Base's table, with a direct (qualified, non-virtual) call.
template <typename T, bool ND, typename TBase>
FRESULT RouteParentQueryInterface(void* pThis, uint32_t offset, FREFIID riid, void** ppv) {
    static_assert(std::is_base_of_v<TBase, T>, "NVRHI_IMPLEMENTS_ROUTE_PARENT(Base): Base must be a base of the class");
    TBase* base = reinterpret_cast<TBase*>(static_cast<char*>(pThis) + offset);
    if constexpr (ND) {
        return base->TBase::NonDelegatingQueryInterface(riid, ppv);
    } else {
        static_assert(!std::is_same_v<decltype(details::QIDeclarer(&TBase::QueryInterface)), IObject*>,
                      "NVRHI_IMPLEMENTS_ROUTE_PARENT(Base): Base implements no QueryInterface (an interface or "
                      "ObjectImpl<...>); list its interfaces with NVRHI_IMPLEMENTS_INTERFACE instead");
        return base->TBase::QueryInterface(riid, ppv);
    }
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
        return QITableAccess::QueryInterface(m_pObject, iid, ppInterface);
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
        return QITableAccess::QueryInterface(m_pObject, iid, ppInterface);
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
    template <typename...>
    friend class nvrhi::WeakReferenceSourceImpl;
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

// ---- The base classes' checks -----------------------------------------------------------------------
// The common base of the four base classes below (ObjectImpl, WeakReferenceSourceImpl, DelegatingObjectImpl,
// DelegatingWeakReferenceSourceImpl). A class derived from it is an implementation class: it owns a reference
// count (or, aggregated, shares its owner's). It is empty and carries UserAllocated (the operators new and
// delete that MakeNewRCObj uses), so it adds no subobject; is_base_of finds it whatever the access, and also
// in a class that derives from it twice.
class ObjectImplTag : public UserAllocated {};

// True for an implementation class (one derived from one of the four base classes); false for interfaces and
// mixins. The base classes list interfaces and mixins only (CheckObjectImplBases).
template <typename T>
inline constexpr bool IsObjectImpl = std::is_base_of_v<ObjectImplTag, T>;

// The bases of a base class: interfaces (IObject or interfaces derived from it) and mixins (classes that
// implement QueryInterface with their own table and own no reference count). A weak object lists
// IWeakReferenceSource (or an interface derived from it).
template <bool Weak, typename... QIBases>
constexpr bool CheckObjectImplBases() {
    static_assert(sizeof...(QIBases) > 0 && (std::is_base_of_v<IObject, QIBases> && ...),
                  "ObjectImpl<Bases...>: list interfaces and classes implementing QueryInterface; "
                  "inherit helper classes directly");
    static_assert(!(IsObjectImpl<QIBases> || ...),
                  "ObjectImpl<Bases...>: a base already owns a reference count: list interfaces only; to build on "
                  "an implementation Foo, derive from Foo directly (class Bar : public Foo) and write Bar's table");
    static_assert(!Weak || (std::is_base_of_v<IWeakReferenceSource, QIBases> || ...),
                  "WeakReferenceSourceImpl<Bases...>: list IWeakReferenceSource (or an interface derived from it)");
    return true;
}

}  // namespace details

/// Base classes of reference-counted objects. Bases are the class's interfaces (IObject or interfaces
/// derived from it) and mixins (classes implementing QueryInterface with their own table). Each base class
/// derives from its Bases directly and owns the reference count:
///
///     class Foo : public ObjectImpl<IA, IB> { ... };    // owns the reference count, writes Foo's table
///     class Bar : public Foo { ... };                    // builds on Foo: writes Bar's table (or
///                                                        // NVRHI_INHERIT_INTERFACE_TABLE())
///
/// An implementation class (one derived from these) is not a valid base: ObjectImpl<Foo> does not compile.
/// None of them implements QueryInterface: every concrete class has an explicit interface table (its own or
/// an inherited one) that lists all its interfaces, see "Interface tables" at the top of this file.
/// NvrhiQIAnswerProbe() answers the liveness probe for the end of that table (QITableQueryInterface).
// In the bodies below, nvrhi::details rather than details: MSVC also searches the (user) bases for names.
template <typename... QIBases>
class ObjectImpl : public QIBases..., protected nvrhi::details::ObjectImplTag {
    static_assert(nvrhi::details::CheckObjectImplBases<false, QIBases...>());

 public:
    ObjectImpl() {}

    // The liveness probe (types.h), for the end of the class's interface table.
    FRESULT NvrhiQIAnswerProbe() const { return m_NumStrongReferences.load() > 0 ? FS_OK : FE_NOT_ALIVE_OBJECT; }

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
        auto pWrapper = reinterpret_cast<nvrhi::details::ObjectWrapperBase*>(&ObjWrapperStorageCopy);
        pWrapper->DestroyObject();
    }

 private:
    template <typename AllocatorType>
    friend class nvrhi::MakeNewRCObj;
    template <typename ObjectType, typename AllocatorType>
    friend class nvrhi::details::ObjectWrapper;

    template <typename ObjectType, typename AllocatorType>
    void Attach(ObjectType* pObject, AllocatorType* pAllocator) throw() {
        static_assert(sizeof(nvrhi::details::ObjectWrapper<ObjectType, AllocatorType>) == sizeof(m_ObjWrapperStorage),
                      "Unexpected object wrapper size");
        new (&m_ObjWrapperStorage) nvrhi::details::ObjectWrapper<ObjectType, AllocatorType>{pObject, pAllocator};
    }

    ObjectImpl(const ObjectImpl&) = delete;
    ObjectImpl(ObjectImpl&&) = delete;
    ObjectImpl& operator=(const ObjectImpl&) = delete;
    ObjectImpl& operator=(ObjectImpl&&) = delete;

    std::atomic<FLONG> m_NumStrongReferences{1};
    nvrhi::details::ObjectWrapperStorage m_ObjWrapperStorage{};
};

/// Like ObjectImpl, with weak references: one of the bases is IWeakReferenceSource (or derived from it).
template <typename... QIBases>
class WeakReferenceSourceImpl : public QIBases..., protected nvrhi::details::ObjectImplTag {
    static_assert(nvrhi::details::CheckObjectImplBases<true, QIBases...>());

 public:
#if NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
    // Constructor with weak reference syntax
    WeakReferenceSourceImpl() noexcept { ::new (&m_Storage.WeakRef) nvrhi::details::WeakReferenceImpl{}; }
#else
    WeakReferenceSourceImpl() noexcept {
        m_Storage.pWeakRef = (nvrhi::details::WeakReferenceImpl*)(*(
            uintptr_t*)((uint8_t*)this + offsetof(WeakReferenceSourceImpl, m_Storage)));
    }
#endif

    // Virtual destructor makes sure all derived classes can be destroyed
    // through the pointer to the base class
    virtual ~WeakReferenceSourceImpl() {
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

    // The liveness probe (types.h), for the end of the class's interface table. Expired while the object is
    // constructed (not attached yet) and once its strong count reached zero.
    FRESULT NvrhiQIAnswerProbe() { return GetWeakReferenceImpl()->IsExpired() ? FE_NOT_ALIVE_OBJECT : FS_OK; }

    void GetWeakReference(IWeakReference** ppv) override final {
        if (ppv) {
            auto pWeakRef = GetWeakReferenceImpl();
            pWeakRef->AddRef();
            *ppv = pWeakRef;
        }
    }

    nvrhi::details::WeakReferenceImpl* GetWeakReferenceImpl() {
#if NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT
        return &m_Storage.WeakRef;
#else
        return m_Storage.pWeakRef;
#endif
    }

 protected:
    template <typename ObjectType, typename AllocatorType>
    friend class nvrhi::details::PackedObjectWrapper;
    template <typename ObjectType, typename AllocatorType>
    friend class nvrhi::details::ObjectWrapper;
    template <typename AllocatorType>
    friend class nvrhi::MakeNewRCObj;

    friend class nvrhi::details::WeakReferenceImpl;

    template <typename ObjectType>
    friend struct nvrhi::details::WeakRefTypeTrait;  // Used for get implement object type of IWeakReference.

    using WeakRefImplType = nvrhi::details::WeakReferenceImpl;

 private:
    WeakReferenceSourceImpl(const WeakReferenceSourceImpl&) = delete;
    WeakReferenceSourceImpl(WeakReferenceSourceImpl&&) = delete;
    WeakReferenceSourceImpl& operator=(const WeakReferenceSourceImpl&) = delete;
    WeakReferenceSourceImpl& operator=(WeakReferenceSourceImpl&&) = delete;

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
        nvrhi::details::WeakReferenceImpl WeakRef;
#else
        nvrhi::details::WeakReferenceImpl* pWeakRef;
#endif
    } m_Storage;
};

/// An aggregated object: AddRef/Release/QueryInterface go to the owner; the owner reaches this object's
/// own interfaces through NonDelegatingQueryInterface (the class's non-delegating table, which it must
/// write: the base class leaves it pure).
template <typename... QIBases>
class DelegatingObjectImpl : public QIBases..., protected nvrhi::details::ObjectImplTag {
    static_assert(nvrhi::details::CheckObjectImplBases<false, QIBases...>());

 public:
    DelegatingObjectImpl(IObject* pOwner) : m_pOwner(pOwner) {}

    FLONG AddRef() override final { return m_pOwner->AddRef(); }

    FLONG Release() override final { return m_pOwner->Release(); }

    FRESULT QueryInterface(FREFIID riid, void** ppv) override { return m_pOwner->QueryInterface(riid, ppv); }

    // The aggregated object's own interfaces: its NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE... table.
    virtual FRESULT NonDelegatingQueryInterface(FREFIID riid, void** ppv) = 0;

    // The liveness probe (types.h) belongs to the owner. Used by a class that overrides QueryInterface with
    // a table of its own (the owner shares only its reference count).
    FRESULT NvrhiQIAnswerProbe() { return m_pOwner->QueryInterface(nvrhi::details::QIStrongRefProbeIID, nullptr); }

    void DestroyObject() {
        auto ObjWrapperStorageCopy = m_ObjWrapperStorage;
        auto pWrapper = reinterpret_cast<nvrhi::details::ObjectWrapperBase*>(&ObjWrapperStorageCopy);
        pWrapper->DestroyObject();
    }

 protected:
    template <typename ObjectType, typename AllocatorType>
    friend class nvrhi::details::ObjectWrapper;

    IObject* m_pOwner;

 private:
    template <typename AllocatorType>
    friend class nvrhi::MakeNewRCObj;

    template <typename ObjectType, typename AllocatorType>
    void Attach(ObjectType* pObject, AllocatorType* pAllocator) throw() {
        static_assert(sizeof(nvrhi::details::ObjectWrapper<ObjectType, AllocatorType>) == sizeof(m_ObjWrapperStorage),
                      "Unexpected object wrapper size");
        new (&m_ObjWrapperStorage) nvrhi::details::ObjectWrapper<ObjectType, AllocatorType>{pObject, pAllocator};
    }

    DelegatingObjectImpl(const DelegatingObjectImpl&) = delete;
    DelegatingObjectImpl(DelegatingObjectImpl&&) = delete;
    DelegatingObjectImpl& operator=(const DelegatingObjectImpl&) = delete;
    DelegatingObjectImpl& operator=(DelegatingObjectImpl&&) = delete;

    nvrhi::details::ObjectWrapperStorage m_ObjWrapperStorage{};
};

/// An aggregated object with weak references: GetWeakReference goes to the owner as well.
template <typename... QIBases>
class DelegatingWeakReferenceSourceImpl : public QIBases..., protected nvrhi::details::ObjectImplTag {
    static_assert(nvrhi::details::CheckObjectImplBases<true, QIBases...>());

 public:
    DelegatingWeakReferenceSourceImpl(IWeakReferenceSource* pOwner) : m_pOwner(pOwner) {}

    FLONG AddRef() override final { return m_pOwner->AddRef(); }

    FLONG Release() override final { return m_pOwner->Release(); }

    FRESULT QueryInterface(FREFIID riid, void** ppv) override { return m_pOwner->QueryInterface(riid, ppv); }

    void GetWeakReference(IWeakReference** ppv) override { return m_pOwner->GetWeakReference(ppv); }

    // The aggregated object's own interfaces: its NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE... table.
    virtual FRESULT NonDelegatingQueryInterface(FREFIID riid, void** ppv) = 0;

    // The liveness probe (types.h) belongs to the owner. Used by a class that overrides QueryInterface with
    // a table of its own (the owner shares only its reference count).
    FRESULT NvrhiQIAnswerProbe() { return m_pOwner->QueryInterface(nvrhi::details::QIStrongRefProbeIID, nullptr); }

    void DestroyObject() {
        auto ObjWrapperStorageCopy = m_ObjWrapperStorage;
        auto pWrapper = reinterpret_cast<nvrhi::details::ObjectWrapperBase*>(&ObjWrapperStorageCopy);
        pWrapper->DestroyObject();
    }

 protected:
    template <typename ObjectType, typename AllocatorType>
    friend class nvrhi::details::ObjectWrapper;

    IWeakReferenceSource* m_pOwner;

 private:
    template <typename AllocatorType>
    friend class nvrhi::MakeNewRCObj;

    template <typename ObjectType, typename AllocatorType>
    void Attach(ObjectType* pObject, AllocatorType* pAllocator) throw() {
        static_assert(sizeof(nvrhi::details::ObjectWrapper<ObjectType, AllocatorType>) == sizeof(m_ObjWrapperStorage),
                      "Unexpected object wrapper size");
        new (&m_ObjWrapperStorage) nvrhi::details::ObjectWrapper<ObjectType, AllocatorType>{pObject, pAllocator};
    }

    DelegatingWeakReferenceSourceImpl(const DelegatingWeakReferenceSourceImpl&) = delete;
    DelegatingWeakReferenceSourceImpl(DelegatingWeakReferenceSourceImpl&&) = delete;
    DelegatingWeakReferenceSourceImpl& operator=(const DelegatingWeakReferenceSourceImpl&) = delete;
    DelegatingWeakReferenceSourceImpl& operator=(DelegatingWeakReferenceSourceImpl&&) = delete;

    nvrhi::details::ObjectWrapperStorage m_ObjWrapperStorage{};
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
        static_assert(CheckInterfaceTable<ObjectType>());
        return RcNewImpl<ObjectType>(0, std::forward<CtorArgTypes>(CtorArgs)...);
    }

    template <typename ObjectType, typename OwnerType, typename... CtorArgTypes>
    ObjectType* RcNewDelegating(OwnerType* pOwner, CtorArgTypes&&... CtorArgs) const {
        static_assert(CheckInterfaceTable<ObjectType>());
        return RcNewDelegatingImpl<ObjectType>(pOwner,
                                               std::forward<CtorArgTypes>(CtorArgs)...);
    }

 private:
    // The per-class table check ("Interface tables" at the top of this file).
    template <typename T>
    static constexpr bool CheckInterfaceTable() {
        static_assert(details::QIDeclaresOwnTable<T> || details::QIDeclaresQueryInterface<T>,
                      "T declares no interface table: add NVRHI_BEGIN/END_INTERFACE_TABLE (list its interfaces "
                      "and class ID) or NVRHI_INHERIT_INTERFACE_TABLE()");
        static_assert(!details::QIDeclaresOwnTable<T> || details::QIDeclaresQueryInterface<T> ||
                          details::QIInheritsTable<T>,
                      "NVRHI_INHERIT_INTERFACE_TABLE(): no base class of T declares an interface table to "
                      "inherit; write T's own table (NVRHI_BEGIN/END_INTERFACE_TABLE)");
        return true;
    }

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
