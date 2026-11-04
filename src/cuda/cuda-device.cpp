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

#include "cuda-backend.h"

#include <nvrhi/common/misc.h>

#include <cstring>

namespace nvrhi::cuda
{
    //////////////////////////////////////////////////////////////////////////
    // Context
    //////////////////////////////////////////////////////////////////////////

    void Context::message(MessageSeverity severity, const std::string& text) const
    {
        if (messageCallback)
            messageCallback->message(severity, ("CUDA: " + text).c_str());
    }

    bool Context::check(CUresult result, const char* what) const
    {
        if (result == CUDA_SUCCESS)
            return true;
        error(std::string(what) + " failed: " + ResultToString(result));
        return false;
    }

    ContextScope::ContextScope(const Context& context)
    {
        if (context.context && g_Cuda.cuCtxPushCurrent(context.context) == CUDA_SUCCESS)
            m_Pushed = true;
    }

    ContextScope::~ContextScope()
    {
        if (m_Pushed)
        {
            CUcontext popped = nullptr;
            g_Cuda.cuCtxPopCurrent(&popped);
        }
    }

    //////////////////////////////////////////////////////////////////////////
    // Creation
    //////////////////////////////////////////////////////////////////////////

    FRESULT CreateDevice(const DeviceDesc& desc, IDevice** ppDevice) noexcept
    {
        *ppDevice = nullptr;

        try
        {
            std::string error;
            if (!LoadDriver(nullptr, error))  // loaded by the graphics factory: this returns its result
            {
                if (desc.messageCallback)
                    desc.messageCallback->message(MessageSeverity::Error, ("CUDA: " + error).c_str());
                return FE_UNSUPPORTED;
            }

            AutoPtr<Device> device = TakeOver(MAKE_RC_OBJ(Device, desc));
            if (!device->initialize(desc, error))
            {
                if (desc.messageCallback)
                    desc.messageCallback->message(MessageSeverity::Error, ("CUDA: " + error).c_str());
                return FE_UNSUPPORTED;
            }

            *ppDevice = device.Detach();
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

    Device::Device(const DeviceDesc& desc)
        : m_TransientPool(m_Context)
    {
        m_Context.messageCallback = desc.messageCallback;
    }

    namespace
    {
        // Selects the CUDA device on the adapter of the graphics device.
        bool FindDeviceForAdapter(const AdapterIdentity& identity, int& outOrdinal, std::string& error)
        {
            int count = 0;
            if (g_Cuda.cuDeviceGetCount(&count) != CUDA_SUCCESS || count == 0)
            {
                error = "no CUDA device";
                return false;
            }

            for (int ordinal = 0; ordinal < count; ++ordinal)
            {
                CUdevice device = 0;
                if (g_Cuda.cuDeviceGet(&device, ordinal) != CUDA_SUCCESS)
                    continue;

                if (identity.hasLuid)
                {
                    char luid[8] = {};
                    unsigned int nodeMask = 0;
                    if (g_Cuda.cuDeviceGetLuid(luid, &nodeMask, device) == CUDA_SUCCESS &&
                        memcmp(luid, identity.luid, sizeof(luid)) == 0)
                    {
                        outOrdinal = ordinal;
                        return true;
                    }
                }

                if (identity.hasUuid)
                {
                    CUuuid uuid = {};
                    if (g_Cuda.cuDeviceGetUuid(&uuid, device) == CUDA_SUCCESS &&
                        memcmp(uuid.bytes, identity.uuid, sizeof(uuid.bytes)) == 0)
                    {
                        outOrdinal = ordinal;
                        return true;
                    }
                }
            }

            error = "the graphics device is not on a CUDA device";
            return false;
        }
    }

    bool Device::initialize(const DeviceDesc& desc, std::string& error)
    {
        int ordinal = desc.deviceOrdinal >= 0 ? desc.deviceOrdinal : 0;

        if (desc.graphicsDevice)
        {
            AdapterIdentity identity;
            const GraphicsAPI api = desc.graphicsDevice->getGraphicsAPI();
            bool known = false;
            if (api == GraphicsAPI::D3D11 || api == GraphicsAPI::D3D12)
                known = GetD3DAdapterIdentity(desc.graphicsDevice, identity);
            else if (api == GraphicsAPI::VULKAN)
                known = GetVulkanAdapterIdentity(desc.graphicsDevice, identity);

            if (!known)
            {
                error = "cannot identify the adapter of the graphics device";
                return false;
            }
            if (!FindDeviceForAdapter(identity, ordinal, error))
                return false;
        }

        CUdevice device = 0;
        CUresult result = g_Cuda.cuDeviceGet(&device, ordinal);
        if (result != CUDA_SUCCESS)
        {
            error = "cuDeviceGet(" + std::to_string(ordinal) + ") failed: " + ResultToString(result);
            return false;
        }

        result = g_Cuda.cuDevicePrimaryCtxRetain(&m_Context.context, device);
        if (result != CUDA_SUCCESS)
        {
            error = "cuDevicePrimaryCtxRetain failed: " + ResultToString(result);
            return false;
        }
        m_PrimaryContextRetained = true;
        m_Context.device = device;
        m_Context.ordinal = ordinal;

        int major = 0, minor = 0, textureAlignment = 512;
        g_Cuda.cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device);
        g_Cuda.cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device);
        g_Cuda.cuDeviceGetAttribute(&textureAlignment, CU_DEVICE_ATTRIBUTE_TEXTURE_ALIGNMENT, device);
        m_Context.computeCapability = uint32_t(major * 10 + minor);
        m_Context.textureAlignment = uint32_t(textureAlignment);

        ContextScope scope(m_Context);

        for (uint32_t q = 0; q < c_NumQueues; ++q)
        {
            m_Queues[q] = MakeMono<Queue>(m_Context, this, CommandQueue(q));
            if (!m_Queues[q]->initialize())
            {
                error = "cannot create the CUDA streams";
                return false;
            }
        }

        if (desc.enableOptix)
        {
            std::string optixError;
            if (!InitializeOptix(m_Context, desc.enableOptixValidation, optixError))
                m_Context.message(MessageSeverity::Info, "OptiX is not available: " + optixError);
        }

        if (desc.graphicsDevice)
        {
            std::string interopError;
            if (!initializeInterop(m_Context, this, desc.graphicsDevice, interopError))
                m_Context.warning("interop with the graphics device is not available: " + interopError);
        }

        char name[256] = {};
        g_Cuda.cuDeviceGetName(name, int(sizeof(name)), device);
        m_Context.message(MessageSeverity::Info, std::string("device ") + std::to_string(ordinal) + ": " + name +
            ", sm_" + std::to_string(m_Context.computeCapability));
        return true;
    }

    Device::~Device()
    {
        if (!m_Context.context)
            return;

        {
            ContextScope scope(m_Context);
            for (auto& queue : m_Queues)
            {
                if (queue)
                    queue->waitForIdle();
            }
            shutdownInterop();
            for (auto& queue : m_Queues)
                queue = nullptr;
            m_TransientPool.destroyAll();
            ShutdownOptix(m_Context);
        }

        if (m_PrimaryContextRetained)
            g_Cuda.cuDevicePrimaryCtxRelease(m_Context.device);
    }

    //////////////////////////////////////////////////////////////////////////
    // Device information
    //////////////////////////////////////////////////////////////////////////

    NativeObject Device::getNativeObject(ObjectType objectType) noexcept
    {
        switch (objectType)
        {
        case ObjectTypes::CUDA_Context:
            return NativeObject(m_Context.context);
        case ObjectTypes::CUDA_Device:
            return NativeObject(uintptr_t(m_Context.device));
        case ObjectTypes::Nvrhi_CUDA_Device:
            return NativeObject(this);
#if NVRHI_WITH_OPTIX
        case ObjectTypes::OptiX_DeviceContext:
            return NativeObject(m_Context.optixContext);
#endif
        default:
            return nullptr;
        }
    }

    NativeObject Device::getNativeQueue(ObjectType objectType, CommandQueue queue) noexcept
    {
        if (objectType != ObjectTypes::CUDA_Stream || uint32_t(queue) >= c_NumQueues || !m_Queues[uint32_t(queue)])
            return nullptr;
        return NativeObject(m_Queues[uint32_t(queue)]->getStream());
    }

    bool Device::isOptixAvailable() noexcept
    {
#if NVRHI_WITH_OPTIX
        return m_Context.optixContext != nullptr;
#else
        return false;
#endif
    }

    bool Device::queryFeatureSupport(Feature feature, void* pInfo, size_t infoSize) noexcept
    {
        (void)pInfo;
        (void)infoSize;

        switch (feature)
        {
        case Feature::ComputeQueue:
        case Feature::CopyQueue:
        case Feature::DeferredCommandLists:
            return true;
        case Feature::SplitBarriers:
        case Feature::AliasingBarriers:
        case Feature::QueueOwnershipTransfer:
            return false;
        case Feature::RayTracingAccelStruct:
        case Feature::RayTracingPipeline:
            return isOptixAvailable();
        default:
            return false;
        }
    }

    FormatSupport Device::queryFormatSupport(Format format) noexcept
    {
        const FormatMapping& mapping = GetFormatMapping(format);
        FormatSupport support = FormatSupport::Buffer;
        if (mapping.arrayFormat != 0)
        {
            support = support | FormatSupport::Texture | FormatSupport::ShaderLoad | FormatSupport::ShaderSample;
            if (mapping.blockSize == 1)
                support = support | FormatSupport::ShaderUavLoad | FormatSupport::ShaderUavStore;
        }
        return support;
    }

    MemoryRequirements& Device::getTextureMemoryRequirements(MemoryRequirements& retVal, ITexture* texture) noexcept
    {
        (void)texture;
        retVal = MemoryRequirements();
        return retVal;
    }

    MemoryRequirements& Device::getBufferMemoryRequirements(MemoryRequirements& retVal, IBuffer* buffer) noexcept
    {
        retVal = MemoryRequirements();
        if (buffer)
        {
            retVal.size = buffer->getDesc().byteSize;
            retVal.alignment = 256;
        }
        return retVal;
    }

    void Device::getTextureTiling(ITexture* texture, uint32_t* numTiles, PackedMipDesc* desc, TileShape* tileShape,
        uint32_t* subresourceTilingsNum, SubresourceTiling* subresourceTilings) noexcept
    {
        (void)texture; (void)numTiles; (void)desc; (void)tileShape; (void)subresourceTilingsNum; (void)subresourceTilings;
        utils::NotSupported();
    }

    void Device::updateTextureTileMappings(ITexture* texture, const TextureTilesMapping* tileMappings,
        uint32_t numTileMappings, CommandQueue executionQueue) noexcept
    {
        (void)texture; (void)tileMappings; (void)numTileMappings; (void)executionQueue;
        utils::NotSupported();
    }

    //////////////////////////////////////////////////////////////////////////
    // Command lists and queues
    //////////////////////////////////////////////////////////////////////////

    CommandListHandle Device::createCommandList(const CommandListParameters& params)
    {
        if (uint32_t(params.queueType) >= c_NumQueues)
            return nullptr;
        return TakeOver(MAKE_RC_OBJ(CommandList, this, m_Context, params));
    }

    uint64_t Device::executeCommandLists(nvrhi::ICommandList* const* pCommandLists, size_t numCommandLists,
        CommandQueue executionQueue) noexcept
    {
        // A submission without fences (ADR 0015).
        SubmitDesc desc;
        desc.commandLists = pCommandLists;
        desc.numCommandLists = uint32_t(numCommandLists);
        desc.queue = executionQueue;
        return submit(desc);
    }

    void Device::queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue, uint64_t instance) noexcept
    {
        if (uint32_t(waitQueue) >= c_NumQueues || uint32_t(executionQueue) >= c_NumQueues)
            return;

        Queue* waiting = m_Queues[uint32_t(waitQueue)].Get();
        Queue* executing = m_Queues[uint32_t(executionQueue)].Get();
        CUevent event = executing->getSubmissionEvent(instance);
        if (!event || waiting == executing)
            return;

        ContextScope scope(m_Context);
        NVRHI_CUDA_CHECK(m_Context, cuStreamWaitEvent(waiting->getStream(), event, 0));
    }

    uint64_t Device::queueGetCompletedInstance(CommandQueue queue) noexcept
    {
        if (uint32_t(queue) >= c_NumQueues)
            return 0;
        ContextScope scope(m_Context);
        return m_Queues[uint32_t(queue)]->updateCompleted();
    }

    bool Device::waitForIdle() noexcept
    {
        ContextScope scope(m_Context);
        for (auto& queue : m_Queues)
            queue->waitForIdle();
        return true;
    }

    void Device::runGarbageCollection() noexcept
    {
        ContextScope scope(m_Context);
        for (auto& queue : m_Queues)
            queue->updateCompleted();
    }

    //////////////////////////////////////////////////////////////////////////
    // Queries
    //////////////////////////////////////////////////////////////////////////

    EventQuery::~EventQuery()
    {
        if (event)
        {
            ContextScope scope(m_Context);
            g_Cuda.cuEventDestroy(event);
        }
    }

    TimerQuery::~TimerQuery()
    {
        ContextScope scope(m_Context);
        if (begin)
            g_Cuda.cuEventDestroy(begin);
        if (end)
            g_Cuda.cuEventDestroy(end);
    }

    EventQueryHandle Device::createEventQuery()
    {
        ContextScope scope(m_Context);
        AutoPtr<EventQuery> query = TakeOver(MAKE_RC_OBJ(EventQuery, m_Context));
        if (!NVRHI_CUDA_CHECK(m_Context, cuEventCreate(&query->event, CU_EVENT_DISABLE_TIMING)))
            return nullptr;
        return query;
    }

    void Device::setEventQuery(IEventQuery* _query, CommandQueue queue) noexcept
    {
        EventQuery* query = checked_cast<EventQuery*>(_query);
        if (!query || uint32_t(queue) >= c_NumQueues)
            return;
        ContextScope scope(m_Context);
        query->started = NVRHI_CUDA_CHECK(m_Context, cuEventRecord(query->event, m_Queues[uint32_t(queue)]->getStream()));
    }

    bool Device::pollEventQuery(IEventQuery* _query) noexcept
    {
        EventQuery* query = checked_cast<EventQuery*>(_query);
        if (!query || !query->started)
            return false;
        ContextScope scope(m_Context);
        return g_Cuda.cuEventQuery(query->event) == CUDA_SUCCESS;
    }

    void Device::waitEventQuery(IEventQuery* _query) noexcept
    {
        EventQuery* query = checked_cast<EventQuery*>(_query);
        if (!query || !query->started)
            return;
        ContextScope scope(m_Context);
        NVRHI_CUDA_CHECK(m_Context, cuEventSynchronize(query->event));
    }

    void Device::resetEventQuery(IEventQuery* _query) noexcept
    {
        EventQuery* query = checked_cast<EventQuery*>(_query);
        if (query)
            query->started = false;
    }

    TimerQueryHandle Device::createTimerQuery()
    {
        ContextScope scope(m_Context);
        AutoPtr<TimerQuery> query = TakeOver(MAKE_RC_OBJ(TimerQuery, m_Context));
        if (!NVRHI_CUDA_CHECK(m_Context, cuEventCreate(&query->begin, CU_EVENT_DEFAULT)) ||
            !NVRHI_CUDA_CHECK(m_Context, cuEventCreate(&query->end, CU_EVENT_DEFAULT)))
            return nullptr;
        return query;
    }

    bool Device::pollTimerQuery(ITimerQuery* _query) noexcept
    {
        TimerQuery* query = checked_cast<TimerQuery*>(_query);
        if (!query || !query->started)
            return false;
        if (query->resolved)
            return true;
        ContextScope scope(m_Context);
        return g_Cuda.cuEventQuery(query->end) == CUDA_SUCCESS;
    }

    float Device::getTimerQueryTime(ITimerQuery* _query) noexcept
    {
        TimerQuery* query = checked_cast<TimerQuery*>(_query);
        if (!query || !query->started)
            return 0.f;
        if (!query->resolved)
        {
            ContextScope scope(m_Context);
            float milliseconds = 0.f;
            if (g_Cuda.cuEventSynchronize(query->end) == CUDA_SUCCESS &&
                g_Cuda.cuEventElapsedTime(&milliseconds, query->begin, query->end) == CUDA_SUCCESS)
            {
                query->time = milliseconds * 0.001f;
            }
            query->resolved = true;
        }
        return query->time;
    }

    void Device::resetTimerQuery(ITimerQuery* _query) noexcept
    {
        TimerQuery* query = checked_cast<TimerQuery*>(_query);
        if (query)
        {
            query->started = false;
            query->resolved = false;
            query->time = 0.f;
        }
    }
}
