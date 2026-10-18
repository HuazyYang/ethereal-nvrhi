// SPDX-License-Identifier: MIT
#pragma once
#include <nvrhi/interop.h>

namespace nvrhi
{
struct InteropRouteCapabilities
{
    bool sameAdapter = false;
    bool bufferSharing = false;
    bool textureSharing = false;
    bool timelineFences = false;
    bool explicitOwnershipTransfer = false;
};

NVRHI_IID(IExternalMemory, "8bc225eb-846d-460c-9740-450432e021da")
struct IExternalMemory : IRHIObject
{
    NVRHI_DECLARE_UUID_TRAITS(IExternalMemory)
    // The exported handle is borrowed from this object and remains valid until
    // this object is released. CUDA import duplicates owned OS handles and retains
    // sourceOwner. KMT handles remain borrowed and are never closed.
    virtual const ExternalMemoryDesc &getDesc() const noexcept = 0;
    virtual IBuffer *getBuffer() const noexcept = 0;
    virtual ITexture *getTexture() const noexcept = 0;
};
using ExternalMemoryHandle = AutoPtr<IExternalMemory>;

// Graphics-side companion to IInteropDevice. Shared resources remain ordinary
// NVRHI buffers/textures. Use SharedResourceFlags::Shared, keepInitialState=false,
// and GPU-only, nonvirtual allocations. No method performs host staging.
NVRHI_IID(IGraphicsInteropDevice, "991675d5-aeeb-4a02-9f48-802d20db7734")
struct IGraphicsInteropDevice : IRHIObject
{
    NVRHI_DECLARE_UUID_TRAITS(IGraphicsInteropDevice)
    virtual const AdapterIdentity &getAdapterIdentity() const noexcept = 0;
    // Currently exposes same-adapter graphics-to-CUDA routes. FE_UNSUPPORTED
    // explicitly rejects other APIs/adapters or missing native extensions.
    // Resource-specific restrictions are validated by exportBuffer/Texture.
    virtual FRESULT queryRoute(GraphicsAPI peerAPI, const AdapterIdentity &peer,
                               InteropRouteCapabilities &capabilities) const noexcept = 0;
    virtual FRESULT exportBuffer(IBuffer *buffer, IExternalMemory **memory) noexcept = 0;
    virtual FRESULT exportTexture(ITexture *texture, IExternalMemory **memory) noexcept = 0;
    virtual FRESULT createFence(uint64_t initialValue, IExternalFence **fence) noexcept = 0;
    // These enqueue GPU operations immediately. Serialize calls with ordinary
    // command-list submissions to the same queue. Both sides use monotonically
    // increasing timeline values. The implementation retains submitted fences.
    virtual FRESULT waitFence(CommandQueue queue, IExternalFence *fence, uint64_t value) noexcept = 0;
    virtual FRESULT signalFence(CommandQueue queue, IExternalFence *fence, uint64_t value) noexcept = 0;
    // Whole-resource ownership transfer. Release must be the final use before
    // submission + signalFence; acquire follows waitFence and precedes graphics
    // access. Vulkan transitions all subresources through GENERAL and EXTERNAL.
    // Calls record into an open command list belonging to this device.
    virtual FRESULT releaseResource(ICommandList *commands, IExternalMemory *memory) noexcept = 0;
    virtual FRESULT acquireResource(ICommandList *commands, IExternalMemory *memory, ResourceStates state) noexcept = 0;
};
using GraphicsInteropDeviceHandle = AutoPtr<IGraphicsInteropDevice>;

// Link nvrhi::interop. Its configured backends are selected without native SDK
// types in application code; unsupported/disabled backends return FE_UNSUPPORTED.
NVRHI_C_API FRESULT nvrhiCreateGraphicsInteropDevice(IDevice *device, IGraphicsInteropDevice **interop) noexcept;
} // namespace nvrhi
namespace nvrhi::d3d11
{
NVRHI_C_API FRESULT nvrhiD3D11CreateGraphicsInteropDevice(IDevice *, IGraphicsInteropDevice **) noexcept;
}
namespace nvrhi::d3d12
{
NVRHI_C_API FRESULT nvrhiD3D12CreateGraphicsInteropDevice(IDevice *, IGraphicsInteropDevice **) noexcept;
}
namespace nvrhi::vulkan
{
NVRHI_C_API FRESULT nvrhiVulkanCreateGraphicsInteropDevice(nvrhi::IDevice *, IGraphicsInteropDevice **) noexcept;
}
