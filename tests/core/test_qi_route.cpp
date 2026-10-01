#include <nvrhi/core/Foundation.h>
#include <nvrhi/core/AutoPtr.h>

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

// Root with two interfaces: answers both; identity is the first one.
struct Two : ObjectImpl<IA, IB> {
    ~Two() { ++g_Destroyed; }
    int A() override { return 1; }
    int B() override { return 2; }
};

// Root with its own class IID; everything else is routed to ObjectImpl (interfaces IA, IB).
NVRHI_SCLSID(Foo, "f0000000-0000-0000-0000-00000000000f")
struct Foo : ObjectImpl<IA, IB>, Counter {
    NVRHI_DECLARE_UUID_TRAITS(Foo)
    ~Foo() { ++g_Destroyed; }
    int A() override { return 11; }
    int B() override { return 12; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Foo)
    NVRHI_IMPLEMENTS_INTERFACE(Foo)
    NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()
};

// Pass-through: Foo already owns the reference count. Bar -> Foo::QueryInterface -> ObjectImpl<...>.
NVRHI_SCLSID(Bar, "ba000000-0000-0000-0000-0000000000ba")
struct Bar : ObjectImpl<Foo> {
    NVRHI_DECLARE_UUID_TRAITS(Bar)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Bar)
    NVRHI_IMPLEMENTS_INTERFACE(Bar)
    NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()
};

// Routing through a class without a table binds to the nearest implementation.
struct Mid : ObjectImpl<IA> {
    int A() override { return 21; }
};
NVRHI_SCLSID(Leaf, "1eaf0000-0000-0000-0000-00000000001f")
struct Leaf : ObjectImpl<Mid> {
    NVRHI_DECLARE_UUID_TRAITS(Leaf)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Leaf)
    NVRHI_IMPLEMENTS_INTERFACE(Leaf)
    NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()
};

// Mixins: bases that implement QueryInterface with their own table but own no reference count.
NVRHI_SCLSID(P1, "c1000000-0000-0000-0000-0000000000c1")
struct P1 : IA {
    NVRHI_DECLARE_UUID_TRAITS(P1)
    int A() override { return 31; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(P1)
    NVRHI_IMPLEMENTS_INTERFACE(IA)
    NVRHI_IMPLEMENTS_INTERFACE(P1)
    NVRHI_END_INTERFACE_TABLE()
};
NVRHI_SCLSID(P2, "c2000000-0000-0000-0000-0000000000c2")
struct P2 : IB {
    NVRHI_DECLARE_UUID_TRAITS(P2)
    int B() override { return 32; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(P2)
    NVRHI_IMPLEMENTS_INTERFACE(IB)
    NVRHI_IMPLEMENTS_INTERFACE(P2)
    NVRHI_END_INTERFACE_TABLE()
};
NVRHI_SCLSID(Multi, "c3000000-0000-0000-0000-0000000000c3")
struct Multi : ObjectImpl<P1, P2>, Counter {
    NVRHI_DECLARE_UUID_TRAITS(Multi)
    ~Multi() { ++g_Destroyed; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Multi)
    NVRHI_IMPLEMENTS_INTERFACE(Multi)
    NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()
};

// Weak root and a weak pass-through. The user-provided constructors matter in non-packed mode
// (NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT=0): MakeNewRCObj stores the control-block pointer in the object's
// memory before constructing it, and MAKE_RC_OBJ(T) value-initializes T, which zeroes that memory first
// unless T has a user-provided default constructor.
struct WeakFoo : WeakReferenceSourceImpl<IWeakReferenceSource> {
    WeakFoo() {}
    ~WeakFoo() { ++g_Destroyed; }
};
NVRHI_SCLSID(WeakBar, "d0000000-0000-0000-0000-0000000000d0")
struct WeakBar : WeakReferenceSourceImpl<WeakFoo> {
    NVRHI_DECLARE_UUID_TRAITS(WeakBar)
    WeakBar() {}
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(WeakBar)
    NVRHI_IMPLEMENTS_INTERFACE(WeakBar)
    NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()
};

// Aggregation: the owner's table starts with a member route; identity must still be the owner's.
NVRHI_SCLSID(Inner, "1a000000-0000-0000-0000-0000000000a1")
struct Inner : DelegatingObjectImpl<IE> {
    NVRHI_DECLARE_UUID_TRAITS(Inner)
    Inner(IObject* pOwner) : DelegatingObjectImpl<IE>(pOwner) {}
    int E() override { return 51; }
    NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(Inner)
    NVRHI_IMPLEMENTS_INTERFACE(Inner)
    NVRHI_END_NON_DELEGATING_INTERFACE_TABLE_ROUTE_PARENT()
};
// Delegating pass-through: InnerEx -> Inner::NonDelegatingQueryInterface -> DelegatingObjectImpl<IE>.
NVRHI_SCLSID(InnerEx, "1b000000-0000-0000-0000-0000000000b1")
struct InnerEx : DelegatingObjectImpl<Inner> {
    NVRHI_DECLARE_UUID_TRAITS(InnerEx)
    InnerEx(IObject* pOwner) : DelegatingObjectImpl<Inner>(pOwner) {}
    NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(InnerEx)
    NVRHI_IMPLEMENTS_INTERFACE(InnerEx)
    NVRHI_END_NON_DELEGATING_INTERFACE_TABLE_ROUTE_PARENT()
};
struct Owner : ObjectImpl<IA> {
    Owner() { m_pInner = MAKE_RC_DELEGATING(InnerEx, this); }
    ~Owner() {
        m_pInner->DestroyObject();
        ++g_Destroyed;
    }
    int A() override { return 52; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Owner)
    NVRHI_IMPLEMENTS_ROUTE_MEMBER(m_pInner)
    NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()
    InnerEx* m_pInner;
};

// Weak root with an extra interface: two vptrs precede the control-block storage. User-provided
// constructor for the same reason as WeakFoo.
struct Weak : WeakReferenceSourceImpl<IWeakReferenceSource, IA> {
    Weak() {}
    ~Weak() { ++g_Destroyed; }
    int A() override { return 41; }
};

// The legacy form keeps working: leading offset entry + ROUTE_PARENT entry + NVRHI_END_INTERFACE_TABLE.
NVRHI_SCLSID(Legacy, "11111111-2222-3333-4444-555555555555")
struct Legacy : ObjectImpl<IA> {
    NVRHI_DECLARE_UUID_TRAITS(Legacy)
    int A() override { return 61; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Legacy)
    NVRHI_IMPLEMENTS_INTERFACE(Legacy)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(ObjectImpl<IA>)
    NVRHI_END_INTERFACE_TABLE()
};

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
    using Core = int;
    using Layer = int;
    using Lifetime = int;
    using Traits = int;
    using K = int;
    using B0 = int;
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
template <class... A> void QIQueryRoot(A&&...) = delete;
template <class... A> void InterfaceTableQueryInterface(A&&...) = delete;
template <class... A> void QIRouteTableQueryInterface(A&&...) = delete;
template <class... A> void RouteMemberQueryInterface(A&&...) = delete;
template <class... A> void QIDeclarer(A&&...) = delete;
template <class... A> void QIIsDelegating(A&&...) = delete;

// Mixin with its own table; its nested B would hijack a `->B::QueryInterface` call.
struct XMix : IY {
    using B = int;
    int Y() override { return 72; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(XMix)
    NVRHI_IMPLEMENTS_INTERFACE(IY)
    NVRHI_END_INTERFACE_TABLE()
};
struct XRoot : nvrhi::ObjectImpl<IX, XMix> {
    using Core = int;
    int X() override { return 71; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(XRoot)
    NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()
};
struct XLeaf : nvrhi::ObjectImpl<XRoot> {
    using Core = int;
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(XLeaf)
    NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()
};
struct XInner : nvrhi::DelegatingObjectImpl<IY> {
    using T = int;
    XInner(nvrhi::IObject* pOwner) : nvrhi::DelegatingObjectImpl<IY>(pOwner) {}
    int Y() override { return 73; }
    NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(XInner)
    NVRHI_END_NON_DELEGATING_INTERFACE_TABLE_ROUTE_PARENT()
};
struct XOwner : nvrhi::ObjectImpl<IX> {
    ~XOwner() {
        if (m_pInner) m_pInner->DestroyObject();
    }
    int X() override { return 74; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(XOwner)
    NVRHI_IMPLEMENTS_ROUTE_MEMBER(m_pInner)
    NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()
    XInner* m_pInner = nullptr;
};
}  // namespace App
}  // namespace QIUser

using namespace QIRouteTest;

// Compile-time classification of bases.
static_assert(details::QIKind<IA> == details::QIInterface, "interface: only IObject's pure QI");
static_assert(details::QIKind<Counter> == details::QINone, "helper: no QI");
static_assert(details::QIKind<Foo> == details::QIImpl, "own table");
static_assert(details::QIKind<Mid> == details::QIImpl, "lookup walks up to ObjectImpl");
static_assert(details::QIKind<P1> == details::QIImpl, "mixin with its own table");
static_assert(!details::QIOwnsRefCount<IA> && !details::QIOwnsRefCount<P1> && details::QIOwnsRefCount<Foo>, "");
static_assert(std::is_same_v<Foo::QITraits::Core, details::QIRootLayer<details::QILifetime::Object, IA, IB>>,
              "root: the layer that owns the reference count");
static_assert(std::is_same_v<Bar::QITraits::Core, details::QIPassThroughLayer<details::QILifetime::Object, Foo>>,
              "pass-through: ObjectImpl<Foo> over Foo");
static_assert(sizeof(Bar) == sizeof(Foo), "pass-through adds no reference count or vptr");

TEST(QueryInterfaceRoute, RootWithTwoInterfaces) {
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
    EXPECT_EQ(p->QueryInterface(nvrhi::uuid_of<IB>(), nullptr), FS_OK);  // probe: no AddRef
    FLONG n = 0;
    for (IObject* x : {static_cast<IObject*>(a), static_cast<IObject*>(b), id1, id2}) n = x->Release();
    EXPECT_EQ(n, 1);
    EXPECT_EQ(static_cast<IA*>(p)->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceRoute, PassThroughChain) {
    g_Destroyed = 0;
    Bar* p = MAKE_RC_OBJ(Bar);
    EXPECT_EQ(QI<Bar>(p), p);                   // Bar's table
    EXPECT_EQ(QI<Foo>(static_cast<IA*>(p)), p);  // Foo's table
    IA* a = QI<IA>(p);                           // ObjectImpl<...>
    IB* b = QI<IB>(p);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->A(), 11);
    EXPECT_EQ(b->B(), 12);
    IObject* id = Identity(b);
    EXPECT_EQ(id, static_cast<IObject*>(static_cast<IA*>(p)));
    EXPECT_EQ(QI<IE>(p), nullptr);
    for (int i = 0; i < 5; ++i) a->Release();  // QI<Bar>, QI<Foo>, a, b, id
    EXPECT_EQ(a->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceRoute, ThroughClassWithoutTable) {
    Leaf* p = MAKE_RC_OBJ(Leaf);
    EXPECT_EQ(QI<Leaf>(p), p);
    IA* a = QI<IA>(p);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->A(), 21);
    EXPECT_EQ(Identity(p), static_cast<IObject*>(p));
    EXPECT_EQ(QI<IB>(p), nullptr);
    for (int i = 0; i < 3; ++i) p->Release();
    EXPECT_EQ(p->Release(), 0);
}

TEST(QueryInterfaceRoute, MixinsInOrder) {
    g_Destroyed = 0;
    Multi* m = MAKE_RC_OBJ(Multi);
    IA* a = QI<IA>(static_cast<P1*>(m));
    IB* b = QI<IB>(static_cast<P1*>(m));  // answered by the second mixin
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->A(), 31);
    EXPECT_EQ(b->B(), 32);
    EXPECT_EQ(QI<Multi>(b), m);
    EXPECT_EQ(QI<P2>(a), static_cast<P2*>(m));
    EXPECT_EQ(Identity(b), static_cast<IObject*>(static_cast<IA*>(m)));  // first mixin's identity
    EXPECT_EQ(QI<IE>(a), nullptr);
    for (int i = 0; i < 5; ++i) a->Release();  // a, b, QI<Multi>, QI<P2>, identity
    EXPECT_EQ(a->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceRoute, WeakPassThrough) {
    g_Destroyed = 0;
    WeakBar* p = MAKE_RC_OBJ(WeakBar);
    EXPECT_EQ(QI<WeakBar>(p), p);
    IWeakReferenceSource* ws = QI<IWeakReferenceSource>(p);
    ASSERT_NE(ws, nullptr);
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

TEST(QueryInterfaceRoute, AggregationWithDelegatingPassThrough) {
    g_Destroyed = 0;
    Owner* p = MAKE_RC_OBJ(Owner);
    IObject* id = Identity(p);
    EXPECT_EQ(id, static_cast<IObject*>(static_cast<IA*>(p)));  // not the member field
    IE* e = QI<IE>(p);                                           // InnerEx -> Inner -> DelegatingObjectImpl
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->E(), 51);
    EXPECT_EQ(Identity(e), id);                                  // identity stays the owner's
    EXPECT_EQ(QI<InnerEx>(p), p->m_pInner);
    EXPECT_EQ(QI<Inner>(p), static_cast<Inner*>(p->m_pInner));
    EXPECT_NE(QI<IA>(e), nullptr);                               // the inner object queries back through the owner
    void* pv = nullptr;
    EXPECT_NE(p->m_pInner->NonDelegatingQueryInterface(IID_IObject, &pv), FS_OK);
    EXPECT_EQ(pv, nullptr);
    for (int i = 0; i < 6; ++i) p->Release();  // id, e, Identity(e), InnerEx, Inner, IA
    EXPECT_EQ(p->Release(), 0);
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(QueryInterfaceRoute, WeakRootWithExtraInterface) {
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

TEST(QueryInterfaceRoute, NameIsolation) {
    using namespace QIUser;
    App::XLeaf* p = MAKE_RC_OBJ(App::XLeaf);
    IX* x = QI<IX>(p);
    IY* y = QI<IY>(p);  // through XRoot's mixin
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
    IY* iy = QI<IY>(o);  // owner -> member -> XInner's non-delegating route
    ASSERT_NE(iy, nullptr);
    EXPECT_EQ(iy->Y(), 73);
    EXPECT_EQ(Identity(iy), static_cast<IObject*>(o));
    iy->Release();
    iy->Release();  // identity
    EXPECT_EQ(o->Release(), 0);
}

TEST(QueryInterfaceRoute, LegacyTable) {
    Legacy* p = MAKE_RC_OBJ(Legacy);
    EXPECT_EQ(QI<Legacy>(p), p);
    IA* a = QI<IA>(p);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->A(), 61);
    EXPECT_EQ(Identity(p), static_cast<IObject*>(static_cast<IA*>(p)));
    for (int i = 0; i < 3; ++i) p->Release();
    EXPECT_EQ(p->Release(), 0);
}
