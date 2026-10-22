/*
* Copyright (c) 2026, ethereal contributors. All rights reserved.
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

// The CUDA driver API, loaded at run time (ADR 0011): NVRHI does not link the driver's import library, so a
// build with the CUDA backend still loads on a machine without an NVIDIA driver.
//
// Every function the backend calls is a member of the function table g_Cuda, named as in <cuda.h>. cuda.h maps
// most names to versioned symbols with macros (cuMemAlloc -> cuMemAlloc_v2); the table members go through the
// same macros, so g_Cuda.cuMemAlloc(...) calls the version the headers declare. The functions are looked up with
// cuGetProcAddress under their unversioned names for the CUDA version of the headers.

#include <cuda.h>
#include <string>

#define NVRHI_CUDA_DRIVER_FUNCTIONS(X) \
    X(cuInit) \
    X(cuDriverGetVersion) \
    X(cuGetErrorName) \
    X(cuGetErrorString) \
    X(cuDeviceGetCount) \
    X(cuDeviceGet) \
    X(cuDeviceGetName) \
    X(cuDeviceGetAttribute) \
    X(cuDeviceGetLuid) \
    X(cuDeviceGetUuid) \
    X(cuDeviceTotalMem) \
    X(cuDevicePrimaryCtxRetain) \
    X(cuDevicePrimaryCtxRelease) \
    X(cuCtxPushCurrent) \
    X(cuCtxPopCurrent) \
    X(cuCtxSynchronize) \
    X(cuMemAlloc) \
    X(cuMemFree) \
    X(cuMemHostAlloc) \
    X(cuMemFreeHost) \
    X(cuMemHostGetDevicePointer) \
    X(cuMemGetInfo) \
    X(cuMemcpyAsync) \
    X(cuMemcpyHtoDAsync) \
    X(cuMemcpyDtoHAsync) \
    X(cuMemcpyDtoDAsync) \
    X(cuMemcpy3DAsync) \
    X(cuMemsetD8Async) \
    X(cuMemsetD32Async) \
    X(cuArray3DCreate) \
    X(cuArrayDestroy) \
    X(cuMipmappedArrayCreate) \
    X(cuMipmappedArrayDestroy) \
    X(cuMipmappedArrayGetLevel) \
    X(cuTexObjectCreate) \
    X(cuTexObjectDestroy) \
    X(cuSurfObjectCreate) \
    X(cuSurfObjectDestroy) \
    X(cuModuleLoadDataEx) \
    X(cuModuleUnload) \
    X(cuModuleGetFunction) \
    X(cuModuleGetGlobal) \
    X(cuFuncGetAttribute) \
    X(cuLaunchKernel) \
    X(cuStreamCreate) \
    X(cuStreamDestroy) \
    X(cuStreamSynchronize) \
    X(cuStreamQuery) \
    X(cuStreamWaitEvent) \
    X(cuEventCreate) \
    X(cuEventDestroy) \
    X(cuEventRecord) \
    X(cuEventQuery) \
    X(cuEventSynchronize) \
    X(cuEventElapsedTime) \
    X(cuImportExternalMemory) \
    X(cuExternalMemoryGetMappedBuffer) \
    X(cuExternalMemoryGetMappedMipmappedArray) \
    X(cuDestroyExternalMemory) \
    X(cuImportExternalSemaphore) \
    X(cuSignalExternalSemaphoresAsync) \
    X(cuWaitExternalSemaphoresAsync) \
    X(cuDestroyExternalSemaphore)

namespace nvrhi::cuda
{
    struct DriverApi
    {
#define NVRHI_CUDA_DRIVER_MEMBER(name) decltype(&::name) name = nullptr;
        NVRHI_CUDA_DRIVER_FUNCTIONS(NVRHI_CUDA_DRIVER_MEMBER)
#undef NVRHI_CUDA_DRIVER_MEMBER
    };

    // The driver function table; valid after LoadDriver returned true.
    extern DriverApi g_Cuda;

    // Loads the driver library (libraryName, or the default name when empty), looks up the functions and calls
    // cuInit. Runs once per process; later calls return the first result. Returns false with a message if the
    // library or a function is missing or cuInit fails.
    bool LoadDriver(const char* libraryName, std::string& outError);

    // "CUDA_ERROR_OUT_OF_MEMORY (out of memory)"
    std::string ResultToString(CUresult result);
}
