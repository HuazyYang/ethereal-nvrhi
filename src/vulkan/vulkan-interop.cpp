// SPDX-License-Identifier: MIT
#include "vulkan-backend.h"
#include <nvrhi/graphics-interop.h>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace nvrhi::vulkan
{
namespace
{
template <class T> T *object(IObject *value)
{
    T *result = nullptr;
    if (!value || NVRHI_FAILED(value->QueryInterface(NVRHI_IID_PPV_ARGS(&result))))
        return nullptr;
    result->Release();
    return result;
}
FRESULT status(vk::Result value)
{
    switch (value)
    {
    case vk::Result::eSuccess:
        return FS_OK;
    case vk::Result::eErrorOutOfHostMemory:
    case vk::Result::eErrorOutOfDeviceMemory:
        return FE_OUT_OF_MEMORY;
    case vk::Result::eErrorFormatNotSupported:
    case vk::Result::eErrorExtensionNotPresent:
    case vk::Result::eErrorFeatureNotPresent:
        return FE_UNSUPPORTED;
    default:
        return FE_GENERIC_ERROR;
    }
}
bool sameAdapter(const AdapterIdentity &a, const AdapterIdentity &b)
{
    if (a.uuidValid && b.uuidValid)
        return std::memcmp(a.uuid, b.uuid, 16) == 0;
    return a.luidValid && b.luidValid && std::memcmp(a.luid, b.luid, 8) == 0 && a.nodeMask && b.nodeMask &&
           (a.nodeMask & b.nodeMask) != 0;
}
#ifdef _WIN32
constexpr auto memoryType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32;
constexpr auto semaphoreType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueWin32;
#else
constexpr auto memoryType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
constexpr auto semaphoreType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd;
#endif
void closeHandle(intptr_t handle)
{
    if (handle == -1)
        return;
#ifdef _WIN32
    if (handle)
        CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    close(int(handle));
#endif
}
NVRHI_CLASS_CLSID(ExportedMemory, "f5476339-f4dd-4d6c-b6f5-3bd6eb46ed46")
class ExportedMemory final : public ObjectImpl<IExternalMemory>
{
  public:
    NVRHI_DECLARE_UUID_TRAITS(ExportedMemory)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ExportedMemory)
    NVRHI_IMPLEMENTS_INTERFACE(IExternalMemory)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_IMPLEMENTS_CLASS(ExportedMemory)
    NVRHI_END_INTERFACE_TABLE()
    AutoPtr<Device> device;
    BufferHandle buffer;
    TextureHandle texture;
    ExternalMemoryDesc desc;
    ExportedMemory(Device *d) : device(d)
    {
        desc.sourceOwner = this;
    }
    ~ExportedMemory()
    {
        closeHandle(desc.handle);
    }
    const ExternalMemoryDesc &getDesc() const noexcept override
    {
        return desc;
    }
    IBuffer *getBuffer() const noexcept override
    {
        return buffer;
    }
    ITexture *getTexture() const noexcept override
    {
        return texture;
    }
};
// Queues retain this native owner, not ExportedFence, to avoid a
// Device -> pending command -> exported fence -> Device reference cycle.
class NativeSemaphore final : public ObjectImpl<IRHIObject>
{
  public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(NativeSemaphore)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    const VulkanContext& context;
    vk::Semaphore semaphore;
    explicit NativeSemaphore(const VulkanContext& c) : context(c) { }
    ~NativeSemaphore()
    {
        if (semaphore) context.device.destroySemaphore(semaphore, context.allocationCallbacks);
    }
};
NVRHI_CLASS_CLSID(ExportedFence, "e21c57fa-2c04-4d3e-aad6-91bc09d6e4ba")
class ExportedFence final : public ObjectImpl<IExternalFence>
{
  public:
    NVRHI_DECLARE_UUID_TRAITS(ExportedFence)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ExportedFence)
    NVRHI_IMPLEMENTS_INTERFACE(IExternalFence)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_IMPLEMENTS_CLASS(ExportedFence)
    NVRHI_END_INTERFACE_TABLE()
    AutoPtr<Device> device;
    const VulkanContext &context;
    AutoPtr<NativeSemaphore> native;
    ExternalFenceDesc desc;
    ExportedFence(Device *d, const VulkanContext &c) : device(d), context(c)
    {
        desc.sourceOwner = this;
    }
    ~ExportedFence()
    {
        closeHandle(desc.handle);
    }
    const ExternalFenceDesc &getDesc() const noexcept override
    {
        return desc;
    }
};
} // namespace
class GraphicsInteropDevice final : public ObjectImpl<IGraphicsInteropDevice>
{
    AutoPtr<Device> m_device;
    const VulkanContext &m_context;
    AdapterIdentity m_identity;
    bool m_supported = false;
    FRESULT exportMemory(vk::DeviceMemory memory, uint64_t size, ExportedMemory *result)
    {
        result->desc.adapter = m_identity;
        result->desc.allocationSize = size;
        result->desc.dedicated = true; // NVRHI managed Vulkan resources use dedicated allocations.
#ifdef _WIN32
        result->desc.handleType = ExternalMemoryHandleType::OpaqueWin32;
        HANDLE handle = nullptr;
        auto info = vk::MemoryGetWin32HandleInfoKHR(memory, memoryType);
        auto code = m_context.device.getMemoryWin32HandleKHR(&info, &handle);
        result->desc.handle = reinterpret_cast<intptr_t>(handle);
#else
        result->desc.handleType = ExternalMemoryHandleType::OpaqueFd;
        int handle = -1;
        auto info = vk::MemoryGetFdInfoKHR(memory, memoryType);
        auto code = m_context.device.getMemoryFdKHR(&info, &handle);
        result->desc.handle = handle;
#endif
        return status(code);
    }
    FRESULT fenceOperation(CommandQueue queue, IExternalFence *value, uint64_t number, bool signal)
    {
        auto fence = object<ExportedFence>(value);
        if (!fence || fence->device != m_device || unsigned(queue) >= unsigned(CommandQueue::Count))
            return FE_INVALID_ARGS;
        if (!m_device->getQueue(queue))
            return FE_UNSUPPORTED;
        // An empty tracked submission retains the semaphore through GPU completion.
        // This avoids dangling native handles without blocking the calling thread.
        auto commands = m_device->createCommandList(CommandListParameters().setQueueType(queue));
        if (!commands)
            return FE_OUT_OF_MEMORY;
        commands->open();
        auto native = object<CommandList>(commands);
        native->getCurrentCmdBuf()->referencedResources.push_back(fence->native.Get());
        commands->close();
        if (signal)
            m_device->queueSignalSemaphore(queue, fence->native->semaphore, number);
        else
            m_device->queueWaitForSemaphore(queue, fence->native->semaphore, number);
        return m_device->executeCommandList(commands, queue) ? FS_OK : FE_GENERIC_ERROR;
    }
    FRESULT transfer(ICommandList *commands, IExternalMemory *value, ResourceStates state, bool acquire)
    {
        auto memory = object<ExportedMemory>(value);
        if (!commands || !memory || memory->device != m_device)
            return FE_INVALID_ARGS;
        if (commands->getDevice()->getNativeObject(ObjectTypes::VK_Device) !=
            m_device->getNativeObject(ObjectTypes::VK_Device))
            return FE_INVALID_ARGS;
        const auto queue = commands->getDesc().queueType;
        if (unsigned(queue) >= unsigned(CommandQueue::Count) || !m_device->getQueue(queue))
            return FE_INVALID_ARGS;
        const auto family = m_device->getQueue(queue)->getQueueFamilyIndex();
        auto command =
            vk::CommandBuffer(static_cast<VkCommandBuffer>(commands->getNativeObject(ObjectTypes::VK_CommandBuffer)));
        if (!command)
            return FE_INVALID_ARGS;
        if (acquire && (state == ResourceStates::Unknown || state == ResourceStates::Common))
            return FE_INVALID_ARGS;
        const auto source = acquire ? VK_QUEUE_FAMILY_EXTERNAL : family;
        const auto destination = acquire ? family : VK_QUEUE_FAMILY_EXTERNAL;
        const auto readsWrites = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
        const auto srcAccess = acquire ? vk::AccessFlags() : readsWrites;
        const auto dstAccess = acquire ? readsWrites : vk::AccessFlags();
        if (auto buffer = memory->buffer.Get())
        {
            if (!acquire)
            {
                commands->setBufferState(buffer, ResourceStates::Common);
                commands->commitBarriers();
            }
            auto barrier = vk::BufferMemoryBarrier()
                               .setSrcAccessMask(srcAccess)
                               .setDstAccessMask(dstAccess)
                               .setSrcQueueFamilyIndex(source)
                               .setDstQueueFamilyIndex(destination)
                               .setBuffer(static_cast<VkBuffer>(buffer->getNativeObject(ObjectTypes::VK_Buffer)))
                               .setOffset(0)
                               .setSize(VK_WHOLE_SIZE);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands,
                                    {}, 0, nullptr, 1, &barrier, 0, nullptr);
            if (acquire)
            {
                commands->beginTrackingBufferState(buffer, ResourceStates::Common);
                commands->setBufferState(buffer, state);
                commands->commitBarriers();
            }
        }
        else if (auto texture = memory->texture.Get())
        {
            // UnorderedAccess is NVRHI's existing GENERAL-layout tracking state;
            // this transition itself does not require a storage-image view/usage.
            if (!acquire)
            {
                commands->setTextureState(texture, AllSubresources, ResourceStates::UnorderedAccess);
                commands->commitBarriers();
            }
            const auto &d = texture->getDesc();
            auto barrier = vk::ImageMemoryBarrier()
                               .setSrcAccessMask(srcAccess)
                               .setDstAccessMask(dstAccess)
                               .setOldLayout(vk::ImageLayout::eGeneral)
                               .setNewLayout(vk::ImageLayout::eGeneral)
                               .setSrcQueueFamilyIndex(source)
                               .setDstQueueFamilyIndex(destination)
                               .setImage(static_cast<VkImage>(texture->getNativeObject(ObjectTypes::VK_Image)))
                               .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, d.mipLevels, 0, d.arraySize});
            command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands,
                                    {}, 0, nullptr, 0, nullptr, 1, &barrier);
            if (acquire)
            {
                commands->beginTrackingTextureState(texture, AllSubresources, ResourceStates::UnorderedAccess);
                commands->setTextureState(texture, AllSubresources, state);
                commands->commitBarriers();
            }
        }
        return FS_OK;
    }

  public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GraphicsInteropDevice)
    NVRHI_IMPLEMENTS_INTERFACE(IGraphicsInteropDevice)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    explicit GraphicsInteropDevice(Device *device) : m_device(device), m_context(device->m_Context)
    {
        vk::PhysicalDeviceIDProperties id;
        vk::PhysicalDeviceProperties2 properties;
        properties.pNext = &id;
        m_context.physicalDevice.getProperties2(&properties);
        std::memcpy(m_identity.uuid, id.deviceUUID.data(), 16);
        m_identity.uuidValid = true;
        std::memcpy(m_identity.luid, id.deviceLUID.data(), 8);
        m_identity.luidValid = id.deviceLUIDValid;
        m_identity.nodeMask = id.deviceNodeMask;
        m_supported = m_context.extensions.external_memory && m_context.extensions.external_semaphore;
        if (m_supported)
        {
            auto timeline = vk::SemaphoreTypeCreateInfo().setSemaphoreType(vk::SemaphoreType::eTimeline);
            auto info = vk::PhysicalDeviceExternalSemaphoreInfo().setHandleType(semaphoreType).setPNext(&timeline);
            auto caps = m_context.physicalDevice.getExternalSemaphoreProperties(info);
            m_supported = bool(caps.externalSemaphoreFeatures & vk::ExternalSemaphoreFeatureFlagBits::eExportable);
        }
    }
    const AdapterIdentity &getAdapterIdentity() const noexcept override
    {
        return m_identity;
    }
    FRESULT queryRoute(GraphicsAPI peerAPI, const AdapterIdentity &peer,
                       InteropRouteCapabilities &caps) const noexcept override
    {
        caps = {};
        caps.sameAdapter = sameAdapter(m_identity, peer);
        if (peerAPI != GraphicsAPI::CUDA || !caps.sameAdapter || !m_supported)
            return FE_UNSUPPORTED;
        caps.bufferSharing = true;
        caps.textureSharing = true;
        caps.timelineFences = true;
        caps.explicitOwnershipTransfer = true;
        return FS_OK;
    }
    FRESULT exportBuffer(IBuffer *value, IExternalMemory **output) noexcept override
    {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        auto buffer = object<Buffer>(value);
        if (!buffer || buffer->m_Context.device != m_context.device)
            return FE_INVALID_ARGS;
        const auto &d = buffer->desc;
        if (!m_supported || !buffer->managed || d.sharedResourceFlags != SharedResourceFlags::Shared || d.isVirtual ||
            d.keepInitialState || d.cpuAccess != CpuAccessMode::None || d.isVolatile)
            return FE_UNSUPPORTED;
        try
        {
            auto result = MAKE_RC_OBJ_PTR(ExportedMemory, m_device.Get());
            result->buffer = buffer;
            const auto required = m_context.device.getBufferMemoryRequirements(buffer->buffer);
            const auto code = exportMemory(buffer->memory, required.size, result);
            if (NVRHI_FAILED(code))
                return code;
            *output = result.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc &)
        {
            return FE_OUT_OF_MEMORY;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }
    FRESULT exportTexture(ITexture *value, IExternalMemory **output) noexcept override
    {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        auto texture = object<Texture>(value);
        if (!texture || texture->m_Context.device != m_context.device)
            return FE_INVALID_ARGS;
        const auto &d = texture->desc;
        const auto &format = getFormatInfo(d.format);
        const unsigned channels = unsigned(format.hasRed) + unsigned(format.hasGreen) +
            unsigned(format.hasBlue) + unsigned(format.hasAlpha);
        const unsigned componentBytes = channels ? format.bytesPerBlock / channels : 0;
        if (!channels || format.bytesPerBlock % channels ||
            (componentBytes != 1 && componentBytes != 2 && componentBytes != 4))
            return FE_UNSUPPORTED;
        if (!m_supported || !texture->managed || d.sharedResourceFlags != SharedResourceFlags::Shared || d.isVirtual ||
            d.isTiled || d.keepInitialState || d.sampleCount != 1 || format.hasDepth || format.hasStencil ||
            format.blockSize != 1 || format.isSRGB || d.format == Format::UNKNOWN || d.format == Format::BGRA8_UNORM ||
            d.format == Format::BGRX8_UNORM || d.format == Format::R10G10B10A2_UNORM ||
            (unsigned(format.hasRed) + unsigned(format.hasGreen) + unsigned(format.hasBlue) +
             unsigned(format.hasAlpha)) == 3)
            return FE_UNSUPPORTED;
        try
        {
            auto result = MAKE_RC_OBJ_PTR(ExportedMemory, m_device.Get());
            result->texture = texture;
            const auto required = m_context.device.getImageMemoryRequirements(texture->image);
            const auto code = exportMemory(texture->memory, required.size, result);
            if (NVRHI_FAILED(code))
                return code;
            *output = result.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc &)
        {
            return FE_OUT_OF_MEMORY;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }
    FRESULT createFence(uint64_t initialValue, IExternalFence **output) noexcept override
    {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        if (initialValue == UINT64_MAX)
            return FE_INVALID_ARGS;
        if (!m_supported)
            return FE_UNSUPPORTED;
        try
        {
            auto fence = MAKE_RC_OBJ_PTR(ExportedFence, m_device.Get(), m_context);
            fence->native = MAKE_RC_OBJ_PTR(NativeSemaphore, m_context);
            fence->desc.adapter = m_identity;
            auto timeline = vk::SemaphoreTypeCreateInfo()
                                .setSemaphoreType(vk::SemaphoreType::eTimeline)
                                .setInitialValue(initialValue);
            auto exportInfo = vk::ExportSemaphoreCreateInfo().setHandleTypes(semaphoreType).setPNext(&timeline);
            auto info = vk::SemaphoreCreateInfo().setPNext(&exportInfo);
            auto code = m_context.device.createSemaphore(&info, m_context.allocationCallbacks, &fence->native->semaphore);
            if (code != vk::Result::eSuccess)
                return status(code);
#ifdef _WIN32
            fence->desc.handleType = ExternalFenceHandleType::TimelineWin32;
            HANDLE handle = nullptr;
            auto handleInfo = vk::SemaphoreGetWin32HandleInfoKHR(fence->native->semaphore, semaphoreType);
            code = m_context.device.getSemaphoreWin32HandleKHR(&handleInfo, &handle);
            fence->desc.handle = reinterpret_cast<intptr_t>(handle);
#else
            fence->desc.handleType = ExternalFenceHandleType::TimelineFd;
            int handle = -1;
            auto handleInfo = vk::SemaphoreGetFdInfoKHR(fence->native->semaphore, semaphoreType);
            code = m_context.device.getSemaphoreFdKHR(&handleInfo, &handle);
            fence->desc.handle = handle;
#endif
            if (code != vk::Result::eSuccess)
                return status(code);
            *output = fence.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc &)
        {
            return FE_OUT_OF_MEMORY;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }
    FRESULT waitFence(CommandQueue queue, IExternalFence *fence, uint64_t value) noexcept override
    {
        try
        {
            return fenceOperation(queue, fence, value, false);
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }
    FRESULT signalFence(CommandQueue queue, IExternalFence *fence, uint64_t value) noexcept override
    {
        try
        {
            return fenceOperation(queue, fence, value, true);
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }
    FRESULT releaseResource(ICommandList *commands, IExternalMemory *memory) noexcept override
    {
        try
        {
            return transfer(commands, memory, ResourceStates::Common, false);
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }
    FRESULT acquireResource(ICommandList *commands, IExternalMemory *memory, ResourceStates state) noexcept override
    {
        try
        {
            return transfer(commands, memory, state, true);
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }
};
FRESULT nvrhiVulkanCreateGraphicsInteropDevice(nvrhi::IDevice *device, IGraphicsInteropDevice **output) noexcept
{
    if (!output)
        return FE_INVALID_ARGS;
    *output = nullptr;
    if (!device)
        return FE_INVALID_ARGS;
    auto native = static_cast<nvrhi::vulkan::IDevice *>(device->getNativeObject(ObjectTypes::Nvrhi_VK_Device));
    auto backend = object<Device>(native);
    if (!backend)
        return FE_UNSUPPORTED;
    try
    {
        *output = MAKE_RC_OBJ(GraphicsInteropDevice, backend);
        return FS_OK;
    }
    catch (const std::bad_alloc &)
    {
        return FE_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return FE_GENERIC_ERROR;
    }
}
} // namespace nvrhi::vulkan
