// SPDX-License-Identifier: MIT
#include <cmath>
#include <cstdio>
#include <fstream>
#include <nvrhi/core/foundation.h>
#include <nvrhi/cuda.h>
#include <nvrhi/shader-program.h>
#include <string>
#include <vector>
using namespace nvrhi;
struct Messages : ObjectImpl<IMessageCallback>
{
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity, const char *message) noexcept override
    {
        std::fprintf(stderr, "NVRHI: %s\n", message);
    }
};
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #c);                                                \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)
#define OK(c) CHECK(NVRHI_SUCCEEDED(c))
static std::vector<char> readFile(const char *path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static ShaderHandle getShader(IShaderProgram *program, ShaderType type)
{
    ShaderHandle shader;
    for (uint32_t i = 0; i < program->getEntryPointCount(); ++i)
    {
        ShaderEntryPointDesc d;
        program->getEntryPointDesc(i, d);
        if (d.shaderType == type)
        {
            program->createShader(i, &shader);
            break;
        }
    }
    return shader;
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    auto callback = MAKE_RC_OBJ_PTR(Messages);
    cuda::DeviceDesc desc;
    desc.errorCB = callback;
    desc.enableOptiX = false;
    auto device = cuda::createDevice(desc);
    if (!device)
        return 77;
    auto artifact = readFile(argv[1]);
    ShaderProgramHandle program;
    OK(nvrhiCreateShaderProgram(device, artifact.data(), artifact.size(), &program));
    auto shader = getShader(program, ShaderType::Compute);
    CHECK(shader);
    BindingLayoutHandle layout;
    OK(device->createBindingLayout(BindingLayoutDesc()
                                       .setVisibility(ShaderType::Compute)
                                       .addItem(BindingLayoutItem::Texture_SRV(0))
                                       .addItem(BindingLayoutItem::Sampler(0))
                                       .addItem(BindingLayoutItem::StructuredBuffer_UAV(0)),
                                   &layout));
    ComputePipelineHandle pipeline;
    OK(device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout),
                                     &pipeline));
    TextureDesc td;
    td.setWidth(4)
        .setHeight(4)
        .setArraySize(3)
        .setMipLevels(3)
        .setFormat(Format::RGBA32_FLOAT)
        .setDimension(TextureDimension::Texture2DArray)
        .setIsUAV(true);
    TextureHandle texture;
    OK(device->createTexture(td, &texture));
    SamplerHandle sampler;
    OK(device->createSampler(SamplerDesc().setAllFilters(false), &sampler));
    BufferHandle output, readback;
    OK(device->createBuffer(BufferDesc().setByteSize(32).setStructStride(16).setCanHaveUAVs(true), &output));
    OK(device->createBuffer(BufferDesc().setByteSize(64).setCpuAccess(CpuAccessMode::Read), &readback));
    BindingSetHandle bindings[2];
    for (int i = 0; i < 2; ++i)
        OK(device->createBindingSet(
            BindingSetDesc()
                .addItem(BindingSetItem::Texture_SRV(0, texture)
                             .setSubresources(i ? TextureSubresourceSet(0, 1, 2, 1)
                                                : TextureSubresourceSet(1, TextureSubresourceSet::AllMipLevels, 1, 1)))
                .addItem(BindingSetItem::Sampler(0, sampler))
                .addItem(BindingSetItem::StructuredBuffer_UAV(0, output)),
            layout, &bindings[i]));
    CommandListHandle list;
    OK(device->createCommandList(CommandListParameters().setQueueType(CommandQueue::Compute), &list));
    list->open();
    list->clearTextureFloat(texture, AllSubresources, Color(1, 2, 3, 4));
    list->clearTextureFloat(texture, TextureSubresourceSet(1, 1, 1, 1), Color(5, 6, 7, 8));
    list->clearTextureFloat(texture, TextureSubresourceSet(2, 1, 1, 1), Color(9, 10, 11, 12));
    for (int i = 0; i < 2; ++i)
    {
        list->setComputeState(ComputeState().setPipeline(pipeline).addBindingSet(bindings[i]));
        list->dispatch(1);
        list->copyBuffer(readback, i * 32, output, 0, 32);
    }
    list->close();
    CHECK(device->executeCommandList(list, CommandQueue::Compute));
    auto values = static_cast<const float *>(device->mapBuffer(readback, CpuAccessMode::Read));
    CHECK(values);
    for (int i = 0; i < 8; ++i)
        CHECK(values[i] == float(5 + i));
    for (int i = 8; i < 16; ++i)
        CHECK(values[i] == float(1 + i % 4));
    device->unmapBuffer(readback);
    std::puts("CUDA layered texture mip/layer views and float subresource clears passed");
    // Integer clear covers every 3D plane; half and normalized colors validate format conversion.
    const Format formats[] = {Format::R32_UINT, Format::RGBA16_FLOAT, Format::RGBA8_UNORM};
    for (Format format : formats)
    {
        td.setWidth(7)
            .setHeight(5)
            .setDepth(3)
            .setArraySize(1)
            .setMipLevels(1)
            .setDimension(TextureDimension::Texture3D)
            .setFormat(format)
            .setIsUAV(format != Format::RGBA8_UNORM)
            .setIsRenderTarget(format == Format::RGBA8_UNORM);
        TextureHandle cleared;
        StagingTextureHandle staging;
        OK(device->createTexture(td, &cleared));
        OK(device->createStagingTexture(td, CpuAccessMode::Read, &staging));
        list->open();
        if (format == Format::R32_UINT)
            list->clearTextureUInt(cleared, AllSubresources, 0x12345678);
        else
            list->clearTextureFloat(cleared, AllSubresources, Color(1, 0.5f, 0, 1));
        list->copyTexture2(staging, TextureSlice(), cleared, TextureSlice());
        list->close();
        CHECK(device->executeCommandList(list, CommandQueue::Compute));
        size_t pitch = 0;
        auto data = static_cast<const uint8_t *>(
            device->mapStagingTexture(staging, TextureSlice(), CpuAccessMode::Read, pitch));
        CHECK(data);
        for (size_t i = 0; i < 7 * 5 * 3; ++i)
        {
            if (format == Format::R32_UINT)
                CHECK(reinterpret_cast<const uint32_t *>(data)[i] == 0x12345678);
            else if (format == Format::RGBA16_FLOAT)
            {
                auto half = reinterpret_cast<const uint16_t *>(data) + i * 4;
                CHECK(half[0] == 0x3c00 && half[1] == 0x3800 && half[2] == 0 && half[3] == 0x3c00);
            }
            else
            {
                auto unorm = data + i * 4;
                CHECK(unorm[0] == 255 && unorm[1] == 128 && unorm[2] == 0 && unorm[3] == 255);
            }
        }
        device->unmapStagingTexture(staging);
    }
    std::puts("CUDA 3D unsigned, half-float and normalized texture clears passed");
    return 0;
}
