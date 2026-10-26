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

#include "cuda-driver.h"
#include "../common/backend-factories.h"

#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace nvrhi::cuda
{
    DriverApi g_Cuda;

    namespace
    {
        // cuGetProcAddress as of CUDA 12.0 (the exported cuGetProcAddress_v2).
        typedef CUresult (CUDAAPI* PFN_GetProcAddress)(const char* symbol, void** pfn, int cudaVersion,
            cuuint64_t flags, CUdriverProcAddressQueryResult* symbolStatus);

        void* OpenLibrary(const char* name)
        {
#ifdef _WIN32
            return (void*)LoadLibraryA(name);
#else
            return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
        }

        void* LibrarySymbol(void* library, const char* name)
        {
#ifdef _WIN32
            return (void*)GetProcAddress(HMODULE(library), name);
#else
            return dlsym(library, name);
#endif
        }

        struct LoadState
        {
            std::once_flag once;
            bool loaded = false;
            std::string error;
        };

        LoadState& GetLoadState()
        {
            static LoadState state;
            return state;
        }

        bool Load(const char* libraryName, std::string& error)
        {
            const char* name = (libraryName && *libraryName) ? libraryName :
#ifdef _WIN32
                "nvcuda.dll";
#else
                "libcuda.so.1";
#endif
            // The library stays loaded for the rest of the process: g_Cuda points into it.
            void* library = OpenLibrary(name);
            if (!library)
            {
                error = std::string("cannot load the CUDA driver library ") + name;
                return false;
            }

            auto getProcAddress = reinterpret_cast<PFN_GetProcAddress>(LibrarySymbol(library, "cuGetProcAddress_v2"));
            if (!getProcAddress)
            {
                error = "the CUDA driver is too old: cuGetProcAddress_v2 is missing (CUDA 12.0 or newer is required)";
                return false;
            }

            DriverApi api;
            bool complete = true;
#define NVRHI_CUDA_DRIVER_LOAD(name) \
            { \
                void* pfn = nullptr; \
                CUdriverProcAddressQueryResult status = CU_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND; \
                if (getProcAddress(#name, &pfn, CUDA_VERSION, CU_GET_PROC_ADDRESS_DEFAULT, &status) != CUDA_SUCCESS || !pfn) \
                { \
                    if (complete) \
                        error = std::string("the CUDA driver does not provide ") + #name; \
                    complete = false; \
                } \
                api.name = reinterpret_cast<decltype(api.name)>(pfn); \
            }
            NVRHI_CUDA_DRIVER_FUNCTIONS(NVRHI_CUDA_DRIVER_LOAD)
#undef NVRHI_CUDA_DRIVER_LOAD

            if (!complete)
                return false;

            const CUresult result = api.cuInit(0);
            if (result != CUDA_SUCCESS)
            {
                const char* errorName = nullptr;
                api.cuGetErrorName(result, &errorName);
                error = std::string("cuInit failed: ") + (errorName ? errorName : "unknown error");
                return false;
            }

            g_Cuda = api;
            return true;
        }
    }

    bool LoadDriver(const char* libraryName, std::string& outError)
    {
        LoadState& state = GetLoadState();
        std::call_once(state.once, [&]
        {
            state.loaded = Load(libraryName, state.error);
        });
        outError = state.error;
        return state.loaded;
    }

    std::string ResultToString(CUresult result)
    {
        const char* name = nullptr;
        const char* text = nullptr;
        if (g_Cuda.cuGetErrorName)
            g_Cuda.cuGetErrorName(result, &name);
        if (g_Cuda.cuGetErrorString)
            g_Cuda.cuGetErrorString(result, &text);
        std::string s = name ? name : ("CUresult " + std::to_string(int(result)));
        if (text)
            s += std::string(" (") + text + ")";
        return s;
    }
}
