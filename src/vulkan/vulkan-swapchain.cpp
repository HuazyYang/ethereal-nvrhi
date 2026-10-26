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

// The Vulkan swap chain (ADR 0014): a VkSwapchainKHR on a surface of the window, presented from the device's present
// queue. beginFrame() acquires an image and makes the graphics queue wait for it; present() signals a semaphore after
// the frame's work, presents, and limits the frames in flight with event queries. Ported from donut's
// DeviceManager_VK.

#include "vulkan-factory.h"

#include <nvrhi/common/misc.h>

#include <queue>

namespace nvrhi::vulkan
{
    NVRHI_CLASS_CLSID(SwapChain, "fe0aa8f8-b373-4024-9b66-56cdb44dcbaf")
    class SwapChain final : public ObjectImpl<IGraphicsSwapChain>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(SwapChain)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::vulkan::IGraphicsSwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IGraphicsSwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(SwapChain)
        NVRHI_END_INTERFACE_TABLE()

        SwapChain(GraphicsFactory* factory, const SwapChainDesc& desc, Device* device)
            : m_Factory(factory)
            , m_Desc(desc)
            , m_Device(device)
            , m_VkDevice(device->getContext().device)
        {
            m_Desc.format = GetSwapChainFormat(desc.format);
        }

        ~SwapChain()
        {
            destroySwapChain();

            while (!m_FramesInFlight.empty())
            {
                m_Device->waitEventQuery(m_FramesInFlight.front());
                m_FramesInFlight.pop();
            }

            for (vk::Semaphore semaphore : m_PresentSemaphores)
                m_VkDevice.destroySemaphore(semaphore);
            for (vk::Semaphore semaphore : m_AcquireSemaphores)
                m_VkDevice.destroySemaphore(semaphore);

            if (m_Surface)
                m_Factory->getInstance().destroySurfaceKHR(m_Surface);
        }

        bool initialize(std::string& outError)
        {
            m_Surface = m_Factory->createSurface(m_Desc.window, m_Desc.display, outError);
            if (!m_Surface)
                return false;

            m_PresentQueue = m_Device->getPresentQueue();
            if (!m_PresentQueue)
            {
                outError = "the device has no present queue: create it with createDeviceOnAdapter on a factory that is not headless";
                return false;
            }

            if (!createSwapChain(outError))
                return false;

            const size_t numPresentSemaphores = m_Images.size();
            for (size_t i = 0; i < numPresentSemaphores; ++i)
                m_PresentSemaphores.push_back(m_VkDevice.createSemaphore(vk::SemaphoreCreateInfo()));

            const size_t numAcquireSemaphores = std::max(size_t(m_Desc.maxFramesInFlight), m_Images.size());
            for (size_t i = 0; i < numAcquireSemaphores; ++i)
                m_AcquireSemaphores.push_back(m_VkDevice.createSemaphore(vk::SemaphoreCreateInfo()));

            return true;
        }

        // IRHIObject
        NativeObject getNativeObject(ObjectType objectType) noexcept override
        {
            switch (objectType)
            {
            case ObjectTypes::VK_SwapchainKHR:
                return static_cast<VkSwapchainKHR>(m_SwapChain);
            case ObjectTypes::VK_SurfaceKHR:
                return static_cast<VkSurfaceKHR>(m_Surface);
            default:
                return nullptr;
            }
        }

        // nvrhi::IGraphicsSwapChain
        const SwapChainDesc& getDesc() const noexcept override { return m_Desc; }
        uint32_t getBackBufferCount() noexcept override { return uint32_t(m_Images.size()); }

        ITexture* getBackBuffer(uint32_t index) noexcept override
        {
            return index < m_Images.size() ? m_Images[index].texture.Get() : nullptr;
        }

        uint32_t getCurrentBackBufferIndex() noexcept override { return m_ImageIndex; }

        FRESULT beginFrame() noexcept override
        {
            try
            {
                const vk::Semaphore semaphore = m_AcquireSemaphores[m_AcquireSemaphoreIndex];

                const vk::Result res = m_VkDevice.acquireNextImageKHR(
                    m_SwapChain,
                    std::numeric_limits<uint64_t>::max(), // timeout
                    semaphore,
                    vk::Fence(),
                    &m_ImageIndex);

                // Out of date: no image was acquired and the semaphore was not signaled; resize and try again.
                if (res == vk::Result::eErrorOutOfDateKHR)
                    return FE_OUT_OF_DATE;

                if (res != vk::Result::eSuccess && res != vk::Result::eSuboptimalKHR) // Suboptimal is considered a success
                    return FE_GENERIC_ERROR;

                m_AcquireSemaphoreIndex = (m_AcquireSemaphoreIndex + 1) % uint32_t(m_AcquireSemaphores.size());

                // Schedule the wait. The actual wait operation will be submitted when the app executes any command list.
                m_Device->queueWaitForSemaphore(CommandQueue::Graphics, semaphore, 0);
                return FS_OK;
            }
            catch (...)
            {
                return FE_GENERIC_ERROR;
            }
        }

        FRESULT present() noexcept override
        {
            try
            {
                const vk::Semaphore semaphore = m_PresentSemaphores[m_ImageIndex];

                m_Device->queueSignalSemaphore(CommandQueue::Graphics, semaphore, 0);

                // NVRHI buffers the semaphores and signals them when something is submitted to a queue.
                // Call 'executeCommandLists' with no command lists to actually signal the semaphore.
                m_Device->executeCommandLists(nullptr, 0);

                vk::PresentInfoKHR info = vk::PresentInfoKHR()
                    .setWaitSemaphoreCount(1)
                    .setPWaitSemaphores(&semaphore)
                    .setSwapchainCount(1)
                    .setPSwapchains(&m_SwapChain)
                    .setPImageIndices(&m_ImageIndex);

                const vk::Result res = m_PresentQueue.presentKHR(&info);
                if (!(res == vk::Result::eSuccess || res == vk::Result::eErrorOutOfDateKHR || res == vk::Result::eSuboptimalKHR))
                    return FE_GENERIC_ERROR;

#ifndef _WIN32
                if (m_Desc.vsync)
                {
                    // according to vulkan-tutorial.com, "the validation layer implementation expects
                    // the application to explicitly synchronize with the GPU"
                    m_PresentQueue.waitIdle();
                }
#endif

                while (m_FramesInFlight.size() >= m_Desc.maxFramesInFlight)
                {
                    EventQueryHandle query = m_FramesInFlight.front();
                    m_FramesInFlight.pop();
                    m_Device->waitEventQuery(query);
                    m_QueryPool.push_back(query);
                }

                EventQueryHandle query;
                if (!m_QueryPool.empty())
                {
                    query = m_QueryPool.back();
                    m_QueryPool.pop_back();
                }
                else
                {
                    m_Device->createEventQuery(&query);
                }

                m_Device->resetEventQuery(query);
                m_Device->setEventQuery(query, CommandQueue::Graphics);
                m_FramesInFlight.push(query);

                return res == vk::Result::eSuccess ? FS_OK : FE_OUT_OF_DATE;
            }
            catch (...)
            {
                return FE_GENERIC_ERROR;
            }
        }

        FRESULT resize(uint32_t width, uint32_t height) noexcept override
        {
            try
            {
                destroySwapChain();
                m_Desc.width = width;
                m_Desc.height = height;
                std::string errorText;
                if (!createSwapChain(errorText))
                {
                    m_Factory->reportValidationMessage(MessageSeverity::Error, errorText);
                    return FE_GENERIC_ERROR;
                }
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

        void setVsyncEnabled(bool enabled) noexcept override { m_Desc.vsync = enabled; }

        // vulkan::IGraphicsSwapChain
        VkSwapchainKHR getVkSwapchain() noexcept override { return m_SwapChain; }
        VkSurfaceKHR getVkSurface() noexcept override { return m_Surface; }

    private:
        struct Image
        {
            vk::Image image;
            TextureHandle texture;
        };

        AutoPtr<GraphicsFactory> m_Factory;   // owns the VkInstance of the surface
        SwapChainDesc m_Desc;
        AutoPtr<Device> m_Device;
        vk::Device m_VkDevice;
        vk::Queue m_PresentQueue;

        vk::SurfaceKHR m_Surface;
        vk::SwapchainKHR m_SwapChain;
        std::vector<Image> m_Images;
        uint32_t m_ImageIndex = 0;

        std::vector<vk::Semaphore> m_PresentSemaphores;
        std::vector<vk::Semaphore> m_AcquireSemaphores;
        uint32_t m_AcquireSemaphoreIndex = 0;

        std::queue<EventQueryHandle> m_FramesInFlight;
        std::vector<EventQueryHandle> m_QueryPool;

        void destroySwapChain()
        {
            if (m_VkDevice)
                m_VkDevice.waitIdle();

            m_Images.clear();

            if (m_SwapChain)
            {
                m_VkDevice.destroySwapchainKHR(m_SwapChain);
                m_SwapChain = nullptr;
            }
        }

        bool createSwapChain(std::string& outError)
        {
            const vk::PhysicalDevice physicalDevice = m_Device->getContext().physicalDevice;
            const vk::SurfaceCapabilitiesKHR surfaceCaps = physicalDevice.getSurfaceCapabilitiesKHR(m_Surface);

            // 0 takes the size of the surface.
            if (m_Desc.width == 0 || m_Desc.height == 0)
            {
                m_Desc.width = surfaceCaps.currentExtent.width;
                m_Desc.height = surfaceCaps.currentExtent.height;
            }

            const vk::SurfaceFormatKHR swapChainFormat = {
                vk::Format(convertFormat(m_Desc.format)),
                vk::ColorSpaceKHR::eSrgbNonlinear
            };

            const vk::Extent2D extent(m_Desc.width, m_Desc.height);

            std::vector<uint32_t> queues = { uint32_t(m_Device->getQueue(CommandQueue::Graphics)->getQueueFamilyIndex()) };
            if (uint32_t(m_Device->getPresentQueueFamily()) != queues[0])
                queues.push_back(uint32_t(m_Device->getPresentQueueFamily()));

            const bool enableSwapChainSharing = queues.size() > 1;
            const bool mutableFormatSupported = m_Device->isDeviceExtensionEnabled(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);

            auto desc = vk::SwapchainCreateInfoKHR()
                .setSurface(m_Surface)
                .setMinImageCount(m_Desc.bufferCount)
                .setImageFormat(swapChainFormat.format)
                .setImageColorSpace(swapChainFormat.colorSpace)
                .setImageExtent(extent)
                .setImageArrayLayers(1)
                .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled)
                .setImageSharingMode(enableSwapChainSharing ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive)
                .setFlags(mutableFormatSupported ? vk::SwapchainCreateFlagBitsKHR::eMutableFormat : vk::SwapchainCreateFlagBitsKHR(0))
                .setQueueFamilyIndexCount(enableSwapChainSharing ? uint32_t(queues.size()) : 0)
                .setPQueueFamilyIndices(enableSwapChainSharing ? queues.data() : nullptr)
                .setPreTransform(vk::SurfaceTransformFlagBitsKHR::eIdentity)
                .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
                .setPresentMode(m_Desc.vsync ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate)
                .setClipped(true)
                .setOldSwapchain(nullptr);

            std::vector<vk::Format> imageFormats = { swapChainFormat.format };
            switch (swapChainFormat.format)
            {
            case vk::Format::eR8G8B8A8Unorm:
                imageFormats.push_back(vk::Format::eR8G8B8A8Srgb);
                break;
            case vk::Format::eR8G8B8A8Srgb:
                imageFormats.push_back(vk::Format::eR8G8B8A8Unorm);
                break;
            case vk::Format::eB8G8R8A8Unorm:
                imageFormats.push_back(vk::Format::eB8G8R8A8Srgb);
                break;
            case vk::Format::eB8G8R8A8Srgb:
                imageFormats.push_back(vk::Format::eB8G8R8A8Unorm);
                break;
            default:
                break;
            }

            auto imageFormatListCreateInfo = vk::ImageFormatListCreateInfo()
                .setViewFormats(imageFormats);

            if (mutableFormatSupported)
                desc.pNext = &imageFormatListCreateInfo;

            const vk::Result res = m_VkDevice.createSwapchainKHR(&desc, nullptr, &m_SwapChain);
            if (res != vk::Result::eSuccess)
            {
                outError = std::string("Failed to create a Vulkan swap chain, error code = ") + resultToString(VkResult(res));
                return false;
            }

            // retrieve swap chain images
            for (vk::Image image : m_VkDevice.getSwapchainImagesKHR(m_SwapChain))
            {
                Image entry;
                entry.image = image;

                TextureDesc textureDesc;
                textureDesc.width = m_Desc.width;
                textureDesc.height = m_Desc.height;
                textureDesc.format = m_Desc.format;
                textureDesc.debugName = "Swap chain image";
                textureDesc.initialState = ResourceStates::Present;
                textureDesc.keepInitialState = true;
                textureDesc.isRenderTarget = true;

                m_Device->createHandleForNativeTexture(ObjectTypes::VK_Image, static_cast<VkImage>(image), textureDesc, &entry.texture);
                m_Images.push_back(entry);
            }

            m_ImageIndex = 0;
            return true;
        }
    };

    FRESULT CreateSwapChain(GraphicsFactory* factory, const SwapChainDesc& desc, nvrhi::IDevice* device,
        IGraphicsSwapChain** ppSwapChain)
    {
        // The device may be the validation layer around a Vulkan device: the swap chain uses the Vulkan device.
        auto vulkanDevice = static_cast<vulkan::IDevice*>(device->getNativeObject(ObjectTypes::Nvrhi_VK_Device));
        Device* backendDevice = vulkanDevice ? checked_cast<Device*>(vulkanDevice) : nullptr;
        if (!backendDevice)
            return FE_INVALID_ARGS;

        AutoPtr<SwapChain> swapChain = MAKE_RC_OBJ_PTR(SwapChain, factory, desc, backendDevice);
        std::string errorText;
        if (!swapChain->initialize(errorText))
        {
            factory->reportValidationMessage(MessageSeverity::Error, errorText);
            return FE_GENERIC_ERROR;
        }
        *ppSwapChain = swapChain.Detach();
        return FS_OK;
    }
}
