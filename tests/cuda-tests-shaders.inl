// The tests of nvrhi_cuda_tests that run Slang code: the fixtures are NVRHI shader containers compiled by ShaderTool
// from tests/shaders/*.slang (tests/CMakeLists.txt). Included by cuda-tests.cpp.

namespace
{
    std::vector<uint8_t> loadFixture(const std::string& directory, const char* name)
    {
        check(!directory.empty(), "the test needs the fixture directory as its second argument");
        const std::string path = directory + "/" + name;
        std::ifstream file(path, std::ios::binary);
        check(file.good(), "cannot open " + path);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    struct Float4
    {
        float x, y, z, w;
    };

    bool nearlyEqual(const Float4& a, const Float4& b)
    {
        auto close = [](float u, float v) { return std::abs(u - v) <= 1e-4f * std::max(1.f, std::abs(v)); };
        return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z) && close(a.w, b.w);
    }

    std::string toString(const Float4& v)
    {
        char text[128];
        std::snprintf(text, sizeof(text), "(%g, %g, %g, %g)", v.x, v.y, v.z, v.w);
        return text;
    }

    const nvrhi::ShaderParameterReflection& requireParameter(const nvrhi::ShaderLibraryReflection* reflection, const char* name,
        nvrhi::ShaderParameterKind kind)
    {
        const nvrhi::ShaderParameterReflection* parameter = nvrhi::findShaderParameter(reflection, name);
        check(parameter != nullptr, std::string("reflection: parameter ") + name);
        check(parameter->kind == kind, std::string("reflection: kind of ") + name);
        return *parameter;
    }

    nvrhi::BufferHandle createStructuredBuffer(nvrhi::IDevice* device, uint32_t count, uint32_t stride, bool uav, const char* name)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = uint64_t(count) * stride;
        desc.structStride = stride;
        desc.canHaveUAVs = uav;
        desc.debugName = name;
        desc.initialState = uav ? nvrhi::ResourceStates::UnorderedAccess : nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        nvrhi::BufferHandle buffer;
        device->createBuffer(desc, &buffer);
        check(buffer != nullptr, std::string("create ") + name);
        return buffer;
    }
}

static void testDispatch(const std::string& fixtures)
{
    const std::vector<uint8_t> container = loadFixture(fixtures, "cuda-dispatch.nvsc");
    nvrhi::cuda::DeviceHandle device = createCudaDevice();

    nvrhi::ShaderLibraryHandle library;
    device->createShaderLibrary(container.data(), container.size(), &library);
    check(library != nullptr, "create the shader library");
    const nvrhi::ShaderLibraryReflection* reflection = library->getReflection();
    check(reflection && reflection->target == nvrhi::ShaderCodeTarget::CudaPTX, "the library reflects CUDA PTX");

    nvrhi::ShaderHandle mainShader, writeScale;
    library->getShader("main", nvrhi::ShaderType::Compute, &mainShader);
    library->getShader("writeScale", nvrhi::ShaderType::Compute, &writeScale);
    check(mainShader && writeScale, "entry points main and writeScale");
    check(mainShader->getReflection() && mainShader->getReflection()->threadGroupSize[0] == 64, "numthreads(64, 1, 1)");
    check(mainShader->getLibraryReflection() == reflection, "a shader of a library shares its reflection");

    // Every binding kind of the fixture, in register space 0.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2).setSize(2))
        .addItem(nvrhi::BindingLayoutItem::Sampler(1))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::RawBuffer_UAV(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(2))
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, 8));
    nvrhi::BindingLayoutHandle layout;
    device->createBindingLayout(layoutDesc, &layout);

    constexpr uint32_t count = 100;
    struct Constants
    {
        float scale[4];
        uint32_t count;
        uint32_t padding[3];
    } constants = { { 2.f, 3.f, 4.f, 5.f }, count, {} };

    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(Constants);
    cbDesc.isConstantBuffer = true;
    cbDesc.debugName = "constants";
    cbDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    cbDesc.keepInitialState = true;
    nvrhi::BufferHandle constantBuffer;
    device->createBuffer(cbDesc, &constantBuffer);

    nvrhi::BufferHandle input = createStructuredBuffer(device, count, 16, false, "input");
    nvrhi::BufferHandle output = createStructuredBuffer(device, count, 16, true, "output");
    nvrhi::BufferHandle counter = createBuffer(device, 16, "counter");

    std::vector<Float4> inputData(count);
    for (uint32_t i = 0; i < count; ++i)
        inputData[i] = { float(i), float(i) * 0.5f, -float(i), 1.f };

    // A 4x4 texture of known texels, two 2x1x1 volumes.
    nvrhi::TextureDesc textureDesc;
    textureDesc.width = 4;
    textureDesc.height = 4;
    textureDesc.format = nvrhi::Format::RGBA32_FLOAT;
    textureDesc.debugName = "texture";
    nvrhi::TextureHandle texture;
    device->createTexture(textureDesc, &texture);
    std::vector<Float4> texels(16);
    for (uint32_t i = 0; i < 16; ++i)
        texels[i] = { float(i) * 10.f, 1.f, 0.f, 0.25f };

    nvrhi::TextureDesc volumeDesc;
    volumeDesc.dimension = nvrhi::TextureDimension::Texture3D;
    volumeDesc.width = 2;
    volumeDesc.height = 1;
    volumeDesc.depth = 1;
    volumeDesc.format = nvrhi::Format::R32_FLOAT;
    nvrhi::TextureHandle volumes[2];
    const float volumeData[2][2] = { { 100.f, 200.f }, { 1000.f, 2000.f } };
    for (int v = 0; v < 2; ++v)
        device->createTexture(volumeDesc, &volumes[v]);

    nvrhi::TextureDesc surfaceDesc = textureDesc;
    surfaceDesc.isUAV = true;
    surfaceDesc.debugName = "surface";
    nvrhi::TextureHandle surface;
    device->createTexture(surfaceDesc, &surface);

    nvrhi::SamplerHandle sampler;
    device->createSampler(nvrhi::SamplerDesc().setAllFilters(false), &sampler);

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constantBuffer))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, input))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, texture))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(2, volumes[0]).setArrayElement(0))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(2, volumes[1]).setArrayElement(1))
        .addItem(nvrhi::BindingSetItem::Sampler(1, sampler))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, output))
        .addItem(nvrhi::BindingSetItem::RawBuffer_UAV(1, counter))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(2, surface))
        .addItem(nvrhi::BindingSetItem::PushConstants(0, 8));
    nvrhi::BindingSetHandle set;
    device->createBindingSet(setDesc, layout, &set);
    check(set != nullptr, "create the binding set");

    nvrhi::ComputePipelineHandle pipeline;
    device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(mainShader).addBindingLayout(layout), &pipeline);
    check(pipeline != nullptr, "create the compute pipeline");

    struct PushConstants
    {
        float bias;
        uint32_t offset;
    } pushConstants = { 0.5f, 7 };

    nvrhi::CommandListHandle commandList = createCommandList(device);
    commandList->open();
    commandList->writeBuffer(constantBuffer, &constants, sizeof(constants), 0);
    commandList->writeBuffer(input, inputData.data(), inputData.size() * sizeof(Float4), 0);
    commandList->writeTexture(texture, 0, 0, texels.data(), 4 * sizeof(Float4), 0);
    for (int v = 0; v < 2; ++v)
        commandList->writeTexture(volumes[v], 0, 0, volumeData[v], sizeof(volumeData[v]), sizeof(volumeData[v]));
    commandList->clearBufferUInt(counter, 0);
    commandList->setComputeState(nvrhi::ComputeState().setPipeline(pipeline).addBindingSet(set));
    commandList->setPushConstants(&pushConstants, sizeof(pushConstants));
    commandList->dispatch((count + 63) / 64);
    commandList->close();
    executeAndWait(device, commandList);

    const std::vector<uint8_t> outputBytes = readBuffer(device, output, count * sizeof(Float4));
    const Float4* results = reinterpret_cast<const Float4*>(outputBytes.data());
    for (uint32_t i = 0; i < count; ++i)
    {
        const Float4& in = inputData[i];
        const Float4& texel = texels[(i % 4) + ((i / 4) % 4) * 4];
        const Float4& sampled = texels[0];
        const float volume = volumeData[1][i % 2];
        const Float4 expected = {
            in.x * 2.f + 0.5f + texel.x + sampled.x + volume,
            in.y * 3.f + 0.5f + texel.y + sampled.y + volume,
            in.z * 4.f + 0.5f + texel.z + sampled.z + volume,
            in.w * 5.f + 0.5f + texel.w + sampled.w + volume };
        check(nearlyEqual(results[i], expected), "dispatch output " + std::to_string(i) + ": " + toString(results[i]) +
            ", expected " + toString(expected));
    }
    const std::vector<uint8_t> counterBytes = readBuffer(device, counter, 4);
    check(*reinterpret_cast<const uint32_t*>(counterBytes.data()) == count, "the atomic counter counted every thread");

    // The surface writes, through a staging texture.
    nvrhi::StagingTextureHandle staging;
    nvrhi::TextureDesc stagingDesc = textureDesc;
    device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read, &staging);
    commandList->open();
    commandList->copyTexture2(staging, nvrhi::TextureSlice(), surface, nvrhi::TextureSlice());
    commandList->close();
    executeAndWait(device, commandList);
    size_t rowPitch = 0;
    const uint8_t* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(),
        nvrhi::CpuAccessMode::Read, rowPitch));
    check(mapped != nullptr, "map the surface read-back");
    for (uint32_t i = 0; i < 16; ++i)
    {
        const Float4& value = *reinterpret_cast<const Float4*>(mapped + (i / 4) * rowPitch + (i % 4) * sizeof(Float4));
        check(nearlyEqual(value, { float(i), 7.f, 0.f, 1.f }), "surface texel " + std::to_string(i) + ": " + toString(value));
    }
    device->unmapStagingTexture(staging);
    std::printf("dispatch: constant, structured, raw buffers, textures with a sampler, a texture array, a surface, "
        "push constants OK\n");

    // A volatile constant buffer: every dispatch sees the version written before it; on two queues.
    nvrhi::BufferDesc volatileDesc = cbDesc;
    volatileDesc.isVolatile = true;
    volatileDesc.maxVersions = 16;
    volatileDesc.debugName = "volatile constants";
    nvrhi::BufferHandle volatileBuffer;
    device->createBuffer(volatileDesc, &volatileBuffer);

    nvrhi::BindingLayoutDesc volatileLayoutDesc;
    volatileLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    volatileLayoutDesc.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, 4));
    nvrhi::BindingLayoutHandle volatileLayout;
    device->createBindingLayout(volatileLayoutDesc, &volatileLayout);
    nvrhi::BindingSetHandle volatileSet;
    device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, volatileBuffer))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, output))
        .addItem(nvrhi::BindingSetItem::PushConstants(0, 4)), volatileLayout, &volatileSet);
    nvrhi::ComputePipelineHandle scalePipeline;
    device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(writeScale).addBindingLayout(volatileLayout), &scalePipeline);
    check(volatileSet && scalePipeline, "the volatile constant buffer pipeline");

    const Constants versions[3] = { { { 1, 2, 3, 4 }, 0, {} }, { { 5, 6, 7, 8 }, 0, {} }, { { 9, 10, 11, 12 }, 0, {} } };
    commandList->open();
    for (uint32_t index = 0; index < 2; ++index)
    {
        commandList->writeBuffer(volatileBuffer, &versions[index], sizeof(Constants), 0);
        commandList->setComputeState(nvrhi::ComputeState().setPipeline(scalePipeline).addBindingSet(volatileSet));
        commandList->setPushConstants(&index, sizeof(index));
        commandList->dispatch(1);
    }
    commandList->close();
    executeAndWait(device, commandList);

    nvrhi::CommandListHandle computeList = createCommandList(device, nvrhi::CommandQueue::Compute);
    computeList->open();
    computeList->writeBuffer(volatileBuffer, &versions[2], sizeof(Constants), 0);
    computeList->setComputeState(nvrhi::ComputeState().setPipeline(scalePipeline).addBindingSet(volatileSet));
    const uint32_t computeIndex = 2;
    computeList->setPushConstants(&computeIndex, sizeof(computeIndex));
    computeList->dispatch(1);
    computeList->close();
    executeAndWait(device, computeList, nvrhi::CommandQueue::Compute);

    const std::vector<uint8_t> scaleBytes = readBuffer(device, output, 3 * sizeof(Float4));
    const Float4* scales = reinterpret_cast<const Float4*>(scaleBytes.data());
    for (uint32_t index = 0; index < 3; ++index)
    {
        const Float4 expected = { versions[index].scale[0], versions[index].scale[1], versions[index].scale[2], versions[index].scale[3] };
        check(nearlyEqual(scales[index], expected), "volatile constant buffer version " + std::to_string(index) + ": " +
            toString(scales[index]));
    }
    std::printf("dispatch: volatile constant buffer versions, the same kernel on the graphics and compute queues OK\n");
}

static void testHandles(const std::string& fixtures)
{
    const std::vector<uint8_t> container = loadFixture(fixtures, "cuda-handles.nvsc");
    nvrhi::cuda::DeviceHandle device = createCudaDevice();

    nvrhi::ShaderHandle shader;
    device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Compute).setEntryName("main"),
        container.data(), container.size(), &shader);
    check(shader != nullptr, "create the shader");
    const nvrhi::ShaderEntryPointReflection* entryPoint = shader->getReflection();
    const nvrhi::ShaderLibraryReflection* library = shader->getLibraryReflection();
    check(entryPoint && library && entryPoint->paramsSize == 24, "the kernel takes 24 bytes of arguments");

    // A 4x4x4 volume of texels x + 10 y + 100 z, and two volumes written through surfaces.
    constexpr uint32_t n = 4;
    nvrhi::TextureDesc volumeDesc;
    volumeDesc.dimension = nvrhi::TextureDimension::Texture3D;
    volumeDesc.width = volumeDesc.height = volumeDesc.depth = n;
    volumeDesc.format = nvrhi::Format::R32_FLOAT;
    volumeDesc.debugName = "volume";
    nvrhi::TextureHandle volume;
    device->createTexture(volumeDesc, &volume);
    std::vector<float> texels(n * n * n);
    for (uint32_t i = 0; i < n * n * n; ++i)
        texels[i] = float(i % n) + 10.f * float((i / n) % n) + 100.f * float(i / (n * n));

    nvrhi::TextureDesc floatsDesc = volumeDesc;
    floatsDesc.isUAV = true;
    floatsDesc.debugName = "floats";
    nvrhi::TextureDesc bytesDesc = floatsDesc;
    bytesDesc.format = nvrhi::Format::RGBA8_UINT;
    bytesDesc.debugName = "bytes";
    nvrhi::TextureHandle floats, bytes;
    device->createTexture(floatsDesc, &floats);
    device->createTexture(bytesDesc, &bytes);
    check(volume && floats && bytes, "create the volumes");

    nvrhi::SamplerHandle pointSampler, linearSampler;
    device->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp), &pointSampler);
    device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp), &linearSampler);

    const nvrhi::TextureSubresourceSet all = nvrhi::AllSubresources;
    const uint64_t pointHandle = device->getTextureHandle(volume, pointSampler, all, nvrhi::Format::UNKNOWN);
    const uint64_t linearHandle = device->getTextureHandle(volume, linearSampler, all, nvrhi::Format::UNKNOWN);
    check(pointHandle && linearHandle && pointHandle != linearHandle, "a texture handle per sampler");
    check(device->getTextureHandle(volume, pointSampler, all, nvrhi::Format::UNKNOWN) == pointHandle,
        "the same texture and sampler give the same handle");
    const uint64_t handles[3] = { linearHandle, device->getSurfaceHandle(floats, 0), device->getSurfaceHandle(bytes, 0) };
    check(handles[1] && handles[2], "surface handles of UAV textures");

    nvrhi::BufferHandle handleBuffer = createBuffer(device, sizeof(handles), "handles");
    nvrhi::BufferHandle output = createBuffer(device, n * n * n * 2 * sizeof(float), "output");

    // The kernel arguments, placed where the reflection says.
    std::vector<uint8_t> arguments(entryPoint->paramsSize, 0);
    auto setArgument = [&](const char* name, const void* value, size_t size) {
        const nvrhi::ShaderParameterReflection* parameter = nvrhi::findShaderEntryPointParameter(library, entryPoint, name);
        check(parameter && parameter->uniformSize == size && parameter->uniformOffset + size <= arguments.size(),
            std::string("kernel argument ") + name);
        std::memcpy(arguments.data() + parameter->uniformOffset, value, size);
    };
    const uint64_t handlesAddress = handleBuffer->getGpuVirtualAddress();
    const uint64_t outputAddress = output->getGpuVirtualAddress();
    setArgument("handles", &handlesAddress, 8);
    setArgument("pointVolume", &pointHandle, 8);
    setArgument("output", &outputAddress, 8);

    nvrhi::BindingLayoutHandle layout;
    device->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, uint32_t(arguments.size()))), &layout);
    nvrhi::BindingSetHandle set;
    device->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0, uint32_t(arguments.size()))),
        layout, &set);
    nvrhi::ComputePipelineHandle pipeline;
    device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout), &pipeline);
    check(set && pipeline, "the pipeline");

    nvrhi::CommandListHandle commandList = createCommandList(device);
    commandList->open();
    commandList->writeTexture(volume, 0, 0, texels.data(), n * sizeof(float), n * n * sizeof(float));
    commandList->writeBuffer(handleBuffer, handles, sizeof(handles), 0);
    commandList->setComputeState(nvrhi::ComputeState().setPipeline(pipeline).addBindingSet(set));
    commandList->setPushConstants(arguments.data(), arguments.size());
    commandList->dispatch(1);
    commandList->close();
    executeAndWait(device, commandList);

    const std::vector<uint8_t> outputBytes = readBuffer(device, output, n * n * n * 2 * sizeof(float));
    const float* results = reinterpret_cast<const float*>(outputBytes.data());
    for (uint32_t i = 0; i < n * n * n; ++i)
    {
        const uint32_t x = i % n;
        const float next = texels[i - x + std::min(x + 1, n - 1)];
        check(results[i * 2] == texels[i], "point sample " + std::to_string(i) + ": " + std::to_string(results[i * 2]));
        check(std::abs(results[i * 2 + 1] - 0.5f * (texels[i] + next)) < 1e-3f,
            "linear sample " + std::to_string(i) + ": " + std::to_string(results[i * 2 + 1]));
    }

    // The surface writes, read back through staging textures.
    auto readVolume = [&](nvrhi::ITexture* texture, const nvrhi::TextureDesc& desc, uint32_t texelSize) {
        nvrhi::StagingTextureHandle staging;
        device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read, &staging);
        commandList->open();
        commandList->copyTexture2(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
        commandList->close();
        executeAndWait(device, commandList);
        size_t rowPitch = 0;
        const uint8_t* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(),
            nvrhi::CpuAccessMode::Read, rowPitch));
        check(mapped != nullptr, std::string("map ") + desc.debugName.c_str());
        std::vector<uint8_t> packed(size_t(n) * n * n * texelSize);
        for (uint32_t z = 0; z < n; ++z)
            for (uint32_t y = 0; y < n; ++y)
                std::memcpy(packed.data() + ((z * n + y) * n) * texelSize, mapped + (z * n + y) * rowPitch, n * texelSize);
        device->unmapStagingTexture(staging);
        return packed;
    };
    const std::vector<uint8_t> floatTexels = readVolume(floats, floatsDesc, 4);
    const std::vector<uint8_t> byteTexels = readVolume(bytes, bytesDesc, 4);
    for (uint32_t i = 0; i < n * n * n; ++i)
    {
        const float value = reinterpret_cast<const float*>(floatTexels.data())[i];
        check(value == texels[i] * 2.f, "R32_FLOAT surface texel " + std::to_string(i) + ": " + std::to_string(value));
        const uint8_t* b = byteTexels.data() + i * 4;
        check(b[0] == i && b[1] == i % n && b[2] == (i / n) % n && b[3] == i / (n * n),
            "RGBA8_UINT surface texel " + std::to_string(i));
    }
    std::printf("handles: texture handles per sampler, surface handles of two element types, kernel arguments placed "
        "by reflection OK\n");

    // A kernel whose arguments end before their alignment: 8 + 4 bytes.
    nvrhi::ShaderHandle unpadded;
    device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Compute).setEntryName("unpaddedArguments"),
        container.data(), container.size(), &unpadded);
    check(unpadded && unpadded->getReflection()->paramsSize == 12, "unpaddedArguments takes 12 bytes of arguments");
    nvrhi::BindingLayoutHandle unpaddedLayout;
    device->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, 12)), &unpaddedLayout);
    nvrhi::BindingSetHandle unpaddedSet;
    device->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0, 12)), unpaddedLayout,
        &unpaddedSet);
    nvrhi::ComputePipelineHandle unpaddedPipeline;
    device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(unpadded).addBindingLayout(unpaddedLayout),
        &unpaddedPipeline);
    uint8_t unpaddedArguments[12];
    const uint32_t unpaddedValue = 0x12345678u;
    std::memcpy(unpaddedArguments, &outputAddress, 8);
    std::memcpy(unpaddedArguments + 8, &unpaddedValue, 4);
    commandList->open();
    commandList->setComputeState(nvrhi::ComputeState().setPipeline(unpaddedPipeline).addBindingSet(unpaddedSet));
    commandList->setPushConstants(unpaddedArguments, sizeof(unpaddedArguments));
    commandList->dispatch(1);
    commandList->close();
    executeAndWait(device, commandList);
    const std::vector<uint8_t> unpaddedResult = readBuffer(device, output, 4);
    check(*reinterpret_cast<const uint32_t*>(unpaddedResult.data()) == unpaddedValue, "the 12-byte kernel arguments arrive");
    std::printf("handles: unpadded kernel arguments OK\n");
}

static void testOptix(const std::string& fixtures)
{
    const std::vector<uint8_t> container = loadFixture(fixtures, "cuda-optix.nvsc");
    nvrhi::cuda::DeviceHandle device = createCudaDevice();
    if (!device->isOptixAvailable())
    {
        std::printf("SKIP: OptiX is not available\n");
        return;
    }

    nvrhi::ShaderLibraryHandle library;
    device->createShaderLibrary(container.data(), container.size(), &library);
    check(library != nullptr, "create the OptiX shader library");
    const nvrhi::ShaderLibraryReflection* reflection = library->getReflection();
    check(reflection && (reflection->target == nvrhi::ShaderCodeTarget::OptixPTX || reflection->target == nvrhi::ShaderCodeTarget::OptixIR),
        "the library reflects OptiX code");

    nvrhi::ShaderHandle rayGeneration, miss, triangleHit, boxIntersection, boxHit;
    library->getShader("rayGeneration", nvrhi::ShaderType::RayGeneration, &rayGeneration);
    library->getShader("miss", nvrhi::ShaderType::Miss, &miss);
    library->getShader("triangleHit", nvrhi::ShaderType::ClosestHit, &triangleHit);
    library->getShader("boxIntersection", nvrhi::ShaderType::Intersection, &boxIntersection);
    library->getShader("boxHit", nvrhi::ShaderType::ClosestHit, &boxHit);
    check(rayGeneration && miss && triangleHit && boxIntersection && boxHit, "the OptiX programs");

    // The record of the triangle hit group: its entry point's ConstantBuffer parameter.
    const nvrhi::ShaderParameterReflection* recordConstants = nvrhi::findShaderEntryPointParameter(reflection,
        triangleHit->getReflection(), "constants");
    check(recordConstants && recordConstants->kind == nvrhi::ShaderParameterKind::ConstantBuffer, "reflection: the record parameter");

    nvrhi::BindingLayoutHandle globalLayout, localLayout;
    device->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::AllRayTracing)
        .addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)), &globalLayout);
    device->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::AllRayTracing)
        .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(recordConstants->registerSlot)), &localLayout);

    nvrhi::rt::PipelineDesc pipelineDesc;
    pipelineDesc.addShader(nvrhi::rt::PipelineShaderDesc().setExportName("rayGen").setShader(rayGeneration))
        .addShader(nvrhi::rt::PipelineShaderDesc().setExportName("miss").setShader(miss))
        .addHitGroup(nvrhi::rt::PipelineHitGroupDesc().setExportName("triangle").setClosestHitShader(triangleHit)
            .setBindingLayout(localLayout))
        .addHitGroup(nvrhi::rt::PipelineHitGroupDesc().setExportName("box").setIntersectionShader(boxIntersection)
            .setClosestHitShader(boxHit).setIsProceduralPrimitive(true))
        .addBindingLayout(globalLayout)
        .setMaxPayloadSize(16)
        .setMaxAttributeSize(8)
        .setMaxRecursionDepth(1);
    nvrhi::rt::PipelineHandle pipeline;
    device->createRayTracingPipeline(pipelineDesc, &pipeline);
    check(pipeline != nullptr, "create the ray tracing pipeline");

    // Geometry: a triangle on the left, a box on the right.
    const float vertices[] = { -0.9f, -0.9f, 0.f, -0.1f, -0.9f, 0.f, -0.5f, 0.9f, 0.f };
    const nvrhi::rt::GeometryAABB box = { 0.1f, -0.5f, -0.1f, 0.9f, 0.5f, 0.1f };
    nvrhi::BufferDesc geometryDesc;
    geometryDesc.isAccelStructBuildInput = true;
    geometryDesc.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
    geometryDesc.keepInitialState = true;
    nvrhi::BufferHandle vertexBuffer, boxBuffer;
    device->createBuffer(nvrhi::BufferDesc(geometryDesc).setByteSize(sizeof(vertices)).setDebugName("vertices"), &vertexBuffer);
    device->createBuffer(nvrhi::BufferDesc(geometryDesc).setByteSize(sizeof(box)).setDebugName("box"), &boxBuffer);

    nvrhi::rt::GeometryDesc triangleGeometry;
    triangleGeometry.setTriangles(nvrhi::rt::GeometryTriangles().setVertexBuffer(vertexBuffer)
        .setVertexFormat(nvrhi::Format::RGB32_FLOAT).setVertexCount(3).setVertexStride(12));
    triangleGeometry.flags = nvrhi::rt::GeometryFlags::Opaque;
    nvrhi::rt::GeometryDesc boxGeometry;
    boxGeometry.setAABBs(nvrhi::rt::GeometryAABBs().setBuffer(boxBuffer).setCount(1).setStride(sizeof(box)));
    boxGeometry.flags = nvrhi::rt::GeometryFlags::Opaque;

    nvrhi::rt::AccelStructHandle triangleBlas, boxBlas, tlas;
    device->createAccelStruct(nvrhi::rt::AccelStructDesc().addBottomLevelGeometry(triangleGeometry)
        .setBuildFlags(nvrhi::rt::AccelStructBuildFlags::AllowCompaction).setDebugName("triangle BLAS"), &triangleBlas);
    device->createAccelStruct(nvrhi::rt::AccelStructDesc().addBottomLevelGeometry(boxGeometry).setDebugName("box BLAS"), &boxBlas);
    device->createAccelStruct(nvrhi::rt::AccelStructDesc().setIsTopLevel(true).setTopLevelMaxInstances(2).setDebugName("TLAS"), &tlas);
    check(triangleBlas && boxBlas && tlas, "create the acceleration structures");

    nvrhi::rt::InstanceDesc instances[2];
    instances[0].setBLAS(triangleBlas).setInstanceMask(0xFF).setInstanceContributionToHitGroupIndex(0);
    instances[1].setBLAS(boxBlas).setInstanceMask(0xFF).setInstanceContributionToHitGroupIndex(1);

    constexpr uint32_t width = 16, height = 16;
    nvrhi::BufferHandle output = createStructuredBuffer(device, width * height, 16, true, "rays");

    struct HitConstants { float tint[4]; } hitConstants = { { 0.f, 1.f, 0.f, 1.f } };
    nvrhi::BufferHandle tintBuffer;
    device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(hitConstants)).setIsConstantBuffer(true)
        .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true).setDebugName("tint"), &tintBuffer);
    nvrhi::BindingSetHandle localSet, globalSet;
    device->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::ConstantBuffer(recordConstants->registerSlot, tintBuffer)),
        localLayout, &localSet);
    device->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, tlas))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, output)), globalLayout, &globalSet);

    nvrhi::rt::ShaderTableHandle table;
    pipeline->createShaderTable(nvrhi::rt::ShaderTableDesc(), &table);
    table->setRayGenerationShader("rayGen");
    check(table->addMissShader("miss") == 0, "addMissShader");
    check(table->addHitGroup("triangle", localSet) == 0 && table->addHitGroup("box") == 1, "addHitGroup");

    nvrhi::CommandListHandle commandList = createCommandList(device);
    commandList->open();
    commandList->writeBuffer(vertexBuffer, vertices, sizeof(vertices), 0);
    commandList->writeBuffer(boxBuffer, &box, sizeof(box), 0);
    commandList->writeBuffer(tintBuffer, &hitConstants, sizeof(hitConstants), 0);
    commandList->buildBottomLevelAccelStruct(triangleBlas, &triangleGeometry, 1, nvrhi::rt::AccelStructBuildFlags::AllowCompaction);
    commandList->buildBottomLevelAccelStruct(boxBlas, &boxGeometry, 1, nvrhi::rt::AccelStructBuildFlags::None);
    commandList->buildTopLevelAccelStruct(tlas, instances, 2, nvrhi::rt::AccelStructBuildFlags::None);
    nvrhi::rt::State state;
    state.setShaderTable(table).addBindingSet(globalSet);
    commandList->setRayTracingState(state);
    // The [[vk::push_constant]] block of the global parameters.
    const Float4 missColor = { 0.25f, 0.5f, 1.f, 1.f };
    commandList->setPushConstants(&missColor, sizeof(missColor));
    commandList->dispatchRays(nvrhi::rt::DispatchRaysArguments().setWidth(width).setHeight(height));
    commandList->close();
    executeAndWait(device, commandList);

    auto checkRays = [&](const char* when)
    {
        const std::vector<uint8_t> bytes = readBuffer(device, output, width * height * sizeof(Float4));
        const Float4* pixels = reinterpret_cast<const Float4*>(bytes.data());
        auto pixel = [&](uint32_t x, uint32_t y) { return pixels[y * width + x]; };
        check(nearlyEqual(pixel(4, 4), { 0.f, 1.f, 0.f, 1.f }), std::string(when) + ": the triangle hit returns the record tint: " + toString(pixel(4, 4)));
        check(nearlyEqual(pixel(11, 8), { 1.f, 0.f, 0.f, 1.f }), std::string(when) + ": the box hit: " + toString(pixel(11, 8)));
        check(nearlyEqual(pixel(0, 15), missColor), std::string(when) + ": a miss returns the push constants: " +
            toString(pixel(0, 15)));
    };
    checkRays("after the build");

    // Compaction of the triangle BLAS, then the same picture again after rebuilding the TLAS.
    commandList->open();
    commandList->compactBottomLevelAccelStructs();
    commandList->buildTopLevelAccelStruct(tlas, instances, 2, nvrhi::rt::AccelStructBuildFlags::None);
    commandList->clearBufferUInt(output, 0);
    commandList->setRayTracingState(state);
    commandList->setPushConstants(&missColor, sizeof(missColor));
    commandList->dispatchRays(nvrhi::rt::DispatchRaysArguments().setWidth(width).setHeight(height));
    commandList->close();
    executeAndWait(device, commandList);
    checkRays("after compaction");
    check(triangleBlas->isCompacted(), "the triangle BLAS was compacted");
    std::printf("optix: triangle and AABB BLAS, TLAS, pipeline with an intersection program, shader table with a record, "
        "push constants, compaction OK\n");
}

static void checkLibraryReflection(const nvrhi::ShaderLibraryReflection* reflection, nvrhi::ShaderCodeTarget target, const char* api)
{
    // SPIR-V bindings carry the register shifts the fixture is compiled with (NVRHI's VulkanBindingOffsets).
    auto binding = [&](nvrhi::ShaderParameterKind kind, uint32_t slot) -> uint32_t
    {
        if (target != nvrhi::ShaderCodeTarget::SPIRV)
            return slot;
        switch (kind)
        {
        case nvrhi::ShaderParameterKind::Sampler: return slot + 128;
        case nvrhi::ShaderParameterKind::ConstantBuffer: return slot + 256;
        case nvrhi::ShaderParameterKind::Texture_UAV:
        case nvrhi::ShaderParameterKind::StructuredBuffer_UAV:
        case nvrhi::ShaderParameterKind::RawBuffer_UAV:
        case nvrhi::ShaderParameterKind::TypedBuffer_UAV: return slot + 384;
        default: return slot;
        }
    };

    check(reflection != nullptr, std::string(api) + ": the library has reflection");
    check(reflection->target == target, std::string(api) + ": reflection target");
    const nvrhi::ShaderEntryPointReflection* entryPoint = nvrhi::findShaderEntryPoint(reflection, "main");
    check(entryPoint && entryPoint->stage == nvrhi::ShaderType::Compute && entryPoint->threadGroupSize[0] == 64,
        std::string(api) + ": entry point main");

    // The registers are the same on every target (ShaderTool cross-maps the CUDA layout to the HLSL one).
    struct Expected { const char* name; nvrhi::ShaderParameterKind kind; uint32_t slot; uint32_t arraySize; };
    const Expected expected[] = {
        { "gConstants", nvrhi::ShaderParameterKind::ConstantBuffer, 0, 0 },
        { "gInput", nvrhi::ShaderParameterKind::StructuredBuffer_SRV, 0, 0 },
        { "gTexture", nvrhi::ShaderParameterKind::Texture_SRV, 1, 0 },
        { "gVolumes", nvrhi::ShaderParameterKind::Texture_SRV, 2, 2 },
        { "gSampler", nvrhi::ShaderParameterKind::Sampler, 1, 0 },
        { "gOutput", nvrhi::ShaderParameterKind::StructuredBuffer_UAV, 0, 0 },
        { "gCounter", nvrhi::ShaderParameterKind::RawBuffer_UAV, 1, 0 },
        { "gSurface", nvrhi::ShaderParameterKind::Texture_UAV, 2, 0 },
    };
    for (const Expected& e : expected)
    {
        const nvrhi::ShaderParameterReflection& parameter = requireParameter(reflection, e.name, e.kind);
        check(parameter.registerSlot == binding(e.kind, e.slot) && parameter.registerSpace == 0,
            std::string(api) + ": register of " + e.name + " is " + std::to_string(parameter.registerSlot));
        check(parameter.arraySize == e.arraySize, std::string(api) + ": array size of " + e.name);
        if (target == nvrhi::ShaderCodeTarget::CudaPTX)
            check(parameter.uniformOffset + parameter.uniformSize <= reflection->globalParamsSize,
                std::string(api) + ": " + e.name + " lies in SLANG_globalParams");
    }

    // The fields of the constant buffer.
    const nvrhi::ShaderParameterReflection& constants = requireParameter(reflection, "gConstants", nvrhi::ShaderParameterKind::ConstantBuffer);
    const nvrhi::ShaderFieldReflection* count = nvrhi::findShaderField(reflection, constants.typeIndex, "count");
    check(count && count->offset == 16, std::string(api) + ": gConstants.count at byte 16");
    std::printf("%s: reflection of the container OK\n", api);
}

static void testReflection(const std::string& fixtures)
{
    const std::vector<uint8_t> container = loadFixture(fixtures, "cuda-reflection.nvsc");
    check(nvrhi::isShaderContainer(container.data(), container.size()), "the fixture is a container");

    {
        nvrhi::cuda::DeviceHandle device = createCudaDevice();
        nvrhi::ShaderLibraryHandle library;
        device->createShaderLibrary(container.data(), container.size(), &library);
        check(library != nullptr, "CUDA: create the shader library");
        checkLibraryReflection(library->getReflection(), nvrhi::ShaderCodeTarget::CudaPTX, "CUDA");
        const nvrhi::ShaderLibraryReflection* reflection = library->getReflection();
        check(reflection->globalParamsSize > 0, "CUDA: the global parameter block has a size");
        const nvrhi::ShaderParameterReflection& volumes = requireParameter(reflection, "gVolumes", nvrhi::ShaderParameterKind::Texture_SRV);
        check(volumes.uniformStride == 8 && volumes.uniformSize == 16, "CUDA: gVolumes is two 8-byte texture objects");
    }

#if TEST_D3D12
    {
        Microsoft::WRL::ComPtr<ID3D12Device> native;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        nvrhi::d3d12::DeviceHandle device = createD3D12Device(native, queue);
        nvrhi::ShaderLibraryHandle library;
        device->createShaderLibrary(container.data(), container.size(), &library);
        check(library != nullptr, "D3D12: create the shader library");
        checkLibraryReflection(library->getReflection(), nvrhi::ShaderCodeTarget::DXIL, "D3D12");

        nvrhi::ShaderHandle shader;
        device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Compute).setEntryName("main"),
            container.data(), container.size(), &shader);
        check(shader && shader->getReflection() && strcmp(shader->getReflection()->name.c_str(), "main") == 0,
            "D3D12: a shader from a container");
    }
#endif

#if TEST_VULKAN
    withVulkanDevice([&](nvrhi::IDevice* device)
    {
        nvrhi::ShaderLibraryHandle library;
        device->createShaderLibrary(container.data(), container.size(), &library);
        check(library != nullptr, "Vulkan: create the shader library");
        checkLibraryReflection(library->getReflection(), nvrhi::ShaderCodeTarget::SPIRV, "Vulkan");
    });
#endif
}
