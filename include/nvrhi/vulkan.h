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
#include <nvrhi/factory.h>
#include <nvrhi/swapchain.h>

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
        // The device extensions that are enabled. Writes up to 'capacity' names (owned by the device) to 'names' and
        // returns the total count; names may be null to query the count.
        virtual uint32_t getEnabledExtensions(_Out_writes_opt_(capacity) const char** names, uint32_t capacity) noexcept = 0;
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

    // DeviceDesc wraps a native device that the caller created (IGraphicsFactory::createDevice).

    // Lets the application change the VkDeviceCreateInfo that createDeviceOnAdapter is about to pass to
    // vkCreateDevice: extra features, pNext structures. Implemented by the application.
    NVRHI_IID(IDeviceCreateCallback, "9df0ed5e-2862-4533-9724-80ed0d4c5a61")
    struct IDeviceCreateCallback : IObject
    {
        NVRHI_DECLARE_UUID_TRAITS(IDeviceCreateCallback)
        virtual void onCreateDevice(VkDeviceCreateInfo& info) noexcept = 0;
    };

    // Physical devices that cannot do this many multiview views and instances are not selected.
    struct MultiviewRequirements
    {
        bool enabled = false;
        uint32_t maxMultiviewViewCount = 15;
        uint32_t maxMultiviewInstanceIndex = 6;
    };

    static_assert(sizeof(MultiviewRequirements) == 12);

    // The swap chain that the device must be able to present: physical devices that cannot create it on a surface
    // of 'window' are not selected. Ignored if window is null.
    struct SurfaceRequirements
    {
        NativeObject window = nullptr;      // as SwapChainDesc::window
        NativeObject display = nullptr;     // as SwapChainDesc::display
        Format format = Format::SRGBA8_UNORM;
        uint32_t bufferCount = 3;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    static_assert(sizeof(SurfaceRequirements) == 32);

    // Creates the native device too (IGraphicsFactory::createDeviceOnAdapter): the factory selects the physical
    // device on its VkInstance, creates the VkDevice with the features NVRHI uses and gets its queues. The other
    // fields mean what they mean in DeviceDesc.
    struct DeviceCreationDesc
    {
        IMessageCallback* errorCB = nullptr; // the device keeps a reference to it

        // The adapter index (IGraphicsFactory::getAdapterInfo); -1 selects the first suitable discrete GPU, or else
        // the first suitable other GPU.
        int32_t adapterIndex = -1;

        // A device without a required extension is not selected; optional ones are enabled when present.
        vector<string> requiredExtensions;
        vector<string> optionalExtensions;

        // The graphics queue is always created, and a present queue unless the factory is headless.
        bool enableComputeQueue = false;
        bool enableCopyQueue = false;
        // Enables the ray tracing extensions (acceleration structures, pipelines, ray queries, ...) that are present.
        bool enableRayTracingExtensions = false;
        // Needs VK_EXT_mutable_descriptor_type (made required) and implies descriptorBindingUniformBufferUpdateAfterBind.
        bool enableHeapDirectlyIndexed = false;
        // Needs descriptorBindingUniformBufferUpdateAfterBind, which narrows the set of usable GPUs.
        bool enableCbvDescriptorStreaming = false;
        // Enables VK_NV_ray_tracing_validation if the driver exposes it (the factory needs
        // GraphicsFactoryDesc::enableRayTracingValidation).
        bool enableRayTracingValidation = false;
        bool aftermathEnabled = false;
        bool logBufferLifetime = false;

        MultiviewRequirements multiview;
        uint32_t maxTimerQueries = 256;

        // Appended at the end of the vkGetPhysicalDeviceFeatures2 chain that selects the device features; must stay
        // alive during createDeviceOnAdapter. Its fields are filled before deviceCreateCallback is called.
        void* physicalDeviceFeatures2Extensions = nullptr;
        // Called before vkCreateDevice. Optional; not kept after createDeviceOnAdapter returns.
        IDeviceCreateCallback* deviceCreateCallback = nullptr;

        SurfaceRequirements surface;

        DeviceCreationDesc& setErrorCallback(IMessageCallback* value) { errorCB = value; return *this; }
        DeviceCreationDesc& setAdapterIndex(int32_t value) { adapterIndex = value; return *this; }
        DeviceCreationDesc& addRequiredExtension(const char* name) { requiredExtensions.push_back(string(name)); return *this; }
        DeviceCreationDesc& addOptionalExtension(const char* name) { optionalExtensions.push_back(string(name)); return *this; }
        DeviceCreationDesc& setEnableComputeQueue(bool value) { enableComputeQueue = value; return *this; }
        DeviceCreationDesc& setEnableCopyQueue(bool value) { enableCopyQueue = value; return *this; }
        DeviceCreationDesc& setEnableRayTracingExtensions(bool value) { enableRayTracingExtensions = value; return *this; }
        DeviceCreationDesc& setEnableHeapDirectlyIndexed(bool value) { enableHeapDirectlyIndexed = value; return *this; }
        DeviceCreationDesc& setEnableCbvDescriptorStreaming(bool value) { enableCbvDescriptorStreaming = value; return *this; }
        DeviceCreationDesc& setEnableRayTracingValidation(bool value) { enableRayTracingValidation = value; return *this; }
        DeviceCreationDesc& setAftermathEnabled(bool value) { aftermathEnabled = value; return *this; }
        DeviceCreationDesc& setLogBufferLifetime(bool value) { logBufferLifetime = value; return *this; }
        DeviceCreationDesc& setDeviceCreateCallback(IDeviceCreateCallback* value) { deviceCreateCallback = value; return *this; }
    };

    static_assert(offsetof(DeviceCreationDesc, requiredExtensions) == 16);
    static_assert(offsetof(DeviceCreationDesc, enableComputeQueue) == 16 + 2 * sizeof(vector<string>));
    static_assert(offsetof(DeviceCreationDesc, multiview) == 16 + 2 * sizeof(vector<string>) + 8);
    static_assert(offsetof(DeviceCreationDesc, physicalDeviceFeatures2Extensions) == 16 + 2 * sizeof(vector<string>) + 24);
    static_assert(sizeof(DeviceCreationDesc) == 16 + 2 * sizeof(vector<string>) + 40 + sizeof(SurfaceRequirements));

    struct GraphicsFactoryDesc
    {
        // Receives the factory's own messages and the validation-layer messages. Optional; the factory keeps a
        // reference to it.
        IMessageCallback* messageCallback = nullptr;

        // Enables VK_LAYER_KHRONOS_validation and VK_EXT_debug_report.
        bool enableDebugRuntime = false;
        // Sets NV_ALLOW_RAYTRACING_VALIDATION=1 before the driver loads and enables VK_EXT_debug_utils, which
        // delivers the ray tracing validation messages (see DeviceCreationDesc::enableRayTracingValidation).
        bool enableRayTracingValidation = false;
        // No surface extensions and no present queues: the factory cannot create swap chains.
        bool headless = false;

        // The Vulkan library; empty for vulkan-1.dll / libvulkan.so.1. Streamline passes sl.interposer.dll.
        string vulkanLibraryName;

        // The instance fails to create without a required extension or layer; optional ones are enabled when
        // present. The surface extensions of the platform are added unless headless.
        vector<string> requiredInstanceExtensions;
        vector<string> optionalInstanceExtensions;
        vector<string> requiredLayers;
        vector<string> optionalLayers;

        // Validation messages with these locations (VkDebugReportCallbackEXT 'location') are not reported.
        vector<uint64_t> ignoredValidationMessageLocations;

        GraphicsFactoryDesc& setMessageCallback(IMessageCallback* value) { messageCallback = value; return *this; }
        GraphicsFactoryDesc& setEnableDebugRuntime(bool value) { enableDebugRuntime = value; return *this; }
        GraphicsFactoryDesc& setEnableRayTracingValidation(bool value) { enableRayTracingValidation = value; return *this; }
        GraphicsFactoryDesc& setHeadless(bool value) { headless = value; return *this; }
    };

    static_assert(offsetof(GraphicsFactoryDesc, vulkanLibraryName) == 16);
    static_assert(offsetof(GraphicsFactoryDesc, requiredInstanceExtensions) == 16 + sizeof(string));
    static_assert(sizeof(GraphicsFactoryDesc) == 16 + sizeof(string) + 4 * sizeof(vector<string>) + sizeof(vector<uint64_t>));

    // The Vulkan swap chain: a VkSwapchainKHR on a surface of the window, presented from the device's present
    // queue. getNativeObject returns ObjectTypes::VK_SwapchainKHR and ObjectTypes::VK_SurfaceKHR.
    NVRHI_IID(IGraphicsSwapChain, "c3e11054-d5ff-4caf-96cd-6bc8f012dacd")
    struct IGraphicsSwapChain : nvrhi::IGraphicsSwapChain
    {
        NVRHI_DECLARE_UUID_TRAITS(IGraphicsSwapChain)
        [[nodiscard]] virtual VkSwapchainKHR getVkSwapchain() noexcept = 0;
        [[nodiscard]] virtual VkSurfaceKHR getVkSurface() noexcept = 0;
    };

    typedef AutoPtr<IGraphicsSwapChain> GraphicsSwapChainHandle;

    NVRHI_IID(IGraphicsFactory, "b89c4f15-1827-4da7-922e-3039f62edf86")
    struct IGraphicsFactory : nvrhi::IGraphicsFactory
    {
        NVRHI_DECLARE_UUID_TRAITS(IGraphicsFactory)

        // Each returns FS_OK and a new reference in the last parameter, or an FE_* code and nullptr.

        // Wraps the native instance, device and queues of desc.
        virtual FRESULT createDevice(const DeviceDesc& desc, IDevice** ppDevice) noexcept = 0;
        // Selects the physical device and creates the VkDevice and its queues on the factory's VkInstance; the
        // NVRHI device owns them.
        virtual FRESULT createDeviceOnAdapter(const DeviceCreationDesc& desc, IDevice** ppDevice) noexcept = 0;
        // A swap chain for 'device' (a Vulkan device created by createDeviceOnAdapter, or the validation layer
        // around one) on desc.window. FE_UNSUPPORTED for a headless factory.
        virtual FRESULT createSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device,
            IGraphicsSwapChain** ppSwapChain) noexcept = 0;

        // The instance extensions and layers that are enabled. Each writes up to 'capacity' names (owned by the
        // factory) to 'names' and returns the total count; names may be null to query the count.
        virtual uint32_t getEnabledInstanceExtensions(_Out_writes_opt_(capacity) const char** names, uint32_t capacity) noexcept = 0;
        virtual uint32_t getEnabledLayers(_Out_writes_opt_(capacity) const char** names, uint32_t capacity) noexcept = 0;
    };

    typedef AutoPtr<IGraphicsFactory> GraphicsFactoryHandle;

    // Creates the Vulkan graphics factory: loads the Vulkan library, creates the VkInstance (Vulkan 1.3 or newer),
    // initializes the Vulkan-Hpp default dispatcher of NVRHI and enumerates the physical devices. Returns FS_OK and
    // a new reference in *ppFactory, or an FE_* code (FE_UNSUPPORTED if the library does not load or the instance
    // cannot be created) and nullptr.
    NVRHI_C_API FRESULT nvrhiVulkanCreateGraphicsFactory(const GraphicsFactoryDesc* pDesc,
        IGraphicsFactory** ppFactory) noexcept;

    inline GraphicsFactoryHandle createGraphicsFactory(const GraphicsFactoryDesc& desc)
    {
        IGraphicsFactory* factory = nullptr;
        nvrhiVulkanCreateGraphicsFactory(&desc, &factory);
        return TakeOver(factory);
    }

    NVRHI_C_API VkFormat nvrhiVulkanConvertFormat(nvrhi::Format format) noexcept;

    NVRHI_C_API const char* nvrhiVulkanResultToString(VkResult result) noexcept;

    inline VkFormat convertFormat(nvrhi::Format format) { return nvrhiVulkanConvertFormat(format); }

    inline const char* resultToString(VkResult result) { return nvrhiVulkanResultToString(result); }
}