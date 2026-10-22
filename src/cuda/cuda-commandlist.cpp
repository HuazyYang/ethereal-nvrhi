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

// Command recording (ADR 0011). A command list resolves everything while it records: upload data goes into
// transient upload memory, the global parameter block of a dispatch is written there from the bound binding sets,
// and each command becomes a plain record that the executor (cuda-executor.cpp) replays on the stream of the
// queue. The only values resolved at replay are the traversable handles of acceleration structures, which OptiX
// returns when it builds them.

#include "cuda-backend.h"

#include <nvrhi/common/misc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace nvrhi::cuda
{
    namespace
    {
        uint16_t FloatToHalf(float value)
        {
            uint32_t f;
            memcpy(&f, &value, 4);
            const uint32_t sign = (f >> 16) & 0x8000u;
            int32_t exponent = int32_t((f >> 23) & 0xFF) - 127 + 15;
            uint32_t mantissa = f & 0x7FFFFFu;
            if (((f >> 23) & 0xFF) == 0xFF)
                return uint16_t(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
            if (exponent >= 31)
                return uint16_t(sign | 0x7C00u);
            if (exponent <= 0)
            {
                if (exponent < -10)
                    return uint16_t(sign);
                mantissa |= 0x800000u;
                const uint32_t shift = uint32_t(14 - exponent);
                return uint16_t(sign | ((mantissa + (1u << (shift - 1))) >> shift));
            }
            const uint32_t rounded = mantissa + 0x1000u;
            if (rounded & 0x800000u)
                return uint16_t(sign | (uint32_t(exponent + 1) << 10));
            return uint16_t(sign | (uint32_t(exponent) << 10) | (rounded >> 13));
        }

        // Encodes one texel of 'format' from float components (clearTextureFloat) or an integer value
        // (clearTextureUInt, integer != nullptr). Returns false for formats that cannot be encoded.
        bool EncodeTexel(Format format, const float* color, const uint32_t* integer, uint8_t* out)
        {
            const FormatMapping& mapping = GetFormatMapping(format);
            const FormatInfo& info = getFormatInfo(format);
            if (mapping.arrayFormat == 0 || mapping.blockSize != 1)
                return false;

            const uint32_t channels = mapping.channels;
            const uint32_t channelBytes = mapping.bytesPerElement / channels;
            for (uint32_t c = 0; c < channels; ++c)
            {
                uint8_t* dst = out + c * channelBytes;
                const float value = color ? color[c] : 0.f;
                const uint32_t iv = integer ? *integer : 0;
                switch (mapping.arrayFormat)
                {
                case CU_AD_FORMAT_FLOAT:
                {
                    const float f = integer ? float(iv) : value;
                    memcpy(dst, &f, 4);
                    break;
                }
                case CU_AD_FORMAT_HALF:
                {
                    const uint16_t h = FloatToHalf(integer ? float(iv) : value);
                    memcpy(dst, &h, 2);
                    break;
                }
                default:
                {
                    uint32_t raw;
                    if (integer || mapping.integer)
                    {
                        raw = integer ? iv : uint32_t(int32_t(value));
                    }
                    else if (info.isSigned)
                    {
                        const float maxValue = float((1u << (channelBytes * 8 - 1)) - 1);
                        raw = uint32_t(int32_t(std::lround(std::clamp(value, -1.f, 1.f) * maxValue)));
                    }
                    else
                    {
                        const float maxValue = float(channelBytes == 4 ? 4294967295.0 : double((1ull << (channelBytes * 8)) - 1));
                        raw = uint32_t(std::llround(std::clamp(value, 0.f, 1.f) * maxValue));
                    }
                    memcpy(dst, &raw, channelBytes);
                    break;
                }
                }
            }
            return true;
        }

        // The z offset of a texture slice in its CUDA array: the depth of a 3D texture, the layer otherwise.
        uint32_t SliceZ(const TextureDesc& desc, const TextureSlice& slice)
        {
            return desc.dimension == TextureDimension::Texture3D ? slice.z : slice.arraySlice;
        }

        bool IsLayered(const TextureDesc& desc)
        {
            switch (desc.dimension)
            {
            case TextureDimension::Texture1DArray:
            case TextureDimension::Texture2DArray:
            case TextureDimension::TextureCube:
            case TextureDimension::TextureCubeArray:
                return true;
            default:
                return false;
            }
        }
    }

    CommandList::CommandList(Device* device, const Context& context, const CommandListParameters& params)
        : m_Device(device)
        , m_DeviceReference(device)
        , m_Context(context)
        , m_Desc(params)
        , m_Upload(device->getTransientPool(), std::max<size_t>(params.uploadChunkSize, 64 * 1024))
        , m_Scratch(device->getTransientPool(), std::max<size_t>(params.scratchChunkSize, 64 * 1024))
    {
    }

    CommandList::~CommandList()
    {
        m_Instance = nullptr;
        m_Upload.releaseAll();
        m_Scratch.releaseAll();
    }

    nvrhi::IDevice* CommandList::getDevice() noexcept
    {
        return m_Device;
    }

    NativeObject CommandList::getNativeObject(ObjectType objectType) noexcept
    {
        switch (objectType)
        {
        case ObjectTypes::CUDA_Stream:
        {
            Queue* queue = m_Device->getQueue(m_Desc.queueType);
            return queue ? NativeObject(queue->getStream()) : nullptr;
        }
        case ObjectTypes::Nvrhi_CUDA_CommandList:
            return NativeObject(this);
        default:
            return nullptr;
        }
    }

    MonoPtr<CommandListInstance> CommandList::takeInstance()
    {
        if (m_IsOpen)
        {
            m_Context.error("executeCommandLists: the command list is open; close it first");
            return nullptr;
        }
        if (!m_Instance)
            return nullptr;

        m_Upload.detachChunks(m_Instance->chunks);
        m_Scratch.detachChunks(m_Instance->chunks);
        return std::move(m_Instance);
    }

    void CommandList::reference(IRHIObject* object)
    {
        if (object && m_Instance)
            m_Instance->references.push_back(object);
    }

    bool CommandList::allocateUpload(size_t size, size_t alignment, TransientAllocation& out)
    {
        if (!m_Upload.allocate(TransientMemory::Upload, size, alignment, out))
        {
            m_Context.error("out of upload memory");
            return false;
        }
        return true;
    }

    bool CommandList::uploadData(const void* data, size_t size, size_t alignment, CUdeviceptr& outDevicePtr)
    {
        TransientAllocation allocation;
        if (!allocateUpload(size, alignment, allocation))
            return false;
        if (data)
            memcpy(allocation.hostPtr, data, size);
        outDevicePtr = allocation.devicePtr;
        return true;
    }

    bool CommandList::allocateScratch(size_t size, size_t alignment, CUdeviceptr& outDevicePtr)
    {
        TransientAllocation allocation;
        if (!m_Scratch.allocate(TransientMemory::Device, size, alignment, allocation))
        {
            m_Context.error("out of scratch memory");
            return false;
        }
        outDevicePtr = allocation.devicePtr;
        return true;
    }

    bool CommandList::copyToScratch(const TransientAllocation& upload, size_t size, size_t alignment, CUdeviceptr& outDevicePtr)
    {
        if (!allocateScratch(size, alignment, outDevicePtr))
            return false;
        CommandCopyMemory* command = appendCommand<CommandCopyMemory>(CommandType::CopyMemory);
        command->dst = outDevicePtr;
        command->src = upload.devicePtr;
        command->size = size;
        return true;
    }

    CUdeviceptr CommandList::getBufferAddress(Buffer* buffer, uint64_t offset)
    {
        if (buffer->desc.isVolatile)
        {
            auto it = m_VolatileBuffers.find(buffer);
            if (it == m_VolatileBuffers.end())
            {
                m_Context.error(std::string("volatile buffer ") + utils::DebugNameToString(buffer->desc.debugName) +
                    " is used before writeBuffer in this command list");
                return 0;
            }
            return it->second + offset;
        }
        return buffer->devicePtr + offset;
    }

    //////////////////////////////////////////////////////////////////////////
    // Recording state
    //////////////////////////////////////////////////////////////////////////

    void CommandList::open() noexcept
    {
        if (m_IsOpen)
            m_Context.warning("open: the command list is already open; the recording is discarded");

        // A recording that was never executed is discarded.
        m_Instance = nullptr;
        m_Upload.releaseAll();
        m_Scratch.releaseAll();

        try
        {
            m_Instance = MakeMono<CommandListInstance>();
            m_Instance->queue = m_Desc.queueType;
        }
        catch (...)
        {
            m_Context.error("open: out of memory");
            return;
        }
        clearState();
        m_IsOpen = true;
    }

    void CommandList::close() noexcept
    {
        m_IsOpen = false;
        clearState();
    }

    void CommandList::clearState() noexcept
    {
        m_ComputeState = ComputeState();
        m_ComputeStateValid = false;
        m_RayTracingState = rt::State();
        m_RayTracingStateValid = false;
        m_PushConstants.clear();
        m_VolatileBuffers.clear();
    }

    void CommandList::enqueueNativeWork(INativeWork* work) noexcept
    {
        if (!work || !m_Instance)
            return;
        try
        {
            reference(work);
            appendCommand<CommandNativeWork>(CommandType::NativeWork)->work = work;
        }
        catch (...)
        {
            m_Context.error("enqueueNativeWork: out of memory");
        }
    }

    //////////////////////////////////////////////////////////////////////////
    // Buffers
    //////////////////////////////////////////////////////////////////////////

    void CommandList::writeBuffer(IBuffer* _buffer, const void* data, size_t dataSize, uint64_t destOffsetBytes) noexcept
    {
        Buffer* buffer = checked_cast<Buffer*>(_buffer);
        if (!buffer || !m_Instance || dataSize == 0)
            return;

        try
        {
            reference(buffer);

            if (buffer->desc.isVolatile)
            {
                // A new version of the buffer in upload memory; later dispatches of this list bind it.
                CUdeviceptr version = 0;
                if (!uploadData(data, dataSize, 256, version))
                    return;
                m_VolatileBuffers[buffer] = version - destOffsetBytes;
                return;
            }

            if (destOffsetBytes + dataSize > buffer->desc.byteSize)
            {
                m_Context.error("writeBuffer: the write exceeds the buffer");
                return;
            }

            CUdeviceptr source = 0;
            if (!uploadData(data, dataSize, 16, source))
                return;
            CommandCopyMemory* command = appendCommand<CommandCopyMemory>(CommandType::CopyMemory);
            command->dst = buffer->devicePtr + destOffsetBytes;
            command->src = source;
            command->size = dataSize;
        }
        catch (...)
        {
            m_Context.error("writeBuffer: out of memory");
        }
    }

    void CommandList::clearBufferUInt(IBuffer* _buffer, uint32_t clearValue) noexcept
    {
        Buffer* buffer = checked_cast<Buffer*>(_buffer);
        if (!buffer || !m_Instance)
            return;
        try
        {
            reference(buffer);
            CommandSetMemory32* command = appendCommand<CommandSetMemory32>(CommandType::SetMemory32);
            command->dst = buffer->devicePtr;
            command->value = clearValue;
            command->count = size_t(buffer->desc.byteSize / 4);
        }
        catch (...)
        {
            m_Context.error("clearBufferUInt: out of memory");
        }
    }

    void CommandList::copyBuffer(IBuffer* _dest, uint64_t destOffsetBytes, IBuffer* _src, uint64_t srcOffsetBytes,
        uint64_t dataSizeBytes) noexcept
    {
        Buffer* dest = checked_cast<Buffer*>(_dest);
        Buffer* src = checked_cast<Buffer*>(_src);
        if (!dest || !src || !m_Instance || dataSizeBytes == 0)
            return;
        try
        {
            reference(dest);
            reference(src);
            const CUdeviceptr source = getBufferAddress(src, srcOffsetBytes);
            if (!source)
                return;
            CommandCopyMemory* command = appendCommand<CommandCopyMemory>(CommandType::CopyMemory);
            command->dst = dest->devicePtr + destOffsetBytes;
            command->src = source;
            command->size = size_t(dataSizeBytes);
        }
        catch (...)
        {
            m_Context.error("copyBuffer: out of memory");
        }
    }

    //////////////////////////////////////////////////////////////////////////
    // Textures
    //////////////////////////////////////////////////////////////////////////

    void CommandList::recordCopy3D(const CUDA_MEMCPY3D& copy)
    {
        appendCommand<CommandCopy3D>(CommandType::Copy3D)->copy = copy;
    }

    void CommandList::writeTexture(ITexture* _dest, uint32_t arraySlice, uint32_t mipLevel, const void* data,
        size_t rowPitch, size_t depthPitch) noexcept
    {
        Texture* dest = checked_cast<Texture*>(_dest);
        if (!dest || !data || !m_Instance || mipLevel >= dest->levels.size())
            return;

        try
        {
            const TextureDesc& desc = dest->desc;
            const FormatMapping& mapping = GetFormatMapping(desc.format);
            const uint32_t block = mapping.blockSize;
            const uint32_t width = std::max(desc.width >> mipLevel, 1u);
            const uint32_t height = std::max(desc.height >> mipLevel, 1u);
            const uint32_t depth = desc.dimension == TextureDimension::Texture3D ? std::max(desc.depth >> mipLevel, 1u) : 1u;
            const size_t rowBytes = size_t((width + block - 1) / block) * mapping.bytesPerElement;
            const uint32_t rows = (height + block - 1) / block;

            TransientAllocation upload;
            if (!allocateUpload(rowBytes * rows * depth, 256, upload))
                return;
            for (uint32_t z = 0; z < depth; ++z)
            {
                for (uint32_t y = 0; y < rows; ++y)
                {
                    memcpy(upload.hostPtr + (size_t(z) * rows + y) * rowBytes,
                        static_cast<const uint8_t*>(data) + z * depthPitch + y * rowPitch, rowBytes);
                }
            }

            reference(dest);
            CUDA_MEMCPY3D copy = {};
            copy.srcMemoryType = CU_MEMORYTYPE_HOST;
            copy.srcHost = upload.hostPtr;
            copy.srcPitch = rowBytes;
            copy.srcHeight = rows;
            copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            copy.dstArray = dest->levels[mipLevel];
            copy.dstZ = IsLayered(desc) ? arraySlice : 0;
            copy.WidthInBytes = rowBytes;
            copy.Height = rows;
            copy.Depth = depth;
            recordCopy3D(copy);
        }
        catch (...)
        {
            m_Context.error("writeTexture: out of memory");
        }
    }

    void CommandList::copyTexture1(ITexture* _dest, const TextureSlice& destSlice, ITexture* _src,
        const TextureSlice& srcSlice) noexcept
    {
        Texture* dest = checked_cast<Texture*>(_dest);
        Texture* src = checked_cast<Texture*>(_src);
        if (!dest || !src || !m_Instance)
            return;

        try
        {
            const TextureSlice d = destSlice.resolve(dest->desc);
            const TextureSlice s = srcSlice.resolve(src->desc);
            if (d.mipLevel >= dest->levels.size() || s.mipLevel >= src->levels.size())
                return;
            const FormatMapping& mapping = GetFormatMapping(src->desc.format);
            const uint32_t block = mapping.blockSize;

            reference(dest);
            reference(src);
            CUDA_MEMCPY3D copy = {};
            copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            copy.srcArray = src->levels[s.mipLevel];
            copy.srcXInBytes = size_t(s.x / block) * mapping.bytesPerElement;
            copy.srcY = s.y / block;
            copy.srcZ = SliceZ(src->desc, s);
            copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            copy.dstArray = dest->levels[d.mipLevel];
            copy.dstXInBytes = size_t(d.x / block) * mapping.bytesPerElement;
            copy.dstY = d.y / block;
            copy.dstZ = SliceZ(dest->desc, d);
            copy.WidthInBytes = size_t((s.width + block - 1) / block) * mapping.bytesPerElement;
            copy.Height = (s.height + block - 1) / block;
            copy.Depth = src->desc.dimension == TextureDimension::Texture3D ? s.depth : 1;
            recordCopy3D(copy);
        }
        catch (...)
        {
            m_Context.error("copyTexture: out of memory");
        }
    }

    void CommandList::copyTexture2(IStagingTexture* _dest, const TextureSlice& destSlice, ITexture* _src,
        const TextureSlice& srcSlice) noexcept
    {
        StagingTexture* dest = checked_cast<StagingTexture*>(_dest);
        Texture* src = checked_cast<Texture*>(_src);
        if (!dest || !src || !m_Instance)
            return;

        try
        {
            const TextureSlice d = destSlice.resolve(dest->desc);
            const TextureSlice s = srcSlice.resolve(src->desc);
            if (s.mipLevel >= src->levels.size())
                return;
            const FormatMapping& mapping = GetFormatMapping(src->desc.format);
            const uint32_t block = mapping.blockSize;
            const StagingTexture::SliceRegion region = dest->getSliceRegion(d.mipLevel, d.arraySlice);

            reference(dest);
            reference(src);
            CUDA_MEMCPY3D copy = {};
            copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            copy.srcArray = src->levels[s.mipLevel];
            copy.srcXInBytes = size_t(s.x / block) * mapping.bytesPerElement;
            copy.srcY = s.y / block;
            copy.srcZ = SliceZ(src->desc, s);
            copy.dstMemoryType = CU_MEMORYTYPE_HOST;
            copy.dstHost = dest->hostPtr + region.offset;
            copy.dstXInBytes = size_t(d.x / block) * mapping.bytesPerElement;
            copy.dstY = d.y / block;
            copy.dstZ = dest->desc.dimension == TextureDimension::Texture3D ? d.z : 0;
            copy.dstPitch = region.rowPitch;
            copy.dstHeight = region.rowPitch ? region.depthPitch / region.rowPitch : 0;
            copy.WidthInBytes = size_t((s.width + block - 1) / block) * mapping.bytesPerElement;
            copy.Height = (s.height + block - 1) / block;
            copy.Depth = src->desc.dimension == TextureDimension::Texture3D ? s.depth : 1;
            recordCopy3D(copy);
        }
        catch (...)
        {
            m_Context.error("copyTexture: out of memory");
        }
    }

    void CommandList::copyTexture3(ITexture* _dest, const TextureSlice& destSlice, IStagingTexture* _src,
        const TextureSlice& srcSlice) noexcept
    {
        Texture* dest = checked_cast<Texture*>(_dest);
        StagingTexture* src = checked_cast<StagingTexture*>(_src);
        if (!dest || !src || !m_Instance)
            return;

        try
        {
            const TextureSlice d = destSlice.resolve(dest->desc);
            const TextureSlice s = srcSlice.resolve(src->desc);
            if (d.mipLevel >= dest->levels.size())
                return;
            const FormatMapping& mapping = GetFormatMapping(dest->desc.format);
            const uint32_t block = mapping.blockSize;
            const StagingTexture::SliceRegion region = src->getSliceRegion(s.mipLevel, s.arraySlice);

            reference(dest);
            reference(src);
            CUDA_MEMCPY3D copy = {};
            copy.srcMemoryType = CU_MEMORYTYPE_HOST;
            copy.srcHost = src->hostPtr + region.offset;
            copy.srcXInBytes = size_t(s.x / block) * mapping.bytesPerElement;
            copy.srcY = s.y / block;
            copy.srcZ = src->desc.dimension == TextureDimension::Texture3D ? s.z : 0;
            copy.srcPitch = region.rowPitch;
            copy.srcHeight = region.rowPitch ? region.depthPitch / region.rowPitch : 0;
            copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            copy.dstArray = dest->levels[d.mipLevel];
            copy.dstXInBytes = size_t(d.x / block) * mapping.bytesPerElement;
            copy.dstY = d.y / block;
            copy.dstZ = SliceZ(dest->desc, d);
            copy.WidthInBytes = size_t((s.width + block - 1) / block) * mapping.bytesPerElement;
            copy.Height = (s.height + block - 1) / block;
            copy.Depth = dest->desc.dimension == TextureDimension::Texture3D ? s.depth : 1;
            recordCopy3D(copy);
        }
        catch (...)
        {
            m_Context.error("copyTexture: out of memory");
        }
    }

    void CommandList::recordClearTexture(Texture* texture, const TextureSubresourceSet& _subresources, const void* texel)
    {
        const TextureDesc& desc = texture->desc;
        const FormatMapping& mapping = GetFormatMapping(desc.format);
        const TextureSubresourceSet subresources = _subresources.resolve(desc, false);
        const bool layered = IsLayered(desc);

        reference(texture);
        for (MipLevel mip = subresources.baseMipLevel; mip < subresources.baseMipLevel + subresources.numMipLevels; ++mip)
        {
            const uint32_t width = std::max(desc.width >> mip, 1u);
            const uint32_t height = std::max(desc.height >> mip, 1u);
            const uint32_t depth = desc.dimension == TextureDimension::Texture3D ? std::max(desc.depth >> mip, 1u)
                : (layered ? subresources.numArraySlices : 1u);
            const size_t rowBytes = size_t(width) * mapping.bytesPerElement;
            const size_t size = rowBytes * height * depth;

            TransientAllocation upload;
            if (!allocateUpload(size, 256, upload))
                return;
            for (size_t offset = 0; offset < size; offset += mapping.bytesPerElement)
                memcpy(upload.hostPtr + offset, texel, mapping.bytesPerElement);

            CUDA_MEMCPY3D copy = {};
            copy.srcMemoryType = CU_MEMORYTYPE_HOST;
            copy.srcHost = upload.hostPtr;
            copy.srcPitch = rowBytes;
            copy.srcHeight = height;
            copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            copy.dstArray = texture->levels[mip];
            copy.dstZ = layered ? subresources.baseArraySlice : 0;
            copy.WidthInBytes = rowBytes;
            copy.Height = height;
            copy.Depth = depth;
            recordCopy3D(copy);
        }
    }

    void CommandList::clearTextureFloat(ITexture* _texture, const TextureSubresourceSet& subresources,
        const Color& clearColor) noexcept
    {
        Texture* texture = checked_cast<Texture*>(_texture);
        if (!texture || !m_Instance)
            return;
        try
        {
            uint8_t texel[16] = {};
            const float color[4] = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };
            if (!EncodeTexel(texture->desc.format, color, nullptr, texel))
            {
                m_Context.error("clearTextureFloat: the format cannot be cleared");
                return;
            }
            recordClearTexture(texture, subresources, texel);
        }
        catch (...)
        {
            m_Context.error("clearTextureFloat: out of memory");
        }
    }

    void CommandList::clearTextureUInt(ITexture* _texture, const TextureSubresourceSet& subresources,
        uint32_t clearColor) noexcept
    {
        Texture* texture = checked_cast<Texture*>(_texture);
        if (!texture || !m_Instance)
            return;
        try
        {
            uint8_t texel[16] = {};
            if (!EncodeTexel(texture->desc.format, nullptr, &clearColor, texel))
            {
                m_Context.error("clearTextureUInt: the format cannot be cleared");
                return;
            }
            recordClearTexture(texture, subresources, texel);
        }
        catch (...)
        {
            m_Context.error("clearTextureUInt: out of memory");
        }
    }

    void CommandList::clearDepthStencilTexture(ITexture* t, const TextureSubresourceSet& subresources, bool clearDepth,
        float depth, bool clearStencil, uint8_t stencil) noexcept
    {
        (void)t; (void)subresources; (void)clearDepth; (void)depth; (void)clearStencil; (void)stencil;
        m_Context.error("clearDepthStencilTexture is not supported on the CUDA backend");
    }

    void CommandList::resolveTexture(ITexture* dest, const TextureSubresourceSet& dstSubresources, ITexture* src,
        const TextureSubresourceSet& srcSubresources) noexcept
    {
        (void)dest; (void)dstSubresources; (void)src; (void)srcSubresources;
        m_Context.error("resolveTexture is not supported on the CUDA backend");
    }

    void CommandList::clearSamplerFeedbackTexture(ISamplerFeedbackTexture* texture) noexcept
    {
        (void)texture;
        utils::NotSupported();
    }

    void CommandList::decodeSamplerFeedbackTexture(IBuffer* buffer, ISamplerFeedbackTexture* texture, Format format) noexcept
    {
        (void)buffer; (void)texture; (void)format;
        utils::NotSupported();
    }

    void CommandList::setSamplerFeedbackTextureState(ISamplerFeedbackTexture* texture, ResourceStates stateBits) noexcept
    {
        (void)texture; (void)stateBits;
        utils::NotSupported();
    }

    //////////////////////////////////////////////////////////////////////////
    // Graphics: not supported
    //////////////////////////////////////////////////////////////////////////

    void CommandList::setGraphicsState(const GraphicsState& state) noexcept
    {
        (void)state;
        m_Context.error("graphics is not supported on the CUDA backend");
    }

    void CommandList::draw(const DrawArguments& args) noexcept { (void)args; utils::NotSupported(); }
    void CommandList::drawIndexed(const DrawArguments& args) noexcept { (void)args; utils::NotSupported(); }
    void CommandList::drawIndirect(uint32_t offsetBytes, uint32_t drawCount) noexcept { (void)offsetBytes; (void)drawCount; utils::NotSupported(); }
    void CommandList::drawIndexedIndirect(uint32_t offsetBytes, uint32_t drawCount) noexcept { (void)offsetBytes; (void)drawCount; utils::NotSupported(); }
    void CommandList::drawIndexedIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount) noexcept { (void)paramOffsetBytes; (void)countOffsetBytes; (void)maxDrawCount; utils::NotSupported(); }
    void CommandList::setMeshletState(const MeshletState& state) noexcept { (void)state; m_Context.error("meshlets are not supported on the CUDA backend"); }
    void CommandList::dispatchMesh(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) noexcept { (void)groupsX; (void)groupsY; (void)groupsZ; utils::NotSupported(); }
    void CommandList::dispatchMeshIndirect(uint32_t offsetBytes, uint32_t maxDrawCount) noexcept { (void)offsetBytes; (void)maxDrawCount; utils::NotSupported(); }
    void CommandList::dispatchMeshIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount) noexcept { (void)paramOffsetBytes; (void)countOffsetBytes; (void)maxDrawCount; utils::NotSupported(); }
    void CommandList::buildOpacityMicromap(rt::IOpacityMicromap* omm, const rt::OpacityMicromapDesc& desc) noexcept { (void)omm; (void)desc; utils::NotSupported(); }
    void CommandList::executeMultiIndirectClusterOperation(const rt::cluster::OperationDesc& desc) noexcept { (void)desc; utils::NotSupported(); }
    void CommandList::convertCoopVecMatrices(coopvec::ConvertMatrixLayoutDesc const* convertDescs, size_t numDescs) noexcept { (void)convertDescs; (void)numDescs; utils::NotSupported(); }

    //////////////////////////////////////////////////////////////////////////
    // Compute
    //////////////////////////////////////////////////////////////////////////

    void CommandList::setPushConstants(const void* data, size_t byteSize) noexcept
    {
        try
        {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            m_PushConstants.assign(bytes, bytes + byteSize);
        }
        catch (...)
        {
            m_Context.error("setPushConstants: out of memory");
        }
    }

    void CommandList::setComputeState(const ComputeState& state) noexcept
    {
        if (!m_Instance)
            return;
        try
        {
            m_ComputeState = state;
            m_ComputeStateValid = state.pipeline != nullptr;
            m_PushConstants.clear();
            reference(state.pipeline);
            reference(state.indirectParams);
            for (IBindingSet* set : state.bindings)
                reference(set);
        }
        catch (...)
        {
            m_Context.error("setComputeState: out of memory");
        }
    }

    bool CommandList::writeBinding(const ParameterMap& parameters, const ParameterSlot& slot, const BindingSetItem& item,
        const BindingSetVector& bindings, uint8_t* destination)
    {
        const ShaderLibraryReflection* reflection = parameters.reflection;
        const ShaderParameterReflection& parameter = reflection->parameters[slot.parameterIndex];

        switch (item.type)
        {
        case ResourceType::ConstantBuffer:
        case ResourceType::VolatileConstantBuffer:
        {
            Buffer* buffer = checked_cast<Buffer*>(item.resourceHandle);
            if (!buffer)
                return true;
            const CUdeviceptr address = getBufferAddress(buffer, item.range.byteOffset);
            memcpy(destination, &address, sizeof(address));
            return address != 0;
        }

        case ResourceType::Texture_SRV:
        {
            Texture* texture = checked_cast<Texture*>(item.resourceHandle);
            if (!texture)
                return true;
            // The sampler: a Sampler binding at the same slot (s<n> with t<n>), in the same set first.
            const SamplerDesc* sampler = nullptr;
            for (IBindingSet* set : bindings)
            {
                if (!set || !set->getDesc())
                    continue;
                for (const BindingSetItem& candidate : set->getDesc()->bindings)
                {
                    if (candidate.type == ResourceType::Sampler && candidate.slot == item.slot && candidate.resourceHandle)
                    {
                        sampler = &checked_cast<Sampler*>(candidate.resourceHandle)->desc;
                        break;
                    }
                }
                if (sampler)
                    break;
            }
            const CUtexObject object = texture->getTextureObject(item.subresources, item.format, sampler);
            memcpy(destination, &object, sizeof(object));
            return object != 0;
        }

        case ResourceType::Texture_UAV:
        {
            Texture* texture = checked_cast<Texture*>(item.resourceHandle);
            if (!texture)
                return true;
            const CUsurfObject surface = texture->getSurfaceObject(item.subresources.baseMipLevel);
            if (!surface)
            {
                m_Context.error(std::string("texture ") + utils::DebugNameToString(texture->desc.debugName) +
                    " is bound as a UAV but was not created with isUAV");
                return false;
            }
            memcpy(destination, &surface, sizeof(surface));
            return true;
        }

        case ResourceType::TypedBuffer_SRV:
        case ResourceType::TypedBuffer_UAV:
        case ResourceType::StructuredBuffer_SRV:
        case ResourceType::StructuredBuffer_UAV:
        case ResourceType::RawBuffer_SRV:
        case ResourceType::RawBuffer_UAV:
        {
            // { T* data; size_t count } for structured and typed buffers, { uint32_t* data; size_t bytes } for raw
            // buffers.
            Buffer* buffer = checked_cast<Buffer*>(item.resourceHandle);
            if (!buffer)
                return true;
            const BufferRange range = item.range.resolve(buffer->desc);
            const CUdeviceptr address = getBufferAddress(buffer, range.byteOffset);
            uint64_t count = range.byteSize;
            if (item.type != ResourceType::RawBuffer_SRV && item.type != ResourceType::RawBuffer_UAV)
            {
                uint64_t stride = buffer->desc.structStride;
                if (item.type == ResourceType::TypedBuffer_SRV || item.type == ResourceType::TypedBuffer_UAV)
                    stride = getFormatInfo(item.format != Format::UNKNOWN ? item.format : buffer->desc.format).bytesPerBlock;
                if (stride == 0 && parameter.typeIndex < reflection->types.size())
                    stride = reflection->types[parameter.typeIndex].size;
                count = stride ? range.byteSize / stride : 0;
            }
            memcpy(destination, &address, sizeof(address));
            if (slot.stride >= 16 || parameter.uniformSize >= 16)
                memcpy(destination + 8, &count, sizeof(count));
            return address != 0;
        }

        case ResourceType::Sampler:
            return true;    // samplers are part of the texture objects

        case ResourceType::RayTracingAccelStruct:
        {
            AccelStruct* as = checked_cast<AccelStruct*>(item.resourceHandle);
            if (!as)
                return true;
            reference(as);
            CommandPatchTraversable* patch = appendCommand<CommandPatchTraversable>(CommandType::PatchTraversable);
            patch->dst = reinterpret_cast<uint64_t*>(destination);
            patch->as = as;
            return true;
        }

        default:
            m_Context.error(std::string("binding type ") + utils::ResourceTypeToString(item.type) +
                " is not supported on the CUDA backend");
            return false;
        }
    }

    bool CommandList::writeGlobals(const ParameterMap& parameters, const BindingSetVector& bindings, uint8_t* globals,
        const char* what)
    {
        bool ok = true;
        for (IBindingSet* _set : bindings)
        {
            BindingSet* set = _set ? checked_cast<BindingSet*>(_set) : nullptr;
            if (!set)
                continue;
            const BindingLayoutDesc* layoutDesc = set->layout->getDesc();
            const uint32_t space = layoutDesc ? layoutDesc->registerSpace : 0;

            for (const BindingSetItem& item : set->desc.bindings)
            {
                const RegisterClass registerClass = GetRegisterClass(item.type);
                if (registerClass == RegisterClass::PushConstants || registerClass == RegisterClass::None)
                    continue;

                const ParameterSlot* slot = parameters.find(registerClass, space, item.slot);
                if (!slot)
                    continue;   // a binding the kernel does not use

                const uint32_t element = item.arrayElement + (item.slot - slot->firstSlot);
                if (element >= slot->slotCount)
                {
                    m_Context.error(std::string(what) + ": array element out of range");
                    ok = false;
                    continue;
                }
                uint8_t* destination = globals + slot->offset + size_t(element) * slot->stride;
                if (slot->offset + size_t(element) * slot->stride + 8 > parameters.globalsSize)
                {
                    m_Context.error(std::string(what) + ": a binding lies outside the global parameter block");
                    ok = false;
                    continue;
                }
                ok = writeBinding(parameters, *slot, item, bindings, destination) && ok;
            }
        }

        // [[vk::push_constant]] constant buffers: the push constants, in upload memory.
        for (const ParameterSlot& slot : parameters.getSlots())
        {
            if (slot.kind != ShaderParameterKind::PushConstants)
                continue;
            CUdeviceptr address = 0;
            if (!m_PushConstants.empty() && !uploadData(m_PushConstants.data(), m_PushConstants.size(), 16, address))
                return false;
            memcpy(globals + slot.offset, &address, sizeof(address));
        }
        return ok;
    }

    void CommandList::recordLaunch(bool indirect, uint32_t gx, uint32_t gy, uint32_t gz, CUdeviceptr indirectArgs)
    {
        if (!m_Instance || !m_ComputeStateValid)
        {
            m_Context.error("dispatch: no compute state");
            return;
        }

        try
        {
            ComputePipeline* pipeline = checked_cast<ComputePipeline*>(m_ComputeState.pipeline);
            KernelInstance kernel;
            if (!pipeline->getKernel(m_Desc.queueType, kernel))
                return;

            CUdeviceptr globalsSrc = 0;
            const size_t globalsSize = pipeline->parameters.globalsSize;
            if (globalsSize > 0)
            {
                TransientAllocation globals;
                if (!allocateUpload(globalsSize, 16, globals))
                    return;
                memset(globals.hostPtr, 0, globalsSize);
                const std::string what = "dispatch of " + pipeline->shader->symbolName;
                if (!writeGlobals(pipeline->parameters, m_ComputeState.bindings, globals.hostPtr, what.c_str()))
                    return;
                globalsSrc = globals.devicePtr;
            }

            // The kernel arguments: the push constants (entry point uniform parameters).
            const size_t paramsSize = pipeline->shader->entryPoint ? pipeline->paramsSize : m_PushConstants.size();
            if (paramsSize > 0 && m_PushConstants.size() < paramsSize)
                m_Context.warning("dispatch of " + pipeline->shader->symbolName + ": fewer push constant bytes than "
                    "the kernel arguments; the rest is zero");

            CommandLaunch* command = appendCommand<CommandLaunch>(indirect ? CommandType::LaunchIndirect : CommandType::Launch, paramsSize);
            command->function = kernel.function;
            command->grid[0] = gx;
            command->grid[1] = gy;
            command->grid[2] = gz;
            for (int axis = 0; axis < 3; ++axis)
                command->block[axis] = pipeline->blockSize[axis];
            command->globalsDst = globalsSize ? kernel.globals : 0;
            command->globalsSrc = globalsSrc;
            command->globalsSize = std::min(globalsSize, kernel.globalsSize);
            command->indirectArgs = indirectArgs;
            command->paramsSize = uint32_t(paramsSize);
            if (paramsSize)
            {
                uint8_t* params = reinterpret_cast<uint8_t*>(command + 1);
                memcpy(params, m_PushConstants.data(), std::min(paramsSize, m_PushConstants.size()));
            }
        }
        catch (...)
        {
            m_Context.error("dispatch: out of memory");
        }
    }

    void CommandList::dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) noexcept
    {
        if (groupsX == 0 || groupsY == 0 || groupsZ == 0)
            return;
        recordLaunch(false, groupsX, groupsY, groupsZ, 0);
    }

    void CommandList::dispatchIndirect(uint32_t offsetBytes) noexcept
    {
        Buffer* buffer = m_ComputeState.indirectParams ? checked_cast<Buffer*>(m_ComputeState.indirectParams) : nullptr;
        if (!buffer)
        {
            m_Context.error("dispatchIndirect: no indirect parameter buffer in the compute state");
            return;
        }
        recordLaunch(true, 0, 0, 0, buffer->devicePtr + offsetBytes);
    }

    //////////////////////////////////////////////////////////////////////////
    // Timer queries
    //////////////////////////////////////////////////////////////////////////

    void CommandList::beginTimerQuery(ITimerQuery* _query) noexcept
    {
        TimerQuery* query = checked_cast<TimerQuery*>(_query);
        if (!query || !m_Instance)
            return;
        try
        {
            m_Instance->timerQueries.push_back(query);
            query->started = true;
            query->resolved = false;
            appendCommand<CommandRecordEvent>(CommandType::RecordEvent)->event = query->begin;
        }
        catch (...)
        {
            m_Context.error("beginTimerQuery: out of memory");
        }
    }

    void CommandList::endTimerQuery(ITimerQuery* _query) noexcept
    {
        TimerQuery* query = checked_cast<TimerQuery*>(_query);
        if (!query || !m_Instance)
            return;
        try
        {
            m_Instance->timerQueries.push_back(query);
            appendCommand<CommandRecordEvent>(CommandType::RecordEvent)->event = query->end;
        }
        catch (...)
        {
            m_Context.error("endTimerQuery: out of memory");
        }
    }
}
