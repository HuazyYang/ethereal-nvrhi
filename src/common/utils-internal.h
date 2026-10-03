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

// Helpers used only inside nvrhi src (not exported). The public helpers are in <nvrhi/utils.h>.

#include <nvrhi/utils.h>
#include <nvrhi/core/foundation.h>
#include <new>
#include <string>

namespace nvrhi::utils
{
    // Implements an interface method that returns a new or existing object through an IXxx** (the COM protocol
    // of the public interfaces) on top of a function that returns it as a handle, nullptr on failure:
    // FS_OK and a new reference in *ppObject, or an FE_* code and *ppObject == nullptr. The interface methods are
    // noexcept, so an exception from 'get' (std::bad_alloc, ...) is turned into an error code here.
    //   FRESULT createTexture(const TextureDesc& d, ITexture** ppTexture) noexcept override
    //   { return utils::ReturnObject(ppTexture, [&] { return createTexture(d); }); }
    template <typename TInterface, typename TGet>
    FRESULT ReturnObject(TInterface** ppObject, TGet&& get, FRESULT failure = FE_GENERIC_ERROR) noexcept
    {
        if (!ppObject)
            return FE_INVALID_ARGS;

        *ppObject = nullptr;
        try
        {
            auto object = get();
            if (!object)
                return failure;

            *ppObject = object.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc&)
        {
            return FE_OUT_OF_MEMORY;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }

    // For interface methods that return an object through ppObject and are written out in full (the validation
    // layer): clears *ppObject first, false for a null ppObject.
    template <typename TInterface>
    bool ResetOutput(TInterface** ppObject) noexcept
    {
        if (!ppObject)
            return false;

        *ppObject = nullptr;
        return true;
    }

    // In the catch (...) handler of such a method: clears *ppObject and returns the error code for the exception.
    template <typename TInterface>
    FRESULT ExceptionToError(TInterface** ppObject) noexcept
    {
        if (ppObject)
            *ppObject = nullptr;

        try
        {
            throw;
        }
        catch (const std::bad_alloc&)
        {
            return FE_OUT_OF_MEMORY;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }

    inline const char* DebugNameToString(const string& debugName)
    {
        return debugName.empty() ? "<UNNAMED>" : debugName.c_str();
    }

    inline const char* DebugNameToString(const std::string& debugName)
    {
        return debugName.empty() ? "<UNNAMED>" : debugName.c_str();
    }

    inline const char* DebugNameToString(const char* debugName)
    {
        return (!debugName || !*debugName) ? "<UNNAMED>" : debugName;
    }

    std::string GenerateHeapDebugName(const HeapDesc& desc);
    std::string GenerateTextureDebugName(const TextureDesc& desc);
    std::string GenerateBufferDebugName(const BufferDesc& desc);

    void NotImplemented();
    void NotSupported();
    void InvalidEnum();
}
