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

#include <vulkan/vulkan.h>
#include <nvrhi/nvrhi.h>

namespace nvrhi 
{
    namespace ObjectTypes
    {
        constexpr ObjectType Nvrhi_VK_Device = 0x00030101;
    };
}

namespace nvrhi::vulkan
{
    NVRHI_IID(IDevice, "164f6617-d205-45c4-8f3e-a687a229c270")
    struct IDevice : nvrhi::IDevice
    {
        NVRHI_DECLARE_UUID_TRAITS(IDevice)
        // Additional Vulkan-specific public methods
        virtual VkSemaphore getQueueSemaphore(CommandQueue queue) noexcept = 0;
        virtual void queueWaitForSemaphore(CommandQueue waitQueue, VkSemaphore semaphore, uint64_t value) noexcept = 0;
        virtual void queueSignalSemaphore(CommandQueue executionQueue, VkSemaphore semaphore, uint64_t value) noexcept = 0;
        virtual uint64_t queueGetCompletedInstance(CommandQueue queue) noexcept = 0;
    };

    typedef AutoPtr<IDevice> DeviceHandle;

    struct DeviceDesc
    {
        IMessageCallback* errorCB = nullptr; // the device keeps a reference to it

        VkInstance instance;
        VkPhysicalDevice physicalDevice;
        VkDevice device;

        // any of the queues can be null if this context doesn't intend to use them
        VkQueue graphicsQueue;
        int graphicsQueueIndex = -1;
        VkQueue transferQueue;
        int transferQueueIndex = -1;
        VkQueue computeQueue;
        int computeQueueIndex = -1;

        VkAllocationCallbacks *allocationCallbacks = nullptr;

        const char **instanceExtensions = nullptr;
        size_t numInstanceExtensions = 0;
        
        const char **deviceExtensions = nullptr;
        size_t numDeviceExtensions = 0;

        uint32_t maxTimerQueries = 256;

        // Indicates if VkPhysicalDeviceVulkan12Features::bufferDeviceAddress was set to 'true' at device creation time
        bool bufferDeviceAddressSupported = false;
        // Indicates if VkPhysicalDeviceVulkan12Features::descriptorBindingUniformBufferUpdateAfterBind was set to
        // 'true' at device creation time. Without it, ConstantBuffer entries of a bindless layout do not get
        // UPDATE_AFTER_BIND. The other descriptorBinding*UpdateAfterBind features are required by bindless layouts.
        bool descriptorBindingUniformBufferUpdateAfterBind = false;
        bool aftermathEnabled = false;
        bool logBufferLifetime = false;

        string vulkanLibraryName; // if empty, use default
    };

    // Creates a Vulkan device. Returns FS_OK and a new reference in *ppDevice, or an FE_* code and nullptr.
    NVRHI_C_API FRESULT nvrhiVulkanCreateDevice(const DeviceDesc* pDesc, IDevice** ppDevice) noexcept;

    NVRHI_C_API VkFormat nvrhiVulkanConvertFormat(nvrhi::Format format) noexcept;

    NVRHI_C_API const char* nvrhiVulkanResultToString(VkResult result) noexcept;

    inline DeviceHandle createDevice(const DeviceDesc& desc)
    {
        IDevice* device = nullptr;
        nvrhiVulkanCreateDevice(&desc, &device);
        return TakeOver(device);
    }

    inline VkFormat convertFormat(nvrhi::Format format) { return nvrhiVulkanConvertFormat(format); }

    inline const char* resultToString(VkResult result) { return nvrhiVulkanResultToString(result); }
}