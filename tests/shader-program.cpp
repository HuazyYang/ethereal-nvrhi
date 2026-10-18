// ShaderProgram runtime contract: no compiler or graphics driver required.
#include "recording-device.h"
#include <nvrhi/shader-program.h>
#include <ShaderToolArtifact.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <iterator>
#include <string>
#include <algorithm>
using namespace nvrhi;
using nvrhi_test::RecordingDevice;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (0)

static std::vector<unsigned char> writeArtifact(uint32_t target,
    std::vector<ShaderToolArtifactBinding> bindings = {}, std::vector<ShaderToolArtifactEntry> entries = {})
{
    static const char code[] = "native-executable";
    if (entries.empty()) {
        ShaderToolArtifactEntry entry{};
        entry.Name = "sourceMain"; entry.Symbol = "nativeMain";
        entry.Stage = target == ST_TARGET_OPTIX_PTX ? ST_STAGE_CLOSEST_HIT : ST_STAGE_COMPUTE;
        entry.ThreadGroupSize[0] = 8; entry.ThreadGroupSize[1] = 2; entry.ThreadGroupSize[2] = 1;
        entry.GlobalParamsSize = 64; entry.EntryParamsSize = 8;
        entry.GlobalParamsAlignment = entry.EntryParamsAlignment = 8;
        entry.GlobalParamsSymbol = "SLANG_globalParams";
        entry.BindingCount = uint32_t(bindings.size());
        entry.Code = code; entry.CodeSize = sizeof(code);
        entries.push_back(entry);
    }
    ShaderToolArtifactDesc desc{};
    desc.Target = target; desc.Compiler = "fixture";
    desc.Bindings = bindings.data(); desc.BindingCount = uint32_t(bindings.size());
    desc.Entries = entries.data(); desc.EntryCount = uint32_t(entries.size());
    size_t size = 0;
    CHECK(!ShaderToolArtifactWrite(&desc, nullptr, 0, &size));
    std::vector<unsigned char> bytes(size);
    CHECK(!ShaderToolArtifactWrite(&desc, bytes.data(), bytes.size(), &size));
    return bytes;
}
static ShaderToolArtifactBinding binding(const char* name, uint32_t kind, uint32_t slot, uint32_t target)
{
    ShaderToolArtifactBinding b{};
    b.Name = name; b.Kind = kind; b.RegisterIndex = slot; b.RegisterSpace = 1;
    b.TargetIndex = target; b.TargetSpace = 7; b.Count = 1;
    b.SamplerIndex = b.SamplerSpace = b.ParentBinding = SHADER_TOOL_ARTIFACT_NONE;
    return b;
}
static ShaderProgramHandle open(IDevice* device, const std::vector<unsigned char>& bytes)
{
    ShaderProgramHandle result;
    CHECK(createShaderProgram(device, bytes.data(), bytes.size(), &result) == FS_OK);
    return result;
}
static void lifetimeAndOptions()
{
    auto device = MAKE_RC_OBJ_PTR(RecordingDevice, GraphicsAPI::VULKAN);
    auto bytes = writeArtifact(ST_TARGET_SPIRV);
    auto program = open(device, bytes);
    // Returned metadata and shaders must outlive the input artifact buffer.
    std::fill(bytes.begin(), bytes.end(), 0);
    ShaderEntryPointDesc entry;
    CHECK(program->getEntryPointDesc(0, entry) == FS_OK);
    CHECK(entry.name == "sourceMain" && entry.symbol == "nativeMain" && entry.threadGroupSize[0] == 8);
    uint32_t index = 99;
    CHECK(program->findEntryPoint("sourceMain", ShaderType::Compute, &index) == FS_OK && index == 0);
    CHECK(program->findEntryPoint("nativeMain", ShaderType::None, &index) == FS_OK && index == 0);
    CHECK(program->findEntryPoint(nullptr, ShaderType::None, &index) == FS_OK && index == 0);
    CHECK(program->findEntryPoint("absent", ShaderType::None, &index) == FE_NOT_FOUND && index == ~0u);
    CHECK(program->findEntryPoint("sourceMain", ShaderType::Pixel, &index) == FE_NOT_FOUND);
    void* queried = nullptr;
    CHECK(program->QueryInterface(uuid_of<IShaderProgram>(), &queried) == FS_OK && queried == program.Get());
    static_cast<IShaderProgram*>(queried)->Release();
    CHECK(program->QueryInterface(uuid_of<IRHIObject>(), &queried) == FS_OK);
    static_cast<IRHIObject*>(queried)->Release();
    CHECK(program->QueryInterface(uuid_of<IObject>(), &queried) == FS_OK);
    static_cast<IObject*>(queried)->Release();
    CHECK(program->QueryInterface(uuid_of<IDevice>(), &queried) == FE_NOINTERFACE && !queried);
    ShaderDesc desc;
    CustomSemantic semantic{}; uint32_t swizzle[4] = {0,1,2,3};
    desc.shaderType = ShaderType::Compute; desc.debugName = "caller-name"; desc.entryName = "ignored";
    desc.hlslExtensionsUAV = 12; desc.useSpecificShaderExt = true;
    desc.numCustomSemantics = 1; desc.pCustomSemantics = &semantic;
    desc.fastGSFlags = FastGeometryShaderFlags::StrictApiOrder; desc.pCoordinateSwizzling = swizzle;
    ShaderHandle shader;
    CHECK(program->createShaderWithDesc(0, desc, &shader) == FS_OK);
    const auto& actual = shader->getDesc();
    CHECK(actual.entryName == "nativeMain" && actual.debugName == "caller-name");
    CHECK(actual.hlslExtensionsUAV == 12 && actual.useSpecificShaderExt && actual.numCustomSemantics == 1);
    CHECK(actual.pCustomSemantics == &semantic && actual.pCoordinateSwizzling == swizzle && actual.fastGSFlags == desc.fastGSFlags);
    const void* code = nullptr; size_t size = 0;
    shader->getBytecode(&code, &size);
    CHECK(size == sizeof("native-executable") && !memcmp(code, "native-executable", size));
    desc.shaderType = ShaderType::Pixel;
    IShader* bad = reinterpret_cast<IShader*>(1);
    CHECK(program->createShaderWithDesc(0, desc, &bad) == FE_INVALID_ARGS && !bad);
    CHECK(program->createShader(88, &bad) == FE_INVALID_ARGS && !bad);
    device->failCreation = true;
    CHECK(program->createShader(0, &bad) == FE_GENERIC_ERROR && !bad);
    CHECK(device->shaderCalls == 2);
    program = nullptr;
    CHECK(shader->getDesc().debugName == "caller-name");
}
static void malformedAndTargets()
{
    const GraphicsAPI apis[] = {GraphicsAPI::D3D11, GraphicsAPI::D3D12, GraphicsAPI::VULKAN, GraphicsAPI::CUDA, GraphicsAPI::CUDA};
    for (uint32_t target = ST_TARGET_DXBC; target <= ST_TARGET_OPTIX_PTX; ++target) {
        auto device = MAKE_RC_OBJ_PTR(RecordingDevice, apis[target - 1]);
        const auto bytes = writeArtifact(target);
        auto program = open(device, bytes);
        ShaderHandle shader;
        CHECK(program->createShader(0, &shader) == FS_OK);
        const void* code = nullptr; size_t size = 0; shader->getBytecode(&code, &size);
        CHECK((target >= ST_TARGET_PTX) == (size == bytes.size() && !memcmp(code, "NSTA", 4)));
        for (size_t n = 0; n < bytes.size(); ++n) {
            IShaderProgram* bad = reinterpret_cast<IShaderProgram*>(1);
            CHECK(createShaderProgram(device, bytes.data(), n, &bad) == FE_INVALID_ARGS && !bad);
        }
        device->api = apis[target - 1] == GraphicsAPI::VULKAN ? GraphicsAPI::D3D12 : GraphicsAPI::VULKAN;
        IShaderProgram* bad = reinterpret_cast<IShaderProgram*>(1);
        CHECK(createShaderProgram(device, bytes.data(), bytes.size(), &bad) == FE_UNSUPPORTED && !bad);
        CHECK(device->shaderCalls == 1);
    }
}
static void ambiguousEntries()
{
    auto device = MAKE_RC_OBJ_PTR(RecordingDevice, GraphicsAPI::VULKAN);
    const auto original = writeArtifact(ST_TARGET_SPIRV);
    ShaderToolArtifactView view{}; ShaderToolArtifactEntry entry{};
    CHECK(!ShaderToolArtifactOpen(original.data(), original.size(), &view));
    CHECK(!ShaderToolArtifactGetEntry(&view, 0, &entry));
    entry.Name = "first"; entry.Symbol = "main";
    auto second = entry; second.Name = "second";
    auto program = open(device, writeArtifact(ST_TARGET_SPIRV, {}, {entry, second}));
    uint32_t index = 0;
    CHECK(program->findEntryPoint("first", ShaderType::Compute, &index) == FS_OK && index == 0);
    CHECK(program->findEntryPoint("second", ShaderType::Compute, &index) == FS_OK && index == 1);
    CHECK(program->findEntryPoint("main", ShaderType::Compute, &index) == FE_INVALID_ARGS && index == ~0u);
    CHECK(program->findEntryPoint(nullptr, ShaderType::Compute, &index) == FE_INVALID_ARGS && index == ~0u);
    second.Name = "main";
    program = open(device, writeArtifact(ST_TARGET_SPIRV, {}, {entry, second}));
    CHECK(program->findEntryPoint("main", ShaderType::Compute, &index) == FS_OK && index == 1);
}
static void reflectedLayouts()
{
    auto device = MAKE_RC_OBJ_PTR(RecordingDevice, GraphicsAPI::VULKAN);
    std::vector<ShaderToolArtifactBinding> bindings;
    auto cb = binding("params", ST_BINDING_CONSTANT_BUFFER, 2, 202); cb.ElementStride = 96;
    bindings.push_back(cb);
    auto array = binding("params.values", ST_BINDING_UNIFORM, 0, 0);
    array.Scope = ST_SCOPE_CONSTANT_BUFFER; array.ParentBinding = 0;
    array.Offset = 16; array.Size = 48; array.ElementStride = 16; array.Count = 3;
    bindings.push_back(array);
    auto matrix = array; matrix.Name = "params.matrix"; matrix.Offset = 64; matrix.Size = 32; matrix.Count = 1;
    bindings.push_back(matrix);
    auto textures = binding("sources", ST_BINDING_TEXTURE, 4, 21); textures.Count = 3;
    bindings.push_back(textures);
    bindings.push_back(binding("sampler", ST_BINDING_SAMPLER, 2, 52));
    bindings.push_back(binding("output", ST_BINDING_RW_STRUCTURED_BUFFER, 3, 103));
    auto program = open(device, writeArtifact(ST_TARGET_SPIRV, bindings));
    ShaderParameterDesc parameter;
    CHECK(program->getParameterCount(0) == 6);
    CHECK(program->getParameterDesc(0, 1, parameter) == FS_OK);
    CHECK(parameter.parentParameter == 0 && parameter.offset == 16 && parameter.size == 48 && parameter.count == 3 && parameter.elementStride == 16);
    CHECK(program->getParameterDesc(0, 2, parameter) == FS_OK && parameter.offset == 64 && parameter.size == 32);
    BindingLayoutDesc layout;
    CHECK(program->getBindingLayoutDesc(0, 1, layout) == FS_OK);
    CHECK(layout.bindings.size() == 4 && layout.bindings[1].slot == 4 && layout.bindings[1].size == 3);
    CHECK(layout.registerSpace == 7 && layout.registerSpaceIsDescriptorSet);
    CHECK(layout.bindingOffsets.shaderResource == 17 && layout.bindingOffsets.sampler == 50 &&
        layout.bindingOffsets.constantBuffer == 200 && layout.bindingOffsets.unorderedAccess == 100);
    // Inconsistent shifts within one register class cannot be represented by NVRHI.
    bindings.push_back(binding("otherSource", ST_BINDING_TEXTURE, 9, 99));
    program = open(device, writeArtifact(ST_TARGET_SPIRV, bindings));
    CHECK(program->getBindingLayoutDesc(0, 1, layout) == FE_UNSUPPORTED && layout.bindings.size() == 4);
    bindings.pop_back();
    bindings.push_back(binding("overlap", ST_BINDING_STRUCTURED_BUFFER, 5, 22));
    program = open(device, writeArtifact(ST_TARGET_SPIRV, bindings));
    CHECK(program->getBindingLayoutDesc(0, 1, layout) == FE_INVALID_ARGS);
    bindings.pop_back();
    bindings.back().TargetIndex = 21; // Physical collision across SRV/UAV classes.
    program = open(device, writeArtifact(ST_TARGET_SPIRV, bindings));
    CHECK(program->getBindingLayoutDesc(0, 1, layout) == FE_INVALID_ARGS);
    device->api = GraphicsAPI::D3D11;
    program = open(device, writeArtifact(ST_TARGET_DXBC));
    CHECK(program->getBindingLayoutDesc(0, 1, layout) == FE_UNSUPPORTED);
    device->api = GraphicsAPI::CUDA;
    auto local = binding("material", ST_BINDING_CONSTANT_BUFFER, 0, 0);
    local.Scope = ST_SCOPE_SHADER_RECORD; local.Size = 8; local.ElementStride = 16;
    program = open(device, writeArtifact(ST_TARGET_OPTIX_PTX, {local}));
    CHECK(program->getParameterDesc(0, 0, parameter) == FS_OK && parameter.scope == ShaderParameterScope::ShaderRecord);
    CHECK(program->getBindingLayoutDesc(0, 1, layout) == FS_OK && layout.bindings.empty());
}
static void realReflection(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    CHECK(file.good());
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto device = MAKE_RC_OBJ_PTR(RecordingDevice, GraphicsAPI::VULKAN);
    auto program = open(device, bytes);
    bool array = false, matrix = false, resources = false;
    for (uint32_t i = 0; i < program->getParameterCount(0); ++i) {
        ShaderParameterDesc p;
        CHECK(program->getParameterDesc(0, i, p) == FS_OK);
        if (p.name == "params.values") { CHECK(p.offset == 16 && p.elementStride == 16 && p.count == 3); array = true; }
        if (p.name == "params.transform") { CHECK(p.offset == 64 && p.size == 32); matrix = true; }
        if (p.name == "sources") { CHECK(p.registerIndex == 4 && p.count == 3); resources = true; }
    }
    CHECK(array && matrix && resources);
    BindingLayoutDesc layout;
    CHECK(program->getBindingLayoutDesc(0, 1, layout) == FS_OK && layout.bindings.size() == 3);
    CHECK(layout.bindingOffsets.constantBuffer == 256 && layout.bindingOffsets.unorderedAccess == 384);
}
int main(int argc, char** argv)
{
    try {
        CHECK(verifyHeaderVersion());
        lifetimeAndOptions(); malformedAndTargets(); ambiguousEntries(); reflectedLayouts();
        if (argc > 1) realReflection(argv[1]);
        std::puts("Shader program artifact contracts passed");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
