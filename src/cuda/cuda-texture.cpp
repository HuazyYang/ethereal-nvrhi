// SPDX-License-Identifier: MIT
#include "cuda-backend.h"
#include <cmath>
namespace nvrhi::cuda
{
// Convert IEEE binary32 to binary16 with round-to-nearest, ties-to-even.
static uint16_t floatToHalf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000, exponent = (bits >> 23) & 255, mantissa = bits & 0x7fffff;
    if (exponent == 255)
        return uint16_t(sign | 0x7c00 | (mantissa ? 0x200 : 0));
    int halfExponent = int(exponent) - 127 + 15;
    if (halfExponent >= 31)
        return uint16_t(sign | 0x7c00);
    if (halfExponent <= 0)
    {
        if (halfExponent < -10)
            return uint16_t(sign);
        mantissa |= 0x800000;
        const uint32_t shift = uint32_t(14 - halfExponent);
        const uint32_t rounded = mantissa + ((1u << (shift - 1)) - 1) + ((mantissa >> shift) & 1);
        return uint16_t(sign | (rounded >> shift));
    }
    mantissa += 0xfff + ((mantissa >> 13) & 1);
    return uint16_t(sign | ((uint32_t(halfExponent) << 10) + (mantissa >> 13)));
}
static void recordClear(CommandList *list, Texture *texture, const TextureSubresourceSet &requested,
                        const std::vector<uint8_t> &pixel)
{
    const auto resources = requested.resolve(texture->desc, false);
    if (!resources.numMipLevels || !resources.numArraySlices || resources.baseMipLevel >= texture->desc.mipLevels ||
        resources.baseArraySlice >= texture->desc.arraySize)
    {
        list->valid = false;
        return;
    }
    AutoPtr<Texture> keep(texture);
    for (uint32_t mip = resources.baseMipLevel; mip < resources.baseMipLevel + resources.numMipLevels; ++mip)
    {
        const uint32_t width = std::max(texture->desc.width >> mip, 1u);
        const uint32_t height = std::max(texture->desc.height >> mip, 1u);
        const uint32_t depth = texture->desc.dimension == TextureDimension::Texture3D
                                   ? std::max(texture->desc.depth >> mip, 1u)
                                   : resources.numArraySlices;
        const size_t rowBytes = size_t(width) * pixel.size();
        // Retain one plane, not a full volume, for large 3D texture clears.
        std::vector<uint8_t> plane(rowBytes * height);
        for (size_t offset = 0; offset < plane.size(); offset += pixel.size())
            std::memcpy(plane.data() + offset, pixel.data(), pixel.size());
        list->record([keep, resources, mip, height, depth, rowBytes, plane = std::move(plane)](Replay &r) {
            CUDA_MEMCPY3D copy{};
            copy.srcMemoryType = CU_MEMORYTYPE_HOST;
            copy.srcHost = plane.data();
            copy.srcPitch = rowBytes;
            copy.srcHeight = height;
            copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            copy.dstArray = keep->level(mip);
            copy.WidthInBytes = rowBytes;
            copy.Height = height;
            copy.Depth = 1;
            for (uint32_t z = 0; z < depth; ++z)
            {
                copy.dstZ = z + (keep->desc.dimension == TextureDimension::Texture3D ? 0 : resources.baseArraySlice);
                if (!r.context->check(cuMemcpy3DAsync(&copy, r.stream), "cuMemcpy3DAsync(texture clear)"))
                    return false;
            }
            return true;
        });
    }
}
void CommandList::clearTextureFloat(ITexture *iface, const TextureSubresourceSet &resources,
                                    const Color &color) noexcept
{
    auto texture = static_cast<Texture *>(iface);
    if (!texture || texture->context != context || (!texture->desc.isUAV && !texture->desc.isRenderTarget))
    {
        valid = false;
        return;
    }
    const auto &format = getFormatInfo(texture->desc.format);
    if (format.kind != FormatKind::Float && format.kind != FormatKind::Normalized)
    {
        unsupported();
        return;
    }
    try
    {
        const unsigned channels =
            unsigned(format.hasRed) + unsigned(format.hasGreen) + unsigned(format.hasBlue) + unsigned(format.hasAlpha);
        const unsigned bytes = format.bytesPerBlock / channels;
        const float components[] = {color.r, color.g, color.b, color.a};
        const bool present[] = {format.hasRed, format.hasGreen, format.hasBlue, format.hasAlpha};
        std::vector<uint8_t> pixel(format.bytesPerBlock);
        unsigned index = 0;
        for (unsigned component = 0; component < 4; ++component)
        {
            if (!present[component])
                continue;
            auto dst = pixel.data() + index++ * bytes;
            const float value = components[component];
            if (format.kind == FormatKind::Float)
            {
                if (bytes == 4)
                    std::memcpy(dst, &value, 4);
                else
                {
                    const uint16_t half = floatToHalf(value);
                    std::memcpy(dst, &half, 2);
                }
            }
            else
            {
                const float clamped =
                    std::isnan(value) ? 0.f : std::max(format.isSigned ? -1.f : 0.f, std::min(1.f, value));
                const int32_t maximum = format.isSigned ? ((1 << (bytes * 8 - 1)) - 1) : ((1 << (bytes * 8)) - 1);
                const int32_t normalized = int32_t(std::lround(clamped * maximum));
                std::memcpy(dst, &normalized, bytes);
            }
        }
        recordClear(this, texture, resources, pixel);
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::clearTextureUInt(ITexture *iface, const TextureSubresourceSet &resources, uint32_t color) noexcept
{
    auto texture = static_cast<Texture *>(iface);
    if (!texture || texture->context != context || (!texture->desc.isUAV && !texture->desc.isRenderTarget))
    {
        valid = false;
        return;
    }
    const auto &format = getFormatInfo(texture->desc.format);
    if (format.kind != FormatKind::Integer)
    {
        unsupported();
        return;
    }
    try
    {
        const unsigned channels =
            unsigned(format.hasRed) + unsigned(format.hasGreen) + unsigned(format.hasBlue) + unsigned(format.hasAlpha);
        const unsigned bytes = format.bytesPerBlock / channels;
        std::vector<uint8_t> pixel(format.bytesPerBlock);
        for (unsigned i = 0; i < channels; ++i)
            std::memcpy(pixel.data() + i * bytes, &color, bytes);
        recordClear(this, texture, resources, pixel);
    }
    catch (...)
    {
        valid = false;
    }
}
static bool validSlice(const TextureDesc &d, const TextureSlice &s)
{
    if (s.mipLevel >= d.mipLevels || s.arraySlice >= d.arraySize)
        return false;
    const auto w = std::max(d.width >> s.mipLevel, 1u), h = std::max(d.height >> s.mipLevel, 1u),
               z = d.dimension == TextureDimension::Texture3D ? std::max(d.depth >> s.mipLevel, 1u) : 1u;
    return s.x <= w && s.width <= w - s.x && s.y <= h && s.height <= h - s.y && s.z <= z && s.depth <= z - s.z;
}
static void setArraySide(CUDA_MEMCPY3D &copy, Texture *texture, const TextureSlice &slice, bool source)
{
    size_t z = texture->desc.dimension == TextureDimension::Texture3D ? slice.z : slice.arraySlice;
    auto bytes = getFormatInfo(texture->desc.format).bytesPerBlock;
    if (source)
    {
        copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        copy.srcArray = texture->level(slice.mipLevel);
        copy.srcXInBytes = size_t(slice.x) * bytes;
        copy.srcY = slice.y;
        copy.srcZ = z;
    }
    else
    {
        copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        copy.dstArray = texture->level(slice.mipLevel);
        copy.dstXInBytes = size_t(slice.x) * bytes;
        copy.dstY = slice.y;
        copy.dstZ = z;
    }
}
void CommandList::writeTexture(ITexture *dest, uint32_t layer, uint32_t mip, const void *data, size_t rowPitch,
                               size_t depthPitch) noexcept
{
    auto texture = static_cast<Texture *>(dest);
    if (!texture || !data || mip >= texture->desc.mipLevels || layer >= texture->desc.arraySize)
    {
        valid = false;
        return;
    }
    TextureSlice slice;
    slice.mipLevel = mip;
    slice.arraySlice = layer;
    slice = slice.resolve(texture->desc);
    const size_t rowBytes = size_t(slice.width) * getFormatInfo(texture->desc.format).bytesPerBlock;
    if (!rowPitch)
        rowPitch = rowBytes;
    if (!depthPitch)
        depthPitch = rowPitch * slice.height;
    if (rowPitch < rowBytes || depthPitch < rowPitch * slice.height)
    {
        valid = false;
        return;
    }
    try
    {
        std::vector<uint8_t> packed(rowBytes * slice.height * slice.depth);
        for (uint32_t z = 0; z < slice.depth; ++z)
            for (uint32_t y = 0; y < slice.height; ++y)
                std::memcpy(packed.data() + (size_t(z) * slice.height + y) * rowBytes,
                            static_cast<const uint8_t *>(data) + size_t(z) * depthPitch + size_t(y) * rowPitch,
                            rowBytes);
        AutoPtr<Texture> keep(texture);
        record([keep, slice, rowBytes, packed = std::move(packed)](Replay &r) {
            CUDA_MEMCPY3D copy{};
            setArraySide(copy, keep, slice, false);
            copy.srcMemoryType = CU_MEMORYTYPE_HOST;
            copy.srcHost = packed.data();
            copy.srcPitch = rowBytes;
            copy.srcHeight = slice.height;
            copy.WidthInBytes = rowBytes;
            copy.Height = slice.height;
            copy.Depth = slice.depth;
            return r.context->check(cuMemcpy3DAsync(&copy, r.stream), "cuMemcpy3DAsync(upload)");
        });
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::copyTexture1(ITexture *dest, const TextureSlice &dst, ITexture *src,
                               const TextureSlice &source) noexcept
{
    auto d = static_cast<Texture *>(dest);
    auto s = static_cast<Texture *>(src);
    if (!d || !s || dst.mipLevel >= d->desc.mipLevels || source.mipLevel >= s->desc.mipLevels)
    {
        valid = false;
        return;
    }
    auto ds = dst.resolve(d->desc), ss = source.resolve(s->desc);
    if (!validSlice(d->desc, ds) || !validSlice(s->desc, ss) || ds.width != ss.width || ds.height != ss.height ||
        ds.depth != ss.depth ||
        getFormatInfo(d->desc.format).bytesPerBlock != getFormatInfo(s->desc.format).bytesPerBlock)
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<Texture> dk(d), sk(s);
        record([dk, sk, ds, ss](Replay &r) {
            CUDA_MEMCPY3D copy{};
            setArraySide(copy, dk, ds, false);
            setArraySide(copy, sk, ss, true);
            copy.WidthInBytes = size_t(ss.width) * getFormatInfo(sk->desc.format).bytesPerBlock;
            copy.Height = ss.height;
            copy.Depth = ss.depth;
            return r.context->check(cuMemcpy3DAsync(&copy, r.stream), "cuMemcpy3DAsync(copy)");
        });
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::copyTexture2(IStagingTexture *dest, const TextureSlice &dst, ITexture *src,
                               const TextureSlice &source) noexcept
{
    auto d = static_cast<StagingTexture *>(dest);
    auto s = static_cast<Texture *>(src);
    if (!d || !s || dst.mipLevel >= d->desc.mipLevels || source.mipLevel >= s->desc.mipLevels)
    {
        valid = false;
        return;
    }
    auto ds = dst.resolve(d->desc), ss = source.resolve(s->desc);
    if (!validSlice(d->desc, ds) || !validSlice(s->desc, ss) || ds.width != ss.width || ds.height != ss.height ||
        ds.depth != ss.depth || d->desc.format != s->desc.format)
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<StagingTexture> dk(d);
        AutoPtr<Texture> sk(s);
        record([dk, sk, ds, ss](Replay &r) {
            CUDA_MEMCPY3D copy{};
            setArraySide(copy, sk, ss, true);
            auto bytes = getFormatInfo(dk->desc.format).bytesPerBlock;
            copy.dstMemoryType = CU_MEMORYTYPE_HOST;
            copy.dstHost = dk->data.data();
            copy.dstPitch = size_t(dk->desc.width) * bytes;
            copy.dstHeight = dk->desc.height;
            copy.dstXInBytes = size_t(ds.x) * bytes;
            copy.dstY = ds.y;
            copy.dstZ = ds.z;
            copy.WidthInBytes = size_t(ss.width) * bytes;
            copy.Height = ss.height;
            copy.Depth = ss.depth;
            return r.context->check(cuMemcpy3DAsync(&copy, r.stream), "cuMemcpy3DAsync(readback)");
        });
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::copyTexture3(ITexture *dest, const TextureSlice &dst, IStagingTexture *src,
                               const TextureSlice &source) noexcept
{
    auto d = static_cast<Texture *>(dest);
    auto s = static_cast<StagingTexture *>(src);
    if (!d || !s || dst.mipLevel >= d->desc.mipLevels || source.mipLevel >= s->desc.mipLevels)
    {
        valid = false;
        return;
    }
    auto ds = dst.resolve(d->desc), ss = source.resolve(s->desc);
    if (!validSlice(d->desc, ds) || !validSlice(s->desc, ss) || ds.width != ss.width || ds.height != ss.height ||
        ds.depth != ss.depth || d->desc.format != s->desc.format)
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<Texture> dk(d);
        AutoPtr<StagingTexture> sk(s);
        record([dk, sk, ds, ss](Replay &r) {
            CUDA_MEMCPY3D copy{};
            setArraySide(copy, dk, ds, false);
            auto bytes = getFormatInfo(sk->desc.format).bytesPerBlock;
            copy.srcMemoryType = CU_MEMORYTYPE_HOST;
            copy.srcHost = sk->data.data();
            copy.srcPitch = size_t(sk->desc.width) * bytes;
            copy.srcHeight = sk->desc.height;
            copy.srcXInBytes = size_t(ss.x) * bytes;
            copy.srcY = ss.y;
            copy.srcZ = ss.z;
            copy.WidthInBytes = size_t(ss.width) * bytes;
            copy.Height = ss.height;
            copy.Depth = ss.depth;
            return r.context->check(cuMemcpy3DAsync(&copy, r.stream), "cuMemcpy3DAsync(staging upload)");
        });
    }
    catch (...)
    {
        valid = false;
    }
}
} // namespace nvrhi::cuda
