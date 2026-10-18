// SPDX-License-Identifier: MIT
#include <nvrhi/core/foundation.h>
#include <nvrhi/cuda.h>
#include <nvrhi/interop.h>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <cstring>
#include <vector>
#include <vulkan/vulkan.h>
using namespace nvrhi;
struct Messages : ObjectImpl<IMessageCallback>
{
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity, const char *text) noexcept override
    {
        std::fprintf(stderr, "NVRHI: %s\n", text);
    }
};
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #c);                                                \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)
#define VK(c) CHECK((c) == VK_SUCCESS)
#define OK(c) CHECK(NVRHI_SUCCEEDED(c))
static uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties p{};
    vkGetPhysicalDeviceMemoryProperties(physical, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return ~0u;
}
int main()
{
    auto callback = MAKE_RC_OBJ_PTR(Messages);
    cuda::DeviceDesc cd;
    cd.enableOptiX = false;
    cd.errorCB = callback;
    auto cudaDevice = cuda::createDevice(cd);
    if (!cudaDevice)
        return 77;
    InteropDeviceHandle interop;
    OK(cuda::nvrhiCudaCreateInteropDevice(cudaDevice, &interop));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    app.pApplicationName = "NVRHI native CUDA/Vulkan interop test";
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance;
    VK(vkCreateInstance(&ci, nullptr, &instance));
    uint32_t count = 0;
    VK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    AdapterIdentity identity;
    for (auto p : devices)
    {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &id;
        vkGetPhysicalDeviceProperties2(p, &properties);
        if (std::memcmp(id.deviceUUID, interop->getAdapterIdentity().uuid, 16) == 0)
        {
            physical = p;
            std::memcpy(identity.uuid, id.deviceUUID, 16);
            identity.uuidValid = true;
            break;
        }
    }
    CHECK(physical);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = ~0u;
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            family = i;
            break;
        }
    CHECK(family != ~0u);
#ifdef _WIN32
    const auto memoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    const auto semaphoreHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    const char *extensions[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
#else
    const auto memoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    const auto semaphoreHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    const char *extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
#endif
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &timeline;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = extensions;
    VkDevice device;
    VK(vkCreateDevice(physical, &dci, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceExternalBufferInfo ebi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
    ebi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    ebi.handleType = memoryHandleType;
    VkExternalBufferProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(physical, &ebi, &ep);
    CHECK(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
    bool dedicated =
        (ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    constexpr uint64_t bytes = 1024;
    VkExternalMemoryBufferCreateInfo externalBuffer{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    externalBuffer.handleTypes = memoryHandleType;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &externalBuffer;
    bci.size = bytes;
    bci.usage = ebi.usage;
    VkBuffer shared;
    VK(vkCreateBuffer(device, &bci, nullptr, &shared));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, shared, &requirements);
    VkDeviceSize offset = dedicated ? 0 : requirements.alignment;
    VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicatedInfo.buffer = shared;
    VkExportMemoryAllocateInfo exportMemory{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exportMemory.handleTypes = memoryHandleType;
    if (dedicated)
        exportMemory.pNext = &dedicatedInfo;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &exportMemory;
    mai.allocationSize = offset + requirements.size;
    mai.memoryTypeIndex = memoryType(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(mai.memoryTypeIndex != ~0u);
    VkDeviceMemory memory;
    VK(vkAllocateMemory(device, &mai, nullptr, &memory));
    VK(vkBindBufferMemory(device, shared, memory, offset));
    bci.pNext = nullptr;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer readback;
    VK(vkCreateBuffer(device, &bci, nullptr, &readback));
    VkMemoryRequirements rr;
    vkGetBufferMemoryRequirements(device, readback, &rr);
    VkMemoryAllocateInfo rmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    rmai.allocationSize = rr.size;
    rmai.memoryTypeIndex = memoryType(physical, rr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(rmai.memoryTypeIndex != ~0u);
    VkDeviceMemory hostMemory;
    VK(vkAllocateMemory(device, &rmai, nullptr, &hostMemory));
    VK(vkBindBufferMemory(device, readback, hostMemory, 0));
    VkExportSemaphoreCreateInfo exportSemaphore{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportSemaphore.handleTypes = semaphoreHandleType;
    VkSemaphoreTypeCreateInfo semaphoreType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    semaphoreType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semaphoreType.pNext = &exportSemaphore;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &semaphoreType;
    VkSemaphore semaphore;
    VK(vkCreateSemaphore(device, &sci, nullptr, &semaphore));
    ExternalMemoryDesc md;
    md.adapter = identity;
    md.allocationSize = mai.allocationSize;
    md.offset = offset;
    md.dedicated = dedicated;
    ExternalFenceDesc fd;
    fd.adapter = identity;
#ifdef _WIN32
    auto getMemory =
        reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
    auto getSemaphore =
        reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR"));
    CHECK(getMemory && getSemaphore);
    VkMemoryGetWin32HandleInfoKHR mh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    mh.memory = memory;
    mh.handleType = memoryHandleType;
    HANDLE memoryHandle = nullptr;
    VK(getMemory(device, &mh, &memoryHandle));
    VkSemaphoreGetWin32HandleInfoKHR sh{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    sh.semaphore = semaphore;
    sh.handleType = semaphoreHandleType;
    HANDLE semaphoreHandle = nullptr;
    VK(getSemaphore(device, &sh, &semaphoreHandle));
    md.handleType = ExternalMemoryHandleType::OpaqueWin32;
    md.handle = reinterpret_cast<intptr_t>(memoryHandle);
    fd.handleType = ExternalFenceHandleType::TimelineWin32;
    fd.handle = reinterpret_cast<intptr_t>(semaphoreHandle);
#else
    auto getMemory = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
    auto getSemaphore = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
    CHECK(getMemory && getSemaphore);
    VkMemoryGetFdInfoKHR mh{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    mh.memory = memory;
    mh.handleType = memoryHandleType;
    int memoryHandle = -1;
    VK(getMemory(device, &mh, &memoryHandle));
    VkSemaphoreGetFdInfoKHR sh{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    sh.semaphore = semaphore;
    sh.handleType = semaphoreHandleType;
    int semaphoreHandle = -1;
    VK(getSemaphore(device, &sh, &semaphoreHandle));
    md.handleType = ExternalMemoryHandleType::OpaqueFd;
    md.handle = memoryHandle;
    fd.handleType = ExternalFenceHandleType::TimelineFd;
    fd.handle = semaphoreHandle;
#endif
    BufferHandle imported;
    OK(interop->importBuffer(md, BufferDesc().setByteSize(bytes), &imported));
    ExternalFenceHandle externalSemaphore;
    OK(interop->importFence(fd, &externalSemaphore));
#ifdef _WIN32
    CloseHandle(memoryHandle);
    CloseHandle(semaphoreHandle);
#else
    close(memoryHandle);
    close(semaphoreHandle);
#endif
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = family;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    VK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 2;
    VkCommandBuffer commands[2];
    VK(vkAllocateCommandBuffers(device, &cai, commands));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(commands[0], &begin));
    vkCmdFillBuffer(commands[0], shared, 0, bytes, 0x99887766);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.buffer = shared;
    barrier.offset = 0;
    barrier.size = bytes;
    vkCmdPipelineBarrier(commands[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 1, &barrier, 0, nullptr);
    VK(vkEndCommandBuffer(commands[0]));
    uint64_t signal = 1;
    VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tsi.signalSemaphoreValueCount = 1;
    tsi.pSignalSemaphoreValues = &signal;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &tsi;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands[0];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &semaphore;
    VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    BufferHandle cudaReadback;
    OK(cudaDevice->createBuffer(BufferDesc().setByteSize(bytes).setCpuAccess(CpuAccessMode::Read), &cudaReadback));
    CommandListHandle cudaCommands;
    OK(cudaDevice->createCommandList(CommandListParameters().setQueueType(CommandQueue::Compute), &cudaCommands));
    cudaCommands->open();
    OK(interop->waitFence(cudaCommands, externalSemaphore, 1));
    cudaCommands->copyBuffer(cudaReadback, 0, imported, 0, bytes);
    cudaCommands->clearBufferUInt(imported, 0x12345678);
    OK(interop->signalFence(cudaCommands, externalSemaphore, 2));
    cudaCommands->close();
    CHECK(cudaDevice->executeCommandList(cudaCommands, CommandQueue::Compute));
    VK(vkBeginCommandBuffer(commands[1], &begin));
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.dstQueueFamilyIndex = family;
    vkCmdPipelineBarrier(commands[1], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         1, &barrier, 0, nullptr);
    VkBufferCopy region{0, 0, bytes};
    vkCmdCopyBuffer(commands[1], shared, readback, 1, &region);
    VK(vkEndCommandBuffer(commands[1]));
    uint64_t wait = 2;
    signal = 3;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    tsi.waitSemaphoreValueCount = 1;
    tsi.pWaitSemaphoreValues = &wait;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &semaphore;
    submit.pWaitDstStageMask = &waitStage;
    submit.pCommandBuffers = &commands[1];
    VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &semaphore;
    wi.pValues = &signal;
    VK(vkWaitSemaphores(device, &wi, 10000000000ull));
    void *mapped = nullptr;
    VK(vkMapMemory(device, hostMemory, 0, bytes, 0, &mapped));
    auto values = static_cast<uint32_t *>(mapped);
    for (uint32_t i = 0; i < bytes / 4; ++i)
        CHECK(values[i] == 0x12345678);
    vkUnmapMemory(device, hostMemory);
    values = static_cast<uint32_t *>(cudaDevice->mapBuffer(cudaReadback, CpuAccessMode::Read));
    CHECK(values);
    for (uint32_t i = 0; i < bytes / 4; ++i)
        CHECK(values[i] == 0x99887766);
    cudaDevice->unmapBuffer(cudaReadback);
    cudaCommands = nullptr;
    imported = nullptr;
    externalSemaphore = nullptr;
    cudaDevice->runGarbageCollection();
    VK(vkDeviceWaitIdle(device));
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroySemaphore(device, semaphore, nullptr);
    vkDestroyBuffer(device, readback, nullptr);
    vkFreeMemory(device, hostMemory, nullptr);
    vkDestroyBuffer(device, shared, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::puts(
        "CUDA/Vulkan native memory offset, queue-family ownership and bidirectional timeline semaphore test passed");
    return 0;
}
