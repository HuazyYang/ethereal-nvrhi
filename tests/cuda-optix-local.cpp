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
    cuda::DeviceDesc deviceDesc;
    deviceDesc.errorCB = callback;
    auto device = cuda::createDevice(deviceDesc);
    if (!device || !device->queryFeatureSupport(Feature::RayTracingPipeline))
        return 77;
    auto bytes = readFile(argv[1]);
    ShaderProgramHandle program;
    OK(nvrhiCreateShaderProgram(device, bytes.data(), bytes.size(), &program));
    auto raygen = getShader(program, ShaderType::RayGeneration);
    auto miss = getShader(program, ShaderType::Miss);
    auto hit = getShader(program, ShaderType::ClosestHit);
    CHECK(raygen && miss && hit);
    BindingLayoutHandle globals, local;
    OK(device->createBindingLayout(BindingLayoutDesc()
                                       .setVisibility(ShaderType::AllRayTracing)
                                       .addItem(BindingLayoutItem::RayTracingAccelStruct(0))
                                       .addItem(BindingLayoutItem::StructuredBuffer_UAV(0)),
                                   &globals));
    OK(device->createBindingLayout(
        BindingLayoutDesc().setVisibility(ShaderType::AllRayTracing).addItem(BindingLayoutItem::ConstantBuffer(0)),
        &local));
    rt::PipelineHandle pipeline;
    OK(device->createRayTracingPipeline(
        rt::PipelineDesc()
            .addShader(rt::PipelineShaderDesc().setExportName("raygen").setShader(raygen).setBindingLayout(local))
            .addShader(rt::PipelineShaderDesc().setExportName("miss").setShader(miss).setBindingLayout(local))
            .addHitGroup(
                rt::PipelineHitGroupDesc().setExportName("material").setClosestHitShader(hit).setBindingLayout(local))
            .addBindingLayout(globals)
            .setMaxPayloadSize(4)
            .setMaxAttributeSize(8),
        &pipeline));
    BufferHandle vertexBuffer, output, readback;
    const float vertices[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    OK(device->createBuffer(BufferDesc().setByteSize(sizeof(vertices)).setIsAccelStructBuildInput(true),
                            &vertexBuffer));
    OK(device->createBuffer(BufferDesc().setByteSize(20).setStructStride(4).setCanHaveUAVs(true), &output));
    OK(device->createBuffer(BufferDesc().setByteSize(40).setCpuAccess(CpuAccessMode::Read), &readback));
    rt::GeometryDesc geometry[2];
    for (int i = 0; i < 2; ++i)
    {
        geometry[i]
            .setTriangles(rt::GeometryTriangles()
                              .setVertexBuffer(vertexBuffer)
                              .setVertexFormat(Format::RGB32_FLOAT)
                              .setVertexCount(3)
                              .setVertexStride(12))
            .setTransform(rt::c_IdentityTransform)
            .setFlags(rt::GeometryFlags::Opaque);
        geometry[i].transform[3] = i ? 2.f : -2.f;
    }
    auto bottomDesc = rt::AccelStructDesc()
                          .addBottomLevelGeometry(geometry[0])
                          .addBottomLevelGeometry(geometry[1])
                          .setBuildFlags(rt::AccelStructBuildFlags::AllowUpdate);
    auto topDesc = rt::AccelStructDesc().setTopLevelMaxInstances(2);
    rt::AccelStructHandle sourceBlas, clonedBlas, sourceTlas, clonedTlas;
    OK(device->createAccelStruct(bottomDesc, &sourceBlas));
    OK(device->createAccelStruct(bottomDesc, &clonedBlas));
    OK(device->createAccelStruct(topDesc, &sourceTlas));
    OK(device->createAccelStruct(topDesc, &clonedTlas));
    rt::InstanceDesc instances[2];
    instances[0].setBLAS(clonedBlas).setInstanceMask(255);
    instances[1].setBLAS(clonedBlas).setInstanceMask(255);
    instances[1].transform[3] = 8.f;
    instances[1].instanceContributionToHitGroupIndex = 2;
    BindingSetHandle globalSet;
    OK(device->createBindingSet(BindingSetDesc()
                                    .addItem(BindingSetItem::RayTracingAccelStruct(0, clonedTlas))
                                    .addItem(BindingSetItem::StructuredBuffer_UAV(0, output)),
                                globals, &globalSet));
    rt::ShaderTableHandle table;
    OK(pipeline->createShaderTable(rt::ShaderTableDesc(), &table));
    // Missing and incompatible local bindings are rejected before an SBT record is appended.
    CHECK(table->addHitGroup("material") == -1);
    CHECK(table->addHitGroup("material", globalSet) == -1);
    CommandListHandle list;
    OK(device->createCommandList(CommandListParameters().setQueueType(CommandQueue::Compute), &list));
    list->open();
    std::vector<BufferHandle> constants;
    std::vector<BindingSetHandle> locals;
    const uint32_t values[] = {11, 22, 33, 44, 7};
    for (uint32_t value : values)
    {
        BufferHandle buffer;
        OK(device->createBuffer(BufferDesc().setByteSize(16).setIsConstantBuffer(true), &buffer));
        const uint32_t data[] = {value, 0, 0, 0};
        list->writeBuffer(buffer, data, sizeof(data));
        BindingSetHandle bindings;
        OK(device->createBindingSet(BindingSetDesc().addItem(BindingSetItem::ConstantBuffer(0, buffer)), local,
                                    &bindings));
        constants.push_back(buffer);
        locals.push_back(bindings);
    }
    BufferHandle settings;
    OK(device->createBuffer(BufferDesc().setByteSize(16).setIsConstantBuffer(true), &settings));
    const float settingsData[] = {-2, 4, 0, 0};
    list->writeBuffer(settings, settingsData, sizeof(settingsData));
    BindingSetHandle settingsSet;
    OK(device->createBindingSet(BindingSetDesc().addItem(BindingSetItem::ConstantBuffer(0, settings)), local,
                                &settingsSet));
    table->setRayGenerationShader("raygen", settingsSet);
    CHECK(table->addMissShader("miss", locals[4]) == 0);
    for (int i = 0; i < 4; ++i)
        CHECK(table->addHitGroup("material", locals[i]) == i);
    list->writeBuffer(vertexBuffer, vertices, sizeof(vertices));
    list->buildBottomLevelAccelStruct(sourceBlas, geometry, 2);
    list->copyRaytracingAccelerationStructure(clonedBlas, sourceBlas);
    list->buildTopLevelAccelStruct(sourceTlas, instances, 2);
    list->copyRaytracingAccelerationStructure(clonedTlas, sourceTlas);
    list->setRayTracingState(rt::State().setShaderTable(table).addBindingSet(globalSet));
    list->dispatchRays(rt::DispatchRaysArguments().setWidth(5));
    list->copyBuffer(readback, 0, output, 0, 20);
    list->close();
    // Dispatch snapshots the table and retains all local resources until GPU completion.
    table->clearHitShaders();
    table->clearMissShaders();
    CHECK(device->executeCommandList(list, CommandQueue::Compute));
    auto result = static_cast<const uint32_t *>(device->mapBuffer(readback, CpuAccessMode::Read));
    CHECK(result);
    for (int i = 0; i < 5; ++i)
        CHECK(result[i] == values[i]);
    device->unmapBuffer(readback);
    std::puts("OptiX multi-geometry pretransforms, per-instance SBT offsets, raygen/miss/hit local material bindings "
              "and BLAS/TLAS clones passed");
    BindingSetHandle sourceGlobalSet;
    OK(device->createBindingSet(BindingSetDesc()
                                    .addItem(BindingSetItem::RayTracingAccelStruct(0, sourceTlas))
                                    .addItem(BindingSetItem::StructuredBuffer_UAV(0, output)),
                                globals, &sourceGlobalSet));
    // Updating the source geometry must not alter either clone's acceleration data.
    for (auto &g : geometry)
        g.transform[3] += 20.f;
    list->open();
    list->buildBottomLevelAccelStruct(sourceBlas, geometry, 2, rt::AccelStructBuildFlags::PerformUpdate);
    for (int i = 0; i < 4; ++i)
        CHECK(table->addHitGroup("material", locals[i]) == i);
    CHECK(table->addMissShader("miss", locals[4]) == 0);
    list->setRayTracingState(rt::State().setShaderTable(table).addBindingSet(globalSet));
    list->dispatchRays(rt::DispatchRaysArguments().setWidth(5));
    list->copyBuffer(readback, 0, output, 0, 20);
    instances[0].setBLAS(sourceBlas);
    instances[1].setBLAS(sourceBlas);
    list->buildTopLevelAccelStruct(sourceTlas, instances, 2);
    list->setRayTracingState(rt::State().setShaderTable(table).addBindingSet(sourceGlobalSet));
    list->dispatchRays(rt::DispatchRaysArguments().setWidth(5));
    list->copyBuffer(readback, 20, output, 0, 20);
    list->close();
    CHECK(device->executeCommandList(list, CommandQueue::Compute));
    list->open();
    sourceGlobalSet = nullptr;
    sourceBlas = nullptr;
    sourceTlas = nullptr;
    clonedBlas = nullptr;
    table = nullptr;
    locals.clear();
    constants.clear();
    settings = nullptr;
    settingsSet = nullptr;
    vertexBuffer = nullptr;
    result = static_cast<const uint32_t *>(device->mapBuffer(readback, CpuAccessMode::Read));
    CHECK(result);
    for (int i = 0; i < 5; ++i)
        CHECK(result[i] == values[i]);
    for (int i = 5; i < 10; ++i)
        CHECK(result[i] == 7);
    device->unmapBuffer(readback);
    std::puts("OptiX clone independence, transform update, SBT snapshot and submission lifetime passed");
    return 0;
}
