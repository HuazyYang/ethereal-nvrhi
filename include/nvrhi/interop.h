// SPDX-License-Identifier: MIT
#pragma once
#include <nvrhi/nvrhi.h>

namespace nvrhi {
    struct AdapterIdentity {
        uint8_t uuid[16]{};
        uint8_t luid[8]{};
        uint32_t nodeMask = 0;
        bool uuidValid = false;
        bool luidValid = false;
    };
    enum class ExternalMemoryHandleType : uint32_t {
        OpaqueFd, OpaqueWin32, OpaqueWin32Kmt,
        D3D11Resource, D3D11ResourceKmt, D3D12Heap, D3D12Resource
    };
    enum class ExternalFenceHandleType : uint32_t {
        BinaryFd, BinaryWin32, TimelineFd, TimelineWin32,
        D3D11Fence, D3D12Fence, D3D11KeyedMutex, D3D11KeyedMutexKmt
    };
    struct ExternalMemoryDesc {
        ExternalMemoryHandleType handleType = ExternalMemoryHandleType::OpaqueFd;
        // Borrowed OS handle. File descriptor zero is valid. The import duplicates
        // owned OS handles; ownership of the supplied handle never transfers.
        intptr_t handle = -1;
        AdapterIdentity adapter;
        uint64_t allocationSize = 0;
        uint64_t offset = 0;
        bool dedicated = false;
        // Required for KMT handles, which do not hold a reference to the resource.
        IRHIObject* sourceOwner = nullptr;
    };
    struct ExternalFenceDesc {
        ExternalFenceHandleType handleType = ExternalFenceHandleType::BinaryFd;
        intptr_t handle = -1;
        AdapterIdentity adapter;
        IRHIObject* sourceOwner = nullptr;
    };
    struct PeerCapabilities {
        bool sameDevice = false;
        bool nativeCopy = false;
        bool nativeAtomics = false;
    };
    NVRHI_IID(IExternalFence, "d937624f-f822-4f0e-9188-44f96c47a2bd")
    struct IExternalFence : IRHIObject {
        NVRHI_DECLARE_UUID_TRAITS(IExternalFence)
        virtual const ExternalFenceDesc& getDesc() const noexcept = 0;
    };
    using ExternalFenceHandle = AutoPtr<IExternalFence>;

    // CUDA-centered external memory and synchronization. Imported resources are
    // ordinary NVRHI resources. No operation silently stages through host memory.
    NVRHI_IID(IInteropDevice, "72867416-53ea-4208-aee4-d2c43ed73ef1")
    struct IInteropDevice : IRHIObject {
        NVRHI_DECLARE_UUID_TRAITS(IInteropDevice)
        virtual const AdapterIdentity& getAdapterIdentity() const noexcept = 0;
        virtual FRESULT importBuffer(const ExternalMemoryDesc& external, const BufferDesc& desc, IBuffer** buffer) noexcept = 0;
        virtual FRESULT importTexture(const ExternalMemoryDesc& external, const TextureDesc& desc, ITexture** texture) noexcept = 0;
        virtual FRESULT importFence(const ExternalFenceDesc& desc, IExternalFence** fence) noexcept = 0;
        // Record native GPU waits/signals in the supplied device command list.
        // Binary fences require value=0. Keyed mutex values are acquire/release keys.
        virtual FRESULT waitFence(ICommandList* commands, IExternalFence* fence, uint64_t value, uint32_t timeoutMs = UINT32_MAX) noexcept = 0;
        virtual FRESULT signalFence(ICommandList* commands, IExternalFence* fence, uint64_t value) noexcept = 0;
        virtual FRESULT queryPeerCapabilities(IDevice* peer, PeerCapabilities& capabilities) const noexcept = 0;
        // Executes after work already submitted to sourceQueue when this command
        // is replayed; callers must submit the source writes before this list.
        virtual FRESULT copyPeerBuffer(ICommandList* commands, IBuffer* destination, uint64_t destinationOffset,
            IBuffer* source, uint64_t sourceOffset, uint64_t bytes, CommandQueue sourceQueue = CommandQueue::Graphics) noexcept = 0;
    };
    using InteropDeviceHandle = AutoPtr<IInteropDevice>;
}
namespace nvrhi::cuda {
    NVRHI_C_API FRESULT nvrhiCudaCreateInteropDevice(IDevice* device, IInteropDevice** interop) noexcept;
}
