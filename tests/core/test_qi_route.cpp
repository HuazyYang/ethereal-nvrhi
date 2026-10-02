// Explicit QueryInterface tables (ADR 0007): every concrete class lists the interfaces it answers, ancestors
// included; routing to a parent class or to an aggregated member is an explicit entry; the base classes own
// the reference count but implement no QueryInterface.
#include <nvrhi/core/Foundation.h>
#include <nvrhi/core/AutoPtr.h>

#include <type_traits>

#include "gtest/gtest.h"

using namespace nvrhi;

namespace QIRouteTest {
NVRHI_IID(IA, "a0000000-0000-0000-0000-000000000001")
struct IA : IObject {
    NVRHI_DECLARE_UUID_TRAITS(IA)
    virtual int A() = 0;
};
NVRHI_IID(IB, "b0000000-0000-0000-0000-000000000002")
struct IB : IObject {
    NVRHI_DECLARE_UUID_TRAITS(IB)
    virtual int B() = 0;
};
NVRHI_IID(IE, "e0000000-0000-0000-0000-000000000006")
struct IE : IObject {
    NVRHI_DECLARE_UUID_TRAITS(IE)
    virtual int E() = 0;
};

static int g_Destroyed = 0;

// A helper class: inherited directly, not listed in ObjectImpl<...>.
struct Counter {
    int m_Count = 0;
};

// Root with two interfaces: answers both; identity is the first entry.
struct Two : ObjectImpl<IA, IB> {
    ~Two() { ++g_Destroyed; }
    int A() override { return 1; }
    int B() override { return 2; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Two)
    NVRHI_IMPLEMENTS_INTERFACE(IA)
    NVRHI_IMPLEMENTS_INTERFACE(IB)
    NVRHI_END_INTERFACE_TABLE()
};

// Root with its own class ID, a helper base first.
NVRHI_SCLSID(Foo, "f0000000-0000-0000-0000-00000000000f")
struct Foo : Counter, ObjectImpl<IA, IB> {
    NVRHI_DECLARE_UUID_TRAITS(Foo)
    ~Foo() { ++g_Destroyed; }
    int A() override { return 11; }
    int B() override { return 12; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Foo)
    NVRHI_IMPLEMENTS_INTERFACE(IA)
    NVRHI_IMPLEMENTS_INTERFACE(IB)
    NVRHI_IMPLEMENTS_CLASS(Foo)
    NVRHI_END_INTERFACE_TABLE()
};

// Pass-through: Foo already owns the reference count. Bar's table adds its class ID and asks Foo's table
// (an explicit route-parent entry).
NVRHI_SCLSID(Bar, "ba000000-0000-0000-0000-0000000000ba")
struct Bar : ObjectImpl<Foo> {
    NVRHI_DECLARE_UUID_TRAITS(Bar)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Bar)
    NVRHI_IMPLEMENTS_INTERFACE(IA)
    NVRHI_IMPLEMENTS_CLASS(Bar)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(Foo)
    NVRHI_END_INTERFACE_TABLE()
};

// The same without a route: the derived class re-lists everything, including Foo's class ID.
NVRHI_SCLSID(BarListed, "ba000000-0000-0000-0000-0000000000bb")
struct BarListed : ObjectImpl<Foo> {
    NVRHI_DECLARE_UUID_TRAITS(BarListed)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(BarListed)
    NVRHI_IMPLEMENTS_INTERFACE(IA)
    NVRHI_IMPLEMENTS_INTERFACE(IB)
    NVRHI_IMPLEMENTS_CLASS(BarListed)
    NVRHI_IMPLEMENTS_CLASS(Foo)
    NVRHI_END_INTERFACE_TABLE()
};

// A class without a table is abstract. A class derived from it (pass-through) writes the table.
struct Mid : ObjectImpl<IA> {
    int A() override { return 21; }
};
NVRHI_SCLSID(Leaf, "1eaf0000-0000-0000-0000-00000000001f")
struct Leaf : ObjectImpl<Mid> {
    NVRHI_DECLARE_UUID_TRAITS(Leaf)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Leaf)
    NVRHI_IMPLEMENTS_INTERFACE(IA)
    NVRHI_IMPLEMENTS_CLASS(Leaf)
    NVRHI_END_INTERFACE_TABLE()
};

// A class derived directly (not through ObjectImpl<...>) from a class with a table inherits the table; it
// says so with NVRHI_INHERIT_INTERFACE_TABLE() (it adds no interface and no class ID).
struct InheritsTable : Foo {
    NVRHI_INHERIT_INTERFACE_TABLE()
    int A() override { return 13; }
};
// The same without the opt-out: it still answers through Foo's table, but MakeNewRCObj rejects it (the
// per-class check), so it is only checked with static_assert below.
struct ForgotTable : Foo {
    int A() override { return 14; }
};

// Mixins: bases that implement QueryInterface with their own table but own no reference count.
NVRHI_SCLSID(P1, "c1000000-0000-0000-0000-0000000000c1")
struct P1 : IA {
    NVRHI_DECLARE_UUID_TRAITS(P1)
    int A() override { return 31; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(P1)
    NVRHI_IMPLEMENTS_INTERFACE(IA)
    NVRHI_IMPLEMENTS_CLASS(P1)
    NVRHI_END_INTERFACE_TABLE()
};
NVRHI_SCLSID(P2, "c2000000-0000-0000-0000-0000000000c2")
struct P2 : IB {
    NVRHI_DECLARE_UUID_TRAITS(P2)
    int B() override { return 32; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(P2)
    NVRHI_IMPLEMENTS_INTERFACE(IB)
    NVRHI_IMPLEMENTS_CLASS(P2)
    NVRHI_END_INTERFACE_TABLE()
};
NVRHI_SCLSID(Multi, "c3000000-0000-0000-0000-0000000000c3")
struct Multi : ObjectImpl<P1, P2>, Counter {
    NVRHI_DECLARE_UUID_TRAITS(Multi)
    ~Multi() { ++g_Destroyed; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Multi)
    NVRHI_IMPLEMENTS_INTERFACE_AS(IA, P1)
    NVRHI_IMPLEMENTS_CLASS(Multi)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(P1)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(P2)
    NVRHI_END_INTERFACE_TABLE()
};

// Weak root and a weak pass-through. The user-provided constructors matter in non-packed mode
// (NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT=0): MakeNewRCObj stores the control-block pointer in the object's
// memory before constructing it, and MAKE_RC_OBJ(T) value-initializes T, which zeroes that memory first
// unless T has a user-provided default constructor.
struct WeakFoo : WeakReferenceSourceImpl<IWeakReferenceSource> {
    WeakFoo() {}
    ~WeakFoo() { ++g_Destroyed; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(WeakFoo)
    NVRHI_IMPLEMENTS_INTERFACE(IWeakReferenceSource)
    NVRHI_END_INTERFACE_TABLE()
};
NVRHI_SCLSID(WeakBar, "d0000000-0000-0000-0000-0000000000d0")
struct WeakBar : WeakReferenceSourceImpl<WeakFoo> {
    NVRHI_DECLARE_UUID_TRAITS(WeakBar)
    WeakBar() {}
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(WeakBar)
    NVRHI_IMPLEMENTS_INTERFACE(IWeakReferenceSource)
    NVRHI_IMPLEMENTS_CLASS(WeakBar)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(WeakFoo)
    NVRHI_END_INTERFACE_TABLE()
};

// Aggregation: the inner objects answer through their non-delegating tables; identity is the owner's.
NVRHI_SCLSID(Inner, "1a000000-0000-0000-0000-0000000000a1")
struct Inner : DelegatingObjectImpl<IE> {
    NVRHI_DECLARE_UUID_TRAITS(Inner)
    Inner(IObject* pOwner) : DelegatingObjectImpl<IE>(pOwner) {}
    int E() override { return 51; }
    NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(Inner)
    NVRHI_IMPLEMENTS_INTERFACE(IE)
    NVRHI_IMPLEMENTS_CLASS(Inner)
    NVRHI_END_INTERFACE_TABLE()
};
// Delegating pass-through: InnerEx's table routes to Inner's non-delegating table.
NVRHI_SCLSID(InnerEx, "1b000000-0000-0000-0000-0000000000b1")
struct InnerEx : DelegatingObjectImpl<Inner> {
    NVRHI_DECLARE_UUID_TRAITS(InnerEx)
    InnerEx(IObject* pOwner) : DelegatingObjectImpl<Inner>(pOwner) {}
    NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(InnerEx)
    NVRHI_IMPLEMENTS_INTERFACE(IE)
    NVRHI_IMPLEMENTS_CLASS(InnerEx)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(Inner)
    NVRHI_END_INTERFACE_TABLE()
};
struct Owner : ObjectImpl<IA> {
    Owner() { m_pInner = MAKE_RC_DELEGATING(InnerEx, this); }
    ~Owner() {
        m_pInner->DestroyObject();
        ++g_Destroyed;
    }
    int A() override { return 52; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Owner)
    NVRHI_IMPLEMENTS_INTERFACE(IA)
    NVRHI_IMPLEMENTS_ROUTE_MEMBER(m_pInner)
    NVRHI_END_INTERFACE_TABLE()
    InnerEx* m_pInner;
};

// A delegating object that shares only its owner's reference count: it overrides QueryInterface with a
// table of its own (identity its own), and still answers its non-delegating table.
struct Shared : DelegatingObjectImpl<IE> {
    Shared(IObject* pOwner) : DelegatingObjectImpl<IE>(pOwner) {}
    int E() override { return 53; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Shared)
    NVRHI_IMPLEMENTS_INTERFACE(IE)
    NVRHI_END_INTERFACE_TABLE()
    NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(Shared)
    NVRHI_IMPLEMENTS_INTERFACE(IE)
    NVRHI_END_INTERFACE_TABLE()
};
struct SharedOwner : ObjectImpl<IA> {
    SharedOwner() : m_Shared(this) {}
    int A() override { return 54; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SharedOwner)
    NVRHI_IMPLEMENTS_INTERFACE(IA)
    NVRHI_END_INTERFACE_TABLE()
    Shared m_Shared;
};

// Weak root with an extra interface: two vptrs precede the control-block storage. User-provided
// constructor for the same reason as WeakFoo.
struct Weak : WeakReferenceSourceImpl<IWeakReferenceSource, IA> {
    Weak() {}
    ~Weak() { ++g_Destroyed; }
    int A() override { return 41; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Weak)
    NVRHI_IMPLEMENTS_INTERFACE(IWeakReferenceSource)
    NVRHI_IMPLEMENTS_INTERFACE(IA)
    NVRHI_END_INTERFACE_TABLE()
};

// An out-of-line table: NVRHI_DECLARE_INTERFACE_TABLE in the class, the table in a source file.
struct OutOfLine : ObjectImpl<IA> {
    int A() override { return 61; }
    NVRHI_DECLARE_INTERFACE_TABLE()
};
NVRHI_BEGIN_INTERFACE_TABLE(OutOfLine)
NVRHI_IMPLEMENTS_INTERFACE(IA)
NVRHI_END_INTERFACE_TABLE()

// A delegating class without a non-delegating table and a weak class without a table stay abstract.
struct InnerNoTable : DelegatingObjectImpl<IE> {
    InnerNoTable(IObject* pOwner) : DelegatingObjectImpl<IE>(pOwner) {}
    int E() override { return 0; }
};
struct WeakNoTable : WeakReferenceSourceImpl<IWeakReferenceSource> {};

// ---- Per-class table check fixtures ----------------------------------------------------------------------
// A table in a private section (the class's default access), with a helper base first: the check and the
// object wrappers reach it through the friend the macro declares, and the member after it stays private.
NVRHI_CLASS_CLSID(PrivateTable, "9a000000-0000-0000-0000-00000000009a")
class PrivateTable : public Counter, public ObjectImpl<IA> {
    NVRHI_DECLARE_UUID_TRAITS(PrivateTable)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(PrivateTable)
    NVRHI_IMPLEMENTS_INTERFACE(IA)
    NVRHI_IMPLEMENTS_CLASS(PrivateTable)
    NVRHI_END_INTERFACE_TABLE()
    int m_AfterTable = 0;

 public:
    int A() override { return 81; }
};
// The opt-out in a private section, over a private table.
class PrivateInherits : public PrivateTable {
    NVRHI_INHERIT_INTERFACE_TABLE()

 public:
    int A() override { return 82; }
};
// Nothing: the trait is false and reading the base's private member is not a hard error.
class PrivateForgot : public PrivateTable {
 public:
    int A() override { return 83; }
};
// An out-of-line table declared in a protected section.
class ProtectedOutOfLine : public ObjectImpl<IA> {
 protected:
    NVRHI_DECLARE_INTERFACE_TABLE()
    int m_AfterDeclare = 0;

 public:
    int A() override { return 84; }
};
NVRHI_BEGIN_INTERFACE_TABLE(ProtectedOutOfLine)
NVRHI_IMPLEMENTS_INTERFACE(IA)
NVRHI_END_INTERFACE_TABLE()

// Class templates: the table names the injected-class-name, the opt-out names nothing.
template <typename Itf>
struct TemplateTable : ObjectImpl<Itf> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(TemplateTable)
    NVRHI_IMPLEMENTS_INTERFACE(Itf)
    NVRHI_END_INTERFACE_TABLE()
    int A() override { return 85; }
};
template <typename Tag>
struct TemplateInherits : Foo {
    NVRHI_INHERIT_INTERFACE_TABLE()
    int A() override { return 86; }
};
template <typename Tag>
struct TemplateForgot : Foo {};

// The opt-out over a non-delegating table (aggregated objects).
struct InnerInherits : InnerEx {
    NVRHI_INHERIT_INTERFACE_TABLE()
    using InnerEx::InnerEx;
};

// A hand-written QueryInterface (no table macro): the check accepts a class that declares it itself.
struct HandWritten : ObjectImpl<IA> {
    FRESULT QueryInterface(FREFIID riid, void** ppv) override {
        if (riid != IID_IObject && riid != nvrhi::uuid_of<IA>()) {
            if (ppv) *ppv = nullptr;
            return FE_NOINTERFACE;
        }
        if (ppv) {
            *ppv = static_cast<IA*>(this);
            AddRef();
        }
        return FS_OK;
    }
    int A() override { return 87; }
};
struct HandWrittenForgot : HandWritten {};

// Whether T's member m_AfterTable / m_AfterDeclare is accessible here (the macros keep the access).
template <typename T>
auto HasAccessibleAfterTable(int) -> decltype(std::declval<T&>().m_AfterTable, char());
template <typename T>
long HasAccessibleAfterTable(long);
template <typename T>
auto HasAccessibleAfterDeclare(int) -> decltype(std::declval<T&>().m_AfterDeclare, char());
template <typename T>
long HasAccessibleAfterDeclare(long);

template <typename I, typename T>
I* QI(T* p) {
    void* pv = nullptr;
    return p->QueryInterface(nvrhi::uuid_of<I>(), &pv) == FS_OK ? static_cast<I*>(pv) : nullptr;
}
template <typename T>
IObject* Identity(T* p) {
    void* pv = nullptr;
    p->QueryInterface(IID_IObject, &pv);
    return static_cast<IObject*>(pv);
}
template <typename T>
FRESULT Probe(T* p) {
    void* pv = reinterpret_cast<void*>(1);
    const FRESULT hr = p->QueryInterface(details::QIStrongRefProbeIID, &pv);
    EXPECT_EQ(pv, nullptr);  // the probe never returns a pointer
    return hr;
}
static FLONG RefCount(IObject* p) {
    p->AddRef();
    return p->Release();
}
}  // namespace QIRouteTest

// Name isolation: user code full of the names the QI machinery uses internally must still compile and
// route. Deleted catch-all functions catch any unqualified call that argument-dependent lookup could
// send here; the nested `std` namespace catches macros that do not spell ::std::.
namespace QIUser {
NVRHI_IID(IX, "0a000000-0000-0000-0000-0000000000a1")
struct IX : nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IX)
    using Self = IX;
    using Bases = int;
    using Base = int;
    using TBase = int;
    using Core = int;
    using Layer = int;
    using Lifetime = int;
    using Traits = int;
    using QITraits = int;
    using K = int;
    using B = int;
    using T = int;
    struct details {};
    virtual int X() = 0;
};
NVRHI_IID(IY, "0b000000-0000-0000-0000-0000000000b1")
struct IY : nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IY)
    virtual int Y() = 0;
};

namespace App {
namespace std {}
template <class... A> void InterfaceTableQueryInterface(A&&...) = delete;
template <class... A> void QITableQueryInterface(A&&...) = delete;
template <class... A> void RouteMemberQueryInterface(A&&...) = delete;
template <class... A> void RouteParentQueryInterface(A&&...) = delete;
template <class... A> void QIDeclarer(A&&...) = delete;
template <class... A> void QIAnswerProbe(A&&...) = delete;
template <class... A> void QIOffsetEntry(A&&...) = delete;
template <class... A> void QISelfEntry(A&&...) = delete;

// Mixin with its own table; its nested TBase would hijack a `->TBase::QueryInterface` call.
struct XMix : IY {
    using TBase = int;
    int Y() override { return 72; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(XMix)
    NVRHI_IMPLEMENTS_INTERFACE(IY)
    NVRHI_END_INTERFACE_TABLE()
};
struct XRoot : nvrhi::ObjectImpl<IX, XMix> {
    using T = int;
    int X() override { return 71; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(XRoot)
    NVRHI_IMPLEMENTS_INTERFACE(IX)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(XMix)
    NVRHI_END_INTERFACE_TABLE()
};
struct XLeaf : nvrhi::ObjectImpl<XRoot> {
    using Core = int;
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(XLeaf)
    NVRHI_IMPLEMENTS_INTERFACE(IX)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(XRoot)
    NVRHI_END_INTERFACE_TABLE()
};
struct XInner : nvrhi::DelegatingObjectImpl<IY> {
    using T = int;
    XInner(nvrhi::IObject* pOwner) : nvrhi::DelegatingObjectImpl<IY>(pOwner) {}
    int Y() override { return 73; }
    NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(XInner)
    NVRHI_IMPLEMENTS_INTERFACE(IY)
    NVRHI_END_INTERFACE_TABLE()
};
struct XOwner : nvrhi::ObjectImpl<IX> {
    ~XOwner() {
        if (m_pInner) m_pInner->DestroyObject();
    }
    int X() override { return 74; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(XOwner)
    NVRHI_IMPLEMENTS_INTERFACE(IX)
    NVRHI_IMPLEMENTS_ROUTE_MEMBER(m_pInner)
    NVRHI_END_INTERFACE_TABLE()
    XInner* m_pInner = nullptr;
};
}  // namespace App
}  // namespace QIUser

using namespace QIRouteTest;

// A class without an explicit table (its own or an inherited one) is abstract: MAKE_RC_OBJ does not compile.
static_assert(std::is_abstract_v<ObjectImpl<IA>>, "ObjectImpl implements no QueryInterface");
static_assert(std::is_abstract_v<WeakReferenceSourceImpl<IWeakReferenceSource>>, "");
static_assert(std::is_abstract_v<Mid>, "no table: abstract");
static_assert(std::is_abstract_v<WeakNoTable>, "no table: abstract");
static_assert(std::is_abstract_v<InnerNoTable>, "no non-delegating table: abstract");
static_assert(!std::is_abstract_v<Leaf> && !std::is_abstract_v<Foo> && !std::is_abstract_v<InheritsTable>, "");
static_assert(!std::is_abstract_v<Inner> && !std::is_abstract_v<InnerEx> && !std::is_abstract_v<OutOfLine>, "");
static_assert(!details::QIOwnsRefCount<IA> && !details::QIOwnsRefCount<P1> && details::QIOwnsRefCount<Foo>, "");
static_assert(sizeof(Bar) == sizeof(Foo), "pass-through adds no reference count or vptr");

// The per-class check (details::QIDeclaresOwnTable, asserted by MakeNewRCObj).
static_assert(details::QIDeclaresOwnTable<Two> && details::QIDeclaresOwnTable<Foo> &&
                  details::QIDeclaresOwnTable<Bar> && details::QIDeclaresOwnTable<Multi>,
              "own inline table");
static_assert(details::QIDeclaresOwnTable<OutOfLine>, "out-of-line table with NVRHI_DECLARE_INTERFACE_TABLE()");
static_assert(details::QIDeclaresOwnTable<Inner> && details::QIDeclaresOwnTable<InnerEx>, "non-delegating table");
static_assert(details::QIDeclaresOwnTable<Shared>, "QueryInterface and non-delegating tables in one class");
static_assert(details::QIDeclaresOwnTable<InheritsTable> && details::QIInheritsTable<InheritsTable>,
              "NVRHI_INHERIT_INTERFACE_TABLE()");
static_assert(details::QIDeclaresOwnTable<InnerInherits> && details::QIInheritsTable<InnerInherits>,
              "NVRHI_INHERIT_INTERFACE_TABLE() over a non-delegating table");
static_assert(!details::QIDeclaresOwnTable<ForgotTable> && !details::QIDeclaresQueryInterface<ForgotTable>,
              "a derived class without a table or the opt-out");
static_assert(details::QIInheritsTable<ForgotTable>, "(it does answer through Foo's table)");
static_assert(details::QIDeclaresOwnTable<TemplateTable<IA>> && details::QIDeclaresOwnTable<TemplateInherits<int>> &&
                  details::QIInheritsTable<TemplateInherits<int>> && !details::QIDeclaresOwnTable<TemplateForgot<int>>,
              "class templates");
static_assert(details::QIDeclaresOwnTable<PrivateTable> && details::QIDeclaresOwnTable<PrivateInherits> &&
                  details::QIInheritsTable<PrivateInherits> && !details::QIDeclaresOwnTable<PrivateForgot>,
              "a table and the opt-out in a private section");
static_assert(details::QIDeclaresOwnTable<ProtectedOutOfLine>, "an out-of-line table declared in a protected section");
static_assert(std::is_same_v<decltype(HasAccessibleAfterTable<PrivateTable>(0)), long> &&
                  std::is_same_v<decltype(HasAccessibleAfterDeclare<ProtectedOutOfLine>(0)), long>,
              "the macros do not change the access of the members after them");
static_assert(!details::QIDeclaresOwnTable<HandWritten> && details::QIDeclaresQueryInterface<HandWritten> &&
                  !details::QIDeclaresQueryInterface<HandWrittenForgot>,
              "a hand-written QueryInterface is accepted for its own class only");
static_assert(!details::QIDeclaresOwnTable<Mid> && !details::QIInheritsTable<Mid> &&
                  !details::QIInheritsTable<WeakNoTable> && !details::QIInheritsTable<InnerNoTable>,
              "no table anywhere: nothing to inherit");

TEST(QueryInterfaceTable, RootWithTwoInterfaces) {
    g_Destroyed = 0;
    Two* p = MAKE_RC_OBJ(Two);
    IA* a = QI<IA>(p);
    IB* b = QI<IB>(p);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->A(), 1);
    EXPECT_EQ(b->B(), 2);
    EXPECT_EQ(QI<IE>(p), nullptr);
    IObject* id1 = Identity(a);
    IObject* id2 = Identity(b);
    EXPECT_EQ(id1, id2);
    EXPECT_EQ(id1, static_cast<IObject*>(static_cast<IA*>(p)));
    EXPECT_EQ(p->QueryInterface(nvrhi::uuid_of<IB>(), nullptr), FS_OK);  // null ppv: no AddRef
    EXPECT_EQ(Probe(p), FS_OK);
    FLONG n = 0;
    for (IObject* x : {static_cast<IObject*>(a), static_cast<IObject*>(b), id1, id2}) n = x->Release();
    EXPECT_EQ(n, 1);
    EXPECT_EQ(static_cast<IA*>(p)->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceTable, ClassIdWithHelperBaseFirst) {
    g_Destroyed = 0;
    Foo* p = MAKE_RC_OBJ(Foo);
    IB* b = static_cast<IB*>(p);
    EXPECT_EQ(QI<Foo>(b), p);
    EXPECT_EQ(QI<IA>(b), static_cast<IA*>(p));
    EXPECT_EQ(Identity(b), static_cast<IObject*>(static_cast<IA*>(p)));
    EXPECT_EQ(RefCount(b), 4);
    for (int i = 0; i < 3; ++i) b->Release();
    EXPECT_EQ(b->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceTable, PassThroughRouteParent) {
    g_Destroyed = 0;
    Bar* p = MAKE_RC_OBJ(Bar);
    EXPECT_EQ(QI<Bar>(p), p);                    // Bar's table
    EXPECT_EQ(QI<Foo>(static_cast<IB*>(p)), p);  // Foo's table, through the route
    IA* a = QI<IA>(p);
    IB* b = QI<IB>(p);  // only Foo's table lists IB
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->A(), 11);
    EXPECT_EQ(b->B(), 12);
    IObject* id = Identity(b);
    EXPECT_EQ(id, static_cast<IObject*>(static_cast<IA*>(p)));
    EXPECT_EQ(QI<IE>(p), nullptr);
    EXPECT_EQ(Probe(p), FS_OK);  // Foo's table misses it, then Bar's end asks the owner of the count
    EXPECT_EQ(RefCount(a), 6);
    for (int i = 0; i < 5; ++i) a->Release();  // QI<Bar>, QI<Foo>, a, b, id
    EXPECT_EQ(a->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceTable, PassThroughRelisted) {
    g_Destroyed = 0;
    BarListed* p = MAKE_RC_OBJ(BarListed);
    EXPECT_EQ(QI<BarListed>(p), p);
    EXPECT_EQ(QI<Foo>(p), static_cast<Foo*>(p));
    EXPECT_EQ(QI<Bar>(p), nullptr);  // not listed
    IB* b = QI<IB>(p);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->B(), 12);
    EXPECT_EQ(Identity(b), static_cast<IObject*>(static_cast<IA*>(p)));
    for (int i = 0; i < 4; ++i) b->Release();
    EXPECT_EQ(b->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceTable, PassThroughOverClassWithoutTable) {
    Leaf* p = MAKE_RC_OBJ(Leaf);
    EXPECT_EQ(QI<Leaf>(p), p);
    IA* a = QI<IA>(p);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->A(), 21);
    EXPECT_EQ(Identity(p), static_cast<IObject*>(p));
    EXPECT_EQ(QI<IB>(p), nullptr);
    EXPECT_EQ(Probe(p), FS_OK);
    for (int i = 0; i < 3; ++i) p->Release();
    EXPECT_EQ(p->Release(), 0);
}

TEST(QueryInterfaceTable, InheritedTable) {
    g_Destroyed = 0;
    InheritsTable* p = MAKE_RC_OBJ(InheritsTable);
    IA* a = QI<IA>(static_cast<IB*>(p));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->A(), 13);
    EXPECT_EQ(QI<Foo>(a), static_cast<Foo*>(p));  // the inherited table answers Foo's class ID
    a->Release();
    a->Release();
    EXPECT_EQ(a->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceTable, MixinsInOrder) {
    g_Destroyed = 0;
    Multi* m = MAKE_RC_OBJ(Multi);
    IA* a = QI<IA>(static_cast<P1*>(m));
    IB* b = QI<IB>(static_cast<P1*>(m));  // answered by the second mixin's table
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->A(), 31);
    EXPECT_EQ(b->B(), 32);
    EXPECT_EQ(QI<Multi>(b), m);
    EXPECT_EQ(QI<P2>(a), static_cast<P2*>(m));
    EXPECT_EQ(QI<P1>(b), static_cast<P1*>(m));
    EXPECT_EQ(Identity(b), static_cast<IObject*>(static_cast<IA*>(m)));  // first entry
    EXPECT_EQ(QI<IE>(a), nullptr);
    EXPECT_EQ(Probe(b), FS_OK);  // the mixins' tables do not answer it, Multi's end does
    for (int i = 0; i < 6; ++i) a->Release();  // a, b, QI<Multi>, QI<P2>, QI<P1>, identity
    EXPECT_EQ(a->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceTable, WeakPassThrough) {
    g_Destroyed = 0;
    WeakBar* p = MAKE_RC_OBJ(WeakBar);
    EXPECT_EQ(QI<WeakBar>(p), p);
    IWeakReferenceSource* ws = QI<IWeakReferenceSource>(p);
    ASSERT_NE(ws, nullptr);
    EXPECT_EQ(Probe(p), FS_OK);
    IWeakReference* wr = nullptr;
    ws->GetWeakReference(&wr);
    ws->Release();
    p->Release();  // QI<WeakBar>
    WeakBar* r = nullptr;
    EXPECT_EQ(wr->Resolve(nvrhi::uuid_of<WeakBar>(), reinterpret_cast<void**>(&r)), FS_OK);
    EXPECT_EQ(r, p);
    r->Release();
    p->Release();
    EXPECT_EQ(g_Destroyed, 1);
    r = nullptr;
    EXPECT_NE(wr->Resolve(nvrhi::uuid_of<WeakBar>(), reinterpret_cast<void**>(&r)), FS_OK);
    EXPECT_EQ(r, nullptr);
    wr->Release();
}

TEST(QueryInterfaceTable, AggregationWithDelegatingPassThrough) {
    g_Destroyed = 0;
    Owner* p = MAKE_RC_OBJ(Owner);
    IObject* id = Identity(p);
    EXPECT_EQ(id, static_cast<IObject*>(static_cast<IA*>(p)));  // not the member field
    IE* e = QI<IE>(p);                                           // owner -> member -> InnerEx's table
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->E(), 51);
    EXPECT_EQ(Identity(e), id);  // identity stays the owner's
    EXPECT_EQ(QI<InnerEx>(p), p->m_pInner);
    EXPECT_EQ(QI<Inner>(p), static_cast<Inner*>(p->m_pInner));  // InnerEx -> route parent -> Inner's table
    EXPECT_NE(QI<IA>(e), nullptr);                               // the inner object queries back through the owner
    void* pv = reinterpret_cast<void*>(1);
    EXPECT_EQ(p->m_pInner->NonDelegatingQueryInterface(IID_IObject, &pv), FE_NOINTERFACE);
    EXPECT_EQ(pv, nullptr);
    // The probe: the inner object forwards it to the owner; its non-delegating table does not answer it.
    EXPECT_EQ(Probe(e), FS_OK);
    EXPECT_EQ(Probe(p), FS_OK);
    EXPECT_EQ(p->m_pInner->NonDelegatingQueryInterface(details::QIStrongRefProbeIID, nullptr), FE_NOINTERFACE);
    for (int i = 0; i < 6; ++i) p->Release();  // id, e, Identity(e), InnerEx, Inner, IA
    EXPECT_EQ(p->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceTable, DelegatingObjectWithOwnTable) {
    SharedOwner* p = MAKE_RC_OBJ(SharedOwner);
    IE* e = &p->m_Shared;
    EXPECT_EQ(QI<IE>(e), e);
    EXPECT_EQ(Identity(e), static_cast<IObject*>(e));  // its own identity
    EXPECT_EQ(QI<IA>(e), nullptr);                     // the owner's interfaces are not reachable
    EXPECT_EQ(RefCount(p), 3);                         // the references went to the owner
    EXPECT_EQ(Probe(e), FS_OK);                        // asked of the owner (NvrhiQIAnswerProbe)
    e->Release();
    e->Release();
    EXPECT_EQ(p->Release(), 0);
}

TEST(QueryInterfaceTable, WeakRootWithExtraInterface) {
    g_Destroyed = 0;
    Weak* p = MAKE_RC_OBJ(Weak);
    IWeakReferenceSource* ws = QI<IWeakReferenceSource>(p);
    IA* a0 = QI<IA>(p);
    ASSERT_TRUE(ws && a0);
    EXPECT_EQ(a0->A(), 41);
    EXPECT_EQ(Identity(a0), static_cast<IObject*>(ws));
    a0->Release();
    a0->Release();
    IWeakReference* wr = nullptr;
    ws->GetWeakReference(&wr);
    ws->Release();
    IA* a = nullptr;
    EXPECT_EQ(wr->Resolve(nvrhi::uuid_of<IA>(), reinterpret_cast<void**>(&a)), FS_OK);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->A(), 41);
    a->Release();
    static_cast<IA*>(p)->Release();
    EXPECT_EQ(g_Destroyed, 1);
    a = nullptr;
    EXPECT_NE(wr->Resolve(nvrhi::uuid_of<IA>(), reinterpret_cast<void**>(&a)), FS_OK);
    EXPECT_EQ(a, nullptr);
    wr->Release();
}

TEST(QueryInterfaceTable, OutOfLineTable) {
    OutOfLine* p = MAKE_RC_OBJ(OutOfLine);
    EXPECT_EQ(QI<IA>(p), static_cast<IA*>(p));
    EXPECT_EQ(Probe(p), FS_OK);
    p->Release();
    EXPECT_EQ(p->Release(), 0);
}

TEST(QueryInterfaceTable, PerClassCheckPrivateSections) {
    PrivateInherits* p = MAKE_RC_OBJ(PrivateInherits);
    IA* a = QI<IA>(static_cast<IA*>(p));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->A(), 82);
    void* pv = nullptr;  // the inherited table answers the base's class ID
    ASSERT_EQ(a->QueryInterface(nvrhi::uuid_of<PrivateTable>(), &pv), FS_OK);
    EXPECT_EQ(pv, static_cast<PrivateTable*>(p));
    EXPECT_EQ(Probe(a), FS_OK);
    a->Release();
    a->Release();
    EXPECT_EQ(a->Release(), 0);

    ProtectedOutOfLine* q = MAKE_RC_OBJ(ProtectedOutOfLine);
    IA* qa = static_cast<IA*>(q);
    EXPECT_EQ(QI<IA>(qa), qa);
    EXPECT_EQ(QI<IB>(qa), nullptr);
    qa->Release();
    EXPECT_EQ(qa->Release(), 0);
}

TEST(QueryInterfaceTable, PerClassCheckTemplatesAndOptOut) {
    TemplateTable<IA>* t = MAKE_RC_OBJ(TemplateTable<IA>);
    EXPECT_EQ(QI<IA>(t), static_cast<IA*>(t));
    EXPECT_EQ(QI<IA>(t)->A(), 85);
    for (int i = 0; i < 2; ++i) t->Release();
    EXPECT_EQ(t->Release(), 0);

    g_Destroyed = 0;
    TemplateInherits<int>* ti = MAKE_RC_OBJ(TemplateInherits<int>);
    IA* a = QI<IA>(static_cast<IB*>(ti));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->A(), 86);
    EXPECT_EQ(QI<Foo>(a), static_cast<Foo*>(ti));
    for (int i = 0; i < 2; ++i) a->Release();
    EXPECT_EQ(a->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);

    HandWritten* h = MAKE_RC_OBJ(HandWritten);
    EXPECT_EQ(QI<IA>(h), static_cast<IA*>(h));
    h->Release();
    EXPECT_EQ(h->Release(), 0);
}

TEST(QueryInterfaceTable, PerClassCheckDelegatingOptOut) {
    g_Destroyed = 0;
    Two* owner = MAKE_RC_OBJ(Two);
    InnerInherits* inner = MAKE_RC_DELEGATING(InnerInherits, static_cast<IA*>(owner));
    void* pv = nullptr;
    ASSERT_EQ(inner->NonDelegatingQueryInterface(nvrhi::uuid_of<IE>(), &pv), FS_OK);  // InnerEx's table
    EXPECT_EQ(static_cast<IE*>(pv)->E(), 51);
    static_cast<IE*>(pv)->Release();  // the reference went to the owner
    inner->DestroyObject();
    EXPECT_EQ(static_cast<IA*>(owner)->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceTable, NameIsolation) {
    using namespace QIUser;
    App::XLeaf* p = MAKE_RC_OBJ(App::XLeaf);
    IX* x = QI<IX>(p);
    IY* y = QI<IY>(p);  // XLeaf -> XRoot -> XMix
    ASSERT_TRUE(x && y);
    EXPECT_EQ(x->X(), 71);
    EXPECT_EQ(y->Y(), 72);
    EXPECT_EQ(Identity(y), static_cast<IObject*>(x));
    x->Release();
    y->Release();
    x->Release();  // identity
    EXPECT_EQ(static_cast<IX*>(p)->Release(), 0);

    App::XOwner* o = MAKE_RC_OBJ(App::XOwner);
    o->m_pInner = MAKE_RC_DELEGATING(App::XInner, o);
    IY* iy = QI<IY>(o);  // owner -> member -> XInner's non-delegating table
    ASSERT_NE(iy, nullptr);
    EXPECT_EQ(iy->Y(), 73);
    EXPECT_EQ(Identity(iy), static_cast<IObject*>(o));
    iy->Release();
    iy->Release();  // identity
    EXPECT_EQ(o->Release(), 0);
}

// ---- Interfaces derived from interfaces: every ancestor is listed ---------------------------------------
namespace QIDerivedTest {
NVRHI_IID(IBase, "02fa2645-d303-4e28-8429-4017971a8c3f")
struct IBase : IObject {
    NVRHI_DECLARE_UUID_TRAITS(IBase)
    virtual int Base() = 0;
};
NVRHI_IID(IMid, "cd60a988-f733-41da-9a8c-f6f78fa19aaa")
struct IMid : IBase {
    NVRHI_DECLARE_UUID_TRAITS(IMid)
    virtual int Mid() = 0;
};
NVRHI_IID(IDev, "75c5d9a6-cafd-4872-a717-04e0cc732550")
struct IDev : IMid {
    NVRHI_DECLARE_UUID_TRAITS(IDev)
    virtual int Dev() = 0;
};
// The interface is not the first base of the interface that derives from it: the entry adjusts the pointer.
struct Skew {
    virtual ~Skew() = default;
    int m_Skew = 0;
};
NVRHI_IID(ISkew, "5d67f8c8-7ddb-4cee-af6a-0d9869858a2b")
struct ISkew : Skew, IBase {
    NVRHI_DECLARE_UUID_TRAITS(ISkew)
};

// A same-named interface in a nested namespace, derived from the outer one (like nvrhi::d3d12::IDevice
// : nvrhi::IDevice). NVRHI_IID must bind its trait to backend::IDev, not to the outer IDev.
namespace backend {
NVRHI_IID(IDev, "6ad592e7-feda-42af-85ac-857f53f30b75")
struct IDev : QIDerivedTest::IDev {
    NVRHI_DECLARE_UUID_TRAITS(IDev)
    virtual int BackendDev() = 0;
};
}  // namespace backend

static int g_DevDestroyed = 0;

NVRHI_CLASS_CLSID(DevImpl, "e1a4c4f0-50d1-4d5c-9a43-4a2b7c1e0d01")
class DevImpl : public ObjectImpl<backend::IDev> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(DevImpl)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DevImpl)
    NVRHI_IMPLEMENTS_INTERFACE(backend::IDev)
    NVRHI_IMPLEMENTS_INTERFACE(QIDerivedTest::IDev)
    NVRHI_IMPLEMENTS_INTERFACE(IMid)
    NVRHI_IMPLEMENTS_INTERFACE(IBase)
    NVRHI_IMPLEMENTS_CLASS(DevImpl)
    NVRHI_END_INTERFACE_TABLE()
    ~DevImpl() { ++g_DevDestroyed; }
    int Base() override { return 1; }
    int Mid() override { return 2; }
    int Dev() override { return 3; }
    int BackendDev() override { return 4; }
};
// Lists only the most derived interface: the ancestors are not answered.
struct PartialImpl : ObjectImpl<backend::IDev> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(PartialImpl)
    NVRHI_IMPLEMENTS_INTERFACE(backend::IDev)
    NVRHI_END_INTERFACE_TABLE()
    int Base() override { return 5; }
    int Mid() override { return 6; }
    int Dev() override { return 7; }
    int BackendDev() override { return 8; }
};
struct SkewImpl : ObjectImpl<ISkew> {
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SkewImpl)
    NVRHI_IMPLEMENTS_INTERFACE(IBase)  // first: the IObject identity (ISkew starts with Skew, not an IObject)
    NVRHI_IMPLEMENTS_INTERFACE(ISkew)
    NVRHI_END_INTERFACE_TABLE()
    int Base() override { return 10; }
};

template <typename I>
void* QIRaw(IObject* p, FRESULT* pHr = nullptr) {
    void* pv = reinterpret_cast<void*>(1);
    FRESULT hr = p->QueryInterface(nvrhi::uuid_of<I>(), &pv);
    if (pHr) *pHr = hr;
    return pv;
}
}  // namespace QIDerivedTest

TEST(QueryInterfaceTable, NestedSameNamedInterfaces) {
    using namespace QIDerivedTest;
    EXPECT_NE(nvrhi::uuid_of<backend::IDev>(), nvrhi::uuid_of<QIDerivedTest::IDev>());
    EXPECT_EQ(nvrhi::uuid_of<backend::IDev>(), backend::IID_IDev);
    EXPECT_EQ(nvrhi::uuid_of<QIDerivedTest::IDev>(), QIDerivedTest::IID_IDev);
}

TEST(QueryInterfaceTable, EveryListedAncestorSameObject) {
    using namespace QIDerivedTest;
    g_DevDestroyed = 0;
    DevImpl* p = MAKE_RC_OBJ(DevImpl);
    backend::IDev* top = p;
    EXPECT_EQ(QIRaw<backend::IDev>(top), static_cast<void*>(top));
    EXPECT_EQ(QIRaw<QIDerivedTest::IDev>(top), static_cast<void*>(static_cast<QIDerivedTest::IDev*>(top)));
    EXPECT_EQ(QIRaw<IMid>(top), static_cast<void*>(static_cast<IMid*>(top)));
    EXPECT_EQ(QIRaw<IBase>(top), static_cast<void*>(static_cast<IBase*>(top)));
    EXPECT_EQ(QIRaw<IObject>(top), static_cast<void*>(static_cast<IObject*>(top)));
    EXPECT_EQ(QIRaw<DevImpl>(top), static_cast<void*>(p));
    EXPECT_EQ(static_cast<IBase*>(QIRaw<IBase>(top))->Base(), 1);
    EXPECT_EQ(static_cast<IMid*>(QIRaw<IMid>(top))->Mid(), 2);
    // An ancestor queries back up to the most derived interface.
    EXPECT_EQ(QIRaw<backend::IDev>(static_cast<IBase*>(QIRaw<IBase>(top))), static_cast<void*>(top));
    FRESULT hr = FS_OK;
    EXPECT_EQ(QIRaw<ISkew>(top, &hr), nullptr);
    EXPECT_EQ(hr, FE_NOINTERFACE);
    EXPECT_EQ(top->QueryInterface(IID_IBase, nullptr), FS_OK);  // null ppv: no AddRef
    // 10 successful queries above, plus the creation reference.
    for (int i = 0; i < 10; ++i) EXPECT_GT(top->Release(), 0);
    EXPECT_EQ(top->Release(), 0);
    EXPECT_EQ(g_DevDestroyed, 1);
}

TEST(QueryInterfaceTable, UnlistedAncestorRefused) {
    using namespace QIDerivedTest;
    PartialImpl* p = MAKE_RC_OBJ(PartialImpl);
    FRESULT hr = FS_OK;
    EXPECT_EQ(QIRaw<backend::IDev>(p), static_cast<void*>(static_cast<backend::IDev*>(p)));
    EXPECT_EQ(QIRaw<IBase>(p, &hr), nullptr);
    EXPECT_EQ(hr, FE_NOINTERFACE);
    EXPECT_EQ(QIRaw<QIDerivedTest::IDev>(p, &hr), nullptr);
    EXPECT_EQ(hr, FE_NOINTERFACE);
    EXPECT_GT(p->Release(), 0);
    EXPECT_EQ(p->Release(), 0);
}

TEST(QueryInterfaceTable, InterfaceNotFirstBase) {
    using namespace QIDerivedTest;
    SkewImpl* p = MAKE_RC_OBJ(SkewImpl);
    ISkew* s = p;
    void* base = QIRaw<IBase>(s);
    EXPECT_EQ(base, static_cast<void*>(static_cast<IBase*>(s)));
    EXPECT_NE(base, static_cast<void*>(s));
    EXPECT_EQ(static_cast<IBase*>(base)->Base(), 10);
    void* skew = QIRaw<ISkew>(static_cast<IBase*>(base));  // an ISkew* is not an IObject* at the same address
    EXPECT_EQ(skew, static_cast<void*>(s));
    EXPECT_EQ(QIRaw<IObject>(static_cast<IBase*>(base)), base);
    IBase* b = static_cast<IBase*>(base);
    EXPECT_EQ(RefCount(b), 4);  // the creation reference and three queries (references added via IObject)
    for (int i = 0; i < 3; ++i) EXPECT_GT(b->Release(), 0);
    EXPECT_EQ(b->Release(), 0);
}
