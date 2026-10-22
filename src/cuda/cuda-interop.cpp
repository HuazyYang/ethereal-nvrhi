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

// IInteropDevice (ADR 0013): resources of the paired graphics device imported as CUDA external memory, and queue
// ordering through shared timeline fences. The graphics API specifics are in cuda-interop-d3d12.cpp and
// cuda-interop-vulkan.cpp.
//
// Textures are mapped as whole mipmapped arrays (cuExternalMemoryGetMappedMipmappedArray): every mip level and
// array slice, 3D textures included. The array descriptor must match the graphics resource exactly; it is derived
// from the texture's TextureDesc by GetArrayDescriptor, with COLOR_ATTACHMENT for render targets.

#include "cuda-backend.h"

#include <nvrhi/common/misc.h>

#include <algorithm>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace nvrhi::cuda
{
    namespace
    {
        // A file descriptor passes to CUDA on a successful import; it is closed here if the import failed.
        void ReleaseHandleAfterFailedImport(const CUDA_EXTERNAL_MEMORY_HANDLE_DESC& desc)
        {
#ifndef _WIN32
            if (desc.type == CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD && desc.handle.fd >= 0)
                close(desc.handle.fd);
#else
            (void)desc;
#endif
        }
    }

    bool InteropDevice::initializeInterop(const Context& context, Device* device, nvrhi::IDevice* graphicsDevice,
        std::string& error)
    {
        m_Context = &context;
        m_Device = device;
        m_GraphicsDevice = graphicsDevice;
        if (!graphicsDevice)
            return true;

        switch (graphicsDevice->getGraphicsAPI())
        {
        case GraphicsAPI::D3D12:
            m_Api = CreateD3D12InteropApi(context, graphicsDevice, error);
            break;
        case GraphicsAPI::VULKAN:
            m_Api = CreateVulkanInteropApi(context, graphicsDevice, error);
            break;
        case GraphicsAPI::D3D11:
            error = "D3D11 has no interop with CUDA (it cannot report the allocation size of a resource)";
            break;
        default:
            error = "the graphics device has no interop with CUDA";
            break;
        }
        return m_Api != nullptr;
    }

    void InteropDevice::shutdownInterop()
    {
        m_Api = nullptr;
        m_GraphicsDevice = nullptr;
    }

    FRESULT InteropDevice::importBuffer(IBuffer* graphicsBuffer, IBuffer** ppBuffer) noexcept
    {
        if (!ppBuffer)
            return FE_INVALID_ARGS;
        *ppBuffer = nullptr;
        if (!graphicsBuffer)
            return FE_INVALID_ARGS;
        if (!m_Api)
            return FE_UNSUPPORTED;

        try
        {
            const BufferDesc& desc = graphicsBuffer->getDesc();
            if ((desc.sharedResourceFlags & SharedResourceFlags::Shared) == 0)
            {
                m_Context->error(std::string("importBuffer: ") + utils::DebugNameToString(desc.debugName) +
                    " was not created with SharedResourceFlags::Shared");
                return FE_INVALID_ARGS;
            }

            CUDA_EXTERNAL_MEMORY_HANDLE_DESC memoryDesc = {};
            std::string error;
            if (!m_Api->getBufferMemory(graphicsBuffer, memoryDesc, error))
            {
                m_Context->error("importBuffer: " + error);
                return FE_GENERIC_ERROR;
            }

            ContextScope scope(*m_Context);
            AutoPtr<Buffer> buffer = TakeOver(MAKE_RC_OBJ(Buffer, *m_Context));
            buffer->desc = desc;
            buffer->desc.sharedResourceFlags = SharedResourceFlags::None;
            buffer->desc.cpuAccess = CpuAccessMode::None;
            buffer->ownsMemory = false;
            buffer->importedResource = graphicsBuffer;

            if (!NVRHI_CUDA_CHECK(*m_Context, cuImportExternalMemory(&buffer->externalMemory, &memoryDesc)))
            {
                ReleaseHandleAfterFailedImport(memoryDesc);
                return FE_GENERIC_ERROR;
            }

            CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufferDesc = {};
            bufferDesc.offset = 0;
            bufferDesc.size = desc.byteSize;
            if (!NVRHI_CUDA_CHECK(*m_Context, cuExternalMemoryGetMappedBuffer(&buffer->devicePtr, buffer->externalMemory, &bufferDesc)))
                return FE_GENERIC_ERROR;

            *ppBuffer = buffer.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc&)
        {
            return FE_OUT_OF_MEMORY;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }

    FRESULT InteropDevice::importTexture(ITexture* graphicsTexture, ITexture** ppTexture) noexcept
    {
        if (!ppTexture)
            return FE_INVALID_ARGS;
        *ppTexture = nullptr;
        if (!graphicsTexture)
            return FE_INVALID_ARGS;
        if (!m_Api)
            return FE_UNSUPPORTED;

        try
        {
            const TextureDesc& desc = graphicsTexture->getDesc();
            if ((desc.sharedResourceFlags & SharedResourceFlags::Shared) == 0)
            {
                m_Context->error(std::string("importTexture: ") + utils::DebugNameToString(desc.debugName) +
                    " was not created with SharedResourceFlags::Shared");
                return FE_INVALID_ARGS;
            }

            CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
            std::string error;
            if (!m_Api->canImportTexture(desc, error) || !GetArrayDescriptor(desc, true, arrayDesc, error))
            {
                m_Context->error("importTexture(" + std::string(utils::DebugNameToString(desc.debugName)) + "): " + error);
                return FE_UNSUPPORTED;
            }

            CUDA_EXTERNAL_MEMORY_HANDLE_DESC memoryDesc = {};
            if (!m_Api->getTextureMemory(graphicsTexture, memoryDesc, error))
            {
                m_Context->error("importTexture: " + error);
                return FE_GENERIC_ERROR;
            }

            ContextScope scope(*m_Context);
            AutoPtr<Texture> texture = TakeOver(MAKE_RC_OBJ(Texture, *m_Context));
            texture->desc = desc;
            texture->desc.sharedResourceFlags = SharedResourceFlags::None;
            texture->desc.mipLevels = std::max(desc.mipLevels, 1u);
            texture->ownsMemory = false;
            texture->importedResource = graphicsTexture;

            if (!NVRHI_CUDA_CHECK(*m_Context, cuImportExternalMemory(&texture->externalMemory, &memoryDesc)))
            {
                ReleaseHandleAfterFailedImport(memoryDesc);
                return FE_GENERIC_ERROR;
            }

            // The whole mip chain, from the start of the (dedicated) allocation.
            CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapDesc = {};
            mipmapDesc.offset = 0;
            mipmapDesc.arrayDesc = arrayDesc;
            mipmapDesc.numLevels = texture->desc.mipLevels;
            if (!NVRHI_CUDA_CHECK(*m_Context, cuExternalMemoryGetMappedMipmappedArray(&texture->mipmappedArray,
                texture->externalMemory, &mipmapDesc)))
                return FE_GENERIC_ERROR;

            if (!texture->initialize())
                return FE_GENERIC_ERROR;

            *ppTexture = texture.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc&)
        {
            return FE_OUT_OF_MEMORY;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }

    FRESULT InteropDevice::computeQueueWaitForGraphicsQueue(CommandQueue computeQueue, CommandQueue graphicsQueue) noexcept
    {
        if (uint32_t(computeQueue) >= c_NumQueues || uint32_t(graphicsQueue) >= c_NumQueues)
            return FE_INVALID_ARGS;
        if (!m_Api)
            return FE_UNSUPPORTED;

        try
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            CUexternalSemaphore semaphore = m_Api->getGraphicsTimeline(graphicsQueue);
            if (!semaphore)
                return FE_GENERIC_ERROR;

            // Values start at 1 and increase on every signal.
            const uint64_t value = ++m_GraphicsValues[uint32_t(graphicsQueue)];
            if (!m_Api->graphicsSignal(graphicsQueue, value))
                return FE_GENERIC_ERROR;

            ContextScope scope(*m_Context);
            CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS params = {};
            params.params.fence.value = value;
            return NVRHI_CUDA_CHECK(*m_Context, cuWaitExternalSemaphoresAsync(&semaphore, &params, 1,
                m_Device->getQueue(computeQueue)->getStream())) ? FS_OK : FE_GENERIC_ERROR;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }

    FRESULT InteropDevice::graphicsQueueWaitForComputeQueue(CommandQueue graphicsQueue, CommandQueue computeQueue) noexcept
    {
        if (uint32_t(computeQueue) >= c_NumQueues || uint32_t(graphicsQueue) >= c_NumQueues)
            return FE_INVALID_ARGS;
        if (!m_Api)
            return FE_UNSUPPORTED;

        try
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            CUexternalSemaphore semaphore = m_Api->getComputeTimeline(computeQueue);
            if (!semaphore)
                return FE_GENERIC_ERROR;

            const uint64_t value = ++m_ComputeValues[uint32_t(computeQueue)];
            {
                ContextScope scope(*m_Context);
                CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS params = {};
                params.params.fence.value = value;
                if (!NVRHI_CUDA_CHECK(*m_Context, cuSignalExternalSemaphoresAsync(&semaphore, &params, 1,
                    m_Device->getQueue(computeQueue)->getStream())))
                    return FE_GENERIC_ERROR;
            }
            return m_Api->graphicsWait(graphicsQueue, computeQueue, value) ? FS_OK : FE_GENERIC_ERROR;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }

#if !NVRHI_CUDA_WITH_D3D12
    MonoPtr<InteropDevice::Api> CreateD3D12InteropApi(const Context& context, nvrhi::IDevice* graphicsDevice, std::string& error)
    {
        (void)context; (void)graphicsDevice;
        error = "NVRHI was built without the D3D12 backend";
        return nullptr;
    }

    bool GetD3DAdapterIdentity(nvrhi::IDevice* graphicsDevice, AdapterIdentity& outIdentity)
    {
        (void)graphicsDevice; (void)outIdentity;
        return false;
    }
#endif

#if !NVRHI_CUDA_WITH_VULKAN
    MonoPtr<InteropDevice::Api> CreateVulkanInteropApi(const Context& context, nvrhi::IDevice* graphicsDevice, std::string& error)
    {
        (void)context; (void)graphicsDevice;
        error = "NVRHI was built without the Vulkan backend";
        return nullptr;
    }

    bool GetVulkanAdapterIdentity(nvrhi::IDevice* graphicsDevice, AdapterIdentity& outIdentity)
    {
        (void)graphicsDevice; (void)outIdentity;
        return false;
    }
#endif
}
