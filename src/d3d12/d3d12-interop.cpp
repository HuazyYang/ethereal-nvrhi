// SPDX-License-Identifier: MIT
#include <nvrhi/graphics-interop.h>
#include <nvrhi/core/foundation.h>
#include <nvrhi/d3d12.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

namespace nvrhi::d3d12
{
namespace
{
FRESULT status(HRESULT hr)
{
    if (SUCCEEDED(hr)) return FS_OK;
    if (hr == E_OUTOFMEMORY) return FE_OUT_OF_MEMORY;
    if (hr == E_INVALIDARG) return FE_INVALID_ARGS;
    if (hr == E_NOINTERFACE || hr == DXGI_ERROR_UNSUPPORTED) return FE_UNSUPPORTED;
    return FE_GENERIC_ERROR;
}
template<class T> T* object(IObject* source)
{
    T* result = nullptr;
    if (!source || NVRHI_FAILED(source->QueryInterface(NVRHI_IID_PPV_ARGS(&result)))) return nullptr;
    result->Release();
    return result;
}
bool sameAdapter(const AdapterIdentity& a, const AdapterIdentity& b)
{
    return a.luidValid && b.luidValid && !std::memcmp(a.luid, b.luid, sizeof(a.luid)) &&
        a.nodeMask && b.nodeMask && (a.nodeMask & b.nodeMask);
}
#define INTERFACES(CLASS, INTERFACE) \
    NVRHI_DECLARE_UUID_TRAITS(CLASS) \
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(CLASS) \
    NVRHI_IMPLEMENTS_INTERFACE(INTERFACE) \
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject) \
    NVRHI_IMPLEMENTS_CLASS(CLASS) \
    NVRHI_END_INTERFACE_TABLE()
NVRHI_CLASS_CLSID(Dx12Memory, "f9c639ab-b1c1-4195-9b9e-0d82417ab120")
class Dx12Memory final : public ObjectImpl<IExternalMemory>
{
public:
    INTERFACES(Dx12Memory, IExternalMemory)
    nvrhi::DeviceHandle device;
    AutoPtr<ID3D12Device> nativeDevice;
    BufferHandle buffer;
    TextureHandle texture;
    ExternalMemoryDesc desc;
    HANDLE handle = nullptr;
    ~Dx12Memory() { if (handle) CloseHandle(handle); }
    const ExternalMemoryDesc& getDesc() const noexcept override { return desc; }
    IBuffer* getBuffer() const noexcept override { return buffer; }
    ITexture* getTexture() const noexcept override { return texture; }
};
NVRHI_CLASS_CLSID(Dx12Fence, "cbd511d2-22d2-4580-9d7b-73cb8b6aa7c6")
class Dx12Fence final : public ObjectImpl<IExternalFence>
{
public:
    INTERFACES(Dx12Fence, IExternalFence)
    nvrhi::DeviceHandle device;
    AutoPtr<ID3D12Device> nativeDevice;
    AutoPtr<ID3D12Fence> fence;
    ExternalFenceDesc desc;
    HANDLE handle = nullptr;
    ~Dx12Fence() { if (handle) CloseHandle(handle); }
    const ExternalFenceDesc& getDesc() const noexcept override { return desc; }
};
class GraphicsInterop final : public ObjectImpl<IGraphicsInteropDevice>
{
    nvrhi::DeviceHandle m_device;
    AutoPtr<ID3D12Device> m_native;
    AdapterIdentity m_identity;
    struct Completion
    {
        AutoPtr<ID3D12Fence> fence;
        uint64_t value = 0;
    } m_completion[3];
    struct Pending { ExternalFenceHandle fence; uint32_t queue; uint64_t value; };
    std::vector<Pending> m_pending;
    std::mutex m_mutex;

    FRESULT exportResource(IRHIObject* source, IBuffer* buffer, ITexture* texture, IExternalMemory** output)
    {
        auto* resource = static_cast<ID3D12Resource*>(source->getNativeObject(ObjectTypes::D3D12_Resource));
        if (!resource) return FE_INVALID_ARGS;
        AutoPtr<ID3D12Device> owner;
        if (FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) || owner != m_native) return FE_INVALID_ARGS;
        D3D12_HEAP_PROPERTIES properties{};
        D3D12_HEAP_FLAGS flags{};
        if (FAILED(resource->GetHeapProperties(&properties, &flags)) || properties.Type != D3D12_HEAP_TYPE_DEFAULT ||
            !(flags & D3D12_HEAP_FLAG_SHARED) || (flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)) return FE_UNSUPPORTED;
        const auto nativeDesc = resource->GetDesc();
        const auto allocation = m_native->GetResourceAllocationInfo(0, 1, &nativeDesc);
        if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX) return FE_UNSUPPORTED;
        auto memory = MAKE_RC_OBJ_PTR(Dx12Memory);
        memory->device = m_device;
        memory->nativeDevice = m_native;
        memory->buffer = buffer;
        memory->texture = texture;
        const auto hr = m_native->CreateSharedHandle(resource, nullptr, GENERIC_ALL, nullptr, &memory->handle);
        if (FAILED(hr)) return status(hr);
        memory->desc.handleType = ExternalMemoryHandleType::D3D12Resource;
        memory->desc.handle = reinterpret_cast<intptr_t>(memory->handle);
        memory->desc.adapter = m_identity;
        memory->desc.allocationSize = allocation.SizeInBytes;
        memory->desc.dedicated = true;
        memory->desc.sourceOwner = memory;
        *output = memory.Detach();
        return FS_OK;
    }
    FRESULT fenceOperation(CommandQueue type, IExternalFence* external, uint64_t value, bool signal)
    {
        auto* fence = object<Dx12Fence>(external);
        if (type >= CommandQueue::Count || !fence || fence->nativeDevice != m_native || value == UINT64_MAX)
            return FE_INVALID_ARGS;
        auto* queue = static_cast<ID3D12CommandQueue*>(m_device->getNativeQueue(ObjectTypes::D3D12_CommandQueue, type));
        if (!queue) return FE_UNSUPPORTED;
        std::lock_guard<std::mutex> guard(m_mutex);
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), [&](const Pending& pending) {
            return m_completion[pending.queue].fence->GetCompletedValue() >= pending.value;
        }), m_pending.end());
        auto& completion = m_completion[uint32_t(type)];
        if (!completion.fence)
        {
            const auto hr = m_native->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion.fence));
            if (FAILED(hr)) return status(hr);
        }
        if (completion.value == UINT64_MAX - 1) return FE_GENERIC_ERROR;
        // Allocate and retain before enqueuing either operation: allocation failure
        // must never leave a native queue using an unretained external fence.
        m_pending.push_back({external, uint32_t(type), ++completion.value});
        auto hr = signal ? queue->Signal(fence->fence, value) : queue->Wait(fence->fence, value);
        if (FAILED(hr)) { m_pending.pop_back(); return status(hr); }
        hr = queue->Signal(completion.fence, completion.value);
        // If retirement signaling fails, preserve the pending reference until destruction.
        if (FAILED(hr)) m_pending.back().value = UINT64_MAX;
        return status(hr);
    }
    FRESULT transfer(nvrhi::ICommandList* commands, IExternalMemory* external, ResourceStates state, bool acquiring)
    {
        auto* memory = object<Dx12Memory>(external);
        if (!commands || !memory || memory->nativeDevice != m_native || !commands->getDevice() ||
            static_cast<ID3D12Device*>(commands->getDevice()->getNativeObject(ObjectTypes::D3D12_Device)) != m_native ||
            !commands->getNativeObject(ObjectTypes::D3D12_GraphicsCommandList)) return FE_INVALID_ARGS;
        if (state == ResourceStates::Unknown) return FE_INVALID_ARGS;
        // CUDA returns shared D3D12 resources in COMMON. Start tracking that
        // external state in this list, which may have no earlier resource use.
        if (acquiring)
        {
            if (memory->buffer) commands->beginTrackingBufferState(memory->buffer, ResourceStates::Common);
            else commands->beginTrackingTextureState(memory->texture, AllSubresources, ResourceStates::Common);
        }
        if (memory->buffer) commands->setBufferState(memory->buffer, state);
        else commands->setTextureState(memory->texture, AllSubresources, state);
        commands->commitBarriers();
        return FS_OK;
    }
public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GraphicsInterop)
    NVRHI_IMPLEMENTS_INTERFACE(IGraphicsInteropDevice)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    GraphicsInterop(nvrhi::IDevice* device, ID3D12Device* native) : m_device(device), m_native(native)
    {
        const auto luid = native->GetAdapterLuid();
        std::memcpy(m_identity.luid, &luid, sizeof(luid));
        m_identity.luidValid = true;
        m_identity.nodeMask = 1;
    }
    ~GraphicsInterop()
    {
        // Queue operations retain fences until native GPU completion. Destruction
        // waits only when this wrapper still owns outstanding fence submissions.
        if (!m_pending.empty()) m_device->waitForIdle();
    }
    const AdapterIdentity& getAdapterIdentity() const noexcept override { return m_identity; }
    FRESULT queryRoute(GraphicsAPI api, const AdapterIdentity& peer, InteropRouteCapabilities& caps) const noexcept override
    {
        caps = {};
        caps.sameAdapter = sameAdapter(m_identity, peer);
        if (api != GraphicsAPI::CUDA || !caps.sameAdapter) return FE_UNSUPPORTED;
        caps.bufferSharing = caps.textureSharing = caps.timelineFences = caps.explicitOwnershipTransfer = true;
        return FS_OK;
    }
    FRESULT exportBuffer(IBuffer* buffer, IExternalMemory** output) noexcept override
    {
        if (!output) return FE_INVALID_ARGS;
        *output = nullptr;
        if (!buffer) return FE_INVALID_ARGS;
        const auto& d = buffer->getDesc();
        if (d.cpuAccess != CpuAccessMode::None || d.isVirtual || d.isVolatile || d.keepInitialState ||
            !(d.sharedResourceFlags & SharedResourceFlags::Shared) ||
            (d.sharedResourceFlags & SharedResourceFlags::Shared_CrossAdapter) != 0) return FE_UNSUPPORTED;
        try { return exportResource(buffer, buffer, nullptr, output); }
        catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
        catch (...) { return FE_GENERIC_ERROR; }
    }
    FRESULT exportTexture(ITexture* texture, IExternalMemory** output) noexcept override
    {
        if (!output) return FE_INVALID_ARGS;
        *output = nullptr;
        if (!texture) return FE_INVALID_ARGS;
        const auto& d = texture->getDesc();
        const auto& format = getFormatInfo(d.format);
        const unsigned channels = unsigned(format.hasRed) + unsigned(format.hasGreen) +
            unsigned(format.hasBlue) + unsigned(format.hasAlpha);
        const unsigned componentBytes = channels ? format.bytesPerBlock / channels : 0;
        // CUDA arrays require equally sized channels and cannot reinterpret packed,
        // BGR, three-channel, compressed, depth, or sRGB graphics formats.
        if ((channels != 1 && channels != 2 && channels != 4) || format.bytesPerBlock % channels ||
            (componentBytes != 1 && componentBytes != 2 && componentBytes != 4) ||
            (format.kind == FormatKind::Float && componentBytes == 1) || format.isSRGB ||
            d.format == Format::BGRA8_UNORM || d.format == Format::BGRX8_UNORM || d.format == Format::R10G10B10A2_UNORM)
            return FE_UNSUPPORTED;
        if (d.isVirtual || d.isTiled || d.keepInitialState || d.sampleCount != 1 || format.hasDepth || format.hasStencil ||
            format.blockSize != 1 || !format.bytesPerBlock ||
            !(d.sharedResourceFlags & SharedResourceFlags::Shared) ||
            (d.sharedResourceFlags & SharedResourceFlags::Shared_CrossAdapter) != 0) return FE_UNSUPPORTED;
        try { return exportResource(texture, nullptr, texture, output); }
        catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
        catch (...) { return FE_GENERIC_ERROR; }
    }
    FRESULT createFence(uint64_t initial, IExternalFence** output) noexcept override
    {
        if (!output) return FE_INVALID_ARGS;
        *output = nullptr;
        if (initial == UINT64_MAX) return FE_INVALID_ARGS;
        try
        {
            auto fence = MAKE_RC_OBJ_PTR(Dx12Fence);
            fence->device = m_device;
            fence->nativeDevice = m_native;
            auto hr = m_native->CreateFence(initial, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence->fence));
            if (FAILED(hr)) return status(hr);
            hr = m_native->CreateSharedHandle(fence->fence, nullptr, GENERIC_ALL, nullptr, &fence->handle);
            if (FAILED(hr)) return status(hr);
            fence->desc.handleType = ExternalFenceHandleType::D3D12Fence;
            fence->desc.handle = reinterpret_cast<intptr_t>(fence->handle);
            fence->desc.adapter = m_identity;
            fence->desc.sourceOwner = fence;
            *output = fence.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
        catch (...) { return FE_GENERIC_ERROR; }
    }
    FRESULT waitFence(CommandQueue queue, IExternalFence* fence, uint64_t value) noexcept override
    {
        try { return fenceOperation(queue, fence, value, false); }
        catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
        catch (...) { return FE_GENERIC_ERROR; }
    }
    FRESULT signalFence(CommandQueue queue, IExternalFence* fence, uint64_t value) noexcept override
    {
        try { return fenceOperation(queue, fence, value, true); }
        catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
        catch (...) { return FE_GENERIC_ERROR; }
    }
    FRESULT releaseResource(nvrhi::ICommandList* commands, IExternalMemory* memory) noexcept override
    { return transfer(commands, memory, ResourceStates::Common, false); }
    FRESULT acquireResource(nvrhi::ICommandList* commands, IExternalMemory* memory, ResourceStates state) noexcept override
    { return transfer(commands, memory, state, true); }
};
#undef INTERFACES
}
FRESULT nvrhiD3D12CreateGraphicsInteropDevice(nvrhi::IDevice* device, IGraphicsInteropDevice** output) noexcept
{
    if (!output) return FE_INVALID_ARGS;
    *output = nullptr;
    if (!device) return FE_INVALID_ARGS;
    if (device->getGraphicsAPI() != GraphicsAPI::D3D12) return FE_UNSUPPORTED;
    auto* native = static_cast<ID3D12Device*>(device->getNativeObject(ObjectTypes::D3D12_Device));
    if (!native || native->GetNodeCount() != 1) return FE_UNSUPPORTED;
    try { *output = MAKE_RC_OBJ(GraphicsInterop, device, native); return FS_OK; }
    catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
    catch (...) { return FE_GENERIC_ERROR; }
}
}
