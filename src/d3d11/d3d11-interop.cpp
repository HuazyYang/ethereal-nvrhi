// SPDX-License-Identifier: MIT
#include <nvrhi/graphics-interop.h>
#include <nvrhi/core/foundation.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

namespace nvrhi::d3d11
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
NVRHI_CLASS_CLSID(Dx11Memory, "c6fcc264-df53-4b83-84b4-7895bc9b418f")
class Dx11Memory final : public ObjectImpl<IExternalMemory>
{
public:
    INTERFACES(Dx11Memory, IExternalMemory)
    DeviceHandle device;
    AutoPtr<ID3D11Device> nativeDevice;
    TextureHandle texture;
    ExternalMemoryDesc desc;
    // KMT handles must not be closed. The retained texture/device own their lifetime.
    const ExternalMemoryDesc& getDesc() const noexcept override { return desc; }
    IBuffer* getBuffer() const noexcept override { return nullptr; }
    ITexture* getTexture() const noexcept override { return texture; }
};
NVRHI_CLASS_CLSID(Dx11Fence, "4a6b56b5-11a7-4207-9a96-b7daf3ad6a61")
class Dx11Fence final : public ObjectImpl<IExternalFence>
{
public:
    INTERFACES(Dx11Fence, IExternalFence)
    DeviceHandle device;
    AutoPtr<ID3D11Device> nativeDevice;
    AutoPtr<ID3D11Fence> fence;
    ExternalFenceDesc desc;
    HANDLE handle = nullptr;
    ~Dx11Fence() { if (handle) CloseHandle(handle); }
    const ExternalFenceDesc& getDesc() const noexcept override { return desc; }
};
class GraphicsInterop final : public ObjectImpl<IGraphicsInteropDevice>
{
    DeviceHandle m_device;
    AutoPtr<ID3D11Device> m_native;
    AutoPtr<ID3D11Device5> m_device5;
    AutoPtr<ID3D11DeviceContext4> m_context;
    AdapterIdentity m_identity;
    AutoPtr<ID3D11Fence> m_completion;
    uint64_t m_value = 0;
    struct Pending { ExternalFenceHandle fence; uint64_t value; };
    std::vector<Pending> m_pending;
    std::mutex m_mutex;

    FRESULT fenceOperation(CommandQueue queue, IExternalFence* external, uint64_t value, bool signal)
    {
        if (queue != CommandQueue::Graphics) return FE_UNSUPPORTED;
        auto* fence = object<Dx11Fence>(external);
        if (!fence || fence->nativeDevice != m_native || value == UINT64_MAX) return FE_INVALID_ARGS;
        std::lock_guard<std::mutex> guard(m_mutex);
        if (!m_completion)
        {
            const auto hr = m_device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_completion));
            if (FAILED(hr)) return status(hr);
        }
        const auto completed = m_completion->GetCompletedValue();
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), [&](const Pending& item) {
            return completed >= item.value;
        }), m_pending.end());
        if (m_value == UINT64_MAX - 1) return FE_GENERIC_ERROR;
        m_pending.push_back({external, ++m_value});
        auto hr = signal ? m_context->Signal(fence->fence, value) : m_context->Wait(fence->fence, value);
        if (FAILED(hr)) { m_pending.pop_back(); return status(hr); }
        hr = m_context->Signal(m_completion, m_value);
        if (FAILED(hr)) m_pending.back().value = UINT64_MAX;
        // D3D11 buffers commands internally; flush submits waits/signals without a CPU wait.
        m_context->Flush();
        return status(hr);
    }
    FRESULT transfer(ICommandList* commands, IExternalMemory* external, ResourceStates state)
    {
        auto* memory = object<Dx11Memory>(external);
        if (!commands || !memory || memory->nativeDevice != m_native || !commands->getDevice() ||
            static_cast<ID3D11Device*>(commands->getDevice()->getNativeObject(ObjectTypes::D3D11_Device)) != m_native ||
            state == ResourceStates::Unknown) return FE_INVALID_ARGS;
        // Remove bound views and invalidate NVRHI's cached state before another API
        // accesses this texture. Native timeline fences establish GPU ordering.
        commands->clearState();
        return FS_OK;
    }
public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GraphicsInterop)
    NVRHI_IMPLEMENTS_INTERFACE(IGraphicsInteropDevice)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    GraphicsInterop(IDevice* device, ID3D11Device* native, ID3D11Device5* device5,
                    ID3D11DeviceContext4* context, const AdapterIdentity& identity)
        : m_device(device), m_native(native), m_device5(device5), m_context(context), m_identity(identity) { }
    ~GraphicsInterop()
    {
        if (!m_pending.empty()) { m_context->Flush(); m_device->waitForIdle(); }
    }
    const AdapterIdentity& getAdapterIdentity() const noexcept override { return m_identity; }
    FRESULT queryRoute(GraphicsAPI api, const AdapterIdentity& peer, InteropRouteCapabilities& caps) const noexcept override
    {
        caps = {};
        caps.sameAdapter = sameAdapter(m_identity, peer);
        if (api != GraphicsAPI::CUDA || !caps.sameAdapter) return FE_UNSUPPORTED;
        caps.textureSharing = caps.timelineFences = true;
        return FS_OK;
    }
    FRESULT exportBuffer(IBuffer*, IExternalMemory** output) noexcept override
    {
        if (!output) return FE_INVALID_ARGS;
        *output = nullptr;
        return FE_UNSUPPORTED;
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
            format.blockSize != 1 || !format.bytesPerBlock || d.sharedResourceFlags != SharedResourceFlags::Shared ||
            (d.dimension != TextureDimension::Texture2D && d.dimension != TextureDimension::Texture2DArray))
            return FE_UNSUPPORTED;
        try
        {
            auto* resource = static_cast<ID3D11Resource*>(texture->getNativeObject(ObjectTypes::D3D11_Resource));
            if (!resource) return FE_INVALID_ARGS;
            AutoPtr<ID3D11Device> owner;
            resource->GetDevice(&owner);
            if (owner != m_native) return FE_INVALID_ARGS;
            AutoPtr<ID3D11Texture2D> nativeTexture;
            AutoPtr<IDXGIResource> dxgiResource;
            if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&nativeTexture))) ||
                FAILED(resource->QueryInterface(IID_PPV_ARGS(&dxgiResource)))) return FE_UNSUPPORTED;
            D3D11_TEXTURE2D_DESC nativeDesc{};
            nativeTexture->GetDesc(&nativeDesc);
            if (nativeDesc.Usage != D3D11_USAGE_DEFAULT || nativeDesc.CPUAccessFlags ||
                !(nativeDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED) ||
                (nativeDesc.MiscFlags & (D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE)))
                return FE_UNSUPPORTED;
            HANDLE shared = nullptr;
            const auto hr = dxgiResource->GetSharedHandle(&shared);
            if (FAILED(hr) || !shared) return FAILED(hr) ? status(hr) : FE_UNSUPPORTED;
            auto memory = MAKE_RC_OBJ_PTR(Dx11Memory);
            memory->device = m_device;
            memory->nativeDevice = m_native;
            memory->texture = texture;
            memory->desc.handleType = ExternalMemoryHandleType::D3D11ResourceKmt;
            memory->desc.handle = reinterpret_cast<intptr_t>(shared);
            memory->desc.adapter = m_identity;
            uint64_t size = 0;
            uint32_t width = nativeDesc.Width, height = nativeDesc.Height;
            for (uint32_t mip = 0; mip < nativeDesc.MipLevels; ++mip)
            {
                size += uint64_t(width) * height * nativeDesc.ArraySize * format.bytesPerBlock;
                width = (std::max)(1u, width / 2);
                height = (std::max)(1u, height / 2);
            }
            memory->desc.allocationSize = size;
            memory->desc.dedicated = true;
            memory->desc.sourceOwner = memory;
            *output = memory.Detach();
            return FS_OK;
        }
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
            auto fence = MAKE_RC_OBJ_PTR(Dx11Fence);
            fence->device = m_device;
            fence->nativeDevice = m_native;
            auto hr = m_device5->CreateFence(initial, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence->fence));
            if (FAILED(hr)) return status(hr);
            hr = fence->fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence->handle);
            if (FAILED(hr)) return status(hr);
            fence->desc.handleType = ExternalFenceHandleType::D3D11Fence;
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
    FRESULT releaseResource(ICommandList* commands, IExternalMemory* memory) noexcept override
    { return transfer(commands, memory, ResourceStates::Common); }
    FRESULT acquireResource(ICommandList* commands, IExternalMemory* memory, ResourceStates state) noexcept override
    { return transfer(commands, memory, state); }
};
#undef INTERFACES
}
FRESULT nvrhiD3D11CreateGraphicsInteropDevice(IDevice* device, IGraphicsInteropDevice** output) noexcept
{
    if (!output) return FE_INVALID_ARGS;
    *output = nullptr;
    if (!device) return FE_INVALID_ARGS;
    if (device->getGraphicsAPI() != GraphicsAPI::D3D11) return FE_UNSUPPORTED;
    auto* native = static_cast<ID3D11Device*>(device->getNativeObject(ObjectTypes::D3D11_Device));
    auto* context = static_cast<ID3D11DeviceContext*>(device->getNativeObject(ObjectTypes::D3D11_DeviceContext));
    if (!native || !context) return FE_UNSUPPORTED;
    AutoPtr<ID3D11Device5> device5;
    AutoPtr<ID3D11DeviceContext4> context4;
    AutoPtr<IDXGIDevice> dxgi;
    AutoPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(native->QueryInterface(IID_PPV_ARGS(&device5))) ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(&context4))) ||
        FAILED(native->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) return FE_UNSUPPORTED;
    AdapterIdentity identity;
    std::memcpy(identity.luid, &desc.AdapterLuid, sizeof(desc.AdapterLuid));
    identity.luidValid = true;
    identity.nodeMask = 1;
    try { *output = MAKE_RC_OBJ(GraphicsInterop, device, native, device5, context4, identity); return FS_OK; }
    catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
    catch (...) { return FE_GENERIC_ERROR; }
}
}
