/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
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

#include "validation-backend.h"

#include "../common/utils-internal.h"
#include <nvrhi/common/misc.h>

#include <sstream>

namespace nvrhi::validation
{
    template<typename T>
    static std::unordered_set<T> SetDifference(std::unordered_set<T> const& a, std::unordered_set<T> const& b)
    {
        std::unordered_set<T> result = a;
        for (T const& item : b)
            result.erase(item);
        return result;
    }
    
    template<typename T>
    static std::unordered_set<T> SetIntersection(std::unordered_set<T> const& a, std::unordered_set<T> const& b)
    {
        std::unordered_set<T> result;
        for (T const& item : a)
        {
            if (b.find(item) != b.end())
                result.insert(item);
        }
        return result;
    }
    
    template<typename T>
    static void SetUnionInplace(std::unordered_set<T>& a, std::unordered_set<T> const& b)
    {
        for (T const& item : b)
            a.insert(item);
    }

    FRESULT nvrhiCreateValidationLayer(IDevice* underlyingDevice, IDevice** ppDevice) noexcept
    {
        if (!ppDevice)
            return FE_INVALID_ARGS;
        *ppDevice = nullptr;
        if (!underlyingDevice)
            return FE_INVALID_ARGS;

        try
        {
            *ppDevice = MAKE_RC_OBJ(DeviceWrapper, underlyingDevice);
        }
        catch (const std::bad_alloc&)
        {
            return FE_OUT_OF_MEMORY;
        }
        return *ppDevice ? FS_OK : FE_GENERIC_ERROR;
    }

    DeviceWrapper::DeviceWrapper(IDevice* device)
        : m_Device(device)
        , m_MessageCallback(device->getMessageCallback())
    {

    }

    void DeviceWrapper::error(const std::string& messageText) const
    {
        m_MessageCallback->message(MessageSeverity::Error, messageText.c_str());
    }

    void DeviceWrapper::warning(const std::string& messageText) const
    {
        m_MessageCallback->message(MessageSeverity::Warning, messageText.c_str());
    }

    NativeObject DeviceWrapper::getNativeObject(ObjectType objectType) noexcept
    {
        return m_Device->getNativeObject(objectType);
    }

    FRESULT DeviceWrapper::createHeap(const HeapDesc& d, IHeap** ppHeap) noexcept
    try
    {
        if (!utils::ResetOutput(ppHeap))
            return FE_INVALID_ARGS;

        if (d.capacity == 0)
        {
            error("Cannot create a Heap with capacity = 0");
            return FE_INVALID_ARGS;
        }

        HeapDesc patchedDesc = d;
        if (patchedDesc.debugName.empty())
            patchedDesc.debugName = utils::GenerateHeapDebugName(patchedDesc);

        return m_Device->createHeap(patchedDesc, ppHeap);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppHeap);
    }

    FRESULT DeviceWrapper::createTexture(const TextureDesc& d, ITexture** ppTexture) noexcept
    try
    {
        if (!utils::ResetOutput(ppTexture))
            return FE_INVALID_ARGS;

        bool anyErrors = false;

        switch (d.dimension)
        {
        case TextureDimension::Texture1D:
        case TextureDimension::Texture1DArray:
        case TextureDimension::Texture2D:
        case TextureDimension::Texture2DArray:
        case TextureDimension::TextureCube:
        case TextureDimension::TextureCubeArray:
        case TextureDimension::Texture2DMS:
        case TextureDimension::Texture2DMSArray:
        case TextureDimension::Texture3D:
            break;

        case TextureDimension::Unknown:
        default:
            error("Unknown texture dimension");
            return FE_INVALID_ARGS;
        }

        const char* dimensionStr = utils::TextureDimensionToString(d.dimension);
        const char* debugName = utils::DebugNameToString(d.debugName);

        if (d.width == 0 || d.height == 0 || d.depth == 0 || d.arraySize == 0 || d.mipLevels == 0)
        {
            std::stringstream ss;
            ss << dimensionStr << " " << debugName << ": width(" << d.width << "), height(" << d.height << "), depth(" << d.depth
                << "), arraySize(" << d.arraySize << ") and mipLevels(" << d.mipLevels << ") must not be zero";
            error(ss.str());
            return FE_INVALID_ARGS;
        }

        switch (d.dimension)  // NOLINT(clang-diagnostic-switch-enum)
        {
        case TextureDimension::Texture1D:
        case TextureDimension::Texture1DArray:
            if (d.height != 1)
            {
                std::stringstream ss;
                ss << dimensionStr << " " << debugName << ": height(" << d.height << ") must be equal to 1";
                error(ss.str());
                anyErrors = true;
            }
            break;
        default:;
        }

        switch (d.dimension)  // NOLINT(clang-diagnostic-switch-enum)
        {
        case TextureDimension::Texture1D:
        case TextureDimension::Texture1DArray:
        case TextureDimension::Texture2D:
        case TextureDimension::Texture2DArray:
        case TextureDimension::TextureCube:
        case TextureDimension::TextureCubeArray:
        case TextureDimension::Texture2DMS:
        case TextureDimension::Texture2DMSArray:
            if (d.depth != 1)
            {
                std::stringstream ss;
                ss << dimensionStr << " " << debugName << ": depth(" << d.depth << ") must be equal to 1";
                error(ss.str());
                anyErrors = true;
            }
            break;
        default:;
        }

        switch (d.dimension)  // NOLINT(clang-diagnostic-switch-enum)
        {
        case TextureDimension::Texture1D:
        case TextureDimension::Texture2D:
        case TextureDimension::Texture2DMS:
        case TextureDimension::Texture3D:
            if (d.arraySize != 1)
            {
                std::stringstream ss;
                ss << dimensionStr << " " << debugName << ": arraySize(" << d.arraySize << ") must be equal to 1";
                error(ss.str());
                anyErrors = true;
            }
            break;
        case TextureDimension::TextureCube:
            if (d.arraySize != 6)
            {
                std::stringstream ss;
                ss << dimensionStr << " " << debugName << ": arraySize(" << d.arraySize << ") must be equal to 6";
                error(ss.str());
                anyErrors = true;
            }
            break;
        case TextureDimension::TextureCubeArray:
            if ((d.arraySize % 6) != 0)
            {
                std::stringstream ss;
                ss << dimensionStr << " " << debugName << ": arraySize(" << d.arraySize << ") must be a multiple of 6";
                error(ss.str());
                anyErrors = true;
            }
            break;
        default:;
        }

        switch (d.dimension)  // NOLINT(clang-diagnostic-switch-enum)
        {
        case TextureDimension::Texture1D:
        case TextureDimension::Texture1DArray:
        case TextureDimension::Texture2D:
        case TextureDimension::Texture2DArray:
        case TextureDimension::TextureCube:
        case TextureDimension::TextureCubeArray:
        case TextureDimension::Texture3D:
            if (d.sampleCount != 1)
            {
                std::stringstream ss;
                ss << dimensionStr << " " << debugName << ": sampleCount(" << d.sampleCount << ") must be equal to 1";
                error(ss.str());
                anyErrors = true;
            }
            break;
        case TextureDimension::Texture2DMS:
        case TextureDimension::Texture2DMSArray:
            if (d.sampleCount != 2 && d.sampleCount != 4 && d.sampleCount != 8)
            {
                std::stringstream ss;
                ss << dimensionStr << " " << debugName << ": sampleCount(" << d.sampleCount << ") must be equal to 2, 4 or 8";
                error(ss.str());
                anyErrors = true;
            }
            if (d.isUAV)
            {
                std::stringstream ss;
                ss << dimensionStr << " " << debugName << ": multi-sampled textures cannot have UAVs (isUAV flag)";
                error(ss.str());
                anyErrors = true;
            }
            break;
        default:;
        }

        if (d.isVirtual && !m_Device->queryFeatureSupport(Feature::VirtualResources))
        {
            std::stringstream ss;
            ss << dimensionStr << " " << debugName << ": The device does not support virtual resources";
            error(ss.str());
            anyErrors = true;
        }

        if (d.keepInitialState && d.initialState == ResourceStates::Unknown)
        {
            std::stringstream ss;
            ss << dimensionStr << " " << debugName << " has initialState = Unknown, which is incompatible with keepInitialState = true.";
            error(ss.str());
            anyErrors = true;
        }
        
        if(anyErrors)
            return FE_INVALID_ARGS;
        
        TextureDesc patchedDesc = d;
        if (patchedDesc.debugName.empty())
            patchedDesc.debugName = utils::GenerateTextureDebugName(patchedDesc);

        return m_Device->createTexture(patchedDesc, ppTexture);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppTexture);
    }

    void DeviceWrapper::getTextureTiling(ITexture* texture, uint32_t* numTiles, PackedMipDesc* desc, TileShape* tileShape, uint32_t* subresourceTilingsNum, SubresourceTiling* subresourceTilings) noexcept
    {
        m_Device->getTextureTiling(texture, numTiles, desc, tileShape, subresourceTilingsNum, subresourceTilings);
    }

    void DeviceWrapper::updateTextureTileMappings(ITexture* texture, const TextureTilesMapping* tileMappings, uint32_t numTileMappings, CommandQueue executionQueue) noexcept
    {
        m_Device->updateTextureTileMappings(texture, tileMappings, numTileMappings, executionQueue);
    }

    FRESULT DeviceWrapper::createSamplerFeedbackTexture(ITexture* pairedTexture, const SamplerFeedbackTextureDesc& desc, ISamplerFeedbackTexture** ppTexture) noexcept
    try
    {
        if (!utils::ResetOutput(ppTexture))
            return FE_INVALID_ARGS;

        const GraphicsAPI graphicsApi = m_Device->getGraphicsAPI();
        if (graphicsApi != GraphicsAPI::D3D12)
        {
            std::stringstream ss;
            ss << "The current graphics API (" << utils::GraphicsAPIToString(m_Device->getGraphicsAPI()) << ") "
                "doesn't support createSamplerFeedbackTexture";
            error(ss.str());
            return FE_INVALID_ARGS;
        }

        return m_Device->createSamplerFeedbackTexture(pairedTexture, desc, ppTexture);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppTexture);
    }

    FRESULT DeviceWrapper::createSamplerFeedbackForNativeTexture(ObjectType objectType, NativeObject texture, ITexture* pairedTexture, ISamplerFeedbackTexture** ppTexture) noexcept
    try
    {
        if (!utils::ResetOutput(ppTexture))
            return FE_INVALID_ARGS;

        const GraphicsAPI graphicsApi = m_Device->getGraphicsAPI();
        if (graphicsApi != GraphicsAPI::D3D12)
        {
            std::stringstream ss;
            ss << "The current graphics API (" << utils::GraphicsAPIToString(m_Device->getGraphicsAPI()) << ") "
                "doesn't support createSamplerFeedbackForNativeTexture";
            error(ss.str());
            return FE_INVALID_ARGS;
        }

        return m_Device->createSamplerFeedbackForNativeTexture(objectType, texture, pairedTexture, ppTexture);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppTexture);
    }

    MemoryRequirements DeviceWrapper::getTextureMemoryRequirements(ITexture* texture)
    {
        if (texture == nullptr)
        {
            error("getTextureMemoryRequirements: texture is NULL");
            return MemoryRequirements();
        }

        MemoryRequirements memReq;

        m_Device->getTextureMemoryRequirements(memReq, texture);

        if (memReq.size == 0)
        {
            std::stringstream ss;
            ss << "Invalid texture " << utils::DebugNameToString(texture->getDesc().debugName) << ": "
                "getTextureMemoryRequirements returned zero size";

            error(ss.str());
        }

        return memReq;
    }

    bool DeviceWrapper::bindTextureMemory(ITexture* texture, IHeap* heap, uint64_t offset) noexcept
    {
        if (texture == nullptr)
        {
            error("bindTextureMemory: texture is NULL");
            return false;
        }

        if (heap == nullptr)
        {
            error("bindTextureMemory: heap is NULL");
            return false;
        }

        const HeapDesc& heapDesc = heap->getDesc();
        const TextureDesc& textureDesc = texture->getDesc();

        if (!textureDesc.isVirtual)
        {
            std::stringstream ss;
            ss << "Cannot perform bindTextureMemory on texture " << utils::DebugNameToString(textureDesc.debugName)
                << " because it was created with isVirtual = false";

            error(ss.str());
            return false;
        }

        MemoryRequirements memReq;

        m_Device->getTextureMemoryRequirements(memReq, texture);

        if (offset + memReq.size > heapDesc.capacity)
        {
            std::stringstream ss;
            ss << "Texture " << utils::DebugNameToString(textureDesc.debugName)
                << " does not fit into heap " << utils::DebugNameToString(heapDesc.debugName)
                << " at offset " << offset << " because it requires " << memReq.size << " bytes,"
                << " and the heap capacity is " << heapDesc.capacity << " bytes";

            error(ss.str());
            return false;
        }

        if (memReq.alignment != 0 && (offset % memReq.alignment) != 0)
        {
            std::stringstream ss;
            ss << "Texture " << utils::DebugNameToString(textureDesc.debugName)
                << " is placed in heap " << utils::DebugNameToString(heapDesc.debugName)
                << " at invalid alignment: required alignment to " << memReq.alignment << " bytes,"
                << " actual offset is " << offset << " bytes";

            error(ss.str());
            return false;
        }

        return m_Device->bindTextureMemory(texture, heap, offset);
    }
    
    FRESULT DeviceWrapper::createHandleForNativeTexture(ObjectType objectType, NativeObject texture, const TextureDesc& desc, ITexture** ppTexture) noexcept
    try
    {
        if (!utils::ResetOutput(ppTexture))
            return FE_INVALID_ARGS;

        return m_Device->createHandleForNativeTexture(objectType, texture, desc, ppTexture);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppTexture);
    }

    FRESULT DeviceWrapper::createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess, IStagingTexture** ppStagingTexture) noexcept
    try
    {
        if (!utils::ResetOutput(ppStagingTexture))
            return FE_INVALID_ARGS;

        TextureDesc patchedDesc = d;
        if (patchedDesc.debugName.empty())
            patchedDesc.debugName = utils::GenerateTextureDebugName(patchedDesc);

        return m_Device->createStagingTexture(patchedDesc, cpuAccess, ppStagingTexture);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppStagingTexture);
    }

    void * DeviceWrapper::mapStagingTexture(IStagingTexture* tex, const TextureSlice& slice, CpuAccessMode cpuAccess, size_t& outRowPitch) noexcept
    {
        return m_Device->mapStagingTexture(tex, slice, cpuAccess, outRowPitch);
    }

    void DeviceWrapper::unmapStagingTexture(IStagingTexture* tex) noexcept
    {
        m_Device->unmapStagingTexture(tex);
    }

    FRESULT DeviceWrapper::createBuffer(const BufferDesc& d, IBuffer** ppBuffer) noexcept
    try
    {
        if (!utils::ResetOutput(ppBuffer))
            return FE_INVALID_ARGS;

        BufferDesc patchedDesc = d;
        if (patchedDesc.debugName.empty())
            patchedDesc.debugName = utils::GenerateBufferDebugName(patchedDesc);
        
        if (d.isVolatile && !d.isConstantBuffer)
        {
            std::stringstream ss;
            ss << "Buffer " << patchedDesc.debugName << " is volatile but is not a constant buffer. Only constant buffers can be made volatile.";
            error(ss.str());
            return FE_INVALID_ARGS;
        }

        if (d.isVolatile && d.maxVersions == 0)
        {
            std::stringstream ss;
            ss << "Volatile constant buffer " << patchedDesc.debugName << " has maxVersions = 0";
            error(ss.str());
            return FE_INVALID_ARGS;
        }

        if (d.isVolatile && (d.isVertexBuffer || d.isIndexBuffer || d.isDrawIndirectArgs || d.canHaveUAVs || d.isAccelStructBuildInput || d.isAccelStructStorage || d.isShaderBindingTable || d.isVirtual))
        {
            std::stringstream ss;
            ss << "Buffer " << patchedDesc.debugName << " is volatile but has unsupported usage flags:";
            if (d.isVertexBuffer) ss << " IsVertexBuffer";
            if (d.isIndexBuffer) ss << " IsIndexBuffer";
            if (d.isDrawIndirectArgs) ss << " IsDrawIndirectArgs";
            if (d.canHaveUAVs) ss << " CanHaveUAVs";
            if (d.isAccelStructBuildInput) ss << " IsAccelStructBuildInput";
            if (d.isAccelStructStorage) ss << " IsAccelStructStorage";
            if (d.isShaderBindingTable) ss << " IsShaderBindingTable";
            if (d.isVirtual) ss << " IsVirtual";
            ss << "." << std::endl << "Only constant buffers can be made volatile, and volatile buffers cannot be virtual.";
            error(ss.str());
            return FE_INVALID_ARGS;
        }

        if (d.isVolatile && d.cpuAccess != CpuAccessMode::None)
        {
            std::stringstream ss;
            ss << "Volatile constant buffer " << patchedDesc.debugName << " must have cpuAccess set to None. Write-discard access is implied.";
            error(ss.str());
            return FE_INVALID_ARGS;
        }

        if (d.isVirtual && !m_Device->queryFeatureSupport(Feature::VirtualResources))
        {
            error("The device does not support virtual resources");
            return FE_INVALID_ARGS;
        }

        if (d.keepInitialState && d.initialState == ResourceStates::Unknown)
        {
            std::stringstream ss;
            ss << "Buffer " << patchedDesc.debugName << " has initialState = Unknown, which is incompatible with keepInitialState = true.";
            error(ss.str());
            return FE_INVALID_ARGS;
        }

        return m_Device->createBuffer(patchedDesc, ppBuffer);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppBuffer);
    }

    void * DeviceWrapper::mapBuffer(IBuffer* b, CpuAccessMode mapFlags) noexcept
    {
        return m_Device->mapBuffer(b, mapFlags);
    }

    void DeviceWrapper::unmapBuffer(IBuffer* b) noexcept
    {
        m_Device->unmapBuffer(b);
    }

    MemoryRequirements DeviceWrapper::getBufferMemoryRequirements(IBuffer* buffer)
    {
        if (buffer == nullptr)
        {
            error("getBufferMemoryRequirements: buffer is NULL");
            return MemoryRequirements();
        }

        MemoryRequirements memReq;

        m_Device->getBufferMemoryRequirements(memReq, buffer);

        if (memReq.size == 0)
        {
            std::stringstream ss;
            ss << "Invalid buffer " << utils::DebugNameToString(buffer->getDesc().debugName) << ": "
                "getBufferMemoryRequirements returned zero size";

            error(ss.str());
        }

        return memReq;
    }

    bool DeviceWrapper::bindBufferMemory(IBuffer* buffer, IHeap* heap, uint64_t offset) noexcept
    {
        if (buffer == nullptr)
        {
            error("bindBufferMemory: texture is NULL");
            return false;
        }

        if (heap == nullptr)
        {
            error("bindBufferMemory: heap is NULL");
            return false;
        }

        const HeapDesc& heapDesc = heap->getDesc();
        const BufferDesc& bufferDesc = buffer->getDesc();

        if (!bufferDesc.isVirtual)
        {
            std::stringstream ss;
            ss << "Cannot perform bindBufferMemory on buffer " << utils::DebugNameToString(bufferDesc.debugName)
                << " because it was created with isVirtual = false";

            error(ss.str());
            return false;
        }

        MemoryRequirements memReq;

        m_Device->getBufferMemoryRequirements(memReq, buffer);

        if (offset + memReq.size > heapDesc.capacity)
        {
            std::stringstream ss;
            ss << "Buffer " << utils::DebugNameToString(bufferDesc.debugName)
                << " does not fit into heap " << utils::DebugNameToString(heapDesc.debugName)
                << " at offset " << offset << " because it requires " << memReq.size << " bytes,"
                << " and the heap capacity is " << heapDesc.capacity << " bytes";

            error(ss.str());
            return false;
        }

        if (memReq.alignment != 0 && (offset % memReq.alignment) != 0)
        {
            std::stringstream ss;
            ss << "Buffer " << utils::DebugNameToString(bufferDesc.debugName)
                << " is placed in heap " << utils::DebugNameToString(heapDesc.debugName)
                << " at invalid alignment: required alignment to " << memReq.alignment << " bytes,"
                << " actual offset is " << offset << " bytes";

            error(ss.str());
            return false;
        }

        return m_Device->bindBufferMemory(buffer, heap, offset);
    }

    FRESULT DeviceWrapper::createHandleForNativeBuffer(ObjectType objectType, NativeObject buffer, const BufferDesc& desc, IBuffer** ppBuffer) noexcept
    try
    {
        if (!utils::ResetOutput(ppBuffer))
            return FE_INVALID_ARGS;

        return m_Device->createHandleForNativeBuffer(objectType, buffer, desc, ppBuffer);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppBuffer);
    }

    FRESULT DeviceWrapper::createShader(const ShaderDesc& d, const void* binary, const size_t binarySize, IShader** ppShader) noexcept
    try
    {
        if (!utils::ResetOutput(ppShader))
            return FE_INVALID_ARGS;

        return m_Device->createShader(d, binary, binarySize, ppShader);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppShader);
    }
    
    FRESULT DeviceWrapper::createShaderSpecialization(IShader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants, IShader** ppShader) noexcept
    try
    {
        if (!utils::ResetOutput(ppShader))
            return FE_INVALID_ARGS;

        if (!m_Device->queryFeatureSupport(Feature::ShaderSpecializations))
        {
            std::stringstream ss;
            ss << "The current graphics API (" << utils::GraphicsAPIToString(m_Device->getGraphicsAPI()) << ") "
                "doesn't support shader specializations";
            error(ss.str());
            return FE_INVALID_ARGS;
        }

        if (constants == nullptr || numConstants == 0)
        {
            error("Both 'constants' and 'numConstatns' must be non-zero in createShaderSpecialization");
            return FE_INVALID_ARGS;
        }

        if (baseShader == nullptr)
        {
            error("baseShader must be non-null in createShaderSpecialization");
            return FE_INVALID_ARGS;
        }

        return m_Device->createShaderSpecialization(baseShader, constants, numConstants, ppShader);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppShader);
    }

    FRESULT DeviceWrapper::createShaderLibrary(const void* binary, const size_t binarySize, IShaderLibrary** ppShaderLibrary) noexcept
    try
    {
        if (!utils::ResetOutput(ppShaderLibrary))
            return FE_INVALID_ARGS;

        return m_Device->createShaderLibrary(binary, binarySize, ppShaderLibrary);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppShaderLibrary);
    }
    
    FRESULT DeviceWrapper::createSampler(const SamplerDesc& d, ISampler** ppSampler) noexcept
    try
    {
        if (!utils::ResetOutput(ppSampler))
            return FE_INVALID_ARGS;

        return m_Device->createSampler(d, ppSampler);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppSampler);
    }

    FRESULT DeviceWrapper::createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, IShader* vertexShader, IInputLayout** ppInputLayout) noexcept
    try
    {
        if (!utils::ResetOutput(ppInputLayout))
            return FE_INVALID_ARGS;

        return m_Device->createInputLayout(d, attributeCount, vertexShader, ppInputLayout);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppInputLayout);
    }

    FRESULT DeviceWrapper::createEventQuery(IEventQuery** ppQuery) noexcept
    try
    {
        if (!utils::ResetOutput(ppQuery))
            return FE_INVALID_ARGS;

        return m_Device->createEventQuery(ppQuery);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppQuery);
    }

    void DeviceWrapper::setEventQuery(IEventQuery* query, CommandQueue queue) noexcept
    {
        m_Device->setEventQuery(query, queue);
    }

    bool DeviceWrapper::pollEventQuery(IEventQuery* query) noexcept
    {
        return m_Device->pollEventQuery(query);
    }

    void DeviceWrapper::waitEventQuery(IEventQuery* query) noexcept
    {
        m_Device->waitEventQuery(query);
    }

    void DeviceWrapper::resetEventQuery(IEventQuery* query) noexcept
    {
        m_Device->resetEventQuery(query);
    }

    FRESULT DeviceWrapper::createTimerQuery(ITimerQuery** ppQuery) noexcept
    try
    {
        if (!utils::ResetOutput(ppQuery))
            return FE_INVALID_ARGS;

        return m_Device->createTimerQuery(ppQuery);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppQuery);
    }

    bool DeviceWrapper::pollTimerQuery(ITimerQuery* query) noexcept
    {
        return m_Device->pollTimerQuery(query);
    }

    float DeviceWrapper::getTimerQueryTime(ITimerQuery* query) noexcept
    {
        return m_Device->getTimerQueryTime(query);
    }

    void DeviceWrapper::resetTimerQuery(ITimerQuery* query) noexcept
    {
        return m_Device->resetTimerQuery(query);
    }

    GraphicsAPI DeviceWrapper::getGraphicsAPI() noexcept
    {
        return m_Device->getGraphicsAPI();
    }

    FRESULT DeviceWrapper::createFramebuffer(const FramebufferDesc& desc, IFramebuffer** ppFramebuffer) noexcept
    try
    {
        if (!utils::ResetOutput(ppFramebuffer))
            return FE_INVALID_ARGS;

        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t arraySize = 0;
        uint32_t sampleCount = 0;

        if (desc.depthAttachment.texture)
        {
            TextureDesc const& d = desc.depthAttachment.texture->getDesc();

            TextureSubresourceSet const subresources = desc.depthAttachment.subresources.resolve(d, true);

            uint32_t const mipWidth = std::max(d.width >> subresources.baseMipLevel, 1u);
            uint32_t const mipHeight = std::max(d.height >> subresources.baseMipLevel, 1u);

            width = mipWidth;
            height = mipHeight;
            arraySize = subresources.numArraySlices;
            sampleCount = d.sampleCount;

            if (!d.isRenderTarget)
            {
                std::stringstream ss;
                ss << "Depth attachment texture " << utils::DebugNameToString(d.debugName)
                    << " must be created with isRenderTarget = true";
                error(ss.str());
                return FE_INVALID_ARGS;
            }

            FormatInfo const& formatInfo = getFormatInfo(d.format);
            if (formatInfo.kind != FormatKind::DepthStencil)
            {
                std::stringstream ss;
                ss << "Depth attachment texture " << utils::DebugNameToString(d.debugName) << " has a format "
                    << formatInfo.name << " that does not support depth or stencil";
                error(ss.str());
                return FE_INVALID_ARGS;
            }
        }

        for (size_t i = 0; i < desc.colorAttachments.size(); ++i)
        {
            FramebufferAttachment const& att = desc.colorAttachments[i];
            if (!att.texture)
            {
                std::stringstream ss;
                ss << "Color attachment " << i << " is NULL";
                error(ss.str());
                return FE_INVALID_ARGS;
            }

            TextureDesc const& d = att.texture->getDesc();
            if (!d.isRenderTarget)
            {
                std::stringstream ss;
                ss << "Color attachment  " << i << " texture " << utils::DebugNameToString(d.debugName)
                    << " must be created with isRenderTarget = true";
                error(ss.str());
                return FE_INVALID_ARGS;
            }

            TextureSubresourceSet const subresources = att.subresources.resolve(d, true);

            uint32_t const mipWidth = std::max(d.width >> subresources.baseMipLevel, 1u);
            uint32_t const mipHeight = std::max(d.height >> subresources.baseMipLevel, 1u);

            if (!width || !height || !arraySize || !sampleCount)
            {
                width = mipWidth;
                height = mipHeight;
                arraySize = subresources.numArraySlices;
                sampleCount = d.sampleCount;
            }
            else
            {
                if (width != mipWidth ||
                    height != mipHeight ||
                    arraySize != subresources.numArraySlices ||
                    sampleCount != d.sampleCount)
                {
                    std::stringstream ss;
                    ss << "Color attachment " << i << " texture " << utils::DebugNameToString(d.debugName)
                        << " has dimensions " << mipWidth << "x" << mipHeight << " with " << subresources.numArraySlices
                        << " array slice(s) and " << sampleCount << " sample(s), which do not match the dimensions of depth"
                        << " or previous color attachments " << width << "x" << height << " with " << arraySize
                        << " array slice(s) and " << sampleCount << " sample(s)";
                    error(ss.str());
                    return FE_INVALID_ARGS;
                }
            }

            FormatInfo const& formatInfo = getFormatInfo(d.format);
            if (!formatInfo.hasRed)
            {
                std::stringstream ss;
                ss << "Color attachment " << i << " texture " << utils::DebugNameToString(d.debugName) << " has a format "
                    << formatInfo.name << " that does not have color or alpha channels";
                error(ss.str());
                return FE_INVALID_ARGS;
            }

            if (formatInfo.blockSize > 1)
            {
                std::stringstream ss;
                ss << "Color attachment " << i << " texture " << utils::DebugNameToString(d.debugName) << " has a format "
                    << formatInfo.name << " that is block-compressed. Block-compressed formats are not supported for render targets.";
                error(ss.str());
                return FE_INVALID_ARGS;
            }
        }

        if (desc.shadingRateAttachment.texture)
        {
            TextureDesc const& d = desc.shadingRateAttachment.texture->getDesc();

            // The shading rate attachment must be an R8_UINT format and have a single sample.
            // Its dimensions may be different (smaller) from the color attachments.
            // Not sure about array size or how VRS works with rendering into a texture array.

            if (d.format != Format::R8_UINT)
            {
                FormatInfo const& formatInfo = getFormatInfo(d.format);

                std::stringstream ss;
                ss << "Shading rate attachment texture " << utils::DebugNameToString(d.debugName) << " has a format "
                    << formatInfo.name << " that is not R8_UINT";
                error(ss.str());
                return FE_INVALID_ARGS;
            }

            if (d.sampleCount != 1)
            {
                std::stringstream ss;
                ss << "Shading rate attachment texture " << utils::DebugNameToString(d.debugName) << " has a sample count "
                    << d.sampleCount << " that is not 1";
                error(ss.str());
                return FE_INVALID_ARGS;
            }

            if (!d.isShadingRateSurface)
            {
                std::stringstream ss;
                ss << "Shading rate attachment texture " << utils::DebugNameToString(d.debugName)
                    << " must be created with isShadingRateSurface = true";
                error(ss.str());
                return FE_INVALID_ARGS;
            }
        }

        return m_Device->createFramebuffer(desc, ppFramebuffer);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppFramebuffer);
    }

    static void UpdateBindingSummaryWithLocation(IMessageCallback* messageCallback, ResourceType type,
        BindingLocation location, BindingSummary& bindings, BindingLocationSet& duplicates)
    {
        switch (type)
        {
        case ResourceType::Texture_SRV:
        case ResourceType::TypedBuffer_SRV:
        case ResourceType::StructuredBuffer_SRV:
        case ResourceType::RawBuffer_SRV:
        case ResourceType::RayTracingAccelStruct:
            location.type = GraphicsResourceType::SRV;
            bindings.rangeSRV.add(location.slot);
            break;

        case ResourceType::Texture_UAV:
        case ResourceType::TypedBuffer_UAV:
        case ResourceType::StructuredBuffer_UAV:
        case ResourceType::RawBuffer_UAV:
        case ResourceType::SamplerFeedbackTexture_UAV:
            location.type = GraphicsResourceType::UAV;
            bindings.rangeUAV.add(location.slot);
            break;

        case ResourceType::ConstantBuffer:
        case ResourceType::VolatileConstantBuffer:
        case ResourceType::PushConstants:
            location.type = GraphicsResourceType::CB;
            bindings.rangeCB.add(location.slot);
            if (type == ResourceType::VolatileConstantBuffer)
                ++bindings.numVolatileCBs;
            break;

        case ResourceType::Sampler:
            location.type = GraphicsResourceType::Sampler;
            bindings.rangeSampler.add(location.slot);
            break;
        
        case ResourceType::None:
        case ResourceType::Count:
        default: {
            std::stringstream ss;
            ss << "Invalid layout item type " << (int)type;
            messageCallback->message(MessageSeverity::Error, ss.str().c_str());
            break;
        }
        }

        if (bindings.locations.find(location) != bindings.locations.end())
        {
            duplicates.insert(location);
        }
        else
        {
            bindings.locations.insert(location);
        }
    }

    static void FillBindingLayoutSummary(IMessageCallback* messageCallback, BindingLayoutDesc const& desc,
        bool ignoreRegisterSpaces, BindingSummary& bindings, BindingLocationSet& duplicates)
    {
        for (const auto& item : desc.bindings)
        {
            BindingLocation location;
            location.registerSpace = ignoreRegisterSpaces ? 0 : desc.registerSpace;
            location.slot = item.slot;
            uint32_t arraySize = item.getArraySize();
            for (location.arrayElement = 0; location.arrayElement < arraySize; ++location.arrayElement)
            {
                UpdateBindingSummaryWithLocation(messageCallback, item.type, location, bindings, duplicates);
            }
        }
    }
    
    static void FillBindingSetSummary(IMessageCallback* messageCallback, BindingSetDesc const& desc,
        bool ignoreRegisterSpaces, uint32_t registerSpace, BindingSummary& bindings, BindingLocationSet& duplicates)
    {
        for (const auto& item : desc.bindings)
        {
            BindingLocation location;
            location.registerSpace = ignoreRegisterSpaces ? 0 : registerSpace;
            location.slot = item.slot;
            location.arrayElement = item.arrayElement;
            UpdateBindingSummaryWithLocation(messageCallback, item.type, location, bindings, duplicates);
        }
    }

    template<typename ItemType, typename DescType>
    const ItemType* SelectShaderStage(const DescType& desc, ShaderType stage)
    {
        switch (stage)
        {
        case ShaderType::Vertex: return std::addressof(desc.VS);
        case ShaderType::Hull: return std::addressof(desc.HS);
        case ShaderType::Domain: return std::addressof(desc.DS);
        case ShaderType::Geometry: return std::addressof(desc.GS);
        case ShaderType::Pixel: return std::addressof(desc.PS);
        case ShaderType::Compute: return std::addressof(desc.CS);
        default:
            utils::InvalidEnum();
            return nullptr;
        }
    }

    template<typename ItemType, typename DescType>
    const ItemType* SelectGraphicsShaderStage(const DescType& desc, ShaderType stage)
    {
        switch (stage)  // NOLINT(clang-diagnostic-switch-enum)
        {
        case ShaderType::Vertex: return std::addressof(desc.VS);
        case ShaderType::Hull: return std::addressof(desc.HS);
        case ShaderType::Domain: return std::addressof(desc.DS);
        case ShaderType::Geometry: return std::addressof(desc.GS);
        case ShaderType::Pixel: return std::addressof(desc.PS);
        default: 
            utils::InvalidEnum();
            return nullptr;
        }
    }

    template<typename ItemType, typename DescType>
    const ItemType* SelectMeshletShaderStage(const DescType& desc, ShaderType stage)
    {
        switch (stage)  // NOLINT(clang-diagnostic-switch-enum)
        {
        case ShaderType::Amplification: return std::addressof(desc.AS);
        case ShaderType::Mesh: return std::addressof(desc.MS);
        case ShaderType::Pixel: return std::addressof(desc.PS);
        default:
            utils::InvalidEnum();
            return nullptr;
        }
    }

    static const ShaderType g_GraphicsShaderStages[] = {
        ShaderType::Vertex,
        ShaderType::Hull,
        ShaderType::Domain,
        ShaderType::Geometry,
        ShaderType::Pixel
    };

    static const ShaderType g_MeshletShaderStages[] = {
        ShaderType::Amplification,
        ShaderType::Mesh,
        ShaderType::Pixel
    };
    
    bool DeviceWrapper::validatePipelineBindingLayouts(const static_vector<BindingLayoutHandle, c_MaxBindingLayouts>& bindingLayouts, const std::vector<IShader*>& shaders) const
    {
        const int numBindingLayouts = int(bindingLayouts.size());
        bool anyErrors = false;
        bool anyDuplicateBindings = false;
        bool anyOverlappingBindings = false;
        std::stringstream ssDuplicateBindings;
        std::stringstream ssOverlappingBindings;

        bool const ignoreRegisterSpaces = (m_Device->getGraphicsAPI() == GraphicsAPI::D3D11);

        for (IShader* shader : shaders)
        {
            ShaderType stage = shader->getDesc().shaderType;

            static_vector<BindingSummary, c_MaxBindingLayouts> bindingsPerLayout;
            bindingsPerLayout.resize(numBindingLayouts);

            // Accumulate binding information about the stage from all layouts

            for (int layoutIndex = 0; layoutIndex < numBindingLayouts; layoutIndex++)
            {
                if (bindingLayouts[layoutIndex] == nullptr)
                {
                    std::stringstream ss;
                    ss << "Binding layout in slot " << layoutIndex << " is NULL";
                    error(ss.str());
                    anyErrors = true;
                }
                else
                {
                    const BindingLayoutDesc* layoutDesc = bindingLayouts[layoutIndex]->getDesc();

                    if (layoutDesc)
                    {
                        if (!(layoutDesc->visibility & stage))
                                continue;

                        BindingLocationSet duplicates;
                        FillBindingLayoutSummary(m_MessageCallback, *layoutDesc, ignoreRegisterSpaces,
                            bindingsPerLayout[layoutIndex], duplicates);

                        // Layouts with duplicates should not have passed validation in createBindingLayout
                        assert(duplicates.empty());
                    }
                }
            }

            // Check for multiple layouts declaring the same bindings

            if (numBindingLayouts > 1)
            {
                BindingSummary bindings = bindingsPerLayout[0];
                BindingSummary duplicates;

                for (int layoutIndex = 1; layoutIndex < numBindingLayouts; layoutIndex++)
                {
                    SetUnionInplace(duplicates.locations, SetIntersection(bindings.locations, bindingsPerLayout[layoutIndex].locations));
                    SetUnionInplace(bindings.locations, bindingsPerLayout[layoutIndex].locations);
                }

                if (duplicates.any())
                {
                    if (!anyDuplicateBindings)
                        ssDuplicateBindings << "Same bindings defined by more than one layout in this pipeline:";

                    ssDuplicateBindings << std::endl << utils::ShaderStageToString(stage) << ": " << duplicates.locations;

                    anyDuplicateBindings = true;
                }
                else if (m_Device->getGraphicsAPI() == GraphicsAPI::D3D11)
                {
                    // Check for overlapping layouts on DX11, because the backend implements each binding set as a single
                    // call to a function like PSSetShaderResources. If binding sets overlap, a set with higher index
                    // will overwrite bindings from the lower-indexed sets, even if they are on different slots.
                    // Do this only when there are no duplicates, as with duplicates the layouts will always overlap.

                    bool overlapSRV = false;
                    bool overlapSampler = false;
                    bool overlapUAV = false;
                    bool overlapCB = false;

                    for (int i = 0; i < numBindingLayouts - 1; i++)
                    {
                        const BindingSummary& set1 = bindingsPerLayout[i];

                        for (int j = i + 1; j < numBindingLayouts; j++)
                        {
                            const BindingSummary& set2 = bindingsPerLayout[j];

                            overlapSRV = overlapSRV || set1.rangeSRV.overlapsWith(set2.rangeSRV);
                            overlapSampler = overlapSampler || set1.rangeSampler.overlapsWith(set2.rangeSampler);
                            overlapUAV = overlapUAV || set1.rangeUAV.overlapsWith(set2.rangeUAV);
                            overlapCB = overlapCB || set1.rangeCB.overlapsWith(set2.rangeCB);
                        }
                    }

                    if (overlapSRV || overlapSampler || overlapUAV || overlapCB)
                    {
                        if (!anyOverlappingBindings)
                            ssOverlappingBindings << "Binding layouts have overlapping register ranges:";

                        ssOverlappingBindings << std::endl << utils::ShaderStageToString(stage) << ": ";

                        bool first = true;
                        auto append = [&first, &ssOverlappingBindings](bool value, const char* text)
                        {
                            if (value)
                            {
                                if (!first) ssOverlappingBindings << ", ";
                                ssOverlappingBindings << text;
                                first = false;
                            }
                        };

                        append(overlapSRV, "SRV");
                        append(overlapSampler, "Sampler");
                        append(overlapUAV, "UAV");
                        append(overlapCB, "CB");

                        anyOverlappingBindings = true;
                    }
                }
            }
        }

        if (anyDuplicateBindings)
        {
            error(ssDuplicateBindings.str());
            anyErrors = true;
        }

        if (anyOverlappingBindings)
        {
            error(ssOverlappingBindings.str());
            anyErrors = true;
        }

        int pushConstantCount = 0;
        uint32_t pushConstantSize = 0;
        enum class RegisterSpaceIsDescriptorSet
        {
            False,
            True,
            Undetermined,
            Mixed
        } registerSpaceIsDescriptorSet = RegisterSpaceIsDescriptorSet::Undetermined;
        static_vector<int, c_MaxBindingLayouts> registerSpaceToLayoutIdx(c_MaxBindingLayouts);
        registerSpaceToLayoutIdx.fill(-1);

        for (int layoutIndex = 0; layoutIndex < numBindingLayouts; layoutIndex++)
        {
            const BindingLayoutDesc* layoutDesc = bindingLayouts[layoutIndex]->getDesc();
            if (layoutDesc)
            {
                for (const auto& item : layoutDesc->bindings)
                {
                    if (item.type == ResourceType::PushConstants)
                    {
                        pushConstantCount++;
                        pushConstantSize = std::max(pushConstantSize, uint32_t(item.size));
                    }
                }

                if (layoutDesc->registerSpaceIsDescriptorSet)
                {
                    if (layoutDesc->registerSpace >= c_MaxBindingLayouts)
                    {
                        std::stringstream errorStream;
                        errorStream << "Binding layout at index " << layoutIndex << " has registerSpace = " << layoutDesc->registerSpace
                            << ". Largest supported registerSpace index is " << (c_MaxBindingLayouts-1);
                        error(errorStream.str());
                        anyErrors = true;
                    }
                    else if (registerSpaceToLayoutIdx[layoutDesc->registerSpace] != -1)
                    {
                        std::stringstream errorStream;
                        errorStream << "Binding layout at index " << layoutIndex << " has registerSpace = " << layoutDesc->registerSpace
                            << ". That register space has already been used in layout index " << registerSpaceToLayoutIdx[layoutDesc->registerSpace];
                        error(errorStream.str());
                        anyErrors = true;
                    }
                    registerSpaceToLayoutIdx[layoutDesc->registerSpace] = layoutIndex;
                    if (registerSpaceIsDescriptorSet == RegisterSpaceIsDescriptorSet::Undetermined)
                    {
                        registerSpaceIsDescriptorSet = RegisterSpaceIsDescriptorSet::True;
                    }
                    else if (registerSpaceIsDescriptorSet == RegisterSpaceIsDescriptorSet::False)
                    {
                        registerSpaceIsDescriptorSet = RegisterSpaceIsDescriptorSet::Mixed;
                    }
                }
                else
                {
                    if (registerSpaceIsDescriptorSet == RegisterSpaceIsDescriptorSet::Undetermined)
                    {
                        registerSpaceIsDescriptorSet = RegisterSpaceIsDescriptorSet::False;
                    }
                    else if (registerSpaceIsDescriptorSet == RegisterSpaceIsDescriptorSet::True)
                    {
                        registerSpaceIsDescriptorSet = RegisterSpaceIsDescriptorSet::Mixed;
                    }
                }
            }
        }
        if (registerSpaceIsDescriptorSet == RegisterSpaceIsDescriptorSet::Mixed)
        {
            error("Pipeline contains Binding layouts with differing values of `registerSpaceIsDescriptorSet`");
            anyErrors = true;
        }

        if (pushConstantCount > 1)
        {
            std::stringstream errorStream;
            errorStream << "Binding layout contains more than one (" << pushConstantCount << ") push constant blocks";
            error(errorStream.str());
            anyErrors = true;
        }

        if (pushConstantSize > c_MaxPushConstantSize)
        {
            std::stringstream errorStream;
            errorStream << "Binding layout declares " << pushConstantSize << " bytes of push constant data, "
                "which exceeds the limit of " << c_MaxPushConstantSize << " bytes";
            error(errorStream.str());
            anyErrors = true;
        }

        return !anyErrors;
    }

    bool DeviceWrapper::validateShaderType(ShaderType expected, const ShaderDesc& shaderDesc, const char* function) const
    {
        if (expected == shaderDesc.shaderType)
            return true;

        std::stringstream ss;
        ss << "Unexpected shader type used in " << function << ": expected shaderType = "
            << utils::ShaderStageToString(expected) << ", actual shaderType = " << utils::ShaderStageToString(shaderDesc.shaderType)
            << " in " << utils::DebugNameToString(shaderDesc.debugName) << ":" << shaderDesc.entryName;
        error(ss.str());
        return false;
    }

    bool DeviceWrapper::validateRenderState(const RenderState& renderState, FramebufferInfo const& fbinfo) const
    {
        if (renderState.depthStencilState.depthTestEnable ||
            renderState.depthStencilState.stencilEnable)
        {
            if (fbinfo.depthFormat == Format::UNKNOWN)
            {
                error("The depth-stencil state indicates that depth or stencil operations are used, "
                    "but the framebuffer info has no depth format.");
                return false;
            }
        }

        if (renderState.rasterState.conservativeRasterEnable && !m_Device->queryFeatureSupport(Feature::ConservativeRasterization))
        {
            warning("Conservative rasterization is not supported on this device");
            return false;
        }

        return true;
    }

    FRESULT DeviceWrapper::createGraphicsPipeline1(const GraphicsPipelineDesc& pipelineDesc, FramebufferInfo const& fbinfo, IGraphicsPipeline** ppPipeline) noexcept
    try
    {
        if (!utils::ResetOutput(ppPipeline))
            return FE_INVALID_ARGS;

        std::vector<IShader*> shaders;

        for (ShaderType stage : g_GraphicsShaderStages)
        {
            IShader* shader = *SelectGraphicsShaderStage<ShaderHandle, GraphicsPipelineDesc>(pipelineDesc, stage);
            if (shader)
            {
                shaders.push_back(shader);

                if (!validateShaderType(stage, shader->getDesc(), "createGraphicsPipeline"))
                    return FE_INVALID_ARGS;
            }
        }

        if (!validatePipelineBindingLayouts(pipelineDesc.bindingLayouts, shaders))
            return FE_INVALID_ARGS;

        if (!validateRenderState(pipelineDesc.renderState, fbinfo))
            return FE_INVALID_ARGS;

        return m_Device->createGraphicsPipeline1(pipelineDesc, fbinfo, ppPipeline);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppPipeline);
    }

    FRESULT DeviceWrapper::createGraphicsPipeline2(const GraphicsPipelineDesc& pipelineDesc, IFramebuffer* fb, IGraphicsPipeline** ppPipeline) noexcept
    try
    {
        if (!utils::ResetOutput(ppPipeline))
            return FE_INVALID_ARGS;

        if (!fb)
        {
            error("framebuffer is NULL");
            return FE_INVALID_ARGS;
        }

        return createGraphicsPipeline1(pipelineDesc, fb->getFramebufferInfo().getInfo(), ppPipeline);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppPipeline);
    }

    FRESULT DeviceWrapper::createComputePipeline(const ComputePipelineDesc& pipelineDesc, IComputePipeline** ppPipeline) noexcept
    try
    {
        if (!utils::ResetOutput(ppPipeline))
            return FE_INVALID_ARGS;

        if (!pipelineDesc.CS)
        {
            error("createComputePipeline: CS = NULL");
            return FE_INVALID_ARGS;
        }

        std::vector<IShader*> shaders = { pipelineDesc.CS };
        
        if (!validatePipelineBindingLayouts(pipelineDesc.bindingLayouts, shaders))
            return FE_INVALID_ARGS;

        if (!validateShaderType(ShaderType::Compute, pipelineDesc.CS->getDesc(), "createComputePipeline"))
            return FE_INVALID_ARGS;

        return m_Device->createComputePipeline(pipelineDesc, ppPipeline);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppPipeline);
    }

    FRESULT DeviceWrapper::createMeshletPipeline1(const MeshletPipelineDesc& pipelineDesc, FramebufferInfo const& fbinfo, IMeshletPipeline** ppPipeline) noexcept
    try
    {
        if (!utils::ResetOutput(ppPipeline))
            return FE_INVALID_ARGS;

        std::vector<IShader*> shaders;

        for (ShaderType stage : g_MeshletShaderStages)
        {
            IShader* shader = *SelectMeshletShaderStage<ShaderHandle, MeshletPipelineDesc>(pipelineDesc, stage);
            if (shader)
            {
                shaders.push_back(shader);

                if (!validateShaderType(stage, shader->getDesc(), "createMeshletPipeline"))
                    return FE_INVALID_ARGS;
            }
        }

        if (!validatePipelineBindingLayouts(pipelineDesc.bindingLayouts, shaders))
            return FE_INVALID_ARGS;
               
        if (!validateRenderState(pipelineDesc.renderState, fbinfo))
            return FE_INVALID_ARGS;

        return m_Device->createMeshletPipeline1(pipelineDesc, fbinfo, ppPipeline);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppPipeline);
    }

    FRESULT DeviceWrapper::createMeshletPipeline2(const MeshletPipelineDesc& pipelineDesc, IFramebuffer* fb, IMeshletPipeline** ppPipeline) noexcept
    try
    {
        if (!utils::ResetOutput(ppPipeline))
            return FE_INVALID_ARGS;

        if (!fb)
        {
            error("framebuffer is NULL");
            return FE_INVALID_ARGS;
        }

        return createMeshletPipeline1(pipelineDesc, fb->getFramebufferInfo().getInfo(), ppPipeline);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppPipeline);
    }

    FRESULT DeviceWrapper::createRayTracingPipeline(const rt::PipelineDesc& desc, rt::IPipeline** ppPipeline) noexcept
    try
    {
        if (!utils::ResetOutput(ppPipeline))
            return FE_INVALID_ARGS;

        return m_Device->createRayTracingPipeline(desc, ppPipeline);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppPipeline);
    }

    FRESULT DeviceWrapper::createBindingLayout(const BindingLayoutDesc& desc, IBindingLayout** ppLayout) noexcept
    try
    {
        if (!utils::ResetOutput(ppLayout))
            return FE_INVALID_ARGS;

        std::stringstream errorStream;
        bool anyErrors = false;
        bool const ignoreRegisterSpaces = (m_Device->getGraphicsAPI() == GraphicsAPI::D3D11);

        BindingSummary bindings;
        BindingLocationSet duplicates;

        FillBindingLayoutSummary(m_MessageCallback, desc, ignoreRegisterSpaces, bindings, duplicates);

        if (desc.visibility == ShaderType::None)
        {
            errorStream << "Cannot create a binding layout with visibility = None" << std::endl;
            anyErrors = true;
        }

        if (!duplicates.empty())
        {
            errorStream << "Binding layout contains duplicate bindings: " << duplicates << std::endl;
            anyErrors = true;
        }

        if (bindings.numVolatileCBs > c_MaxVolatileConstantBuffersPerLayout)
        {
            errorStream << "Binding layout contains too many volatile CBs (" << bindings.numVolatileCBs << ")" << std::endl;
            anyErrors = true;
        }

        uint32_t noneItemCount = 0;
        uint32_t pushConstantCount = 0;
        uint32_t zeroSizeCount = 0;
        for (const BindingLayoutItem& item : desc.bindings)
        {
            if (item.type == ResourceType::None)
                noneItemCount++;

            if (item.type == ResourceType::PushConstants)
            {
                if (item.size == 0)
                {
                    errorStream << "Push constant block size cannot be 0" << std::endl;
                    anyErrors = true;
                }

                if (item.size > c_MaxPushConstantSize)
                {
                    errorStream << "Push constant block size (" << item.size << ") cannot exceed " << c_MaxPushConstantSize << " bytes" << std::endl;
                    anyErrors = true;
                }

                if ((item.size % 4) != 0)
                {
                    errorStream << "Push constant block size (" << item.size << ") must be a multiple of 4" << std::endl;
                    anyErrors = true;
                }

                pushConstantCount++;
            }
            else
            {
                if (item.size == 0)
                    zeroSizeCount++;

                if (item.size > 1 && item.type == ResourceType::VolatileConstantBuffer)
                {
                    errorStream << "Arrays of volatile constant buffers are not supported (size = " << item.size << ")" << std::endl;
                    anyErrors = true;
                }
            }
        }

        if (noneItemCount)
        {
            errorStream << "Binding layout contains " << noneItemCount << " item(s) with type = None" << std::endl;
            anyErrors = true;
        }

        if (zeroSizeCount)
        {
            errorStream << "Binding layout contains " << zeroSizeCount << " item(s) with size = 0" << std::endl;
            anyErrors = true;
        }

        if (pushConstantCount > 1)
        {
            errorStream << "Binding layout contains more than one (" << pushConstantCount << ") push constant blocks" << std::endl;
            anyErrors = true;
        }

        const GraphicsAPI graphicsApi = m_Device->getGraphicsAPI();
        
        if (desc.registerSpace != 0 && graphicsApi == GraphicsAPI::VULKAN && !desc.registerSpaceIsDescriptorSet)
        {
            errorStream << "Binding layout has nonzero registerSpace (" << desc.registerSpace << "), which is supported "
                " on Vulkan only if the registerSpaceIsDescriptorSet flag is set" << std::endl;
            anyErrors = true;
        }

        if (anyErrors)
        {
            error(errorStream.str());
            return FE_INVALID_ARGS;
        }

        return m_Device->createBindingLayout(desc, ppLayout);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppLayout);
    }

    FRESULT DeviceWrapper::createBindlessLayout(const BindlessLayoutDesc& desc, IBindingLayout** ppLayout) noexcept
    try
    {
        if (!utils::ResetOutput(ppLayout))
            return FE_INVALID_ARGS;

        std::stringstream errorStream;
        bool anyErrors = false;

        if (desc.visibility == ShaderType::None)
        {
            errorStream << "Cannot create a bindless layout with visibility = None" << std::endl;
            anyErrors = true;
        }

        if (desc.registerSpaces.empty() && desc.layoutType == BindlessLayoutDesc::LayoutType::Immutable)
        {
            errorStream << "Bindless layout has no register spaces assigned" << std::endl;
            anyErrors = true;
        }

        if (!desc.registerSpaces.empty() && desc.layoutType != BindlessLayoutDesc::LayoutType::Immutable)
        {
            errorStream << "Bindless mutable layout has register spaces assigned" << std::endl;
            anyErrors = true;
        }

        if (desc.maxCapacity == 0)
        {
            errorStream << "Bindless layout has maxCapacity = 0" << std::endl;
            anyErrors = true;
        }

        for (const BindingLayoutItem& item : desc.registerSpaces)
        {
            switch (item.type)
            {
            case ResourceType::Texture_SRV:
            case ResourceType::TypedBuffer_SRV:
            case ResourceType::StructuredBuffer_SRV:
            case ResourceType::RawBuffer_SRV:
            case ResourceType::RayTracingAccelStruct:
            case ResourceType::ConstantBuffer:
            case ResourceType::Texture_UAV:
            case ResourceType::TypedBuffer_UAV:
            case ResourceType::StructuredBuffer_UAV:
            case ResourceType::RawBuffer_UAV:
                continue;
            case ResourceType::VolatileConstantBuffer:
                errorStream << "Volatile CBs cannot be placed into a bindless layout (slot " << item.slot << ")" << std::endl;
                anyErrors = true;
                break;
            case ResourceType::Sampler:
                errorStream << "Bindless samplers are not implemented (slot " << item.slot << ")" << std::endl;
                anyErrors = true;
                break;
            case ResourceType::PushConstants:
                errorStream << "Push constants cannot be placed into a bindless layout (slot " << item.slot << ")" << std::endl;
                anyErrors = true;
                break;

            case ResourceType::None:
            case ResourceType::Count:
            default:
                errorStream << "Invalid resource type " << int(item.type) << " in slot " << item.slot << std::endl;
                anyErrors = true;
                break;
            }
        }

        if (anyErrors)
        {
            error(errorStream.str());
            return FE_INVALID_ARGS;
        }

        return m_Device->createBindlessLayout(desc, ppLayout);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppLayout);
    }

    static bool textureDimensionsCompatible(TextureDimension resourceDimension, TextureDimension viewDimension)
    {
        if (resourceDimension == viewDimension)
            return true;

        if (resourceDimension == TextureDimension::Texture3D)
            return viewDimension == TextureDimension::Texture2DArray;

        if (resourceDimension == TextureDimension::TextureCube || resourceDimension == TextureDimension::TextureCubeArray)
            return viewDimension == TextureDimension::Texture2DArray;

        return false;
    }

    bool DeviceWrapper::validateBindingSetItem(const BindingSetItem& binding, IDescriptorTable* pOptDescriptorTable, std::stringstream& errorStream)
    {
        bool isDescriptorTable = pOptDescriptorTable != nullptr;
        IBindingLayout* pBindingLayout = pOptDescriptorTable ? pOptDescriptorTable->getLayout() : nullptr;
        const BindlessLayoutDesc* pBindlessLayoutDesc = pBindingLayout ? pBindingLayout->getBindlessDesc() : nullptr;

        if (pBindlessLayoutDesc)
        {
            if (pBindlessLayoutDesc->layoutType == BindlessLayoutDesc::LayoutType::MutableSrvUavCbv)
            {
                switch (binding.type)
                {
                case ResourceType::None:
                case ResourceType::Texture_SRV:
                case ResourceType::Texture_UAV:
                case ResourceType::TypedBuffer_SRV:
                case ResourceType::TypedBuffer_UAV:
                case ResourceType::StructuredBuffer_SRV:
                case ResourceType::StructuredBuffer_UAV:
                case ResourceType::RawBuffer_SRV:
                case ResourceType::RawBuffer_UAV:
                case ResourceType::ConstantBuffer:
                    // Valid descriptors
                    break;
                case ResourceType::Sampler:
                    errorStream << "ResourceType::Sampler is not allowed for BindlessLayoutDesc::LayoutType::MutableSrvUavCbv." << std::endl;
                    return false;
                case ResourceType::SamplerFeedbackTexture_UAV:
                    errorStream << "ResourceType::SamplerFeedbackTexture_UAV is not allowed for bindless layouts with LayoutType::MutableSrvUavCbv." << std::endl;
                    return false;
                case ResourceType::VolatileConstantBuffer:
                    errorStream << "ResourceType::VolatileConstantBuffer is not allowed for bindless layouts with LayoutType::MutableSrvUavCbv." << std::endl;
                    return false;
                case ResourceType::PushConstants:
                    errorStream << "ResourceType::PushConstants is not allowed for bindless layouts with LayoutType::MutableSrvUavCbv." << std::endl;
                    return false;
                case ResourceType::RayTracingAccelStruct:
                    errorStream << "ResourceType::RayTracingAccelStruct is not allowed for bindless layouts with LayoutType::MutableSrvUavCbv." << std::endl;
                    return false;
                default:
                    // Invalid resource types will be dealt with later in this function.
                    break;
                }
            }
            else if (pBindlessLayoutDesc->layoutType == BindlessLayoutDesc::LayoutType::MutableCounters)
            {
                if (binding.type != ResourceType::StructuredBuffer_UAV)
                {
                    errorStream << "Only ResourceType::StructuredBuffer_UAV can be used for bindless layouts with LayoutType::MutableCounters." << std::endl;
                    return false;
                }
            }
            else if (pBindlessLayoutDesc->layoutType == BindlessLayoutDesc::LayoutType::MutableSampler)
            {
                if (binding.type != ResourceType::Sampler)
                {
                    errorStream << "Only ResourceType::Sampler can be used for bindless layouts with LayoutType::MutableSampler." << std::endl;
                    return false;
                }
            }
        }

        if ((binding.overrideComponentMapping & c_ComponentMappingExplicit) && binding.type != ResourceType::Texture_SRV)
        {
            errorStream << "Component mappings are only supported on ResourceType::Texture_SRV bindings." << std::endl;
            return false;
        }

        switch (binding.type)
        {
        case ResourceType::None:
            if (!isDescriptorTable)
            {
                errorStream << "ResourceType::None bindings are not allowed in binding sets." << std::endl;
                return false;
            }
            break;

        case ResourceType::Texture_SRV:
        case ResourceType::Texture_UAV:
        {
            ITexture* texture = checked_cast<ITexture*>(binding.resourceHandle);

            if (texture == nullptr)
            {
                errorStream << "Null resource bindings are not allowed for textures." << std::endl;
                return false;
            }

            const TextureDesc& desc = texture->getDesc();

            TextureSubresourceSet subresources = binding.subresources.resolve(desc, false);
            if (subresources.numArraySlices == 0 || subresources.numMipLevels == 0)
            {
                errorStream << "The specified subresource set (BaseMipLevel = " << binding.subresources.baseMipLevel
                    << ", NumMipLevels = " << binding.subresources.numMipLevels
                    << ", BaseArraySlice = " << binding.subresources.baseArraySlice
                    << ", NumArraySlices = " << binding.subresources.numArraySlices
                    << ") does not intersect with the texture being bound (" << utils::DebugNameToString(desc.debugName)
                    << ", MipLevels = " << desc.mipLevels
                    << ", ArraySize = " << desc.arraySize << ")" << std::endl;

                return false;
            }

            if ((binding.type == ResourceType::Texture_UAV) && !desc.isUAV)
            {
                errorStream << "Texture " << utils::DebugNameToString(desc.debugName)
                    << " cannot be used as a UAV because it does not have the isUAV flag set." << std::endl;
                return false;
            }

            if (binding.dimension != TextureDimension::Unknown)
            {
                if (!textureDimensionsCompatible(desc.dimension, binding.dimension))
                {
                    errorStream << "Requested binding dimension (" << utils::TextureDimensionToString(binding.dimension) << ") "
                        "is incompatible with the dimension (" << utils::TextureDimensionToString(desc.dimension) << ") "
                        "of texture " << utils::DebugNameToString(desc.debugName) << std::endl;
                    return false;
                }
            }

            break;
        }

        case ResourceType::SamplerFeedbackTexture_UAV:
            // Nothing to validate for sampler feedback resources: their bindings have no parameters, and NULL is allowed.
            break;

        case ResourceType::TypedBuffer_SRV:
        case ResourceType::TypedBuffer_UAV:
        case ResourceType::StructuredBuffer_SRV:
        case ResourceType::StructuredBuffer_UAV:
        case ResourceType::RawBuffer_SRV:
        case ResourceType::RawBuffer_UAV:
        case ResourceType::ConstantBuffer:
        case ResourceType::VolatileConstantBuffer:
        {
            IBuffer* buffer = checked_cast<IBuffer*>(binding.resourceHandle);

            if (buffer == nullptr && binding.type != ResourceType::TypedBuffer_SRV && binding.type != ResourceType::TypedBuffer_UAV && m_Device->getGraphicsAPI() != GraphicsAPI::VULKAN)
            {
                errorStream << "Null resource bindings are not allowed for buffers, unless it's a "
                    "TypedBuffer_SRV or TypedBuffer_UAV type binding on DX11 or DX12." << std::endl;
                return false;
            }

            if (buffer == nullptr)
                return true;

            const BufferDesc& desc = buffer->getDesc();

            bool isTypedView = (binding.type == ResourceType::TypedBuffer_SRV) || (binding.type == ResourceType::TypedBuffer_UAV);
            bool isStructuredView = (binding.type == ResourceType::StructuredBuffer_SRV) || (binding.type == ResourceType::StructuredBuffer_UAV);
            bool isRawView = (binding.type == ResourceType::RawBuffer_SRV) || (binding.type == ResourceType::RawBuffer_UAV);
            bool isUAV = (binding.type == ResourceType::TypedBuffer_UAV) || (binding.type == ResourceType::StructuredBuffer_UAV) || (binding.type == ResourceType::RawBuffer_UAV);
            bool isConstantView = (binding.type == ResourceType::ConstantBuffer) || (binding.type == ResourceType::VolatileConstantBuffer);
            
            if (isTypedView && !desc.canHaveTypedViews)
            {
                errorStream << "Cannot bind buffer " << utils::DebugNameToString(desc.debugName)
                    << " as " << utils::ResourceTypeToString(binding.type)
                    << " because it doesn't support typed views (BufferDesc::canHaveTypedViews)." << std::endl;
                return false;
            }

            if (isStructuredView && desc.structStride == 0)
            {
                errorStream << "Cannot bind buffer " << utils::DebugNameToString(desc.debugName)
                    << " as " << utils::ResourceTypeToString(binding.type)
                    << " because it doesn't have structStride specified at creation." << std::endl;
                return false;
            }

            if (isRawView && !desc.canHaveRawViews)
            {
                errorStream << "Cannot bind buffer " << utils::DebugNameToString(desc.debugName)
                    << " as " << utils::ResourceTypeToString(binding.type)
                    << " because it doesn't support raw views (BufferDesc::canHaveRawViews)." << std::endl;
                return false;
            }

            if (isUAV && !desc.canHaveUAVs)
            {
                errorStream << "Cannot bind buffer " << utils::DebugNameToString(desc.debugName)
                    << " as " << utils::ResourceTypeToString(binding.type)
                    << " because it doesn't support unordeded access views (BufferDesc::canHaveUAVs)." << std::endl;
                return false;
            }

            if (isConstantView && !desc.isConstantBuffer)
            {
                errorStream << "Cannot bind buffer " << utils::DebugNameToString(desc.debugName)
                    << " as " << utils::ResourceTypeToString(binding.type)
                    << " because it doesn't support constant buffer views (BufferDesc::isConstantBuffer)." << std::endl;
                return false;
            }

            if (binding.type == ResourceType::ConstantBuffer && desc.isVolatile)
            {
                errorStream << "Cannot bind buffer " << utils::DebugNameToString(desc.debugName)
                    << " as a regular ConstantBuffer because it's a VolatileConstantBuffer." << std::endl;
                return false;
            }

            if (binding.type == ResourceType::VolatileConstantBuffer && !desc.isVolatile)
            {
                errorStream << "Cannot bind buffer " << utils::DebugNameToString(desc.debugName)
                    << " as a VolatileConstantBuffer because it's a regular ConstantBuffer." << std::endl;
                return false;
            }

            if (isTypedView && (binding.format == Format::UNKNOWN && desc.format == Format::UNKNOWN))
            {
                errorStream << "Both binding for typed buffer " << utils::DebugNameToString(desc.debugName)
                    << " and its BufferDesc have format == UNKNOWN." << std::endl;
                return false;
            }

            if (binding.type == ResourceType::ConstantBuffer && !binding.range.isEntireBuffer(desc))
            {
                if (!queryFeatureSupport(Feature::ConstantBufferRanges))
                {
                    errorStream << "Partial binding of constant buffers is not supported by the device (used for " << utils::DebugNameToString(desc.debugName) << ")";
                    return false;
                }

                const BufferRange range = binding.range.resolve(desc);
                if ((range.byteOffset % c_ConstantBufferOffsetSizeAlignment) != 0)
                {
                    errorStream << "Constant buffer offsets must be a multiple of " << c_ConstantBufferOffsetSizeAlignment << " bytes. Buffer "
                        << utils::DebugNameToString(desc.debugName) << " is bound with effective byteOffset = " << range.byteOffset << ".";
                    return false;
                }

                if (range.byteSize == 0 || (range.byteSize % c_ConstantBufferOffsetSizeAlignment) != 0)
                {
                    errorStream << "Constant buffer bindings must have nonzero byteSize that is a multiple of " << c_ConstantBufferOffsetSizeAlignment << " bytes. Buffer "
                        << utils::DebugNameToString(desc.debugName) << " is bound with effective byteSize = " << range.byteSize << ".";
                    return false;
                }
            }

            if (binding.type == ResourceType::VolatileConstantBuffer && !binding.range.isEntireBuffer(desc))
            {
                const BufferRange range = binding.range.resolve(desc);
                errorStream << "Volatile constant buffers cannot be partially bound. Buffer " << utils::DebugNameToString(desc.debugName)
                    << " is bound with effective byteOffset = " << range.byteOffset << ", byteSize = " << range.byteSize << ".";
                return false;
            }

            break;
        }

        case ResourceType::Sampler:
            if (binding.resourceHandle == nullptr)
            {
                errorStream << "Null resource bindings are not allowed for samplers." << std::endl;
                return false;
            }
            break;

        case ResourceType::RayTracingAccelStruct:
            if (binding.resourceHandle == nullptr)
            {
                errorStream << "Null resource bindings are not allowed for ray tracing acceleration structures." << std::endl;
                return false;
            }
            break;

        case ResourceType::PushConstants:
            if (isDescriptorTable)
            {
                errorStream << "Push constants cannot be used in a descriptor table." << std::endl;
                return false;
            }
            if (binding.resourceHandle != nullptr)
            {
                errorStream << "Push constants cannot have a resource specified." << std::endl;
                return false;
            }
            if (binding.range.byteSize == 0)
            {
                errorStream << "Push constants must have nonzero size specified." << std::endl;
                return false;
            }
            break;

        case ResourceType::Count:
        default:
            errorStream << "Unrecognized resourceType = " << uint32_t(binding.type) << std::endl;
            return false;
        }

        return true;
    }

    FRESULT DeviceWrapper::createBindingSet(const BindingSetDesc& desc, IBindingLayout* layout, IBindingSet** ppBindingSet) noexcept
    try
    {
        if (!utils::ResetOutput(ppBindingSet))
            return FE_INVALID_ARGS;

        if (layout == nullptr)
        {
            error("Cannot create a binding set without a valid layout");
            return FE_INVALID_ARGS;
        }

        const BindingLayoutDesc* layoutDesc = layout->getDesc();
        if (!layoutDesc)
        {
            error("Cannot create a binding set from a bindless layout");
            return FE_INVALID_ARGS;
        }

        std::stringstream errorStream;
        bool anyErrors = false;
        bool const ignoreRegisterSpaces = (m_Device->getGraphicsAPI() == GraphicsAPI::D3D11);

        BindingSummary layoutBindings;
        BindingLocationSet layoutDuplicates;

        FillBindingLayoutSummary(m_MessageCallback, *layoutDesc, ignoreRegisterSpaces,
            layoutBindings, layoutDuplicates);

        BindingSummary setBindings;
        BindingLocationSet setDuplicates;

        FillBindingSetSummary(m_MessageCallback, desc, ignoreRegisterSpaces, layoutDesc->registerSpace,
            setBindings, setDuplicates);

        BindingLocationSet declaredNotBound;
        BindingLocationSet boundNotDeclared;

        declaredNotBound = SetDifference(layoutBindings.locations, setBindings.locations);
        boundNotDeclared = SetDifference(setBindings.locations, layoutBindings.locations);

        if (!declaredNotBound.empty())
        {
            errorStream << "Bindings declared in the layout are not present in the binding set: " << declaredNotBound << std::endl;
            anyErrors = true;
        }

        if (!boundNotDeclared.empty())
        {
            errorStream << "Bindings in the binding set are not declared in the layout: " << boundNotDeclared << std::endl;
            anyErrors = true;
        }

        if (!setDuplicates.empty())
        {
            errorStream << "Binding set contains duplicate bindings: " << setDuplicates << std::endl;
            anyErrors = true;
        }

        for (size_t index = 0; index < desc.bindings.size(); index++)
        {
            if (!validateBindingSetItem(desc.bindings[index], nullptr, errorStream))
                anyErrors = true;
        }

        if (anyErrors)
        {
            error(errorStream.str());
            return FE_INVALID_ARGS;
        }

        // Unwrap the resources
        BindingSetDesc patchedDesc = desc;
        for (auto& binding : patchedDesc.bindings)
        {
            binding.resourceHandle = unwrapResource(binding.resourceHandle);
        }

        return m_Device->createBindingSet(patchedDesc, layout, ppBindingSet);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppBindingSet);
    }

    FRESULT DeviceWrapper::createDescriptorTable(IBindingLayout* layout, IDescriptorTable** ppDescriptorTable) noexcept
    try
    {
        if (!utils::ResetOutput(ppDescriptorTable))
            return FE_INVALID_ARGS;

        if (!layout->getBindlessDesc()) 
        {
            error("Descriptor tables can only be created with bindless layouts");
            return FE_INVALID_ARGS;
        }

        return m_Device->createDescriptorTable(layout, ppDescriptorTable);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppDescriptorTable);
    }

    void DeviceWrapper::resizeDescriptorTable(IDescriptorTable* descriptorTable, uint32_t newSize, bool keepContents) noexcept
    {
        m_Device->resizeDescriptorTable(descriptorTable, newSize, keepContents);
    }

    bool DeviceWrapper::writeDescriptorTable(IDescriptorTable* descriptorTable, const BindingSetItem& item) noexcept
    {
        std::stringstream errorStream;
        
        if (!validateBindingSetItem(item, descriptorTable, errorStream))
        {
            error(errorStream.str());
            return false;
        }

        BindingSetItem patchedItem = item;
        patchedItem.resourceHandle = unwrapResource(patchedItem.resourceHandle);

        return m_Device->writeDescriptorTable(descriptorTable, patchedItem);
    }

    FRESULT DeviceWrapper::createOpacityMicromap(const rt::OpacityMicromapDesc& desc, rt::IOpacityMicromap** ppOpacityMicromap) noexcept
    try
    {
        if (!utils::ResetOutput(ppOpacityMicromap))
            return FE_INVALID_ARGS;

        if (desc.inputBuffer == nullptr)
        {
            error("OpacityMicromapDesc:inputBuffer is nullptr");
            return FE_INVALID_ARGS;
        }

        if (desc.perOmmDescs == nullptr)
        {
            error("OpacityMicromapDesc:perOmmDescs is nullptr");
            return FE_INVALID_ARGS;
        }

        const FRESULT result = m_Device->createOpacityMicromap(desc, ppOpacityMicromap);
        if (NVRHI_FAILED(result))
            error("createOpacityMicromap failed");
        return result;
    }
    catch (...)
    {
        return utils::ExceptionToError(ppOpacityMicromap);
    }

    FRESULT DeviceWrapper::createAccelStruct(const rt::AccelStructDesc& desc, rt::IAccelStruct** ppAccelStruct) noexcept
    try
    {
        if (!utils::ResetOutput(ppAccelStruct))
            return FE_INVALID_ARGS;

        rt::AccelStructHandle as;
        const FRESULT result = m_Device->createAccelStruct(desc, &as);
        if (NVRHI_FAILED(result))
            return result;

        if ((desc.buildFlags & rt::AccelStructBuildFlags::AllowCompaction) != 0 &&
            desc.isTopLevel)
        {
            std::stringstream ss;
            ss << "Cannot create TLAS " << utils::DebugNameToString(desc.debugName)
                << " with the AllowCompaction flag set: compaction is not supported for TLAS'es";
            error(ss.str());
            return FE_INVALID_ARGS;
        }

        if ((desc.buildFlags & rt::AccelStructBuildFlags::AllowUpdate) != 0 &&
            (desc.buildFlags & rt::AccelStructBuildFlags::AllowCompaction) != 0)
        {
            std::stringstream ss;
            ss << "Cannot create AccelStruct " << utils::DebugNameToString(desc.debugName)
                << " with incompatible flags: AllowUpdate and AllowCompaction";
            error(ss.str());
            return FE_INVALID_ARGS;
        }

        AccelStructWrapper* wrapper = MAKE_RC_OBJ(AccelStructWrapper, as);
        wrapper->isTopLevel = desc.isTopLevel;
        wrapper->allowUpdate = !!(desc.buildFlags & rt::AccelStructBuildFlags::AllowUpdate);
        wrapper->allowCompaction = !!(desc.buildFlags & rt::AccelStructBuildFlags::AllowCompaction);
        wrapper->maxInstances = desc.topLevelMaxInstances;
        
        *ppAccelStruct = wrapper;
        return FS_OK;
    }
    catch (...)
    {
        return utils::ExceptionToError(ppAccelStruct);
    }

    bool DeviceWrapper::queryTopLevelAccelStructPrebuildInfo(const rt::AccelStructDesc& desc,
        uint32_t instanceCount, rt::AccelStructPrebuildInfo& outInfo) noexcept
    {
        return m_Device->queryTopLevelAccelStructPrebuildInfo(desc, instanceCount, outInfo);
    }

    MemoryRequirements DeviceWrapper::getAccelStructMemoryRequirements(rt::IAccelStruct* as)
    {
        if (as == nullptr)
        {
            error("getAccelStructMemoryRequirements: as is NULL");
            return MemoryRequirements();
        }

        AccelStructWrapper* wrapper = queryWrapper<AccelStructWrapper>(as);
        if (wrapper)
            as = wrapper->getUnderlyingObject();

        MemoryRequirements memReq;

        m_Device->getAccelStructMemoryRequirements(memReq, as);
        
        return memReq;
    }

    static const char* kOperationTypeStrings[] =
    {
        "Move",
        "ClasBuild",
        "ClasBuildTemplates",
        "ClasInstantiateTemplates",
        "BlasBuild"
    };
    static_assert(std::size(kOperationTypeStrings) == uint32_t(rt::cluster::OperationType::BlasBuild) + 1);


    bool DeviceWrapper::validateClusterOperationParams(const rt::cluster::OperationParams& params) const
    {
        bool isValid = true;

        const char* operationType = "Unknown";
        if (uint32_t(params.type) < std::size(kOperationTypeStrings))
        {
            operationType = kOperationTypeStrings[uint32_t(params.type)];
        }

        switch (params.mode)
        {
        case rt::cluster::OperationMode::ImplicitDestinations:
        case rt::cluster::OperationMode::ExplicitDestinations:
        case rt::cluster::OperationMode::GetSizes:
            break;
        default:
            isValid = false;
            error("cluster::OperationParams " + std::string(operationType) + " unknown cluster::OperationMode");
            break;
        }

        bool validateClasParams = false;
        switch (params.type)
        {
        case rt::cluster::OperationType::Move:
            break;
        case rt::cluster::OperationType::ClasBuild:
        case rt::cluster::OperationType::ClasBuildTemplates:
        case rt::cluster::OperationType::ClasInstantiateTemplates:
            validateClasParams = true;
            break;
        case rt::cluster::OperationType::BlasBuild:
            break;
        }

        if (validateClasParams)
        {
            nvrhi::Format vertexFormat = params.clas.vertexFormat;
            const bool validVertexFormat =
                (vertexFormat == nvrhi::Format::RGBA32_FLOAT)
                || (vertexFormat == nvrhi::Format::RGB32_FLOAT)
                || (vertexFormat == nvrhi::Format::RG32_FLOAT)
                || (vertexFormat == nvrhi::Format::RGBA16_FLOAT)
                || (vertexFormat == nvrhi::Format::RG16_FLOAT)
                || (vertexFormat == nvrhi::Format::RGBA16_SNORM)
                || (vertexFormat == nvrhi::Format::RG16_SNORM)
                || (vertexFormat == nvrhi::Format::RGBA8_SNORM)
                || (vertexFormat == nvrhi::Format::RG8_SNORM)
                || (vertexFormat == nvrhi::Format::RGBA16_UNORM)
                || (vertexFormat == nvrhi::Format::RG16_UNORM)
                || (vertexFormat == nvrhi::Format::RGBA8_UNORM)
                || (vertexFormat == nvrhi::Format::RG8_UNORM)
                || (vertexFormat == nvrhi::Format::R10G10B10A2_UNORM);
            if (!validVertexFormat)
            {
                error("cluster::OperationParams " + std::string(operationType) + " does not have a valid vertex format");
                isValid = false;
            }

            if (params.clas.maxGeometryIndex > nvrhi::rt::cluster::kMaxGeometryIndex)
            {
                error("cluster::OperationParams " + std::string(operationType) + " has a maxGeometryIndex over " + std::to_string(nvrhi::rt::cluster::kMaxGeometryIndex));
                isValid = false;
            }

            if (params.clas.minPositionTruncateBitCount > 32)
            {
                error("cluster::OperationParams " + std::string(operationType) + " minPositionTruncateBitCount over " + std::to_string(32));
                isValid = false;
            }

            if (params.clas.maxTriangleCount > nvrhi::rt::cluster::kClasMaxTriangles)
            {
                error("cluster::OperationParams " + std::string(operationType) + " maxTriangleCount over " + std::to_string(nvrhi::rt::cluster::kClasMaxTriangles));
                isValid = false;
            }

            if (params.clas.maxVertexCount > nvrhi::rt::cluster::kClasMaxVertices)
            {
                error("cluster::OperationParams " + std::string(operationType) + " maxVertexCount over " + std::to_string(nvrhi::rt::cluster::kClasMaxVertices));
                isValid = false;
            }

            if (params.clas.maxTriangleCount > params.clas.maxTotalTriangleCount)
            {
                error("cluster::OperationParams " + std::string(operationType) + " maxTriangleCount over maxTotalTriangleCount. maxTotalTriangleCount must be greater than the sum of all triangles in the operation");
                isValid = false;
            }

            if (params.clas.maxVertexCount > params.clas.maxTotalVertexCount)
            {
                error("cluster::OperationParams " + std::string(operationType) + " maxVertexCount over maxTotalVertexCount. maxTotalVertexCount must be greater than the sum of all vertices in the operation");
                isValid = false;
            }

            if (params.clas.maxUniqueGeometryCount > params.clas.maxTriangleCount)
            {
                error("cluster::OperationParams " + std::string(operationType) + " maxUniqueGeometryCount over maxTriangleCount. Maximum 1 geometry per triangle");
                isValid = false;
            }
        }
        
        return isValid;
    }

    rt::cluster::OperationSizeInfo DeviceWrapper::getClusterOperationSizeInfo(const rt::cluster::OperationParams& params)
    {
        if (!validateClusterOperationParams(params))
        {
            return rt::cluster::OperationSizeInfo{};
        }

        rt::cluster::OperationSizeInfo result;
        return m_Device->getClusterOperationSizeInfo(result, params);
    }

    bool DeviceWrapper::bindAccelStructMemory(rt::IAccelStruct* as, IHeap* heap, uint64_t offset) noexcept
    {
        if (as == nullptr)
        {
            error("bindAccelStructMemory: texture is NULL");
            return false;
        }

        if (heap == nullptr)
        {
            error("bindAccelStructMemory: heap is NULL");
            return false;
        }

        AccelStructWrapper* wrapper = queryWrapper<AccelStructWrapper>(as);
        if (wrapper)
            as = wrapper->getUnderlyingObject();

        const HeapDesc& heapDesc = heap->getDesc();
        const rt::AccelStructDesc& asDesc = as->getDesc();

        if (!asDesc.isVirtual)
        {
            std::stringstream ss;
            ss << "Cannot perform bindAccelStructMemory on AccelStruct " << utils::DebugNameToString(asDesc.debugName)
                << " because it was created with isVirtual = false";

            error(ss.str());
            return false;
        }

        MemoryRequirements memReq;

        m_Device->getAccelStructMemoryRequirements(memReq, as);

        if (offset + memReq.size > heapDesc.capacity)
        {
            std::stringstream ss;
            ss << "AccelStruct " << utils::DebugNameToString(asDesc.debugName)
                << " does not fit into heap " << utils::DebugNameToString(heapDesc.debugName)
                << " at offset " << offset << " because it requires " << memReq.size << " bytes,"
                << " and the heap capacity is " << heapDesc.capacity << " bytes";

            error(ss.str());
            return false;
        }

        if (memReq.alignment != 0 && (offset % memReq.alignment) != 0)
        {
            std::stringstream ss;
            ss << "AccelStruct " << utils::DebugNameToString(asDesc.debugName)
                << " is placed in heap " << utils::DebugNameToString(heapDesc.debugName)
                << " at invalid alignment: required alignment to " << memReq.alignment << " bytes,"
                << " actual offset is " << offset << " bytes";

            error(ss.str());
            return false;
        }

        return m_Device->bindAccelStructMemory(as, heap, offset);
    }

    FRESULT DeviceWrapper::createCommandList(const CommandListParameters& params, nvrhi::ICommandList** ppCommandList) noexcept
    try
    {
        if (!utils::ResetOutput(ppCommandList))
            return FE_INVALID_ARGS;

        switch(params.queueType)
        {
        case CommandQueue::Graphics:
            // Assume the graphics queue always exists
            break;

        case CommandQueue::Compute:
            if (!m_Device->queryFeatureSupport(Feature::ComputeQueue))
            {
                error("Compute queue is not supported or initialized in this device");
                return FE_INVALID_ARGS;
            }
            break;

        case CommandQueue::Copy:
            if (!m_Device->queryFeatureSupport(Feature::CopyQueue))
            {
                error("Copy queue is not supported or initialized in this device");
                return FE_INVALID_ARGS;
            }
            break;

        case CommandQueue::Count:
        default:
            utils::InvalidEnum();
            return FE_INVALID_ARGS;
        }

        CommandListHandle commandList;
        const FRESULT result = m_Device->createCommandList(params, &commandList);
        if (NVRHI_FAILED(result))
            return result;

        *ppCommandList = MAKE_RC_OBJ(CommandListWrapper, this, commandList, params.enableImmediateExecution, params.queueType);
        return FS_OK;
    }
    catch (...)
    {
        return utils::ExceptionToError(ppCommandList);
    }
    
    uint64_t DeviceWrapper::executeCommandLists(ICommandList* const* pCommandLists, size_t numCommandLists, CommandQueue executionQueue) noexcept
    {
        if (numCommandLists == 0)
            return 0;

        if (pCommandLists == nullptr)
        {
            error("executeCommandLists: pCommandLists is NULL");
            return 0;
        }

        std::vector<ICommandList*> unwrappedCommandLists;
        unwrappedCommandLists.resize(numCommandLists);

        for(size_t i = 0; i < numCommandLists; i++)
        {
            if (pCommandLists[i] == nullptr)
            {
                std::stringstream ss;
                ss << "executeCommandLists: pCommandLists[" << i << "] is NULL";
                error(ss.str());
                return 0;
            }

            const CommandListParameters& desc = pCommandLists[i]->getDesc();
            if (desc.queueType != executionQueue)
            {
                std::stringstream ss;
                ss << "executeCommandLists: The command list [" << i << "] type is " << utils::CommandQueueToString(desc.queueType)
                    << ", it cannot be executed on a " << utils::CommandQueueToString(executionQueue) << " queue";
                error(ss.str());
                return 0;
            }

            CommandListWrapper* wrapper = queryWrapper<CommandListWrapper>(pCommandLists[i]);
            if (wrapper)
            {
                if (!wrapper->requireExecuteState())
                    return 0;

                unwrappedCommandLists[i] = wrapper->getUnderlyingCommandList();
            }
            else
                unwrappedCommandLists[i] = pCommandLists[i];
        }

        return m_Device->executeCommandLists(unwrappedCommandLists.data(), unwrappedCommandLists.size(), executionQueue);
    }

    void DeviceWrapper::queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue, uint64_t instance) noexcept
    {
        m_Device->queueWaitForCommandList(waitQueue, executionQueue, instance);
    }

    bool DeviceWrapper::waitForIdle() noexcept
    {
        return m_Device->waitForIdle();
    }

    FRESULT DeviceWrapper::createCommandListLifetimeTracker(CommandQueue executionQueue, ICommandListLifetimeTracker** ppTracker) noexcept
    try
    {
        if (!utils::ResetOutput(ppTracker))
            return FE_INVALID_ARGS;

        return m_Device->createCommandListLifetimeTracker(executionQueue, ppTracker);
    }
    catch (...)
    {
        return utils::ExceptionToError(ppTracker);
    }

    void DeviceWrapper::runGarbageCollection() noexcept
    {
        m_Device->runGarbageCollection();
    }

    bool DeviceWrapper::queryFeatureSupport(Feature feature, void* pInfo, size_t infoSize) noexcept
    {
        return m_Device->queryFeatureSupport(feature, pInfo, infoSize);
    }

    FormatSupport DeviceWrapper::queryFormatSupport(Format format) noexcept
    {
        return m_Device->queryFormatSupport(format);
    }

    coopvec::DeviceFeatures DeviceWrapper::queryCoopVecFeatures()
    {
        coopvec::DeviceFeatures result;
        return m_Device->queryCoopVecFeatures(result);
    }

    coopvec::MatMulFormatSupport DeviceWrapper::queryCoopVecMatMulFormatSupport(const coopvec::MatMulFormatCombo& combination)
    {
        coopvec::MatMulFormatSupport result;
        return m_Device->queryCoopVecMatMulFormatSupport(result, combination);
    }

    coopvec::TrainingFormatSupport DeviceWrapper::queryCoopVecTrainingFormatSupport(coopvec::DataType componentType)
    {
        coopvec::TrainingFormatSupport result;
        return m_Device->queryCoopVecTrainingFormatSupport(result, componentType);
    }

    size_t DeviceWrapper::getCoopVecMatrixSize(coopvec::DataType type, coopvec::MatrixLayout layout, int rows, int columns) noexcept
    {
        if (!m_Device->queryFeatureSupport(Feature::CooperativeVectorInferencing))
        {
            error("getCoopVecMatrixSize: Cooperative Vectors are not supported by the device");
            return 0;
        }

        if (rows <= 0 || columns <= 0)
        {
            error("getCoopVecMatrixSize: rows and columns must be positive");
            return 0;
        }
        
        return m_Device->getCoopVecMatrixSize(type, layout, rows, columns);
    }

    NativeObject DeviceWrapper::getNativeQueue(ObjectType objectType, CommandQueue queue) noexcept
    {
        return m_Device->getNativeQueue(objectType, queue);
    }

    IMessageCallback* DeviceWrapper::getMessageCallback() noexcept
    {
        return m_MessageCallback;
    }

    bool DeviceWrapper::isAftermathEnabled() noexcept
    {
        return m_Device->isAftermathEnabled();
    }

    IAftermathCrashDumpHelper* DeviceWrapper::getAftermathCrashDumpHelper() noexcept
    {
        return m_Device->getAftermathCrashDumpHelper();
    }

    void Range::add(uint32_t item)
    {
        min = std::min(min, item);
        max = std::max(max, item);
    }

    bool Range::empty() const
    {
        return min > max;
    }

    bool Range::overlapsWith(const Range& other) const
    {
        return !empty() && !other.empty() && max >= other.min && min <= other.max;
    }

    bool BindingSummary::any() const
    {
        return !locations.empty();
    }

    bool BindingSummary::overlapsWith(const BindingSummary& other) const
    {
        return rangeSRV.overlapsWith(other.rangeSRV)
            || rangeSampler.overlapsWith(other.rangeSampler)
            || rangeUAV.overlapsWith(other.rangeUAV)
            || rangeCB.overlapsWith(other.rangeCB);
    }

    IRHIObject* unwrapResource(IRHIObject* resource)
    {
        if (!resource)
            return nullptr;
        
        AccelStructWrapper* asWrapper = queryWrapper<AccelStructWrapper>(resource);

        if (asWrapper)
            return asWrapper->getUnderlyingObject();

        // More resource types to be added here when their wrappers are implemented

        return resource;
    }
    
    std::ostream& operator<<(std::ostream& os, const BindingLocationSet& set)
    {
        bool first = true;
        for (BindingLocation const& item : set)
        {
            if (!first)
                os << ", ";

            if (item.registerSpace != 0)
                os << "space" << item.registerSpace << ".";

            char const* prefix = "?";
            switch(item.type)
            {
                case GraphicsResourceType::SRV: prefix = "t"; break;
                case GraphicsResourceType::Sampler: prefix = "s"; break;
                case GraphicsResourceType::UAV: prefix = "u"; break;
                case GraphicsResourceType::CB: prefix = "b"; break;
            }
            os << prefix << item.slot;

            if (item.arrayElement != 0)
                os << "[" << item.arrayElement << "]";

            first = false;
        }
        return os;
    }
} // namespace nvrhi::validation
