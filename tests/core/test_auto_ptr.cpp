#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <array>
#include <list>
#include <type_traits>
#include <unordered_map>
#ifdef NVRHI_TEST_WITH_VLD
#include <vld.h>
#endif
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/autoptr.h>

#include "gtest/gtest.h"

using namespace nvrhi;

template <typename Type>
Type* MakeNewObj() {
    return MAKE_RC_OBJ(Type);
}

namespace Test {
// {82AA31B6-F0DF-4C11-864B-FC1643660D0B}
NVRHI_CCLSID(Object, "82aa31b6-f0df-4c11-864b-fc1643660d0b")
class Object : public WeakReferenceSourceImpl<IWeakReferenceSource> {
    NVRHI_DECLARE_UUID_TRAITS(Object)
 public:
    static void Create(Object** ppObj) { *ppObj = MakeNewObj<Object>(); }

    virtual FRESULT QueryInterface(const FIID& iid, void** ppInterface) override;

    Object()
        : m_Value(0) {}

    ~Object() {}
    std::atomic_int m_Value;
};

class StrongObject : public ObjectImpl<IObject> {
public:
    StrongObject() {}

    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(StrongObject)
    NVRHI_IMPLEMENTS_INTERFACE(IObject)
    NVRHI_END_INTERFACE_TABLE()

    std::atomic_int m_Value;
};

NVRHI_CCLSID(DelegatingObj, "139d4e04-fb74-4070-b642-a2e6e2d1709b")
class DelegatingObj : public DelegatingObjectImpl<IObject> {
    NVRHI_DECLARE_UUID_TRAITS(DelegatingObj)
 public:
    DelegatingObj(IObject* pOwner)
        : DelegatingObjectImpl<IObject>(pOwner) {}

    NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE(DelegatingObj)
    NVRHI_IMPLEMENTS_INTERFACE(IObject)
    NVRHI_IMPLEMENTS_CLASS(DelegatingObj)
    NVRHI_END_INTERFACE_TABLE()
};

NVRHI_BEGIN_INTERFACE_TABLE(Object)
NVRHI_IMPLEMENTS_INTERFACE(IWeakReferenceSource)
NVRHI_IMPLEMENTS_CLASS(Object)
NVRHI_END_INTERFACE_TABLE()

// {0CBC582D-66A2-452B-BAC4-CDACABA2D9A8}
NVRHI_CCLSID(DerivedObject, "0cbc582d-66a2-452b-bac4-cdacaba2d9a8")
class DerivedObject : public Object {
    NVRHI_DECLARE_UUID_TRAITS(DerivedObject)
 public:
    DerivedObject() : m_Value2{1} {}

    FRESULT QueryInterface(FREFIID riid, void** ppInterface) override;

    int m_Value2;
};

NVRHI_BEGIN_INTERFACE_TABLE(DerivedObject)
NVRHI_IMPLEMENTS_INTERFACE(IWeakReferenceSource)
NVRHI_IMPLEMENTS_CLASS(DerivedObject)
NVRHI_IMPLEMENTS_ROUTE_PARENT(Object)
NVRHI_END_INTERFACE_TABLE()

using SmartPtr = AutoPtr<Object>;
using WeakPtr = nvrhi::WeakPtr<Object>;
static_assert(
    std::is_same<WeakPtr::WeakRefType, nvrhi::details::WeakReferenceImpl>::value,
    "Implement weak reference type is requrired for WeakPtr");
// DerivedObject derives from the implementation Object directly: Object's control block (WeakRefImplType) is
// its control block too.
static_assert(
    std::is_same<nvrhi::WeakPtr<DerivedObject>::WeakRefType, nvrhi::details::WeakReferenceImpl>::value,
    "A class derived from an implementation keeps its control block type");
static_assert(nvrhi::details::IsObjectImpl<DerivedObject> && nvrhi::details::IsObjectImpl<DelegatingObj> &&
                  !nvrhi::details::IsObjectImpl<IWeakReferenceSource>,
              "Implementation classes, not interfaces");

TEST(Common, MakeNewRCObj) {
    auto Obj1 = MAKE_RC_OBJ(Object);
    auto Obj2 = MAKE_RC_OBJ(StrongObject);
    Obj1->Release();
    Obj2->Release();
}

TEST(Common_RefCntAutoPtr, Constructors) {
    {
        SmartPtr SP0;
        SmartPtr SP1(nullptr);
        auto* pRawPtr = MakeNewObj<Object>();
        SmartPtr SP2(pRawPtr);
        SmartPtr SP2_1(pRawPtr);
        pRawPtr->Release();
        EXPECT_EQ(SP2, SP2_1);

        SmartPtr SP3(SP0);
        SmartPtr SP4(SP2);
        SmartPtr SP5(std::move(SP3));
        EXPECT_TRUE(!SP3);
        SmartPtr SP6(std::move(SP4));
        EXPECT_TRUE(!SP4);

        AutoPtr<DerivedObject> DerivedSP = TakeOver(MakeNewObj<DerivedObject>());

        SmartPtr SP7(DerivedSP);
        SmartPtr SP8(std::move(DerivedSP));
        EXPECT_EQ(SP7, SP8);
        EXPECT_TRUE(!DerivedSP);
    }
}

TEST(Common_RefCntAutoPtr, AttachDetach) {
    {
        auto* pRawPtr = MakeNewObj<Object>();

        SmartPtr SP0;
        SP0.Attach(nullptr);
        EXPECT_TRUE(!SP0);
        SP0.Attach(pRawPtr);
        EXPECT_TRUE(SP0);
    }

    {
        auto* pRawPtr = MakeNewObj<Object>();

        SmartPtr SP0;
        SP0.Attach(pRawPtr);
        EXPECT_TRUE(SP0);
        SP0.Attach(nullptr);
        EXPECT_TRUE(!SP0);
    }

    {
        auto* pRawPtr = MakeNewObj<Object>();

        SmartPtr SP0(TakeOver(MakeNewObj<Object>()));
        SP0.Attach(pRawPtr);
        EXPECT_TRUE(SP0);
    }

    {
        SmartPtr SP0 = TakeOver(MakeNewObj<Object>());
        EXPECT_TRUE(SP0);

        auto* pRawPtr = MakeNewObj<Object>();
        SP0.Attach(pRawPtr);
        auto* pRawPtr2 = SP0.Detach();
        pRawPtr2->Release();

        auto* pRawPtr3 = SmartPtr().Detach();
        EXPECT_TRUE(pRawPtr3 == nullptr);
        auto* pRawPtr4 = SmartPtr(TakeOver(MakeNewObj<Object>())).Detach();
        EXPECT_TRUE(pRawPtr4 != nullptr);
        pRawPtr4->Release();
    }
}

TEST(Common_RefCntAutoPtr, OperatorEqual) {
    {
        SmartPtr SP0;
        auto pRawPtr1 = MakeNewObj<Object>();
        SmartPtr SP1(pRawPtr1);
        SmartPtr SP2(pRawPtr1);
        SP0 = SP0;
        SP0 = std::move(SP0);
        SP0 = nullptr;
        EXPECT_EQ(SP0, nullptr);

        SP1 = pRawPtr1;
        SP1 = SP1;
        SP1 = std::move(SP1);
        EXPECT_EQ(SP1.Get(), pRawPtr1);

        SP1 = SP2;
        SP1 = std::move(SP2);
        EXPECT_EQ(SP1.Get(), pRawPtr1);

        pRawPtr1->Release();

        auto pRawPtr2 = MakeNewObj<Object>();
        SmartPtr SP3(pRawPtr2);

        SP0 = pRawPtr2;
        SmartPtr SP4;
        SP4 = SP3;
        SmartPtr SP5;
        SP5 = std::move(SP4);
        EXPECT_TRUE(!SP4);

        SP1 = pRawPtr2;
        SP1 = nullptr;
        SP1 = std::move(SP5);
        EXPECT_TRUE(!SP5);

        pRawPtr2->Release();

        AutoPtr<DerivedObject> DerivedSP(TakeOver(MakeNewObj<DerivedObject>()));
        SP1 = DerivedSP;
        SP2 = std::move(DerivedSP);
        EXPECT_TRUE(!DerivedSP);
    }
}

TEST(Common_RefCntAutoPtr, LogicalOperators) {
    {
        auto pRawPtr1 = MakeNewObj<Object>();
        auto pRawPtr2 = MakeNewObj<Object>();
        SmartPtr SP0, SP1(pRawPtr1), SP2(pRawPtr1), SP3(pRawPtr2);
        EXPECT_TRUE(!SP0);
        bool b1 = SP0 != nullptr;
        EXPECT_TRUE(!b1);

        EXPECT_TRUE(!(!SP1));
        EXPECT_TRUE(SP1);
        EXPECT_TRUE(SP0 != SP1);
        EXPECT_TRUE(SP0 == SP0);
        EXPECT_TRUE(SP1 == SP1);
        EXPECT_TRUE(SP1 == SP2);
        EXPECT_TRUE(SP1 != SP3);
        EXPECT_TRUE(SP0 < SP3);
        EXPECT_TRUE((SP1 < SP3) == (pRawPtr1 < pRawPtr2));

        pRawPtr1->Release();
        pRawPtr2->Release();
    }
}

TEST(Common_RefCntAutoPtr, OperatorAmpersand) {
    {
        SmartPtr SP0, SP1(TakeOver(MakeNewObj<Object>())), SP2, SP3, SP4(TakeOver(MakeNewObj<Object>()));
        auto* pRawPtr = MakeNewObj<Object>();

        *static_cast<Object**>(&SP0) = pRawPtr;
        SP0.Detach();
        SP2 = pRawPtr;
        SP2.Detach();

        Object::Create(&SP3);

        Object::Create(&SP1);
        *static_cast<Object**>(&SP4) = pRawPtr;

        {
            SmartPtr SP5(TakeOver(MakeNewObj<Object>()));
            auto pDblPtr = &SP5;
            *(Object**)&SP5 = MakeNewObj<Object>();
            auto pDblPtr2 = &SP5;
            Object::Create(pDblPtr2);
        }

        SmartPtr SP6(TakeOver(MakeNewObj<Object>()));
        // This will not work:
        // Object **pDblPtr3 = &SP6;
        // *pDblPtr3 = new Object;

        pRawPtr->Release();
    }
}

TEST(Common_RefCntWeakPtr, Constructors) {
    {
        SmartPtr SP0, SP1(TakeOver(MakeNewObj<Object>()));
        WeakPtr WP0;
        WeakPtr WP1(WP0);
        WeakPtr WP2(SP0);
        WeakPtr WP3(SP1);
        WeakPtr WP4(WP3);
        WeakPtr WP5(std::move(WP0));
        WeakPtr WP6(std::move(WP4));

        auto* pRawPtr = MakeNewObj<Object>();
        WeakPtr WP7(pRawPtr);
        pRawPtr->Release();
    }
}

TEST(Common_RefCntWeakPtr, OperatorEqual) {
    {
        auto* pRawPtr = MakeNewObj<Object>();
        SmartPtr SP0, SP1(pRawPtr);
        WeakPtr WP0, WP1(SP1), WP2(SP1);
        WP0 = WP0;
        WP0 = std::move(WP0);
        WP1 = WP1;
        WP1 = std::move(WP1);
        WP1 = WP2;
        WP1 = std::move(WP2);
        WP1 = pRawPtr;
        WP0 = pRawPtr;
        WP0.Reset();
        WP0 = WP2;

        WP1 = WP0;
        WP0 = SP1;
        WP2 = std::move(WP1);

        pRawPtr->Release();
    }
}

TEST(Common_RefCntWeakPtr, Lock) {
    {
        SmartPtr SP0, SP1(TakeOver(MakeNewObj<Object>()));
        WeakPtr WP0, WP1(SP0), WP2(SP1), WP3(SP1);
        EXPECT_TRUE(WP0 == WP1);
        EXPECT_TRUE(WP0 != WP2);
        EXPECT_TRUE(WP2 == WP3);
        SP1.Reset();
        EXPECT_TRUE(WP2 == WP3);
    }

    // Test Lock()
    {
        SmartPtr SP0, SP1(TakeOver(MakeNewObj<Object>()));
        WeakPtr WP0, WP1(SP0), WP2(SP1);
        WeakPtr WP3(WP2);
        auto L1 = WP0.Lock();
        EXPECT_TRUE(!L1);
        L1 = WP1.Lock();
        EXPECT_TRUE(!L1);
        L1 = WP2.Lock();
        EXPECT_TRUE(L1);
        L1 = WP3.Lock();
        EXPECT_TRUE(L1);
        auto pRawPtr = SP1.Detach();
        L1.Reset();

        L1 = WP3.Lock();
        EXPECT_TRUE(L1);
        L1.Reset();

        pRawPtr->Release();

        L1 = WP3.Lock();
        EXPECT_TRUE(!L1);
    }
}

TEST(Common_RefCntAutoPtr, Misc) {

    {
        class OwnerTest : public WeakReferenceSourceImpl<IWeakReferenceSource> {
        public:
           OwnerTest(int* pFlag) : m_pFlag{pFlag} {
               Obj = MAKE_RC_DELEGATING(DelegatingObj, this);
               // Retain a weak reference
               IWeakReference* pWeakRef = nullptr;
               GetWeakReference(&pWeakRef);
               *pFlag = 0;
           }

            NVRHI_BEGIN_INTERFACE_TABLE_INLINE(OwnerTest)
            NVRHI_IMPLEMENTS_INTERFACE(IWeakReferenceSource)
            NVRHI_IMPLEMENTS_ROUTE_MEMBER(Obj)
            NVRHI_END_INTERFACE_TABLE()

            ~OwnerTest() {
                *m_pFlag = 1;
                Obj->DestroyObject();
                GetWeakReferenceImpl()->Release();  // Actually release weak reference
            }

        private:
            int* m_pFlag;
            DelegatingObj* Obj;
        };

        int Flag;
        OwnerTest* pOwnerObject = MAKE_RC_OBJ(OwnerTest, &Flag);
        AutoPtr<DelegatingObj> Obj;
        pOwnerObject->QueryInterface(NVRHI_IID_PPV_ARGS(&Obj));
        EXPECT_TRUE(Obj != nullptr);
        pOwnerObject->Release();
        Obj.Reset();
        EXPECT_EQ(Flag, 1);
    }

    {
        class SelfRefTest : public WeakReferenceSourceImpl<IWeakReferenceSource> {
        public:
            SelfRefTest(int* pFlag)
                : wpSelf(this),
                  m_pFlag{ pFlag } {
                *m_pFlag = 0;
            }

            virtual FRESULT QueryInterface(const FIID& IID, void** ppInterface) { return FS_OK; }

            ~SelfRefTest() { *m_pFlag = 1; }

        private:
            int* m_pFlag;
            nvrhi::WeakPtr<SelfRefTest> wpSelf;
        };

        int Flags;
        SelfRefTest* pSelfRefTest = MAKE_RC_OBJ(SelfRefTest, &Flags);
        pSelfRefTest->Release();
        EXPECT_EQ(Flags, 1);

        { AutoPtr<SelfRefTest> pSelfRefTest2 = MAKE_RC_OBJ_PTR(SelfRefTest, &Flags); }
        EXPECT_EQ(Flags, 1);
    }

    {
        class ExceptionTest1 : public WeakReferenceSourceImpl<IWeakReferenceSource> {
        public:
           ExceptionTest1() : wpSelf(this) { throw std::runtime_error("test exception"); }

           virtual FRESULT QueryInterface(const FIID& IID, void** ppInterface) {
               return FS_OK;
           }

        private:
            nvrhi::WeakPtr<ExceptionTest1> wpSelf;
        };

        try {
            auto* pExceptionTest = MakeNewObj<ExceptionTest1>();
            (void)pExceptionTest;
        } catch (std::runtime_error&) {
        }
    }

    {
        class ExceptionTest2 : public WeakReferenceSourceImpl<IWeakReferenceSource> {
        public:
           ExceptionTest2() : wpSelf(this) {
               throw std::runtime_error("test exception");
           }

            virtual FRESULT QueryInterface(const FIID& IID, void** ppInterface) { return FS_OK; }

        private:
            nvrhi::WeakPtr<ExceptionTest2> wpSelf;
        };

        try {
            auto* pExceptionTest =
                MAKE_RC_OBJ(ExceptionTest2);
            (void)pExceptionTest;
        } catch (std::runtime_error&) {
        }
    }

    {
        class ExceptionTest3 : public WeakReferenceSourceImpl<IWeakReferenceSource> {
        public:
           ExceptionTest3() : m_Member(*this) {}

           class Subclass {
            public:
                Subclass(ExceptionTest3& parent)
                    : wpSelf(&parent) {
                    throw std::runtime_error("test exception");
                }

            private:
                nvrhi::WeakPtr<ExceptionTest3> wpSelf;
            };
            virtual FRESULT QueryInterface(const FIID& IID, void** ppInterface) { return FS_OK; }

        private:
            Subclass m_Member;
        };

        try {
            auto* pExceptionTest =
                MAKE_RC_OBJ(ExceptionTest3);
            (void)pExceptionTest;
        } catch (std::runtime_error&) {
        }
    }

    {
        class OwnerObject : public WeakReferenceSourceImpl<IWeakReferenceSource> {
        public:
           OwnerObject() {}

           void CreateMember() {
               try {
                   m_pMember = MAKE_RC_OBJ_PTR(ExceptionTest4, *this);
               } catch (...) {
               }
            }
            virtual FRESULT QueryInterface(const FIID& IID, void** ppInterface) { return FS_OK; }

            class ExceptionTest4 : public WeakReferenceSourceImpl<IWeakReferenceSource> {
            public:
               ExceptionTest4(OwnerObject& owner) : m_Member(owner, *this) {}

               class Subclass {
                public:
                    Subclass(OwnerObject& owner, ExceptionTest4& parent)
                        : wpParent(&parent),
                          wpOwner(&owner) {
                        throw std::runtime_error("test exception");
                    }

                private:
                    nvrhi::WeakPtr<ExceptionTest4> wpParent;
                    nvrhi::WeakPtr<OwnerObject> wpOwner;
                };
                virtual FRESULT QueryInterface(const FIID& IID, void** ppInterface) { return FS_OK; }

            private:
                Subclass m_Member;
            };

            AutoPtr<ExceptionTest4> m_pMember;
        };

        AutoPtr<OwnerObject> pOwner(
            MAKE_RC_OBJ_PTR(OwnerObject));
        pOwner->CreateMember();
    }

    {
        class OwnerObject : public WeakReferenceSourceImpl<IWeakReferenceSource> {
        public:
            OwnerObject() {
                m_pMember = MAKE_RC_OBJ_PTR(ExceptionTest4, *this);
            }

            virtual FRESULT QueryInterface(const FIID& IID, void** ppInterface) { return FS_OK; }

            class ExceptionTest4 : public WeakReferenceSourceImpl<IWeakReferenceSource> {
            public:
               ExceptionTest4(OwnerObject& owner) : m_Member(owner, *this) {}

               class Subclass {
                public:
                    Subclass(OwnerObject& owner, ExceptionTest4& parent)
                        : wpParent(&parent),
                          wpOwner(&owner) {
                        throw std::runtime_error("test exception");
                    }

                private:
                    nvrhi::WeakPtr<ExceptionTest4> wpParent;
                    nvrhi::WeakPtr<OwnerObject> wpOwner;
                };
                virtual FRESULT QueryInterface(const FIID& IID, void** ppInterface) { return FS_OK; }

            private:
                Subclass m_Member;
            };

            AutoPtr<ExceptionTest4> m_pMember;
        };

        try {
            AutoPtr<OwnerObject> pOwner(
                MAKE_RC_OBJ_PTR(OwnerObject));
        } catch (...) {
        }
    }

    {
        class TestObject : public WeakReferenceSourceImpl<IWeakReferenceSource> {
        public:
           TestObject() {}

           virtual FRESULT QueryInterface(const FIID& IID,
                                          void** ppInterface) override final {
               return FS_OK;
           }

            inline virtual FLONG Release() override final {
                return WeakReferenceSourceImpl<IWeakReferenceSource>::Release([&]()                    //
                                                                 { ppWeakPtr->Reset(); }  //
                );
            }
            nvrhi::WeakPtr<TestObject>* ppWeakPtr = nullptr;
        };

        AutoPtr<TestObject> pObj(
            MAKE_RC_OBJ_PTR(TestObject));
        nvrhi::WeakPtr<TestObject> pWeakPtr(pObj);

        pObj->ppWeakPtr = &pWeakPtr;
        pObj.Reset();
    }
}

class RefCntAutoPtrThreadingTest {
public:
    ~RefCntAutoPtrThreadingTest();

    void StartConcurrencyTest();
    void RunConcurrencyTest();

    static void WorkerThreadFunc(RefCntAutoPtrThreadingTest* This, size_t ThreadNum);

    void StartWorkerThreadsAndWait(int SignalIdx);
    void WaitSiblingWorkerThreads(int SignalIdx);

    std::vector<std::thread> m_Threads;

    Object* m_pSharedObject = nullptr;
#ifdef DILIGENT_DEBUG
    static const int NumThreadInterations = 10000;
#else
    static const int NumThreadInterations = 50000;
#endif
    Signal m_WorkerThreadSignal[2];
    Signal m_MainThreadSignal;

    std::mutex m_NumThreadsCompletedMtx;
    std::atomic_int m_NumThreadsCompleted[2];
    std::atomic_int m_NumThreadsReady;
};

RefCntAutoPtrThreadingTest::~RefCntAutoPtrThreadingTest() {
    m_WorkerThreadSignal[0].Trigger(true, -1);

    for (auto& t : m_Threads) t.join();

    // ("Performed ", int{NumThreadInterations}, " iterations on ", m_Threads.size(), " threads");
}

void RefCntAutoPtrThreadingTest::WaitSiblingWorkerThreads(int SignalIdx) {
    auto NumThreads = static_cast<int>(m_Threads.size());
    if (++m_NumThreadsCompleted[SignalIdx] == NumThreads) {
        EXPECT_FALSE(m_WorkerThreadSignal[1 - SignalIdx].IsTriggered());
        m_MainThreadSignal.Trigger();
    } else {
        while (m_NumThreadsCompleted[SignalIdx] < NumThreads) std::this_thread::yield();
    }
}

void RefCntAutoPtrThreadingTest::StartWorkerThreadsAndWait(int SignalIdx) {
    m_NumThreadsCompleted[SignalIdx] = 0;
    m_WorkerThreadSignal[SignalIdx].Trigger(true);

    m_MainThreadSignal.Wait(true, 1);
}

void RefCntAutoPtrThreadingTest::WorkerThreadFunc(RefCntAutoPtrThreadingTest* This, size_t ThreadNum) {
    const int NumThreads = static_cast<int>(This->m_Threads.size());
    while (true) {
        for (int i = 0; i < NumThreadInterations; ++i) {
            // Wait until main() sends data
            auto SignaledValue = This->m_WorkerThreadSignal[0].Wait(true, NumThreads);
            if (SignaledValue < 0) {
                return;
            }

            {
                auto* pObject = This->m_pSharedObject;
                for (int j = 0; j < 100; ++j) {
                    // LOG_INFO_MESSAGE("t",std::this_thread::get_id(), ": AddRef" );
                    pObject->m_Value++;
                    pObject->AddRef();
                }
                This->WaitSiblingWorkerThreads(0);

                This->m_WorkerThreadSignal[1].Wait(true, NumThreads);
                for (int j = 0; j < 100; ++j) {
                    // LOG_INFO_MESSAGE("t",std::this_thread::get_id(), ": Release" );
                    pObject->m_Value--;
                    pObject->Release();
                }
                This->WaitSiblingWorkerThreads(1);
            }

            {
                This->m_WorkerThreadSignal[0].Wait(true, NumThreads);
                auto* pObject = This->m_pSharedObject;
                auto* pRefCounters = pObject->GetWeakReferenceImpl();
                if (ThreadNum % 3 == 0) {
                    pObject->m_Value++;
                    pObject->AddRef();
                } else
                    pRefCounters->AddRef();
                This->WaitSiblingWorkerThreads(0);

                This->m_WorkerThreadSignal[1].Wait(true, NumThreads);
                if (ThreadNum % 3 == 0) {
                    pObject->m_Value--;
                    pObject->Release();
                } else
                    pRefCounters->Release();
                This->WaitSiblingWorkerThreads(1);
            }

            {
                // Test interferences of ReleaseStrongRef() and QueryObject()

                // Goal: catch scenario when QueryObject() runs between
                // AtomicDecrement() and acquiring the lock in ReleaseStrongRef():

                //                       m_lNumStrongReferences == 1
                //

                //                                   Scenario I
                //
                //             Thread 1                 |                  Thread 2             |            Thread 3
                //                                      |                                       |
                //                                      |                                       |
                //                                      |                                       |
                //                                      |   1. Acquire the lock                 |
                //                                      |   2. Increment m_lNumStrongReferences |
                // 1. Decrement m_lNumStrongReferences  |   3. Read StrongRefCnt > 1            |
                // 2. Test RefCount!=0                  |   4. Return the reference to object   |
                // 3. DO NOT destroy the object         |                                       |
                // 4. Wait for the lock                 |                                       |

                //                                   Scenario I
                //
                //             Thread 1                 |                  Thread 2             |            Thread 3
                //                                      |                                       |
                //                                      |                                       |
                // 1. Decrement m_lNumStrongReferences  |                                       |
                //                                      |   1. Acquire the lock                 |
                // 2. Test RefCount==0                  |   2. Increment m_lNumStrongReferences |
                // 3. Start destroying the object       |   3. Read StrongRefCnt == 1           |
                // 4. Wait for the lock                 |   4. DO NOT create the object         |
                //                                      |   5. Decrement m_lNumStrongReferences |
                //                                      |                                       | 1. Acquire the lock
                //                                      |                                       | 2. Increment
                //                                      m_lNumStrongReferences | | 3. Read StrongRefCnt == 1 | | 4. DO
                //                                      NOT create the object | | 5. Decrement m_lNumStrongReferences
                // 5. Acquire the lock                  |
                // 6. DESTROY the object                |

                This->m_WorkerThreadSignal[0].Wait(true, NumThreads);
                auto* pObject = This->m_pSharedObject;

                nvrhi::WeakPtr<Object> weakPtr(pObject);
                AutoPtr<Object> strongPtr, strongPtr2;
                if (ThreadNum < 2) {
                    strongPtr = pObject;
                    strongPtr->m_Value++;
                } else
                    weakPtr = WeakPtr(pObject);
                This->WaitSiblingWorkerThreads(0);

                This->m_WorkerThreadSignal[1].Wait(true, NumThreads);
                if (ThreadNum == 0) {
                    strongPtr->m_Value--;
                    strongPtr.Reset();
                } else {
                    strongPtr2 = weakPtr.Lock();
                    if (strongPtr2)
                        strongPtr2->m_Value++;
                    weakPtr.Reset();
                }
                This->WaitSiblingWorkerThreads(1);
            }

            {
                This->m_WorkerThreadSignal[0].Wait(true, NumThreads);
                auto* pObject = This->m_pSharedObject;

                nvrhi::WeakPtr<Object> weakPtr;
                AutoPtr<Object> strongPtr;
                if (ThreadNum % 4 == 0) {
                    strongPtr = pObject;
                    strongPtr->m_Value++;
                } else
                    weakPtr = WeakPtr(pObject);
                This->WaitSiblingWorkerThreads(0);

                This->m_WorkerThreadSignal[1].Wait(true, NumThreads);
                if (ThreadNum % 4 == 0) {
                    strongPtr->m_Value--;
                    strongPtr.Reset();
                } else {
                    auto Ptr = weakPtr.Lock();
                    if (Ptr)
                        Ptr->m_Value++;
                    Ptr.Reset();
                }
                This->WaitSiblingWorkerThreads(1);
            }
        }
    }
}

void RefCntAutoPtrThreadingTest::StartConcurrencyTest() {
    auto numCores = std::thread::hardware_concurrency();
    m_Threads.resize(std::max(numCores, 4u));
    for (auto& t : m_Threads) t = std::thread(WorkerThreadFunc, this, &t - m_Threads.data());
}

void RefCntAutoPtrThreadingTest::RunConcurrencyTest() {
    for (int i = 0; i < NumThreadInterations; ++i) {
        m_pSharedObject = MakeNewObj<Object>();

        StartWorkerThreadsAndWait(0);

        StartWorkerThreadsAndWait(1);

        m_pSharedObject->Release();
        m_pSharedObject = MakeNewObj<Object>();

        StartWorkerThreadsAndWait(0);

        StartWorkerThreadsAndWait(1);

        m_pSharedObject->Release();

        {
            m_pSharedObject = MakeNewObj<Object>();

            StartWorkerThreadsAndWait(0);

            StartWorkerThreadsAndWait(1);

            m_pSharedObject->Release();
        }

        {
            m_pSharedObject = MakeNewObj<Object>();

            StartWorkerThreadsAndWait(0);

            StartWorkerThreadsAndWait(1);

            m_pSharedObject->Release();
        }
    }
}

TEST(Common_RefCntAutoPtr, Threading) {
    RefCntAutoPtrThreadingTest ThreadingTest;
    ThreadingTest.StartConcurrencyTest();
    ThreadingTest.RunConcurrencyTest();
}


// ---- MonoPtr ---------------------------------------------------------------------------------------

namespace MonoPtrTest {
struct Counted {
    static inline int alive = 0;
    int value;
    explicit Counted(int v = 0) : value(v) { ++alive; }
    virtual ~Counted() { --alive; }
};

struct DerivedCounted : Counted {
    int extra;
    DerivedCounted(int v, int e) : Counted(v), extra(e) {}
};

struct ThrowingCtor {
    ThrowingCtor() { throw 42; }
};
}  // namespace MonoPtrTest

TEST(Common_MonoPtr, Dereference) {
    using namespace MonoPtrTest;
    {
        MonoPtr<Counted> p = MakeMono<Counted>(7);
        EXPECT_EQ((*p).value, 7);
        (*p).value = 8;
        EXPECT_EQ(p->value, 8);

        const MonoPtr<Counted>& cp = p;
        static_assert(std::is_same_v<decltype(*cp), Counted&>, "operator* yields an lvalue of the pointee");
        EXPECT_EQ(&*cp, cp.Get());
    }
    EXPECT_EQ(Counted::alive, 0);
}

TEST(Common_MonoPtr, MakeMonoPropagatesExceptions) {
    using namespace MonoPtrTest;
    static_assert(!noexcept(MakeMono<Counted>(1)), "MakeMono must not be noexcept: new and the ctor may throw");
    EXPECT_THROW(MakeMono<ThrowingCtor>(), int);
}

TEST(Common_MonoPtr, DerivedToBase) {
    using namespace MonoPtrTest;
    static_assert(std::is_convertible_v<DefaultDeleter<DerivedCounted>, DefaultDeleter<Counted>>,
                  "DefaultDeleter<Derived> converts to DefaultDeleter<Base>");
    static_assert(!std::is_convertible_v<DefaultDeleter<Counted>, DefaultDeleter<DerivedCounted>>,
                  "DefaultDeleter<Base> does not convert to DefaultDeleter<Derived>");
    {
        MonoPtr<DerivedCounted> d = MakeMono<DerivedCounted>(1, 2);
        DerivedCounted* raw = d.Get();
        MonoPtr<Counted> b(std::move(d));
        EXPECT_TRUE(d == nullptr);
        EXPECT_EQ(b.Get(), raw);

        MonoPtr<Counted> b2;
        b2 = MakeMono<DerivedCounted>(3, 4);
        EXPECT_EQ(b2->value, 3);
        EXPECT_EQ(Counted::alive, 2);
    }
    EXPECT_EQ(Counted::alive, 0);
}

TEST(Common_MonoPtr, MoveOnlyInContainers) {
    using namespace MonoPtrTest;
    {
        std::array<MonoPtr<Counted>, 3> queues;
        EXPECT_TRUE(queues[0] == nullptr);
        queues[1] = MakeMono<Counted>(1);

        std::unordered_map<int, MonoPtr<Counted>> states;
        states.insert(std::make_pair(5, MakeMono<Counted>(5)));
        states.emplace(6, std::move(queues[1]));
        EXPECT_TRUE(queues[1] == nullptr);
        EXPECT_EQ(states.at(6)->value, 1);
        EXPECT_EQ(Counted::alive, 2);

        states.erase(5);
        EXPECT_EQ(Counted::alive, 1);
    }
    EXPECT_EQ(Counted::alive, 0);
}

// ---- Internal reference-counted helper objects (the pattern used for pooled backend objects) ------------

class PooledChunk;
NVRHI_CCLSID(PooledChunk, "cd78373c-e1d7-4f91-af6e-5ca4a801f053")
class PooledChunk final : public ObjectImpl<IObject> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(PooledChunk)

    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(PooledChunk)
    NVRHI_IMPLEMENTS_INTERFACE(IObject)
    NVRHI_IMPLEMENTS_CLASS(PooledChunk)
    NVRHI_END_INTERFACE_TABLE()

    static inline std::atomic_int alive{0};
    uint64_t version = 0;

    PooledChunk() { ++alive; }
    ~PooledChunk() { --alive; }
};

TEST(Common_RefCntAutoPtr, InternalPooledObject) {
    {
        std::list<AutoPtr<PooledChunk>> pool;
        AutoPtr<PooledChunk> current = MAKE_RC_OBJ_PTR(PooledChunk);
        EXPECT_EQ(PooledChunk::alive, 1);

        // Retire to the pool, then take it back: shared ownership as with std::shared_ptr.
        pool.push_back(current);
        current.Reset();
        EXPECT_EQ(PooledChunk::alive, 1);
        AutoPtr<PooledChunk> chunk = pool.front();
        pool.pop_front();
        EXPECT_EQ(PooledChunk::alive, 1);

        AutoPtr<IObject> identity;
        EXPECT_EQ(chunk->QueryInterface(IID_IObject, reinterpret_cast<void**>(identity.GetAddressOf())), FS_OK);
        EXPECT_EQ(identity.Get(), static_cast<IObject*>(chunk.Get()));

        AutoPtr<PooledChunk> byClassId;
        EXPECT_EQ(identity->QueryInterface(IID_PooledChunk, reinterpret_cast<void**>(byClassId.GetAddressOf())),
                  FS_OK);
        EXPECT_TRUE(byClassId == chunk);

        AutoPtr<IWeakReferenceSource> weakSource;
        EXPECT_NE(identity->QueryInterface(NVRHI_IID_PPV_ARGS(weakSource.GetAddressOf())), FS_OK);
        EXPECT_TRUE(weakSource == nullptr);
    }
    EXPECT_EQ(PooledChunk::alive, 0);
}

}  // namespace Test
