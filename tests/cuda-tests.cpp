// The CUDA backend (ADR 0011) and its interop with the graphics devices (ADR 0013), on real devices.
//
// usage: nvrhi_cuda_tests <test> [fixture directory] [--verbose] [--case <text>]
//   qi              QueryInterface identities of the device, its IInteropDevice and the command list
//   copy            buffers and textures: writes, copies, clears, staging read-back
//   transient       upload memory reused while submissions are in flight
//   queues          the three queues, queueWaitForCommandList, event and timer queries, native work
//   dispatch        Slang kernels: binding sets, push constants, textures, surfaces (fixtures)
//   optix           OptiX: acceleration structures, a pipeline, a shader table (fixtures)
//   reflection      one container loaded on D3D12, Vulkan and CUDA (fixtures)
//   interop-d3d12   import of shared D3D12 buffers and textures (every shape, all mips), timeline ordering
//   interop-vulkan  the same with Vulkan
//   interop-d3d11   D3D11 has no interop: every IInteropDevice method returns FE_UNSUPPORTED
#include <nvrhi/nvrhi.h>
#include <nvrhi/cuda.h>
#include <nvrhi/interop.h>
#include <nvrhi/shader-reflection.h>
#include <nvrhi/core/foundation.h>
#include <nvrhi/common/misc.h>
#include "test-factory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#if TEST_D3D12
#include <nvrhi/d3d12.h>
#endif
#if TEST_D3D11
#include <nvrhi/d3d11.h>
#endif
#if TEST_D3D11 || TEST_D3D12
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif
#if TEST_VULKAN
#define VK_NO_PROTOTYPES
#include <nvrhi/vulkan.h>
#if !TEST_SHARED
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace
{
    void check(bool value, const std::string& message)
    {
        if (!value)
            throw std::runtime_error(message);
    }

    struct Messages : nvrhi::ObjectImpl<nvrhi::IMessageCallback>
    {
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IMessageCallback)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_END_INTERFACE_TABLE()

        unsigned errors = 0;
        bool quiet = false;     // expected errors are not counted
        bool verbose = false;   // also print the info messages
        void message(nvrhi::MessageSeverity severity, const char* text) noexcept override
        {
            const bool error = severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal;
            if (error && !quiet)
                ++errors;
            if (severity != nvrhi::MessageSeverity::Info || verbose)
                std::printf("NVRHI%s: %s\n", quiet ? " (expected)" : "", text);
        }
    };

    Messages* g_Messages = nullptr;
    const char* g_CaseFilter = nullptr;     // --case <text>: only the texture cases whose name contains it

    nvrhi::cuda::DeviceHandle createCudaDevice(nvrhi::IDevice* graphicsDevice = nullptr)
    {
        nvrhi::cuda::DeviceDesc desc;
        desc.messageCallback = g_Messages;
        desc.graphicsDevice = graphicsDevice;
        nvrhi::cuda::DeviceHandle device;
        const nvrhi::FRESULT result = nvrhi_test::graphicsFactory()->createCudaDevice(desc, &device);
        check(result == nvrhi::FS_OK && device, "create the CUDA device");
        return device;
    }

    // A deterministic byte pattern for a texel of a subresource.
    uint8_t patternByte(uint32_t seed, uint32_t mip, uint32_t slice, size_t offset)
    {
        uint32_t h = seed * 2654435761u ^ (mip * 40503u) ^ (slice * 9973u) ^ uint32_t(offset * 2246822519u);
        h ^= h >> 13;
        h *= 3266489917u;
        h ^= h >> 16;
        return uint8_t(h);
    }

    std::vector<uint8_t> makePattern(uint32_t seed, size_t size)
    {
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; ++i)
            data[i] = patternByte(seed, 0, 0, i);
        return data;
    }

    void executeAndWait(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
        nvrhi::CommandQueue queue = nvrhi::CommandQueue::Graphics)
    {
        device->executeCommandLists(&commandList, 1, queue);
        check(device->waitForIdle(), "waitForIdle");
        device->runGarbageCollection();
    }

    nvrhi::CommandListHandle createCommandList(nvrhi::IDevice* device,
        nvrhi::CommandQueue queue = nvrhi::CommandQueue::Graphics)
    {
        nvrhi::CommandListHandle commandList;
        device->createCommandList(nvrhi::CommandListParameters().setQueueType(queue).setEnableImmediateExecution(false), &commandList);
        check(commandList != nullptr, "create a command list");
        return commandList;
    }

    nvrhi::BufferHandle createBuffer(nvrhi::IDevice* device, size_t size, const char* name,
        nvrhi::CpuAccessMode cpuAccess = nvrhi::CpuAccessMode::None,
        nvrhi::SharedResourceFlags shared = nvrhi::SharedResourceFlags::None)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = size;
        desc.debugName = name;
        desc.cpuAccess = cpuAccess;
        desc.sharedResourceFlags = shared;
        desc.canHaveUAVs = cpuAccess == nvrhi::CpuAccessMode::None;
        desc.initialState = cpuAccess == nvrhi::CpuAccessMode::Read ? nvrhi::ResourceStates::CopyDest : nvrhi::ResourceStates::Common;
        desc.keepInitialState = true;
        nvrhi::BufferHandle buffer;
        device->createBuffer(desc, &buffer);
        check(buffer != nullptr, std::string("create buffer ") + name);
        return buffer;
    }

    // Reads a buffer back through a CPU-readable buffer of the same device.
    std::vector<uint8_t> readBuffer(nvrhi::IDevice* device, nvrhi::IBuffer* buffer, size_t size)
    {
        nvrhi::BufferHandle readback = createBuffer(device, size, "readback", nvrhi::CpuAccessMode::Read);
        nvrhi::CommandListHandle commandList = createCommandList(device);
        commandList->open();
        commandList->copyBuffer(readback, 0, buffer, 0, size);
        commandList->close();
        executeAndWait(device, commandList);
        const uint8_t* data = static_cast<const uint8_t*>(device->mapBuffer(readback, nvrhi::CpuAccessMode::Read));
        check(data != nullptr, "map the read-back buffer");
        std::vector<uint8_t> result(data, data + size);
        device->unmapBuffer(readback);
        return result;
    }

    //////////////////////////////////////////////////////////////////////////
    // Textures: shapes and patterns shared by the copy and interop tests
    //////////////////////////////////////////////////////////////////////////

    struct TextureCase
    {
        const char* name;
        nvrhi::TextureDimension dimension;
        nvrhi::Format format;
        uint32_t width, height, depthOrSlices, mips;
    };

    const TextureCase c_TextureCases[] = {
        { "2D RGBA8_UNORM with mips",   nvrhi::TextureDimension::Texture2D,      nvrhi::Format::RGBA8_UNORM,  64, 32, 1, 4 },
        { "2D array R16_FLOAT",         nvrhi::TextureDimension::Texture2DArray, nvrhi::Format::R16_FLOAT,    32, 32, 3, 2 },
        { "2D array RGBA8 1024 x3",     nvrhi::TextureDimension::Texture2DArray, nvrhi::Format::RGBA8_UNORM,  1024, 1024, 3, 1 },
        { "cube RGBA8 256 with mips",   nvrhi::TextureDimension::TextureCube,    nvrhi::Format::RGBA8_UNORM,  256, 256, 6, 3 },
        { "3D R32_FLOAT 128x128x64 with mips", nvrhi::TextureDimension::Texture3D, nvrhi::Format::R32_FLOAT, 128, 128, 64, 4 },
        { "2D RGBA16_FLOAT 1000x600 with mips", nvrhi::TextureDimension::Texture2D, nvrhi::Format::RGBA16_FLOAT, 1000, 600, 1, 5 },
        { "cube R32_FLOAT",             nvrhi::TextureDimension::TextureCube,    nvrhi::Format::R32_FLOAT,    16, 16, 6, 1 },
        { "3D R32_UINT with mips",      nvrhi::TextureDimension::Texture3D,      nvrhi::Format::R32_UINT,     16, 16, 8, 3 },
        { "2D R8_SINT",                 nvrhi::TextureDimension::Texture2D,      nvrhi::Format::R8_SINT,      40, 24, 1, 1 },
        { "2D RGBA32_FLOAT",            nvrhi::TextureDimension::Texture2D,      nvrhi::Format::RGBA32_FLOAT, 16, 8,  1, 1 },
        { "2D R8_UNORM",                nvrhi::TextureDimension::Texture2D,      nvrhi::Format::R8_UNORM,     33, 17, 1, 2 },
    };

    nvrhi::TextureDesc makeTextureDesc(const TextureCase& c, nvrhi::SharedResourceFlags shared, bool uav)
    {
        nvrhi::TextureDesc desc;
        desc.dimension = c.dimension;
        desc.format = c.format;
        desc.width = c.width;
        desc.height = c.height;
        if (c.dimension == nvrhi::TextureDimension::Texture3D)
            desc.depth = c.depthOrSlices;
        else
            desc.arraySize = c.depthOrSlices;
        desc.mipLevels = c.mips;
        desc.sharedResourceFlags = shared;
        desc.isUAV = uav;
        desc.debugName = c.name;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        return desc;
    }

    uint32_t mipExtent(uint32_t size, uint32_t mip) { return std::max(size >> mip, 1u); }

    uint32_t sliceCount(const nvrhi::TextureDesc& desc)
    {
        return desc.dimension == nvrhi::TextureDimension::Texture3D ? 1 : desc.arraySize;
    }

    uint32_t depthOf(const nvrhi::TextureDesc& desc, uint32_t mip)
    {
        return desc.dimension == nvrhi::TextureDimension::Texture3D ? mipExtent(desc.depth, mip) : 1;
    }

    // Tightly packed texels of one subresource.
    std::vector<uint8_t> subresourcePattern(const nvrhi::TextureDesc& desc, uint32_t seed, uint32_t mip, uint32_t slice,
        size_t& rowPitch, size_t& depthPitch)
    {
        const uint32_t bytes = nvrhi::getFormatInfo(desc.format).bytesPerBlock;
        rowPitch = size_t(mipExtent(desc.width, mip)) * bytes;
        depthPitch = rowPitch * mipExtent(desc.height, mip);
        std::vector<uint8_t> data(depthPitch * depthOf(desc, mip));
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = patternByte(seed, mip, slice, i);
        // Avoid half-float NaN payloads, which a copy may canonicalize.
        if (desc.format == nvrhi::Format::R16_FLOAT || desc.format == nvrhi::Format::R32_FLOAT ||
            desc.format == nvrhi::Format::RGBA32_FLOAT)
        {
            const size_t component = desc.format == nvrhi::Format::R16_FLOAT ? 2 : 4;
            for (size_t i = component - 1; i < data.size(); i += component)
                data[i] &= 0x3F;
        }
        return data;
    }

    void writeTexturePattern(nvrhi::ICommandList* commandList, nvrhi::ITexture* texture, uint32_t seed)
    {
        const nvrhi::TextureDesc& desc = texture->getDesc();
        for (uint32_t slice = 0; slice < sliceCount(desc); ++slice)
        {
            for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
            {
                size_t rowPitch, depthPitch;
                const std::vector<uint8_t> data = subresourcePattern(desc, seed, mip, slice, rowPitch, depthPitch);
                commandList->writeTexture(texture, slice, mip, data.data(), rowPitch, depthPitch);
            }
        }
    }

    // Copies every subresource into a staging texture of the same device and compares it with the pattern.
    void verifyTexturePattern(nvrhi::IDevice* device, nvrhi::ITexture* texture, uint32_t seed, const std::string& what)
    {
        const nvrhi::TextureDesc& desc = texture->getDesc();
        nvrhi::TextureDesc stagingDesc = desc;
        stagingDesc.sharedResourceFlags = nvrhi::SharedResourceFlags::None;
        stagingDesc.isUAV = false;
        stagingDesc.isRenderTarget = false;
        stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
        nvrhi::StagingTextureHandle staging;
        device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read, &staging);
        check(staging != nullptr, what + ": create a staging texture");

        nvrhi::CommandListHandle commandList = createCommandList(device);
        commandList->open();
        for (uint32_t slice = 0; slice < sliceCount(desc); ++slice)
        {
            for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
            {
                // One depth slice per copy: the Vulkan backend copies 3D textures to staging that way.
                for (uint32_t z = 0; z < depthOf(desc, mip); ++z)
                {
                    nvrhi::TextureSlice textureSlice;
                    textureSlice.mipLevel = mip;
                    textureSlice.arraySlice = slice;
                    textureSlice.z = z;
                    textureSlice.depth = 1;
                    commandList->copyTexture2(staging, textureSlice, texture, textureSlice);
                }
            }
        }
        commandList->close();
        executeAndWait(device, commandList);

        for (uint32_t slice = 0; slice < sliceCount(desc); ++slice)
        {
            for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
            {
                size_t rowPitch, depthPitch;
                const std::vector<uint8_t> expected = subresourcePattern(desc, seed, mip, slice, rowPitch, depthPitch);
                nvrhi::TextureSlice textureSlice;
                textureSlice.mipLevel = mip;
                textureSlice.arraySlice = slice;
                size_t mappedRowPitch = 0;
                const uint8_t* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, textureSlice,
                    nvrhi::CpuAccessMode::Read, mappedRowPitch));
                check(mapped != nullptr, what + ": map the staging texture");
                const uint32_t rows = mipExtent(desc.height, mip);
                const uint32_t depth = depthOf(desc, mip);
                // 3D slices of a staging texture follow each other at rows * mappedRowPitch.
                for (uint32_t z = 0; z < depth; ++z)
                {
                    for (uint32_t y = 0; y < rows; ++y)
                    {
                        const uint8_t* actualRow = mapped + (size_t(z) * rows + y) * mappedRowPitch;
                        const uint8_t* expectedRow = expected.data() + z * depthPitch + y * rowPitch;
                        if (memcmp(actualRow, expectedRow, rowPitch) != 0)
                        {
                            device->unmapStagingTexture(staging);
                            throw std::runtime_error(what + ": mismatch at mip " + std::to_string(mip) + ", slice " +
                                std::to_string(slice) + ", z " + std::to_string(z) + ", row " + std::to_string(y));
                        }
                    }
                }
                device->unmapStagingTexture(staging);
            }
        }
    }

    //////////////////////////////////////////////////////////////////////////
    // qi
    //////////////////////////////////////////////////////////////////////////

    void* query(nvrhi::IObject* object, const nvrhi::GUID& iid)
    {
        void* pv = nullptr;
        if (object->QueryInterface(iid, &pv) != nvrhi::FS_OK)
            return nullptr;
        static_cast<nvrhi::IObject*>(pv)->Release();
        return pv;
    }

    struct NativeWork : nvrhi::ObjectImpl<nvrhi::cuda::INativeWork>
    {
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(NativeWork)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::cuda::INativeWork)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_END_INTERFACE_TABLE()

        nvrhi::NativeObject stream = nullptr;
        int calls = 0;
        void execute(nvrhi::NativeObject s) noexcept override { stream = s; ++calls; }
    };

    void testQueryInterface()
    {
        nvrhi::cuda::DeviceHandle device = createCudaDevice();
        nvrhi::IObject* identity = static_cast<nvrhi::IObject*>(query(device, nvrhi::IID_IObject));
        check(identity != nullptr, "device: IObject");

        nvrhi::IDevice* base = static_cast<nvrhi::IDevice*>(query(device, nvrhi::IID_IDevice));
        nvrhi::cuda::IDevice* cuda = static_cast<nvrhi::cuda::IDevice*>(query(device, nvrhi::cuda::IID_IDevice));
        nvrhi::IRHIObject* rhi = static_cast<nvrhi::IRHIObject*>(query(device, nvrhi::IID_IRHIObject));
        nvrhi::IInteropDevice* interop = static_cast<nvrhi::IInteropDevice*>(query(device, nvrhi::IID_IInteropDevice));
        check(base && cuda && rhi && interop, "device: IDevice, cuda::IDevice, IRHIObject and IInteropDevice are answered");
        check(base == static_cast<nvrhi::IDevice*>(device.Get()) && cuda == device.Get(), "device: interface pointers");

        // Every interface leads back to the same identity (ROUTE_PARENT keeps one object, ADR 0013).
        for (nvrhi::IObject* object : { static_cast<nvrhi::IObject*>(base), static_cast<nvrhi::IObject*>(interop),
            static_cast<nvrhi::IObject*>(rhi) })
            check(query(object, nvrhi::IID_IObject) == identity, "device: one identity through every interface");
        check(query(interop, nvrhi::cuda::IID_IDevice) == cuda, "IInteropDevice -> cuda::IDevice");
        check(interop->getComputeDevice() == base, "IInteropDevice::getComputeDevice");
        check(interop->getGraphicsDevice() == nullptr && !interop->isInteropSupported(), "unpaired: no interop");
        check(query(device, nvrhi::IID_IGraphicsFactory) == nullptr, "device refuses IGraphicsFactory");
        check(device->getGraphicsAPI() == nvrhi::GraphicsAPI::CUDA, "getGraphicsAPI");
        check(device->getNativeObject(nvrhi::ObjectTypes::CUDA_Context) != nullptr, "native CUDA context");
        check(device->getNativeQueue(nvrhi::ObjectTypes::CUDA_Stream, nvrhi::CommandQueue::Compute) != nullptr, "native stream");
        std::printf("CUDA device: sm_%u, OptiX %s\n", device->getComputeCapability(), device->isOptixAvailable() ? "available" : "unavailable");

        nvrhi::BufferHandle buffer;
        nvrhi::FRESULT unsupported = interop->importBuffer(nullptr, &buffer);
        check(unsupported == nvrhi::FE_INVALID_ARGS, "importBuffer(nullptr)");

        // The command list: nvrhi::ICommandList and cuda::ICommandList; native work runs on the queue's stream.
        nvrhi::CommandListHandle commandList = createCommandList(device, nvrhi::CommandQueue::Compute);
        nvrhi::cuda::ICommandList* cudaList = static_cast<nvrhi::cuda::ICommandList*>(query(commandList, nvrhi::cuda::IID_ICommandList));
        check(cudaList != nullptr && static_cast<nvrhi::ICommandList*>(cudaList) == commandList.Get(), "command list: cuda::ICommandList");
        nvrhi::AutoPtr<NativeWork> work = MAKE_RC_OBJ_PTR(NativeWork);
        commandList->open();
        cudaList->enqueueNativeWork(work);
        commandList->close();
        check(work->calls == 0, "native work runs when the list executes, not when it is recorded");
        executeAndWait(device, commandList, nvrhi::CommandQueue::Compute);
        check(work->calls == 1 && work->stream == device->getNativeQueue(nvrhi::ObjectTypes::CUDA_Stream, nvrhi::CommandQueue::Compute),
            "native work ran once on the compute stream");

        // Through the validation layer.
        nvrhi::DeviceHandle validation = nvrhi_test::createValidationLayer(device);
        check(validation != nullptr, "validation layer over the CUDA device");
        check(validation->getGraphicsAPI() == nvrhi::GraphicsAPI::CUDA, "validation: getGraphicsAPI");
        nvrhi::BufferHandle validated = createBuffer(validation, 256, "validated buffer");
        check(validated != nullptr, "validation: createBuffer");
        // The wrapper answers IInteropDevice because the CUDA device does, with the wrapper's identity.
        nvrhi::IInteropDevice* validationInterop = static_cast<nvrhi::IInteropDevice*>(query(validation, nvrhi::IID_IInteropDevice));
        check(validationInterop != nullptr, "validation: IInteropDevice");
        check(query(validationInterop, nvrhi::IID_IObject) == query(validation, nvrhi::IID_IObject), "validation: one identity");
        check(validationInterop->getComputeDevice() == validation.Get(), "validation: getComputeDevice is the wrapper");
        check(query(validationInterop, nvrhi::IID_IDevice) == validation.Get(), "validation: IInteropDevice -> IDevice");
        g_Messages->quiet = true;
        check(validationInterop->importBuffer(nullptr, &buffer) == nvrhi::FE_INVALID_ARGS, "validation: importBuffer(nullptr)");
        g_Messages->quiet = false;
    }

    //////////////////////////////////////////////////////////////////////////
    // copy
    //////////////////////////////////////////////////////////////////////////

    void testCopies(nvrhi::IDevice* device)
    {
        // Buffers: write, copy, clear, read back.
        const size_t size = 300 * 1024 + 12;    // larger than one upload chunk, not a multiple of 4 KB
        const std::vector<uint8_t> pattern = makePattern(1, size);
        nvrhi::BufferHandle source = createBuffer(device, size, "source");
        nvrhi::BufferHandle destination = createBuffer(device, size, "destination");
        nvrhi::BufferHandle cleared = createBuffer(device, 4096, "cleared");

        nvrhi::CommandListHandle commandList = createCommandList(device);
        commandList->open();
        commandList->writeBuffer(source, pattern.data(), size, 0);
        commandList->copyBuffer(destination, 0, source, 0, size);
        commandList->writeBuffer(destination, pattern.data() + 1000, 64, 16);     // a partial write at an offset
        commandList->clearBufferUInt(cleared, 0xA5C3E1F7u);
        commandList->close();
        executeAndWait(device, commandList);

        std::vector<uint8_t> expected = pattern;
        memcpy(expected.data() + 16, pattern.data() + 1000, 64);
        check(readBuffer(device, destination, size) == expected, "buffer write + copy");
        const std::vector<uint8_t> clearedData = readBuffer(device, cleared, 4096);
        for (size_t i = 0; i < clearedData.size(); i += 4)
            check(*reinterpret_cast<const uint32_t*>(&clearedData[i]) == 0xA5C3E1F7u, "clearBufferUInt");
        std::printf("buffers: write, copy, clear, read-back OK\n");

        // Textures: every shape, every mip and slice, through writeTexture, a texture-to-texture copy and staging.
        for (const TextureCase& c : c_TextureCases)
        {
            nvrhi::TextureHandle texture, copy;
            device->createTexture(makeTextureDesc(c, nvrhi::SharedResourceFlags::None, true), &texture);
            device->createTexture(makeTextureDesc(c, nvrhi::SharedResourceFlags::None, false), &copy);
            check(texture && copy, std::string("create texture ") + c.name);

            commandList->open();
            writeTexturePattern(commandList, texture, 7);
            for (uint32_t slice = 0; slice < sliceCount(texture->getDesc()); ++slice)
            {
                for (uint32_t mip = 0; mip < c.mips; ++mip)
                {
                    nvrhi::TextureSlice textureSlice;
                    textureSlice.mipLevel = mip;
                    textureSlice.arraySlice = slice;
                    commandList->copyTexture1(copy, textureSlice, texture, textureSlice);
                }
            }
            commandList->close();
            executeAndWait(device, commandList);

            verifyTexturePattern(device, texture, 7, c.name);
            verifyTexturePattern(device, copy, 7, std::string(c.name) + " (copy)");
            std::printf("texture %s: write, copy, staging read-back OK\n", c.name);
        }

        // clearTextureFloat on a normalized format.
        nvrhi::TextureHandle target;
        nvrhi::TextureDesc desc = makeTextureDesc(c_TextureCases[0], nvrhi::SharedResourceFlags::None, true);
        device->createTexture(desc, &target);
        commandList->open();
        commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(1.f, 0.f, 0.5f, 0.25f));
        commandList->close();
        executeAndWait(device, commandList);
        nvrhi::StagingTextureHandle staging;
        desc.isUAV = false;
        device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read, &staging);
        commandList->open();
        commandList->copyTexture2(staging, nvrhi::TextureSlice().setMipLevel(2), target, nvrhi::TextureSlice().setMipLevel(2));
        commandList->close();
        executeAndWait(device, commandList);
        size_t rowPitch = 0;
        const uint8_t* texel = static_cast<const uint8_t*>(device->mapStagingTexture(staging,
            nvrhi::TextureSlice().setMipLevel(2), nvrhi::CpuAccessMode::Read, rowPitch));
        check(texel && texel[0] == 255 && texel[1] == 0 && texel[2] == 128 && texel[3] == 64, "clearTextureFloat");
        device->unmapStagingTexture(staging);
        std::printf("clearTextureFloat OK\n");
    }

    //////////////////////////////////////////////////////////////////////////
    // transient
    //////////////////////////////////////////////////////////////////////////

    void testTransient(nvrhi::IDevice* device)
    {
        // Many submissions without waiting: each writes through upload memory, which must not be reused before
        // the submission that reads it has finished.
        constexpr int buffers = 8;
        constexpr int iterations = 160;
        const size_t sizes[] = { 1024, 48 * 1024, 200 * 1024, 2 * 1024 * 1024 + 4 };
        std::vector<nvrhi::BufferHandle> targets;
        for (int i = 0; i < buffers; ++i)
            targets.push_back(createBuffer(device, sizes[i % 4], "transient target"));

        std::vector<nvrhi::CommandListHandle> lists;
        for (int i = 0; i < 3; ++i)
            lists.push_back(createCommandList(device));

        uint64_t lastInstance = 0;
        for (int i = 0; i < iterations; ++i)
        {
            nvrhi::ICommandList* commandList = lists[i % lists.size()];
            const int target = i % buffers;
            const std::vector<uint8_t> data = makePattern(uint32_t(i), sizes[target % 4]);
            commandList->open();
            commandList->writeBuffer(targets[target], data.data(), data.size(), 0);
            commandList->close();
            lastInstance = device->executeCommandList(commandList);
            if (i % 37 == 0)
                device->runGarbageCollection();
        }
        check(lastInstance == uint64_t(iterations), "submission numbers count the submissions");
        check(device->waitForIdle(), "waitForIdle");
        auto cuda = static_cast<nvrhi::cuda::IDevice*>(query(device, nvrhi::cuda::IID_IDevice));
        check(!cuda || cuda->queueGetCompletedInstance(nvrhi::CommandQueue::Graphics) == lastInstance, "every submission finished");

        for (int target = 0; target < buffers; ++target)
        {
            int last = iterations - buffers + target;
            check(readBuffer(device, targets[target], sizes[target % 4]) == makePattern(uint32_t(last), sizes[target % 4]),
                "transient: buffer " + std::to_string(target) + " has the last write");
        }
        std::printf("transient: %d submissions in flight, upload memory reused safely\n", iterations);
    }

    //////////////////////////////////////////////////////////////////////////
    // queues
    //////////////////////////////////////////////////////////////////////////

    void testQueues(nvrhi::IDevice* device)
    {
        const size_t size = 1024 * 1024;
        nvrhi::BufferHandle a = createBuffer(device, size, "a");
        nvrhi::BufferHandle b = createBuffer(device, size, "b");
        const std::vector<uint8_t> pattern = makePattern(42, size);

        // Copy queue writes a, compute queue copies a to b after it, graphics reads b.
        nvrhi::CommandListHandle copyList = createCommandList(device, nvrhi::CommandQueue::Copy);
        nvrhi::CommandListHandle computeList = createCommandList(device, nvrhi::CommandQueue::Compute);
        copyList->open();
        copyList->writeBuffer(a, pattern.data(), size, 0);
        copyList->close();
        nvrhi::ICommandList* list = copyList;
        const uint64_t copyInstance = device->executeCommandLists(&list, 1, nvrhi::CommandQueue::Copy);
        device->queueWaitForCommandList(nvrhi::CommandQueue::Compute, nvrhi::CommandQueue::Copy, copyInstance);

        nvrhi::TimerQueryHandle timer;
        device->createTimerQuery(&timer);
        computeList->open();
        computeList->beginTimerQuery(timer);
        computeList->copyBuffer(b, 0, a, 0, size);
        computeList->endTimerQuery(timer);
        computeList->close();
        list = computeList;
        device->executeCommandLists(&list, 1, nvrhi::CommandQueue::Compute);

        nvrhi::EventQueryHandle event;
        device->createEventQuery(&event);
        device->setEventQuery(event, nvrhi::CommandQueue::Compute);
        device->waitEventQuery(event);
        check(device->pollEventQuery(event), "event query signalled");
        check(device->pollTimerQuery(timer), "timer query finished");
        const float seconds = device->getTimerQueryTime(timer);
        check(seconds >= 0.f && seconds < 1.f, "timer query time");
        check(readBuffer(device, b, size) == pattern, "copy queue -> compute queue ordering");
        std::printf("queues: copy -> compute ordering, event and timer queries OK (copy took %.3f ms)\n", seconds * 1000.f);
    }

    //////////////////////////////////////////////////////////////////////////
    // Interop
    //////////////////////////////////////////////////////////////////////////

    void testInterop(nvrhi::IDevice* graphics, const char* api)
    {
        nvrhi::cuda::DeviceHandle cuda = createCudaDevice(graphics);
        nvrhi::InteropDeviceHandle interop;
        check(cuda->QueryInterface(NVRHI_IID_PPV_ARGS(&interop)) == nvrhi::FS_OK, "query IInteropDevice");
        check(interop->isInteropSupported(), std::string(api) + ": interop is supported");
        check(interop->getGraphicsDevice() == graphics, "getGraphicsDevice");
        auto ordered = [&](nvrhi::FRESULT result, const char* what) { check(result == nvrhi::FS_OK, what); };

        // Buffers, graphics -> CUDA -> graphics.
        const size_t size = 256 * 1024;
        nvrhi::BufferHandle shared = createBuffer(graphics, size, "shared buffer", nvrhi::CpuAccessMode::None,
            nvrhi::SharedResourceFlags::Shared);
        const std::vector<uint8_t> fromGraphics = makePattern(100, size);
        nvrhi::CommandListHandle graphicsList = createCommandList(graphics);
        graphicsList->open();
        graphicsList->writeBuffer(shared, fromGraphics.data(), size, 0);
        graphicsList->close();
        graphics->executeCommandList(graphicsList);
        ordered(interop->computeQueueWaitForGraphicsQueue(nvrhi::CommandQueue::Graphics, nvrhi::CommandQueue::Graphics),
            "computeQueueWaitForGraphicsQueue");

        nvrhi::BufferHandle imported;
        ordered(interop->importBuffer(shared, &imported), "importBuffer");
        check(readBuffer(cuda, imported, size) == fromGraphics, std::string(api) + ": CUDA reads the graphics writes");

        const std::vector<uint8_t> fromCuda = makePattern(200, size);
        nvrhi::CommandListHandle cudaList = createCommandList(cuda);
        cudaList->open();
        cudaList->writeBuffer(imported, fromCuda.data(), size, 0);
        cudaList->close();
        cuda->executeCommandList(cudaList);
        ordered(interop->graphicsQueueWaitForComputeQueue(nvrhi::CommandQueue::Graphics, nvrhi::CommandQueue::Graphics),
            "graphicsQueueWaitForComputeQueue");
        check(readBuffer(graphics, shared, size) == fromCuda, std::string(api) + ": graphics reads the CUDA writes");
        std::printf("%s interop: buffer both ways OK\n", api);

        // Textures: every shape with all mips and slices, both ways.
        const char* filter = g_CaseFilter;
        for (const TextureCase& c : c_TextureCases)
        {
            if (filter && !std::strstr(c.name, filter))
                continue;
            const auto caseStart = std::chrono::steady_clock::now();
            nvrhi::TextureHandle texture;
            graphics->createTexture(makeTextureDesc(c, nvrhi::SharedResourceFlags::Shared, true), &texture);
            check(texture != nullptr, std::string(api) + ": create shared texture " + c.name);

            // Vulkan pads the layers of mipmapped arrays beyond the CUDA layout: those are refused (ADR 0013).
            const bool layered = c.dimension == nvrhi::TextureDimension::Texture2DArray ||
                c.dimension == nvrhi::TextureDimension::TextureCube;
            if (graphics->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN && layered && c.mips > 1)
            {
                nvrhi::TextureHandle refused;
                g_Messages->quiet = true;
                const nvrhi::FRESULT result = interop->importTexture(texture, &refused);
                g_Messages->quiet = false;
                check(result == nvrhi::FE_UNSUPPORTED && !refused, std::string(api) + ": importTexture refuses " + c.name);
                std::printf("%s interop: texture %s is refused as designed\n", api, c.name);
                continue;
            }

            graphicsList->open();
            writeTexturePattern(graphicsList, texture, 300);
            graphicsList->close();
            graphics->executeCommandList(graphicsList);
            // The graphics device reads its own writes first: a mismatch after this is the import's.
            verifyTexturePattern(graphics, texture, 300, std::string(api) + " " + c.name);
            ordered(interop->computeQueueWaitForGraphicsQueue(nvrhi::CommandQueue::Graphics, nvrhi::CommandQueue::Graphics),
                "computeQueueWaitForGraphicsQueue");

            nvrhi::TextureHandle importedTexture;
            const nvrhi::FRESULT result = interop->importTexture(texture, &importedTexture);
            check(result == nvrhi::FS_OK, std::string(api) + ": importTexture " + c.name);
            verifyTexturePattern(cuda, importedTexture, 300, std::string(api) + " -> CUDA " + c.name);

            cudaList->open();
            writeTexturePattern(cudaList, importedTexture, 400);
            cudaList->close();
            cuda->executeCommandList(cudaList);
            ordered(interop->graphicsQueueWaitForComputeQueue(nvrhi::CommandQueue::Graphics, nvrhi::CommandQueue::Graphics),
                "graphicsQueueWaitForComputeQueue");
            verifyTexturePattern(graphics, texture, 400, std::string("CUDA -> ") + api + " " + c.name);
            std::printf("%s interop: texture %s both ways OK (%.1f s)\n", api, c.name,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - caseStart).count());
        }

        cuda->waitForIdle();
        graphics->waitForIdle();
    }

#if TEST_D3D11 || TEST_D3D12
    // The first NVIDIA adapter: interop needs the graphics device on the CUDA GPU.
    Microsoft::WRL::ComPtr<IDXGIAdapter1> findNvidiaAdapter()
    {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.VendorId == 0x10DE)
                return adapter;
        }
        throw std::runtime_error("no NVIDIA adapter");
    }
#endif

#if TEST_D3D12
    nvrhi::d3d12::DeviceHandle createD3D12Device(Microsoft::WRL::ComPtr<ID3D12Device>& native,
        Microsoft::WRL::ComPtr<ID3D12CommandQueue>& queue);

    void testD3D12Interop()
    {
        Microsoft::WRL::ComPtr<ID3D12Device> native;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        nvrhi::d3d12::DeviceHandle device = createD3D12Device(native, queue);
        testInterop(device, "D3D12");
    }
#endif

#if TEST_D3D11
    void testD3D11Interop()
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter = findNvidiaAdapter();
        Microsoft::WRL::ComPtr<ID3D11Device> native;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        check(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &native, nullptr, &context)), "D3D11 device");
        nvrhi::d3d11::DeviceDesc desc;
        desc.context = context.Get();
        desc.messageCallback = g_Messages;
        nvrhi::DeviceHandle device;
        nvrhi_test::graphicsFactory()->createD3D11Device(desc, &device);
        check(device != nullptr, "nvrhi D3D11 device");

        // The CUDA device is selected on the D3D11 adapter, without interop.
        g_Messages->quiet = true;   // the device reports that D3D11 has no interop
        nvrhi::cuda::DeviceHandle cuda = createCudaDevice(device);
        g_Messages->quiet = false;
        nvrhi::InteropDeviceHandle interop;
        check(cuda->QueryInterface(NVRHI_IID_PPV_ARGS(&interop)) == nvrhi::FS_OK, "query IInteropDevice");
        check(!interop->isInteropSupported(), "D3D11: no interop");
        nvrhi::BufferHandle buffer = createBuffer(device, 256, "d3d11 buffer");
        nvrhi::BufferHandle importedBuffer;
        nvrhi::TextureHandle importedTexture;
        check(interop->importBuffer(buffer, &importedBuffer) == nvrhi::FE_UNSUPPORTED && !importedBuffer, "importBuffer unsupported");
        check(interop->importTexture(nullptr, &importedTexture) == nvrhi::FE_INVALID_ARGS, "importTexture(nullptr)");
        check(interop->computeQueueWaitForGraphicsQueue(nvrhi::CommandQueue::Graphics, nvrhi::CommandQueue::Graphics) == nvrhi::FE_UNSUPPORTED,
            "computeQueueWaitForGraphicsQueue unsupported");
        check(interop->graphicsQueueWaitForComputeQueue(nvrhi::CommandQueue::Graphics, nvrhi::CommandQueue::Graphics) == nvrhi::FE_UNSUPPORTED,
            "graphicsQueueWaitForComputeQueue unsupported");
        std::printf("D3D11: the CUDA device is on the D3D11 adapter; interop is FE_UNSUPPORTED as designed\n");
    }
#endif

#if TEST_VULKAN
    // Creates an NVRHI Vulkan device on the NVIDIA GPU, with the external memory and semaphore extensions, and
    // runs 'body' with it.
    void withVulkanDevice(const std::function<void(nvrhi::IDevice*)>& body)
    {
#ifdef _WIN32
        auto loader = LoadLibraryA("vulkan-1.dll");
        check(loader != nullptr, "Vulkan loader");
#define LOAD_VK(name) auto name = reinterpret_cast<PFN_##name>(GetProcAddress(loader, #name)); check(name != nullptr, #name)
#else
        auto loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        check(loader != nullptr, "Vulkan loader");
#define LOAD_VK(name) auto name = reinterpret_cast<PFN_##name>(dlsym(loader, #name)); check(name != nullptr, #name)
#endif
        LOAD_VK(vkGetInstanceProcAddr);
        LOAD_VK(vkCreateInstance);
        LOAD_VK(vkEnumeratePhysicalDevices);
        LOAD_VK(vkGetPhysicalDeviceProperties);
        LOAD_VK(vkGetPhysicalDeviceQueueFamilyProperties);
        LOAD_VK(vkCreateDevice);
        LOAD_VK(vkGetDeviceQueue);
        LOAD_VK(vkGetDeviceProcAddr);
        LOAD_VK(vkDestroyDevice);
        LOAD_VK(vkDestroyInstance);
#undef LOAD_VK
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        VkInstance instance;
        check(vkCreateInstance(&instanceInfo, nullptr, &instance) == VK_SUCCESS, "Vulkan instance");
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count, "Vulkan physical devices");
        std::vector<VkPhysicalDevice> physical(count);
        vkEnumeratePhysicalDevices(instance, &count, physical.data());
        VkPhysicalDevice nvidia = VK_NULL_HANDLE;
        for (VkPhysicalDevice candidate : physical)
        {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.vendorID == 0x10DE)
            {
                nvidia = candidate;
                break;
            }
        }
        check(nvidia != VK_NULL_HANDLE, "an NVIDIA Vulkan device");
        vkGetPhysicalDeviceQueueFamilyProperties(nvidia, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(nvidia, &count, families.data());
        uint32_t family = 0;
        while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            ++family;
        check(family < count, "Vulkan graphics queue family");

        const char* extensions[] = {
#ifdef _WIN32
            "VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32",
#else
            "VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd",
#endif
        };
        float priority = 1.f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan12Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features.timelineSemaphore = VK_TRUE;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &features;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = uint32_t(std::size(extensions));
        deviceInfo.ppEnabledExtensionNames = extensions;
        VkDevice native;
        check(vkCreateDevice(nvidia, &deviceInfo, nullptr, &native) == VK_SUCCESS, "Vulkan device");

        nvrhi::vulkan::DeviceDesc desc{};
        desc.instance = instance;
        desc.physicalDevice = nvidia;
        desc.device = native;
        desc.deviceExtensions = extensions;
        desc.numDeviceExtensions = std::size(extensions);
        desc.graphicsQueueIndex = int(family);
        vkGetDeviceQueue(native, family, 0, &desc.graphicsQueue);
        desc.errorCB = g_Messages;
#if !TEST_SHARED
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance, vkGetInstanceProcAddr, native, vkGetDeviceProcAddr);
#endif
        try
        {
            nvrhi::vulkan::DeviceHandle device;
            nvrhi_test::graphicsFactory()->createVulkanDevice(desc, &device);
            check(device != nullptr, "nvrhi Vulkan device");
            body(device);
        }
        catch (...)
        {
            vkDestroyDevice(native, nullptr);
            vkDestroyInstance(instance, nullptr);
            throw;
        }
        vkDestroyDevice(native, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

    void testVulkanInterop()
    {
        withVulkanDevice([](nvrhi::IDevice* device) { testInterop(device, "Vulkan"); });
    }

#endif

#if TEST_D3D12
    // An NVRHI D3D12 device on the NVIDIA GPU.
    nvrhi::d3d12::DeviceHandle createD3D12Device(Microsoft::WRL::ComPtr<ID3D12Device>& native,
        Microsoft::WRL::ComPtr<ID3D12CommandQueue>& queue)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter = findNvidiaAdapter();
        check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&native))), "D3D12 device");
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        check(SUCCEEDED(native->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))), "D3D12 queue");
        nvrhi::d3d12::DeviceDesc desc;
        desc.pDevice = native.Get();
        desc.pGraphicsCommandQueue = queue.Get();
        desc.errorCB = g_Messages;
        nvrhi::d3d12::DeviceHandle device;
        nvrhi_test::graphicsFactory()->createD3D12Device(desc, &device);
        check(device != nullptr, "nvrhi D3D12 device");
        return device;
    }
#endif

    //////////////////////////////////////////////////////////////////////////
    // Fixtures (dispatch, optix, reflection): cuda-tests-shaders.inl
    //////////////////////////////////////////////////////////////////////////
}

#include "cuda-tests-shaders.inl"

int main(int argc, char** argv)
{
    try
    {
        check(argc >= 2, "usage: nvrhi_cuda_tests <test> [fixture directory]");
        const std::string test = argv[1];
        std::string fixtures;
        nvrhi::AutoPtr<Messages> messages = MAKE_RC_OBJ_PTR(Messages);
        g_Messages = messages;
        for (int i = 2; i < argc; ++i)
        {
            const std::string argument = argv[i];
            if (argument == "--verbose")
                messages->verbose = true;
            else if (argument == "--case" && i + 1 < argc)
                g_CaseFilter = argv[++i];
            else
                fixtures = argument;
        }

        if (!nvrhi_test::graphicsFactory()->isBackendAvailable(nvrhi::GraphicsAPI::CUDA))
        {
            std::printf("SKIP: no CUDA driver\n");
            return 0;
        }

        if (test == "qi")
            testQueryInterface();
        else if (test == "copy")
        {
            nvrhi::cuda::DeviceHandle device = createCudaDevice();
            testCopies(device);
            nvrhi::DeviceHandle validation = nvrhi_test::createValidationLayer(device);
            std::printf("-- through the validation layer\n");
            testCopies(validation);
        }
        else if (test == "transient")
            testTransient(createCudaDevice());
        else if (test == "queues")
            testQueues(createCudaDevice());
        else if (test == "dispatch")
            testDispatch(fixtures);
        else if (test == "handles")
            testHandles(fixtures);
        else if (test == "optix")
            testOptix(fixtures);
        else if (test == "reflection")
            testReflection(fixtures);
#if TEST_D3D12
        else if (test == "interop-d3d12")
            testD3D12Interop();
#endif
#if TEST_VULKAN
        else if (test == "interop-vulkan")
            testVulkanInterop();
#endif
#if TEST_D3D11
        else if (test == "interop-d3d11")
            testD3D11Interop();
#endif
        else
            throw std::runtime_error("unknown test " + test);

        check(messages->errors == 0, std::to_string(messages->errors) + " error messages");
        std::printf("PASS: %s\n", test.c_str());
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
