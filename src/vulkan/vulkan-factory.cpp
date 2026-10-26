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

// The Vulkan graphics factory (ADR 0014): the Vulkan library loaded at run time, the VkInstance with its debug
// callbacks, the physical devices, and the VkDevice with the features NVRHI uses. Ported from donut's
// DeviceManager_VK.

#include "vulkan-factory.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace nvrhi::vulkan
{
    namespace
    {
        constexpr uint32_t kMinimumVulkanVersion = VK_API_VERSION_1_3;
        constexpr uint32_t kQueueIndex = 0;

        std::vector<const char*> ToPointers(const std::vector<std::string>& strings)
        {
            std::vector<const char*> pointers;
            pointers.reserve(strings.size());
            for (const std::string& s : strings)
                pointers.push_back(s.c_str());
            return pointers;
        }

        template <typename T>
        std::vector<T> SetToVector(const std::unordered_set<T>& set)
        {
            return std::vector<T>(set.begin(), set.end());
        }

        uint32_t CopyNames(const std::vector<std::string>& source, const char** names, uint32_t capacity)
        {
            const uint32_t count = uint32_t(source.size());
            if (names)
            {
                for (uint32_t i = 0; i < count && i < capacity; i++)
                    names[i] = source[i].c_str();
            }
            return count;
        }

        // The device extensions that are enabled when present.
        const char* const kDefaultOptionalDeviceExtensions[] = {
            VK_EXT_DEBUG_MARKER_EXTENSION_NAME,
            VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
            VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME,
            VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME,
            VK_EXT_MESH_SHADER_EXTENSION_NAME,
            VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
#if NVRHI_WITH_AFTERMATH
            VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME,
            VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME,
#endif
        };

        // DeviceCreationDesc::enableRayTracingExtensions
        const char* const kRayTracingExtensions[] = {
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
            VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME,
            VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME
        };

        VKAPI_ATTR VkBool32 VKAPI_CALL DebugReportCallback(
            vk::DebugReportFlagsEXT flags,
            vk::DebugReportObjectTypeEXT objType,
            uint64_t obj,
            size_t location,
            int32_t code,
            const char* layerPrefix,
            const char* msg,
            void* userData)
        {
            (void)flags;
            (void)objType;
            (void)obj;
            const auto factory = static_cast<const GraphicsFactory*>(userData);
            if (!factory || factory->isIgnoredValidationMessage(uint64_t(location)))
                return VK_FALSE;

            char header[160];
            snprintf(header, sizeof(header), "[Vulkan: location=0x%zx code=%d, layerPrefix='%s'] ", location, code,
                layerPrefix ? layerPrefix : "");
            factory->reportValidationMessage(MessageSeverity::Warning, std::string(header) + (msg ? msg : ""));
            return VK_FALSE;
        }

        VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(
            vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
            vk::DebugUtilsMessageTypeFlagsEXT messageTypes,
            const vk::DebugUtilsMessengerCallbackDataEXT* callbackData,
            void* userData)
        {
            (void)messageTypes;
            const auto factory = static_cast<const GraphicsFactory*>(userData);
            if (!factory)
                return VK_FALSE;

            const char* idName = (callbackData && callbackData->pMessageIdName) ? callbackData->pMessageIdName : "";
            const char* text = (callbackData && callbackData->pMessage) ? callbackData->pMessage : "";
            factory->reportValidationMessage(
                severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError ? MessageSeverity::Error : MessageSeverity::Warning,
                std::string("[Vulkan debug-utils: ") + idName + "] " + text);
            return VK_FALSE;
        }
    }

    Format GetSwapChainFormat(Format format)
    {
        if (format == Format::SRGBA8_UNORM)
            return Format::SBGRA8_UNORM;
        if (format == Format::RGBA8_UNORM)
            return Format::BGRA8_UNORM;
        return format;
    }

    GraphicsFactory::GraphicsFactory(const GraphicsFactoryDesc& desc)
        : GraphicsFactoryBase(desc.messageCallback)
        , m_Desc(desc)
    { }

    GraphicsFactory::~GraphicsFactory()
    {
        if (m_Instance)
        {
            if (m_DebugReportCallback)
                m_Instance.destroyDebugReportCallbackEXT(m_DebugReportCallback);
            if (m_DebugUtilsMessenger)
                m_Instance.destroyDebugUtilsMessengerEXT(m_DebugUtilsMessenger);
            m_Instance.destroy();
        }
    }

    bool GraphicsFactory::initialize(std::string& outError)
    {
        if (m_Desc.enableRayTracingValidation)
        {
            // The driver only exposes VK_NV_ray_tracing_validation if this is set before the ICD loads.
#ifdef _WIN32
            const int envResult = _putenv_s("NV_ALLOW_RAYTRACING_VALIDATION", "1");
#else
            const int envResult = setenv("NV_ALLOW_RAYTRACING_VALIDATION", "1", 1);
#endif
            if (envResult != 0)
                warning("Failed to set NV_ALLOW_RAYTRACING_VALIDATION - ray tracing validation will be inert.");
        }

        try
        {
            m_Loader = std::make_unique<VulkanDynamicLoader>(std::string(m_Desc.vulkanLibraryName.c_str()));
        }
        catch (const std::exception& e)
        {
            outError = std::string("cannot load the Vulkan library: ") + e.what();
            return false;
        }

        m_GetInstanceProcAddr = m_Loader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
        if (!m_GetInstanceProcAddr)
        {
            outError = "vkGetInstanceProcAddr is not available";
            return false;
        }
        VULKAN_HPP_DEFAULT_DISPATCHER.init(m_GetInstanceProcAddr);

        try
        {
            if (!createInstance(outError))
                return false;
            installDebugCallbacks();
            enumeratePhysicalDevices();
        }
        catch (const std::exception& e)
        {
            outError = e.what();
            return false;
        }
        return true;
    }

    bool GraphicsFactory::createInstance(std::string& outError)
    {
        std::unordered_set<std::string> requiredExtensions = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
        std::unordered_set<std::string> optionalExtensions = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME };
        std::unordered_set<std::string> requiredLayers;
        std::unordered_set<std::string> optionalLayers;

        if (!m_Desc.headless)
        {
            requiredExtensions.insert(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            requiredExtensions.insert(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
            requiredExtensions.insert(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif
        }

        for (const string& name : m_Desc.requiredInstanceExtensions)
            requiredExtensions.insert(name.c_str());
        for (const string& name : m_Desc.optionalInstanceExtensions)
            optionalExtensions.insert(name.c_str());
        for (const string& name : m_Desc.requiredLayers)
            requiredLayers.insert(name.c_str());
        for (const string& name : m_Desc.optionalLayers)
            optionalLayers.insert(name.c_str());

        if (m_Desc.enableDebugRuntime)
        {
            requiredExtensions.insert(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
            requiredLayers.insert("VK_LAYER_KHRONOS_validation");
        }

        // Figure out which optional extensions are supported, and which required ones are not.
        std::unordered_set<std::string> enabledExtensions = requiredExtensions;
        std::unordered_set<std::string> missingExtensions = requiredExtensions;
        for (const auto& extension : vk::enumerateInstanceExtensionProperties())
        {
            const std::string name = extension.extensionName;
            if (optionalExtensions.count(name))
                enabledExtensions.insert(name);
            missingExtensions.erase(name);
        }

        if (!missingExtensions.empty())
        {
            std::stringstream ss;
            ss << "Cannot create a Vulkan instance because the following required extension(s) are not supported:";
            for (const auto& name : missingExtensions)
                ss << std::endl << "  - " << name;
            outError = ss.str();
            return false;
        }

        std::unordered_set<std::string> enabledLayers = requiredLayers;
        std::unordered_set<std::string> missingLayers = requiredLayers;
        for (const auto& layer : vk::enumerateInstanceLayerProperties())
        {
            const std::string name = layer.layerName;
            if (optionalLayers.count(name))
                enabledLayers.insert(name);
            missingLayers.erase(name);
        }

        if (!missingLayers.empty())
        {
            std::stringstream ss;
            ss << "Cannot create a Vulkan instance because the following required layer(s) are not supported:";
            for (const auto& name : missingLayers)
                ss << std::endl << "  - " << name;
            outError = ss.str();
            return false;
        }

        m_EnabledInstanceExtensions = SetToVector(enabledExtensions);
        m_EnabledLayers = SetToVector(enabledLayers);

        {
            std::stringstream ss;
            ss << "Enabled Vulkan instance extensions:";
            for (const auto& name : m_EnabledInstanceExtensions)
                ss << std::endl << "    " << name;
            ss << std::endl << "Enabled Vulkan layers:";
            for (const auto& name : m_EnabledLayers)
                ss << std::endl << "    " << name;
            info(ss.str());
        }

        auto applicationInfo = vk::ApplicationInfo();

        // Query the Vulkan API version supported on the system to make sure we use at least 1.3 when that's present.
        vk::Result res = vk::enumerateInstanceVersion(&applicationInfo.apiVersion);
        if (res != vk::Result::eSuccess)
        {
            outError = std::string("Call to vkEnumerateInstanceVersion failed, error code = ") + resultToString(VkResult(res));
            return false;
        }

        if (applicationInfo.apiVersion < kMinimumVulkanVersion)
        {
            char text[200];
            snprintf(text, sizeof(text), "The Vulkan API version supported on the system (%d.%d.%d) is too low, at least %d.%d.%d is required.",
                VK_API_VERSION_MAJOR(applicationInfo.apiVersion), VK_API_VERSION_MINOR(applicationInfo.apiVersion), VK_API_VERSION_PATCH(applicationInfo.apiVersion),
                VK_API_VERSION_MAJOR(kMinimumVulkanVersion), VK_API_VERSION_MINOR(kMinimumVulkanVersion), VK_API_VERSION_PATCH(kMinimumVulkanVersion));
            outError = text;
            return false;
        }

        // Spec says: A non-zero variant indicates the API is a variant of the Vulkan API and applications will typically need to be modified to run against it.
        if (VK_API_VERSION_VARIANT(applicationInfo.apiVersion) != 0)
        {
            outError = "The Vulkan API supported on the system uses an unexpected variant: " +
                std::to_string(VK_API_VERSION_VARIANT(applicationInfo.apiVersion));
            return false;
        }

        const std::vector<const char*> extensionPointers = ToPointers(m_EnabledInstanceExtensions);
        const std::vector<const char*> layerPointers = ToPointers(m_EnabledLayers);

        vk::InstanceCreateInfo createInfo = vk::InstanceCreateInfo()
            .setEnabledLayerCount(uint32_t(layerPointers.size()))
            .setPpEnabledLayerNames(layerPointers.data())
            .setEnabledExtensionCount(uint32_t(extensionPointers.size()))
            .setPpEnabledExtensionNames(extensionPointers.data())
            .setPApplicationInfo(&applicationInfo);

        res = vk::createInstance(&createInfo, nullptr, &m_Instance);
        if (res != vk::Result::eSuccess)
        {
            outError = std::string("Failed to create a Vulkan instance, error code = ") + resultToString(VkResult(res));
            return false;
        }

        VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance);
        return true;
    }

    void GraphicsFactory::installDebugCallbacks()
    {
        if (m_Desc.enableDebugRuntime)
        {
            auto createInfo = vk::DebugReportCallbackCreateInfoEXT()
                .setFlags(vk::DebugReportFlagBitsEXT::eError |
                          vk::DebugReportFlagBitsEXT::eWarning |
                          vk::DebugReportFlagBitsEXT::ePerformanceWarning)
                .setPfnCallback(DebugReportCallback)
                .setPUserData(this);

            if (m_Instance.createDebugReportCallbackEXT(&createInfo, nullptr, &m_DebugReportCallback) != vk::Result::eSuccess)
                warning("Failed to create the Vulkan debug report callback.");
        }

        if (m_Desc.enableRayTracingValidation)
        {
            // Ray tracing validation reports only through debug-utils.
            if (!isInstanceExtensionEnabled(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
            {
                warning("VK_EXT_debug_utils is not enabled on the instance - ray-tracing-validation messages cannot be delivered.");
                return;
            }

            auto createInfo = vk::DebugUtilsMessengerCreateInfoEXT()
                .setMessageSeverity(vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                                    vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
                .setMessageType(vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                                vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                                vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance)
                .setPfnUserCallback(DebugUtilsCallback)
                .setPUserData(this);

            const vk::Result res = m_Instance.createDebugUtilsMessengerEXT(&createInfo, nullptr, &m_DebugUtilsMessenger);
            if (res != vk::Result::eSuccess)
            {
                warning(std::string("Failed to create a debug-utils messenger, error code = ") + resultToString(VkResult(res)) +
                    " - ray-tracing-validation messages cannot be delivered.");
            }
        }
    }

    void GraphicsFactory::enumeratePhysicalDevices()
    {
        m_PhysicalDevices = m_Instance.enumeratePhysicalDevices();

        for (vk::PhysicalDevice physicalDevice : m_PhysicalDevices)
        {
            vk::PhysicalDeviceProperties2 properties2;
            vk::PhysicalDeviceIDProperties idProperties;
            properties2.pNext = &idProperties;
            physicalDevice.getProperties2(&properties2);
            const auto& properties = properties2.properties;

            AdapterInfo adapterInfo;
            adapterInfo.name = string(properties.deviceName.data());
            adapterInfo.vendorID = properties.vendorID;
            adapterInfo.deviceID = properties.deviceID;
            adapterInfo.isDiscrete = properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
            adapterInfo.isSoftware = properties.deviceType == vk::PhysicalDeviceType::eCpu;
            adapterInfo.nativeAdapter = static_cast<VkPhysicalDevice>(physicalDevice);

            static_assert(sizeof(adapterInfo.uuid) == VK_UUID_SIZE);
            memcpy(adapterInfo.uuid, idProperties.deviceUUID.data(), sizeof(adapterInfo.uuid));
            adapterInfo.uuidValid = true;

            if (idProperties.deviceLUIDValid)
            {
                static_assert(sizeof(adapterInfo.luid) == VK_LUID_SIZE);
                memcpy(adapterInfo.luid, idProperties.deviceLUID.data(), sizeof(adapterInfo.luid));
                adapterInfo.luidValid = true;
            }

            // Go through the memory types to figure out the amount of VRAM on this physical device.
            const vk::PhysicalDeviceMemoryProperties memoryProperties = physicalDevice.getMemoryProperties();
            for (uint32_t heapIndex = 0; heapIndex < memoryProperties.memoryHeapCount; ++heapIndex)
            {
                const vk::MemoryHeap& heap = memoryProperties.memoryHeaps[heapIndex];
                if (heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal)
                    adapterInfo.dedicatedVideoMemory += heap.size;
            }

            m_Adapters.push_back(std::move(adapterInfo));
        }
    }

    bool GraphicsFactory::isInstanceExtensionEnabled(const char* name) const
    {
        return std::find(m_EnabledInstanceExtensions.begin(), m_EnabledInstanceExtensions.end(), name) != m_EnabledInstanceExtensions.end();
    }

    bool GraphicsFactory::isIgnoredValidationMessage(uint64_t location) const
    {
        for (uint64_t ignored : m_Desc.ignoredValidationMessageLocations)
        {
            if (ignored == location)
                return true;
        }
        return false;
    }

    vk::SurfaceKHR GraphicsFactory::createSurface(NativeObject window, NativeObject display, std::string& outError) const
    {
        vk::SurfaceKHR surface;
        vk::Result res = vk::Result::eErrorExtensionNotPresent;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        (void)display;
        auto createInfo = vk::Win32SurfaceCreateInfoKHR()
            .setHinstance(GetModuleHandle(nullptr))
            .setHwnd(static_cast<HWND>(window));
        res = m_Instance.createWin32SurfaceKHR(&createInfo, nullptr, &surface);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
        auto createInfo = vk::XlibSurfaceCreateInfoKHR()
            .setDpy(static_cast<Display*>(display))
            .setWindow(::Window(reinterpret_cast<uintptr_t>(window)));
        res = m_Instance.createXlibSurfaceKHR(&createInfo, nullptr, &surface);
#else
        (void)window;
        (void)display;
#endif
        if (res != vk::Result::eSuccess)
        {
            outError = std::string("Failed to create a window surface, error code = ") + resultToString(VkResult(res));
            return vk::SurfaceKHR();
        }
        return surface;
    }

    bool GraphicsFactory::findQueueFamilies(vk::PhysicalDevice physicalDevice, vk::SurfaceKHR surface,
        const DeviceCreationDesc& desc, QueueFamilies& outFamilies) const
    {
        QueueFamilies families;
        const auto properties = physicalDevice.getQueueFamilyProperties();

        for (int i = 0; i < int(properties.size()); i++)
        {
            const auto& queueFamily = properties[i];
            if (queueFamily.queueCount == 0)
                continue;

            if (families.graphics == -1 && (queueFamily.queueFlags & vk::QueueFlagBits::eGraphics))
                families.graphics = i;

            if (families.compute == -1 &&
                (queueFamily.queueFlags & vk::QueueFlagBits::eCompute) &&
                !(queueFamily.queueFlags & vk::QueueFlagBits::eGraphics))
            {
                families.compute = i;
            }

            if (families.transfer == -1 &&
                (queueFamily.queueFlags & vk::QueueFlagBits::eTransfer) &&
                !(queueFamily.queueFlags & vk::QueueFlagBits::eCompute) &&
                !(queueFamily.queueFlags & vk::QueueFlagBits::eGraphics))
            {
                families.transfer = i;
            }

            if (families.present == -1 && !m_Desc.headless)
            {
                bool presentSupported = false;
                if (surface)
                    presentSupported = physicalDevice.getSurfaceSupportKHR(uint32_t(i), surface) != VK_FALSE;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
                else
                    presentSupported = physicalDevice.getWin32PresentationSupportKHR(uint32_t(i)) != VK_FALSE;
#else
                else
                    presentSupported = (queueFamily.queueFlags & vk::QueueFlagBits::eGraphics) != vk::QueueFlags();
#endif
                if (presentSupported)
                    families.present = i;
            }
        }

        outFamilies = families;

        return !(families.graphics == -1 ||
            (families.present == -1 && !m_Desc.headless) ||
            (families.compute == -1 && desc.enableComputeQueue) ||
            (families.transfer == -1 && desc.enableCopyQueue));
    }

    bool GraphicsFactory::pickPhysicalDevice(const DeviceCreationDesc& desc,
        const std::unordered_set<std::string>& requiredExtensions, vk::SurfaceKHR surface,
        vk::PhysicalDevice& outDevice, QueueFamilies& outFamilies) const
    {
        const VkFormat requestedFormat = convertFormat(GetSwapChainFormat(desc.surface.format));
        const vk::Extent2D requestedExtent(desc.surface.width, desc.surface.height);

        const int adapterIndex = desc.adapterIndex;
        int firstDevice = 0;
        int lastDevice = int(m_PhysicalDevices.size()) - 1;
        if (adapterIndex >= 0)
        {
            if (adapterIndex > lastDevice)
            {
                error("The specified Vulkan physical device " + std::to_string(adapterIndex) + " does not exist.");
                return false;
            }
            firstDevice = adapterIndex;
            lastDevice = adapterIndex;
        }

        // Start building an error message in case we cannot find a device.
        std::stringstream errorStream;
        errorStream << "Cannot find a Vulkan device that supports all the required extensions and properties.";

        struct Candidate
        {
            vk::PhysicalDevice device;
            QueueFamilies families;
        };
        std::vector<Candidate> discreteGPUs;
        std::vector<Candidate> otherGPUs;

        for (int deviceIndex = firstDevice; deviceIndex <= lastDevice; ++deviceIndex)
        {
            const vk::PhysicalDevice& dev = m_PhysicalDevices[deviceIndex];

            vk::PhysicalDeviceProperties2 prop2;
            const auto& prop = prop2.properties;
            vk::PhysicalDeviceMultiviewProperties multiViewProp;
            prop2.pNext = &multiViewProp;
            dev.getProperties2(&prop2);

            errorStream << std::endl << prop.deviceName.data() << ":";

            // check that all required device extensions are present
            std::unordered_set<std::string> missingExtensions = requiredExtensions;
            for (const auto& ext : dev.enumerateDeviceExtensionProperties())
                missingExtensions.erase(std::string(ext.extensionName.data()));

            bool deviceIsGood = true;

            if (!missingExtensions.empty())
            {
                for (const auto& ext : missingExtensions)
                    errorStream << std::endl << "  - missing " << ext;
                deviceIsGood = false;
            }

            if (prop.apiVersion < kMinimumVulkanVersion)
            {
                errorStream << std::endl << "  - does not support Vulkan " <<
                    VK_API_VERSION_MAJOR(kMinimumVulkanVersion) << "." << VK_API_VERSION_MINOR(kMinimumVulkanVersion);
                deviceIsGood = false;
            }

            if (desc.multiview.enabled)
            {
                if (multiViewProp.maxMultiviewViewCount < desc.multiview.maxMultiviewViewCount ||
                    multiViewProp.maxMultiviewInstanceIndex < desc.multiview.maxMultiviewInstanceIndex)
                {
                    errorStream
                        << std::endl
                        << " -  does not support required multi-view properties, required (maxMultiviewViewCount = "
                        << desc.multiview.maxMultiviewViewCount << ", maxMultiviewInstanceIndex = "
                        << desc.multiview.maxMultiviewInstanceIndex
                        << "), candidate (maxMultiviewViewCount = " << multiViewProp.maxMultiviewViewCount
                        << ", maxMultiviewInstanceIndex = " << multiViewProp.maxMultiviewInstanceIndex << ")";
                    deviceIsGood = false;
                }
            }

            vk::PhysicalDeviceFeatures2 deviceFeatures2{};
            vk::PhysicalDeviceVulkan13Features vulkan13Features{};
            vk::PhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{};
            deviceFeatures2.pNext = &vulkan13Features;
            vulkan13Features.pNext = &descriptorIndexingFeatures;

            dev.getFeatures2(&deviceFeatures2);
            if (!deviceFeatures2.features.samplerAnisotropy)
            {
                // device is a toaster oven
                errorStream << std::endl << "  - does not support samplerAnisotropy";
                deviceIsGood = false;
            }
            if (!deviceFeatures2.features.textureCompressionBC)
            {
                errorStream << std::endl << "  - does not support textureCompressionBC";
                deviceIsGood = false;
            }
            if (!vulkan13Features.dynamicRendering)
            {
                errorStream << std::endl << "  - does not support dynamicRendering";
                deviceIsGood = false;
            }
            if (!vulkan13Features.synchronization2)
            {
                errorStream << std::endl << "  - does not support synchronization2";
                deviceIsGood = false;
            }
            // Bindless descriptor tables need update-after-bind for every type they can hold.
            if (!(descriptorIndexingFeatures.descriptorBindingSampledImageUpdateAfterBind
                    && descriptorIndexingFeatures.descriptorBindingStorageImageUpdateAfterBind
                    && descriptorIndexingFeatures.descriptorBindingStorageBufferUpdateAfterBind
                    && descriptorIndexingFeatures.descriptorBindingUniformTexelBufferUpdateAfterBind
                    && descriptorIndexingFeatures.descriptorBindingStorageTexelBufferUpdateAfterBind))
            {
                errorStream << std::endl << "  - does not support update-after-bind descriptor indexing";
                deviceIsGood = false;
            }
            // The mutable heap can alias uniform buffers, so it needs this too.
            if ((desc.enableCbvDescriptorStreaming || desc.enableHeapDirectlyIndexed) &&
                !descriptorIndexingFeatures.descriptorBindingUniformBufferUpdateAfterBind)
            {
                errorStream << std::endl << "  - does not support descriptorBindingUniformBufferUpdateAfterBind";
                deviceIsGood = false;
            }

            QueueFamilies families;
            if (!findQueueFamilies(dev, surface, desc, families))
            {
                // device doesn't have all the queue families we need
                errorStream << std::endl << "  - does not support the necessary queue types";
                deviceIsGood = false;
            }

            if (deviceIsGood && surface)
            {
                const bool surfaceSupported = dev.getSurfaceSupportKHR(uint32_t(families.present), surface) != VK_FALSE;
                if (!surfaceSupported)
                {
                    errorStream << std::endl << "  - does not support the window surface";
                    deviceIsGood = false;
                }
                else
                {
                    // check that this device supports our intended swap chain creation parameters
                    const auto surfaceCaps = dev.getSurfaceCapabilitiesKHR(surface);
                    const auto surfaceFmts = dev.getSurfaceFormatsKHR(surface);

                    if (surfaceCaps.minImageCount > desc.surface.bufferCount ||
                        (surfaceCaps.maxImageCount < desc.surface.bufferCount && surfaceCaps.maxImageCount > 0))
                    {
                        errorStream << std::endl << "  - cannot support the requested swap chain image count:";
                        errorStream << " requested " << desc.surface.bufferCount << ", available " << surfaceCaps.minImageCount << " - " << surfaceCaps.maxImageCount;
                        deviceIsGood = false;
                    }

                    if (requestedExtent.width != 0 && requestedExtent.height != 0 &&
                        (surfaceCaps.minImageExtent.width > requestedExtent.width ||
                         surfaceCaps.minImageExtent.height > requestedExtent.height ||
                         surfaceCaps.maxImageExtent.width < requestedExtent.width ||
                         surfaceCaps.maxImageExtent.height < requestedExtent.height))
                    {
                        errorStream << std::endl << "  - cannot support the requested swap chain size:";
                        errorStream << " requested " << requestedExtent.width << "x" << requestedExtent.height << ", ";
                        errorStream << " available " << surfaceCaps.minImageExtent.width << "x" << surfaceCaps.minImageExtent.height;
                        errorStream << " - " << surfaceCaps.maxImageExtent.width << "x" << surfaceCaps.maxImageExtent.height;
                        deviceIsGood = false;
                    }

                    bool surfaceFormatPresent = false;
                    for (const vk::SurfaceFormatKHR& surfaceFmt : surfaceFmts)
                    {
                        if (surfaceFmt.format == vk::Format(requestedFormat))
                        {
                            surfaceFormatPresent = true;
                            break;
                        }
                    }

                    if (!surfaceFormatPresent)
                    {
                        // can't create a swap chain using the format requested
                        errorStream << std::endl << "  - does not support the requested swap chain format";
                        deviceIsGood = false;
                    }

                    // check that we can present from the graphics queue
                    if (!dev.getSurfaceSupportKHR(uint32_t(families.graphics), surface))
                    {
                        errorStream << std::endl << "  - cannot present";
                        deviceIsGood = false;
                    }
                }
            }

            if (!deviceIsGood)
                continue;

            if (prop.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
                discreteGPUs.push_back({ dev, families });
            else
                otherGPUs.push_back({ dev, families });
        }

        // pick the first discrete GPU if it exists, otherwise the first integrated GPU
        const std::vector<Candidate>& candidates = !discreteGPUs.empty() ? discreteGPUs : otherGPUs;
        if (!candidates.empty())
        {
            outDevice = candidates[0].device;
            outFamilies = candidates[0].families;
            return true;
        }

        error(errorStream.str());
        return false;
    }

    NativeObject GraphicsFactory::getNativeObject(ObjectType objectType) noexcept
    {
        switch (objectType)
        {
        case ObjectTypes::VK_Instance:
            return static_cast<VkInstance>(m_Instance);
        case ObjectTypes::VK_GetInstanceProcAddr:
            return reinterpret_cast<NativeObject>(m_GetInstanceProcAddr);
        default:
            return nullptr;
        }
    }

    uint32_t GraphicsFactory::getEnabledInstanceExtensions(const char** names, uint32_t capacity) noexcept
    {
        return CopyNames(m_EnabledInstanceExtensions, names, capacity);
    }

    uint32_t GraphicsFactory::getEnabledLayers(const char** names, uint32_t capacity) noexcept
    {
        return CopyNames(m_EnabledLayers, names, capacity);
    }

    FRESULT GraphicsFactory::createDevice(const DeviceDesc& desc, IDevice** ppDevice) noexcept
    {
        if (!BeginCreate(ppDevice))
            return FE_INVALID_ARGS;
        if (!desc.instance || !desc.physicalDevice || !desc.device)
            return FE_INVALID_ARGS;
        return vulkan::CreateDevice(desc, nullptr, ppDevice);
    }

    FRESULT GraphicsFactory::createDeviceOnAdapter(const DeviceCreationDesc& desc, IDevice** ppDevice) noexcept
    {
        if (!BeginCreate(ppDevice))
            return FE_INVALID_ARGS;

        vk::SurfaceKHR surface;
        vk::Device device;

        try
        {
            std::unordered_set<std::string> enabledExtensions;
            std::unordered_set<std::string> optionalExtensions(std::begin(kDefaultOptionalDeviceExtensions),
                std::end(kDefaultOptionalDeviceExtensions));

            for (const string& name : desc.requiredExtensions)
                enabledExtensions.insert(name.c_str());
            for (const string& name : desc.optionalExtensions)
                optionalExtensions.insert(name.c_str());

            // Promote to required so an unsuitable GPU is rejected during selection, not after
            if (desc.enableHeapDirectlyIndexed)
                enabledExtensions.insert(VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME);

            if (!m_Desc.headless && desc.surface.window)
            {
                std::string errorText;
                surface = createSurface(desc.surface.window, desc.surface.display, errorText);
                if (!surface)
                {
                    error(errorText);
                    return FE_GENERIC_ERROR;
                }
            }

            vk::PhysicalDevice physicalDevice;
            QueueFamilies families;
            const bool picked = pickPhysicalDevice(desc, enabledExtensions, surface, physicalDevice, families);

            if (surface)
            {
                m_Instance.destroySurfaceKHR(surface);
                surface = vk::SurfaceKHR();
            }

            if (!picked)
                return FE_NOT_FOUND;

            // figure out which optional extensions are supported
            bool swapChainMutableFormatSupported = false;
            for (const auto& ext : physicalDevice.enumerateDeviceExtensionProperties())
            {
                const std::string name = ext.extensionName;
                if (optionalExtensions.count(name))
                {
                    if (name == VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME && m_Desc.headless)
                        continue;

                    enabledExtensions.insert(name);
                }

                // Gated by the NV_ALLOW_RAYTRACING_VALIDATION opt-in set before ICD load.
                if (desc.enableRayTracingValidation && name == VK_NV_RAY_TRACING_VALIDATION_EXTENSION_NAME)
                    enabledExtensions.insert(name);

                if (desc.enableRayTracingExtensions &&
                    std::find(std::begin(kRayTracingExtensions), std::end(kRayTracingExtensions), name) != std::end(kRayTracingExtensions))
                {
                    enabledExtensions.insert(name);
                }
            }

            if (!m_Desc.headless)
                enabledExtensions.insert(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

            const vk::PhysicalDeviceProperties physicalDeviceProperties = physicalDevice.getProperties();
            const std::string rendererName = physicalDeviceProperties.deviceName.data();

            bool accelStructSupported = false;
            bool rayPipelineSupported = false;
            bool rayQuerySupported = false;
            bool vrsSupported = false;
            bool interlockSupported = false;
            bool barycentricSupported = false;
            bool aftermathSupported = false;
            bool clusterAccelerationStructureSupported = false;
            bool mutableDescriptorTypeSupported = false;
            bool linearSweptSpheresSupported = false;
            bool meshShaderSupported = false;
            bool rayTracingPositionFetchSupported = false;
            bool rayTracingValidationSupported = false;

            std::stringstream extensionLog;
            extensionLog << "Enabled Vulkan device extensions:";
            for (const auto& ext : enabledExtensions)
            {
                extensionLog << std::endl << "    " << ext;

                if (ext == VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
                    accelStructSupported = true;
                else if (ext == VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)
                    rayPipelineSupported = true;
                else if (ext == VK_KHR_RAY_QUERY_EXTENSION_NAME)
                    rayQuerySupported = true;
                else if (ext == VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)
                    vrsSupported = true;
                else if (ext == VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME)
                    interlockSupported = true;
                else if (ext == VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME)
                    barycentricSupported = true;
                else if (ext == VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME)
                    swapChainMutableFormatSupported = true;
                else if (ext == VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME)
                    aftermathSupported = true;
                else if (ext == VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME)
                    clusterAccelerationStructureSupported = true;
                else if (ext == VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME)
                    mutableDescriptorTypeSupported = true;
                else if (ext == VK_NV_RAY_TRACING_LINEAR_SWEPT_SPHERES_EXTENSION_NAME)
                    linearSweptSpheresSupported = true;
                else if (ext == VK_EXT_MESH_SHADER_EXTENSION_NAME)
                    meshShaderSupported = true;
                else if (ext == VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME)
                    rayTracingPositionFetchSupported = true;
                else if (ext == VK_NV_RAY_TRACING_VALIDATION_EXTENSION_NAME)
                    rayTracingValidationSupported = true;
            }
            info(extensionLog.str());
            (void)swapChainMutableFormatSupported;

#define APPEND_EXTENSION(condition, desc) if (condition) { (desc).pNext = pNext; pNext = &(desc); }  // NOLINT(cppcoreguidelines-macro-usage)
            void* pNext = nullptr;

            vk::PhysicalDeviceFeatures2 physicalDeviceFeatures2;
            // Determine support for Buffer Device Address, the Vulkan 1.2 way
            auto bufferDeviceAddressFeatures = vk::PhysicalDeviceBufferDeviceAddressFeatures();
            // Determine support for aftermath
            auto aftermathPhysicalFeatures = vk::PhysicalDeviceDiagnosticsConfigFeaturesNV();
            // Determine support for mesh and task shaders
            auto meshShaderFeatures = vk::PhysicalDeviceMeshShaderFeaturesEXT();

            // Put the user-provided extension structure at the end of the chain
            pNext = desc.physicalDeviceFeatures2Extensions;
            APPEND_EXTENSION(true, bufferDeviceAddressFeatures);
            APPEND_EXTENSION(aftermathSupported, aftermathPhysicalFeatures);
            APPEND_EXTENSION(meshShaderSupported, meshShaderFeatures);

            physicalDeviceFeatures2.pNext = pNext;
            physicalDevice.getFeatures2(&physicalDeviceFeatures2);

            // Optional Vulkan 1.2 features. Queried with a chain of their own, because VkPhysicalDeviceVulkan12Features
            // overlaps the per-feature structures (bufferDeviceAddressFeatures) in the chain above.
            vk::PhysicalDeviceVulkan12Features supportedVulkan12Features;
            {
                vk::PhysicalDeviceFeatures2 query;
                query.pNext = &supportedVulkan12Features;
                physicalDevice.getFeatures2(&query);
            }

            std::unordered_set<int> uniqueQueueFamilies = { families.graphics };
            if (!m_Desc.headless)
                uniqueQueueFamilies.insert(families.present);
            if (desc.enableComputeQueue)
                uniqueQueueFamilies.insert(families.compute);
            if (desc.enableCopyQueue)
                uniqueQueueFamilies.insert(families.transfer);

            float priority = 1.f;
            std::vector<vk::DeviceQueueCreateInfo> queueDesc;
            queueDesc.reserve(uniqueQueueFamilies.size());
            for (int queueFamily : uniqueQueueFamilies)
            {
                queueDesc.push_back(vk::DeviceQueueCreateInfo()
                    .setQueueFamilyIndex(queueFamily)
                    .setQueueCount(1)
                    .setPQueuePriorities(&priority));
            }

            auto accelStructFeatures = vk::PhysicalDeviceAccelerationStructureFeaturesKHR()
                .setAccelerationStructure(true);
            auto rayPipelineFeatures = vk::PhysicalDeviceRayTracingPipelineFeaturesKHR()
                .setRayTracingPipeline(true)
                .setRayTraversalPrimitiveCulling(true);
            auto rayQueryFeatures = vk::PhysicalDeviceRayQueryFeaturesKHR()
                .setRayQuery(true);
            auto interlockFeatures = vk::PhysicalDeviceFragmentShaderInterlockFeaturesEXT()
                .setFragmentShaderPixelInterlock(true);
            auto barycentricFeatures = vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR()
                .setFragmentShaderBarycentric(true);
            auto vrsFeatures = vk::PhysicalDeviceFragmentShadingRateFeaturesKHR()
                .setPipelineFragmentShadingRate(true)
                .setPrimitiveFragmentShadingRate(true)
                .setAttachmentFragmentShadingRate(true);
            auto vulkan13features = vk::PhysicalDeviceVulkan13Features()
                .setDynamicRendering(true)
                .setSynchronization2(true)
                .setMaintenance4(true);
#if NVRHI_WITH_AFTERMATH
            auto aftermathFeatures = vk::DeviceDiagnosticsConfigCreateInfoNV()
                .setFlags(vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableResourceTracking
                    | vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderDebugInfo
                    | vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderErrorReporting);
#endif
            auto clusterAccelerationStructureFeatures = vk::PhysicalDeviceClusterAccelerationStructureFeaturesNV()
                .setClusterAccelerationStructure(true);
            auto mutableDescriptorTypeFeatures = vk::PhysicalDeviceMutableDescriptorTypeFeaturesEXT()
                .setMutableDescriptorType(true);
            auto linearSweptSpheresFeatures = vk::PhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV()
                .setSpheres(true)
                .setLinearSweptSpheres(true);
            auto rayTracingPositionFetchFeatures = vk::PhysicalDeviceRayTracingPositionFetchFeaturesKHR()
                .setRayTracingPositionFetch(true);
            auto rayTracingValidationFeatures = vk::PhysicalDeviceRayTracingValidationFeaturesNV()
                .setRayTracingValidation(true);

            pNext = nullptr;
            APPEND_EXTENSION(true, vulkan13features)
            APPEND_EXTENSION(accelStructSupported, accelStructFeatures)
            APPEND_EXTENSION(rayPipelineSupported, rayPipelineFeatures)
            APPEND_EXTENSION(rayQuerySupported, rayQueryFeatures)
            APPEND_EXTENSION(vrsSupported, vrsFeatures)
            APPEND_EXTENSION(interlockSupported, interlockFeatures)
            APPEND_EXTENSION(barycentricSupported, barycentricFeatures)
            APPEND_EXTENSION(clusterAccelerationStructureSupported, clusterAccelerationStructureFeatures)
            APPEND_EXTENSION(mutableDescriptorTypeSupported, mutableDescriptorTypeFeatures)
            APPEND_EXTENSION(linearSweptSpheresSupported, linearSweptSpheresFeatures)
            APPEND_EXTENSION(rayTracingValidationSupported, rayTracingValidationFeatures)
            APPEND_EXTENSION(meshShaderSupported, meshShaderFeatures)
            APPEND_EXTENSION(rayTracingPositionFetchSupported, rayTracingPositionFetchFeatures)

            // Too easy to end up silently inert, so report which way it went.
            if (desc.enableRayTracingValidation)
            {
                if (rayTracingValidationSupported)
                    info("VK_NV_ray_tracing_validation: ENABLED (driver-level ray tracing validation active; "
                         "messages delivered via VK_EXT_debug_utils)");
                else
                    warning("VK_NV_ray_tracing_validation requested but NOT exposed by the driver - "
                            "ray tracing validation is INERT this run. NV_ALLOW_RAYTRACING_VALIDATION=1 "
                            "must be set before instance creation (GraphicsFactoryDesc::enableRayTracingValidation); "
                            "if this persists, the driver may not support the extension (requires r550+).");
            }

            // These mesh shader features require other device features to be enabled:
            // - VkPhysicalDeviceMultiviewFeaturesKHR::multiview
            // - VkPhysicalDeviceFragmentShadingRateFeaturesKHR::primitiveFragmentShadingRate
            // Disable the mesh shader features by default, apps can override this if needed.
            meshShaderFeatures.multiviewMeshShader = false;
            meshShaderFeatures.primitiveFragmentShadingRateMeshShader = false;

#if NVRHI_WITH_AFTERMATH
            if (aftermathPhysicalFeatures.diagnosticsConfig && desc.aftermathEnabled)
                APPEND_EXTENSION(aftermathSupported, aftermathFeatures);
#endif
#undef APPEND_EXTENSION

            auto deviceFeatures = vk::PhysicalDeviceFeatures()
                .setShaderImageGatherExtended(true)
                .setSamplerAnisotropy(true)
                .setTessellationShader(true)
                .setTextureCompressionBC(true)
                .setGeometryShader(true)
                .setImageCubeArray(true)
                .setShaderInt16(true)
                .setFillModeNonSolid(true)
                .setFragmentStoresAndAtomics(true)
                .setDualSrcBlend(true)
                .setVertexPipelineStoresAndAtomics(true)
                .setShaderInt64(true)
                .setShaderStorageImageWriteWithoutFormat(true)
                .setShaderStorageImageReadWithoutFormat(true)
                .setMultiViewport(desc.multiview.enabled);

            // Add a Vulkan 1.1 structure with default settings to make it easier for apps to modify them
            auto vulkan11features = vk::PhysicalDeviceVulkan11Features()
                .setStorageBuffer16BitAccess(true)
                .setVariablePointersStorageBuffer(true)
                .setPNext(pNext);

            auto vulkan12features = vk::PhysicalDeviceVulkan12Features()
                .setDescriptorIndexing(true)
                .setRuntimeDescriptorArray(true)
                .setDescriptorBindingPartiallyBound(true)
                .setDescriptorBindingVariableDescriptorCount(true)
                .setDescriptorBindingSampledImageUpdateAfterBind(true)
                .setDescriptorBindingStorageImageUpdateAfterBind(true)
                .setDescriptorBindingStorageBufferUpdateAfterBind(true)
                .setDescriptorBindingUniformTexelBufferUpdateAfterBind(true)
                .setDescriptorBindingStorageTexelBufferUpdateAfterBind(true)
                .setDescriptorBindingUniformBufferUpdateAfterBind(desc.enableCbvDescriptorStreaming
                    || desc.enableHeapDirectlyIndexed)
                .setDescriptorBindingUpdateUnusedWhilePending(true)
                .setTimelineSemaphore(true)
                .setShaderSampledImageArrayNonUniformIndexing(true)
                // NonUniformResourceIndex() on a bindless RWTexture/RWBuffer array makes DXC declare the
                // StorageImage/StorageBufferArrayNonUniformIndexing SPIR-V capabilities, which need these features.
                // Storage buffers are required with descriptorIndexing; storage images are optional.
                .setShaderStorageBufferArrayNonUniformIndexing(supportedVulkan12Features.shaderStorageBufferArrayNonUniformIndexing)
                .setShaderStorageImageArrayNonUniformIndexing(supportedVulkan12Features.shaderStorageImageArrayNonUniformIndexing)
                .setBufferDeviceAddress(bufferDeviceAddressFeatures.bufferDeviceAddress)
                .setShaderSubgroupExtendedTypes(true)
                .setScalarBlockLayout(true)
                .setShaderBufferInt64Atomics(true)
                .setPNext(&vulkan11features);

            const std::vector<std::string> extensionNames = SetToVector(enabledExtensions);
            const std::vector<const char*> extensionPointers = ToPointers(extensionNames);

            auto deviceCreateInfo = vk::DeviceCreateInfo()
                .setPQueueCreateInfos(queueDesc.data())
                .setQueueCreateInfoCount(uint32_t(queueDesc.size()))
                .setPEnabledFeatures(&deviceFeatures)
                .setEnabledExtensionCount(uint32_t(extensionPointers.size()))
                .setPpEnabledExtensionNames(extensionPointers.data())
                .setPNext(&vulkan12features);

            if (desc.deviceCreateCallback)
                desc.deviceCreateCallback->onCreateDevice(*reinterpret_cast<VkDeviceCreateInfo*>(&deviceCreateInfo));

            const vk::Result res = physicalDevice.createDevice(&deviceCreateInfo, nullptr, &device);
            if (res != vk::Result::eSuccess)
            {
                error(std::string("Failed to create a Vulkan physical device, error code = ") + resultToString(VkResult(res)));
                return FE_GENERIC_ERROR;
            }

            VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

            vk::Queue graphicsQueue, computeQueue, transferQueue, presentQueue;
            device.getQueue(families.graphics, kQueueIndex, &graphicsQueue);
            if (desc.enableComputeQueue)
                device.getQueue(families.compute, kQueueIndex, &computeQueue);
            if (desc.enableCopyQueue)
                device.getQueue(families.transfer, kQueueIndex, &transferQueue);
            if (!m_Desc.headless)
                device.getQueue(families.present, kQueueIndex, &presentQueue);

            const std::vector<const char*> instanceExtensionPointers = ToPointers(m_EnabledInstanceExtensions);

            DeviceDesc deviceDesc;
            deviceDesc.errorCB = desc.errorCB;
            deviceDesc.instance = m_Instance;
            deviceDesc.physicalDevice = physicalDevice;
            deviceDesc.device = device;
            deviceDesc.graphicsQueue = graphicsQueue;
            deviceDesc.graphicsQueueIndex = families.graphics;
            if (desc.enableComputeQueue)
            {
                deviceDesc.computeQueue = computeQueue;
                deviceDesc.computeQueueIndex = families.compute;
            }
            else
            {
                deviceDesc.computeQueue = VK_NULL_HANDLE;
            }
            if (desc.enableCopyQueue)
            {
                deviceDesc.transferQueue = transferQueue;
                deviceDesc.transferQueueIndex = families.transfer;
            }
            else
            {
                deviceDesc.transferQueue = VK_NULL_HANDLE;
            }
            deviceDesc.instanceExtensions = const_cast<const char**>(instanceExtensionPointers.data());
            deviceDesc.numInstanceExtensions = instanceExtensionPointers.size();
            deviceDesc.deviceExtensions = const_cast<const char**>(extensionPointers.data());
            deviceDesc.numDeviceExtensions = extensionPointers.size();
            deviceDesc.maxTimerQueries = desc.maxTimerQueries;
            deviceDesc.bufferDeviceAddressSupported = vulkan12features.bufferDeviceAddress;
            deviceDesc.descriptorBindingUniformBufferUpdateAfterBind = vulkan12features.descriptorBindingUniformBufferUpdateAfterBind;
            deviceDesc.aftermathEnabled = desc.aftermathEnabled;
            deviceDesc.logBufferLifetime = desc.logBufferLifetime;
            deviceDesc.vulkanLibraryName = m_Desc.vulkanLibraryName;

            NativeDeviceOwnership ownership;
            ownership.owner = static_cast<nvrhi::IGraphicsFactory*>(this);
            ownership.ownsDevice = true;
            ownership.presentQueue = presentQueue;
            ownership.presentQueueFamily = m_Desc.headless ? -1 : families.present;

            const FRESULT result = vulkan::CreateDevice(deviceDesc, &ownership, ppDevice);
            if (NVRHI_FAILED(result))
            {
                device.destroy();
                return result;
            }

            info("Created Vulkan device: " + rendererName);
            return FS_OK;
        }
        catch (const std::bad_alloc&)
        {
            if (surface)
                m_Instance.destroySurfaceKHR(surface);
            if (device && !*ppDevice)
                device.destroy();
            return FE_OUT_OF_MEMORY;
        }
        catch (const std::exception& e)
        {
            error(std::string("Vulkan device creation failed: ") + e.what());
            if (surface)
                m_Instance.destroySurfaceKHR(surface);
            if (device && !*ppDevice)
                device.destroy();
            return FE_GENERIC_ERROR;
        }
    }

    FRESULT GraphicsFactory::createSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device,
        IGraphicsSwapChain** ppSwapChain) noexcept
    {
        if (!BeginCreate(ppSwapChain))
            return FE_INVALID_ARGS;
        if (!device || !desc.window)
            return FE_INVALID_ARGS;
        if (m_Desc.headless)
        {
            error("A headless Vulkan graphics factory cannot create swap chains");
            return FE_UNSUPPORTED;
        }

        try
        {
            return CreateSwapChain(this, desc, device, ppSwapChain);
        }
        catch (const std::bad_alloc&)
        {
            return FE_OUT_OF_MEMORY;
        }
        catch (const std::exception& e)
        {
            error(std::string("Vulkan swap chain creation failed: ") + e.what());
            return FE_GENERIC_ERROR;
        }
    }

    FRESULT nvrhiVulkanCreateGraphicsFactory(const GraphicsFactoryDesc* pDesc, IGraphicsFactory** ppFactory) noexcept
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
                    pDesc->messageCallback->message(MessageSeverity::Error, ("Vulkan graphics factory: " + errorText).c_str());
                return FE_UNSUPPORTED;
            }
            *ppFactory = factory.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc&)
        {
            return FE_OUT_OF_MEMORY;
        }
    }
}
