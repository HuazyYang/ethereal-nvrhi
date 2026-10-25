/*
* Copyright (c) 2026, ethereal contributors. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

// Ray tracing through OptiX 9 (ADR 0011).
//
// - A ray tracing pipeline links the OptiX programs of its shaders: one OptixModule per shader library, one
//   program group per shader or hit group. The Slang global parameters are the launch parameters
//   (SLANG_globalParams).
// - A shader table is written into transient memory when a dispatch is recorded: each record is the program
//   group header followed by the entry point's parameters, written from the record's local binding set.
// - Acceleration structures are built on the stream. OptiX returns the traversable handle when the build is
//   enqueued, so instances and parameters that refer to an acceleration structure get its handle at replay.
//
// This file is the only one that calls OptiX. Its function table is renamed and its stubs are static, so that
// NVRHI does not clash with an application that uses OptiX itself.

#include "cuda-backend.h"

#include <nvrhi/common/misc.h>

#include <algorithm>
#include <cstring>

#if NVRHI_WITH_OPTIX
#include <optix_function_table.h>
#undef OPTIX_FUNCTION_TABLE_SYMBOL
#define OPTIX_FUNCTION_TABLE_SYMBOL g_nvrhiOptixFunctionTable
#ifndef OPTIX_ENABLE_SDK_MIXING
#define OPTIX_ENABLE_SDK_MIXING 1
#endif
#include <optix_function_table_definition.h>
#include <optix_stubs.h>

#if OPTIX_VERSION < 90000
#error "The CUDA backend of NVRHI requires the OptiX SDK 9.0 or newer"
#endif
#endif

namespace nvrhi::cuda
{
    //////////////////////////////////////////////////////////////////////////
    // Objects that need no OptiX
    //////////////////////////////////////////////////////////////////////////

    AccelStruct::~AccelStruct()
    {
        ContextScope scope(m_Context);
        if (buffer)
            g_Cuda.cuMemFree(buffer);
        if (compactedSizeHost)
            g_Cuda.cuMemFreeHost(compactedSizeHost);
    }

    NativeObject AccelStruct::getNativeObject(ObjectType objectType) noexcept
    {
        switch (objectType)
        {
        case ObjectTypes::OptiX_TraversableHandle:
            return NativeObject(uintptr_t(traversable));
        case ObjectTypes::CUDA_DevicePtr:
            return NativeObject(uintptr_t(buffer));
        default:
            return nullptr;
        }
    }

    int RayTracingPipeline::findProgram(const char* exportName, ShaderType type) const
    {
        if (!exportName)
            return -1;
        for (size_t i = 0; i < programs.size(); ++i)
        {
            if (programs[i].type == type && strcmp(programs[i].exportName.c_str(), exportName) == 0)
                return int(i);
        }
        return -1;
    }

    rt::ShaderTableHandle RayTracingPipeline::createShaderTable(const rt::ShaderTableDesc& tableDesc)
    {
        AutoPtr<ShaderTable> table = TakeOver(MAKE_RC_OBJ(ShaderTable));
        table->desc = tableDesc;
        table->pipeline = this;
        return table;
    }

    uint32_t ShaderTable::getNumEntries() const noexcept
    {
        return uint32_t((hasRayGeneration ? 1 : 0) + missShaders.size() + hitGroups.size() + callableShaders.size());
    }

    rt::IPipeline* ShaderTable::getPipeline() const noexcept
    {
        return pipeline;
    }

    void ShaderTable::setRayGenerationShader(const char* exportName, IBindingSet* bindings) noexcept
    {
        const int program = pipeline->findProgram(exportName, ShaderType::RayGeneration);
        if (program < 0)
        {
            hasRayGeneration = false;
            return;
        }
        rayGeneration.programIndex = uint32_t(program);
        rayGeneration.bindings = bindings;
        hasRayGeneration = true;
    }

    namespace
    {
        int AddEntry(RayTracingPipeline* pipeline, std::vector<ShaderTable::Entry>& entries, const char* exportName,
            ShaderType type, IBindingSet* bindings) noexcept
        {
            const int program = pipeline->findProgram(exportName, type);
            if (program < 0)
                return -1;
            try
            {
                ShaderTable::Entry entry;
                entry.programIndex = uint32_t(program);
                entry.bindings = bindings;
                entries.push_back(entry);
                return int(entries.size()) - 1;
            }
            catch (...)
            {
                return -1;
            }
        }
    }

    int ShaderTable::addMissShader(const char* exportName, IBindingSet* bindings) noexcept
    {
        return AddEntry(pipeline, missShaders, exportName, ShaderType::Miss, bindings);
    }

    int ShaderTable::addHitGroup(const char* exportName, IBindingSet* bindings) noexcept
    {
        return AddEntry(pipeline, hitGroups, exportName, ShaderType::ClosestHit, bindings);
    }

    int ShaderTable::addCallableShader(const char* exportName, IBindingSet* bindings) noexcept
    {
        return AddEntry(pipeline, callableShaders, exportName, ShaderType::Callable, bindings);
    }

    void Device::addPendingCompaction(AccelStruct* as)
    {
        std::lock_guard<std::mutex> lock(m_CompactionMutex);
        for (const auto& pending : m_PendingCompactions)
        {
            if (pending == as)
                return;
        }
        m_PendingCompactions.push_back(as);
    }

    std::vector<AutoPtr<AccelStruct>> Device::takeCompactableAccelStructs()
    {
        std::vector<AutoPtr<AccelStruct>> ready;
        std::lock_guard<std::mutex> lock(m_CompactionMutex);
        for (auto it = m_PendingCompactions.begin(); it != m_PendingCompactions.end();)
        {
            AccelStruct* as = *it;
            Queue* queue = getQueue(as->buildQueue);
            if (as->buildInstance != 0 && queue && queue->updateCompleted() >= as->buildInstance)
            {
                ready.push_back(*it);
                it = m_PendingCompactions.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return ready;
    }

    rt::cluster::OperationSizeInfo& Device::getClusterOperationSizeInfo(rt::cluster::OperationSizeInfo& retVal,
        const rt::cluster::OperationParams& params) noexcept
    {
        (void)params;
        retVal = rt::cluster::OperationSizeInfo();
        return retVal;
    }

    MemoryRequirements& Device::getAccelStructMemoryRequirements(MemoryRequirements& retVal, rt::IAccelStruct* _as) noexcept
    {
        retVal = MemoryRequirements();
        if (AccelStruct* as = checked_cast<AccelStruct*>(_as))
        {
            retVal.size = as->bufferSize;
            retVal.alignment = 128;
        }
        return retVal;
    }

#if NVRHI_WITH_OPTIX

    //////////////////////////////////////////////////////////////////////////
    // OptiX
    //////////////////////////////////////////////////////////////////////////

    namespace
    {
        const char* OptixResultToString(OptixResult result)
        {
            const char* name = optixGetErrorName(result);
            return name ? name : "OptiX error";
        }

        bool OptixCheck(const Context& context, OptixResult result, const char* what)
        {
            if (result == OPTIX_SUCCESS)
                return true;
            context.error(std::string(what) + " failed: " + OptixResultToString(result));
            return false;
        }

        void OptixLog(unsigned int level, const char* tag, const char* message, void* data)
        {
            const Context* context = static_cast<const Context*>(data);
            if (!context || !message)
                return;
            const MessageSeverity severity = level <= 1 ? MessageSeverity::Fatal :
                level == 2 ? MessageSeverity::Error : level == 3 ? MessageSeverity::Warning : MessageSeverity::Info;
            context->message(severity, std::string("OptiX [") + (tag ? tag : "") + "]: " + message);
        }

        struct OptixInitState
        {
            std::once_flag once;
            OptixResult result = OPTIX_ERROR_INTERNAL_ERROR;
        };
    }

#define NVRHI_OPTIX_CHECK(ctx, call) OptixCheck((ctx), (call), #call)

    bool InitializeOptix(Context& context, bool enableValidation, std::string& error)
    {
        static OptixInitState state;
        std::call_once(state.once, [] { state.result = optixInit(); });
        if (state.result != OPTIX_SUCCESS)
        {
            error = std::string("optixInit failed: ") + OptixResultToString(state.result);
            return false;
        }

        OptixDeviceContextOptions options = {};
        options.logCallbackFunction = &OptixLog;
        options.logCallbackData = &context;
        options.logCallbackLevel = 3;
        options.validationMode = enableValidation ? OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_ALL : OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_OFF;

        const OptixResult result = optixDeviceContextCreate(context.context, &options, &context.optixContext);
        if (result != OPTIX_SUCCESS)
        {
            context.optixContext = nullptr;
            error = std::string("optixDeviceContextCreate failed: ") + OptixResultToString(result);
            return false;
        }
        return true;
    }

    void ShutdownOptix(Context& context)
    {
        if (context.optixContext)
        {
            optixDeviceContextDestroy(context.optixContext);
            context.optixContext = nullptr;
        }
    }

    //////////////////////////////////////////////////////////////////////////
    // Acceleration structures
    //////////////////////////////////////////////////////////////////////////

    namespace
    {
        // One geometry of a bottom-level build, resolved to device addresses when the build is recorded. The build
        // inputs point into these records, which stay in place while the recording is replayed.
        struct GeometryRecord
        {
            uint32_t type;                  // OptixBuildInputType
            unsigned int flags;             // OptixGeometryFlags, the one SBT record of the input
            CUdeviceptr vertexBuffer;
            uint32_t vertexCount;
            uint32_t vertexStride;
            OptixVertexFormat vertexFormat;
            uint32_t indexTriplets;
            CUdeviceptr indexBuffer;
            OptixIndicesFormat indexFormat;
            uint32_t indexStride;
            CUdeviceptr transform;          // 12 floats, or 0
            CUdeviceptr aabbBuffer;
            uint32_t aabbCount;
            uint32_t aabbStride;
        };

        struct CommandBuildAccelStruct
        {
            CommandHeader header;
            AccelStruct* as;
            OptixAccelBuildOptions options;
            CUdeviceptr temp;
            size_t tempSize;
            CUdeviceptr output;
            size_t outputSize;
            CUdeviceptr instances;          // top level
            uint32_t instanceCount;
            uint32_t geometryCount;         // bottom level: GeometryRecords follow the command
            uint32_t emitCompactedSize;
            uint32_t isTopLevel;
        };

        OptixAccelBuildOptions ConvertBuildOptions(rt::AccelStructBuildFlags flags)
        {
            OptixAccelBuildOptions options = {};
            if ((flags & rt::AccelStructBuildFlags::AllowUpdate) != 0)
                options.buildFlags |= OPTIX_BUILD_FLAG_ALLOW_UPDATE;
            if ((flags & rt::AccelStructBuildFlags::AllowCompaction) != 0)
                options.buildFlags |= OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
            if ((flags & rt::AccelStructBuildFlags::PreferFastTrace) != 0)
                options.buildFlags |= OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
            else if ((flags & rt::AccelStructBuildFlags::PreferFastBuild) != 0)
                options.buildFlags |= OPTIX_BUILD_FLAG_PREFER_FAST_BUILD;
            options.operation = (flags & rt::AccelStructBuildFlags::PerformUpdate) != 0 ? OPTIX_BUILD_OPERATION_UPDATE
                : OPTIX_BUILD_OPERATION_BUILD;
            return options;
        }

        bool ConvertVertexFormat(Format format, OptixVertexFormat& out)
        {
            switch (format)
            {
            case Format::RGB32_FLOAT: out = OPTIX_VERTEX_FORMAT_FLOAT3; return true;
            case Format::RG32_FLOAT: out = OPTIX_VERTEX_FORMAT_FLOAT2; return true;
            case Format::RGBA32_FLOAT: out = OPTIX_VERTEX_FORMAT_FLOAT3; return true;   // the w component is ignored
            case Format::RGBA16_FLOAT: out = OPTIX_VERTEX_FORMAT_HALF3; return true;
            case Format::RG16_FLOAT: out = OPTIX_VERTEX_FORMAT_HALF2; return true;
            default: return false;
            }
        }

        unsigned int ConvertGeometryFlags(rt::GeometryFlags flags)
        {
            unsigned int result = OPTIX_GEOMETRY_FLAG_NONE;
            if ((flags & rt::GeometryFlags::Opaque) != 0)
                result |= OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT;
            if ((flags & rt::GeometryFlags::NoDuplicateAnyHitInvocation) != 0)
                result |= OPTIX_GEOMETRY_FLAG_REQUIRE_SINGLE_ANYHIT_CALL;
            return result;
        }

        unsigned int ConvertInstanceFlags(rt::InstanceFlags flags)
        {
            unsigned int result = OPTIX_INSTANCE_FLAG_NONE;
            if ((flags & rt::InstanceFlags::TriangleCullDisable) != 0)
                result |= OPTIX_INSTANCE_FLAG_DISABLE_TRIANGLE_FACE_CULLING;
            if ((flags & rt::InstanceFlags::TriangleFrontCounterclockwise) != 0)
                result |= OPTIX_INSTANCE_FLAG_FLIP_TRIANGLE_FACING;
            if ((flags & rt::InstanceFlags::ForceOpaque) != 0)
                result |= OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT;
            if ((flags & rt::InstanceFlags::ForceNonOpaque) != 0)
                result |= OPTIX_INSTANCE_FLAG_ENFORCE_ANYHIT;
            return result;
        }

        // Converts the geometries of a bottom-level build. With 'resolve' false (sizing at creation) the device
        // addresses are left 0; OptiX needs only the counts and formats then.
        bool ConvertGeometries(const Context& context, const rt::GeometryDesc* geometries, size_t count,
            std::vector<GeometryRecord>& records)
        {
            records.resize(count);
            for (size_t i = 0; i < count; ++i)
            {
                const rt::GeometryDesc& geometry = geometries[i];
                GeometryRecord& record = records[i];
                record = GeometryRecord();
                record.flags = ConvertGeometryFlags(geometry.flags);

                if (geometry.geometryType == rt::GeometryType::Triangles)
                {
                    const rt::GeometryTriangles& triangles = geometry.geometryData.triangles;
                    record.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
                    if (!ConvertVertexFormat(triangles.vertexFormat, record.vertexFormat))
                    {
                        context.error(std::string("vertex format ") + utils::FormatToString(triangles.vertexFormat) +
                            " is not supported by OptiX");
                        return false;
                    }
                    if (Buffer* vertexBuffer = triangles.vertexBuffer ? checked_cast<Buffer*>(triangles.vertexBuffer) : nullptr)
                        record.vertexBuffer = vertexBuffer->devicePtr + triangles.vertexOffset;
                    record.vertexCount = triangles.vertexCount;
                    record.vertexStride = triangles.vertexStride;

                    if (triangles.indexFormat == Format::R32_UINT || triangles.indexFormat == Format::R16_UINT)
                    {
                        const bool wide = triangles.indexFormat == Format::R32_UINT;
                        record.indexFormat = wide ? OPTIX_INDICES_FORMAT_UNSIGNED_INT3 : OPTIX_INDICES_FORMAT_UNSIGNED_SHORT3;
                        record.indexStride = wide ? 12 : 6;
                        record.indexTriplets = triangles.indexCount / 3;
                        if (Buffer* indexBuffer = triangles.indexBuffer ? checked_cast<Buffer*>(triangles.indexBuffer) : nullptr)
                            record.indexBuffer = indexBuffer->devicePtr + triangles.indexOffset;
                    }
                    else if (triangles.indexFormat == Format::UNKNOWN)
                    {
                        record.indexFormat = OPTIX_INDICES_FORMAT_NONE;
                    }
                    else
                    {
                        context.error("index format is not supported by OptiX");
                        return false;
                    }
                }
                else if (geometry.geometryType == rt::GeometryType::AABBs)
                {
                    // rt::GeometryAABB and OptixAabb have the same layout: min xyz, max xyz.
                    const rt::GeometryAABBs& aabbs = geometry.geometryData.aabbs;
                    record.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
                    if (Buffer* buffer = aabbs.buffer ? checked_cast<Buffer*>(aabbs.buffer) : nullptr)
                        record.aabbBuffer = buffer->devicePtr + aabbs.offset;
                    record.aabbCount = aabbs.count;
                    record.aabbStride = aabbs.stride;
                }
                else
                {
                    context.error("spheres and linear swept spheres are not supported on the CUDA backend");
                    return false;
                }
            }
            return true;
        }

        // Fills the OptiX build inputs from records; the inputs point into the records.
        void MakeBuildInputs(GeometryRecord* records, size_t count, std::vector<OptixBuildInput>& inputs)
        {
            inputs.assign(count, OptixBuildInput{});
            for (size_t i = 0; i < count; ++i)
            {
                GeometryRecord& record = records[i];
                OptixBuildInput& input = inputs[i];
                input.type = OptixBuildInputType(record.type);
                if (input.type == OPTIX_BUILD_INPUT_TYPE_TRIANGLES)
                {
                    OptixBuildInputTriangleArray& triangles = input.triangleArray;
                    triangles.vertexBuffers = &record.vertexBuffer;
                    triangles.numVertices = record.vertexCount;
                    triangles.vertexFormat = record.vertexFormat;
                    triangles.vertexStrideInBytes = record.vertexStride;
                    triangles.indexBuffer = record.indexBuffer;
                    triangles.numIndexTriplets = record.indexTriplets;
                    triangles.indexFormat = record.indexFormat;
                    triangles.indexStrideInBytes = record.indexStride;
                    triangles.preTransform = record.transform;
                    triangles.transformFormat = record.transform ? OPTIX_TRANSFORM_FORMAT_MATRIX_FLOAT12 : OPTIX_TRANSFORM_FORMAT_NONE;
                    triangles.flags = &record.flags;
                    triangles.numSbtRecords = 1;
                }
                else
                {
                    OptixBuildInputCustomPrimitiveArray& primitives = input.customPrimitiveArray;
                    primitives.aabbBuffers = &record.aabbBuffer;
                    primitives.numPrimitives = record.aabbCount;
                    primitives.strideInBytes = record.aabbStride;
                    primitives.flags = &record.flags;
                    primitives.numSbtRecords = 1;
                }
            }
        }

        OptixBuildInput MakeInstanceInput(CUdeviceptr instances, uint32_t count)
        {
            OptixBuildInput input = {};
            input.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
            input.instanceArray.instances = instances;
            input.instanceArray.numInstances = count;
            return input;
        }
    }

    bool Device::queryTopLevelAccelStructPrebuildInfo(const rt::AccelStructDesc& desc, uint32_t instanceCount,
        rt::AccelStructPrebuildInfo& outInfo) noexcept
    {
        if (!m_Context.optixContext || !desc.isTopLevel || instanceCount > desc.topLevelMaxInstances)
            return false;
        const OptixAccelBuildOptions options = ConvertBuildOptions(desc.buildFlags);
        const OptixBuildInput input = MakeInstanceInput(0, instanceCount);
        OptixAccelBufferSizes sizes = {};
        if (optixAccelComputeMemoryUsage(m_Context.optixContext, &options, &input, 1, &sizes) != OPTIX_SUCCESS)
            return false;
        outInfo.resultBytes = sizes.outputSizeInBytes;
        outInfo.scratchBytes = sizes.tempSizeInBytes;
        outInfo.updateScratchBytes = sizes.tempUpdateSizeInBytes;
        return true;
    }

    rt::AccelStructHandle Device::createAccelStruct(const rt::AccelStructDesc& desc)
    {
        if (!m_Context.optixContext)
        {
            m_Context.error("createAccelStruct: OptiX is not available");
            return nullptr;
        }
        if (desc.isVirtual)
        {
            m_Context.error("createAccelStruct: virtual acceleration structures are not supported");
            return nullptr;
        }

        OptixAccelBuildOptions options = ConvertBuildOptions(desc.buildFlags);
        options.operation = OPTIX_BUILD_OPERATION_BUILD;
        OptixAccelBufferSizes sizes = {};

        if (desc.isTopLevel)
        {
            const OptixBuildInput input = MakeInstanceInput(0, uint32_t(desc.topLevelMaxInstances));
            if (!NVRHI_OPTIX_CHECK(m_Context, optixAccelComputeMemoryUsage(m_Context.optixContext, &options, &input, 1, &sizes)))
                return nullptr;
        }
        else
        {
            std::vector<GeometryRecord> records;
            if (!ConvertGeometries(m_Context, desc.bottomLevelGeometries.data(), desc.bottomLevelGeometries.size(), records))
                return nullptr;
            std::vector<OptixBuildInput> inputs;
            MakeBuildInputs(records.data(), records.size(), inputs);
            if (!NVRHI_OPTIX_CHECK(m_Context, optixAccelComputeMemoryUsage(m_Context.optixContext, &options,
                inputs.data(), uint32_t(inputs.size()), &sizes)))
                return nullptr;
        }

        ContextScope scope(m_Context);
        AutoPtr<AccelStruct> as = TakeOver(MAKE_RC_OBJ(AccelStruct, m_Context));
        as->desc = desc;
        as->bufferSize = std::max<size_t>(sizes.outputSizeInBytes, 128);
        as->scratchSize = sizes.tempSizeInBytes;
        as->updateScratchSize = sizes.tempUpdateSizeInBytes;
        if (!NVRHI_CUDA_CHECK(m_Context, cuMemAlloc(&as->buffer, as->bufferSize)))
            return nullptr;

        if ((desc.buildFlags & rt::AccelStructBuildFlags::AllowCompaction) != 0)
        {
            void* host = nullptr;
            if (!NVRHI_CUDA_CHECK(m_Context, cuMemHostAlloc(&host, sizeof(uint64_t), CU_MEMHOSTALLOC_DEVICEMAP | CU_MEMHOSTALLOC_PORTABLE)) ||
                !NVRHI_CUDA_CHECK(m_Context, cuMemHostGetDevicePointer(&as->compactedSizeDevice, host, 0)))
                return nullptr;
            as->compactedSizeHost = static_cast<uint64_t*>(host);
            *as->compactedSizeHost = 0;
        }
        return as;
    }

    void CommandList::buildBottomLevelAccelStruct(rt::IAccelStruct* _as, const rt::GeometryDesc* pGeometries,
        size_t numGeometries, rt::AccelStructBuildFlags buildFlags) noexcept
    {
        AccelStruct* as = checked_cast<AccelStruct*>(_as);
        if (!as || !m_Instance || as->desc.isTopLevel)
            return;

        try
        {
            std::vector<GeometryRecord> records;
            if (!ConvertGeometries(m_Context, pGeometries, numGeometries, records))
                return;

            for (size_t i = 0; i < numGeometries; ++i)
            {
                const rt::GeometryDesc& geometry = pGeometries[i];
                if (geometry.geometryType == rt::GeometryType::Triangles)
                {
                    reference(geometry.geometryData.triangles.vertexBuffer);
                    reference(geometry.geometryData.triangles.indexBuffer);
                }
                else if (geometry.geometryType == rt::GeometryType::AABBs)
                {
                    reference(geometry.geometryData.aabbs.buffer);
                }
                if (geometry.useTransform)
                {
                    TransientAllocation upload;
                    if (!allocateUpload(sizeof(geometry.transform), 64, upload))
                        return;
                    memcpy(upload.hostPtr, geometry.transform, sizeof(geometry.transform));
                    if (!copyToScratch(upload, sizeof(geometry.transform), 64, records[i].transform))
                        return;
                }
            }

            const OptixAccelBuildOptions options = ConvertBuildOptions(buildFlags | (as->desc.buildFlags & ~rt::AccelStructBuildFlags::PerformUpdate));
            std::vector<OptixBuildInput> inputs;
            MakeBuildInputs(records.data(), records.size(), inputs);
            OptixAccelBufferSizes sizes = {};
            if (!NVRHI_OPTIX_CHECK(m_Context, optixAccelComputeMemoryUsage(m_Context.optixContext, &options,
                inputs.data(), uint32_t(inputs.size()), &sizes)))
                return;
            if (sizes.outputSizeInBytes > as->bufferSize)
            {
                m_Context.error("buildBottomLevelAccelStruct: the geometry needs more memory than the acceleration "
                    "structure was created for");
                return;
            }

            const size_t tempSize = options.operation == OPTIX_BUILD_OPERATION_UPDATE ? sizes.tempUpdateSizeInBytes : sizes.tempSizeInBytes;
            CUdeviceptr temp = 0;
            if (tempSize && !allocateScratch(tempSize, OPTIX_ACCEL_BUFFER_BYTE_ALIGNMENT, temp))
                return;

            reference(as);
            const bool compact = (options.buildFlags & OPTIX_BUILD_FLAG_ALLOW_COMPACTION) != 0 && as->compactedSizeDevice;
            CommandBuildAccelStruct* command = appendCommand<CommandBuildAccelStruct>(CommandType::BuildAccelStruct,
                records.size() * sizeof(GeometryRecord));
            command->as = as;
            command->options = options;
            command->temp = temp;
            command->tempSize = tempSize;
            command->output = as->buffer;
            command->outputSize = as->bufferSize;
            command->geometryCount = uint32_t(records.size());
            command->emitCompactedSize = compact ? 1 : 0;
            memcpy(command + 1, records.data(), records.size() * sizeof(GeometryRecord));

            if (compact && !as->compacted)
            {
                as->buildInstance = 0;     // set when the build is executed
                m_Device->addPendingCompaction(as);
            }
        }
        catch (...)
        {
            m_Context.error("buildBottomLevelAccelStruct: out of memory");
        }
    }

    void CommandList::buildTopLevelAccelStruct(rt::IAccelStruct* _as, const rt::InstanceDesc* pInstances,
        size_t numInstances, rt::AccelStructBuildFlags buildFlags) noexcept
    {
        AccelStruct* as = checked_cast<AccelStruct*>(_as);
        if (!as || !m_Instance || !as->desc.isTopLevel)
            return;
        if (numInstances > as->desc.topLevelMaxInstances)
        {
            m_Context.error("buildTopLevelAccelStruct: more instances than topLevelMaxInstances");
            return;
        }

        try
        {
            const size_t instancesSize = std::max<size_t>(numInstances, 1) * sizeof(OptixInstance);
            TransientAllocation upload;
            if (!allocateUpload(instancesSize, OPTIX_INSTANCE_BYTE_ALIGNMENT, upload))
                return;
            memset(upload.hostPtr, 0, instancesSize);

            OptixInstance* instances = reinterpret_cast<OptixInstance*>(upload.hostPtr);
            as->bottomLevelReferences.clear();
            for (size_t i = 0; i < numInstances; ++i)
            {
                const rt::InstanceDesc& src = pInstances[i];
                OptixInstance& dst = instances[i];
                memcpy(dst.transform, src.transform, sizeof(dst.transform));
                dst.instanceId = src.instanceID;
                dst.sbtOffset = src.instanceContributionToHitGroupIndex;
                dst.visibilityMask = src.instanceMask;
                dst.flags = ConvertInstanceFlags(src.flags);

                AccelStruct* blas = src.bottomLevelAS ? checked_cast<AccelStruct*>(src.bottomLevelAS) : nullptr;
                if (!blas)
                {
                    dst.visibilityMask = 0;     // an empty instance
                    continue;
                }
                reference(blas);
                as->bottomLevelReferences.push_back(blas);
                // The BLAS handle is known when its build has been enqueued: written at replay.
                CommandPatchTraversable* patch = appendCommand<CommandPatchTraversable>(CommandType::PatchTraversable);
                patch->dst = reinterpret_cast<uint64_t*>(&dst.traversableHandle);
                patch->as = blas;
            }

            CUdeviceptr deviceInstances = 0;
            if (!copyToScratch(upload, instancesSize, OPTIX_INSTANCE_BYTE_ALIGNMENT, deviceInstances))
                return;

            const OptixAccelBuildOptions options = ConvertBuildOptions(buildFlags | (as->desc.buildFlags & ~rt::AccelStructBuildFlags::PerformUpdate));
            const OptixBuildInput input = MakeInstanceInput(deviceInstances, uint32_t(numInstances));
            OptixAccelBufferSizes sizes = {};
            if (!NVRHI_OPTIX_CHECK(m_Context, optixAccelComputeMemoryUsage(m_Context.optixContext, &options, &input, 1, &sizes)))
                return;
            const size_t tempSize = options.operation == OPTIX_BUILD_OPERATION_UPDATE ? sizes.tempUpdateSizeInBytes : sizes.tempSizeInBytes;
            CUdeviceptr temp = 0;
            if (tempSize && !allocateScratch(tempSize, OPTIX_ACCEL_BUFFER_BYTE_ALIGNMENT, temp))
                return;

            reference(as);
            CommandBuildAccelStruct* command = appendCommand<CommandBuildAccelStruct>(CommandType::BuildAccelStruct);
            command->as = as;
            command->options = options;
            command->temp = temp;
            command->tempSize = tempSize;
            command->output = as->buffer;
            command->outputSize = as->bufferSize;
            command->instances = deviceInstances;
            command->instanceCount = uint32_t(numInstances);
            command->isTopLevel = 1;
        }
        catch (...)
        {
            m_Context.error("buildTopLevelAccelStruct: out of memory");
        }
    }

    void CommandList::buildTopLevelAccelStructFromBuffer(rt::IAccelStruct* as, nvrhi::IBuffer* instanceBuffer,
        uint64_t instanceBufferOffset, size_t numInstances, rt::AccelStructBuildFlags buildFlags) noexcept
    {
        (void)as; (void)instanceBuffer; (void)instanceBufferOffset; (void)numInstances; (void)buildFlags;
        // OptiX instances hold traversable handles, not the BLAS addresses of rt::InstanceDesc.
        m_Context.error("buildTopLevelAccelStructFromBuffer is not supported on the CUDA backend");
    }

    void CommandList::compactBottomLevelAccelStructs() noexcept
    {
        if (!m_Instance)
            return;
        try
        {
            ContextScope scope(m_Context);
            for (AutoPtr<AccelStruct>& as : m_Device->takeCompactableAccelStructs())
            {
                const size_t size = size_t(*as->compactedSizeHost);
                if (size == 0 || size >= as->bufferSize)
                    continue;
                CUdeviceptr buffer = 0;
                if (!NVRHI_CUDA_CHECK(m_Context, cuMemAlloc(&buffer, size)))
                    continue;
                reference(as);
                CommandCompactAccelStruct* command = appendCommand<CommandCompactAccelStruct>(CommandType::CompactAccelStruct);
                command->as = as;
                command->newBuffer = buffer;
                command->newSize = size;
            }
        }
        catch (...)
        {
            m_Context.error("compactBottomLevelAccelStructs: out of memory");
        }
    }

    void CommandList::copyRaytracingAccelerationStructure(rt::IAccelStruct* destination, rt::IAccelStruct* source) noexcept
    {
        (void)destination; (void)source;
        m_Context.error("copyRaytracingAccelerationStructure is not supported on the CUDA backend");
    }

    //////////////////////////////////////////////////////////////////////////
    // Pipelines
    //////////////////////////////////////////////////////////////////////////

    namespace
    {
        // optixUtilAccumulateStackSizes / optixUtilComputeStackSizes (optix_stack_size.h), written out so that
        // only the static OptiX stubs of this file are used.
        void AccumulateStackSizes(const OptixStackSizes& group, OptixStackSizes& total)
        {
            total.cssRG = std::max(total.cssRG, group.cssRG);
            total.cssMS = std::max(total.cssMS, group.cssMS);
            total.cssCH = std::max(total.cssCH, group.cssCH);
            total.cssAH = std::max(total.cssAH, group.cssAH);
            total.cssIS = std::max(total.cssIS, group.cssIS);
            total.cssCC = std::max(total.cssCC, group.cssCC);
            total.dssDC = std::max(total.dssDC, group.dssDC);
        }
    }

    RayTracingPipeline::~RayTracingPipeline()
    {
        if (pipeline)
            optixPipelineDestroy(pipeline);
        for (Program& program : programs)
        {
            if (program.group)
                optixProgramGroupDestroy(program.group);
        }
        for (OptixModule module : modules)
            optixModuleDestroy(module);
    }

    NativeObject RayTracingPipeline::getNativeObject(ObjectType objectType) noexcept
    {
        return objectType == ObjectTypes::OptiX_Pipeline ? NativeObject(pipeline) : nullptr;
    }

    rt::PipelineHandle Device::createRayTracingPipeline(const rt::PipelineDesc& desc)
    {
        if (!m_Context.optixContext)
        {
            m_Context.error("createRayTracingPipeline: OptiX is not available");
            return nullptr;
        }

        AutoPtr<RayTracingPipeline> pipeline = TakeOver(MAKE_RC_OBJ(RayTracingPipeline, m_Context));
        pipeline->desc = desc;

        // Every OptiX program of the pipeline, to find the libraries and the shader with the launch parameters.
        std::vector<Shader*> shaders;
        auto addShader = [&](IShader* _shader) -> Shader*
        {
            if (!_shader)
                return nullptr;
            Shader* shader = checked_cast<Shader*>(_shader);
            if (!shader || !shader->isOptix)
            {
                m_Context.error("createRayTracingPipeline: a shader is not an OptiX program");
                return nullptr;
            }
            shaders.push_back(shader);
            if (std::find(pipeline->libraries.begin(), pipeline->libraries.end(), shader->library) == pipeline->libraries.end())
                pipeline->libraries.push_back(shader->library);
            return shader;
        };
        for (const rt::PipelineShaderDesc& shader : desc.shaders)
        {
            if (!addShader(shader.shader))
                return nullptr;
        }
        for (const rt::PipelineHitGroupDesc& hitGroup : desc.hitGroups)
        {
            if ((hitGroup.closestHitShader && !addShader(hitGroup.closestHitShader)) ||
                (hitGroup.anyHitShader && !addShader(hitGroup.anyHitShader)) ||
                (hitGroup.intersectionShader && !addShader(hitGroup.intersectionShader)))
                return nullptr;
        }
        if (shaders.empty())
        {
            m_Context.error("createRayTracingPipeline: no shaders");
            return nullptr;
        }

        // The launch parameters: the global parameters of the libraries, which share one layout.
        const Shader* rayGeneration = nullptr;
        for (const rt::PipelineShaderDesc& shader : desc.shaders)
        {
            if (shader.shader && shader.shader->getDesc().shaderType == ShaderType::RayGeneration)
                rayGeneration = checked_cast<Shader*>(shader.shader.Get());
        }
        const Shader* parameterShader = rayGeneration ? rayGeneration : shaders.front();
        if (!pipeline->parameters.initialize(m_Context, parameterShader->reflection, "ray tracing pipeline"))
            return nullptr;

        OptixPipelineCompileOptions pipelineOptions = {};
        pipelineOptions.usesMotionBlur = 0;
        // The acceleration structures of rt::IAccelStruct are one level of instances (a TLAS) over geometry.
        pipelineOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
        // Slang passes the payload by value, one payload value per 32-bit word.
        pipelineOptions.numPayloadValues = std::clamp<int>(int((desc.maxPayloadSize + 3) / 4), 2, 32);
        pipelineOptions.numAttributeValues = std::clamp<int>(int((desc.maxAttributeSize + 3) / 4), 2, 8);
        pipelineOptions.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
        pipelineOptions.pipelineLaunchParamsVariableName = "SLANG_globalParams";
        pipelineOptions.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE | OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM;

        OptixModuleCompileOptions moduleOptions = {};
        moduleOptions.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
        moduleOptions.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
        moduleOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_DEFAULT;

        char log[4096];
        for (const AutoPtr<ShaderLibrary>& library : pipeline->libraries)
        {
            size_t logSize = sizeof(log);
            log[0] = 0;
            OptixModule module = nullptr;
            const OptixResult result = optixModuleCreate(m_Context.optixContext, &moduleOptions, &pipelineOptions,
                library->optix.code.data(), library->optix.code.size(), log, &logSize, &module);
            if (result != OPTIX_SUCCESS)
            {
                m_Context.error(std::string("optixModuleCreate failed: ") + OptixResultToString(result) + "\n" + log);
                return nullptr;
            }
            pipeline->modules.push_back(module);
        }

        auto moduleOf = [&](const Shader* shader) -> OptixModule
        {
            const auto it = std::find(pipeline->libraries.begin(), pipeline->libraries.end(), shader->library);
            return pipeline->modules[size_t(it - pipeline->libraries.begin())];
        };

        // The program groups: one per shader, one per hit group.
        uint32_t maxRecordData = 0;
        auto addProgram = [&](const char* exportName, ShaderType type, const OptixProgramGroupDesc& groupDesc,
            const Shader* recordShader) -> bool
        {
            RayTracingPipeline::Program program;
            program.exportName = exportName;
            program.type = type;
            const std::string what = std::string("ray tracing program ") + exportName;
            if (recordShader && !program.record.initializeEntryPoint(m_Context, recordShader->reflection,
                recordShader->entryPoint, what.c_str()))
                return false;
            program.recordDataSize = program.record.globalsSize;
            maxRecordData = std::max(maxRecordData, program.recordDataSize);

            OptixProgramGroupOptions groupOptions = {};
            size_t logSize = sizeof(log);
            log[0] = 0;
            const OptixResult result = optixProgramGroupCreate(m_Context.optixContext, &groupDesc, 1, &groupOptions,
                log, &logSize, &program.group);
            if (result != OPTIX_SUCCESS)
            {
                m_Context.error(what + ": optixProgramGroupCreate failed: " + OptixResultToString(result) + "\n" + log);
                return false;
            }
            if (!NVRHI_OPTIX_CHECK(m_Context, optixSbtRecordPackHeader(program.group, program.header)))
                return false;
            pipeline->programs.push_back(std::move(program));
            return true;
        };

        for (const rt::PipelineShaderDesc& shaderDesc : desc.shaders)
        {
            const Shader* shader = checked_cast<Shader*>(shaderDesc.shader.Get());
            const ShaderType type = shader->desc.shaderType;
            const char* exportName = shaderDesc.exportName.empty() ? shader->desc.entryName.c_str() : shaderDesc.exportName.c_str();

            OptixProgramGroupDesc groupDesc = {};
            switch (type)
            {
            case ShaderType::RayGeneration:
                groupDesc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
                groupDesc.raygen.module = moduleOf(shader);
                groupDesc.raygen.entryFunctionName = shader->symbolName.c_str();
                break;
            case ShaderType::Miss:
                groupDesc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
                groupDesc.miss.module = moduleOf(shader);
                groupDesc.miss.entryFunctionName = shader->symbolName.c_str();
                break;
            case ShaderType::Callable:
                groupDesc.kind = OPTIX_PROGRAM_GROUP_KIND_CALLABLES;
                groupDesc.callables.moduleDC = moduleOf(shader);
                groupDesc.callables.entryFunctionNameDC = shader->symbolName.c_str();
                break;
            default:
                m_Context.error(std::string("createRayTracingPipeline: shader ") + exportName +
                    " is not a ray generation, miss or callable shader");
                return nullptr;
            }
            if (!addProgram(exportName, type, groupDesc, shader))
                return nullptr;
        }

        for (const rt::PipelineHitGroupDesc& hitGroup : desc.hitGroups)
        {
            const Shader* closestHit = hitGroup.closestHitShader ? checked_cast<Shader*>(hitGroup.closestHitShader.Get()) : nullptr;
            const Shader* anyHit = hitGroup.anyHitShader ? checked_cast<Shader*>(hitGroup.anyHitShader.Get()) : nullptr;
            const Shader* intersection = hitGroup.intersectionShader ? checked_cast<Shader*>(hitGroup.intersectionShader.Get()) : nullptr;

            OptixProgramGroupDesc groupDesc = {};
            groupDesc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
            if (closestHit)
            {
                groupDesc.hitgroup.moduleCH = moduleOf(closestHit);
                groupDesc.hitgroup.entryFunctionNameCH = closestHit->symbolName.c_str();
            }
            if (anyHit)
            {
                groupDesc.hitgroup.moduleAH = moduleOf(anyHit);
                groupDesc.hitgroup.entryFunctionNameAH = anyHit->symbolName.c_str();
            }
            if (intersection)
            {
                groupDesc.hitgroup.moduleIS = moduleOf(intersection);
                groupDesc.hitgroup.entryFunctionNameIS = intersection->symbolName.c_str();
            }
            // The programs of a hit group share its record: the parameters of the first program describe it.
            const Shader* recordShader = closestHit ? closestHit : (anyHit ? anyHit : intersection);
            if (!addProgram(hitGroup.exportName.c_str(), ShaderType::ClosestHit, groupDesc, recordShader))
                return nullptr;
        }

        pipeline->recordStride = uint32_t((OPTIX_SBT_RECORD_HEADER_SIZE + maxRecordData + OPTIX_SBT_RECORD_ALIGNMENT - 1) &
            ~(OPTIX_SBT_RECORD_ALIGNMENT - 1));

        std::vector<OptixProgramGroup> groups;
        for (const auto& program : pipeline->programs)
            groups.push_back(program.group);

        OptixPipelineLinkOptions linkOptions = {};
        linkOptions.maxTraceDepth = std::max(desc.maxRecursionDepth, 1u);
        size_t logSize = sizeof(log);
        log[0] = 0;
        const OptixResult result = optixPipelineCreate(m_Context.optixContext, &pipelineOptions, &linkOptions,
            groups.data(), uint32_t(groups.size()), log, &logSize, &pipeline->pipeline);
        if (result != OPTIX_SUCCESS)
        {
            m_Context.error(std::string("optixPipelineCreate failed: ") + OptixResultToString(result) + "\n" + log);
            return nullptr;
        }

        // Stack sizes for maxTraceDepth levels of recursion, no continuation or direct callables nesting.
        OptixStackSizes stackSizes = {};
        for (const auto& program : pipeline->programs)
        {
            OptixStackSizes groupSizes = {};
            if (!NVRHI_OPTIX_CHECK(m_Context, optixProgramGroupGetStackSize(program.group, &groupSizes, pipeline->pipeline)))
                return nullptr;
            AccumulateStackSizes(groupSizes, stackSizes);
        }
        const unsigned int maxTraceDepth = linkOptions.maxTraceDepth;
        const unsigned int cssCHOrMSPlusIS = std::max(stackSizes.cssCH, stackSizes.cssMS + stackSizes.cssIS);
        const unsigned int continuationStackSize = stackSizes.cssRG + std::max(maxTraceDepth, 1u) * std::max(cssCHOrMSPlusIS, stackSizes.cssAH);
        if (!NVRHI_OPTIX_CHECK(m_Context, optixPipelineSetStackSize(pipeline->pipeline, stackSizes.dssDC, stackSizes.dssDC,
            continuationStackSize, 2)))
            return nullptr;

        return pipeline;
    }

    //////////////////////////////////////////////////////////////////////////
    // Dispatch
    //////////////////////////////////////////////////////////////////////////

    void CommandList::setRayTracingState(const rt::State& state) noexcept
    {
        if (!m_Instance)
            return;
        try
        {
            m_RayTracingState = state;
            m_RayTracingStateValid = state.shaderTable != nullptr;
            reference(state.shaderTable);
            for (IBindingSet* set : state.bindings)
                reference(set);
        }
        catch (...)
        {
            m_Context.error("setRayTracingState: out of memory");
        }
    }

    bool CommandList::buildShaderBindingTable(ShaderTable* table, CommandLaunchRays& launch)
    {
        RayTracingPipeline* pipeline = table->pipeline;
        if (!table->hasRayGeneration)
        {
            m_Context.error("dispatchRays: the shader table has no ray generation shader");
            return false;
        }

        const size_t stride = pipeline->recordStride;
        const size_t count = table->getNumEntries();
        TransientAllocation upload;
        if (!allocateUpload(count * stride, OPTIX_SBT_RECORD_ALIGNMENT, upload))
            return false;
        memset(upload.hostPtr, 0, count * stride);

        bool ok = true;
        size_t index = 0;
        auto writeRecord = [&](const ShaderTable::Entry& entry)
        {
            const RayTracingPipeline::Program& program = pipeline->programs[entry.programIndex];
            uint8_t* record = upload.hostPtr + index * stride;
            memcpy(record, program.header, OPTIX_SBT_RECORD_HEADER_SIZE);
            if (entry.bindings && program.recordDataSize > 0)
            {
                reference(entry.bindings);
                BindingSetVector bindings;
                bindings.push_back(entry.bindings);
                const std::string what = std::string("shader record of ") + program.exportName.c_str();
                ok = writeGlobals(program.record, bindings, record + OPTIX_SBT_RECORD_HEADER_SIZE, what.c_str()) && ok;
            }
            ++index;
        };

        writeRecord(table->rayGeneration);
        for (const auto& entry : table->missShaders)
            writeRecord(entry);
        for (const auto& entry : table->hitGroups)
            writeRecord(entry);
        for (const auto& entry : table->callableShaders)
            writeRecord(entry);
        if (!ok)
            return false;

        CUdeviceptr sbt = 0;
        if (!copyToScratch(upload, count * stride, OPTIX_SBT_RECORD_ALIGNMENT, sbt))
            return false;

        OptixShaderBindingTable& out = launch.sbt;
        out = OptixShaderBindingTable();
        out.raygenRecord = sbt;
        CUdeviceptr next = sbt + stride;
        auto setRange = [&](CUdeviceptr& base, unsigned int& strideOut, unsigned int& countOut, size_t entries)
        {
            base = entries ? next : 0;
            strideOut = entries ? unsigned(stride) : 0;
            countOut = unsigned(entries);
            next += entries * stride;
        };
        setRange(out.missRecordBase, out.missRecordStrideInBytes, out.missRecordCount, table->missShaders.size());
        setRange(out.hitgroupRecordBase, out.hitgroupRecordStrideInBytes, out.hitgroupRecordCount, table->hitGroups.size());
        setRange(out.callablesRecordBase, out.callablesRecordStrideInBytes, out.callablesRecordCount, table->callableShaders.size());
        return true;
    }

    void CommandList::dispatchRays(const rt::DispatchRaysArguments& args) noexcept
    {
        if (!m_Instance || !m_RayTracingStateValid)
        {
            m_Context.error("dispatchRays: no ray tracing state");
            return;
        }

        try
        {
            ShaderTable* table = checked_cast<ShaderTable*>(m_RayTracingState.shaderTable);
            RayTracingPipeline* pipeline = table->pipeline;

            CommandLaunchRays launch = {};
            launch.pipeline = pipeline;
            launch.width = args.width;
            launch.height = args.height;
            launch.depth = args.depth;

            const size_t globalsSize = pipeline->parameters.globalsSize;
            if (globalsSize > 0)
            {
                TransientAllocation globals;
                if (!allocateUpload(globalsSize, 16, globals))
                    return;
                memset(globals.hostPtr, 0, globalsSize);
                if (!writeGlobals(pipeline->parameters, m_RayTracingState.bindings, globals.hostPtr, "dispatchRays"))
                    return;
                launch.paramsSrc = globals.devicePtr;
                launch.paramsSize = globalsSize;
                if (!allocateScratch(globalsSize, 16, launch.params))
                    return;
            }

            if (!buildShaderBindingTable(table, launch))
                return;

            CommandLaunchRays* command = appendCommand<CommandLaunchRays>(CommandType::LaunchRays);
            const CommandHeader header = command->header;
            *command = launch;
            command->header = header;
        }
        catch (...)
        {
            m_Context.error("dispatchRays: out of memory");
        }
    }

    //////////////////////////////////////////////////////////////////////////
    // Replay
    //////////////////////////////////////////////////////////////////////////

    bool ExecuteRayTracingCommand(const Context& context, CUstream stream, const CommandHeader& header,
        CommandListInstance& instance)
    {
        switch (header.type)
        {
        case CommandType::BuildAccelStruct:
        {
            const auto& command = reinterpret_cast<const CommandBuildAccelStruct&>(header);
            OptixTraversableHandle handle = 0;
            OptixAccelEmitDesc emit = {};
            if (command.emitCompactedSize)
            {
                emit.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
                emit.result = command.as->compactedSizeDevice;
            }

            OptixResult result;
            if (command.isTopLevel)
            {
                const OptixBuildInput input = MakeInstanceInput(command.instances, command.instanceCount);
                result = optixAccelBuild(context.optixContext, stream, &command.options, &input, 1, command.temp,
                    command.tempSize, command.output, command.outputSize, &handle, nullptr, 0);
            }
            else
            {
                // The records follow the command; the build inputs point into them.
                GeometryRecord* records = const_cast<GeometryRecord*>(reinterpret_cast<const GeometryRecord*>(&command + 1));
                std::vector<OptixBuildInput> inputs;
                MakeBuildInputs(records, command.geometryCount, inputs);
                result = optixAccelBuild(context.optixContext, stream, &command.options, inputs.data(),
                    uint32_t(inputs.size()), command.temp, command.tempSize, command.output, command.outputSize,
                    &handle, command.emitCompactedSize ? &emit : nullptr, command.emitCompactedSize ? 1 : 0);
            }
            if (!OptixCheck(context, result, "optixAccelBuild"))
                return false;

            command.as->traversable = handle;
            if (command.emitCompactedSize)
            {
                command.as->buildInstance = instance.submission;
                command.as->buildQueue = instance.queue;
            }
            return true;
        }

        case CommandType::CompactAccelStruct:
        {
            const auto& command = reinterpret_cast<const CommandCompactAccelStruct&>(header);
            OptixTraversableHandle handle = 0;
            if (!OptixCheck(context, optixAccelCompact(context.optixContext, stream, command.as->traversable,
                command.newBuffer, command.newSize, &handle), "optixAccelCompact"))
            {
                instance.deferredFrees.push_back(command.newBuffer);
                return false;
            }
            // The old buffer is read by the compaction: freed when the submission has finished.
            instance.deferredFrees.push_back(command.as->buffer);
            command.as->buffer = command.newBuffer;
            command.as->bufferSize = command.newSize;
            command.as->traversable = handle;
            command.as->compacted = true;
            return true;
        }

        case CommandType::LaunchRays:
        {
            const auto& command = reinterpret_cast<const CommandLaunchRays&>(header);
            if (command.paramsSize &&
                !NVRHI_CUDA_CHECK(context, cuMemcpyAsync(command.params, command.paramsSrc, command.paramsSize, stream)))
                return false;
            return OptixCheck(context, optixLaunch(command.pipeline->pipeline, stream, command.params, command.paramsSize,
                &command.sbt, command.width, command.height, command.depth), "optixLaunch");
        }

        default:
            context.error("unsupported ray tracing command");
            return false;
        }
    }

#else // NVRHI_WITH_OPTIX

    bool InitializeOptix(Context& context, bool enableValidation, std::string& error)
    {
        (void)context; (void)enableValidation;
        error = "NVRHI was built without OptiX";
        return false;
    }

    void ShutdownOptix(Context& context)
    {
        (void)context;
    }

    RayTracingPipeline::~RayTracingPipeline() = default;

    NativeObject RayTracingPipeline::getNativeObject(ObjectType objectType) noexcept
    {
        (void)objectType;
        return nullptr;
    }

    bool Device::queryTopLevelAccelStructPrebuildInfo(const rt::AccelStructDesc& desc, uint32_t instanceCount,
        rt::AccelStructPrebuildInfo& outInfo) noexcept
    {
        (void)desc; (void)instanceCount; (void)outInfo;
        return false;
    }

    rt::AccelStructHandle Device::createAccelStruct(const rt::AccelStructDesc& desc)
    {
        (void)desc;
        m_Context.error("createAccelStruct: NVRHI was built without OptiX");
        return nullptr;
    }

    rt::PipelineHandle Device::createRayTracingPipeline(const rt::PipelineDesc& desc)
    {
        (void)desc;
        m_Context.error("createRayTracingPipeline: NVRHI was built without OptiX");
        return nullptr;
    }

    void CommandList::buildBottomLevelAccelStruct(rt::IAccelStruct* as, const rt::GeometryDesc* pGeometries,
        size_t numGeometries, rt::AccelStructBuildFlags buildFlags) noexcept
    {
        (void)as; (void)pGeometries; (void)numGeometries; (void)buildFlags;
        utils::NotSupported();
    }

    void CommandList::buildTopLevelAccelStruct(rt::IAccelStruct* as, const rt::InstanceDesc* pInstances,
        size_t numInstances, rt::AccelStructBuildFlags buildFlags) noexcept
    {
        (void)as; (void)pInstances; (void)numInstances; (void)buildFlags;
        utils::NotSupported();
    }

    void CommandList::buildTopLevelAccelStructFromBuffer(rt::IAccelStruct* as, nvrhi::IBuffer* instanceBuffer,
        uint64_t instanceBufferOffset, size_t numInstances, rt::AccelStructBuildFlags buildFlags) noexcept
    {
        (void)as; (void)instanceBuffer; (void)instanceBufferOffset; (void)numInstances; (void)buildFlags;
        utils::NotSupported();
    }

    void CommandList::compactBottomLevelAccelStructs() noexcept { }

    void CommandList::copyRaytracingAccelerationStructure(rt::IAccelStruct* destination, rt::IAccelStruct* source) noexcept
    {
        (void)destination; (void)source;
        utils::NotSupported();
    }

    void CommandList::setRayTracingState(const rt::State& state) noexcept
    {
        (void)state;
        m_Context.error("ray tracing needs NVRHI built with OptiX");
    }

    bool CommandList::buildShaderBindingTable(ShaderTable* table, CommandLaunchRays& launch)
    {
        (void)table; (void)launch;
        return false;
    }

    void CommandList::dispatchRays(const rt::DispatchRaysArguments& args) noexcept
    {
        (void)args;
        utils::NotSupported();
    }

    bool ExecuteRayTracingCommand(const Context& context, CUstream stream, const CommandHeader& header,
        CommandListInstance& instance)
    {
        (void)stream; (void)header; (void)instance;
        context.error("ray tracing needs NVRHI built with OptiX");
        return false;
    }

#endif // NVRHI_WITH_OPTIX
}
