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

#pragma once

// The CUDA and OptiX backend (ADR 0011).
//
// - One CUstream per CommandQueue. A command list records commands into a byte stream; resource bindings, push
//   constants and upload data are resolved while recording, into transient memory owned by the recording.
//   executeCommandLists replays the commands on the stream of the queue and records a CUevent per submission.
// - The global parameters of a Slang kernel live in its module's SLANG_globalParams constant; each queue loads
//   its own copy of a module, so that the queues never race on that constant.
// - The device implements IInteropDevice through the InteropDevice mixin (ADR 0013).

#include <nvrhi/cuda.h>
#include <nvrhi/interop.h>
#include <nvrhi/shader-reflection.h>

#include "cuda-driver.h"
#include "../common/utils-internal.h"
#include "../common/shader-container.h"
#include "../common/backend-factories.h"

#include <array>
#include <deque>
#include <mutex>
#include <new>
#include <type_traits>
#include <string>
#include <unordered_map>
#include <vector>

#if NVRHI_WITH_OPTIX
#include <optix.h>
#endif

namespace nvrhi::cuda
{
    class Device;
    class Queue;
    class Buffer;
    class Texture;
    class CommandList;
    class AccelStruct;

    constexpr uint32_t c_NumQueues = uint32_t(CommandQueue::Count);

    //////////////////////////////////////////////////////////////////////////
    // Context
    //////////////////////////////////////////////////////////////////////////

    // State shared by every object of a device. Lives as long as the device; objects keep a reference to the
    // device (as the other backends do with their contexts, through the device's lifetime contract).
    struct Context
    {
        CUdevice device = 0;
        CUcontext context = nullptr;
        int32_t ordinal = 0;
        uint32_t computeCapability = 0;
        uint32_t textureAlignment = 512;
        MessageCallbackHandle messageCallback;
#if NVRHI_WITH_OPTIX
        OptixDeviceContext optixContext = nullptr;
#endif

        void message(MessageSeverity severity, const std::string& text) const;
        void error(const std::string& text) const { message(MessageSeverity::Error, text); }
        void warning(const std::string& text) const { message(MessageSeverity::Warning, text); }
        // Reports a failed call; returns true on success.
        bool check(CUresult result, const char* what) const;
    };

#define NVRHI_CUDA_CHECK(ctx, call) (ctx).check(g_Cuda.call, #call)

    // Makes the context current on the calling thread for the lifetime of the scope.
    class ContextScope
    {
    public:
        explicit ContextScope(const Context& context);
        ~ContextScope();
        ContextScope(const ContextScope&) = delete;
        ContextScope& operator=(const ContextScope&) = delete;
    private:
        bool m_Pushed = false;
    };

    //////////////////////////////////////////////////////////////////////////
    // Formats
    //////////////////////////////////////////////////////////////////////////

    struct FormatMapping
    {
        Format format = Format::UNKNOWN;
        CUarray_format arrayFormat = CUarray_format(0); // 0: no CUDA array format
        uint8_t channels = 0;
        uint8_t bytesPerElement = 0;    // per texel, or per block for compressed formats
        uint8_t blockSize = 1;          // 4 for the BC formats
        bool normalizedRead = false;    // UNORM/SNORM: read as float
        bool srgb = false;
        bool integer = false;           // UINT/SINT: no filtering
    };

    // The mapping of an NVRHI format; arrayFormat is 0 for formats CUDA arrays cannot hold (3-component formats,
    // depth-stencil formats, ...).
    const FormatMapping& GetFormatMapping(Format format);

    //////////////////////////////////////////////////////////////////////////
    // Transient memory (cuda-transient-heap.cpp)
    //////////////////////////////////////////////////////////////////////////

    enum class TransientMemory : uint8_t
    {
        Upload,     // pinned host memory, also mapped to the device
        Device      // device memory
    };

    struct TransientChunk
    {
        TransientMemory kind = TransientMemory::Upload;
        size_t size = 0;
        size_t used = 0;
        CUdeviceptr devicePtr = 0;
        uint8_t* hostPtr = nullptr;     // Upload chunks only
    };

    // The chunks of a device. A recording takes chunks from the pool and returns them when its submission has
    // finished executing (or when the recording is discarded).
    class TransientPool
    {
    public:
        explicit TransientPool(const Context& context) : m_Context(context) { }
        ~TransientPool() { destroyAll(); }

        TransientChunk* acquire(TransientMemory kind, size_t minSize);
        void release(TransientChunk* chunk);
        // Frees every chunk; the device calls it before it releases the CUDA context.
        void destroyAll();

    private:
        const Context& m_Context;
        std::mutex m_Mutex;
        std::vector<TransientChunk*> m_All;
        std::vector<TransientChunk*> m_Free;
    };

    struct TransientAllocation
    {
        uint8_t* hostPtr = nullptr;     // Upload memory only
        CUdeviceptr devicePtr = 0;
    };

    // Suballocates transient memory for one recording.
    class TransientAllocator
    {
    public:
        explicit TransientAllocator(TransientPool& pool, size_t chunkSize) : m_Pool(pool), m_ChunkSize(chunkSize) { }
        ~TransientAllocator() { releaseAll(); }

        // Returns false if memory runs out.
        bool allocate(TransientMemory kind, size_t size, size_t alignment, TransientAllocation& outAllocation);
        // Moves the chunks of the recording into 'chunks' (to keep them alive until the submission finishes).
        void detachChunks(std::vector<TransientChunk*>& chunks);
        void releaseAll();

    private:
        TransientPool& m_Pool;
        size_t m_ChunkSize;
        std::vector<TransientChunk*> m_Chunks;
        std::array<TransientChunk*, 2> m_Current{};
    };

    //////////////////////////////////////////////////////////////////////////
    // Resources
    //////////////////////////////////////////////////////////////////////////

    NVRHI_CLASS_CLSID(Buffer, "68545f14-c6f2-457d-abf8-3f8cc40ed6bc")
    class Buffer : public ObjectImpl<IBuffer>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Buffer)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Buffer)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IBuffer)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Buffer)
        NVRHI_END_INTERFACE_TABLE()

        BufferDesc desc;
        CUdeviceptr devicePtr = 0;
        void* hostPtr = nullptr;            // cpuAccess buffers: pinned host memory
        bool ownsMemory = true;             // false for native handles and imports
        CUexternalMemory externalMemory = nullptr;
        RHIObjectHandle importedResource;   // the graphics buffer an import aliases

        explicit Buffer(const Context& context) : m_Context(context) { }
        ~Buffer();

        const BufferDesc& getDesc() const noexcept override { return desc; }
        GpuVirtualAddress getGpuVirtualAddress() const noexcept override { return GpuVirtualAddress(devicePtr); }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;

    private:
        const Context& m_Context;
    };

    struct TextureObjectKey
    {
        TextureSubresourceSet subresources;
        Format format = Format::UNKNOWN;
        uint32_t samplerHash = 0;
        bool operator==(const TextureObjectKey& o) const
        {
            return subresources == o.subresources && format == o.format && samplerHash == o.samplerHash;
        }
    };

    struct TextureObjectKeyHash
    {
        size_t operator()(const TextureObjectKey& k) const;
    };

    NVRHI_CLASS_CLSID(Texture, "77a9956e-f3c0-4f07-b95b-cfa40f577407")
    class Texture : public ObjectImpl<ITexture>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Texture)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Texture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ITexture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Texture)
        NVRHI_END_INTERFACE_TABLE()

        TextureDesc desc;
        CUmipmappedArray mipmappedArray = nullptr;
        std::vector<CUarray> levels;            // one CUarray per mip level (all array slices / depth)
        std::vector<CUsurfObject> surfaces;     // one per mip level, when the texture is a UAV
        bool ownsMemory = true;
        CUexternalMemory externalMemory = nullptr;
        RHIObjectHandle importedResource;       // the graphics texture an import aliases

        explicit Texture(const Context& context) : m_Context(context) { }
        ~Texture();

        // Creates the levels' surface objects; false on failure.
        bool initialize();

        const TextureDesc& getDesc() const noexcept override { return desc; }
        NativeObject getNativeView(ObjectType objectType, Format format, const TextureSubresourceSet& subresources,
            TextureDimension dimension, bool isReadOnlyDSV, const ComponentMapping* overrideComponentMapping) noexcept override;
        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        // A texture object for sampling, cached by subresources, format and sampler (null sampler: point/clamp).
        CUtexObject getTextureObject(const TextureSubresourceSet& subresources, Format format, const SamplerDesc* sampler);
        CUsurfObject getSurfaceObject(MipLevel mipLevel) const;

    private:
        const Context& m_Context;
        std::mutex m_Mutex;
        std::unordered_map<TextureObjectKey, CUtexObject, TextureObjectKeyHash> m_TextureObjects;
    };

    NVRHI_CLASS_CLSID(StagingTexture, "f11dda3f-1fdf-4225-a0d6-06a4ad23a69e")
    class StagingTexture : public ObjectImpl<IStagingTexture>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(StagingTexture)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(StagingTexture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IStagingTexture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(StagingTexture)
        NVRHI_END_INTERFACE_TABLE()

        // Tightly packed rows (rounded up to whole blocks), slices, mip levels; array slices outermost.
        struct SliceRegion
        {
            size_t offset = 0;
            size_t rowPitch = 0;
            size_t depthPitch = 0;
        };

        TextureDesc desc;
        CpuAccessMode cpuAccess = CpuAccessMode::None;
        uint8_t* hostPtr = nullptr;
        size_t size = 0;

        explicit StagingTexture(const Context& context) : m_Context(context) { }
        ~StagingTexture();

        bool initialize();
        SliceRegion getSliceRegion(MipLevel mipLevel, ArraySlice arraySlice) const;

        const TextureDesc& getDesc() const noexcept override { return desc; }

    private:
        const Context& m_Context;
    };

    NVRHI_CLASS_CLSID(Sampler, "dc5a655f-5dd8-4e03-8f63-d94f955a4991")
    class Sampler : public ObjectImpl<ISampler>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Sampler)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Sampler)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ISampler)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Sampler)
        NVRHI_END_INTERFACE_TABLE()

        SamplerDesc desc;
        const SamplerDesc& getDesc() const noexcept override { return desc; }
    };

    //////////////////////////////////////////////////////////////////////////
    // Shaders (cuda-shader.cpp)
    //////////////////////////////////////////////////////////////////////////

    NVRHI_CLASS_CLSID(ShaderLibrary, "8ee24019-3ea4-4250-8d44-22bd2e0c958f")
    class ShaderLibrary : public ObjectImpl<IShaderLibrary>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(ShaderLibrary)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ShaderLibrary)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IShaderLibrary)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(ShaderLibrary)
        NVRHI_END_INTERFACE_TABLE()

        // The code of one kind of program, with its reflection: a container may hold both.
        struct Part
        {
            ShaderCodeTarget target = ShaderCodeTarget::Unknown;
            std::vector<char> code;                     // PTX (NUL-terminated) or OptiX-IR
            MonoPtr<ShaderLibraryReflection> reflection;
            bool empty() const { return code.empty(); }
        };

        Part compute;   // CUDA kernels
        Part optix;     // OptiX programs

        explicit ShaderLibrary(const Context& context) : m_Context(context) { }
        ~ShaderLibrary();

        // Reads a container (or plain PTX, taken as compute code); false with a message on failure.
        bool initialize(const void* binary, size_t binarySize, std::string& error);

        // The module of the compute code loaded for one queue (each queue has its own SLANG_globalParams);
        // nullptr on failure.
        CUmodule getModule(CommandQueue queue);

        void getBytecode(const void** ppBytecode, size_t* pSize) const noexcept override;
        const ShaderLibraryReflection* getReflection() const noexcept override
        {
            return compute.reflection ? compute.reflection.Get() : optix.reflection.Get();
        }
        ShaderHandle getShader(const char* entryName, ShaderType shaderType, const ShaderDesc* desc = nullptr);
        FRESULT getShader(const char* entryName, ShaderType shaderType, IShader** ppShader) noexcept override { return utils::ReturnObject(ppShader, [&] { return getShader(entryName, shaderType); }); }

    private:
        const Context& m_Context;
        std::mutex m_Mutex;
        std::array<CUmodule, c_NumQueues> m_Modules{};
    };

    NVRHI_CLASS_CLSID(Shader, "72c1148e-60f1-447b-be1a-32c8055ec78c")
    class Shader : public ObjectImpl<IShader>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Shader)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Shader)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IShader)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Shader)
        NVRHI_END_INTERFACE_TABLE()

        ShaderDesc desc;
        AutoPtr<ShaderLibrary> library;
        bool isOptix = false;       // an OptiX program (library->optix) or a CUDA kernel (library->compute)
        const ShaderLibraryReflection* reflection = nullptr;   // of that part of the library
        const ShaderEntryPointReflection* entryPoint = nullptr;
        std::string symbolName;     // the kernel or OptiX program in the code

        const ShaderLibrary::Part& getPart() const { return isOptix ? library->optix : library->compute; }

        const ShaderDesc& getDesc() const noexcept override { return desc; }
        void getBytecode(const void** ppBytecode, size_t* pSize) const noexcept override
        {
            const ShaderLibrary::Part& part = getPart();
            if (ppBytecode) *ppBytecode = part.code.data();
            if (pSize) *pSize = part.code.size();
        }
        const ShaderEntryPointReflection* getReflection() const noexcept override { return entryPoint; }
        const ShaderLibraryReflection* getLibraryReflection() const noexcept override { return reflection; }
    };

    //////////////////////////////////////////////////////////////////////////
    // Resource bindings (cuda-resource-bindings.cpp)
    //////////////////////////////////////////////////////////////////////////

    NVRHI_CLASS_CLSID(BindingLayout, "cf904cd2-e57a-4529-9ba8-dd1a83374bd9")
    class BindingLayout : public ObjectImpl<IBindingLayout>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(BindingLayout)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(BindingLayout)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IBindingLayout)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(BindingLayout)
        NVRHI_END_INTERFACE_TABLE()

        BindingLayoutDesc desc;

        const BindingLayoutDesc* getDesc() const noexcept override { return &desc; }
        const BindlessLayoutDesc* getBindlessDesc() const noexcept override { return nullptr; }
    };

    NVRHI_CLASS_CLSID(BindingSet, "d49483e5-759f-4c58-bc96-d5be89266f09")
    class BindingSet : public ObjectImpl<IBindingSet>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(BindingSet)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(BindingSet)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IBindingSet)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(BindingSet)
        NVRHI_END_INTERFACE_TABLE()

        BindingSetDesc desc;
        BindingLayoutHandle layout;
        std::vector<RHIObjectHandle> resources;     // keeps the bound resources alive

        const BindingSetDesc* getDesc() const noexcept override { return &desc; }
        IBindingLayout* getLayout() const noexcept override { return layout; }
    };

    // The HLSL register class of a binding: t, u, b or s (and push constants).
    enum class RegisterClass : uint8_t
    {
        SRV,
        UAV,
        CBV,
        Sampler,
        PushConstants,
        None
    };

    RegisterClass GetRegisterClass(ResourceType type);
    RegisterClass GetRegisterClass(ShaderParameterKind kind);

    // Where a global parameter of a kernel's SLANG_globalParams block is bound from.
    struct ParameterSlot
    {
        uint32_t parameterIndex = 0;    // in the reflection
        RegisterClass registerClass = RegisterClass::None;
        uint32_t registerSpace = 0;
        uint32_t firstSlot = 0;
        uint32_t slotCount = 1;
        uint32_t offset = 0;            // in the global block
        uint32_t stride = 0;            // between array elements
        ShaderParameterKind kind = ShaderParameterKind::Uniform;
    };

    // The global parameters of an entry point, by register.
    class ParameterMap
    {
    public:
        // Builds the map from the reflection; fails (with a message) if the reflection describes no CUDA layout.
        bool initialize(const Context& context, const ShaderLibraryReflection* reflection, const char* what);
        // The map of an entry point's parameters (an OptiX SBT record). Their register spaces are ignored: a
        // record is bound from one local binding set.
        bool initializeEntryPoint(const Context& context, const ShaderLibraryReflection* reflection,
            const ShaderEntryPointReflection* entryPoint, const char* what);
        const ParameterSlot* find(RegisterClass registerClass, uint32_t registerSpace, uint32_t slot) const;
        const std::vector<ParameterSlot>& getSlots() const { return m_Slots; }

        const ShaderLibraryReflection* reflection = nullptr;
        uint32_t globalsSize = 0;
    private:
        std::vector<ParameterSlot> m_Slots;
        bool m_IgnoreSpace = false;
        void addParameter(const Context& context, uint32_t index, const char* what);
    };

    //////////////////////////////////////////////////////////////////////////
    // Compute (cuda-compute.cpp)
    //////////////////////////////////////////////////////////////////////////

    // One kernel loaded on one queue.
    struct KernelInstance
    {
        CUfunction function = nullptr;
        CUdeviceptr globals = 0;        // SLANG_globalParams
        size_t globalsSize = 0;
    };

    NVRHI_CLASS_CLSID(ComputePipeline, "04c21029-6c9b-4809-88f6-8908b493046b")
    class ComputePipeline : public ObjectImpl<IComputePipeline>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(ComputePipeline)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ComputePipeline)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IComputePipeline)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(ComputePipeline)
        NVRHI_END_INTERFACE_TABLE()

        ComputePipelineDesc desc;
        AutoPtr<Shader> shader;
        ParameterMap parameters;
        std::array<uint32_t, 3> blockSize{ 1, 1, 1 };
        uint32_t paramsSize = 0;        // the entry point's argument block (push constants)

        explicit ComputePipeline(const Context& context) : m_Context(context) { }

        // The kernel on 'queue', loading its module on first use; false on failure.
        bool getKernel(CommandQueue queue, KernelInstance& outKernel);

        const ComputePipelineDesc& getDesc() const noexcept override { return desc; }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;

    private:
        const Context& m_Context;
        std::mutex m_Mutex;
        std::array<KernelInstance, c_NumQueues> m_Kernels{};
    };

    //////////////////////////////////////////////////////////////////////////
    // Queries (cuda-queue.cpp)
    //////////////////////////////////////////////////////////////////////////

    NVRHI_CLASS_CLSID(EventQuery, "11760449-9835-410e-b65b-4c7330ab4aa0")
    class EventQuery : public ObjectImpl<IEventQuery>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(EventQuery)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(EventQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IEventQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(EventQuery)
        NVRHI_END_INTERFACE_TABLE()

        CUevent event = nullptr;
        bool started = false;

        explicit EventQuery(const Context& context) : m_Context(context) { }
        ~EventQuery();
    private:
        const Context& m_Context;
    };

    NVRHI_CLASS_CLSID(TimerQuery, "4c8476ba-9151-46ec-8beb-960be5545b31")
    class TimerQuery : public ObjectImpl<ITimerQuery>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(TimerQuery)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(TimerQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ITimerQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(TimerQuery)
        NVRHI_END_INTERFACE_TABLE()

        CUevent begin = nullptr;
        CUevent end = nullptr;
        bool started = false;
        bool resolved = false;
        float time = 0.f;

        explicit TimerQuery(const Context& context) : m_Context(context) { }
        ~TimerQuery();
    private:
        const Context& m_Context;
    };

    //////////////////////////////////////////////////////////////////////////
    // Ray tracing (cuda-optix.cpp)
    //////////////////////////////////////////////////////////////////////////

    NVRHI_CLASS_CLSID(AccelStruct, "79b35e27-7447-46b7-ad02-74557ecd670b")
    class AccelStruct : public ObjectImpl<rt::IAccelStruct>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(AccelStruct)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(AccelStruct)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::rt::IAccelStruct)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(AccelStruct)
        NVRHI_END_INTERFACE_TABLE()

        rt::AccelStructDesc desc;
        CUdeviceptr buffer = 0;         // the acceleration structure
        size_t bufferSize = 0;
        size_t scratchSize = 0;         // for builds, from optixAccelComputeMemoryUsage
        size_t updateScratchSize = 0;
        uint64_t traversable = 0;       // OptixTraversableHandle, set by the build (at replay)
        bool compacted = false;
        // Compaction: the emitted compacted size, written by the build into compactedSizeBuffer (mapped host memory).
        uint64_t* compactedSizeHost = nullptr;
        CUdeviceptr compactedSizeDevice = 0;
        uint64_t buildInstance = 0;     // submission of the last build that emitted the size
        CommandQueue buildQueue = CommandQueue::Graphics;
        bool compactionPending = false;
        std::vector<rt::InstanceDesc> instances;   // the last TLAS build (keeps the BLAS references alive)
        std::vector<RHIObjectHandle> bottomLevelReferences;

        explicit AccelStruct(const Context& context) : m_Context(context) { }
        ~AccelStruct();

        const rt::AccelStructDesc& getDesc() const noexcept override { return desc; }
        bool isCompacted() const noexcept override { return compacted; }
        uint64_t getDeviceAddress() const noexcept override { return uint64_t(buffer); }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;

    private:
        const Context& m_Context;
    };

    class RayTracingPipeline;

    NVRHI_CLASS_CLSID(ShaderTable, "72bd0ab6-41c4-4df2-8d0e-9a1dfd29d126")
    class ShaderTable : public ObjectImpl<rt::IShaderTable>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(ShaderTable)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ShaderTable)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::rt::IShaderTable)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(ShaderTable)
        NVRHI_END_INTERFACE_TABLE()

        struct Entry
        {
            uint32_t programIndex = 0;      // into RayTracingPipeline::programs
            BindingSetHandle bindings;      // local bindings: the SBT record data
        };

        rt::ShaderTableDesc desc;
        AutoPtr<RayTracingPipeline> pipeline;
        Entry rayGeneration;
        bool hasRayGeneration = false;
        std::vector<Entry> missShaders;
        std::vector<Entry> hitGroups;
        std::vector<Entry> callableShaders;

        const rt::ShaderTableDesc& getDesc() const noexcept override { return desc; }
        uint32_t getNumEntries() const noexcept override;
        rt::IPipeline* getPipeline() const noexcept override;
        void setRayGenerationShader(const char* exportName, IBindingSet* bindings) noexcept override;
        int addMissShader(const char* exportName, IBindingSet* bindings) noexcept override;
        int addHitGroup(const char* exportName, IBindingSet* bindings) noexcept override;
        int addCallableShader(const char* exportName, IBindingSet* bindings) noexcept override;
        void clearMissShaders() noexcept override { missShaders.clear(); }
        void clearHitShaders() noexcept override { hitGroups.clear(); }
        void clearCallableShaders() noexcept override { callableShaders.clear(); }
    };

    NVRHI_CLASS_CLSID(RayTracingPipeline, "221ce299-a3f3-4ca6-a64d-ef2e62ab8438")
    class RayTracingPipeline : public ObjectImpl<rt::IPipeline>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(RayTracingPipeline)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(RayTracingPipeline)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::rt::IPipeline)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(RayTracingPipeline)
        NVRHI_END_INTERFACE_TABLE()

        // One OptiX program group: a ray generation, miss or callable shader, or a hit group.
        struct Program
        {
            string exportName;
            ShaderType type = ShaderType::None;     // RayGeneration, Miss, Callable or ClosestHit for hit groups
            ParameterMap record;                    // the record data: the parameters of the program's entry point
            uint32_t recordDataSize = 0;
#if NVRHI_WITH_OPTIX
            OptixProgramGroup group = nullptr;
            alignas(OPTIX_SBT_RECORD_ALIGNMENT) uint8_t header[OPTIX_SBT_RECORD_HEADER_SIZE] = {};
#endif
        };

        rt::PipelineDesc desc;
        ParameterMap parameters;            // the global (launch) parameters
        std::vector<Program> programs;
        std::vector<AutoPtr<ShaderLibrary>> libraries;
        uint32_t recordStride = 0;          // one stride for every record of the table, header included
#if NVRHI_WITH_OPTIX
        std::vector<OptixModule> modules;
        OptixPipeline pipeline = nullptr;
#endif

        explicit RayTracingPipeline(const Context& context) : m_Context(context) { }
        ~RayTracingPipeline();

        int findProgram(const char* exportName, ShaderType type) const;

        const rt::PipelineDesc& getDesc() const noexcept override { return desc; }
        rt::ShaderTableHandle createShaderTable(const rt::ShaderTableDesc& desc);
        FRESULT createShaderTable(const rt::ShaderTableDesc& tableDesc, rt::IShaderTable** ppShaderTable) noexcept override { return utils::ReturnObject(ppShaderTable, [&] { return createShaderTable(tableDesc); }); }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;

    private:
        const Context& m_Context;
    };

    //////////////////////////////////////////////////////////////////////////
    // Command recording (cuda-commandlist.cpp, cuda-executor.cpp)
    //////////////////////////////////////////////////////////////////////////

    enum class CommandType : uint32_t
    {
        CopyMemory,         // cuMemcpyAsync (any memory, unified addressing)
        SetMemory32,        // cuMemsetD32Async
        Copy3D,             // cuMemcpy3DAsync
        Launch,             // [globals copy] + cuLaunchKernel
        LaunchIndirect,     // reads the group counts from device memory first
        NativeWork,
        RecordEvent,        // timer queries
        PatchTraversable,   // writes the traversable handle of an AS into upload memory
        BuildAccelStruct,
        CompactAccelStruct,
        CopyAccelStruct,
        LaunchRays,         // [globals copy] + optixLaunch
    };

    struct CommandHeader
    {
        CommandType type;
        uint32_t size;      // including the header, a multiple of 8
    };

    // Commands that a recording resolves completely; the executor replays them on a stream.
    struct CommandCopyMemory { CommandHeader header; CUdeviceptr dst; CUdeviceptr src; size_t size; };
    struct CommandSetMemory32 { CommandHeader header; CUdeviceptr dst; uint32_t value; size_t count; };
    struct CommandCopy3D { CommandHeader header; CUDA_MEMCPY3D copy; };
    struct CommandLaunch
    {
        CommandHeader header;
        CUfunction function;
        uint32_t grid[3];
        uint32_t block[3];
        CUdeviceptr globalsDst;     // SLANG_globalParams, 0 if none
        CUdeviceptr globalsSrc;     // in upload memory
        size_t globalsSize;
        CUdeviceptr indirectArgs;   // LaunchIndirect: three uint32 group counts
        uint32_t paramsSize;        // the argument block follows the command
        uint32_t reserved;
    };
    struct CommandNativeWork { CommandHeader header; INativeWork* work; };
    struct CommandRecordEvent { CommandHeader header; CUevent event; };
    struct CommandPatchTraversable { CommandHeader header; uint64_t* dst; AccelStruct* as; };
    struct CommandCopyAccelStruct { CommandHeader header; AccelStruct* dst; AccelStruct* src; };
    struct CommandCompactAccelStruct { CommandHeader header; AccelStruct* as; CUdeviceptr newBuffer; size_t newSize; };
    struct CommandLaunchRays
    {
        CommandHeader header;
        RayTracingPipeline* pipeline;
        CUdeviceptr params;         // the launch parameters, in device memory
        size_t paramsSize;
        CUdeviceptr paramsSrc;      // in upload memory, copied to 'params' first
        uint32_t width, height, depth;
        uint32_t reserved;
#if NVRHI_WITH_OPTIX
        OptixShaderBindingTable sbt;
#endif
    };

    // The resources, memory and work of one recording, from execution until the submission finishes.
    struct CommandListInstance
    {
        std::vector<uint8_t> commands;
        std::vector<RHIObjectHandle> references;
        std::vector<TransientChunk*> chunks;
        std::vector<CUdeviceptr> deferredFrees;     // device memory released when the submission finishes
        std::vector<AutoPtr<TimerQuery>> timerQueries;
        CommandQueue queue = CommandQueue::Graphics;
        uint64_t submission = 0;
    };

    NVRHI_CLASS_CLSID(CommandList, "cdf8102f-c538-4d45-9852-9224c277a801")
    class CommandList : public ObjectImpl<cuda::ICommandList>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(CommandList)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(CommandList)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::cuda::ICommandList)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ICommandList)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(CommandList)
        NVRHI_END_INTERFACE_TABLE()

        CommandList(Device* device, const Context& context, const CommandListParameters& params);
        ~CommandList();

        // Hands the recording over for execution; the list can be opened again right away.
        MonoPtr<CommandListInstance> takeInstance();

        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        // cuda::ICommandList
        void enqueueNativeWork(INativeWork* work) noexcept override;

        // ICommandList
        void open() noexcept override;
        void close() noexcept override;
        void clearState() noexcept override;

        void clearTextureFloat(ITexture* t, const TextureSubresourceSet& subresources, const Color& clearColor) noexcept override;
        void clearDepthStencilTexture(ITexture* t, const TextureSubresourceSet& subresources, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) noexcept override;
        void clearTextureUInt(ITexture* t, const TextureSubresourceSet& subresources, uint32_t clearColor) noexcept override;
        void clearSamplerFeedbackTexture(ISamplerFeedbackTexture* texture) noexcept override;
        void decodeSamplerFeedbackTexture(IBuffer* buffer, ISamplerFeedbackTexture* texture, Format format) noexcept override;
        void setSamplerFeedbackTextureState(ISamplerFeedbackTexture* texture, ResourceStates stateBits) noexcept override;

        void copyTexture1(ITexture* dest, const TextureSlice& destSlice, ITexture* src, const TextureSlice& srcSlice) noexcept override;
        void copyTexture2(IStagingTexture* dest, const TextureSlice& destSlice, ITexture* src, const TextureSlice& srcSlice) noexcept override;
        void copyTexture3(ITexture* dest, const TextureSlice& destSlice, IStagingTexture* src, const TextureSlice& srcSlice) noexcept override;
        void writeTexture(ITexture* dest, uint32_t arraySlice, uint32_t mipLevel, const void* data, size_t rowPitch, size_t depthPitch) noexcept override;
        void resolveTexture(ITexture* dest, const TextureSubresourceSet& dstSubresources, ITexture* src, const TextureSubresourceSet& srcSubresources) noexcept override;

        void writeBuffer(IBuffer* b, const void* data, size_t dataSize, uint64_t destOffsetBytes) noexcept override;
        void clearBufferUInt(IBuffer* b, uint32_t clearValue) noexcept override;
        void copyBuffer(IBuffer* dest, uint64_t destOffsetBytes, IBuffer* src, uint64_t srcOffsetBytes, uint64_t dataSizeBytes) noexcept override;

        void setPushConstants(const void* data, size_t byteSize) noexcept override;

        void setGraphicsState(const GraphicsState& state) noexcept override;
        void draw(const DrawArguments& args) noexcept override;
        void drawIndexed(const DrawArguments& args) noexcept override;
        void drawIndirect(uint32_t offsetBytes, uint32_t drawCount) noexcept override;
        void drawIndexedIndirect(uint32_t offsetBytes, uint32_t drawCount) noexcept override;
        void drawIndexedIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount) noexcept override;

        void setComputeState(const ComputeState& state) noexcept override;
        void dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) noexcept override;
        void dispatchIndirect(uint32_t offsetBytes) noexcept override;

        void setMeshletState(const MeshletState& state) noexcept override;
        void dispatchMesh(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) noexcept override;
        void dispatchMeshIndirect(uint32_t offsetBytes, uint32_t maxDrawCount) noexcept override;
        void dispatchMeshIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount) noexcept override;

        void setRayTracingState(const rt::State& state) noexcept override;
        void dispatchRays(const rt::DispatchRaysArguments& args) noexcept override;

        void buildOpacityMicromap(rt::IOpacityMicromap* omm, const rt::OpacityMicromapDesc& desc) noexcept override;
        void buildBottomLevelAccelStruct(rt::IAccelStruct* as, const rt::GeometryDesc* pGeometries, size_t numGeometries, rt::AccelStructBuildFlags buildFlags) noexcept override;
        void compactBottomLevelAccelStructs() noexcept override;
        void copyRaytracingAccelerationStructure(rt::IAccelStruct* destination, rt::IAccelStruct* source) noexcept override;
        void buildTopLevelAccelStruct(rt::IAccelStruct* as, const rt::InstanceDesc* pInstances, size_t numInstances, rt::AccelStructBuildFlags buildFlags) noexcept override;
        void buildTopLevelAccelStructFromBuffer(rt::IAccelStruct* as, nvrhi::IBuffer* instanceBuffer, uint64_t instanceBufferOffset, size_t numInstances, rt::AccelStructBuildFlags buildFlags) noexcept override;
        void executeMultiIndirectClusterOperation(const rt::cluster::OperationDesc& desc) noexcept override;

        void convertCoopVecMatrices(coopvec::ConvertMatrixLayoutDesc const* convertDescs, size_t numDescs) noexcept override;

        void beginTimerQuery(ITimerQuery* query) noexcept override;
        void endTimerQuery(ITimerQuery* query) noexcept override;

        void beginMarker(const char* name) noexcept override { (void)name; }
        void endMarker() noexcept override { }

        // Work on one stream executes in order: there are no barriers and no resource states.
        void setEnableAutomaticBarriers(bool enable) noexcept override { (void)enable; }
        void setResourceStatesForBindingSet(IBindingSet* bindingSet) noexcept override { (void)bindingSet; }
        void setEnableUavBarriersForTexture(ITexture* texture, bool enableBarriers) noexcept override { (void)texture; (void)enableBarriers; }
        void setEnableUavBarriersForBuffer(IBuffer* buffer, bool enableBarriers) noexcept override { (void)buffer; (void)enableBarriers; }
        void beginTrackingTextureState(ITexture* texture, const TextureSubresourceSet& subresources, ResourceStates stateBits) noexcept override { (void)texture; (void)subresources; (void)stateBits; }
        void beginTrackingBufferState(IBuffer* buffer, ResourceStates stateBits) noexcept override { (void)buffer; (void)stateBits; }
        void setTextureState(ITexture* texture, const TextureSubresourceSet& subresources, ResourceStates stateBits) noexcept override { (void)texture; (void)subresources; (void)stateBits; }
        void setBufferState(IBuffer* buffer, ResourceStates stateBits) noexcept override { (void)buffer; (void)stateBits; }
        void setAccelStructState(rt::IAccelStruct* as, ResourceStates stateBits) noexcept override { (void)as; (void)stateBits; }
        void setPermanentTextureState(ITexture* texture, ResourceStates stateBits) noexcept override { (void)texture; (void)stateBits; }
        void setPermanentBufferState(IBuffer* buffer, ResourceStates stateBits) noexcept override { (void)buffer; (void)stateBits; }
        void commitBarriers() noexcept override { }
        ResourceStates getTextureSubresourceState(ITexture* texture, ArraySlice arraySlice, MipLevel mipLevel) noexcept override { (void)texture; (void)arraySlice; (void)mipLevel; return ResourceStates::Common; }
        ResourceStates getBufferState(IBuffer* buffer) noexcept override { (void)buffer; return ResourceStates::Common; }

        nvrhi::IDevice* getDevice() noexcept override;
        const CommandListParameters& getDesc() noexcept override { return m_Desc; }

    private:
        Device* m_Device;   // the device keeps the queues; command lists keep the device alive
        nvrhi::DeviceHandle m_DeviceReference;
        const Context& m_Context;
        CommandListParameters m_Desc;
        TransientAllocator m_Upload;
        TransientAllocator m_Scratch;
        MonoPtr<CommandListInstance> m_Instance;
        bool m_IsOpen = false;

        // State
        ComputeState m_ComputeState;
        bool m_ComputeStateValid = false;
        rt::State m_RayTracingState;
        bool m_RayTracingStateValid = false;
        std::vector<uint8_t> m_PushConstants;
        std::unordered_map<IBuffer*, CUdeviceptr> m_VolatileBuffers;   // volatile constant buffer -> current version

        template <typename T>
        T* appendCommand(CommandType type, size_t extraBytes = 0);
        void reference(IRHIObject* object);
        bool allocateUpload(size_t size, size_t alignment, TransientAllocation& out);
        bool uploadData(const void* data, size_t size, size_t alignment, CUdeviceptr& outDevicePtr);
        bool allocateScratch(size_t size, size_t alignment, CUdeviceptr& outDevicePtr);
        // Copies 'upload' (upload memory) to new scratch device memory with a recorded copy.
        bool copyToScratch(const TransientAllocation& upload, size_t size, size_t alignment, CUdeviceptr& outDevicePtr);
        CUdeviceptr getBufferAddress(Buffer* buffer, uint64_t offset);
        void recordCopy3D(const CUDA_MEMCPY3D& copy);
        void recordClearTexture(Texture* texture, const TextureSubresourceSet& subresources, const void* texelValue);
        // Writes the bindings into a global parameter block (in upload memory); the handles of acceleration
        // structures are written at replay (PatchTraversable), when they are known.
        bool writeGlobals(const ParameterMap& parameters, const BindingSetVector& bindings, uint8_t* globals,
            const char* what);
        bool writeBinding(const ParameterMap& parameters, const ParameterSlot& slot, const BindingSetItem& item,
            const BindingSetVector& bindings, uint8_t* destination);
        void recordLaunch(bool indirect, uint32_t gx, uint32_t gy, uint32_t gz, CUdeviceptr indirectArgs);
        // Writes the shader binding table of the current ray tracing state into transient memory.
        bool buildShaderBindingTable(ShaderTable* table, CommandLaunchRays& launch);
    };

    // Appends a command record (zero-initialized, followed by extraBytes) to the recording. The pointer is valid
    // until the next command is appended.
    template <typename T>
    T* CommandList::appendCommand(CommandType type, size_t extraBytes)
    {
        static_assert(std::is_trivially_copyable_v<T>, "commands are plain records");
        const size_t size = (sizeof(T) + extraBytes + 7) & ~size_t(7);
        std::vector<uint8_t>& commands = m_Instance->commands;
        const size_t offset = commands.size();
        commands.resize(offset + size);
        T* command = new (commands.data() + offset) T();
        command->header.type = type;
        command->header.size = uint32_t(size);
        return command;
    }

    //////////////////////////////////////////////////////////////////////////
    // Queue (cuda-queue.cpp)
    //////////////////////////////////////////////////////////////////////////

    class Queue
    {
    public:
        Queue(const Context& context, Device* device, CommandQueue type);
        ~Queue();

        bool initialize();
        CUstream getStream() const { return m_Stream; }
        CommandQueue getType() const { return m_Type; }

        // Replays the instances on the stream; returns the submission number.
        uint64_t submit(MonoPtr<CommandListInstance>* instances, size_t count);
        // Polls the submissions; releases those that finished. Returns the last finished submission.
        uint64_t updateCompleted();
        uint64_t getLastSubmitted();
        // The event that signals when 'submission' has finished, or nullptr if it has (or is unknown).
        CUevent getSubmissionEvent(uint64_t submission);
        void waitForIdle();

    private:
        struct Submission
        {
            uint64_t number = 0;
            CUevent event = nullptr;
            std::vector<MonoPtr<CommandListInstance>> instances;
        };

        const Context& m_Context;
        Device* m_Device;
        CommandQueue m_Type;
        CUstream m_Stream = nullptr;
        std::mutex m_Mutex;
        uint64_t m_LastSubmitted = 0;
        uint64_t m_LastCompleted = 0;
        std::deque<Submission> m_InFlight;
        std::vector<CUevent> m_FreeEvents;

        void retire(Submission& submission);
    };

    // Replays a recording on a stream (cuda-executor.cpp).
    bool ExecuteCommands(const Context& context, CUstream stream, CommandListInstance& instance);
    // Replays the ray tracing commands (cuda-optix.cpp).
    bool ExecuteRayTracingCommand(const Context& context, CUstream stream, const CommandHeader& command,
        CommandListInstance& instance);

    //////////////////////////////////////////////////////////////////////////
    // Interop (cuda-interop.cpp)
    //////////////////////////////////////////////////////////////////////////

    // IInteropDevice for the paired graphics device. A mixin of the device: it owns no reference count and is
    // reached through the device's QueryInterface table (NVRHI_IMPLEMENTS_ROUTE_PARENT).
    NVRHI_CLASS_CLSID(InteropDevice, "41e47e98-1504-4056-8856-7a341b13356c")
    class InteropDevice : public IInteropDevice
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(InteropDevice)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(InteropDevice)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IInteropDevice)
        NVRHI_IMPLEMENTS_CLASS(InteropDevice)
        NVRHI_END_INTERFACE_TABLE()

        nvrhi::IDevice* getGraphicsDevice() noexcept override { return m_GraphicsDevice; }
        bool isInteropSupported() noexcept override { return m_Api != nullptr; }
        FRESULT importBuffer(IBuffer* graphicsBuffer, IBuffer** ppBuffer) noexcept override;
        FRESULT importTexture(ITexture* graphicsTexture, ITexture** ppTexture) noexcept override;
        FRESULT computeQueueWaitForGraphicsQueue(CommandQueue computeQueue, CommandQueue graphicsQueue) noexcept override;
        FRESULT graphicsQueueWaitForComputeQueue(CommandQueue graphicsQueue, CommandQueue computeQueue) noexcept override;

        // The API-specific part (cuda-interop-d3d12.cpp, cuda-interop-vulkan.cpp).
        class Api
        {
        public:
            virtual ~Api() = default;
            // The shared handle and allocation size of a graphics resource created with SharedResourceFlags::Shared.
            virtual bool getBufferMemory(IBuffer* buffer, CUDA_EXTERNAL_MEMORY_HANDLE_DESC& outDesc, std::string& error) = 0;
            virtual bool getTextureMemory(ITexture* texture, CUDA_EXTERNAL_MEMORY_HANDLE_DESC& outDesc, std::string& error) = 0;
            // False with a reason for textures whose memory layout CUDA cannot map (FE_UNSUPPORTED).
            virtual bool canImportTexture(const TextureDesc& desc, std::string& reason) { (void)desc; (void)reason; return true; }
            // One shared timeline fence per queue, in both directions.
            virtual CUexternalSemaphore getGraphicsTimeline(CommandQueue queue) = 0;    // signalled by graphics
            virtual CUexternalSemaphore getComputeTimeline(CommandQueue queue) = 0;     // signalled by compute
            // Signals the graphics timeline of 'queue' with 'value' after the work executed on the queue so far.
            virtual bool graphicsSignal(CommandQueue queue, uint64_t value) = 0;
            // Graphics work executed on 'queue' after this call waits for the compute timeline of 'computeQueue'.
            virtual bool graphicsWait(CommandQueue queue, CommandQueue computeQueue, uint64_t value) = 0;
        };

    protected:
        // Pairs the device with graphicsDevice (null: not paired). Returns false with a message if the graphics
        // API cannot share resources with CUDA; the device still works, without interop.
        bool initializeInterop(const Context& context, Device* device, nvrhi::IDevice* graphicsDevice, std::string& error);
        void shutdownInterop();

    private:
        const Context* m_Context = nullptr;
        Device* m_Device = nullptr;
        nvrhi::DeviceHandle m_GraphicsDevice;
        MonoPtr<Api> m_Api;
        std::mutex m_Mutex;
        std::array<uint64_t, c_NumQueues> m_GraphicsValues{};  // last value signalled on each graphics timeline
        std::array<uint64_t, c_NumQueues> m_ComputeValues{};   // ... on each compute timeline
    };

    MonoPtr<InteropDevice::Api> CreateD3D12InteropApi(const Context& context, nvrhi::IDevice* graphicsDevice, std::string& error);
    MonoPtr<InteropDevice::Api> CreateVulkanInteropApi(const Context& context, nvrhi::IDevice* graphicsDevice, std::string& error);

    // The adapter of a graphics device, to select the CUDA device on the same GPU.
    struct AdapterIdentity
    {
        bool hasLuid = false;
        uint8_t luid[8] = {};
        bool hasUuid = false;
        uint8_t uuid[16] = {};
    };

    // D3D11 and D3D12 devices (cuda-interop-d3d12.cpp); false if the device is not one or the query fails.
    bool GetD3DAdapterIdentity(nvrhi::IDevice* graphicsDevice, AdapterIdentity& outIdentity);
    // Vulkan devices (cuda-interop-vulkan.cpp).
    bool GetVulkanAdapterIdentity(nvrhi::IDevice* graphicsDevice, AdapterIdentity& outIdentity);

    // OptiX (cuda-optix.cpp): creates the device context for the CUDA context; false with a message if OptiX is
    // not compiled in or not available in the driver.
    bool InitializeOptix(Context& context, bool enableValidation, std::string& error);
    void ShutdownOptix(Context& context);

    // Fills the array descriptor of a texture (cuda-texture.cpp); false with a message if CUDA cannot hold it.
    bool GetArrayDescriptor(const TextureDesc& desc, bool forInterop, CUDA_ARRAY3D_DESCRIPTOR& outDescriptor, std::string& error);

    //////////////////////////////////////////////////////////////////////////
    // Device (cuda-device.cpp)
    //////////////////////////////////////////////////////////////////////////

    NVRHI_CLASS_CLSID(Device, "d4b04138-4ecb-4dea-87bc-5bd54e7824a3")
    class Device : public ObjectImpl<cuda::IDevice, InteropDevice>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Device)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Device)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::cuda::IDevice)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IDevice)
        // IRHIObject is a base through IDevice and through IInteropDevice: answered through IDevice.
        NVRHI_IMPLEMENTS_INTERFACE_AS(nvrhi::IRHIObject, nvrhi::IDevice)
        NVRHI_IMPLEMENTS_CLASS(Device)
        NVRHI_IMPLEMENTS_ROUTE_PARENT(InteropDevice)
        NVRHI_END_INTERFACE_TABLE()

        explicit Device(const DeviceDesc& desc);
        ~Device();

        // Creates the context, queues and OptiX; false with a message on failure.
        bool initialize(const DeviceDesc& desc, std::string& error);

        const Context& getContext() const { return m_Context; }
        Queue* getQueue(CommandQueue queue) { return m_Queues[uint32_t(queue)].Get(); }
        TransientPool& getTransientPool() { return m_TransientPool; }

        // Acceleration structures built with AllowCompaction, until compactBottomLevelAccelStructs compacts them.
        void addPendingCompaction(AccelStruct* as);
        // Removes and returns those whose build has finished.
        std::vector<AutoPtr<AccelStruct>> takeCompactableAccelStructs();

        // IRHIObject
        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        // IInteropDevice (the rest is in InteropDevice)
        nvrhi::IDevice* getComputeDevice() noexcept override { return this; }

        // cuda::IDevice
        int32_t getDeviceOrdinal() noexcept override { return m_Context.ordinal; }
        uint32_t getComputeCapability() noexcept override { return m_Context.computeCapability; }
        bool isOptixAvailable() noexcept override;
        uint64_t queueGetCompletedInstance(CommandQueue queue) noexcept override;
        uint64_t getTextureHandle(nvrhi::ITexture* texture, nvrhi::ISampler* sampler,
            const TextureSubresourceSet& subresources, Format format) noexcept override;
        uint64_t getSurfaceHandle(nvrhi::ITexture* texture, MipLevel mipLevel) noexcept override;

        // IDevice
        FRESULT createHeap(const HeapDesc& d, IHeap** ppHeap) noexcept override { (void)d; return unsupported(ppHeap, "createHeap"); }
        bool queryTopLevelAccelStructPrebuildInfo(const rt::AccelStructDesc& desc, uint32_t instanceCount, rt::AccelStructPrebuildInfo& outInfo) noexcept override;

        TextureHandle createTexture(const TextureDesc& d);
        FRESULT createTexture(const TextureDesc& d, ITexture** ppTexture) noexcept override { return utils::ReturnObject(ppTexture, [&] { return createTexture(d); }); }
        MemoryRequirements& getTextureMemoryRequirements(MemoryRequirements& retVal, ITexture* texture) noexcept override;
        bool bindTextureMemory(ITexture* texture, IHeap* heap, uint64_t offset) noexcept override { (void)texture; (void)heap; (void)offset; return false; }

        TextureHandle createHandleForNativeTexture(ObjectType objectType, NativeObject texture, const TextureDesc& desc);
        FRESULT createHandleForNativeTexture(ObjectType objectType, NativeObject texture, const TextureDesc& desc, ITexture** ppTexture) noexcept override { return utils::ReturnObject(ppTexture, [&] { return createHandleForNativeTexture(objectType, texture, desc); }); }

        StagingTextureHandle createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess);
        FRESULT createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess, IStagingTexture** ppStagingTexture) noexcept override { return utils::ReturnObject(ppStagingTexture, [&] { return createStagingTexture(d, cpuAccess); }); }
        void* mapStagingTexture(IStagingTexture* tex, const TextureSlice& slice, CpuAccessMode cpuAccess, size_t& outRowPitch) noexcept override;
        void unmapStagingTexture(IStagingTexture* tex) noexcept override { (void)tex; }

        void getTextureTiling(ITexture* texture, uint32_t* numTiles, PackedMipDesc* desc, TileShape* tileShape, uint32_t* subresourceTilingsNum, SubresourceTiling* subresourceTilings) noexcept override;
        void updateTextureTileMappings(ITexture* texture, const TextureTilesMapping* tileMappings, uint32_t numTileMappings, CommandQueue executionQueue) noexcept override;

        FRESULT createSamplerFeedbackTexture(ITexture* pairedTexture, const SamplerFeedbackTextureDesc& desc, ISamplerFeedbackTexture** ppTexture) noexcept override { (void)pairedTexture; (void)desc; return unsupported(ppTexture, "createSamplerFeedbackTexture"); }
        FRESULT createSamplerFeedbackForNativeTexture(ObjectType objectType, NativeObject texture, ITexture* pairedTexture, ISamplerFeedbackTexture** ppTexture) noexcept override { (void)objectType; (void)texture; (void)pairedTexture; return unsupported(ppTexture, "createSamplerFeedbackForNativeTexture"); }

        BufferHandle createBuffer(const BufferDesc& d);
        FRESULT createBuffer(const BufferDesc& d, IBuffer** ppBuffer) noexcept override { return utils::ReturnObject(ppBuffer, [&] { return createBuffer(d); }); }
        void* mapBuffer(IBuffer* buffer, CpuAccessMode cpuAccess) noexcept override;
        void unmapBuffer(IBuffer* buffer) noexcept override { (void)buffer; }
        MemoryRequirements& getBufferMemoryRequirements(MemoryRequirements& retVal, IBuffer* buffer) noexcept override;
        bool bindBufferMemory(IBuffer* buffer, IHeap* heap, uint64_t offset) noexcept override { (void)buffer; (void)heap; (void)offset; return false; }

        BufferHandle createHandleForNativeBuffer(ObjectType objectType, NativeObject buffer, const BufferDesc& desc);
        FRESULT createHandleForNativeBuffer(ObjectType objectType, NativeObject buffer, const BufferDesc& desc, IBuffer** ppBuffer) noexcept override { return utils::ReturnObject(ppBuffer, [&] { return createHandleForNativeBuffer(objectType, buffer, desc); }); }

        ShaderHandle createShader(const ShaderDesc& d, const void* binary, size_t binarySize);
        FRESULT createShader(const ShaderDesc& d, const void* binary, size_t binarySize, IShader** ppShader) noexcept override { return utils::ReturnObject(ppShader, [&] { return createShader(d, binary, binarySize); }); }
        FRESULT createShaderSpecialization(IShader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants, IShader** ppShader) noexcept override { (void)baseShader; (void)constants; (void)numConstants; return unsupported(ppShader, "createShaderSpecialization"); }
        ShaderLibraryHandle createShaderLibrary(const void* binary, size_t binarySize);
        FRESULT createShaderLibrary(const void* binary, size_t binarySize, IShaderLibrary** ppShaderLibrary) noexcept override { return utils::ReturnObject(ppShaderLibrary, [&] { return createShaderLibrary(binary, binarySize); }); }

        SamplerHandle createSampler(const SamplerDesc& d);
        FRESULT createSampler(const SamplerDesc& d, ISampler** ppSampler) noexcept override { return utils::ReturnObject(ppSampler, [&] { return createSampler(d); }); }

        FRESULT createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, IShader* vertexShader, IInputLayout** ppInputLayout) noexcept override { (void)d; (void)attributeCount; (void)vertexShader; return unsupported(ppInputLayout, "createInputLayout"); }

        EventQueryHandle createEventQuery();
        FRESULT createEventQuery(IEventQuery** ppQuery) noexcept override { return utils::ReturnObject(ppQuery, [&] { return createEventQuery(); }); }
        void setEventQuery(IEventQuery* query, CommandQueue queue) noexcept override;
        bool pollEventQuery(IEventQuery* query) noexcept override;
        void waitEventQuery(IEventQuery* query) noexcept override;
        void resetEventQuery(IEventQuery* query) noexcept override;

        TimerQueryHandle createTimerQuery();
        FRESULT createTimerQuery(ITimerQuery** ppQuery) noexcept override { return utils::ReturnObject(ppQuery, [&] { return createTimerQuery(); }); }
        bool pollTimerQuery(ITimerQuery* query) noexcept override;
        float getTimerQueryTime(ITimerQuery* query) noexcept override;
        void resetTimerQuery(ITimerQuery* query) noexcept override;

        GraphicsAPI getGraphicsAPI() noexcept override { return GraphicsAPI::CUDA; }

        FRESULT createFramebuffer(const FramebufferDesc& desc, IFramebuffer** ppFramebuffer) noexcept override { (void)desc; return unsupported(ppFramebuffer, "createFramebuffer"); }
        FRESULT createGraphicsPipeline1(const GraphicsPipelineDesc& desc, const FramebufferInfo& fbinfo, IGraphicsPipeline** ppPipeline) noexcept override { (void)desc; (void)fbinfo; return unsupported(ppPipeline, "createGraphicsPipeline"); }
        FRESULT createGraphicsPipeline2(const GraphicsPipelineDesc& desc, IFramebuffer* fb, IGraphicsPipeline** ppPipeline) noexcept override { (void)desc; (void)fb; return unsupported(ppPipeline, "createGraphicsPipeline"); }

        ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc);
        FRESULT createComputePipeline(const ComputePipelineDesc& desc, IComputePipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createComputePipeline(desc); }); }

        FRESULT createMeshletPipeline1(const MeshletPipelineDesc& desc, const FramebufferInfo& fbinfo, IMeshletPipeline** ppPipeline) noexcept override { (void)desc; (void)fbinfo; return unsupported(ppPipeline, "createMeshletPipeline"); }
        FRESULT createMeshletPipeline2(const MeshletPipelineDesc& desc, IFramebuffer* fb, IMeshletPipeline** ppPipeline) noexcept override { (void)desc; (void)fb; return unsupported(ppPipeline, "createMeshletPipeline"); }

        rt::PipelineHandle createRayTracingPipeline(const rt::PipelineDesc& desc);
        FRESULT createRayTracingPipeline(const rt::PipelineDesc& desc, rt::IPipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createRayTracingPipeline(desc); }, FE_UNSUPPORTED); }

        BindingLayoutHandle createBindingLayout(const BindingLayoutDesc& desc);
        FRESULT createBindingLayout(const BindingLayoutDesc& desc, IBindingLayout** ppLayout) noexcept override { return utils::ReturnObject(ppLayout, [&] { return createBindingLayout(desc); }); }
        FRESULT createBindlessLayout(const BindlessLayoutDesc& desc, IBindingLayout** ppLayout) noexcept override { (void)desc; return unsupported(ppLayout, "createBindlessLayout"); }

        BindingSetHandle createBindingSet(const BindingSetDesc& desc, IBindingLayout* layout);
        FRESULT createBindingSet(const BindingSetDesc& desc, IBindingLayout* layout, IBindingSet** ppBindingSet) noexcept override { return utils::ReturnObject(ppBindingSet, [&] { return createBindingSet(desc, layout); }); }
        FRESULT createDescriptorTable(IBindingLayout* layout, IDescriptorTable** ppDescriptorTable) noexcept override { (void)layout; return unsupported(ppDescriptorTable, "createDescriptorTable"); }
        void resizeDescriptorTable(IDescriptorTable* descriptorTable, uint32_t newSize, bool keepContents) noexcept override { (void)descriptorTable; (void)newSize; (void)keepContents; utils::NotSupported(); }
        bool writeDescriptorTable(IDescriptorTable* descriptorTable, const BindingSetItem& item) noexcept override { (void)descriptorTable; (void)item; utils::NotSupported(); return false; }

        FRESULT createOpacityMicromap(const rt::OpacityMicromapDesc& desc, rt::IOpacityMicromap** ppOpacityMicromap) noexcept override { (void)desc; return unsupported(ppOpacityMicromap, "createOpacityMicromap"); }
        rt::AccelStructHandle createAccelStruct(const rt::AccelStructDesc& desc);
        FRESULT createAccelStruct(const rt::AccelStructDesc& desc, rt::IAccelStruct** ppAccelStruct) noexcept override { return utils::ReturnObject(ppAccelStruct, [&] { return createAccelStruct(desc); }, FE_UNSUPPORTED); }
        MemoryRequirements& getAccelStructMemoryRequirements(MemoryRequirements& retVal, rt::IAccelStruct* as) noexcept override;
        rt::cluster::OperationSizeInfo& getClusterOperationSizeInfo(rt::cluster::OperationSizeInfo& retVal, const rt::cluster::OperationParams& params) noexcept override;
        bool bindAccelStructMemory(rt::IAccelStruct* as, IHeap* heap, uint64_t offset) noexcept override { (void)as; (void)heap; (void)offset; return false; }

        CommandListHandle createCommandList(const CommandListParameters& params);
        FRESULT createCommandList(const CommandListParameters& params, nvrhi::ICommandList** ppCommandList) noexcept override { return utils::ReturnObject(ppCommandList, [&] { return createCommandList(params); }); }
        uint64_t executeCommandLists(nvrhi::ICommandList* const* pCommandLists, size_t numCommandLists, CommandQueue executionQueue) noexcept override;
        void queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue, uint64_t instance) noexcept override;
        bool waitForIdle() noexcept override;
        FRESULT createCommandListLifetimeTracker(CommandQueue executionQueue, ICommandListLifetimeTracker** ppTracker) noexcept override { (void)executionQueue; return unsupported(ppTracker, "createCommandListLifetimeTracker"); }
        void runGarbageCollection() noexcept override;

        bool queryFeatureSupport(Feature feature, void* pInfo, size_t infoSize) noexcept override;
        FormatSupport queryFormatSupport(Format format) noexcept override;
        coopvec::DeviceFeatures& queryCoopVecFeatures(coopvec::DeviceFeatures& retVal) noexcept override { return retVal; }
        coopvec::MatMulFormatSupport& queryCoopVecMatMulFormatSupport(coopvec::MatMulFormatSupport& retVal, const coopvec::MatMulFormatCombo& combination) noexcept override { (void)combination; return retVal; }
        coopvec::TrainingFormatSupport& queryCoopVecTrainingFormatSupport(coopvec::TrainingFormatSupport& retVal, coopvec::DataType componentType) noexcept override { (void)componentType; return retVal; }
        size_t getCoopVecMatrixSize(coopvec::DataType type, coopvec::MatrixLayout layout, int rows, int columns) noexcept override { (void)type; (void)layout; (void)rows; (void)columns; return 0; }

        NativeObject getNativeQueue(ObjectType objectType, CommandQueue queue) noexcept override;
        IMessageCallback* getMessageCallback() noexcept override { return m_Context.messageCallback; }
        bool isAftermathEnabled() noexcept override { return false; }
        IAftermathCrashDumpHelper* getAftermathCrashDumpHelper() noexcept override { return nullptr; }

    private:
        Context m_Context;
        TransientPool m_TransientPool;
        std::array<MonoPtr<Queue>, c_NumQueues> m_Queues;
        bool m_PrimaryContextRetained = false;
        std::mutex m_CompactionMutex;
        std::vector<AutoPtr<AccelStruct>> m_PendingCompactions;

        template <typename TInterface>
        FRESULT unsupported(TInterface** ppObject, const char* what) noexcept
        {
            if (!ppObject)
                return FE_INVALID_ARGS;
            *ppObject = nullptr;
            try
            {
                m_Context.error(std::string(what) + " is not supported on the CUDA backend");
            }
            catch (...)
            {
            }
            return FE_UNSUPPORTED;
        }
    };
}
