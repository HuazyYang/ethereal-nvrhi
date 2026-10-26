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

// The graphics factories (ADR 0014). There is one factory per backend: d3d11::IGraphicsFactory,
// d3d12::IGraphicsFactory, vulkan::IGraphicsFactory and cuda::IGraphicsFactory, each declared in its backend header
// with its own GraphicsFactoryDesc and creation function. They all derive from the IGraphicsFactory below.
//
// A backend factory owns the API objects of its backend (the DXGI factory and the loaded D3D DLLs, or the Vulkan
// library and VkInstance, or the CUDA driver). It enumerates the adapters, creates devices and swap chains
// (<nvrhi/swapchain.h>), and wraps devices in the validation layer.
//
//     nvrhi::d3d12::GraphicsFactoryDesc factoryDesc;
//     factoryDesc.sdkVersion = 619;                   // the Agility SDK in <exe dir>\D3D12
//     factoryDesc.sdkPath = ".\\D3D12\\";
//     nvrhi::d3d12::GraphicsFactoryHandle factory = nvrhi::d3d12::createGraphicsFactory(factoryDesc);
//
//     nvrhi::d3d12::DeviceCreationDesc deviceDesc;    // no native objects: the factory creates them
//     nvrhi::d3d12::DeviceHandle device;
//     factory->createDeviceOnAdapter(deviceDesc, &device);
//
// This header pulls in no D3D, Vulkan or CUDA header.

#include <nvrhi/nvrhi.h>

namespace nvrhi
{
    // An adapter (D3D: a DXGI adapter; Vulkan: a physical device; CUDA: a device ordinal) as a factory enumerates
    // it. Adapter indices are the indices of getAdapterInfo, which follow the order of IDXGIFactory::EnumAdapters,
    // vkEnumeratePhysicalDevices or cuDeviceGet. The *DeviceCreationDesc::adapterIndex fields use the same indices.
    struct AdapterInfo
    {
        string name;
        uint32_t vendorID = 0;
        uint32_t deviceID = 0;
        uint64_t dedicatedVideoMemory = 0;

        uint8_t uuid[16] = {};          // Vulkan and CUDA: the device UUID
        uint8_t luid[8] = {};           // D3D: the adapter LUID; Vulkan and CUDA: the LUID, when the driver reports one
        bool uuidValid = false;
        bool luidValid = false;
        bool isDiscrete = false;        // Vulkan: VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU; DXGI and CUDA do not report it
        bool isSoftware = false;        // D3D: DXGI_ADAPTER_FLAG_SOFTWARE (WARP); Vulkan: VK_PHYSICAL_DEVICE_TYPE_CPU

        // The native adapter, not AddRef'd, valid as long as the factory lives: IDXGIAdapter1* (D3D),
        // VkPhysicalDevice (Vulkan) or the CUdevice ordinal (CUDA, cast to intptr_t).
        NativeObject nativeAdapter = nullptr;
    };

    static_assert(offsetof(AdapterInfo, vendorID) == sizeof(string));
    static_assert(offsetof(AdapterInfo, uuid) == sizeof(string) + 16);
    static_assert(offsetof(AdapterInfo, nativeAdapter) == sizeof(string) + 48);
    static_assert(sizeof(AdapterInfo) == sizeof(string) + 56);

    NVRHI_IID(IGraphicsFactory, "b69890d4-2483-41e6-9341-31f9fc6922e4")
    struct IGraphicsFactory : IObject
    {
        NVRHI_DECLARE_UUID_TRAITS(IGraphicsFactory)

        // The backend of this factory. QueryInterface for d3d11::IGraphicsFactory, d3d12::IGraphicsFactory,
        // vulkan::IGraphicsFactory or cuda::IGraphicsFactory to create devices and swap chains.
        virtual GraphicsAPI getGraphicsAPI() noexcept = 0;

        // The adapters of the system, enumerated when the factory was created.
        virtual uint32_t getAdapterCount() noexcept = 0;
        // Fills outInfo and returns FS_OK, or returns FE_NOT_FOUND if index >= getAdapterCount().
        virtual FRESULT getAdapterInfo(uint32_t index, AdapterInfo& outInfo) noexcept = 0;

        // A native object of the factory, not AddRef'd, or nullptr: ObjectTypes::DXGI_Factory (IDXGIFactory2*),
        // ObjectTypes::D3D12_DeviceFactory (ID3D12DeviceFactory*, when the factory loads the Agility SDK),
        // ObjectTypes::VK_Instance, ObjectTypes::VK_GetInstanceProcAddr (PFN_vkGetInstanceProcAddr).
        virtual NativeObject getNativeObject(ObjectType objectType) noexcept = 0;

        // Wraps underlyingDevice in the validation layer. FE_UNSUPPORTED if NVRHI was built without it.
        virtual FRESULT createValidationLayer(IDevice* underlyingDevice, IDevice** ppDevice) noexcept = 0;
    };

    typedef AutoPtr<IGraphicsFactory> GraphicsFactoryHandle;
}
