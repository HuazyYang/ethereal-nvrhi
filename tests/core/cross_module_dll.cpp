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

// The DLL side of the cross-module foundation.h test (see cross_module_exe.cpp). Built with the DLL CRT and its
// own NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT.

#define NVRHI_XM_DLL_BUILD 1
#include "cross_module.h"

#include <nvrhi/core/foundation.h>
#include <nvrhi/core/autoptr.h>
#include <nvrhi/common/misc.h>

#include <cstdlib>
#include <mutex>
#include <set>

namespace {

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

    XmAllocStats Stats() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats;
    }

 private:
    void* Track(void* p) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (p) {
            ++m_stats.allocations;
            m_live.insert(p);
        }
        return p;
    }
    bool Untrack(void* p) {
        if (!p) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_live.erase(p) == 0) {
            ++m_stats.foreignFrees;
            return false;
        }
        ++m_stats.frees;
        return true;
    }

    std::mutex m_mutex;
    std::set<void*> m_live;
    XmAllocStats m_stats{};
};

CountingAllocator g_Allocator;

NVRHI_CLASS_CLSID(DllCounter, "3801c3f4-5aeb-4bbb-8308-38d83744fcfe")
class DllCounter final : public nvrhi::ObjectImpl<IXmCounter> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(DllCounter)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DllCounter)
    NVRHI_IMPLEMENTS_INTERFACE(IXmCounter)
    NVRHI_IMPLEMENTS_CLASS(DllCounter)
    NVRHI_END_INTERFACE_TABLE()

    int Increment() noexcept override { return ++m_value; }

 private:
    int m_value = 0;
};

NVRHI_CLASS_CLSID(DllWeakCounter, "2d1cb160-a796-497b-9134-597f2ffb1a12")
class DllWeakCounter final : public nvrhi::WeakReferenceSourceImpl<IXmWeakCounter> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(DllWeakCounter)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DllWeakCounter)
    NVRHI_IMPLEMENTS_INTERFACE(IXmWeakCounter)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IWeakReferenceSource)
    NVRHI_IMPLEMENTS_CLASS(DllWeakCounter)
    NVRHI_END_INTERFACE_TABLE()

    int Increment() noexcept override { return ++m_value; }

 private:
    int m_value = 0;
};

}  // namespace

NVRHI_XM_API int nvrhiXmDllPackMode() noexcept { return NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT; }

NVRHI_XM_API nvrhi::IMemoryAllocator* nvrhiXmDllDefaultAllocator() noexcept { return nvrhi::GetDefaultMemAllocator(); }

NVRHI_XM_API void nvrhiXmDllCreateCounter(IXmCounter** ppCounter) noexcept {
    *ppCounter = nvrhi::MakeNewRCObj<nvrhi::IMemoryAllocator>(&g_Allocator).RcNew<DllCounter>();
}

NVRHI_XM_API void nvrhiXmDllCreateWeakCounter(IXmWeakCounter** ppCounter) noexcept {
    *ppCounter = nvrhi::MakeNewRCObj<nvrhi::IMemoryAllocator>(&g_Allocator).RcNew<DllWeakCounter>();
}

NVRHI_XM_API void nvrhiXmDllGetStats(XmAllocStats* pStats) noexcept { *pStats = g_Allocator.Stats(); }

NVRHI_XM_API int nvrhiXmDllCheckedCastOwnObjects() noexcept {
    nvrhi::AutoPtr<IXmCounter> counter;
    nvrhiXmDllCreateCounter(&counter);
    nvrhi::AutoPtr<IXmWeakCounter> weak;
    nvrhiXmDllCreateWeakCounter(&weak);

    // checked_cast verifies through the class IDs of this module's own classes (Debug builds).
    DllCounter* c = nvrhi::checked_cast<DllCounter*>(counter.Get());
    DllWeakCounter* w = nvrhi::checked_cast<DllWeakCounter*>(weak.Get());
    nvrhi::AutoPtr<DllCounter> viaClassId;
    const bool classIdAnswered =
        counter->QueryInterface(nvrhi::uuid_of<DllCounter>(), reinterpret_cast<void**>(viaClassId.GetAddressOf())) ==
        nvrhi::FS_OK;
    return (c == viaClassId.Get() && classIdAnswered && c->Increment() == 1 && w->Increment() == 1) ? 1 : 0;
}

NVRHI_XM_API void nvrhiXmDllRelease(nvrhi::IObject* pObject) noexcept { pObject->Release(); }

NVRHI_XM_API void nvrhiXmDllFillVector(nvrhi::vector<int>* pVector, int count) noexcept {
    for (int i = 0; i < count; ++i) pVector->push_back(i);
}

NVRHI_XM_API void nvrhiXmDllFillString(nvrhi::string* pString, const char* text) noexcept {
    *pString = text;
    pString->append(" (from the DLL, long enough for the heap)");
}

NVRHI_XM_API void nvrhiXmDllFreeVector(nvrhi::vector<int>* pVector) noexcept {
    pVector->clear();
    pVector->shrink_to_fit();
}

NVRHI_XM_API void nvrhiXmDllFreeString(nvrhi::string* pString) noexcept {
    pString->clear();
    pString->shrink_to_fit();
}

NVRHI_XM_API void nvrhiXmDllResizeAndReleaseBlob(nvrhi::IDataBlob* pBlob, size_t size) noexcept {
    pBlob->Resize(size);
    static_cast<char*>(pBlob->GetDataPtr())[size - 1] = 1;
    pBlob->Release();
}
