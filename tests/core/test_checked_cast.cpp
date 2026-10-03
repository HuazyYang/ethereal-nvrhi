// checked_cast without RTTI (ADR 0006): the Debug check asks QueryInterface for the target's IID or class ID
// and compares the answer with the static_cast. With explicit interface tables (ADR 0007) the liveness probe
// is answered by the end of the class's table, which asks the base class that owns the reference count. It must never add a reference to an object whose strong
// count is zero (destructor, DestroyObject, pre-destroy callback) or to a weak-referenceable object that is
// still being constructed.
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/autoptr.h>
#include <nvrhi/common/misc.h>

#include "gtest/gtest.h"

using namespace nvrhi;

namespace CheckedCastTest {
NVRHI_IID(IFoo, "6f0c1e2a-5d1b-4a8e-9a43-1c7e0b5d2f01")
struct IFoo : IObject {
    NVRHI_DECLARE_UUID_TRAITS(IFoo)
    virtual int Foo() = 0;
};
NVRHI_IID(IBar, "6f0c1e2a-5d1b-4a8e-9a43-1c7e0b5d2f02")
struct IBar : IObject {
    NVRHI_DECLARE_UUID_TRAITS(IBar)
    virtual int Bar() = 0;
};

static int g_Destroyed = 0;

static FLONG RefCount(IObject* p) {
    p->AddRef();
    return p->Release();
}

static bool IsAlive(IObject* p) { return p->QueryInterface(details::QIStrongRefProbeIID, nullptr) == FS_OK; }

// A helper base that is not an IObject, listed first: the class ID entry must not depend on the layout.
struct Helper {
    int m_Value = 7;
};

NVRHI_CLASS_CLSID(FooImpl, "6f0c1e2a-5d1b-4a8e-9a43-1c7e0b5d2f11")
class FooImpl : public Helper, public ObjectImpl<IFoo> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(FooImpl)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(FooImpl)
    NVRHI_IMPLEMENTS_INTERFACE(IFoo)
    NVRHI_IMPLEMENTS_CLASS(FooImpl)
    NVRHI_END_INTERFACE_TABLE()
    ~FooImpl() { ++g_Destroyed; }
    int Foo() override { return 1; }
};

NVRHI_CLASS_CLSID(OtherFoo, "6f0c1e2a-5d1b-4a8e-9a43-1c7e0b5d2f12")
class OtherFoo : public ObjectImpl<IFoo> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(OtherFoo)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(OtherFoo)
    NVRHI_IMPLEMENTS_INTERFACE(IFoo)
    NVRHI_IMPLEMENTS_CLASS(OtherFoo)
    NVRHI_END_INTERFACE_TABLE()
    int Foo() override { return 2; }
};

// Derived from an implementation directly (ObjectImpl<FooImpl> does not compile): one reference count,
// FooImpl's. The table adds the class ID and routes to FooImpl's table; the probe is answered at the end of
// DerivedFoo's table, by FooImpl's ObjectImpl (public inheritance keeps NvrhiQIAnswerProbe reachable). It casts
// itself in its destructor, where the count is zero.
struct DerivedResult {
    bool alive = true;
    bool matches = false;
    bool matchesParent = false;
};
static DerivedResult g_Derived;

NVRHI_CLASS_CLSID(DerivedFoo, "6f0c1e2a-5d1b-4a8e-9a43-1c7e0b5d2f16")
class DerivedFoo : public FooImpl {
 public:
    NVRHI_DECLARE_UUID_TRAITS(DerivedFoo)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DerivedFoo)
    NVRHI_IMPLEMENTS_INTERFACE(IFoo)
    NVRHI_IMPLEMENTS_CLASS(DerivedFoo)
    NVRHI_IMPLEMENTS_ROUTE_PARENT(FooImpl)
    NVRHI_END_INTERFACE_TABLE()
    ~DerivedFoo() {
        IFoo* self = this;
        g_Derived.alive = IsAlive(self);
        g_Derived.matches = details::QICastMatches(self, this);
        g_Derived.matchesParent = details::QICastMatches(self, static_cast<FooImpl*>(this));
        (void)checked_cast<DerivedFoo*>(self);
    }
    int Foo() override { return 10; }
};

// Two interfaces: the cast from IBar must land on the same object.
NVRHI_CLASS_CLSID(FooBar, "6f0c1e2a-5d1b-4a8e-9a43-1c7e0b5d2f13")
class FooBar : public ObjectImpl<IFoo, IBar> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(FooBar)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(FooBar)
    NVRHI_IMPLEMENTS_INTERFACE(IFoo)
    NVRHI_IMPLEMENTS_INTERFACE(IBar)
    NVRHI_IMPLEMENTS_CLASS(FooBar)
    NVRHI_END_INTERFACE_TABLE()
    int Foo() override { return 3; }
    int Bar() override { return 4; }
};

// Aggregation: the owner answers IFoo with its inner object, so static_cast<IFoo*>(owner) is not the
// answer. checked_cast must report the mismatch.
struct InnerFoo : DelegatingObjectImpl<IFoo> {
    InnerFoo(IObject* pOwner) : DelegatingObjectImpl<IFoo>(pOwner) {}
    int Foo() override { return 5; }
    NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(InnerFoo)
    NVRHI_IMPLEMENTS_INTERFACE(IFoo)
    NVRHI_END_INTERFACE_TABLE()
};
struct OuterBar : ObjectImpl<IBar> {
    OuterBar() { m_pInner = MAKE_RC_DELEGATING(InnerFoo, this); }
    ~OuterBar() { m_pInner->DestroyObject(); }
    int Bar() override { return 6; }
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(OuterBar)
    NVRHI_IMPLEMENTS_INTERFACE(IBar)
    NVRHI_IMPLEMENTS_ROUTE_MEMBER(m_pInner)
    NVRHI_END_INTERFACE_TABLE()
    InnerFoo* m_pInner;
};

// Casts itself in its destructor, where the strong count is already zero. A QueryInterface with a pointer
// would AddRef 0 -> 1 and Release 1 -> 0, destroying the object a second time.
struct DtorResult {
    bool matches = false;
    bool alive = true;
    FooImpl* cast = nullptr;
};
static DtorResult g_Dtor;

NVRHI_CLASS_CLSID(SelfCastInDtor, "6f0c1e2a-5d1b-4a8e-9a43-1c7e0b5d2f14")
class SelfCastInDtor : public ObjectImpl<IFoo> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(SelfCastInDtor)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SelfCastInDtor)
    NVRHI_IMPLEMENTS_INTERFACE(IFoo)
    NVRHI_IMPLEMENTS_CLASS(SelfCastInDtor)
    NVRHI_END_INTERFACE_TABLE()
    ~SelfCastInDtor() {
        IFoo* self = this;
        g_Dtor.alive = IsAlive(self);
        g_Dtor.matches = details::QICastMatches(self, this);
        (void)checked_cast<SelfCastInDtor*>(self);  // asserts in Debug if the check fails
        ++g_Destroyed;
    }
    int Foo() override { return 8; }
};

// The same with weak references: the object is destroyed by TryDestroyObject, after its state became
// Destroyed. AddRef on it would trip NVRHI_VERIFY (state must be Alive).
struct WeakResult {
    bool aliveInCtor = true;
    bool matchesInCtor = false;
    bool aliveInPreDestroy = true;
    bool matchesInPreDestroy = false;
    bool aliveInDtor = true;
    bool matchesInDtor = false;
};
static WeakResult g_Weak;

NVRHI_CLASS_CLSID(WeakSelfCast, "6f0c1e2a-5d1b-4a8e-9a43-1c7e0b5d2f15")
class WeakSelfCast : public WeakReferenceSourceImpl<IWeakReferenceSource, IFoo> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(WeakSelfCast)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(WeakSelfCast)
    NVRHI_IMPLEMENTS_INTERFACE(IWeakReferenceSource)
    NVRHI_IMPLEMENTS_INTERFACE(IFoo)
    NVRHI_IMPLEMENTS_CLASS(WeakSelfCast)
    NVRHI_END_INTERFACE_TABLE()
    WeakSelfCast() {
        // Not attached to its control block yet: the state is NotInitialized.
        IFoo* self = this;
        g_Weak.aliveInCtor = IsAlive(self);
        g_Weak.matchesInCtor = details::QICastMatches(self, this);
    }
    ~WeakSelfCast() {
        IFoo* self = this;
        g_Weak.aliveInDtor = IsAlive(self);
        g_Weak.matchesInDtor = details::QICastMatches(self, this);
        (void)checked_cast<WeakSelfCast*>(self);
        ++g_Destroyed;
    }
    FLONG Release() noexcept override {
        return WeakReferenceSourceImpl<IWeakReferenceSource, IFoo>::Release([this]() {
            // The strong count just reached zero; the object is not destroyed yet.
            IFoo* self = this;
            g_Weak.aliveInPreDestroy = IsAlive(self);
            g_Weak.matchesInPreDestroy = details::QICastMatches(self, this);
        });
    }
    int Foo() override { return 9; }
};
}  // namespace CheckedCastTest

using namespace CheckedCastTest;

TEST(CheckedCast, ImplementationClass) {
    g_Destroyed = 0;
    {
        AutoPtr<FooImpl> obj = MAKE_RC_OBJ_PTR(FooImpl);
        IFoo* foo = obj;
        EXPECT_EQ(RefCount(foo), 1);

        FooImpl* impl = checked_cast<FooImpl*>(foo);
        EXPECT_EQ(impl, obj.Get());
        EXPECT_EQ(impl->m_Value, 7);
        EXPECT_TRUE(details::QICastMatches(foo, impl));
        EXPECT_EQ(RefCount(foo), 1);  // the check releases what it queried

        const IFoo* constFoo = foo;
        EXPECT_EQ(checked_cast<const FooImpl*>(constFoo), obj.Get());
        EXPECT_EQ(RefCount(foo), 1);

        EXPECT_EQ(checked_cast<FooImpl*>(static_cast<IFoo*>(nullptr)), nullptr);
    }
    EXPECT_EQ(g_Destroyed, 1);
}

TEST(CheckedCast, DerivedImplementation) {
    g_Destroyed = 0;
    g_Derived = {};
    {
        AutoPtr<DerivedFoo> obj = MAKE_RC_OBJ_PTR(DerivedFoo);
        IFoo* foo = obj;
        EXPECT_EQ(foo->Foo(), 10);
        EXPECT_EQ(checked_cast<DerivedFoo*>(foo), obj.Get());                        // DerivedFoo's table
        EXPECT_EQ(checked_cast<FooImpl*>(foo), static_cast<FooImpl*>(obj.Get()));  // FooImpl's, through the route
        EXPECT_EQ(checked_cast<const FooImpl*>(static_cast<const IFoo*>(foo))->m_Value, 7);
        EXPECT_TRUE(IsAlive(foo));
        EXPECT_EQ(details::QIAnswerProbe(obj.Get(), 0), FS_OK);
        EXPECT_EQ(RefCount(foo), 1);  // one reference count
        EXPECT_EQ(RefCount(static_cast<IFoo*>(static_cast<FooImpl*>(obj.Get()))), 1);
    }
    EXPECT_EQ(g_Destroyed, 1);  // destroyed once
    EXPECT_FALSE(g_Derived.alive);
    EXPECT_TRUE(g_Derived.matches);
    EXPECT_TRUE(g_Derived.matchesParent);
}

TEST(CheckedCast, Interfaces) {
    AutoPtr<FooBar> obj = MAKE_RC_OBJ_PTR(FooBar);
    IBar* bar = obj;
    IFoo* foo = obj;

    // Interface to class, and IObject to interface.
    EXPECT_EQ(checked_cast<FooBar*>(bar), obj.Get());
    EXPECT_EQ(checked_cast<FooBar*>(foo), obj.Get());
    IObject* object = foo;  // IObject of the first interface
    EXPECT_EQ(checked_cast<IFoo*>(object), foo);
    EXPECT_TRUE(details::QICastMatches(object, foo));
    EXPECT_EQ(RefCount(foo), 1);
}

TEST(CheckedCast, DetectsWrongClass) {
    AutoPtr<OtherFoo> other = MAKE_RC_OBJ_PTR(OtherFoo);
    IFoo* foo = other;
    // The static_cast compiles, but the object is not a FooImpl: it does not answer FooImpl's class ID.
    EXPECT_FALSE(details::QICastMatches(foo, static_cast<FooImpl*>(foo)));
    EXPECT_EQ(RefCount(foo), 1);

    // An unrelated interface.
    AutoPtr<FooImpl> impl = MAKE_RC_OBJ_PTR(FooImpl);
    IObject* object = static_cast<IFoo*>(impl);
    EXPECT_FALSE(details::QICastMatches(object, static_cast<IBar*>(object)));
    EXPECT_EQ(RefCount(object), 1);
}

TEST(CheckedCast, DetectsWrongPointer) {
    AutoPtr<OuterBar> outer = MAKE_RC_OBJ_PTR(OuterBar);
    IObject* object = static_cast<IBar*>(outer);
    // The object answers IFoo, but with its inner object, not with the static_cast of the outer one.
    void* pv = nullptr;
    ASSERT_EQ(object->QueryInterface(uuid_of<IFoo>(), &pv), FS_OK);
    static_cast<IFoo*>(pv)->Release();
    EXPECT_NE(pv, static_cast<void*>(static_cast<IFoo*>(object)));
    EXPECT_FALSE(details::QICastMatches(object, static_cast<IFoo*>(object)));
    EXPECT_EQ(RefCount(object), 1);
}

TEST(CheckedCast, LivenessProbe) {
    AutoPtr<FooImpl> obj = MAKE_RC_OBJ_PTR(FooImpl);
    IFoo* foo = obj;
    EXPECT_TRUE(IsAlive(foo));
    void* pv = reinterpret_cast<void*>(1);
    EXPECT_EQ(foo->QueryInterface(details::QIStrongRefProbeIID, &pv), FS_OK);
    EXPECT_EQ(pv, nullptr);  // the probe never returns a pointer
    EXPECT_EQ(RefCount(foo), 1);

    // Through the owner of an aggregated object.
    AutoPtr<OuterBar> outer = MAKE_RC_OBJ_PTR(OuterBar);
    EXPECT_TRUE(IsAlive(outer->m_pInner));

    // A null ppv only asks: no pointer, no reference.
    EXPECT_EQ(foo->QueryInterface(uuid_of<FooImpl>(), nullptr), FS_OK);
    EXPECT_EQ(foo->QueryInterface(uuid_of<IBar>(), nullptr), FE_NOINTERFACE);
    EXPECT_EQ(RefCount(foo), 1);
}

TEST(CheckedCast, ZeroRefCountInDestructor) {
    g_Destroyed = 0;
    g_Dtor = {};
    {
        AutoPtr<SelfCastInDtor> obj = MAKE_RC_OBJ_PTR(SelfCastInDtor);
        EXPECT_TRUE(IsAlive(static_cast<IFoo*>(obj)));
    }
    EXPECT_EQ(g_Destroyed, 1);  // destroyed once
    EXPECT_FALSE(g_Dtor.alive);
    EXPECT_TRUE(g_Dtor.matches);
}

TEST(CheckedCast, ZeroRefCountWeakObject) {
    g_Destroyed = 0;
    g_Weak = {};
    {
        AutoPtr<WeakSelfCast> obj = MAKE_RC_OBJ_PTR(WeakSelfCast);
        EXPECT_TRUE(IsAlive(static_cast<IFoo*>(obj)));
        EXPECT_TRUE(details::QICastMatches(static_cast<IFoo*>(obj), obj.Get()));

        WeakPtr<WeakSelfCast> weak(obj);
        obj.Reset();
        EXPECT_FALSE(weak.Lock());
    }
    EXPECT_EQ(g_Destroyed, 1);
    EXPECT_FALSE(g_Weak.aliveInCtor);
    EXPECT_TRUE(g_Weak.matchesInCtor);
    EXPECT_FALSE(g_Weak.aliveInPreDestroy);
    EXPECT_TRUE(g_Weak.matchesInPreDestroy);
    EXPECT_FALSE(g_Weak.aliveInDtor);
    EXPECT_TRUE(g_Weak.matchesInDtor);
}

TEST(CheckedCast, UncheckedCast) {
    AutoPtr<FooImpl> obj = MAKE_RC_OBJ_PTR(FooImpl);
    Helper* helper = obj.Get();
    // Helper is not an IObject: checked_cast does not compile for it, unchecked_cast is a static_cast.
    EXPECT_EQ(unchecked_cast<FooImpl*>(helper), obj.Get());
}
