// SPDX-License-Identifier: MIT
#include <ShaderToolArtifact.h>
#include <cstdio>
#include <cstring>
#include <nvrhi/cuda.h>
#include <vector>
using namespace nvrhi;
static const char *ptx = R"(.version 7.0
.target sm_50
.address_size 64
.const .align 8 .b8 SLANG_globalParams[16];
.visible .entry fill()
{
 .reg .b32 %r<6>;
 .reg .b64 %rd<6>;
 .reg .pred %p;
 mov.u64 %rd0,SLANG_globalParams;
 ld.const.u64 %rd1,[%rd0];
 ld.const.u64 %rd2,[%rd0+8];
 mov.u32 %r0,%ctaid.x;
 mov.u32 %r1,%ntid.x;
 mov.u32 %r2,%tid.x;
 mad.lo.u32 %r3,%r0,%r1,%r2;
 cvt.u64.u32 %rd3,%r3;
 setp.ge.u64 %p,%rd3,%rd2;
 @%p bra done;
 mul.lo.u32 %r4,%r3,3;
 add.u32 %r5,%r4,7;
 mul.wide.u32 %rd4,%r3,4;
 add.u64 %rd5,%rd1,%rd4;
 st.global.u32 [%rd5],%r5;
 done: ret;
}
)";
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "failed: %s line %d\n", #c, __LINE__);                                                \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)
int main()
{
    cuda::DeviceDesc desc;
    desc.enableOptiX = false;
    auto device = cuda::createDevice(desc);
    if (!device)
    {
        std::puts("CUDA device unavailable");
        return 77;
    }
    ShaderToolArtifactEntry entry{};
    entry.Name = entry.Symbol = "fill";
    entry.GlobalParamsSymbol = "SLANG_globalParams";
    entry.Stage = ST_STAGE_COMPUTE;
    entry.ThreadGroupSize[0] = 64;
    entry.ThreadGroupSize[1] = entry.ThreadGroupSize[2] = 1;
    entry.GlobalParamsSize = 16;
    entry.GlobalParamsAlignment = 8;
    entry.EntryParamsAlignment = 1;
    entry.BindingCount = 1;
    entry.Code = ptx;
    entry.CodeSize = std::strlen(ptx);
    ShaderToolArtifactBinding binding{};
    binding.Name = "output";
    binding.Kind = ST_BINDING_RW_STRUCTURED_BUFFER;
    binding.Scope = ST_SCOPE_GLOBAL;
    binding.Size = 16;
    binding.ElementStride = 4;
    binding.Count = 1;
    binding.ParentBinding = binding.SamplerIndex = binding.SamplerSpace = SHADER_TOOL_ARTIFACT_NONE;
    ShaderToolArtifactDesc artifact{};
    artifact.Target = ST_TARGET_PTX;
    artifact.Compiler = "CUDA backend fixture";
    artifact.Entries = &entry;
    artifact.EntryCount = 1;
    artifact.Bindings = &binding;
    artifact.BindingCount = 1;
    size_t size = 0;
    CHECK(ShaderToolArtifactWrite(&artifact, nullptr, 0, &size) == 0);
    std::vector<uint8_t> bytes(size);
    CHECK(ShaderToolArtifactWrite(&artifact, bytes.data(), size, &size) == 0);
    ShaderHandle shader;
    CHECK(NVRHI_SUCCEEDED(device->createShader(ShaderDesc().setShaderType(ShaderType::Compute).setEntryName("fill"),
                                               bytes.data(), bytes.size(), &shader)));
    CHECK(shader);
    BindingLayoutHandle layout;
    CHECK(NVRHI_SUCCEEDED(device->createBindingLayout(
        BindingLayoutDesc().setVisibility(ShaderType::Compute).addItem(BindingLayoutItem::StructuredBuffer_UAV(0)),
        &layout)));
    CHECK(layout);
    ComputePipelineHandle pipeline;
    CHECK(NVRHI_SUCCEEDED(device->createComputePipeline(
        ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout), &pipeline)));
    CHECK(pipeline);
    constexpr uint32_t count = 259;
    BufferHandle buffer;
    CHECK(NVRHI_SUCCEEDED(
        device->createBuffer(BufferDesc().setByteSize(count * 4).setStructStride(4).setCanHaveUAVs(true), &buffer)));
    CHECK(buffer);
    BufferHandle readback;
    CHECK(NVRHI_SUCCEEDED(
        device->createBuffer(BufferDesc().setByteSize(count * 4).setCpuAccess(CpuAccessMode::Read), &readback)));
    CHECK(readback);
    BindingSetHandle set;
    CHECK(NVRHI_SUCCEEDED(device->createBindingSet(
        BindingSetDesc().addItem(BindingSetItem::StructuredBuffer_UAV(0, buffer)), layout, &set)));
    CHECK(set);
    CommandListHandle list;
    CHECK(
        NVRHI_SUCCEEDED(device->createCommandList(CommandListParameters().setQueueType(CommandQueue::Compute), &list)));
    CHECK(list);
    list->open();
    list->setComputeState(ComputeState().setPipeline(pipeline).addBindingSet(set));
    list->dispatch((count + 63) / 64);
    list->copyBuffer(readback, 0, buffer, 0, count * 4);
    list->close();
    auto id = device->executeCommandList(list, CommandQueue::Compute);
    CHECK(id);
    // Reopening releases recording references; submitted work must keep them alive.
    list->open();
    list->close();
    set = nullptr;
    pipeline = nullptr;
    shader = nullptr;
    buffer = nullptr;
    auto result = static_cast<const uint32_t *>(device->mapBuffer(readback, CpuAccessMode::Read));
    CHECK(result);
    for (uint32_t i = 0; i < count; ++i)
        CHECK(result[i] == i * 3 + 7);
    device->unmapBuffer(readback);
    IFramebuffer *unsupported = reinterpret_cast<IFramebuffer *>(1);
    CHECK(NVRHI_SUCCEEDED(device->createFramebuffer(FramebufferDesc(), &unsupported) == FE_UNSUPPORTED));
    CHECK(!unsupported);

    // 3D texture upload/download and copied subregions exercise pitched transfers.
    TextureDesc td;
    td.width = 7;
    td.height = 5;
    td.depth = 3;
    td.dimension = TextureDimension::Texture3D;
    td.format = Format::R32_UINT;
    td.isUAV = true;
    TextureHandle texture;
    CHECK(NVRHI_SUCCEEDED(device->createTexture(td, &texture)));
    StagingTextureHandle staging;
    CHECK(NVRHI_SUCCEEDED(device->createStagingTexture(td, CpuAccessMode::Read, &staging)));
    std::vector<uint32_t> texels(td.width * td.height * td.depth);
    for (uint32_t i = 0; i < texels.size(); ++i)
        texels[i] = i * 17 + 11;
    list->open();
    list->writeTexture(texture, 0, 0, texels.data(), td.width * 4, td.width * td.height * 4);
    list->copyTexture2(staging, TextureSlice(), texture, TextureSlice());
    list->close();
    CHECK(device->executeCommandList(list, CommandQueue::Compute));
    size_t pitch = 0;
    auto pixels =
        static_cast<const uint32_t *>(device->mapStagingTexture(staging, TextureSlice(), CpuAccessMode::Read, pitch));
    CHECK(pixels && pitch == td.width * 4);
    for (size_t i = 0; i < texels.size(); ++i)
        CHECK(pixels[i] == texels[i]);
    device->unmapStagingTexture(staging);
    EventQueryHandle event;
    CHECK(NVRHI_SUCCEEDED(device->createEventQuery(&event)));
    device->setEventQuery(event, CommandQueue::Compute);
    device->waitEventQuery(event);
    CHECK(device->pollEventQuery(event));
    device->resetEventQuery(event);
    CHECK(!device->pollEventQuery(event));
    std::puts("CUDA constant ABI, dispatch, lifetime, 3D transfers, events and unsupported output tests passed");
    return 0;
}
