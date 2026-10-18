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
    if (argc < 3)
    {
        std::fprintf(stderr, "Usage: nvrhi_cuda_slang compute.nsta raytracing.nsta\n");
        return 1;
    }
    auto callback = MAKE_RC_OBJ_PTR(Messages);
    cuda::DeviceDesc d;
    d.errorCB = callback;
    d.enableOptiX = true;
    auto device = cuda::createDevice(d);
    if (!device)
        return 77;
    auto computeBytes = readFile(argv[1]);
    ShaderProgramHandle compute;
    OK(nvrhiCreateShaderProgram(device, computeBytes.data(), computeBytes.size(), &compute));
    auto cs = getShader(compute, ShaderType::Compute);
    CHECK(cs);
    constexpr uint32_t count = 259;
    struct Params
    {
        uint32_t count;
        float scale;
        uint32_t pad[2];
    } params{count, 2.5f, {0, 0}};
    std::vector<float> input(count);
    for (uint32_t i = 0; i < count; ++i)
        input[i] = float(i) * 0.5f;
    BufferHandle source, dest, constant, readback;
    OK(device->createBuffer(BufferDesc().setByteSize(count * 4).setStructStride(4), &source));
    OK(device->createBuffer(BufferDesc().setByteSize(count * 4).setStructStride(4).setCanHaveUAVs(true), &dest));
    OK(device->createBuffer(BufferDesc().setByteSize(sizeof(params)).setIsConstantBuffer(true), &constant));
    OK(device->createBuffer(BufferDesc().setByteSize(count * 4).setCpuAccess(CpuAccessMode::Read), &readback));
    BindingLayoutDesc ld;
    OK(compute->getBindingLayoutDesc(0, 0, ld));
    BindingLayoutHandle layout;
    OK(device->createBindingLayout(ld, &layout));
    BindingSetHandle set;
    OK(device->createBindingSet(BindingSetDesc()
                                    .addItem(BindingSetItem::ConstantBuffer(2, constant))
                                    .addItem(BindingSetItem::StructuredBuffer_SRV(3, source))
                                    .addItem(BindingSetItem::StructuredBuffer_UAV(4, dest)),
                                layout, &set));
    ComputePipelineHandle pipeline;
    OK(device->createComputePipeline(ComputePipelineDesc().setComputeShader(cs).addBindingLayout(layout), &pipeline));
    CommandListHandle list;
    OK(device->createCommandList(CommandListParameters().setQueueType(CommandQueue::Compute), &list));
    list->open();
    list->writeBuffer(source, input.data(), input.size() * 4);
    list->writeBuffer(constant, &params, sizeof(params));
    list->setComputeState(ComputeState().setPipeline(pipeline).addBindingSet(set));
    list->dispatch((count + 63) / 64);
    list->copyBuffer(readback, 0, dest, 0, count * 4);
    list->close();
    CHECK(device->executeCommandList(list, CommandQueue::Compute));
    auto output = static_cast<float *>(device->mapBuffer(readback, CpuAccessMode::Read));
    CHECK(output);
    for (uint32_t i = 0; i < count; ++i)
        CHECK(output[i] == input[i] * params.scale);
    device->unmapBuffer(readback);
    std::puts("Slang native CUDA constant block + CB + structured buffers passed");
    if (argc >= 4)
    {
        auto entryBytes = readFile(argv[3]);
        ShaderProgramHandle entryProgram;
        OK(nvrhiCreateShaderProgram(device, entryBytes.data(), entryBytes.size(), &entryProgram));
        auto entryShader = getShader(entryProgram, ShaderType::Compute);
        CHECK(entryShader);
        BindingLayoutHandle el;
        OK(device->createBindingLayout(
            BindingLayoutDesc().setVisibility(ShaderType::Compute).addItem(BindingLayoutItem::StructuredBuffer_UAV(0)),
            &el));
        BindingSetHandle es;
        OK(device->createBindingSet(BindingSetDesc().addItem(BindingSetItem::StructuredBuffer_UAV(0, dest)), el, &es));
        ComputePipelineHandle ep;
        OK(device->createComputePipeline(ComputePipelineDesc().setComputeShader(entryShader).addBindingLayout(el),
                                         &ep));
        struct EntryData
        {
            uint32_t factor, padding;
            uint64_t bias;
        } data{13, 0, uint64_t(29) << 32};
        list->open();
        list->setComputeState(ComputeState().setPipeline(ep).addBindingSet(es));
        list->setPushConstants(&data, sizeof(data));
        list->dispatch(1);
        list->copyBuffer(readback, 0, dest, 0, 16);
        list->close();
        CHECK(device->executeCommandList(list, CommandQueue::Compute));
        auto values = static_cast<uint32_t *>(device->mapBuffer(readback, CpuAccessMode::Read));
        CHECK(values);
        for (uint32_t i = 0; i < 4; ++i)
            CHECK(values[i] == 42 + i);
        device->unmapBuffer(readback);
        std::puts("Slang mixed 32/64-bit packed CUDA entry parameters passed");
    }
    if (argc >= 5)
    {
        auto textureBytes = readFile(argv[4]);
        ShaderProgramHandle textureProgram;
        OK(nvrhiCreateShaderProgram(device, textureBytes.data(), textureBytes.size(), &textureProgram));
        auto textureShader = getShader(textureProgram, ShaderType::Compute);
        CHECK(textureShader);
        TextureDesc td;
        td.width = 2;
        td.height = 1;
        td.format = Format::RGBA32_FLOAT;
        TextureHandle texture;
        OK(device->createTexture(td, &texture));
        SamplerHandle point, linear;
        OK(device->createSampler(SamplerDesc().setAllFilters(false), &point));
        OK(device->createSampler(SamplerDesc().setAllFilters(true), &linear));
        BindingLayoutDesc tlDesc;
        OK(textureProgram->getBindingLayoutDesc(0, 0, tlDesc));
        BindingLayoutHandle tl;
        OK(device->createBindingLayout(tlDesc, &tl));
        BufferHandle textureResult;
        OK(device->createBuffer(BufferDesc().setByteSize(32).setStructStride(16).setCanHaveUAVs(true), &textureResult));
        BindingSetHandle ts;
        OK(device->createBindingSet(BindingSetDesc()
                                        .addItem(BindingSetItem::Texture_SRV(0, texture))
                                        .addItem(BindingSetItem::Texture_SRV(1, texture))
                                        .addItem(BindingSetItem::Sampler(0, point))
                                        .addItem(BindingSetItem::Sampler(1, linear))
                                        .addItem(BindingSetItem::StructuredBuffer_UAV(0, textureResult)),
                                    tl, &ts));
        ComputePipelineHandle tp;
        OK(device->createComputePipeline(ComputePipelineDesc().setComputeShader(textureShader).addBindingLayout(tl),
                                         &tp));
        const float texels[] = {1, 0, 0, 1, 0, 0, 1, 1};
        list->open();
        list->writeTexture(texture, 0, 0, texels, sizeof(texels));
        list->setComputeState(ComputeState().setPipeline(tp).addBindingSet(ts));
        list->dispatch(1);
        list->copyBuffer(readback, 0, textureResult, 0, 32);
        list->close();
        CHECK(device->executeCommandList(list, CommandQueue::Compute));
        auto values = static_cast<float *>(device->mapBuffer(readback, CpuAccessMode::Read));
        CHECK(values);
        CHECK(values[0] == 0 && values[2] == 1 && values[3] == 1);
        CHECK(std::abs(values[4] - 0.5f) < 1e-6f && std::abs(values[6] - 0.5f) < 1e-6f && values[7] == 1);
        device->unmapBuffer(readback);
        std::puts("Slang CUDA same-resource point/linear sampler pairings passed");
    }
    if (!device->queryFeatureSupport(Feature::RayTracingPipeline))
        return 77;
    auto rayBytes = readFile(argv[2]);
    ShaderProgramHandle ray;
    OK(nvrhiCreateShaderProgram(device, rayBytes.data(), rayBytes.size(), &ray));
    auto rg = getShader(ray, ShaderType::RayGeneration), ms = getShader(ray, ShaderType::Miss),
         ch = getShader(ray, ShaderType::ClosestHit);
    CHECK(rg && ms && ch);
    const float vertices[] = {-1.f, -1.f, 0.f, 1.f, -1.f, 0.f, 0.f, 1.f, 0.f};
    BufferHandle vb;
    OK(device->createBuffer(BufferDesc().setByteSize(sizeof(vertices)).setIsAccelStructBuildInput(true), &vb));
    rt::GeometryDesc geometry;
    geometry.setTriangles(rt::GeometryTriangles()
                              .setVertexBuffer(vb)
                              .setVertexFormat(Format::RGB32_FLOAT)
                              .setVertexCount(3)
                              .setVertexStride(12));
    geometry.flags = rt::GeometryFlags::Opaque;
    rt::AccelStructHandle blas, tlas;
    OK(device->createAccelStruct(rt::AccelStructDesc().addBottomLevelGeometry(geometry).setBuildFlags(
                                     rt::AccelStructBuildFlags::AllowCompaction),
                                 &blas));
    OK(device->createAccelStruct(
        rt::AccelStructDesc().setTopLevelMaxInstances(1).setBuildFlags(rt::AccelStructBuildFlags::AllowUpdate), &tlas));
    rt::InstanceDesc instance;
    instance.setBLAS(blas).setInstanceMask(255);
    BindingLayoutHandle rayLayout;
    OK(device->createBindingLayout(BindingLayoutDesc()
                                       .setVisibility(ShaderType::AllRayTracing)
                                       .addItem(BindingLayoutItem::RayTracingAccelStruct(0))
                                       .addItem(BindingLayoutItem::StructuredBuffer_UAV(0)),
                                   &rayLayout));
    BindingSetHandle rayBindings;
    OK(device->createBindingSet(BindingSetDesc()
                                    .addItem(BindingSetItem::RayTracingAccelStruct(0, tlas))
                                    .addItem(BindingSetItem::StructuredBuffer_UAV(0, dest)),
                                rayLayout, &rayBindings));
    rt::PipelineHandle rayPipeline;
    OK(device->createRayTracingPipeline(
        rt::PipelineDesc()
            .addShader(rt::PipelineShaderDesc().setExportName("raygen").setShader(rg))
            .addShader(rt::PipelineShaderDesc().setExportName("miss").setShader(ms))
            .addHitGroup(rt::PipelineHitGroupDesc().setExportName("hit").setClosestHitShader(ch))
            .addBindingLayout(rayLayout)
            .setMaxPayloadSize(4)
            .setMaxAttributeSize(8),
        &rayPipeline));
    rt::ShaderTableHandle table;
    OK(rayPipeline->createShaderTable(rt::ShaderTableDesc(), &table));
    table->setRayGenerationShader("raygen");
    CHECK(table->addMissShader("miss") == 0);
    CHECK(table->addHitGroup("hit") == 0);
    list->open();
    list->writeBuffer(vb, vertices, sizeof(vertices));
    list->buildBottomLevelAccelStruct(blas, &geometry, 1);
    list->buildTopLevelAccelStruct(tlas, &instance, 1);
    list->setRayTracingState(rt::State().setShaderTable(table).addBindingSet(rayBindings));
    list->dispatchRays(rt::DispatchRaysArguments().setWidth(count));
    list->copyBuffer(readback, 0, dest, 0, count * 4);
    list->close();
    CHECK(device->executeCommandList(list, CommandQueue::Compute));
    auto hits = static_cast<uint32_t *>(device->mapBuffer(readback, CpuAccessMode::Read));
    CHECK(hits);
    for (uint32_t i = 0; i < count; ++i)
        CHECK(hits[i] == 42);
    device->unmapBuffer(readback);
    // Update the instance transform so all primary rays miss the triangle.
    instance.transform[3] = 10.f;
    list->open();
    list->buildTopLevelAccelStruct(tlas, &instance, 1, rt::AccelStructBuildFlags::PerformUpdate);
    list->setRayTracingState(rt::State().setShaderTable(table).addBindingSet(rayBindings));
    list->dispatchRays(rt::DispatchRaysArguments().setWidth(count));
    list->copyBuffer(readback, 0, dest, 0, count * 4);
    list->close();
    CHECK(device->executeCommandList(list, CommandQueue::Compute));
    hits = static_cast<uint32_t *>(device->mapBuffer(readback, CpuAccessMode::Read));
    CHECK(hits);
    for (uint32_t i = 0; i < count; ++i)
        CHECK(hits[i] == 7);
    device->unmapBuffer(readback);
    std::puts("Slang OptiX triangle trace, payload, miss, BLAS compaction and TLAS update passed");
    auto intersection = getShader(ray, ShaderType::Intersection);
    if (intersection)
    {
        rt::GeometryAABB bounds{-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};
        BufferHandle boxes;
        OK(device->createBuffer(BufferDesc().setByteSize(sizeof(bounds)).setIsAccelStructBuildInput(true), &boxes));
        rt::GeometryDesc custom;
        custom.setAABBs(rt::GeometryAABBs().setBuffer(boxes).setCount(1).setStride(sizeof(bounds)));
        rt::AccelStructHandle customBlas;
        OK(device->createAccelStruct(
            rt::AccelStructDesc().addBottomLevelGeometry(custom).setBuildFlags(rt::AccelStructBuildFlags::AllowUpdate),
            &customBlas));
        rt::PipelineHandle customPipeline;
        OK(device->createRayTracingPipeline(
            rt::PipelineDesc()
                .addShader(rt::PipelineShaderDesc().setExportName("raygen").setShader(rg))
                .addShader(rt::PipelineShaderDesc().setExportName("miss").setShader(ms))
                .addHitGroup(rt::PipelineHitGroupDesc()
                                 .setExportName("hit")
                                 .setClosestHitShader(ch)
                                 .setIntersectionShader(intersection)
                                 .setIsProceduralPrimitive(true))
                .addBindingLayout(rayLayout)
                .setMaxPayloadSize(4)
                .setMaxAttributeSize(8),
            &customPipeline));
        rt::ShaderTableHandle customTable;
        OK(customPipeline->createShaderTable(rt::ShaderTableDesc(), &customTable));
        customTable->setRayGenerationShader("raygen");
        CHECK(customTable->addMissShader("miss") == 0);
        CHECK(customTable->addHitGroup("hit") == 0);
        instance.setTransform(rt::c_IdentityTransform).setBLAS(customBlas);
        list->open();
        list->writeBuffer(boxes, &bounds, sizeof(bounds));
        list->buildBottomLevelAccelStruct(customBlas, &custom, 1);
        list->buildTopLevelAccelStruct(tlas, &instance, 1);
        list->setRayTracingState(rt::State().setShaderTable(customTable).addBindingSet(rayBindings));
        list->dispatchRays(rt::DispatchRaysArguments().setWidth(count));
        list->copyBuffer(readback, 0, dest, 0, count * 4);
        list->close();
        CHECK(device->executeCommandList(list, CommandQueue::Compute));
        hits = static_cast<uint32_t *>(device->mapBuffer(readback, CpuAccessMode::Read));
        CHECK(hits);
        for (uint32_t i = 0; i < count; ++i)
            CHECK(hits[i] == 42);
        device->unmapBuffer(readback);
        bounds.minX += 10.f;
        bounds.maxX += 10.f;
        list->open();
        list->writeBuffer(boxes, &bounds, sizeof(bounds));
        list->buildBottomLevelAccelStruct(customBlas, &custom, 1, rt::AccelStructBuildFlags::PerformUpdate);
        list->buildTopLevelAccelStruct(tlas, &instance, 1);
        list->setRayTracingState(rt::State().setShaderTable(customTable).addBindingSet(rayBindings));
        list->dispatchRays(rt::DispatchRaysArguments().setWidth(count));
        list->copyBuffer(readback, 0, dest, 0, count * 4);
        list->close();
        CHECK(device->executeCommandList(list, CommandQueue::Compute));
        hits = static_cast<uint32_t *>(device->mapBuffer(readback, CpuAccessMode::Read));
        CHECK(hits);
        for (uint32_t i = 0; i < count; ++i)
            CHECK(hits[i] == 7);
        device->unmapBuffer(readback);
        std::puts("Slang OptiX procedural AABB intersection and BLAS update passed");
    }
    return 0;
}
