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

// Interop with D3D12 (ADR 0013).
//
// - Memory: NVRHI creates shared buffers and textures as committed resources on a shared heap and keeps an NT
//   handle for them (getNativeObject(SharedHandle)). They are imported as D3D12_RESOURCE with the DEDICATED flag;
//   the size is the allocation size from GetResourceAllocationInfo. The handle stays NVRHI's.
// - Timelines: shared ID3D12Fence objects, imported as D3D12_FENCE semaphores. Graphics signals and waits go
//   straight to the ID3D12CommandQueue, in order with executeCommandLists.
// The adapter identity of D3D11 devices is here too.

#include "cuda-backend.h"

#if NVRHI_CUDA_WITH_D3D12

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <directx/d3d12.h>
#include <d3d11.h>
#include <dxgi.h>

namespace nvrhi::cuda
{
    namespace
    {
        bool CopyLuid(const LUID& luid, AdapterIdentity& out)
        {
            static_assert(sizeof(LUID) == sizeof(out.luid));
            memcpy(out.luid, &luid, sizeof(out.luid));
            out.hasLuid = true;
            return true;
        }

        class D3D12InteropApi final : public InteropDevice::Api
        {
        public:
            D3D12InteropApi(const Context& context, nvrhi::IDevice* graphicsDevice, ID3D12Device* device)
                : m_Context(context)
                , m_GraphicsDevice(graphicsDevice)
                , m_Device(device)
            {
            }

            ~D3D12InteropApi() override
            {
                for (Timeline& timeline : m_GraphicsTimelines)
                    timeline.destroy();
                for (Timeline& timeline : m_ComputeTimelines)
                    timeline.destroy();
            }

            bool getBufferMemory(IBuffer* buffer, CUDA_EXTERNAL_MEMORY_HANDLE_DESC& out, std::string& error) override
            {
                return getMemory(buffer, out, error);
            }

            bool getTextureMemory(ITexture* texture, CUDA_EXTERNAL_MEMORY_HANDLE_DESC& out, std::string& error) override
            {
                return getMemory(texture, out, error);
            }

            CUexternalSemaphore getGraphicsTimeline(CommandQueue queue) override
            {
                return getTimeline(m_GraphicsTimelines[uint32_t(queue)]);
            }

            CUexternalSemaphore getComputeTimeline(CommandQueue queue) override
            {
                return getTimeline(m_ComputeTimelines[uint32_t(queue)]);
            }

            bool graphicsSignal(CommandQueue queue, uint64_t value) override
            {
                ID3D12CommandQueue* commandQueue = getQueue(queue);
                Timeline& timeline = m_GraphicsTimelines[uint32_t(queue)];
                return commandQueue && timeline.fence && SUCCEEDED(commandQueue->Signal(timeline.fence, value));
            }

            bool graphicsWait(CommandQueue queue, CommandQueue computeQueue, uint64_t value) override
            {
                ID3D12CommandQueue* commandQueue = getQueue(queue);
                Timeline& timeline = m_ComputeTimelines[uint32_t(computeQueue)];
                return commandQueue && timeline.fence && SUCCEEDED(commandQueue->Wait(timeline.fence, value));
            }

        private:
            struct Timeline
            {
                AutoPtr<ID3D12Fence> fence;
                HANDLE handle = nullptr;
                CUexternalSemaphore semaphore = nullptr;

                void destroy()
                {
                    if (semaphore)
                        g_Cuda.cuDestroyExternalSemaphore(semaphore);
                    if (handle)
                        CloseHandle(handle);
                    semaphore = nullptr;
                    handle = nullptr;
                    fence = nullptr;
                }
            };

            const Context& m_Context;
            nvrhi::IDevice* m_GraphicsDevice;   // the interop device keeps the reference
            AutoPtr<ID3D12Device> m_Device;
            std::array<Timeline, c_NumQueues> m_GraphicsTimelines;
            std::array<Timeline, c_NumQueues> m_ComputeTimelines;

            ID3D12CommandQueue* getQueue(CommandQueue queue)
            {
                return static_cast<ID3D12CommandQueue*>(m_GraphicsDevice->getNativeQueue(ObjectTypes::D3D12_CommandQueue, queue));
            }

            bool getMemory(IRHIObject* object, CUDA_EXTERNAL_MEMORY_HANDLE_DESC& out, std::string& error)
            {
                ID3D12Resource* resource = static_cast<ID3D12Resource*>(object->getNativeObject(ObjectTypes::D3D12_Resource));
                HANDLE handle = static_cast<HANDLE>(object->getNativeObject(ObjectTypes::SharedHandle));
                if (!resource || !handle)
                {
                    error = "the resource is not a shared D3D12 resource";
                    return false;
                }

                const D3D12_RESOURCE_DESC desc = resource->GetDesc();
                const D3D12_RESOURCE_ALLOCATION_INFO allocation = m_Device->GetResourceAllocationInfo(0, 1, &desc);

                out = CUDA_EXTERNAL_MEMORY_HANDLE_DESC();
                out.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
                out.handle.win32.handle = handle;
                out.size = allocation.SizeInBytes;
                out.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;     // a committed resource
                return true;
            }

            CUexternalSemaphore getTimeline(Timeline& timeline)
            {
                if (timeline.semaphore)
                    return timeline.semaphore;

                if (FAILED(m_Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&timeline.fence))) ||
                    FAILED(m_Device->CreateSharedHandle(timeline.fence, nullptr, GENERIC_ALL, nullptr, &timeline.handle)))
                {
                    m_Context.error("cannot create a shared D3D12 fence");
                    timeline.destroy();
                    return nullptr;
                }

                CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC desc = {};
                desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE;
                desc.handle.win32.handle = timeline.handle;
                ContextScope scope(m_Context);
                if (!NVRHI_CUDA_CHECK(m_Context, cuImportExternalSemaphore(&timeline.semaphore, &desc)))
                {
                    timeline.destroy();
                    return nullptr;
                }
                return timeline.semaphore;
            }
        };
    }

    MonoPtr<InteropDevice::Api> CreateD3D12InteropApi(const Context& context, nvrhi::IDevice* graphicsDevice, std::string& error)
    {
        ID3D12Device* device = static_cast<ID3D12Device*>(graphicsDevice->getNativeObject(ObjectTypes::D3D12_Device));
        if (!device)
        {
            error = "the graphics device has no ID3D12Device";
            return nullptr;
        }
        return MakeMono<D3D12InteropApi>(context, graphicsDevice, device);
    }

    bool GetD3DAdapterIdentity(nvrhi::IDevice* graphicsDevice, AdapterIdentity& outIdentity)
    {
        if (graphicsDevice->getGraphicsAPI() == GraphicsAPI::D3D12)
        {
            ID3D12Device* device = static_cast<ID3D12Device*>(graphicsDevice->getNativeObject(ObjectTypes::D3D12_Device));
            return device && CopyLuid(device->GetAdapterLuid(), outIdentity);
        }

        if (graphicsDevice->getGraphicsAPI() == GraphicsAPI::D3D11)
        {
            ID3D11Device* device = static_cast<ID3D11Device*>(graphicsDevice->getNativeObject(ObjectTypes::D3D11_Device));
            if (!device)
                return false;
            AutoPtr<IDXGIDevice> dxgiDevice;
            AutoPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC desc = {};
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
                FAILED(adapter->GetDesc(&desc)))
                return false;
            return CopyLuid(desc.AdapterLuid, outIdentity);
        }

        return false;
    }
}

#endif // NVRHI_CUDA_WITH_D3D12
