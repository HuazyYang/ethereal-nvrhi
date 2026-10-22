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

// The graphics factory (ADR 0010). The backends compiled into this build are named by the
// NVRHI_FACTORY_WITH_* definitions that CMakeLists.txt sets on the target that holds this file.

#include <nvrhi/factory.h>
#include <nvrhi/core/foundation.h>

#include "../common/backend-factories.h"

#include <new>

#ifndef NVRHI_FACTORY_WITH_D3D11
#define NVRHI_FACTORY_WITH_D3D11 0
#endif
#ifndef NVRHI_FACTORY_WITH_D3D12
#define NVRHI_FACTORY_WITH_D3D12 0
#endif
#ifndef NVRHI_FACTORY_WITH_VULKAN
#define NVRHI_FACTORY_WITH_VULKAN 0
#endif
#ifndef NVRHI_FACTORY_WITH_CUDA
#define NVRHI_FACTORY_WITH_CUDA 0
#endif
#ifndef NVRHI_FACTORY_WITH_VALIDATION
#define NVRHI_FACTORY_WITH_VALIDATION 0
#endif

namespace nvrhi
{
    namespace
    {
        template <typename TInterface>
        FRESULT Unsupported(TInterface** ppDevice) noexcept
        {
            if (!ppDevice)
                return FE_INVALID_ARGS;
            *ppDevice = nullptr;
            return FE_UNSUPPORTED;
        }

        // Clears the output and checks the arguments that every creation method takes.
        template <typename TInterface>
        bool BeginCreate(TInterface** ppDevice) noexcept
        {
            if (!ppDevice)
                return false;
            *ppDevice = nullptr;
            return true;
        }
    }

    NVRHI_CLASS_CLSID(GraphicsFactory, "34cd2858-5a99-4168-9bf7-7aa3a2d29482")
    class GraphicsFactory : public ObjectImpl<IGraphicsFactory>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(GraphicsFactory)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GraphicsFactory)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IGraphicsFactory)
        NVRHI_IMPLEMENTS_CLASS(GraphicsFactory)
        NVRHI_END_INTERFACE_TABLE()

        explicit GraphicsFactory(const GraphicsFactoryDesc& desc)
            : m_MessageCallback(desc.messageCallback)
        { }

        bool isBackendAvailable(GraphicsAPI api) noexcept override
        {
            switch (api)
            {
            case GraphicsAPI::D3D11:
                return NVRHI_FACTORY_WITH_D3D11 != 0;
            case GraphicsAPI::D3D12:
                return NVRHI_FACTORY_WITH_D3D12 != 0;
            case GraphicsAPI::VULKAN:
                return NVRHI_FACTORY_WITH_VULKAN != 0;
            case GraphicsAPI::CUDA:
#if NVRHI_FACTORY_WITH_CUDA
                return cuda::IsDriverAvailable();
#else
                return false;
#endif
            default:
                return false;
            }
        }

        FRESULT createD3D11Device(const d3d11::DeviceDesc& desc, IDevice** ppDevice) noexcept override
        {
#if NVRHI_FACTORY_WITH_D3D11
            if (!BeginCreate(ppDevice))
                return FE_INVALID_ARGS;
            return d3d11::CreateDevice(desc, ppDevice);
#else
            (void)desc;
            return unsupported(GraphicsAPI::D3D11, ppDevice);
#endif
        }

        FRESULT createD3D12Device(const d3d12::DeviceDesc& desc, d3d12::IDevice** ppDevice) noexcept override
        {
#if NVRHI_FACTORY_WITH_D3D12
            if (!BeginCreate(ppDevice))
                return FE_INVALID_ARGS;
            return d3d12::CreateDevice(desc, ppDevice);
#else
            (void)desc;
            return unsupported(GraphicsAPI::D3D12, ppDevice);
#endif
        }

        FRESULT createVulkanDevice(const vulkan::DeviceDesc& desc, vulkan::IDevice** ppDevice) noexcept override
        {
#if NVRHI_FACTORY_WITH_VULKAN
            if (!BeginCreate(ppDevice))
                return FE_INVALID_ARGS;
            return vulkan::CreateDevice(desc, ppDevice);
#else
            (void)desc;
            return unsupported(GraphicsAPI::VULKAN, ppDevice);
#endif
        }

        FRESULT createCudaDevice(const cuda::DeviceDesc& desc, cuda::IDevice** ppDevice) noexcept override
        {
#if NVRHI_FACTORY_WITH_CUDA
            if (!BeginCreate(ppDevice))
                return FE_INVALID_ARGS;
            return cuda::CreateDevice(desc, ppDevice);
#else
            (void)desc;
            return unsupported(GraphicsAPI::CUDA, ppDevice);
#endif
        }

        FRESULT createValidationLayer(IDevice* underlyingDevice, IDevice** ppDevice) noexcept override
        {
            if (!BeginCreate(ppDevice))
                return FE_INVALID_ARGS;
            if (!underlyingDevice)
                return FE_INVALID_ARGS;
#if NVRHI_FACTORY_WITH_VALIDATION
            return validation::CreateValidationLayer(underlyingDevice, ppDevice);
#else
            message(MessageSeverity::Warning, "NVRHI was built without the validation layer");
            return FE_UNSUPPORTED;
#endif
        }

    private:
        MessageCallbackHandle m_MessageCallback;

        void message(MessageSeverity severity, const char* text) noexcept
        {
            if (m_MessageCallback)
                m_MessageCallback->message(severity, text);
        }

        template <typename TInterface>
        [[maybe_unused]] FRESULT unsupported(GraphicsAPI api, TInterface** ppDevice) noexcept
        {
            switch (api)
            {
            case GraphicsAPI::D3D11: message(MessageSeverity::Error, "NVRHI was built without the D3D11 backend"); break;
            case GraphicsAPI::D3D12: message(MessageSeverity::Error, "NVRHI was built without the D3D12 backend"); break;
            case GraphicsAPI::VULKAN: message(MessageSeverity::Error, "NVRHI was built without the Vulkan backend"); break;
            case GraphicsAPI::CUDA: message(MessageSeverity::Error, "NVRHI was built without the CUDA backend"); break;
            default: break;
            }
            return Unsupported(ppDevice);
        }
    };

    FRESULT nvrhiCreateGraphicsFactory(const GraphicsFactoryDesc* pDesc, IGraphicsFactory** ppFactory) noexcept
    {
        if (!ppFactory)
            return FE_INVALID_ARGS;
        *ppFactory = nullptr;
        if (!pDesc)
            return FE_INVALID_ARGS;

        try
        {
            *ppFactory = MAKE_RC_OBJ(GraphicsFactory, *pDesc);
        }
        catch (const std::bad_alloc&)
        {
            return FE_OUT_OF_MEMORY;
        }
        return *ppFactory ? FS_OK : FE_GENERIC_ERROR;
    }
}
