// SPDX-License-Identifier: MIT
// No Vulkan driver/device is needed: all invalid descriptors fail before dispatch.
#include <nvrhi/vulkan.h>
#if TEST_STATIC_VULKAN_DISPATCH
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#endif
#include <cstdio>
#include <cstdint>
namespace {
bool rejects(const nvrhi::vulkan::DeviceDesc* desc) {
    auto* output = reinterpret_cast<nvrhi::vulkan::IDevice*>(uintptr_t(1));
    return nvrhi::vulkan::nvrhiVulkanCreateDevice(desc, &output) == nvrhi::FE_INVALID_ARGS && output == nullptr;
}
}
int main() {
    nvrhi::vulkan::DeviceDesc defaults; // Deliberately default-init, not value-init.
    if (defaults.instance || defaults.physicalDevice || defaults.device || defaults.graphicsQueue ||
        defaults.computeQueue || defaults.transferQueue || !rejects(nullptr) || !rejects(&defaults)) return 1;
    defaults.instance = reinterpret_cast<VkInstance>(uintptr_t(1));
    if (!rejects(&defaults)) return 2;
    defaults.physicalDevice = reinterpret_cast<VkPhysicalDevice>(uintptr_t(1));
    if (!rejects(&defaults)) return 3;
    defaults.device = reinterpret_cast<VkDevice>(uintptr_t(1));
    const VkQueue fakeQueue = reinterpret_cast<VkQueue>(uintptr_t(1));
    defaults.graphicsQueue = fakeQueue;
    if (!rejects(&defaults)) return 4;
    defaults.graphicsQueue = VK_NULL_HANDLE; defaults.computeQueue = fakeQueue;
    if (!rejects(&defaults)) return 5;
    defaults.computeQueue = VK_NULL_HANDLE; defaults.transferQueue = fakeQueue;
    if (!rejects(&defaults)) return 6;
    if (nvrhi::vulkan::nvrhiVulkanCreateDevice(&defaults, nullptr) != nvrhi::FE_INVALID_ARGS) return 7;
    std::puts("PASS Vulkan default handles, missing required handles and invalid queue families without driver dispatch");
    return 0;
}
