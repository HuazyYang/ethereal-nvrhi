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

// The graphics factory: the one entry point that creates NVRHI devices of every backend and the validation
// layer (ADR 0010). The device descriptors stay in the backend headers (<nvrhi/d3d11.h>, <nvrhi/d3d12.h>,
// <nvrhi/vulkan.h>, <nvrhi/cuda.h>); include the header of the backend you create a device for.
//
//     nvrhi::GraphicsFactoryHandle factory = nvrhi::createGraphicsFactory(nvrhi::GraphicsFactoryDesc());
//     nvrhi::d3d12::DeviceHandle device;
//     factory->createD3D12Device(desc, &device);

#include <nvrhi/nvrhi.h>

namespace nvrhi
{
    namespace d3d11 { struct DeviceDesc; }
    namespace d3d12 { struct DeviceDesc; struct IDevice; }
    namespace vulkan { struct DeviceDesc; struct IDevice; }
    namespace cuda { struct DeviceDesc; struct IDevice; }

    struct GraphicsFactoryDesc
    {
        // Receives the factory's own messages (a backend that fails to load, ...). Optional; the factory keeps a
        // reference to it. The devices report through the callback in their own descriptors.
        IMessageCallback* messageCallback = nullptr;

        GraphicsFactoryDesc& setMessageCallback(IMessageCallback* value) { messageCallback = value; return *this; }
    };

    NVRHI_IID(IGraphicsFactory, "b69890d4-2483-41e6-9341-31f9fc6922e4")
    struct IGraphicsFactory : IObject
    {
        NVRHI_DECLARE_UUID_TRAITS(IGraphicsFactory)

        // True if the backend is compiled into this NVRHI build and, for backends whose driver library is loaded
        // at run time (CUDA), that library could be loaded.
        virtual bool isBackendAvailable(GraphicsAPI api) noexcept = 0;

        // Each creates a device of one backend. They return FS_OK and a new reference in the last parameter, or an
        // FE_* code and nullptr; FE_UNSUPPORTED if the backend is not compiled in or cannot be loaded.
        virtual FRESULT createD3D11Device(const d3d11::DeviceDesc& desc, IDevice** ppDevice) noexcept = 0;
        virtual FRESULT createD3D12Device(const d3d12::DeviceDesc& desc, d3d12::IDevice** ppDevice) noexcept = 0;
        virtual FRESULT createVulkanDevice(const vulkan::DeviceDesc& desc, vulkan::IDevice** ppDevice) noexcept = 0;
        virtual FRESULT createCudaDevice(const cuda::DeviceDesc& desc, cuda::IDevice** ppDevice) noexcept = 0;

        // Wraps underlyingDevice in the validation layer. FE_UNSUPPORTED if NVRHI was built without it.
        virtual FRESULT createValidationLayer(IDevice* underlyingDevice, IDevice** ppDevice) noexcept = 0;
    };

    typedef AutoPtr<IGraphicsFactory> GraphicsFactoryHandle;

    // Creates the graphics factory. Returns FS_OK and a new reference in *ppFactory, or an FE_* code and nullptr.
    NVRHI_C_API FRESULT nvrhiCreateGraphicsFactory(const GraphicsFactoryDesc* pDesc,
        IGraphicsFactory** ppFactory) noexcept;

    inline GraphicsFactoryHandle createGraphicsFactory(const GraphicsFactoryDesc& desc)
    {
        IGraphicsFactory* factory = nullptr;
        nvrhiCreateGraphicsFactory(&desc, &factory);
        return TakeOver(factory);
    }

    static_assert(sizeof(GraphicsFactoryDesc) == 8);
}
