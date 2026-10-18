// SPDX-License-Identifier: MIT
#include "cuda-backend.h"

namespace nvrhi::cuda
{
static ResourceType resourceType(uint32_t kind)
{
    switch (kind)
    {
    case ST_BINDING_CONSTANT_BUFFER:
        return ResourceType::ConstantBuffer;
    case ST_BINDING_STRUCTURED_BUFFER:
        return ResourceType::StructuredBuffer_SRV;
    case ST_BINDING_RW_STRUCTURED_BUFFER:
        return ResourceType::StructuredBuffer_UAV;
    case ST_BINDING_BYTE_ADDRESS_BUFFER:
        return ResourceType::RawBuffer_SRV;
    case ST_BINDING_RW_BYTE_ADDRESS_BUFFER:
        return ResourceType::RawBuffer_UAV;
    case ST_BINDING_TEXTURE:
        return ResourceType::Texture_SRV;
    case ST_BINDING_RW_TEXTURE:
        return ResourceType::Texture_UAV;
    case ST_BINDING_SAMPLER:
        return ResourceType::Sampler;
    case ST_BINDING_ACCELERATION_STRUCTURE:
        return ResourceType::RayTracingAccelStruct;
    case ST_BINDING_TYPED_BUFFER:
        return ResourceType::TypedBuffer_SRV;
    case ST_BINDING_RW_TYPED_BUFFER:
        return ResourceType::TypedBuffer_UAV;
    default:
        return ResourceType::None;
    }
}
static const BindingSetItem *findBinding(const BindingSetVector &sets, uint32_t space, uint32_t slot, ResourceType type,
                                         uint32_t element = 0)
{
    for (auto set : sets)
    {
        if (!set || !set->getLayout() || !set->getLayout()->getDesc())
            continue;
        if (set->getLayout()->getDesc()->registerSpace != space)
            continue;
        for (auto &item : set->getDesc()->bindings)
            if (item.slot == slot && item.arrayElement == element &&
                (item.type == type ||
                 (type == ResourceType::ConstantBuffer && item.type == ResourceType::VolatileConstantBuffer)))
                return &item;
    }
    return nullptr;
}
static CUaddress_mode addressMode(SamplerAddressMode mode)
{
    switch (mode)
    {
    case SamplerAddressMode::Wrap:
        return CU_TR_ADDRESS_MODE_WRAP;
    case SamplerAddressMode::Mirror:
        return CU_TR_ADDRESS_MODE_MIRROR;
    case SamplerAddressMode::Border:
        return CU_TR_ADDRESS_MODE_BORDER;
    default:
        return CU_TR_ADDRESS_MODE_CLAMP;
    }
}
static bool textureObject(Replay &replay, const BindingSetItem &item, const SamplerDesc *sampler, uint64_t &value,
                          bool writable)
{
    auto texture = static_cast<Texture *>(item.resourceHandle);
    if (!texture || texture->context != replay.context)
        return false;
    if (item.dimension != TextureDimension::Unknown && item.dimension != texture->desc.dimension)
    {
        replay.context->error("CUDA texture view dimension reinterpretation is unsupported");
        return false;
    }
    if (item.format != Format::UNKNOWN && item.format != texture->desc.format)
    {
        replay.context->error("CUDA texture format reinterpretation is unsupported");
        return false;
    }
    if (!resolveComponentMapping(item.overrideComponentMapping, texture->desc.defaultComponentMapping).isIdentity())
    {
        replay.context->error("CUDA texture swizzle is unsupported");
        return false;
    }
    const auto subresources = item.subresources.resolve(texture->desc, writable);
    if (!subresources.numMipLevels || !subresources.numArraySlices ||
        subresources.baseMipLevel >= texture->desc.mipLevels || subresources.baseArraySlice >= texture->desc.arraySize)
        return false;
    const bool sliced = subresources.baseArraySlice != 0 || subresources.numArraySlices != texture->desc.arraySize;
    if (writable && sliced)
    {
        replay.context->error("CUDA surface objects cannot represent partial array-slice views");
        return false;
    }
    CUDA_RESOURCE_DESC resource{};
    if (writable || subresources.numMipLevels == 1 || !texture->mipArray)
    {
        resource.resType = CU_RESOURCE_TYPE_ARRAY;
        resource.res.array.hArray = texture->level(subresources.baseMipLevel);
    }
    else
    {
        resource.resType = CU_RESOURCE_TYPE_MIPMAPPED_ARRAY;
        resource.res.mipmap.hMipmappedArray = texture->mipArray;
    }
    if (writable)
    {
        if (getFormatInfo(texture->desc.format).kind == FormatKind::Normalized)
        {
            replay.context->error("CUDA normalized texture surface stores require explicit shader conversion");
            return false;
        }
        CUsurfObject surface = 0;
        if (!replay.context->check(cuSurfObjectCreate(&surface, &resource), "cuSurfObjectCreate"))
            return false;
        replay.surfaces.push_back(surface);
        value = surface;
        return true;
    }
    CUDA_TEXTURE_DESC descriptor{};
    descriptor.addressMode[0] = descriptor.addressMode[1] = descriptor.addressMode[2] = CU_TR_ADDRESS_MODE_CLAMP;
    descriptor.filterMode = CU_TR_FILTER_MODE_POINT;
    descriptor.maxMipmapLevelClamp = float(subresources.numMipLevels - 1);
    if (getFormatInfo(texture->desc.format).kind == FormatKind::Integer)
        descriptor.flags |= CU_TRSF_READ_AS_INTEGER;
    if (sampler)
    {
        descriptor.flags |= CU_TRSF_NORMALIZED_COORDINATES;
        descriptor.addressMode[0] = addressMode(sampler->addressU);
        descriptor.addressMode[1] = addressMode(sampler->addressV);
        descriptor.addressMode[2] = addressMode(sampler->addressW);
        descriptor.filterMode = sampler->minFilter ? CU_TR_FILTER_MODE_LINEAR : CU_TR_FILTER_MODE_POINT;
        descriptor.mipmapFilterMode = sampler->mipFilter ? CU_TR_FILTER_MODE_LINEAR : CU_TR_FILTER_MODE_POINT;
        descriptor.mipmapLevelBias = sampler->mipBias;
        descriptor.maxAnisotropy = unsigned(sampler->maxAnisotropy);
        std::memcpy(descriptor.borderColor, &sampler->borderColor, sizeof(descriptor.borderColor));
    }
    CUDA_RESOURCE_VIEW_DESC view{};
    CUDA_RESOURCE_VIEW_DESC *viewPtr = nullptr;
    if (sliced || (resource.resType == CU_RESOURCE_TYPE_MIPMAPPED_ARRAY &&
                   (subresources.baseMipLevel != 0 || subresources.numMipLevels != texture->desc.mipLevels)))
    {
        CUDA_ARRAY3D_DESCRIPTOR native{};
        if (!replay.context->check(
                cuArray3DGetDescriptor(
                    &native,
                    texture->level(resource.resType == CU_RESOURCE_TYPE_ARRAY ? subresources.baseMipLevel : 0)),
                "cuArray3DGetDescriptor(texture view)"))
            return false;
        CUresourceViewFormat firstFormat;
        switch (native.Format)
        {
        case CU_AD_FORMAT_UNSIGNED_INT8:
            firstFormat = CU_RES_VIEW_FORMAT_UINT_1X8;
            break;
        case CU_AD_FORMAT_SIGNED_INT8:
            firstFormat = CU_RES_VIEW_FORMAT_SINT_1X8;
            break;
        case CU_AD_FORMAT_UNSIGNED_INT16:
            firstFormat = CU_RES_VIEW_FORMAT_UINT_1X16;
            break;
        case CU_AD_FORMAT_SIGNED_INT16:
            firstFormat = CU_RES_VIEW_FORMAT_SINT_1X16;
            break;
        case CU_AD_FORMAT_UNSIGNED_INT32:
            firstFormat = CU_RES_VIEW_FORMAT_UINT_1X32;
            break;
        case CU_AD_FORMAT_SIGNED_INT32:
            firstFormat = CU_RES_VIEW_FORMAT_SINT_1X32;
            break;
        case CU_AD_FORMAT_HALF:
            firstFormat = CU_RES_VIEW_FORMAT_FLOAT_1X16;
            break;
        case CU_AD_FORMAT_FLOAT:
            firstFormat = CU_RES_VIEW_FORMAT_FLOAT_1X32;
            break;
        default:
            return false;
        }
        view.format = static_cast<CUresourceViewFormat>(unsigned(firstFormat) +
                                                        (native.NumChannels == 4 ? 2 : native.NumChannels - 1));
        view.width = native.Width;
        view.height = native.Height;
        view.depth = native.Depth;
        view.firstLayer = subresources.baseArraySlice;
        view.lastLayer = subresources.baseArraySlice + subresources.numArraySlices - 1;
        if (resource.resType == CU_RESOURCE_TYPE_MIPMAPPED_ARRAY)
        {
            view.firstMipmapLevel = subresources.baseMipLevel;
            view.lastMipmapLevel = subresources.baseMipLevel + subresources.numMipLevels - 1;
        }
        viewPtr = &view;
    }
    CUtexObject object = 0;
    if (!replay.context->check(cuTexObjectCreate(&object, &resource, &descriptor, viewPtr), "cuTexObjectCreate"))
        return false;
    replay.textures.push_back(object);
    value = object;
    return true;
}
static bool packParametersInternal(Replay &replay, Shader *shader, const BindingSetVector &sets,
                                   const std::vector<uint8_t> &push, std::vector<uint8_t> &globals,
                                   std::vector<uint8_t> &entry, bool shaderRecord)
{
    globals.resize(shaderRecord ? 0 : shader->entry.GlobalParamsSize);
    const bool rayShader = (shader->desc.shaderType & ShaderType::AllRayTracing) != ShaderType::None;
    entry.resize(shaderRecord || !rayShader ? shader->entry.EntryParamsSize : 0);
    for (uint32_t i = 0; i < shader->entry.BindingCount; ++i)
    {
        ShaderToolArtifactBinding b{};
        if (ShaderToolArtifactGetBinding(&shader->artifact, shader->entry.BindingFirst + i, &b) != 0)
            return false;
        if (b.Scope == ST_SCOPE_CONSTANT_BUFFER || b.Kind == ST_BINDING_SAMPLER ||
            (b.Scope == ST_SCOPE_SHADER_RECORD) != shaderRecord)
            continue;
        auto &block = b.Scope == ST_SCOPE_ENTRY || b.Scope == ST_SCOPE_SHADER_RECORD ? entry : globals;
        if (b.Offset > block.size() || b.Size > block.size() - b.Offset)
            return false;
        if (b.Kind == ST_BINDING_UNIFORM)
        {
            if (b.Offset > push.size() || b.Size > push.size() - b.Offset)
            {
                replay.context->error("Missing CUDA uniform/push-constant data");
                return false;
            }
            std::memcpy(block.data() + b.Offset, push.data() + b.Offset, b.Size);
            continue;
        }
        const auto type = resourceType(b.Kind);
        if (type == ResourceType::None)
            return false;
        const uint32_t count = std::max(b.Count, 1u), stride = count > 1 ? b.Size / count : b.Size;
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto item = findBinding(sets, b.RegisterSpace, b.RegisterIndex, type, index);
            if (!item || !item->resourceHandle)
            {
                replay.context->error("Missing reflected CUDA resource binding");
                return false;
            }
            uint64_t values[2]{};
            switch (type)
            {
            case ResourceType::ConstantBuffer:
            case ResourceType::StructuredBuffer_SRV:
            case ResourceType::StructuredBuffer_UAV:
            case ResourceType::RawBuffer_SRV:
            case ResourceType::RawBuffer_UAV: {
                auto buffer = static_cast<Buffer *>(item->resourceHandle);
                if (buffer->context != replay.context)
                    return false;
                auto range = item->range.resolve(buffer->desc);
                values[0] = buffer->memory->ptr + range.byteOffset;
                if (type == ResourceType::ConstantBuffer)
                {
                    if (stride != 8)
                        return false;
                }
                else
                {
                    if (stride != 16)
                        return false;
                    values[1] = range.byteSize;
                    if (type == ResourceType::StructuredBuffer_SRV || type == ResourceType::StructuredBuffer_UAV)
                    {
                        if (!b.ElementStride || range.byteSize % b.ElementStride)
                            return false;
                        values[1] /= b.ElementStride;
                    }
                }
                break;
            }
            case ResourceType::Texture_SRV:
            case ResourceType::Texture_UAV: {
                const SamplerDesc *sampler = nullptr;
                if (b.SamplerIndex != SHADER_TOOL_ARTIFACT_NONE)
                {
                    auto samplerItem = findBinding(sets, b.SamplerSpace, b.SamplerIndex, ResourceType::Sampler);
                    if (!samplerItem || !samplerItem->resourceHandle)
                        return false;
                    sampler = &static_cast<ISampler *>(samplerItem->resourceHandle)->getDesc();
                }
                if (stride != 8 || !textureObject(replay, *item, sampler, values[0], type == ResourceType::Texture_UAV))
                    return false;
                break;
            }
            case ResourceType::RayTracingAccelStruct:
                if (stride != 8)
                    return false;
                values[0] = static_cast<rt::IAccelStruct *>(item->resourceHandle)->getDeviceAddress();
                break;
            default:
                replay.context->error("Unsupported reflected CUDA binding kind");
                return false;
            }
            if (stride > sizeof(values))
                return false;
            std::memcpy(block.data() + b.Offset + index * stride, values, stride);
        }
    }
    return true;
}
bool packParameters(Replay &replay, Shader *shader, const BindingSetVector &sets, const std::vector<uint8_t> &push,
                    std::vector<uint8_t> &globals, std::vector<uint8_t> &entry)
{
    return packParametersInternal(replay, shader, sets, push, globals, entry, false);
}
bool packShaderRecord(Replay &replay, Shader *shader, IBindingSet *set, std::vector<uint8_t> &payload)
{
    BindingSetVector sets;
    if (set)
        sets.push_back(set);
    std::vector<uint8_t> globals, push;
    return packParametersInternal(replay, shader, sets, push, globals, payload, true);
}
static bool launch(Replay &replay, const ComputeState &state, const std::vector<uint8_t> &push, uint32_t x, uint32_t y,
                   uint32_t z)
{
    if (!x || !y || !z)
        return true;
    auto pipeline = static_cast<ComputePipeline *>(state.pipeline);
    auto shader = static_cast<Shader *>(pipeline->desc.CS.Get());
    std::vector<uint8_t> globals, entry;
    if (!packParameters(replay, shader, state.bindings, push, globals, entry))
        return false;
    const auto &block = shader->entry.ThreadGroupSize;
    const bool hasGlobals = !globals.empty();
    if (hasGlobals)
    {
        if (!shader->entry.GlobalParamsSymbol || !shader->entry.GlobalParamsSymbol[0])
        {
            replay.context->error("CUDA artifact has no global parameter symbol");
            return false;
        }
        // Constant memory belongs to a module, so serialize its updates across streams.
        if (!shader->parameterFence &&
            !replay.context->check(cuEventCreate(&shader->parameterFence, CU_EVENT_DISABLE_TIMING),
                                   "cuEventCreate(parameters)"))
            return false;
        if (shader->parameterFenceRecorded &&
            !replay.context->check(cuStreamWaitEvent(replay.stream, shader->parameterFence, 0),
                                   "cuStreamWaitEvent(parameters)"))
            return false;
        CUdeviceptr address = 0;
        size_t capacity = 0;
        if (!replay.context->check(
                cuModuleGetGlobal(&address, &capacity, shader->module, shader->entry.GlobalParamsSymbol),
                "cuModuleGetGlobal(parameters)") ||
            capacity < globals.size())
            return false;
        replay.hostBlocks.push_back(std::move(globals));
        auto &bytes = replay.hostBlocks.back();
        if (!replay.context->check(cuMemcpyHtoDAsync(address, bytes.data(), bytes.size(), replay.stream),
                                   "cuMemcpyHtoDAsync(constants)"))
            return false;
    }
    // Slang entry-point uniform arguments use the native packed CUDA argument ABI.
    size_t entrySize = entry.size();
    void *extra[] = {CU_LAUNCH_PARAM_BUFFER_POINTER, entry.data(), CU_LAUNCH_PARAM_BUFFER_SIZE, &entrySize,
                     CU_LAUNCH_PARAM_END};
    if (!replay.context->check(cuLaunchKernel(shader->function, x, y, z, block[0], block[1], block[2], 0, replay.stream,
                                              nullptr, entry.empty() ? nullptr : extra),
                               "cuLaunchKernel"))
        return false;
    if (hasGlobals)
    {
        shader->parameterFenceRecorded =
            replay.context->check(cuEventRecord(shader->parameterFence, replay.stream), "cuEventRecord(parameters)");
        return shader->parameterFenceRecorded;
    }
    return true;
}
void CommandList::dispatch(uint32_t x, uint32_t y, uint32_t z) noexcept
{
    if (!computeState.pipeline)
    {
        valid = false;
        context->error("CUDA dispatch requires a compute pipeline");
        return;
    }
    try
    {
        auto state = computeState;
        auto refs = stateRefs;
        auto push = pushConstants;
        record([state, refs = std::move(refs), push = std::move(push), x, y, z](Replay &r) {
            return launch(r, state, push, x, y, z);
        });
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::dispatchIndirect(uint32_t offset) noexcept
{
    if (!computeState.pipeline || !computeState.indirectParams ||
        uint64_t(offset) + sizeof(DispatchIndirectArguments) > computeState.indirectParams->getDesc().byteSize)
    {
        valid = false;
        return;
    }
    try
    {
        auto state = computeState;
        auto refs = stateRefs;
        auto push = pushConstants;
        record([state, refs = std::move(refs), push = std::move(push), offset](Replay &r) {
            DispatchIndirectArguments args{};
            auto b = static_cast<Buffer *>(state.indirectParams);
            if (!r.context->check(cuStreamSynchronize(r.stream), "cuStreamSynchronize(indirect)") ||
                !r.context->check(cuMemcpyDtoH(&args, b->memory->ptr + offset, sizeof(args)), "cuMemcpyDtoH(indirect)"))
                return false;
            return launch(r, state, push, args.groupsX, args.groupsY, args.groupsZ);
        });
    }
    catch (...)
    {
        valid = false;
    }
}
} // namespace nvrhi::cuda
