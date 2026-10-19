// SPDX-License-Identifier: MIT
#include "cuda-backend.h"
#include <cstdio>
#if NVRHI_WITH_OPTIX
#include <optix_stubs.h>
#endif

namespace nvrhi::cuda
{
bool Context::check(CUresult result, const char *operation) const noexcept
{
    if (result == CUDA_SUCCESS)
        return true;
    const char *name = nullptr, *description = nullptr;
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &description);
    char message[1024];
    std::snprintf(message, sizeof(message), "%s: %s (%s)", operation, name ? name : "CUDA error",
                  description ? description : "");
    error(message);
    return false;
}
Context::~Context()
{
    if (!handle)
        return;
    {
        Scope scope(handle);
        for (auto s : streams)
            if (s)
                cuStreamSynchronize(s);
#if NVRHI_WITH_OPTIX
        if (optix)
            optixDeviceContextDestroy(optix);
#endif
        for (auto s : streams)
            if (s)
                cuStreamDestroy(s);
    }
    cuDevicePrimaryCtxRelease(device);
}
AllocationPtr allocate(ContextPtr context, size_t size)
{
    auto a = std::make_shared<Allocation>();
    a->context = context;
    if (size && !context->check(cuMemAlloc(&a->ptr, size), "cuMemAlloc"))
        return {};
    return a;
}
NativeObject Buffer::getNativeObject(ObjectType type) noexcept
{
    return type == ObjectTypes::CUDA_DevicePointer ? reinterpret_cast<void *>(memory->ptr) : nullptr;
}
bool Buffer::queryMemoryRequirements(MemoryRequirements &out) noexcept
{
    out.size = desc.byteSize;
    out.alignment = 256;
    return true;
}
Texture::~Texture()
{
    Scope scope(context->handle);
    if (owned && mipArray)
        cuMipmappedArrayDestroy(mipArray);
}
CUarray Texture::level(uint32_t mip) const
{
    CUarray a = externalArray;
    if (mipArray)
        context->check(cuMipmappedArrayGetLevel(&a, mipArray, mip), "cuMipmappedArrayGetLevel");
    return a;
}
NativeObject Texture::getNativeObject(ObjectType type) noexcept
{
    Scope scope(context->handle);
    return type == ObjectTypes::CUDA_Array ? level(0) : nullptr;
}
Shader::~Shader()
{
    Scope scope(context->handle);
    if (parameterFence)
        cuEventDestroy(parameterFence);
    if (module)
        cuModuleUnload(module);
}

static FRESULT createDeviceImpl(const DeviceDesc *desc, nvrhi::IDevice **out, bool validation) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (!desc || desc->deviceOrdinal < 0 || (validation && !desc->enableOptiX))
        return FE_INVALID_ARGS;
#if !NVRHI_WITH_OPTIX
    if (validation)
        return FE_UNSUPPORTED;
#endif
    try
    {
        auto c = std::make_shared<Context>();
        c->callback = desc->errorCB;
        if (!c->check(cuInit(0), "cuInit"))
            return FE_UNSUPPORTED;
        int count = 0;
        if (!c->check(cuDeviceGetCount(&count), "cuDeviceGetCount"))
            return FE_GENERIC_ERROR;
        if (desc->deviceOrdinal >= count)
            return FE_INVALID_ARGS;
        if (!c->check(cuDeviceGet(&c->device, desc->deviceOrdinal), "cuDeviceGet") ||
            !c->check(cuDevicePrimaryCtxRetain(&c->handle, c->device), "cuDevicePrimaryCtxRetain"))
            return FE_GENERIC_ERROR;
        Scope scope(c->handle);
        for (auto &s : c->streams)
            if (!c->check(cuStreamCreate(&s, CU_STREAM_NON_BLOCKING), "cuStreamCreate"))
                return FE_GENERIC_ERROR;
#if NVRHI_WITH_OPTIX
        if (desc->enableOptiX)
        {
            if (initializeOptix() != OPTIX_SUCCESS)
            {
                c->error("OptiX initialization failed");
                return FE_UNSUPPORTED;
            }
            OptixDeviceContextOptions options{};
            options.logCallbackFunction = [](unsigned int level, const char *tag, const char *message, void *p) {
                auto context = static_cast<Context *>(p);
                if (!context->callback)
                    return;
                char text[8192];
                std::snprintf(text, sizeof(text), "OptiX [%s]: %s", tag ? tag : "", message ? message : "");
                context->callback->message(level <= 2   ? MessageSeverity::Error
                                           : level == 3 ? MessageSeverity::Warning
                                                        : MessageSeverity::Info,
                                           text);
            };
            options.logCallbackData = c.get();
            options.logCallbackLevel = validation ? 4 : 2;
            options.validationMode =
                validation ? OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_ALL : OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_OFF;
            const auto result = optixDeviceContextCreate(c->handle, &options, &c->optix);
            if (result != OPTIX_SUCCESS)
            {
                char message[256];
                std::snprintf(message, sizeof(message), "optixDeviceContextCreate: %s", optixGetErrorName(result));
                c->error(message);
                return FE_GENERIC_ERROR;
            }
        }
#else
        if (desc->enableOptiX)
            c->error("NVRHI was built without OptiX; compute remains available");
#endif
        *out = MAKE_RC_OBJ(Device, c);
        return *out ? FS_OK : FE_OUT_OF_MEMORY;
    }
    catch (const std::bad_alloc &)
    {
        return FE_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return FE_GENERIC_ERROR;
    }
}
FRESULT nvrhiCudaCreateDevice(const DeviceDesc *desc, nvrhi::IDevice **out) noexcept
{
    return createDeviceImpl(desc, out, false);
}
FRESULT nvrhiCudaCreateDeviceWithValidation(const DeviceDesc *desc, nvrhi::IDevice **out) noexcept
{
    return createDeviceImpl(desc, out, true);
}
Device::~Device()
{
    waitForIdle();
}
GraphicsAPI Device::getGraphicsAPI() noexcept
{
    return GraphicsAPI::CUDA;
}
IMessageCallback *Device::getMessageCallback() noexcept
{
    return context->callback;
}
NativeObject Device::getNativeObject(ObjectType type) noexcept
{
    return type == ObjectTypes::CUDA_Context ? context->handle : nullptr;
}
NativeObject Device::getNativeQueue(ObjectType type, CommandQueue queue) noexcept
{
    return type == ObjectTypes::CUDA_Stream && unsigned(queue) < 3 ? context->streams[unsigned(queue)] : nullptr;
}
FRESULT Device::createBuffer(const BufferDesc &d, IBuffer **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (!d.byteSize)
        return FE_INVALID_ARGS;
    if (d.isVirtual || d.sharedResourceFlags != SharedResourceFlags::None)
        return FE_UNSUPPORTED;
    try
    {
        Scope scope(context->handle);
        auto b = TakeOver(MAKE_RC_OBJ(Buffer, context, d));
        b->memory = allocate(context, size_t(d.byteSize));
        if (!b->memory)
            return FE_OUT_OF_MEMORY;
        *out = b.Detach();
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
FRESULT Device::createHandleForNativeBuffer(ObjectType type, NativeObject ptr, const BufferDesc &d,
                                            IBuffer **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (type != ObjectTypes::CUDA_DevicePointer)
        return FE_UNSUPPORTED;
    if (!ptr || !d.byteSize)
        return FE_INVALID_ARGS;
    try
    {
        auto b = TakeOver(MAKE_RC_OBJ(Buffer, context, d));
        b->memory = std::make_shared<Allocation>();
        b->memory->context = context;
        b->memory->ptr = reinterpret_cast<CUdeviceptr>(ptr);
        b->memory->owned = false;
        *out = b.Detach();
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
void *Device::mapBuffer(IBuffer *buffer, CpuAccessMode mode) noexcept
{
    auto b = static_cast<Buffer *>(buffer);
    if (!b || mode == CpuAccessMode::None || b->desc.cpuAccess != mode || b->mapMode != CpuAccessMode::None)
        return nullptr;
    if (!waitForIdle())
        return nullptr;
    try
    {
        b->mapped.resize(size_t(b->desc.byteSize));
    }
    catch (...)
    {
        return nullptr;
    }
    Scope scope(context->handle);
    if (mode == CpuAccessMode::Read &&
        !context->check(cuMemcpyDtoH(b->mapped.data(), b->memory->ptr, b->mapped.size()), "cuMemcpyDtoH"))
        return nullptr;
    b->mapMode = mode;
    return b->mapped.data();
}
void Device::unmapBuffer(IBuffer *buffer) noexcept
{
    auto b = static_cast<Buffer *>(buffer);
    if (!b || b->mapMode == CpuAccessMode::None)
        return;
    Scope scope(context->handle);
    if (b->mapMode == CpuAccessMode::Write)
        context->check(cuMemcpyHtoD(b->memory->ptr, b->mapped.data(), b->mapped.size()), "cuMemcpyHtoD");
    b->mapMode = CpuAccessMode::None;
}
MemoryRequirements &Device::getBufferMemoryRequirements(MemoryRequirements &out, IBuffer *b) noexcept
{
    if (b)
        b->queryMemoryRequirements(out);
    return out;
}
MemoryRequirements &Device::getTextureMemoryRequirements(MemoryRequirements &out, ITexture *t) noexcept
{
    if (t)
        t->queryMemoryRequirements(out);
    return out;
}

bool arrayFormat(Format format, CUarray_format &f, unsigned &channels)
{
    // Packed and BGR formats need explicit conversion, never reinterpret them as RGBA.
    if (format == Format::BGRA8_UNORM || format == Format::BGRX8_UNORM || format == Format::R10G10B10A2_UNORM)
        return false;
    const auto &info = getFormatInfo(format);
    channels = unsigned(info.hasRed) + unsigned(info.hasGreen) + unsigned(info.hasBlue) + unsigned(info.hasAlpha);
    if (info.kind == FormatKind::DepthStencil || info.blockSize != 1 || channels == 0 || channels == 3 || info.isSRGB)
        return false;
    const auto bits = 8 * info.bytesPerBlock / channels;
    if (info.kind == FormatKind::Float)
    {
        if (bits == 16)
            f = CU_AD_FORMAT_HALF;
        else if (bits == 32)
            f = CU_AD_FORMAT_FLOAT;
        else
            return false;
        return true;
    }
    if (bits == 8)
        f = info.isSigned ? CU_AD_FORMAT_SIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT8;
    else if (bits == 16)
        f = info.isSigned ? CU_AD_FORMAT_SIGNED_INT16 : CU_AD_FORMAT_UNSIGNED_INT16;
    else if (bits == 32)
        f = info.isSigned ? CU_AD_FORMAT_SIGNED_INT32 : CU_AD_FORMAT_UNSIGNED_INT32;
    else
        return false;
    return true;
}
FormatSupport Device::queryFormatSupport(Format f) noexcept
{
    CUarray_format format{};
    unsigned n = 0;
    if (!arrayFormat(f, format, n))
        return FormatSupport::None;
    auto support = FormatSupport::Texture | FormatSupport::ShaderLoad | FormatSupport::ShaderSample;
    if (getFormatInfo(f).kind != FormatKind::Normalized)
        support = support | FormatSupport::ShaderUavLoad | FormatSupport::ShaderUavStore;
    return support;
}
FRESULT Device::createTexture(const TextureDesc &d, ITexture **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (!d.width || !d.height || !d.depth || !d.mipLevels || !d.arraySize)
        return FE_INVALID_ARGS;
    if (d.isVirtual || d.isTiled || d.isTypeless || d.sampleCount != 1 ||
        d.sharedResourceFlags != SharedResourceFlags::None ||
        (d.isUAV && getFormatInfo(d.format).kind == FormatKind::Normalized))
        return FE_UNSUPPORTED;
    if (d.dimension == TextureDimension::Unknown || d.dimension == TextureDimension::Texture2DMS ||
        d.dimension == TextureDimension::Texture2DMSArray)
        return FE_UNSUPPORTED;
    CUDA_ARRAY3D_DESCRIPTOR a{};
    if (!arrayFormat(d.format, a.Format, a.NumChannels))
        return FE_UNSUPPORTED;
    a.Width = d.width;
    a.Height = d.height;
    a.Depth = d.dimension == TextureDimension::Texture3D ? d.depth : 0;
    if (d.dimension == TextureDimension::Texture1D)
        a.Height = 0;
    if (d.dimension == TextureDimension::Texture1DArray)
    {
        a.Height = 0;
        a.Depth = d.arraySize;
        a.Flags |= CUDA_ARRAY3D_LAYERED;
    }
    if (d.dimension == TextureDimension::Texture2DArray)
    {
        a.Depth = d.arraySize;
        a.Flags |= CUDA_ARRAY3D_LAYERED;
    }
    if (d.dimension == TextureDimension::TextureCube || d.dimension == TextureDimension::TextureCubeArray)
    {
        a.Depth = d.arraySize;
        a.Flags |= CUDA_ARRAY3D_CUBEMAP;
        if (d.arraySize > 6)
            a.Flags |= CUDA_ARRAY3D_LAYERED;
    }
    if (d.isUAV)
        a.Flags |= CUDA_ARRAY3D_SURFACE_LDST;
    try
    {
        Scope scope(context->handle);
        auto t = TakeOver(MAKE_RC_OBJ(Texture, context, d));
        if (!context->check(cuMipmappedArrayCreate(&t->mipArray, &a, d.mipLevels), "cuMipmappedArrayCreate"))
            return FE_GENERIC_ERROR;
        *out = t.Detach();
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
FRESULT Device::createHandleForNativeTexture(ObjectType type, NativeObject ptr, const TextureDesc &d,
                                             ITexture **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (type != ObjectTypes::CUDA_Array)
        return FE_UNSUPPORTED;
    if (!ptr || d.mipLevels != 1)
        return FE_INVALID_ARGS;
    try
    {
        auto t = TakeOver(MAKE_RC_OBJ(Texture, context, d));
        t->externalArray = static_cast<CUarray>(ptr);
        t->owned = false;
        *out = t.Detach();
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
FRESULT Device::createStagingTexture(const TextureDesc &d, CpuAccessMode access, IStagingTexture **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    CUarray_format format{};
    unsigned channels = 0;
    if (!d.width || !d.height || !d.depth)
        return FE_INVALID_ARGS;
    if (d.mipLevels != 1 || d.arraySize != 1 || access == CpuAccessMode::None ||
        !arrayFormat(d.format, format, channels))
        return FE_UNSUPPORTED;
    try
    {
        auto t = TakeOver(MAKE_RC_OBJ(StagingTexture, d, access));
        t->data.resize(size_t(d.width) * d.height * d.depth * getFormatInfo(d.format).bytesPerBlock);
        *out = t.Detach();
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
void *Device::mapStagingTexture(IStagingTexture *texture, const TextureSlice &slice, CpuAccessMode mode,
                                size_t &pitch) noexcept
{
    auto t = static_cast<StagingTexture *>(texture);
    pitch = 0;
    if (!t || mode != t->access || slice.mipLevel || slice.arraySlice)
        return nullptr;
    if (!waitForIdle())
        return nullptr;
    const auto region = slice.resolve(t->desc);
    if (region.x >= t->desc.width || region.y >= t->desc.height || region.z >= t->desc.depth ||
        region.width > t->desc.width - region.x || region.height > t->desc.height - region.y ||
        region.depth > t->desc.depth - region.z)
        return nullptr;
    const size_t pixelSize = getFormatInfo(t->desc.format).bytesPerBlock;
    pitch = size_t(t->desc.width) * pixelSize;
    return t->data.data() + (size_t(region.z) * t->desc.height + region.y) * pitch + size_t(region.x) * pixelSize;
}
void Device::unmapStagingTexture(IStagingTexture *) noexcept
{
}
FRESULT Device::createShader(const ShaderDesc &d, const void *binary, size_t size, IShader **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (!binary || !size)
        return FE_INVALID_ARGS;
    try
    {
        Scope scope(context->handle);
        auto s = TakeOver(MAKE_RC_OBJ(Shader, context, d));
        s->bytes.assign(static_cast<const uint8_t *>(binary), static_cast<const uint8_t *>(binary) + size);
        if (ShaderToolArtifactOpen(s->bytes.data(), size, &s->artifact) != 0)
        {
            context->error("CUDA shaders require NSTA artifacts with exact parameter reflection");
            return FE_INVALID_ARGS;
        }
        ShaderToolArtifactInfo info{};
        ShaderToolArtifactGetInfo(&s->artifact, &info);
        if (info.Target != ST_TARGET_PTX && info.Target != ST_TARGET_OPTIX_PTX)
            return FE_INVALID_ARGS;
        bool found = false;
        for (uint32_t i = 0; i < info.EntryCount; ++i)
        {
            ShaderToolArtifactGetEntry(&s->artifact, i, &s->entry);
            if ((std::strcmp(s->entry.Name, d.entryName.c_str()) == 0 ||
                 std::strcmp(s->entry.Symbol, d.entryName.c_str()) == 0))
            {
                found = true;
                break;
            }
        }
        if (!found)
            return FE_INVALID_ARGS;
        if (info.Target == ST_TARGET_PTX)
        {
            if (d.shaderType != ShaderType::Compute)
                return FE_UNSUPPORTED;
            std::string ptx(static_cast<const char *>(s->entry.Code), s->entry.CodeSize);
            if (!context->check(cuModuleLoadDataEx(&s->module, ptx.c_str(), 0, nullptr, nullptr),
                                "cuModuleLoadDataEx") ||
                !context->check(cuModuleGetFunction(&s->function, s->module, s->entry.Symbol), "cuModuleGetFunction"))
                return FE_GENERIC_ERROR;
        }
        *out = s.Detach();
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
FRESULT Device::createSampler(const SamplerDesc &d, ISampler **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (d.reductionType != SamplerReductionType::Standard || d.minFilter != d.magFilter ||
        d.addressU == SamplerAddressMode::MirrorOnce || d.addressV == SamplerAddressMode::MirrorOnce ||
        d.addressW == SamplerAddressMode::MirrorOnce)
        return FE_UNSUPPORTED;
    try
    {
        *out = MAKE_RC_OBJ(Sampler, d);
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
FRESULT Device::createBindingLayout(const BindingLayoutDesc &d, IBindingLayout **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    try
    {
        *out = MAKE_RC_OBJ(BindingLayout, d);
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
FRESULT Device::createBindingSet(const BindingSetDesc &d, IBindingLayout *layout, IBindingSet **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (!layout || !layout->getDesc())
        return FE_INVALID_ARGS;
    for (auto &b : d.bindings)
    {
        bool found = false;
        for (auto &l : layout->getDesc()->bindings)
            if (l.slot == b.slot && l.type == b.type && b.arrayElement < l.getArraySize())
                found = true;
        if (!found)
            return FE_INVALID_ARGS;
    }
    try
    {
        *out = MAKE_RC_OBJ(BindingSet, d, layout);
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
FRESULT Device::createComputePipeline(const ComputePipelineDesc &d, IComputePipeline **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (!d.CS || d.CS->getDesc().shaderType != ShaderType::Compute)
        return FE_INVALID_ARGS;
    try
    {
        *out = MAKE_RC_OBJ(ComputePipeline, d);
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
bool Device::queryFeatureSupport(Feature feature, void *info, size_t size) noexcept
{
    if (feature == Feature::ComputeQueue || feature == Feature::CopyQueue || feature == Feature::DeferredCommandLists ||
        feature == Feature::ConstantBufferRanges)
        return true;
    if (feature == Feature::WaveLaneCountMinMax)
    {
        if (info && size >= sizeof(WaveLaneCountMinMaxFeatureInfo))
        {
            auto p = static_cast<WaveLaneCountMinMaxFeatureInfo *>(info);
            p->minWaveLaneCount = p->maxWaveLaneCount = 32;
        }
        return true;
    }
#if NVRHI_WITH_OPTIX
    if (feature == Feature::RayTracingPipeline || feature == Feature::RayTracingAccelStruct)
        return context->optix != nullptr;
#endif
    return false;
}
} // namespace nvrhi::cuda
