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

// Interop with Vulkan (ADR 0013).
//
// - Memory: NVRHI allocates the memory of a shared buffer or image as a dedicated, exportable allocation and keeps
//   its OPAQUE_WIN32 handle (or OPAQUE_FD) as getNativeObject(SharedHandle). It is imported with the DEDICATED flag
//   and the size of the memory requirements. A file descriptor passes to CUDA on import, so a duplicate is
//   imported; a Win32 handle stays NVRHI's.
// - Timelines: exportable timeline semaphores, imported as TIMELINE_SEMAPHORE_WIN32 (or _FD). The graphics queue
//   signals with an empty vkQueueSubmit, which is ordered after the work submitted before it; it waits through
//   vulkan::IDevice::queueWaitForSemaphore, which makes the next NVRHI submission wait.
// The device needs VK_KHR_external_memory_win32 / _fd and VK_KHR_external_semaphore_win32 / _fd.
//
// The few Vulkan functions are loaded through the loader that the application has loaded.

#include "cuda-backend.h"

#if NVRHI_CUDA_WITH_VULKAN

#include <nvrhi/vulkan.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <cstring>

namespace nvrhi::cuda
{
    namespace
    {
        PFN_vkGetInstanceProcAddr GetLoaderEntryPoint()
        {
#ifdef _WIN32
            HMODULE loader = GetModuleHandleA("vulkan-1.dll");
            if (!loader)
                loader = LoadLibraryA("vulkan-1.dll");
            return loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr")) : nullptr;
#else
            void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
            if (!loader)
                loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
            return loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr")) : nullptr;
#endif
        }

        struct VulkanFunctions
        {
            PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2 = nullptr;
            PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
            PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
            PFN_vkCreateSemaphore createSemaphore = nullptr;
            PFN_vkDestroySemaphore destroySemaphore = nullptr;
            PFN_vkQueueSubmit queueSubmit = nullptr;
#ifdef _WIN32
            PFN_vkGetSemaphoreWin32HandleKHR getSemaphoreWin32Handle = nullptr;
#else
            PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;
#endif

            bool load(VkInstance instance, VkDevice device)
            {
                PFN_vkGetInstanceProcAddr getInstanceProcAddr = GetLoaderEntryPoint();
                if (!getInstanceProcAddr)
                    return false;
                auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
                if (!getDeviceProcAddr)
                    return false;

                getPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                    getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
                getBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(getDeviceProcAddr(device, "vkGetBufferMemoryRequirements"));
                getImageMemoryRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(getDeviceProcAddr(device, "vkGetImageMemoryRequirements"));
                createSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(getDeviceProcAddr(device, "vkCreateSemaphore"));
                destroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(getDeviceProcAddr(device, "vkDestroySemaphore"));
                queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(getDeviceProcAddr(device, "vkQueueSubmit"));
#ifdef _WIN32
                // Null when VK_KHR_external_semaphore_win32 is not enabled.
                getSemaphoreWin32Handle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(getDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR"));
                return getBufferMemoryRequirements && getImageMemoryRequirements && createSemaphore && destroySemaphore &&
                    queueSubmit && getSemaphoreWin32Handle;
#else
                getSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(getDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
                return getBufferMemoryRequirements && getImageMemoryRequirements && createSemaphore && destroySemaphore &&
                    queueSubmit && getSemaphoreFd;
#endif
            }
        };

        class VulkanInteropApi final : public InteropDevice::Api
        {
        public:
            VulkanInteropApi(const Context& context, nvrhi::IDevice* graphicsDevice, nvrhi::vulkan::IDevice* vulkanDevice,
                VkDevice device, const VulkanFunctions& functions)
                : m_Context(context)
                , m_GraphicsDevice(graphicsDevice)
                , m_VulkanDevice(vulkanDevice)
                , m_Device(device)
                , m_Vk(functions)
            {
            }

            ~VulkanInteropApi() override
            {
                for (Timeline& timeline : m_GraphicsTimelines)
                    destroy(timeline);
                for (Timeline& timeline : m_ComputeTimelines)
                    destroy(timeline);
            }

            bool getBufferMemory(IBuffer* buffer, CUDA_EXTERNAL_MEMORY_HANDLE_DESC& out, std::string& error) override
            {
                VkBuffer vkBuffer = VkBuffer(buffer->getNativeObject(ObjectTypes::VK_Buffer));
                if (!vkBuffer)
                {
                    error = "the buffer is not a Vulkan buffer";
                    return false;
                }
                VkMemoryRequirements requirements = {};
                m_Vk.getBufferMemoryRequirements(m_Device, vkBuffer, &requirements);
                return getMemory(buffer, requirements.size, out, error);
            }

            bool getTextureMemory(ITexture* texture, CUDA_EXTERNAL_MEMORY_HANDLE_DESC& out, std::string& error) override
            {
                VkImage image = VkImage(texture->getNativeObject(ObjectTypes::VK_Image));
                if (!image)
                {
                    error = "the texture is not a Vulkan image";
                    return false;
                }
                VkMemoryRequirements requirements = {};
                m_Vk.getImageMemoryRequirements(m_Device, image, &requirements);
                return getMemory(texture, requirements.size, out, error);
            }

            bool canImportTexture(const TextureDesc& desc, std::string& reason) override
            {
                // The NVIDIA Vulkan driver pads the array layers of a mipmapped image beyond what a layered CUDA
                // array expects: from the second layer on, CUDA would read the wrong memory. Single-level arrays and
                // cube maps, 2D and 3D textures with mip chains map exactly (tests/cuda-tests.cpp, interop-vulkan).
                const bool layered = desc.dimension == TextureDimension::Texture1DArray ||
                    desc.dimension == TextureDimension::Texture2DArray || desc.dimension == TextureDimension::TextureCube ||
                    desc.dimension == TextureDimension::TextureCubeArray;
                if (layered && desc.mipLevels > 1)
                {
                    reason = "Vulkan array and cube textures with more than one mip level cannot be mapped into CUDA";
                    return false;
                }
                return true;
            }

            CUexternalSemaphore getGraphicsTimeline(CommandQueue queue) override
            {
                return getTimeline(m_GraphicsTimelines[uint32_t(queue)]);
            }

            CUexternalSemaphore getComputeTimeline(CommandQueue queue) override
            {
                return getTimeline(m_ComputeTimelines[uint32_t(queue)]);
            }

            bool graphicsSignal(CommandQueue queue, uint64_t value) override
            {
                VkQueue vkQueue = VkQueue(m_GraphicsDevice->getNativeQueue(ObjectTypes::VK_Queue, queue));
                Timeline& timeline = m_GraphicsTimelines[uint32_t(queue)];
                if (!vkQueue || !timeline.semaphore)
                    return false;

                VkTimelineSemaphoreSubmitInfo timelineInfo = {};
                timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
                timelineInfo.signalSemaphoreValueCount = 1;
                timelineInfo.pSignalSemaphoreValues = &value;
                VkSubmitInfo submit = {};
                submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit.pNext = &timelineInfo;
                submit.signalSemaphoreCount = 1;
                submit.pSignalSemaphores = &timeline.semaphore;
                return m_Vk.queueSubmit(vkQueue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
            }

            bool graphicsWait(CommandQueue queue, CommandQueue computeQueue, uint64_t value) override
            {
                Timeline& timeline = m_ComputeTimelines[uint32_t(computeQueue)];
                if (!timeline.semaphore)
                    return false;
                m_VulkanDevice->queueWaitForSemaphore(queue, timeline.semaphore, value);
                return true;
            }

        private:
            struct Timeline
            {
                VkSemaphore semaphore = VK_NULL_HANDLE;
                CUexternalSemaphore imported = nullptr;
            };

            const Context& m_Context;
            nvrhi::IDevice* m_GraphicsDevice;           // the interop device keeps the reference
            nvrhi::vulkan::IDevice* m_VulkanDevice;     // the same device, or the one under the validation layer
            VkDevice m_Device;
            VulkanFunctions m_Vk;
            std::array<Timeline, c_NumQueues> m_GraphicsTimelines;
            std::array<Timeline, c_NumQueues> m_ComputeTimelines;

            bool getMemory(IRHIObject* object, VkDeviceSize size, CUDA_EXTERNAL_MEMORY_HANDLE_DESC& out, std::string& error)
            {
                void* handle = object->getNativeObject(ObjectTypes::SharedHandle);
                out = CUDA_EXTERNAL_MEMORY_HANDLE_DESC();
#ifdef _WIN32
                if (!handle)
                {
                    error = "the resource has no shared memory handle";
                    return false;
                }
                out.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
                out.handle.win32.handle = handle;
#else
                const int fd = int(intptr_t(handle));
                if (fd <= 0)
                {
                    error = "the resource has no shared memory file descriptor";
                    return false;
                }
                out.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
                out.handle.fd = dup(fd);     // CUDA takes ownership of the descriptor it imports
#endif
                out.size = size;
                out.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;     // NVRHI allocates shared memory as dedicated
                return true;
            }

            CUexternalSemaphore getTimeline(Timeline& timeline)
            {
                if (timeline.imported)
                    return timeline.imported;

                VkSemaphoreTypeCreateInfo typeInfo = {};
                typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
                typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
                typeInfo.initialValue = 0;
                VkExportSemaphoreCreateInfo exportInfo = {};
                exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
                exportInfo.pNext = &typeInfo;
#ifdef _WIN32
                exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
                exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
                VkSemaphoreCreateInfo createInfo = {};
                createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                createInfo.pNext = &exportInfo;
                if (m_Vk.createSemaphore(m_Device, &createInfo, nullptr, &timeline.semaphore) != VK_SUCCESS)
                {
                    m_Context.error("cannot create an exportable Vulkan timeline semaphore");
                    return nullptr;
                }

                CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC desc = {};
#ifdef _WIN32
                VkSemaphoreGetWin32HandleInfoKHR handleInfo = {};
                handleInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
                handleInfo.semaphore = timeline.semaphore;
                handleInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
                HANDLE handle = nullptr;
                if (m_Vk.getSemaphoreWin32Handle(m_Device, &handleInfo, &handle) != VK_SUCCESS)
                {
                    m_Context.error("vkGetSemaphoreWin32HandleKHR failed");
                    destroy(timeline);
                    return nullptr;
                }
                desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_WIN32;
                desc.handle.win32.handle = handle;
#else
                VkSemaphoreGetFdInfoKHR fdInfo = {};
                fdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
                fdInfo.semaphore = timeline.semaphore;
                fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
                int fd = -1;
                if (m_Vk.getSemaphoreFd(m_Device, &fdInfo, &fd) != VK_SUCCESS)
                {
                    m_Context.error("vkGetSemaphoreFdKHR failed");
                    destroy(timeline);
                    return nullptr;
                }
                desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_FD;
                desc.handle.fd = fd;
#endif

                ContextScope scope(m_Context);
                const bool imported = NVRHI_CUDA_CHECK(m_Context, cuImportExternalSemaphore(&timeline.imported, &desc));
#ifdef _WIN32
                // The handle stays the application's: CUDA keeps its own reference.
                CloseHandle(handle);
#else
                if (!imported)
                    close(fd);   // passed to CUDA on success
#endif
                if (!imported)
                {
                    timeline.imported = nullptr;
                    destroy(timeline);
                    return nullptr;
                }
                return timeline.imported;
            }

            void destroy(Timeline& timeline)
            {
                if (timeline.imported)
                    g_Cuda.cuDestroyExternalSemaphore(timeline.imported);
                if (timeline.semaphore)
                    m_Vk.destroySemaphore(m_Device, timeline.semaphore, nullptr);
                timeline = Timeline();
            }
        };
    }

    MonoPtr<InteropDevice::Api> CreateVulkanInteropApi(const Context& context, nvrhi::IDevice* graphicsDevice, std::string& error)
    {
        VkInstance instance = VkInstance(graphicsDevice->getNativeObject(ObjectTypes::VK_Instance));
        VkDevice device = VkDevice(graphicsDevice->getNativeObject(ObjectTypes::VK_Device));
        // The NVRHI device itself, also through the validation layer (which answers only the generic interfaces).
        auto vulkanDevice = static_cast<nvrhi::vulkan::IDevice*>(graphicsDevice->getNativeObject(ObjectTypes::Nvrhi_VK_Device));
        if (!instance || !device || !vulkanDevice)
        {
            error = "the graphics device is not an NVRHI Vulkan device";
            return nullptr;
        }

        VulkanFunctions functions;
        if (!functions.load(instance, device))
        {
            error = "the Vulkan device lacks the external semaphore extension, or the loader is not found";
            return nullptr;
        }
        return MakeMono<VulkanInteropApi>(context, graphicsDevice, vulkanDevice, device, functions);
    }

    bool GetVulkanAdapterIdentity(nvrhi::IDevice* graphicsDevice, AdapterIdentity& outIdentity)
    {
        VkInstance instance = VkInstance(graphicsDevice->getNativeObject(ObjectTypes::VK_Instance));
        VkPhysicalDevice physicalDevice = VkPhysicalDevice(graphicsDevice->getNativeObject(ObjectTypes::VK_PhysicalDevice));
        PFN_vkGetInstanceProcAddr getInstanceProcAddr = GetLoaderEntryPoint();
        if (!instance || !physicalDevice || !getInstanceProcAddr)
            return false;
        auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
        if (!getProperties2)
            return false;

        VkPhysicalDeviceIDProperties idProperties = {};
        idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 properties = {};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &idProperties;
        getProperties2(physicalDevice, &properties);

        static_assert(sizeof(idProperties.deviceUUID) == sizeof(outIdentity.uuid));
        memcpy(outIdentity.uuid, idProperties.deviceUUID, sizeof(outIdentity.uuid));
        outIdentity.hasUuid = true;
        if (idProperties.deviceLUIDValid)
        {
            memcpy(outIdentity.luid, idProperties.deviceLUID, sizeof(outIdentity.luid));
            outIdentity.hasLuid = true;
        }
        return true;
    }
}

#endif // NVRHI_CUDA_WITH_VULKAN
