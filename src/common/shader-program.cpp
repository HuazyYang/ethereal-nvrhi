// SPDX-License-Identifier: MIT
#include <nvrhi/shader-program.h>
#include <nvrhi/core/foundation.h>
#if NVRHI_WITH_SLANG
#include <ShaderToolArtifact.h>
#include <new>
#include <cstring>
#include <vector>
namespace nvrhi {
namespace {
ShaderType shaderType(uint32_t stage)
{
    switch(stage) {
    case ST_STAGE_VERTEX: return ShaderType::Vertex;
    case ST_STAGE_HULL: return ShaderType::Hull;
    case ST_STAGE_DOMAIN: return ShaderType::Domain;
    case ST_STAGE_GEOMETRY: return ShaderType::Geometry;
    case ST_STAGE_FRAGMENT: return ShaderType::Pixel;
    case ST_STAGE_COMPUTE: return ShaderType::Compute;
    case ST_STAGE_MESH: return ShaderType::Mesh;
    case ST_STAGE_AMPLIFICATION: return ShaderType::Amplification;
    case ST_STAGE_RAY_GENERATION: return ShaderType::RayGeneration;
    case ST_STAGE_INTERSECTION: return ShaderType::Intersection;
    case ST_STAGE_ANY_HIT: return ShaderType::AnyHit;
    case ST_STAGE_CLOSEST_HIT: return ShaderType::ClosestHit;
    case ST_STAGE_MISS: return ShaderType::Miss;
    case ST_STAGE_CALLABLE: return ShaderType::Callable;
    default: return ShaderType::None;
    }
}
ResourceType resourceType(uint32_t kind)
{
    switch(kind) {
    case ST_BINDING_CONSTANT_BUFFER: return ResourceType::ConstantBuffer;
    case ST_BINDING_STRUCTURED_BUFFER: return ResourceType::StructuredBuffer_SRV;
    case ST_BINDING_RW_STRUCTURED_BUFFER: return ResourceType::StructuredBuffer_UAV;
    case ST_BINDING_BYTE_ADDRESS_BUFFER: return ResourceType::RawBuffer_SRV;
    case ST_BINDING_RW_BYTE_ADDRESS_BUFFER: return ResourceType::RawBuffer_UAV;
    case ST_BINDING_TEXTURE: return ResourceType::Texture_SRV;
    case ST_BINDING_RW_TEXTURE: return ResourceType::Texture_UAV;
    case ST_BINDING_SAMPLER: return ResourceType::Sampler;
    case ST_BINDING_ACCELERATION_STRUCTURE: return ResourceType::RayTracingAccelStruct;
    case ST_BINDING_TYPED_BUFFER: return ResourceType::TypedBuffer_SRV;
    case ST_BINDING_RW_TYPED_BUFFER: return ResourceType::TypedBuffer_UAV;
    default: return ResourceType::None;
    }
}
bool compatible(GraphicsAPI api, uint32_t target)
{
    switch(api) {
    case GraphicsAPI::D3D11: return target == ST_TARGET_DXBC;
    case GraphicsAPI::D3D12: return target == ST_TARGET_DXIL;
    case GraphicsAPI::VULKAN: return target == ST_TARGET_SPIRV;
    case GraphicsAPI::CUDA: return target == ST_TARGET_PTX || target == ST_TARGET_OPTIX_PTX;
    default: return false;
    }
}
class ShaderProgram final : public ObjectImpl<IShaderProgram>
{
    DeviceHandle m_device;
    std::vector<unsigned char> m_bytes;
    ShaderToolArtifactView m_view{};
    ShaderToolArtifactInfo m_info{};
public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ShaderProgram)
    NVRHI_IMPLEMENTS_INTERFACE(IShaderProgram)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    ShaderProgram(IDevice* device, const void* data, size_t size) : m_device(device)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        m_bytes.assign(bytes, bytes + size);
        ShaderToolArtifactOpen(m_bytes.data(), m_bytes.size(), &m_view);
        ShaderToolArtifactGetInfo(&m_view, &m_info);
    }
    uint32_t getEntryPointCount() const noexcept override { return m_info.EntryCount; }
    FRESULT getEntryPointDesc(uint32_t index, ShaderEntryPointDesc& desc) const noexcept override
    {
        ShaderToolArtifactEntry entry{};
        if(ShaderToolArtifactGetEntry(&m_view,index,&entry)) return FE_INVALID_ARGS;
        desc.name=entry.Name; desc.symbol=entry.Symbol; desc.shaderType=shaderType(entry.Stage);
        for(unsigned axis=0;axis<3;++axis) desc.threadGroupSize[axis]=entry.ThreadGroupSize[axis];
        desc.globalParamsSize=entry.GlobalParamsSize; desc.entryParamsSize=entry.EntryParamsSize;
        return FS_OK;
    }
    uint32_t getParameterCount(uint32_t index) const noexcept override
    {
        ShaderToolArtifactEntry entry{};
        return ShaderToolArtifactGetEntry(&m_view,index,&entry) ? 0 : entry.BindingCount;
    }
    FRESULT getParameterDesc(uint32_t entryIndex,uint32_t index,ShaderParameterDesc& desc) const noexcept override
    {
        ShaderToolArtifactEntry entry{}; ShaderToolArtifactBinding binding{};
        if(ShaderToolArtifactGetEntry(&m_view,entryIndex,&entry) || index>=entry.BindingCount ||
            ShaderToolArtifactGetBinding(&m_view,entry.BindingFirst+index,&binding)) return FE_INVALID_ARGS;
        desc.name=binding.Name; desc.resourceType=resourceType(binding.Kind);
        desc.scope=static_cast<ShaderParameterScope>(binding.Scope);
        desc.registerIndex=binding.RegisterIndex; desc.registerSpace=binding.RegisterSpace;
        desc.targetIndex=binding.TargetIndex; desc.targetSpace=binding.TargetSpace;
        desc.offset=binding.Offset; desc.size=binding.Size; desc.elementStride=binding.ElementStride; desc.count=binding.Count;
        desc.parentParameter=binding.ParentBinding==SHADER_TOOL_ARTIFACT_NONE ? ~0u : binding.ParentBinding-entry.BindingFirst;
        desc.samplerRegisterIndex=binding.SamplerIndex; desc.samplerRegisterSpace=binding.SamplerSpace;
        return FS_OK;
    }
    FRESULT findEntryPoint(const char* name, ShaderType type, uint32_t* index) const noexcept override
    {
        if (!index) return FE_INVALID_ARGS;
        *index = ~0u;
        const bool named = name && *name;
        // Resolve logical names first: several SPIR-V entries can export "main".
        for (unsigned pass = 0; pass < (named ? 2u : 1u); ++pass) {
            uint32_t found = ~0u;
            for (uint32_t i = 0; i < m_info.EntryCount; ++i) {
                ShaderToolArtifactEntry e{};
                if (ShaderToolArtifactGetEntry(&m_view, i, &e)) return FE_INVALID_ARGS;
                if (type != ShaderType::None && shaderType(e.Stage) != type) continue;
                if (named && std::strcmp(name, pass == 0 ? e.Name : e.Symbol)) continue;
                if (found != ~0u) return FE_INVALID_ARGS;
                found = i;
            }
            if (found != ~0u) { *index = found; return FS_OK; }
        }
        return FE_NOT_FOUND;
    }
    FRESULT createShader(uint32_t index, IShader** shader) noexcept override
    {
        return createShaderWithDesc(index, ShaderDesc(), shader);
    }
    FRESULT createShaderWithDesc(uint32_t index, const ShaderDesc& options, IShader** shader) noexcept override
    {
        if (!shader) return FE_INVALID_ARGS;
        *shader = nullptr;
        ShaderToolArtifactEntry entry{};
        if (ShaderToolArtifactGetEntry(&m_view, index, &entry)) return FE_INVALID_ARGS;
        const ShaderType type = shaderType(entry.Stage);
        if (type == ShaderType::None) return FE_UNSUPPORTED;
        if (options.shaderType != ShaderType::None && options.shaderType != type) return FE_INVALID_ARGS;
        if (options.numCustomSemantics && !options.pCustomSemantics) return FE_INVALID_ARGS;
        try {
            ShaderDesc desc = options;
            desc.shaderType = type;
            desc.entryName = entry.Symbol;
            if (desc.debugName.empty()) desc.debugName = entry.Name;
            // CUDA consumes reflection together with executable code to build its launch ABI.
            if (m_device->getGraphicsAPI() == GraphicsAPI::CUDA)
                return m_device->createShader(desc, m_bytes.data(), m_bytes.size(), shader);
            return m_device->createShader(desc, entry.Code, entry.CodeSize, shader);
        } catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
          catch (...) { return FE_GENERIC_ERROR; }
    }
    FRESULT getBindingLayoutDesc(uint32_t index, uint32_t space, BindingLayoutDesc& desc) const noexcept override
    {
        ShaderToolArtifactEntry entry{};
        if (ShaderToolArtifactGetEntry(&m_view, index, &entry)) return FE_INVALID_ARGS;
        const auto api = m_device->getGraphicsAPI();
        if (api == GraphicsAPI::D3D11 && space != 0) return FE_UNSUPPORTED;
        try {
            BindingLayoutDesc result;
            result.visibility = shaderType(entry.Stage);
            result.registerSpace = space;
            result.registerSpaceIsDescriptorSet = api != GraphicsAPI::D3D11;
            bool assigned[4] = {};
            bool assignedSet = false;
            uint32_t offsets[4] = {0, 128, 256, 384};
            struct Seen { uint32_t category, slot, count, physical; ResourceType type; };
            std::vector<Seen> seen;
            for (uint32_t i = 0; i < entry.BindingCount; ++i) {
                ShaderToolArtifactBinding binding{};
                if (ShaderToolArtifactGetBinding(&m_view, entry.BindingFirst + i, &binding)) return FE_INVALID_ARGS;
                if (binding.Kind == ST_BINDING_UNIFORM || binding.Scope == ST_SCOPE_CONSTANT_BUFFER ||
                    binding.Scope == ST_SCOPE_SHADER_RECORD || binding.RegisterSpace != space) continue;
                const ResourceType type = resourceType(binding.Kind);
                if (type == ResourceType::None || !binding.Count || binding.Count > UINT16_MAX) return FE_UNSUPPORTED;
                if (uint64_t(binding.RegisterIndex) + binding.Count > uint64_t(UINT32_MAX) + 1) return FE_INVALID_ARGS;
                uint32_t category = 0;
                switch (binding.Kind) {
                case ST_BINDING_SAMPLER: category = 1; break;
                case ST_BINDING_CONSTANT_BUFFER: category = 2; break;
                case ST_BINDING_RW_STRUCTURED_BUFFER: case ST_BINDING_RW_BYTE_ADDRESS_BUFFER:
                case ST_BINDING_RW_TEXTURE: case ST_BINDING_RW_TYPED_BUFFER: category = 3; break;
                default: break;
                }
                if (api == GraphicsAPI::VULKAN) {
                    if (binding.TargetIndex < binding.RegisterIndex) return FE_UNSUPPORTED;
                    const uint32_t shift = binding.TargetIndex - binding.RegisterIndex;
                    if (assigned[category] && offsets[category] != shift) return FE_UNSUPPORTED;
                    assigned[category] = true;
                    offsets[category] = shift;
                    if (assignedSet && result.registerSpace != binding.TargetSpace) return FE_UNSUPPORTED;
                    assignedSet = true;
                    result.registerSpace = binding.TargetSpace;
                }
                bool duplicate = false;
                for (const auto& previous : seen) {
                    if (previous.category == category && previous.slot == binding.RegisterIndex &&
                        previous.count == binding.Count && previous.type == type &&
                        (api != GraphicsAPI::VULKAN || previous.physical == binding.TargetIndex)) {
                        duplicate = true; break;
                    }
                    if (previous.category == category &&
                        uint64_t(previous.slot) < uint64_t(binding.RegisterIndex) + binding.Count &&
                        uint64_t(binding.RegisterIndex) < uint64_t(previous.slot) + previous.count)
                        return FE_INVALID_ARGS;
                    if (api == GraphicsAPI::VULKAN && previous.physical == binding.TargetIndex)
                        return FE_INVALID_ARGS;
                }
                if (duplicate) continue;
                seen.push_back({category, binding.RegisterIndex, binding.Count, binding.TargetIndex, type});
                BindingLayoutItem item{};
                item.slot = binding.RegisterIndex;
                item.type = type;
                item.size = static_cast<uint16_t>(binding.Count);
                result.bindings.push_back(item);
            }
            result.bindingOffsets.shaderResource = offsets[0];
            result.bindingOffsets.sampler = offsets[1];
            result.bindingOffsets.constantBuffer = offsets[2];
            result.bindingOffsets.unorderedAccess = offsets[3];
            desc = std::move(result);
            return FS_OK;
        } catch (const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
          catch (...) { return FE_GENERIC_ERROR; }
    }
};
}
FRESULT nvrhiCreateShaderProgram(IDevice* device,const void* artifact,size_t size,IShaderProgram** program) noexcept
{
    if(!program) return FE_INVALID_ARGS;
    *program=nullptr;
    if(!device || !artifact || !size) return FE_INVALID_ARGS;
    ShaderToolArtifactView view{}; ShaderToolArtifactInfo info{};
    if(ShaderToolArtifactOpen(artifact,size,&view) || ShaderToolArtifactGetInfo(&view,&info)) return FE_INVALID_ARGS;
    if(!compatible(device->getGraphicsAPI(),info.Target)) return FE_UNSUPPORTED;
    try {
        auto result=MAKE_RC_OBJ_PTR(ShaderProgram,device,artifact,size);
        *program=result.Detach(); return FS_OK;
    } catch(const std::bad_alloc&) { return FE_OUT_OF_MEMORY; }
      catch(...) { return FE_GENERIC_ERROR; }
}
}
#else
namespace nvrhi {
FRESULT nvrhiCreateShaderProgram(IDevice* device,const void* artifact,size_t size,IShaderProgram** program) noexcept
{
    if(!program) return FE_INVALID_ARGS;
    *program=nullptr;
    return (!device || !artifact || !size) ? FE_INVALID_ARGS : FE_UNSUPPORTED;
}
}
#endif
