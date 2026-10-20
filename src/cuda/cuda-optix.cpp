// SPDX-License-Identifier: MIT
#include "cuda-backend.h"
#if NVRHI_WITH_OPTIX
#include <map>
#include <optix_function_table_definition.h>
#include <optix_stack_size.h>
#include <optix_stubs.h>
#endif

namespace nvrhi::cuda
{
bool packParameters(Replay &, Shader *, const BindingSetVector &, const std::vector<uint8_t> &, std::vector<uint8_t> &,
                    std::vector<uint8_t> &);
bool packShaderRecord(Replay &, Shader *, IBindingSet *, std::vector<uint8_t> &);
#if NVRHI_WITH_OPTIX
OptixResult initializeOptix()
{
    static std::once_flag once;
    static OptixResult result = OPTIX_ERROR_UNKNOWN;
    std::call_once(once, [] { result = optixInit(); });
    return result;
}
static bool checkOptix(ContextPtr c, OptixResult result, const char *operation)
{
    if (result == OPTIX_SUCCESS)
        return true;
    std::string message = std::string(operation) + ": " + optixGetErrorName(result);
    c->error(message.c_str());
    return false;
}
NVRHI_CLASS_CLSID(AccelStruct, "5490c020-09e1-40ad-a1ab-701827580020")
class AccelStruct : public ObjectImpl<rt::IAccelStruct>
{
  public:
    CUDA_INTERFACES(AccelStruct, rt::IAccelStruct)
    ContextPtr context;
    rt::AccelStructDesc desc;
    AllocationPtr memory;
    OptixTraversableHandle handle = 0;
    OptixAccelBufferSizes sizes{};
    bool built = false, compacted = false;
    std::vector<RHIObjectHandle> resources;
    AccelStruct(ContextPtr c, const rt::AccelStructDesc &d) : context(c), desc(d)
    {
    }
    const rt::AccelStructDesc &getDesc() const noexcept override
    {
        return desc;
    }
    bool isCompacted() const noexcept override
    {
        return compacted;
    }
    uint64_t getDeviceAddress() const noexcept override
    {
        return handle;
    }
    bool queryMemoryRequirements(MemoryRequirements &out) noexcept override
    {
        out.size = sizes.outputSizeInBytes;
        out.alignment = OPTIX_ACCEL_BUFFER_BYTE_ALIGNMENT;
        return true;
    }
};
struct Group
{
    OptixProgramGroup handle = nullptr;
    ShaderHandle shader;
    ShaderHandle recordShader;
    BindingLayoutHandle bindingLayout;
    OptixProgramGroupKind kind{};
};
NVRHI_CLASS_CLSID(RayPipeline, "5490c021-09e1-40ad-a1ab-701827580021")
class RayPipeline : public ObjectImpl<rt::IPipeline>
{
  public:
    CUDA_INTERFACES(RayPipeline, rt::IPipeline)
    ContextPtr context;
    rt::PipelineDesc desc;
    OptixPipeline pipeline = nullptr;
    OptixProgramGroup emptyMiss = nullptr;
    std::vector<OptixModule> modules;
    std::map<std::string, Group> groups;
    ShaderHandle parameterShader;
    RayPipeline(ContextPtr c, const rt::PipelineDesc &d) : context(c), desc(d)
    {
    }
    ~RayPipeline()
    {
        Scope scope(context->handle);
        if (pipeline)
            optixPipelineDestroy(pipeline);
        if (emptyMiss)
            optixProgramGroupDestroy(emptyMiss);
        for (auto &g : groups)
            if (g.second.handle)
                optixProgramGroupDestroy(g.second.handle);
        for (auto m : modules)
            optixModuleDestroy(m);
    }
    const rt::PipelineDesc &getDesc() const noexcept override
    {
        return desc;
    }
    FRESULT createShaderTable(const rt::ShaderTableDesc &d, rt::IShaderTable **out) noexcept override;
};
struct TableEntry
{
    std::string name;
    BindingSetHandle binding;
};
NVRHI_CLASS_CLSID(ShaderTable, "5490c022-09e1-40ad-a1ab-701827580022")
class ShaderTable : public ObjectImpl<rt::IShaderTable>
{
  public:
    CUDA_INTERFACES(ShaderTable, rt::IShaderTable)
    AutoPtr<RayPipeline> pipeline;
    rt::ShaderTableDesc desc;
    TableEntry raygen;
    std::vector<TableEntry> miss, hit, callable;
    ShaderTable(RayPipeline *p, const rt::ShaderTableDesc &d) : pipeline(p), desc(d)
    {
    }
    const rt::ShaderTableDesc &getDesc() const noexcept override
    {
        return desc;
    }
    uint32_t getNumEntries() const noexcept override
    {
        return uint32_t(!raygen.name.empty()) + uint32_t(miss.size() + hit.size() + callable.size());
    }
    rt::IPipeline *getPipeline() const noexcept override
    {
        return pipeline;
    }
    bool valid(const char *name, OptixProgramGroupKind kind, IBindingSet *bindings)
    {
        if (!name)
            return false;
        auto i = pipeline->groups.find(name);
        if (i == pipeline->groups.end() || i->second.kind != kind)
            return false;
        if (bool(bindings) != bool(i->second.bindingLayout) ||
            (bindings && bindings->getLayout() != i->second.bindingLayout))
        {
            pipeline->context->error("OptiX shader record bindings must match the program group's local layout");
            return false;
        }
        return true;
    }
    void setRayGenerationShader(const char *name, IBindingSet *bindings = nullptr) noexcept override
    {
        try
        {
            if (valid(name, OPTIX_PROGRAM_GROUP_KIND_RAYGEN, bindings))
                raygen = {name, bindings};
        }
        catch (...)
        {
            pipeline->context->error("Shader table allocation failed");
        }
    }
    int add(std::vector<TableEntry> &list, const char *name, IBindingSet *bindings, OptixProgramGroupKind kind) noexcept
    {
        try
        {
            if (!valid(name, kind, bindings) || (desc.isCached && getNumEntries() >= desc.maxEntries))
                return -1;
            list.push_back({name, bindings});
            return int(list.size() - 1);
        }
        catch (...)
        {
            return -1;
        }
    }
    int addMissShader(const char *n, IBindingSet *b = nullptr) noexcept override
    {
        return add(miss, n, b, OPTIX_PROGRAM_GROUP_KIND_MISS);
    }
    int addHitGroup(const char *n, IBindingSet *b = nullptr) noexcept override
    {
        return add(hit, n, b, OPTIX_PROGRAM_GROUP_KIND_HITGROUP);
    }
    int addCallableShader(const char *n, IBindingSet *b = nullptr) noexcept override
    {
        return add(callable, n, b, OPTIX_PROGRAM_GROUP_KIND_CALLABLES);
    }
    void clearMissShaders() noexcept override
    {
        miss.clear();
    }
    void clearHitShaders() noexcept override
    {
        hit.clear();
    }
    void clearCallableShaders() noexcept override
    {
        callable.clear();
    }
};
FRESULT RayPipeline::createShaderTable(const rt::ShaderTableDesc &d, rt::IShaderTable **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (d.isCached && !d.maxEntries)
        return FE_INVALID_ARGS;
    try
    {
        *out = MAKE_RC_OBJ(ShaderTable, this, d);
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
FRESULT Device::createRayTracingPipeline(const rt::PipelineDesc &d, rt::IPipeline **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (!context->optix)
        return FE_UNSUPPORTED;
    if (d.allowOpacityMicromaps || d.maxPayloadSize > 32 * 4 || d.maxAttributeSize > 8 * 4)
        return FE_UNSUPPORTED;
    if (d.shaders.empty() || !d.maxRecursionDepth)
        return FE_INVALID_ARGS;
    try
    {
        Scope scope(context->handle);
        auto p = TakeOver(MAKE_RC_OBJ(RayPipeline, context, d));
        Shader *reference = nullptr;
        for (auto &s : d.shaders)
            if (s.shader)
            {
                reference = static_cast<Shader *>(s.shader.Get());
                break;
            }
        if (!reference)
            return FE_INVALID_ARGS;
        p->parameterShader = reference;
        OptixPipelineCompileOptions options{};
        options.usesMotionBlur = 0;
        options.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
        options.numPayloadValues = (d.maxPayloadSize + 3) / 4;
        options.numAttributeValues = (d.maxAttributeSize + 3) / 4;
        options.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
        options.pipelineLaunchParamsVariableName =
            reference->entry.GlobalParamsSize ? reference->entry.GlobalParamsSymbol : nullptr;
        options.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE | OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM;
        std::unordered_map<Shader *, OptixModule> cache;
        auto module = [&](IShader *iface) -> OptixModule {
            if (!iface)
                return nullptr;
            auto s = static_cast<Shader *>(iface);
            auto found = cache.find(s);
            if (found != cache.end())
                return found->second;
            ShaderToolArtifactInfo info{};
            ShaderToolArtifactGetInfo(&s->artifact, &info);
            if (info.Target != ST_TARGET_OPTIX_PTX || s->entry.GlobalParamsSize != reference->entry.GlobalParamsSize ||
                std::strcmp(s->entry.GlobalParamsSymbol, reference->entry.GlobalParamsSymbol))
            {
                context->error("OptiX pipeline shader global layouts must agree");
                return nullptr;
            }
            OptixModuleCompileOptions mo{};
            mo.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
            mo.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL;
            OptixModule m = nullptr;
            char log[8192]{};
            size_t n = sizeof(log);
            auto status = optixModuleCreate(context->optix, &mo, &options, static_cast<const char *>(s->entry.Code),
                                            s->entry.CodeSize, log, &n, &m);
            if (status != OPTIX_SUCCESS)
            {
                context->error(log);
                checkOptix(context, status, "optixModuleCreate");
                return nullptr;
            }
            p->modules.push_back(m);
            cache.emplace(s, m);
            return m;
        };
        auto addGroup = [&](const std::string &name, OptixProgramGroupDesc &gd, IShader *shader, IBindingLayout *layout,
                            const std::vector<IShader *> &recordShaders) -> bool {
            if (name.empty() || p->groups.count(name))
                return false;
            OptixProgramGroupOptions go{};
            Group group;
            group.kind = gd.kind;
            group.shader = shader;
            group.bindingLayout = layout;
            for (auto iface : recordShaders)
            {
                if (!iface)
                    continue;
                auto s = static_cast<Shader *>(iface);
                if (!s->entry.EntryParamsSize)
                    continue;
                if (!layout || s->entry.EntryParamsAlignment > OPTIX_SBT_RECORD_ALIGNMENT)
                {
                    context->error(
                        "OptiX shader record parameters require a local binding layout and supported alignment");
                    return false;
                }
                for (uint32_t i = 0; i < s->entry.BindingCount; ++i)
                {
                    ShaderToolArtifactBinding binding{};
                    if (ShaderToolArtifactGetBinding(&s->artifact, s->entry.BindingFirst + i, &binding) != 0)
                        return false;
                    if (binding.Scope == ST_SCOPE_SHADER_RECORD && binding.Kind == ST_BINDING_UNIFORM)
                    {
                        context->error("Use a uniform ConstantBuffer or resource parameter for NVRHI local bindings; "
                                       "inline shader-record uniforms have no binding-set representation");
                        return false;
                    }
                }
                if (!group.recordShader)
                    group.recordShader = s;
                else
                {
                    auto reference = static_cast<Shader *>(group.recordShader.Get());
                    // All hit-group stages see the same payload. Require identical resource layouts.
                    std::vector<ShaderToolArtifactBinding> expected, actual;
                    auto collect = [](Shader *source, std::vector<ShaderToolArtifactBinding> &bindings) {
                        for (uint32_t i = 0; i < source->entry.BindingCount; ++i)
                        {
                            ShaderToolArtifactBinding b{};
                            if (ShaderToolArtifactGetBinding(&source->artifact, source->entry.BindingFirst + i, &b) !=
                                0)
                                return false;
                            if (b.Scope == ST_SCOPE_SHADER_RECORD)
                                bindings.push_back(b);
                        }
                        return true;
                    };
                    if (s->entry.EntryParamsSize != reference->entry.EntryParamsSize || !collect(reference, expected) ||
                        !collect(s, actual) || expected.size() != actual.size())
                        return false;
                    for (size_t i = 0; i < expected.size(); ++i)
                    {
                        const auto &a = expected[i], &b = actual[i];
                        if (a.Kind != b.Kind || a.RegisterIndex != b.RegisterIndex ||
                            a.RegisterSpace != b.RegisterSpace || a.Offset != b.Offset || a.Size != b.Size ||
                            a.ElementStride != b.ElementStride || a.Count != b.Count ||
                            a.SamplerIndex != b.SamplerIndex || a.SamplerSpace != b.SamplerSpace)
                        {
                            context->error("OptiX hit-group stages have incompatible shader record layouts");
                            return false;
                        }
                    }
                }
            }
            if (layout && !group.recordShader)
            {
                context->error("OptiX local binding layout has no shader record parameters");
                return false;
            }
            char log[8192]{};
            size_t n = sizeof(log);
            auto status = optixProgramGroupCreate(context->optix, &gd, 1, &go, log, &n, &group.handle);
            if (status != OPTIX_SUCCESS)
            {
                context->error(log);
                return false;
            }
            p->groups.emplace(name, group);
            return true;
        };
        for (auto &s : d.shaders)
        {
            if (!s.shader)
                return FE_INVALID_ARGS;
            auto shader = static_cast<Shader *>(s.shader.Get());
            OptixProgramGroupDesc gd{};
            auto m = module(shader);
            if (!m)
                return FE_GENERIC_ERROR;
            switch (shader->desc.shaderType)
            {
            case ShaderType::RayGeneration:
                gd.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
                gd.raygen.module = m;
                gd.raygen.entryFunctionName = shader->entry.Symbol;
                break;
            case ShaderType::Miss:
                gd.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
                gd.miss.module = m;
                gd.miss.entryFunctionName = shader->entry.Symbol;
                break;
            case ShaderType::Callable:
                gd.kind = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
                gd.callables.moduleCC = m;
                gd.callables.entryFunctionNameCC = shader->entry.Symbol;
                break;
            default:
                return FE_INVALID_ARGS;
            }
            if (!addGroup(s.exportName.c_str(), gd, shader, s.bindingLayout, {shader}))
                return FE_GENERIC_ERROR;
        }
        for (auto &h : d.hitGroups)
        {
            OptixProgramGroupDesc gd{};
            gd.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
            auto set = [&](IShader *iface, OptixModule &m, const char *&name) {
                if (!iface)
                    return true;
                auto s = static_cast<Shader *>(iface);
                m = module(s);
                name = s->entry.Symbol;
                return m != nullptr;
            };
            if (!set(h.closestHitShader, gd.hitgroup.moduleCH, gd.hitgroup.entryFunctionNameCH) ||
                !set(h.anyHitShader, gd.hitgroup.moduleAH, gd.hitgroup.entryFunctionNameAH) ||
                !set(h.intersectionShader, gd.hitgroup.moduleIS, gd.hitgroup.entryFunctionNameIS))
                return FE_GENERIC_ERROR;
            if (h.isProceduralPrimitive && !h.intersectionShader)
                return FE_INVALID_ARGS;
            if (!addGroup(h.exportName.c_str(), gd,
                          h.closestHitShader ? h.closestHitShader.Get() : h.intersectionShader.Get(), h.bindingLayout,
                          {h.closestHitShader, h.anyHitShader, h.intersectionShader}))
                return FE_GENERIC_ERROR;
        }
        std::vector<OptixProgramGroup> groups;
        for (auto &g : p->groups)
            groups.push_back(g.second.handle);
        // Some drivers require a miss SBT record even for raygen-only launches.
        // OptiX supports a no-op miss group with a null module and entry point.
        // Keep it private so it does not consume a public shader-table entry.
        OptixProgramGroupDesc emptyMissDesc{};
        emptyMissDesc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        OptixProgramGroupOptions emptyMissOptions{};
        if (!checkOptix(context,
                        optixProgramGroupCreate(context->optix, &emptyMissDesc, 1, &emptyMissOptions,
                                                nullptr, nullptr, &p->emptyMiss),
                        "optixProgramGroupCreate(empty miss)"))
            return FE_GENERIC_ERROR;
        groups.push_back(p->emptyMiss);
        OptixPipelineLinkOptions link{};
        link.maxTraceDepth = d.maxRecursionDepth;
        char log[8192]{};
        size_t n = sizeof(log);
        auto status = optixPipelineCreate(context->optix, &options, &link, groups.data(), unsigned(groups.size()), log,
                                          &n, &p->pipeline);
        if (status != OPTIX_SUCCESS)
        {
            context->error(log);
            return FE_GENERIC_ERROR;
        }
        OptixStackSizes stack{};
        for (auto g : groups)
            if (!checkOptix(context, optixUtilAccumulateStackSizes(g, &stack, p->pipeline),
                            "optixUtilAccumulateStackSizes"))
                return FE_GENERIC_ERROR;
        unsigned traversal = 0, state = 0, continuation = 0;
        optixUtilComputeStackSizes(&stack, d.maxRecursionDepth, d.maxRecursionDepth, 0, &traversal, &state,
                                   &continuation);
        if (!checkOptix(context, optixPipelineSetStackSize(p->pipeline, traversal, state, continuation, 2),
                        "optixPipelineSetStackSize"))
            return FE_GENERIC_ERROR;
        *out = p.Detach();
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}

struct BuildInputs
{
    std::vector<OptixBuildInput> inputs;
    std::vector<CUdeviceptr> pointers;
    std::vector<unsigned> flags;
    bool convert(const rt::GeometryDesc *geometries, size_t count, Replay *replay = nullptr)
    {
        inputs.resize(count);
        pointers.resize(count);
        flags.resize(count);
        for (size_t i = 0; i < count; ++i)
        {
            const auto &g = geometries[i];
            auto &input = inputs[i];
            flags[i] = (g.flags & rt::GeometryFlags::Opaque) != rt::GeometryFlags::None
                           ? OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT
                           : OPTIX_GEOMETRY_FLAG_NONE;
            if ((g.flags & rt::GeometryFlags::NoDuplicateAnyHitInvocation) != rt::GeometryFlags::None)
                flags[i] |= OPTIX_GEOMETRY_FLAG_REQUIRE_SINGLE_ANYHIT_CALL;
            if (g.useTransform && g.geometryType != rt::GeometryType::Triangles)
                return false;
            if (g.geometryType == rt::GeometryType::Triangles)
            {
                const auto &t = g.geometryData.triangles;
                if (!t.vertexBuffer || t.vertexFormat != Format::RGB32_FLOAT || t.opacityMicromap)
                    return false;
                input.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
                pointers[i] = t.vertexBuffer->getGpuVirtualAddress() + t.vertexOffset;
                auto &a = input.triangleArray;
                a.vertexBuffers = &pointers[i];
                a.numVertices = t.vertexCount;
                a.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
                a.vertexStrideInBytes = t.vertexStride;
                a.flags = &flags[i];
                a.numSbtRecords = 1;
                if (g.useTransform)
                {
                    a.transformFormat = OPTIX_TRANSFORM_FORMAT_MATRIX_FLOAT12;
                    // Memory-size queries inspect only whether a pre-transform is present.
                    a.preTransform = OPTIX_GEOMETRY_TRANSFORM_BYTE_ALIGNMENT;
                    if (replay)
                    {
                        auto transform = allocate(replay->context, sizeof(g.transform));
                        if (!transform)
                            return false;
                        replay->temporaries.push_back(transform);
                        replay->hostBlocks.emplace_back(sizeof(g.transform));
                        auto &bytes = replay->hostBlocks.back();
                        std::memcpy(bytes.data(), g.transform, bytes.size());
                        if (!replay->context->check(
                                cuMemcpyHtoDAsync(transform->ptr, bytes.data(), bytes.size(), replay->stream),
                                "cuMemcpyHtoDAsync(geometry transform)"))
                            return false;
                        a.preTransform = transform->ptr;
                    }
                }
                if (t.indexBuffer)
                {
                    if (t.indexFormat != Format::R32_UINT && t.indexFormat != Format::R16_UINT)
                        return false;
                    a.indexBuffer = t.indexBuffer->getGpuVirtualAddress() + t.indexOffset;
                    a.numIndexTriplets = t.indexCount / 3;
                    a.indexFormat = t.indexFormat == Format::R32_UINT ? OPTIX_INDICES_FORMAT_UNSIGNED_INT3
                                                                      : OPTIX_INDICES_FORMAT_UNSIGNED_SHORT3;
                }
            }
            else if (g.geometryType == rt::GeometryType::AABBs)
            {
                const auto &a = g.geometryData.aabbs;
                if (!a.buffer)
                    return false;
                input.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
                pointers[i] = a.buffer->getGpuVirtualAddress() + a.offset;
                auto &c = input.customPrimitiveArray;
                c.aabbBuffers = &pointers[i];
                c.numPrimitives = a.count;
                c.strideInBytes = a.stride;
                c.flags = &flags[i];
                c.numSbtRecords = 1;
            }
            else
                return false;
        }
        return true;
    }
};
static OptixAccelBuildOptions buildOptions(rt::AccelStructBuildFlags flags, bool update = false)
{
    OptixAccelBuildOptions o{};
    o.operation = update ? OPTIX_BUILD_OPERATION_UPDATE : OPTIX_BUILD_OPERATION_BUILD;
    if ((flags & rt::AccelStructBuildFlags::AllowUpdate) != rt::AccelStructBuildFlags::None)
        o.buildFlags |= OPTIX_BUILD_FLAG_ALLOW_UPDATE;
    if ((flags & rt::AccelStructBuildFlags::AllowCompaction) != rt::AccelStructBuildFlags::None)
        o.buildFlags |= OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
    if ((flags & rt::AccelStructBuildFlags::PreferFastTrace) != rt::AccelStructBuildFlags::None)
        o.buildFlags |= OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    if ((flags & rt::AccelStructBuildFlags::PreferFastBuild) != rt::AccelStructBuildFlags::None)
        o.buildFlags |= OPTIX_BUILD_FLAG_PREFER_FAST_BUILD;
    return o;
}
FRESULT Device::createAccelStruct(const rt::AccelStructDesc &d, rt::IAccelStruct **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (!context->optix || d.isVirtual)
        return FE_UNSUPPORTED;
    try
    {
        Scope scope(context->handle);
        auto a = TakeOver(MAKE_RC_OBJ(AccelStruct, context, d));
        auto options = buildOptions(d.buildFlags);
        BuildInputs inputs;
        if (d.isTopLevel)
        {
            OptixBuildInput input{};
            input.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
            input.instanceArray.numInstances = unsigned(d.topLevelMaxInstances);
            inputs.inputs.push_back(input);
        }
        else
        {
            if (d.bottomLevelGeometries.empty() ||
                !inputs.convert(d.bottomLevelGeometries.data(), d.bottomLevelGeometries.size()))
                return FE_UNSUPPORTED;
            for (auto &g : d.bottomLevelGeometries)
            {
                if (g.geometryType == rt::GeometryType::Triangles)
                {
                    if (g.geometryData.triangles.vertexBuffer)
                        a->resources.emplace_back(g.geometryData.triangles.vertexBuffer);
                    if (g.geometryData.triangles.indexBuffer)
                        a->resources.emplace_back(g.geometryData.triangles.indexBuffer);
                }
                else if (g.geometryData.aabbs.buffer)
                    a->resources.emplace_back(g.geometryData.aabbs.buffer);
            }
        }
        if (!checkOptix(context,
                        optixAccelComputeMemoryUsage(context->optix, &options, inputs.inputs.data(),
                                                     unsigned(inputs.inputs.size()), &a->sizes),
                        "optixAccelComputeMemoryUsage"))
            return FE_GENERIC_ERROR;
        a->memory = allocate(context, a->sizes.outputSizeInBytes);
        if (!a->memory)
            return FE_OUT_OF_MEMORY;
        *out = a.Detach();
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
static bool build(Replay &replay, AccelStruct *as, const OptixBuildInput *inputs, unsigned count,
                  rt::AccelStructBuildFlags flags)
{
    const bool update = (flags & rt::AccelStructBuildFlags::PerformUpdate) != rt::AccelStructBuildFlags::None;
    if (as->compacted || (update && (!as->built || (as->desc.buildFlags & rt::AccelStructBuildFlags::AllowUpdate) ==
                                                       rt::AccelStructBuildFlags::None)))
        return false;
    auto options = buildOptions(as->desc.buildFlags, update);
    OptixAccelBufferSizes sizes{};
    if (!checkOptix(replay.context,
                    optixAccelComputeMemoryUsage(replay.context->optix, &options, inputs, count, &sizes),
                    "optixAccelComputeMemoryUsage"))
        return false;
    if (sizes.outputSizeInBytes > as->sizes.outputSizeInBytes)
        return false;
    auto scratch = allocate(replay.context, update ? sizes.tempUpdateSizeInBytes : sizes.tempSizeInBytes);
    if (!scratch)
        return false;
    replay.temporaries.push_back(scratch);
    OptixAccelEmitDesc emit{};
    AllocationPtr sizeBuffer;
    if (!update &&
        (as->desc.buildFlags & rt::AccelStructBuildFlags::AllowCompaction) != rt::AccelStructBuildFlags::None)
    {
        sizeBuffer = allocate(replay.context, sizeof(uint64_t));
        if (!sizeBuffer)
            return false;
        replay.temporaries.push_back(sizeBuffer);
        emit.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
        emit.result = sizeBuffer->ptr;
    }
    if (!checkOptix(replay.context,
                    optixAccelBuild(replay.context->optix, replay.stream, &options, inputs, count, scratch->ptr,
                                    update ? sizes.tempUpdateSizeInBytes : sizes.tempSizeInBytes, as->memory->ptr,
                                    as->sizes.outputSizeInBytes, &as->handle, sizeBuffer ? &emit : nullptr,
                                    sizeBuffer ? 1 : 0),
                    "optixAccelBuild"))
        return false;
    as->built = true;
    if (sizeBuffer)
    {
        uint64_t compactSize = 0;
        if (!replay.context->check(cuStreamSynchronize(replay.stream), "cuStreamSynchronize(compaction)") ||
            !replay.context->check(cuMemcpyDtoH(&compactSize, sizeBuffer->ptr, sizeof(compactSize)),
                                   "cuMemcpyDtoH(compaction size)"))
            return false;
        if (compactSize && compactSize < as->sizes.outputSizeInBytes)
        {
            auto memory = allocate(replay.context, size_t(compactSize));
            if (!memory)
                return false;
            if (!checkOptix(replay.context,
                            optixAccelCompact(replay.context->optix, replay.stream, as->handle, memory->ptr,
                                              size_t(compactSize), &as->handle),
                            "optixAccelCompact"))
                return false;
            replay.temporaries.push_back(as->memory);
            as->memory = memory;
            as->sizes.outputSizeInBytes = size_t(compactSize);
            as->compacted = true;
        }
    }
    return true;
}
void CommandList::buildBottomLevelAccelStruct(rt::IAccelStruct *iface, const rt::GeometryDesc *geometries, size_t count,
                                              rt::AccelStructBuildFlags flags) noexcept
{
    auto as = static_cast<AccelStruct *>(iface);
    if (!as || as->desc.isTopLevel || !geometries || !count)
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<AccelStruct> keep(as);
        std::vector<rt::GeometryDesc> gs(geometries, geometries + count);
        std::vector<RHIObjectHandle> refs;
        for (auto &g : gs)
        {
            if (g.geometryType == rt::GeometryType::Triangles)
            {
                if (g.geometryData.triangles.vertexBuffer)
                    refs.emplace_back(g.geometryData.triangles.vertexBuffer);
                if (g.geometryData.triangles.indexBuffer)
                    refs.emplace_back(g.geometryData.triangles.indexBuffer);
            }
            else if (g.geometryType == rt::GeometryType::AABBs && g.geometryData.aabbs.buffer)
                refs.emplace_back(g.geometryData.aabbs.buffer);
        }
        record([keep, gs = std::move(gs), refs = std::move(refs), flags](Replay &r) {
            BuildInputs input;
            if (!input.convert(gs.data(), gs.size(), &r))
                return false;
            return build(r, keep, input.inputs.data(), unsigned(input.inputs.size()), flags);
        });
    }
    catch (...)
    {
        valid = false;
    }
}
static bool buildInstances(Replay &r, AccelStruct *as, const std::vector<rt::InstanceDesc> &instances, bool addresses,
                           rt::AccelStructBuildFlags flags)
{
    if (instances.size() > as->desc.topLevelMaxInstances)
        return false;
    std::vector<OptixInstance> native(instances.size());
    for (size_t i = 0; i < instances.size(); ++i)
    {
        auto &n = native[i];
        auto &s = instances[i];
        std::memcpy(n.transform, s.transform, sizeof(n.transform));
        n.instanceId = s.instanceID;
        n.visibilityMask = s.instanceMask;
        n.sbtOffset = s.instanceContributionToHitGroupIndex;
        n.flags = static_cast<unsigned>(s.flags) & 0x3f;
        n.traversableHandle =
            addresses ? s.blasDeviceAddress : (s.bottomLevelAS ? s.bottomLevelAS->getDeviceAddress() : 0);
        if (!n.traversableHandle)
        {
            r.context->error("OptiX instance references an unbuilt BLAS");
            return false;
        }
    }
    auto memory = allocate(r.context, native.size() * sizeof(OptixInstance));
    if (!memory)
        return false;
    r.temporaries.push_back(memory);
    r.hostBlocks.emplace_back(native.size() * sizeof(OptixInstance));
    std::memcpy(r.hostBlocks.back().data(), native.data(), r.hostBlocks.back().size());
    if (!r.context->check(
            cuMemcpyHtoDAsync(memory->ptr, r.hostBlocks.back().data(), r.hostBlocks.back().size(), r.stream),
            "cuMemcpyHtoDAsync(instances)"))
        return false;
    OptixBuildInput input{};
    input.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    input.instanceArray.instances = memory->ptr;
    input.instanceArray.numInstances = unsigned(native.size());
    return build(r, as, &input, 1, flags);
}
void CommandList::buildTopLevelAccelStruct(rt::IAccelStruct *iface, const rt::InstanceDesc *instances, size_t count,
                                           rt::AccelStructBuildFlags flags) noexcept
{
    auto as = static_cast<AccelStruct *>(iface);
    if (!as || !as->desc.isTopLevel || (!instances && count))
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<AccelStruct> keep(as);
        std::vector<rt::InstanceDesc> copy(instances, instances + count);
        std::vector<rt::AccelStructHandle> refs;
        for (auto &i : copy)
            if (i.bottomLevelAS)
                refs.emplace_back(i.bottomLevelAS);
        record([keep, copy = std::move(copy), refs = std::move(refs), flags](Replay &r) {
            if (keep->desc.trackLiveness)
            {
                keep->resources.clear();
                for (auto &ref : refs)
                    keep->resources.emplace_back(ref.Get());
            }
            return buildInstances(r, keep, copy, false, flags);
        });
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::buildTopLevelAccelStructFromBuffer(rt::IAccelStruct *iface, IBuffer *buffer, uint64_t offset,
                                                     size_t count, rt::AccelStructBuildFlags flags) noexcept
{
    auto as = static_cast<AccelStruct *>(iface);
    auto b = static_cast<Buffer *>(buffer);
    if (!as || !as->desc.isTopLevel || !b || offset > b->desc.byteSize ||
        count > (b->desc.byteSize - offset) / sizeof(rt::InstanceDesc))
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<AccelStruct> keep(as);
        AutoPtr<Buffer> bk(b);
        record([keep, bk, offset, count, flags](Replay &r) {
            std::vector<rt::InstanceDesc> copy(count);
            if (!r.context->check(cuStreamSynchronize(r.stream), "cuStreamSynchronize(instance buffer)") ||
                !r.context->check(cuMemcpyDtoH(copy.data(), bk->memory->ptr + offset, count * sizeof(rt::InstanceDesc)),
                                  "cuMemcpyDtoH(instance buffer)"))
                return false;
            return buildInstances(r, keep, copy, true, flags);
        });
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::setRayTracingState(const rt::State &state) noexcept
{
    clearState();
    rayState = state;
    try
    {
        if (state.shaderTable)
            stateRefs.emplace_back(state.shaderTable);
        for (auto b : state.bindings)
            if (b)
                stateRefs.emplace_back(b);
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::dispatchRays(const rt::DispatchRaysArguments &args) noexcept
{
    auto table = static_cast<ShaderTable *>(rayState.shaderTable);
    if (!table || table->raygen.name.empty())
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<RayPipeline> pipeline = table->pipeline;
        auto raygen = table->raygen;
        auto miss = table->miss, hit = table->hit, callable = table->callable;
        auto state = rayState;
        auto refs = stateRefs;
        auto push = pushConstants;
        record([pipeline, raygen, miss = std::move(miss), hit = std::move(hit), callable = std::move(callable), state,
                refs = std::move(refs), push = std::move(push), args](Replay &r) {
            if (!args.width || !args.height || !args.depth)
                return true;
            std::vector<uint8_t> globals, entry;
            if (!packParameters(r, static_cast<Shader *>(pipeline->parameterShader.Get()), state.bindings, push,
                                globals, entry) ||
                !entry.empty())
                return false;
            auto upload = [&](std::vector<uint8_t> bytes) -> CUdeviceptr {
                if (bytes.empty())
                    return 0;
                auto a = allocate(r.context, bytes.size());
                if (!a)
                    return 0;
                r.temporaries.push_back(a);
                r.hostBlocks.push_back(std::move(bytes));
                auto &h = r.hostBlocks.back();
                if (!r.context->check(cuMemcpyHtoDAsync(a->ptr, h.data(), h.size(), r.stream),
                                      "cuMemcpyHtoDAsync(OptiX)"))
                    return 0;
                return a->ptr;
            };
            size_t parameterSize = globals.size();
            CUdeviceptr parameters = upload(std::move(globals));
            if (parameterSize && !parameters)
                return false;
            auto records = [&](const std::vector<TableEntry> &entries, unsigned &stride) -> CUdeviceptr {
                if (entries.empty())
                    return 0;
                size_t payloadSize = 0;
                for (const auto &entry : entries)
                {
                    auto g = pipeline->groups.find(entry.name);
                    if (g == pipeline->groups.end())
                        return 0;
                    if (g->second.recordShader)
                        payloadSize = std::max(
                            payloadSize,
                            size_t(static_cast<Shader *>(g->second.recordShader.Get())->entry.EntryParamsSize));
                }
                stride = unsigned((OPTIX_SBT_RECORD_HEADER_SIZE + payloadSize + OPTIX_SBT_RECORD_ALIGNMENT - 1) &
                                  ~(OPTIX_SBT_RECORD_ALIGNMENT - 1));
                std::vector<uint8_t> bytes(entries.size() * stride);
                for (size_t i = 0; i < entries.size(); ++i)
                {
                    auto &group = pipeline->groups.at(entries[i].name);
                    auto record = bytes.data() + i * stride;
                    if (!checkOptix(r.context, optixSbtRecordPackHeader(group.handle, record),
                                    "optixSbtRecordPackHeader"))
                        return 0;
                    if (group.recordShader)
                    {
                        std::vector<uint8_t> payload;
                        if (!packShaderRecord(r, static_cast<Shader *>(group.recordShader.Get()), entries[i].binding,
                                              payload))
                            return 0;
                        std::memcpy(record + OPTIX_SBT_RECORD_HEADER_SIZE, payload.data(), payload.size());
                    }
                }
                return upload(std::move(bytes));
            };
            OptixShaderBindingTable sbt{};
            unsigned raygenStride = 0;
            sbt.raygenRecord = records({raygen}, raygenStride);
            sbt.missRecordBase = records(miss, sbt.missRecordStrideInBytes);
            sbt.missRecordCount = unsigned(miss.size());
            if (miss.empty())
            {
                std::vector<uint8_t> header(OPTIX_SBT_RECORD_HEADER_SIZE);
                if (!checkOptix(r.context, optixSbtRecordPackHeader(pipeline->emptyMiss, header.data()),
                                "optixSbtRecordPackHeader(empty miss)"))
                    return false;
                sbt.missRecordBase = upload(std::move(header));
                sbt.missRecordStrideInBytes = OPTIX_SBT_RECORD_HEADER_SIZE;
                sbt.missRecordCount = 1;
            }
            sbt.hitgroupRecordBase = records(hit, sbt.hitgroupRecordStrideInBytes);
            sbt.hitgroupRecordCount = unsigned(hit.size());
            sbt.callablesRecordBase = records(callable, sbt.callablesRecordStrideInBytes);
            sbt.callablesRecordCount = unsigned(callable.size());
            if (!sbt.raygenRecord || !sbt.missRecordBase ||
                (!hit.empty() && !sbt.hitgroupRecordBase) || (!callable.empty() && !sbt.callablesRecordBase))
                return false;
            return checkOptix(r.context,
                              optixLaunch(pipeline->pipeline, r.stream, parameters, parameterSize, &sbt, args.width,
                                          args.height, args.depth),
                              "optixLaunch");
        });
    }
    catch (...)
    {
        valid = false;
    }
}
#else
FRESULT Device::createRayTracingPipeline(const rt::PipelineDesc &, rt::IPipeline **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    return FE_UNSUPPORTED;
}
FRESULT Device::createAccelStruct(const rt::AccelStructDesc &, rt::IAccelStruct **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    return FE_UNSUPPORTED;
}
void CommandList::setRayTracingState(const rt::State &) noexcept
{
    unsupported();
    valid = false;
}
void CommandList::dispatchRays(const rt::DispatchRaysArguments &) noexcept
{
    unsupported();
    valid = false;
}
void CommandList::buildBottomLevelAccelStruct(rt::IAccelStruct *, const rt::GeometryDesc *, size_t,
                                              rt::AccelStructBuildFlags) noexcept
{
    unsupported();
    valid = false;
}
void CommandList::buildTopLevelAccelStruct(rt::IAccelStruct *, const rt::InstanceDesc *, size_t,
                                           rt::AccelStructBuildFlags) noexcept
{
    unsupported();
    valid = false;
}
void CommandList::buildTopLevelAccelStructFromBuffer(rt::IAccelStruct *, IBuffer *, uint64_t, size_t,
                                                     rt::AccelStructBuildFlags) noexcept
{
    unsupported();
    valid = false;
}
#endif
MemoryRequirements &Device::getAccelStructMemoryRequirements(MemoryRequirements &out, rt::IAccelStruct *as) noexcept
{
    if (as)
        as->queryMemoryRequirements(out);
    return out;
}
// Compaction is performed directly after builds requesting AllowCompaction.
void CommandList::compactBottomLevelAccelStructs() noexcept
{
}
void CommandList::copyRaytracingAccelerationStructure(rt::IAccelStruct *destination, rt::IAccelStruct *source) noexcept
{
#if NVRHI_WITH_OPTIX
    auto dst = static_cast<AccelStruct *>(destination);
    auto src = static_cast<AccelStruct *>(source);
    if (!dst || !src || dst == src || dst->context != context || src->context != context ||
        dst->desc.isTopLevel != src->desc.isTopLevel)
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<AccelStruct> dstKeep(dst), srcKeep(src);
        record([dstKeep, srcKeep](Replay &r) {
            if (!srcKeep->built || dstKeep->sizes.outputSizeInBytes < srcKeep->sizes.outputSizeInBytes)
            {
                r.context->error(
                    "Acceleration structure clone requires a built source and sufficient destination space");
                return false;
            }
            OptixRelocationInfo info{};
            int compatible = 0;
            if (!checkOptix(r.context, optixAccelGetRelocationInfo(r.context->optix, srcKeep->handle, &info),
                            "optixAccelGetRelocationInfo") ||
                !checkOptix(r.context, optixCheckRelocationCompatibility(r.context->optix, &info, &compatible),
                            "optixCheckRelocationCompatibility") ||
                !compatible)
                return false;
            if (!r.context->check(cuMemcpyDtoDAsync(dstKeep->memory->ptr, srcKeep->memory->ptr,
                                                    srcKeep->sizes.outputSizeInBytes, r.stream),
                                  "cuMemcpyDtoDAsync(acceleration structure clone)") ||
                !checkOptix(r.context,
                            optixAccelRelocate(r.context->optix, r.stream, &info, nullptr, 0, dstKeep->memory->ptr,
                                               srcKeep->sizes.outputSizeInBytes, &dstKeep->handle),
                            "optixAccelRelocate"))
                return false;
            // Preserve child traversables and geometry resources independently of the source object.
            dstKeep->resources = srcKeep->resources;
            dstKeep->sizes = srcKeep->sizes;
            dstKeep->desc.buildFlags = srcKeep->desc.buildFlags;
            dstKeep->compacted = srcKeep->compacted;
            dstKeep->built = true;
            return true;
        });
    }
    catch (...)
    {
        valid = false;
    }
#else
    unsupported();
    valid = false;
#endif
}
} // namespace nvrhi::cuda
