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

// The Vulkan graphics factory and swap chain (ADR 0014). Module-private.

#include "vulkan-backend.h"
#include "../common/graphics-factory-base.h"

#include <memory>
#include <unordered_set>

namespace nvrhi::vulkan
{
#if VK_HEADER_VERSION >= 301
    typedef vk::detail::DynamicLoader VulkanDynamicLoader;
#else
    typedef vk::DynamicLoader VulkanDynamicLoader;
#endif

    NVRHI_CLASS_CLSID(GraphicsFactory, "cc520c40-8004-4913-8b1f-051fd1e7dee1")
    class GraphicsFactory final : public GraphicsFactoryBase<IGraphicsFactory>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(GraphicsFactory)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GraphicsFactory)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::vulkan::IGraphicsFactory)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IGraphicsFactory)
        NVRHI_IMPLEMENTS_CLASS(GraphicsFactory)
        NVRHI_END_INTERFACE_TABLE()

        explicit GraphicsFactory(const GraphicsFactoryDesc& desc);
        ~GraphicsFactory();
        bool initialize(std::string& outError);

        // nvrhi::IGraphicsFactory
        GraphicsAPI getGraphicsAPI() noexcept override { return GraphicsAPI::VULKAN; }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        // vulkan::IGraphicsFactory
        FRESULT createDevice(const DeviceDesc& desc, IDevice** ppDevice) noexcept override;
        FRESULT createDeviceOnAdapter(const DeviceCreationDesc& desc, IDevice** ppDevice) noexcept override;
        FRESULT createSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device,
            IGraphicsSwapChain** ppSwapChain) noexcept override;
        uint32_t getEnabledInstanceExtensions(const char** names, uint32_t capacity) noexcept override;
        uint32_t getEnabledLayers(const char** names, uint32_t capacity) noexcept override;

        // Internal
        [[nodiscard]] vk::Instance getInstance() const { return m_Instance; }
        [[nodiscard]] bool isIgnoredValidationMessage(uint64_t location) const;
        void reportValidationMessage(MessageSeverity severity, const std::string& text) const { message(severity, text); }

        // A surface on 'window' (SwapChainDesc::window / display). Returns a null surface and an error message on
        // failure.
        vk::SurfaceKHR createSurface(NativeObject window, NativeObject display, std::string& outError) const;

    private:
        struct QueueFamilies
        {
            int graphics = -1;
            int compute = -1;
            int transfer = -1;
            int present = -1;
        };

        GraphicsFactoryDesc m_Desc;
        std::unique_ptr<VulkanDynamicLoader> m_Loader;
        PFN_vkGetInstanceProcAddr m_GetInstanceProcAddr = nullptr;

        vk::Instance m_Instance;
        vk::DebugReportCallbackEXT m_DebugReportCallback;
        vk::DebugUtilsMessengerEXT m_DebugUtilsMessenger;

        std::vector<std::string> m_EnabledInstanceExtensions;
        std::vector<std::string> m_EnabledLayers;
        std::vector<vk::PhysicalDevice> m_PhysicalDevices;

        bool createInstance(std::string& outError);
        void installDebugCallbacks();
        void enumeratePhysicalDevices();
        bool isInstanceExtensionEnabled(const char* name) const;

        bool findQueueFamilies(vk::PhysicalDevice physicalDevice, vk::SurfaceKHR surface,
            const DeviceCreationDesc& desc, QueueFamilies& outFamilies) const;
        bool pickPhysicalDevice(const DeviceCreationDesc& desc, const std::unordered_set<std::string>& requiredExtensions,
            vk::SurfaceKHR surface, vk::PhysicalDevice& outDevice, QueueFamilies& outFamilies) const;
    };

    // vulkan-swapchain.cpp
    FRESULT CreateSwapChain(GraphicsFactory* factory, const SwapChainDesc& desc, nvrhi::IDevice* device,
        IGraphicsSwapChain** ppSwapChain);

    // The format a Vulkan swap chain uses for an NVRHI format: surfaces offer BGRA8, not RGBA8.
    Format GetSwapChainFormat(Format format);
}
