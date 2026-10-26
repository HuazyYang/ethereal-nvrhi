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

// The CUDA graphics factory (ADR 0014): loads the CUDA driver, enumerates the CUDA devices and creates NVRHI CUDA
// devices and windowless swap chains.

#include <nvrhi/cuda.h>

#include "cuda-driver.h"
#include "../common/graphics-factory-base.h"

#include <cstring>
#include <new>

namespace nvrhi::cuda
{
    // cuda-swapchain.cpp
    FRESULT CreateSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device, IGraphicsSwapChain** ppSwapChain);

    NVRHI_CLASS_CLSID(GraphicsFactory, "e73aed20-8f9b-457d-9341-bf5a7207857e")
    class GraphicsFactory final : public GraphicsFactoryBase<IGraphicsFactory>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(GraphicsFactory)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GraphicsFactory)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::cuda::IGraphicsFactory)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IGraphicsFactory)
        NVRHI_IMPLEMENTS_CLASS(GraphicsFactory)
        NVRHI_END_INTERFACE_TABLE()

        explicit GraphicsFactory(const GraphicsFactoryDesc& desc)
            : GraphicsFactoryBase(desc.messageCallback)
            , m_DriverLibraryName(desc.driverLibraryName)
        { }

        bool initialize(std::string& outError)
        {
            if (!LoadDriver(m_DriverLibraryName.empty() ? nullptr : m_DriverLibraryName.c_str(), outError))
                return false;

            int count = 0;
            if (g_Cuda.cuDeviceGetCount(&count) != CUDA_SUCCESS)
                count = 0;

            for (int ordinal = 0; ordinal < count; ++ordinal)
            {
                CUdevice device = 0;
                if (g_Cuda.cuDeviceGet(&device, ordinal) != CUDA_SUCCESS)
                    continue;

                AdapterInfo info;
                char name[256] = {};
                g_Cuda.cuDeviceGetName(name, int(sizeof(name)), device);
                info.name = string(name);

                CUuuid uuid = {};
                if (g_Cuda.cuDeviceGetUuid(&uuid, device) == CUDA_SUCCESS)
                {
                    static_assert(sizeof(info.uuid) == sizeof(uuid.bytes));
                    memcpy(info.uuid, uuid.bytes, sizeof(info.uuid));
                    info.uuidValid = true;
                }

                char luid[8] = {};
                unsigned int nodeMask = 0;
                if (g_Cuda.cuDeviceGetLuid(luid, &nodeMask, device) == CUDA_SUCCESS)
                {
                    memcpy(info.luid, luid, sizeof(info.luid));
                    info.luidValid = true;
                }

                size_t totalMemory = 0;
                if (g_Cuda.cuDeviceTotalMem(&totalMemory, device) == CUDA_SUCCESS)
                    info.dedicatedVideoMemory = totalMemory;

                info.vendorID = 0x10DE;
                info.nativeAdapter = reinterpret_cast<NativeObject>(intptr_t(ordinal));
                m_Adapters.push_back(std::move(info));
            }

            return true;
        }

        // nvrhi::IGraphicsFactory
        GraphicsAPI getGraphicsAPI() noexcept override { return GraphicsAPI::CUDA; }
        NativeObject getNativeObject(ObjectType objectType) noexcept override { (void)objectType; return nullptr; }

        // cuda::IGraphicsFactory
        FRESULT createDevice(const DeviceDesc& desc, IDevice** ppDevice) noexcept override
        {
            if (!BeginCreate(ppDevice))
                return FE_INVALID_ARGS;
            return cuda::CreateDevice(desc, ppDevice);
        }

        FRESULT createSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device,
            IGraphicsSwapChain** ppSwapChain) noexcept override
        {
            if (!BeginCreate(ppSwapChain))
                return FE_INVALID_ARGS;
            if (!device || desc.width == 0 || desc.height == 0)
                return FE_INVALID_ARGS;
            try
            {
                const FRESULT result = CreateSwapChain(desc, device, ppSwapChain);
                if (NVRHI_FAILED(result))
                    error("Failed to create the CUDA swap chain");
                return result;
            }
            catch (const std::bad_alloc&)
            {
                return FE_OUT_OF_MEMORY;
            }
        }

    private:
        string m_DriverLibraryName;
    };

    FRESULT nvrhiCudaCreateGraphicsFactory(const GraphicsFactoryDesc* pDesc, IGraphicsFactory** ppFactory) noexcept
    {
        if (!BeginCreate(ppFactory))
            return FE_INVALID_ARGS;
        if (!pDesc)
            return FE_INVALID_ARGS;

        try
        {
            AutoPtr<GraphicsFactory> factory = MAKE_RC_OBJ_PTR(GraphicsFactory, *pDesc);
            std::string errorText;
            if (!factory->initialize(errorText))
            {
                if (pDesc->messageCallback)
                    pDesc->messageCallback->message(MessageSeverity::Error, ("CUDA graphics factory: " + errorText).c_str());
                return FE_UNSUPPORTED;
            }
            *ppFactory = factory.Detach();
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
}
