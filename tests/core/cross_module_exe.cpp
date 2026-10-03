/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

// The cross-module foundation.h test. A DLL (cross_module_dll.cpp, the DLL CRT) and this EXE (the CRT of the tree)
// each create COM objects with foundation.h, built with opposite NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT values, and
// hand each other interface pointers only:
// - F1: the EXE holds AutoPtr / WeakPtr references to DLL objects, drops the last strong reference, then the
//   last weak one, and Lock()s after expiry. The control block is reached through IWeakReference only, so all
//   of it runs (and frees) in the DLL: the DLL's allocator frees everything it allocated, the EXE's frees nothing
//   of it. The reverse direction too: the DLL drops the last references to EXE objects.
// - F2: the two packing modes differ, which is harmless once F1 holds.
// - F3: checked_cast and class IDs are used within each module only.
// - nvrhi_core: GetDefaultMemAllocator() is the same in both modules; containers and blobs filled in one module
//   are freed in the other.

#include "cross_module.h"

#include <nvrhi/core/foundation.h>
#include <nvrhi/core/autoptr.h>
#include <nvrhi/core/datablob.h>
#include <nvrhi/common/misc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace {

int g_Failures = 0;

#define XM_CHECK(cond)                                                     \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_Failures;                                                  \
        }                                                                  \
    } while (false)

class CountingAllocator final : public nvrhi::IMemoryAllocator {
 public:
    void* Allocate(size_t size) noexcept override { return Track(std::malloc(size)); }
    void Free(void* p) noexcept override {
        if (Untrack(p)) std::free(p);
    }
    void* AllocateAligned(size_t size, size_t alignment) noexcept override {
        return Track(_aligned_malloc(size, alignment));
    }
    void FreeAligned(void* p) noexcept override {
        if (Untrack(p)) _aligned_free(p);
    }

    XmAllocStats stats{};

 private:
    void* Track(void* p) {
        if (p) {
            ++stats.allocations;
            m_live.insert(p);
        }
        return p;
    }
    bool Untrack(void* p) {
        if (!p) return false;
        if (m_live.erase(p) == 0) {
            ++stats.foreignFrees;
            return false;
        }
        ++stats.frees;
        return true;
    }
    std::set<void*> m_live;
};

CountingAllocator g_ExeAllocator;

NVRHI_CLASS_CLSID(ExeWeakCounter, "9d043b09-e2d5-4156-8d7c-0a4642744523")
class ExeWeakCounter final : public nvrhi::WeakReferenceSourceImpl<IXmWeakCounter> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(ExeWeakCounter)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ExeWeakCounter)
    NVRHI_IMPLEMENTS_INTERFACE(IXmWeakCounter)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IWeakReferenceSource)
    NVRHI_IMPLEMENTS_CLASS(ExeWeakCounter)
    NVRHI_END_INTERFACE_TABLE()

    int Increment() noexcept override { return ++m_value; }

 private:
    int m_value = 0;
};

XmAllocStats DllStats() {
    XmAllocStats s{};
    nvrhiXmDllGetStats(&s);
    return s;
}

// F1 (and F2): DLL objects, referenced and finally released by the EXE.
void TestDllObjectsReleasedByExe() {
    const XmAllocStats before = DllStats();
    const XmAllocStats exeBefore = g_ExeAllocator.stats;
    {
        nvrhi::AutoPtr<IXmCounter> counter;
        nvrhiXmDllCreateCounter(&counter);
        XM_CHECK(counter && counter->Increment() == 1);
        nvrhi::AutoPtr<IXmCounter> copy = counter;
        counter = nullptr;
        XM_CHECK(copy->Increment() == 2);
    }  // the last strong reference of an ObjectImpl, dropped in the EXE

    {
        nvrhi::AutoPtr<IXmWeakCounter> strong;
        nvrhiXmDllCreateWeakCounter(&strong);
        nvrhi::WeakPtr<IXmWeakCounter> weak(strong);
        nvrhi::WeakPtr<IXmWeakCounter> weak2 = weak;
        XM_CHECK(weak.IsValid());
        {
            nvrhi::AutoPtr<IXmWeakCounter> locked = weak.Lock();
            XM_CHECK(locked && locked->Increment() == 1);
        }
        strong = nullptr;  // the last strong reference: the object is destroyed in the DLL
        XM_CHECK(!weak.IsValid());
        XM_CHECK(!weak.Lock());  // Lock() after expiry
        weak.Reset();
        XM_CHECK(!weak2.Lock());
    }  // the last weak reference: the control block is destroyed (and freed) in the DLL

    const XmAllocStats after = DllStats();
    XM_CHECK(after.allocations - before.allocations == 2);
    XM_CHECK(after.frees - before.frees == 2);  // the DLL freed everything it allocated
    XM_CHECK(after.foreignFrees == 0);
    XM_CHECK(g_ExeAllocator.stats.allocations == exeBefore.allocations);  // the EXE's allocator saw none of it
    XM_CHECK(g_ExeAllocator.stats.frees == exeBefore.frees);
    XM_CHECK(g_ExeAllocator.stats.foreignFrees == 0);
}

// The reverse: EXE objects whose last strong and weak references the DLL drops.
void TestExeObjectsReleasedByDll() {
    const XmAllocStats dllBefore = DllStats();
    const XmAllocStats before = g_ExeAllocator.stats;
    {
        IXmWeakCounter* object = nvrhi::MakeNewRCObj<nvrhi::IMemoryAllocator>(&g_ExeAllocator).RcNew<ExeWeakCounter>();
        nvrhi::IWeakReference* weakRef = nullptr;
        object->GetWeakReference(&weakRef);
        nvrhi::WeakPtr<IXmWeakCounter> weak(object);
        nvrhiXmDllRelease(object);  // the last strong reference, released by DLL code
        XM_CHECK(!weak.Lock());
        weak.Reset();
        nvrhiXmDllRelease(weakRef);  // the last weak reference, released by DLL code
    }
    XM_CHECK(g_ExeAllocator.stats.allocations - before.allocations == 1);
    XM_CHECK(g_ExeAllocator.stats.frees - before.frees == 1);
    XM_CHECK(g_ExeAllocator.stats.foreignFrees == 0);
    const XmAllocStats dllAfter = DllStats();
    XM_CHECK(dllAfter.allocations == dllBefore.allocations && dllAfter.frees == dllBefore.frees);
}

// F3: checked_cast within each module, on that module's own objects.
void TestCheckedCastWithinModules() {
    XM_CHECK(nvrhiXmDllCheckedCastOwnObjects() == 1);

    nvrhi::AutoPtr<IXmWeakCounter> object =
        nvrhi::TakeOver(static_cast<IXmWeakCounter*>(MAKE_RC_OBJ(ExeWeakCounter)));
    ExeWeakCounter* impl = nvrhi::checked_cast<ExeWeakCounter*>(object.Get());
    XM_CHECK(impl->Increment() == 1);
    XM_CHECK(object->QueryInterface(nvrhi::uuid_of<ExeWeakCounter>(), nullptr) == nvrhi::FS_OK);
}

// nvrhi_core: one default allocator, shared by both modules (the EXE on the static CRT, the DLL on the DLL CRT).
void TestSharedHeap() {
    XM_CHECK(nvrhi::GetDefaultMemAllocator() == nvrhiXmDllDefaultAllocator());

    {
        nvrhi::vector<int> v;
        nvrhiXmDllFillVector(&v, 1000);  // allocated in the DLL
        XM_CHECK(v.size() == 1000 && v[999] == 999);
        v.push_back(1000);               // reallocated in the EXE
        nvrhi::string s;
        nvrhiXmDllFillString(&s, "a string");
        XM_CHECK(s.find("from the DLL") != nvrhi::string::npos);
        s += " and from the EXE";
    }  // freed in the EXE

    {
        nvrhi::vector<int> v;
        for (int i = 0; i < 1000; ++i) v.push_back(i);  // allocated in the EXE
        nvrhiXmDllFreeVector(&v);                      // freed in the DLL
        XM_CHECK(v.empty() && v.capacity() == 0);
        nvrhi::string s = "a string long enough for the heap, from the EXE";
        nvrhiXmDllFreeString(&s);
        XM_CHECK(s.empty());
    }

    nvrhi::IDataBlob* blob = nullptr;
    XM_CHECK(nvrhi::CreateBlob(16, &blob) == nvrhi::FS_OK && blob);
    std::memset(blob->GetDataPtr(), 0, blob->GetSize());
    nvrhiXmDllResizeAndReleaseBlob(blob, 1 << 20);  // resized and released by the DLL

    nvrhi::IDataBlob* none = nullptr;
    XM_CHECK(nvrhi::CreateBlob(16, nullptr) == nvrhi::FE_INVALID_ARGS);
    XM_CHECK(nvrhi::CreateStringBlob(5, &none) == nvrhi::FS_OK && none->GetSize() == 5);
    none->Release();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // the output so far survives a crash
    std::printf("cross-module test: DLL NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT=%d, EXE=%d\n", nvrhiXmDllPackMode(),
                NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT);
    XM_CHECK(nvrhiXmDllPackMode() != NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT);

    std::printf("TestDllObjectsReleasedByExe\n");
    TestDllObjectsReleasedByExe();
    std::printf("TestExeObjectsReleasedByDll\n");
    TestExeObjectsReleasedByDll();
    std::printf("TestCheckedCastWithinModules\n");
    TestCheckedCastWithinModules();
    std::printf("TestSharedHeap\n");
    TestSharedHeap();

    if (g_Failures) {
        std::printf("%d check(s) failed\n", g_Failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
