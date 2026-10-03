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

#pragma once

// The interface between the two modules of the cross-module foundation.h test: a test DLL (cross_module_dll.cpp,
// the DLL CRT) and a test EXE (cross_module_exe.cpp, the CRT of the tree). They share interfaces and C functions
// only, as the module-private rule (foundation.h) requires; each module has its own implementation classes.

#include <nvrhi/core/types.h>
#include <nvrhi/core/memory.h>
#include <nvrhi/core/containers.h>

#if defined(NVRHI_XM_DLL_BUILD)
#define NVRHI_XM_API extern "C" __declspec(dllexport)
#else
#define NVRHI_XM_API extern "C" __declspec(dllimport)
#endif

NVRHI_IID(IXmCounter, "9d65d40c-eca9-4353-92c5-51b212b8a8ad")
struct IXmCounter : nvrhi::IObject {
    NVRHI_DECLARE_UUID_TRAITS(IXmCounter)
    virtual int Increment() noexcept = 0;
};

// A weak-referenceable counter.
NVRHI_IID(IXmWeakCounter, "31ed5cdc-e8a4-4a92-9034-5055c5815e60")
struct IXmWeakCounter : nvrhi::IWeakReferenceSource {
    NVRHI_DECLARE_UUID_TRAITS(IXmWeakCounter)
    virtual int Increment() noexcept = 0;
};

// The counts of a module's counting allocator: blocks allocated, blocks freed, and frees of blocks it never
// allocated.
struct XmAllocStats {
    int64_t allocations;
    int64_t frees;
    int64_t foreignFrees;
};

// The DLL's NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT.
NVRHI_XM_API int nvrhiXmDllPackMode() noexcept;
// GetDefaultMemAllocator() as the DLL sees it.
NVRHI_XM_API nvrhi::IMemoryAllocator* nvrhiXmDllDefaultAllocator() noexcept;
// Objects created by the DLL with its own counting allocator.
NVRHI_XM_API void nvrhiXmDllCreateCounter(IXmCounter** ppCounter) noexcept;
NVRHI_XM_API void nvrhiXmDllCreateWeakCounter(IXmWeakCounter** ppCounter) noexcept;
NVRHI_XM_API void nvrhiXmDllGetStats(XmAllocStats* pStats) noexcept;
// checked_cast inside the DLL, on objects of the DLL: 1 when every cast checks out.
NVRHI_XM_API int nvrhiXmDllCheckedCastOwnObjects() noexcept;
// Releases a reference in the DLL (the last one of an object the EXE created, for instance).
NVRHI_XM_API void nvrhiXmDllRelease(nvrhi::IObject* pObject) noexcept;
// Containers: filled by the DLL (allocated there), emptied by the DLL (freed there).
NVRHI_XM_API void nvrhiXmDllFillVector(nvrhi::vector<int>* pVector, int count) noexcept;
NVRHI_XM_API void nvrhiXmDllFillString(nvrhi::string* pString, const char* text) noexcept;
NVRHI_XM_API void nvrhiXmDllFreeVector(nvrhi::vector<int>* pVector) noexcept;
NVRHI_XM_API void nvrhiXmDllFreeString(nvrhi::string* pString) noexcept;
// Resizes a blob (reallocating its storage) and releases it, in the DLL.
NVRHI_XM_API void nvrhiXmDllResizeAndReleaseBlob(nvrhi::IDataBlob* pBlob, size_t size) noexcept;
