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

// Textures are CUDA mipmapped arrays: one CUarray per mip level holding every array slice (layered arrays) or
// depth slice (3D arrays). Shaders read them through texture objects, which also carry the sampler state (CUDA
// has no separate samplers), and write them through one surface object per mip level.

#include "cuda-backend.h"

#include <nvrhi/common/misc.h>

#include <algorithm>
#include <cstring>

namespace nvrhi::cuda
{
    bool GetArrayDescriptor(const TextureDesc& desc, bool forInterop, CUDA_ARRAY3D_DESCRIPTOR& out, std::string& error)
    {
        out = CUDA_ARRAY3D_DESCRIPTOR();

        const FormatMapping& mapping = GetFormatMapping(desc.format);
        if (mapping.arrayFormat == 0)
        {
            error = std::string("format ") + utils::FormatToString(desc.format) + " has no CUDA array format";
            return false;
        }
        if (desc.sampleCount > 1)
        {
            error = "multisampled textures are not supported";
            return false;
        }
        if (desc.isVirtual || desc.isTiled)
        {
            error = "virtual and tiled textures are not supported";
            return false;
        }

        out.Format = mapping.arrayFormat;
        out.NumChannels = mapping.channels;

        // Surface load/store needs uncompressed formats. A color target of the graphics API must be mapped with
        // COLOR_ATTACHMENT (cuExternalMemoryGetMappedMipmappedArray).
        if (desc.isUAV && mapping.blockSize == 1)
            out.Flags |= CUDA_ARRAY3D_SURFACE_LDST;
        if (forInterop && desc.isRenderTarget)
            out.Flags |= CUDA_ARRAY3D_COLOR_ATTACHMENT;

        switch (desc.dimension)
        {
        case TextureDimension::Texture1D:
            out.Width = desc.width;
            break;
        case TextureDimension::Texture1DArray:
            out.Width = desc.width;
            out.Depth = desc.arraySize;
            out.Flags |= CUDA_ARRAY3D_LAYERED;
            break;
        case TextureDimension::Texture2D:
            out.Width = desc.width;
            out.Height = desc.height;
            break;
        case TextureDimension::Texture2DArray:
            out.Width = desc.width;
            out.Height = desc.height;
            out.Depth = desc.arraySize;
            out.Flags |= CUDA_ARRAY3D_LAYERED;
            break;
        case TextureDimension::TextureCube:
            out.Width = desc.width;
            out.Height = desc.height;
            out.Depth = 6;
            out.Flags |= CUDA_ARRAY3D_CUBEMAP;
            break;
        case TextureDimension::TextureCubeArray:
            out.Width = desc.width;
            out.Height = desc.height;
            out.Depth = desc.arraySize;     // 6 per cube
            out.Flags |= CUDA_ARRAY3D_CUBEMAP | CUDA_ARRAY3D_LAYERED;
            break;
        case TextureDimension::Texture3D:
            out.Width = desc.width;
            out.Height = desc.height;
            out.Depth = desc.depth;
            break;
        default:
            error = "unsupported texture dimension";
            return false;
        }
        return true;
    }

    size_t TextureObjectKeyHash::operator()(const TextureObjectKey& k) const
    {
        size_t hash = 0;
        hash_combine(hash, k.subresources.baseMipLevel);
        hash_combine(hash, k.subresources.numMipLevels);
        hash_combine(hash, k.subresources.baseArraySlice);
        hash_combine(hash, k.subresources.numArraySlices);
        hash_combine(hash, uint32_t(k.format));
        hash_combine(hash, k.samplerHash);
        return hash;
    }

    namespace
    {
        CUaddress_mode ConvertAddressMode(SamplerAddressMode mode)
        {
            switch (mode)
            {
            case SamplerAddressMode::Wrap: return CU_TR_ADDRESS_MODE_WRAP;
            case SamplerAddressMode::Border: return CU_TR_ADDRESS_MODE_BORDER;
            case SamplerAddressMode::Mirror:
            case SamplerAddressMode::MirrorOnce: return CU_TR_ADDRESS_MODE_MIRROR;
            case SamplerAddressMode::Clamp:
            default: return CU_TR_ADDRESS_MODE_CLAMP;
            }
        }

        uint32_t HashSampler(const SamplerDesc* sampler)
        {
            if (!sampler)
                return 0;
            // FNV-1a over the fields that reach the texture object.
            uint32_t hash = 2166136261u;
            auto add = [&](const void* data, size_t size)
            {
                const uint8_t* bytes = static_cast<const uint8_t*>(data);
                for (size_t i = 0; i < size; ++i)
                    hash = (hash ^ bytes[i]) * 16777619u;
            };
            add(&sampler->borderColor, sizeof(sampler->borderColor));
            add(&sampler->maxAnisotropy, sizeof(sampler->maxAnisotropy));
            add(&sampler->mipBias, sizeof(sampler->mipBias));
            const uint8_t flags[] = { uint8_t(sampler->minFilter), uint8_t(sampler->magFilter), uint8_t(sampler->mipFilter),
                uint8_t(sampler->addressU), uint8_t(sampler->addressV), uint8_t(sampler->addressW) };
            add(flags, sizeof(flags));
            return hash | 1u;   // never 0, which stands for "no sampler"
        }
    }

    Texture::~Texture()
    {
        ContextScope scope(m_Context);
        for (auto& entry : m_TextureObjects)
            g_Cuda.cuTexObjectDestroy(entry.second);
        for (CUsurfObject surface : surfaces)
        {
            if (surface)
                g_Cuda.cuSurfObjectDestroy(surface);
        }
        if (mipmappedArray && (ownsMemory || externalMemory))
            g_Cuda.cuMipmappedArrayDestroy(mipmappedArray);
        if (externalMemory)
            g_Cuda.cuDestroyExternalMemory(externalMemory);
    }

    bool Texture::initialize()
    {
        levels.resize(desc.mipLevels);
        for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
        {
            if (!NVRHI_CUDA_CHECK(m_Context, cuMipmappedArrayGetLevel(&levels[mip], mipmappedArray, mip)))
                return false;
        }

        if (desc.isUAV && GetFormatMapping(desc.format).blockSize == 1)
        {
            surfaces.resize(desc.mipLevels);
            for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
            {
                CUDA_RESOURCE_DESC resource = {};
                resource.resType = CU_RESOURCE_TYPE_ARRAY;
                resource.res.array.hArray = levels[mip];
                if (!NVRHI_CUDA_CHECK(m_Context, cuSurfObjectCreate(&surfaces[mip], &resource)))
                    return false;
            }
        }
        return true;
    }

    CUsurfObject Texture::getSurfaceObject(MipLevel mipLevel) const
    {
        return mipLevel < surfaces.size() ? surfaces[mipLevel] : 0;
    }

    CUtexObject Texture::getTextureObject(const TextureSubresourceSet& _subresources, Format format, const SamplerDesc* sampler)
    {
        const TextureSubresourceSet subresources = _subresources.resolve(desc, false);
        if (format == Format::UNKNOWN)
            format = desc.format;

        TextureObjectKey key;
        key.subresources = subresources;
        key.format = format;
        key.samplerHash = HashSampler(sampler);

        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_TextureObjects.find(key);
        if (it != m_TextureObjects.end())
            return it->second;

        const FormatMapping& mapping = GetFormatMapping(format);
        const bool wholeChain = subresources.baseMipLevel == 0 && subresources.numMipLevels == desc.mipLevels;

        // A view of the whole mip chain, or of a single level (every array slice). Narrower views would need
        // CUDA resource views with explicit view formats; they are not needed by Slang code so far.
        CUDA_RESOURCE_DESC resource = {};
        if (wholeChain)
        {
            resource.resType = CU_RESOURCE_TYPE_MIPMAPPED_ARRAY;
            resource.res.mipmap.hMipmappedArray = mipmappedArray;
        }
        else
        {
            if (subresources.numMipLevels != 1)
                m_Context.warning("a texture view of several but not all mip levels is bound as its first level");
            resource.resType = CU_RESOURCE_TYPE_ARRAY;
            resource.res.array.hArray = levels[subresources.baseMipLevel];
        }

        SamplerDesc defaultSampler;
        defaultSampler.setAllFilters(false);
        const SamplerDesc& s = sampler ? *sampler : defaultSampler;

        CUDA_TEXTURE_DESC texture = {};
        texture.addressMode[0] = ConvertAddressMode(s.addressU);
        texture.addressMode[1] = ConvertAddressMode(s.addressV);
        texture.addressMode[2] = ConvertAddressMode(s.addressW);
        // Integer formats are read as integers and cannot be filtered.
        const bool filter = !mapping.integer && (s.minFilter || s.magFilter);
        texture.filterMode = filter ? CU_TR_FILTER_MODE_LINEAR : CU_TR_FILTER_MODE_POINT;
        texture.mipmapFilterMode = (!mapping.integer && s.mipFilter) ? CU_TR_FILTER_MODE_LINEAR : CU_TR_FILTER_MODE_POINT;
        texture.flags = CU_TRSF_NORMALIZED_COORDINATES;     // Slang samples with normalized coordinates
        if (mapping.integer)
            texture.flags |= CU_TRSF_READ_AS_INTEGER;
        if (mapping.srgb)
            texture.flags |= CU_TRSF_SRGB;
        texture.maxAnisotropy = unsigned(std::max(1.f, s.maxAnisotropy));
        texture.mipmapLevelBias = s.mipBias;
        texture.minMipmapLevelClamp = 0.f;
        texture.maxMipmapLevelClamp = float(wholeChain ? desc.mipLevels - 1 : 0);
        texture.borderColor[0] = s.borderColor.r;
        texture.borderColor[1] = s.borderColor.g;
        texture.borderColor[2] = s.borderColor.b;
        texture.borderColor[3] = s.borderColor.a;

        CUtexObject object = 0;
        ContextScope scope(m_Context);
        if (!NVRHI_CUDA_CHECK(m_Context, cuTexObjectCreate(&object, &resource, &texture, nullptr)))
            return 0;
        m_TextureObjects[key] = object;
        return object;
    }

    NativeObject Texture::getNativeView(ObjectType objectType, Format format, const TextureSubresourceSet& subresources,
        TextureDimension dimension, bool isReadOnlyDSV, const ComponentMapping* overrideComponentMapping) noexcept
    {
        (void)dimension; (void)isReadOnlyDSV; (void)overrideComponentMapping;
        try
        {
            switch (objectType)
            {
            case ObjectTypes::CUDA_TexObject:
                return NativeObject(uintptr_t(getTextureObject(subresources, format, nullptr)));
            case ObjectTypes::CUDA_SurfObject:
                return NativeObject(uintptr_t(getSurfaceObject(subresources.baseMipLevel)));
            case ObjectTypes::CUDA_Array:
                return subresources.baseMipLevel < levels.size() ? NativeObject(levels[subresources.baseMipLevel]) : nullptr;
            default:
                return nullptr;
            }
        }
        catch (...)
        {
            return nullptr;
        }
    }

    NativeObject Texture::getNativeObject(ObjectType objectType) noexcept
    {
        switch (objectType)
        {
        case ObjectTypes::CUDA_MipmappedArray:
            return NativeObject(mipmappedArray);
        case ObjectTypes::CUDA_Array:
            return levels.empty() ? nullptr : NativeObject(levels[0]);
        case ObjectTypes::CUDA_SurfObject:
            return NativeObject(uintptr_t(getSurfaceObject(0)));
        case ObjectTypes::CUDA_ExternalMemory:
            return NativeObject(externalMemory);
        default:
            return nullptr;
        }
    }

    TextureHandle Device::createTexture(const TextureDesc& desc)
    {
        if (desc.sharedResourceFlags != SharedResourceFlags::None)
        {
            m_Context.error("CUDA textures cannot be shared with other devices; import a graphics texture with "
                "IInteropDevice::importTexture instead");
            return nullptr;
        }

        CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
        std::string error;
        if (!GetArrayDescriptor(desc, false, arrayDesc, error))
        {
            m_Context.error("createTexture(" + std::string(utils::DebugNameToString(desc.debugName)) + "): " + error);
            return nullptr;
        }

        ContextScope scope(m_Context);
        AutoPtr<Texture> texture = TakeOver(MAKE_RC_OBJ(Texture, m_Context));
        texture->desc = desc;
        texture->desc.mipLevels = std::max(desc.mipLevels, 1u);
        if (!NVRHI_CUDA_CHECK(m_Context, cuMipmappedArrayCreate(&texture->mipmappedArray, &arrayDesc, texture->desc.mipLevels)))
            return nullptr;
        if (!texture->initialize())
            return nullptr;
        return texture;
    }

    TextureHandle Device::createHandleForNativeTexture(ObjectType objectType, NativeObject nativeTexture, const TextureDesc& desc)
    {
        if (objectType != ObjectTypes::CUDA_MipmappedArray || !nativeTexture)
            return nullptr;

        ContextScope scope(m_Context);
        AutoPtr<Texture> texture = TakeOver(MAKE_RC_OBJ(Texture, m_Context));
        texture->desc = desc;
        texture->desc.mipLevels = std::max(desc.mipLevels, 1u);
        texture->mipmappedArray = CUmipmappedArray(nativeTexture);
        texture->ownsMemory = false;
        if (!texture->initialize())
            return nullptr;
        return texture;
    }

    SamplerHandle Device::createSampler(const SamplerDesc& desc)
    {
        Sampler* sampler = MAKE_RC_OBJ(Sampler);
        sampler->desc = desc;
        return TakeOver(sampler);
    }

    //////////////////////////////////////////////////////////////////////////
    // Staging textures: pinned host memory, slices of tightly packed rows
    //////////////////////////////////////////////////////////////////////////

    StagingTexture::~StagingTexture()
    {
        if (hostPtr)
        {
            ContextScope scope(m_Context);
            g_Cuda.cuMemFreeHost(hostPtr);
        }
    }

    StagingTexture::SliceRegion StagingTexture::getSliceRegion(MipLevel mipLevel, ArraySlice arraySlice) const
    {
        const FormatMapping& mapping = GetFormatMapping(desc.format);
        const uint32_t block = mapping.blockSize;
        SliceRegion region;
        size_t offset = 0;
        for (ArraySlice slice = 0; slice < desc.arraySize; ++slice)
        {
            for (MipLevel mip = 0; mip < desc.mipLevels; ++mip)
            {
                const uint32_t w = std::max(desc.width >> mip, 1u);
                const uint32_t h = std::max(desc.height >> mip, 1u);
                const uint32_t d = desc.dimension == TextureDimension::Texture3D ? std::max(desc.depth >> mip, 1u) : 1u;
                const size_t rowPitch = size_t((w + block - 1) / block) * mapping.bytesPerElement;
                const size_t depthPitch = rowPitch * ((h + block - 1) / block);
                if (slice == arraySlice && mip == mipLevel)
                {
                    region.offset = offset;
                    region.rowPitch = rowPitch;
                    region.depthPitch = depthPitch;
                    return region;
                }
                offset = (offset + depthPitch * d + 255) & ~size_t(255);
            }
        }
        region.offset = offset;     // the total size, for mipLevel == mipLevels
        return region;
    }

    bool StagingTexture::initialize()
    {
        size = getSliceRegion(desc.mipLevels, desc.arraySize - 1).offset;
        if (desc.mipLevels > 0)
        {
            // The region past the last level of the last slice is the total size.
            const SliceRegion last = getSliceRegion(desc.mipLevels - 1, desc.arraySize - 1);
            const uint32_t d = desc.dimension == TextureDimension::Texture3D ? std::max(desc.depth >> (desc.mipLevels - 1), 1u) : 1u;
            size = last.offset + last.depthPitch * d;
        }
        void* host = nullptr;
        ContextScope scope(m_Context);
        if (!NVRHI_CUDA_CHECK(m_Context, cuMemHostAlloc(&host, std::max<size_t>(size, 1), CU_MEMHOSTALLOC_PORTABLE)))
            return false;
        hostPtr = static_cast<uint8_t*>(host);
        return true;
    }

    StagingTextureHandle Device::createStagingTexture(const TextureDesc& desc, CpuAccessMode cpuAccess)
    {
        const FormatMapping& mapping = GetFormatMapping(desc.format);
        if (mapping.bytesPerElement == 0)
            return nullptr;

        AutoPtr<StagingTexture> staging = TakeOver(MAKE_RC_OBJ(StagingTexture, m_Context));
        staging->desc = desc;
        staging->desc.mipLevels = std::max(desc.mipLevels, 1u);
        staging->desc.arraySize = std::max(desc.arraySize, 1u);
        staging->cpuAccess = cpuAccess;
        if (!staging->initialize())
            return nullptr;
        return staging;
    }

    void* Device::mapStagingTexture(IStagingTexture* _texture, const TextureSlice& _slice, CpuAccessMode cpuAccess,
        size_t& outRowPitch) noexcept
    {
        (void)cpuAccess;
        StagingTexture* texture = checked_cast<StagingTexture*>(_texture);
        if (!texture)
            return nullptr;
        const TextureSlice slice = _slice.resolve(texture->desc);
        const StagingTexture::SliceRegion region = texture->getSliceRegion(slice.mipLevel, slice.arraySlice);
        outRowPitch = region.rowPitch;
        return texture->hostPtr + region.offset;
    }
}
