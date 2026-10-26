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

// The CUDA backend (ADR 0011): compute through the CUDA driver API and ray tracing through OptiX 9, fed with
// Slang code compiled offline into NVRHI shader containers (<nvrhi/shader-reflection.h>). Create the device with
// cuda::IGraphicsFactory::createDevice (cuda::createGraphicsFactory, ADR 0014).
//
// - Each CommandQueue is one CUDA stream. Command lists record; executeCommandLists replays them on the stream
//   of the queue.
// - Graphics pipelines, framebuffers, input layouts, meshlets and sampler feedback are not supported.
// - A device created with DeviceDesc::graphicsDevice also implements IInteropDevice (<nvrhi/interop.h>).
//
// This header does not include the CUDA headers: native handles are returned as NativeObject
// (see the CUDA_* object types in <nvrhi/common/resource.h>).

#include <nvrhi/nvrhi.h>
#include <nvrhi/factory.h>
#include <nvrhi/swapchain.h>

namespace nvrhi
{
    namespace ObjectTypes
    {
        constexpr ObjectType Nvrhi_CUDA_Device          = 0x00040101;
        constexpr ObjectType Nvrhi_CUDA_CommandList     = 0x00040102;
    };
}

namespace nvrhi::cuda
{
    // Work that the application records into a command list and that runs on the CUDA stream of the queue when
    // the command list executes: launches of its own kernels, CUDA libraries, ... Implemented by the application;
    // the command list keeps a reference until the command list instance finishes executing.
    NVRHI_IID(INativeWork, "1d5510f5-ac28-4321-a1ba-3360deac8144")
    struct INativeWork : IRHIObject
    {
        NVRHI_DECLARE_UUID_TRAITS(INativeWork)
        // Called on the thread that executes the command list, with the CUDA context current. 'stream' is the
        // CUstream. Work must be enqueued on that stream, not run synchronously.
        virtual void execute(NativeObject stream) noexcept = 0;
    };

    typedef AutoPtr<INativeWork> NativeWorkHandle;

    NVRHI_IID(ICommandList, "e4b09653-8459-47ba-8329-fedaf791c70a")
    struct ICommandList : nvrhi::ICommandList
    {
        NVRHI_DECLARE_UUID_TRAITS(ICommandList)
        // Records a call of work->execute(stream) at this point of the command list.
        virtual void enqueueNativeWork(INativeWork* work) noexcept = 0;
    };

    typedef AutoPtr<ICommandList> CommandListHandle;

    NVRHI_IID(IDevice, "8655f424-f566-46e9-9baf-d61175113744")
    struct IDevice : nvrhi::IDevice
    {
        NVRHI_DECLARE_UUID_TRAITS(IDevice)
        // The CUDA device ordinal.
        virtual int32_t getDeviceOrdinal() noexcept = 0;
        // The compute capability: major * 10 + minor (75 for sm_75).
        virtual uint32_t getComputeCapability() noexcept = 0;
        // True if the OptiX device context exists: ray tracing pipelines and acceleration structures work.
        virtual bool isOptixAvailable() noexcept = 0;
        // The last command list instance (as returned by executeCommandLists) that finished on the queue.
        virtual uint64_t queueGetCompletedInstance(CommandQueue queue) noexcept = 0;

        // Values for Slang's DescriptorHandle<T> (Texture3D<float>.Handle, Sampler2D<float4>.Handle,
        // RWTexture3D<float>.Handle, ...), which the CUDA target lowers to the CUDA texture and surface objects.
        // A texture handle reads the subresources with the sampler's filtering and addressing (nullptr: point
        // sampling, clamp) and normalized coordinates; a surface handle reads and writes one mip level. A handle
        // stays valid while the texture lives. Both return 0 on failure.
        virtual uint64_t getTextureHandle(nvrhi::ITexture* texture, nvrhi::ISampler* sampler,
            const TextureSubresourceSet& subresources, Format format) noexcept = 0;
        virtual uint64_t getSurfaceHandle(nvrhi::ITexture* texture, MipLevel mipLevel) noexcept = 0;
    };

    typedef AutoPtr<IDevice> DeviceHandle;

    struct DeviceDesc
    {
        IMessageCallback* messageCallback = nullptr;    // the device keeps a reference to it

        // A D3D12 or Vulkan device to pair with. The CUDA device on the same adapter is selected (by LUID, or by
        // the Vulkan device UUID), and the device implements IInteropDevice for it. The CUDA device keeps a
        // reference to it. Optional; D3D11 devices can select the adapter but get no interop (ADR 0013).
        nvrhi::IDevice* graphicsDevice = nullptr;

        // Without graphicsDevice: the CUDA device ordinal (-1: device 0).
        int32_t deviceOrdinal = -1;

        uint32_t maxTimerQueries = 256;

        // Create the OptiX device context if NVRHI was built with OptiX and the driver supports it.
        bool enableOptix = true;
        // OptiX validation mode (OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_ALL); slow.
        bool enableOptixValidation = false;

        DeviceDesc& setMessageCallback(IMessageCallback* value) { messageCallback = value; return *this; }
        DeviceDesc& setGraphicsDevice(nvrhi::IDevice* value) { graphicsDevice = value; return *this; }
        DeviceDesc& setDeviceOrdinal(int32_t value) { deviceOrdinal = value; return *this; }
        DeviceDesc& setMaxTimerQueries(uint32_t value) { maxTimerQueries = value; return *this; }
        DeviceDesc& setEnableOptix(bool value) { enableOptix = value; return *this; }
        DeviceDesc& setEnableOptixValidation(bool value) { enableOptixValidation = value; return *this; }
    };

    static_assert(offsetof(DeviceDesc, graphicsDevice) == 8);
    static_assert(offsetof(DeviceDesc, deviceOrdinal) == 16);
    static_assert(offsetof(DeviceDesc, enableOptix) == 24);
    static_assert(sizeof(DeviceDesc) == 32);

    struct GraphicsFactoryDesc
    {
        // Receives the factory's own messages. Optional; the factory keeps a reference to it.
        IMessageCallback* messageCallback = nullptr;
        // The CUDA driver library. If empty: nvcuda.dll on Windows, libcuda.so.1 elsewhere.
        string driverLibraryName;

        GraphicsFactoryDesc& setMessageCallback(IMessageCallback* value) { messageCallback = value; return *this; }
    };

    static_assert(sizeof(GraphicsFactoryDesc) == 8 + sizeof(string));

    // The CUDA swap chain has no window (SwapChainDesc::window is null) and presents nothing: it rotates
    // SwapChainDesc::bufferCount back buffers (CUDA textures, with isUAV) and paces the frames. present() sets an
    // event query on the graphics queue for the frame slot and moves to the next slot; beginFrame() waits until the
    // stream work of the slot that is reused has finished. present() also keeps at most maxFramesInFlight frames in
    // flight.
    NVRHI_IID(IGraphicsSwapChain, "46e5efd4-75eb-477c-93c6-5cb7bf92c92a")
    struct IGraphicsSwapChain : nvrhi::IGraphicsSwapChain
    {
        NVRHI_DECLARE_UUID_TRAITS(IGraphicsSwapChain)
        // The event query that back buffer 'index' waits for in beginFrame(), not AddRef'd, or nullptr if the back
        // buffer has not been presented yet.
        virtual IEventQuery* getFrameQuery(uint32_t index) noexcept = 0;
    };

    typedef AutoPtr<IGraphicsSwapChain> GraphicsSwapChainHandle;

    NVRHI_IID(IGraphicsFactory, "0be3ec10-1dee-4df5-b9f7-d85778a8b8f4")
    struct IGraphicsFactory : nvrhi::IGraphicsFactory
    {
        NVRHI_DECLARE_UUID_TRAITS(IGraphicsFactory)

        // Each returns FS_OK and a new reference in the last parameter, or an FE_* code and nullptr.

        // Creates the CUDA device of desc (the device ordinal, or the device paired with desc.graphicsDevice).
        virtual FRESULT createDevice(const DeviceDesc& desc, IDevice** ppDevice) noexcept = 0;
        // A windowless swap chain for 'device' (a CUDA device, or the validation layer around one).
        virtual FRESULT createSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device,
            IGraphicsSwapChain** ppSwapChain) noexcept = 0;
    };

    typedef AutoPtr<IGraphicsFactory> GraphicsFactoryHandle;

    // Creates the CUDA graphics factory: loads and initializes the CUDA driver and enumerates the devices. Returns
    // FS_OK and a new reference in *ppFactory, or an FE_* code (FE_UNSUPPORTED if the driver does not load) and
    // nullptr.
    NVRHI_C_API FRESULT nvrhiCudaCreateGraphicsFactory(const GraphicsFactoryDesc* pDesc,
        IGraphicsFactory** ppFactory) noexcept;

    inline GraphicsFactoryHandle createGraphicsFactory(const GraphicsFactoryDesc& desc)
    {
        IGraphicsFactory* factory = nullptr;
        nvrhiCudaCreateGraphicsFactory(&desc, &factory);
        return TakeOver(factory);
    }
}
