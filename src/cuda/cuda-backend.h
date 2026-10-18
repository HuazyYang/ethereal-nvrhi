// SPDX-License-Identifier: MIT
// CUDA backend implementation. Unsupported raster and optional feature operations fail explicitly.
#pragma once
#include <ShaderToolArtifact.h>
#include <algorithm>
#include <cstring>
#include <cuda.h>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <nvrhi/core/foundation.h>
#include <nvrhi/cuda.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#if NVRHI_WITH_OPTIX
#include <optix.h>
#endif

namespace nvrhi::cuda
{
struct Scope
{
    CUcontext previous = nullptr;
    bool active = false;
    explicit Scope(CUcontext c) : active(cuCtxPushCurrent(c) == CUDA_SUCCESS)
    {
    }
    ~Scope()
    {
        if (active)
            cuCtxPopCurrent(&previous);
    }
};
struct Context
{
    CUdevice device = 0;
    CUcontext handle = nullptr;
    CUstream streams[3]{};
    AutoPtr<IMessageCallback> callback;
#if NVRHI_WITH_OPTIX
    OptixDeviceContext optix = nullptr;
#endif
    ~Context();
    bool check(CUresult result, const char *operation) const noexcept;
    void error(const char *message) const noexcept
    {
        if (callback)
            callback->message(MessageSeverity::Error, message);
    }
};
using ContextPtr = std::shared_ptr<Context>;
#if NVRHI_WITH_OPTIX
OptixResult initializeOptix();
#endif
bool arrayFormat(Format, CUarray_format &, unsigned &);
#define CUDA_INTERFACES(C, I)                                                                                          \
    NVRHI_DECLARE_UUID_TRAITS(C)                                                                                       \
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(C)                                                                              \
    NVRHI_IMPLEMENTS_INTERFACE(I)                                                                                      \
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject) NVRHI_IMPLEMENTS_CLASS(C) NVRHI_END_INTERFACE_TABLE()

struct Allocation
{
    ContextPtr context;
    CUdeviceptr ptr = 0;
    bool owned = true;
    std::shared_ptr<void> externalOwner;
    ~Allocation()
    {
        if (ptr && owned)
        {
            Scope scope(context->handle);
            cuMemFree(ptr);
        }
    }
};
using AllocationPtr = std::shared_ptr<Allocation>;
AllocationPtr allocate(ContextPtr context, size_t size);

NVRHI_CLASS_CLSID(Buffer, "5490c001-09e1-40ad-a1ab-701827580001")
class Buffer : public ObjectImpl<IBuffer>
{
  public:
    CUDA_INTERFACES(Buffer, IBuffer)
    ContextPtr context;
    BufferDesc desc;
    AllocationPtr memory;
    std::vector<uint8_t> mapped;
    CpuAccessMode mapMode = CpuAccessMode::None;
    Buffer(ContextPtr c, const BufferDesc &d) : context(c), desc(d)
    {
    }
    const BufferDesc &getDesc() const noexcept override
    {
        return desc;
    }
    GpuVirtualAddress getGpuVirtualAddress() const noexcept override
    {
        return memory ? memory->ptr : 0;
    }
    NativeObject getNativeObject(ObjectType type) noexcept override;
    bool queryMemoryRequirements(MemoryRequirements &out) noexcept override;
};
NVRHI_CLASS_CLSID(Texture, "5490c002-09e1-40ad-a1ab-701827580002")
class Texture : public ObjectImpl<ITexture>
{
  public:
    CUDA_INTERFACES(Texture, ITexture)
    ContextPtr context;
    TextureDesc desc;
    CUmipmappedArray mipArray = nullptr;
    CUarray externalArray = nullptr;
    bool owned = true;
    std::shared_ptr<void> externalOwner;
    Texture(ContextPtr c, const TextureDesc &d) : context(c), desc(d)
    {
    }
    ~Texture();
    CUarray level(uint32_t mip) const;
    const TextureDesc &getDesc() const noexcept override
    {
        return desc;
    }
    NativeObject getNativeObject(ObjectType type) noexcept override;
    NativeObject getNativeView(ObjectType, Format, const TextureSubresourceSet &, TextureDimension, bool,
                               const ComponentMapping *) noexcept override
    {
        return nullptr;
    }
};
NVRHI_CLASS_CLSID(StagingTexture, "5490c003-09e1-40ad-a1ab-701827580003")
class StagingTexture : public ObjectImpl<IStagingTexture>
{
  public:
    CUDA_INTERFACES(StagingTexture, IStagingTexture)
    TextureDesc desc;
    std::vector<uint8_t> data;
    CpuAccessMode access;
    StagingTexture(const TextureDesc &d, CpuAccessMode a) : desc(d), access(a)
    {
    }
    const TextureDesc &getDesc() const noexcept override
    {
        return desc;
    }
};
NVRHI_CLASS_CLSID(Shader, "5490c004-09e1-40ad-a1ab-701827580004")
class Shader : public ObjectImpl<IShader>
{
  public:
    CUDA_INTERFACES(Shader, IShader)
    ContextPtr context;
    ShaderDesc desc;
    std::vector<uint8_t> bytes;
    CUmodule module = nullptr;
    CUfunction function = nullptr;
    CUevent parameterFence = nullptr;
    bool parameterFenceRecorded = false;
    ShaderToolArtifactView artifact{};
    ShaderToolArtifactEntry entry{};
    Shader(ContextPtr c, const ShaderDesc &d) : context(c), desc(d)
    {
    }
    ~Shader();
    const ShaderDesc &getDesc() const noexcept override
    {
        return desc;
    }
    void getBytecode(const void **p, size_t *n) const noexcept override
    {
        if (p)
            *p = bytes.data();
        if (n)
            *n = bytes.size();
    }
};
NVRHI_CLASS_CLSID(Sampler, "5490c005-09e1-40ad-a1ab-701827580005")
class Sampler : public ObjectImpl<ISampler>
{
  public:
    CUDA_INTERFACES(Sampler, ISampler)
    SamplerDesc desc;
    explicit Sampler(const SamplerDesc &d) : desc(d)
    {
    }
    const SamplerDesc &getDesc() const noexcept override
    {
        return desc;
    }
};
NVRHI_CLASS_CLSID(BindingLayout, "5490c006-09e1-40ad-a1ab-701827580006")
class BindingLayout : public ObjectImpl<IBindingLayout>
{
  public:
    CUDA_INTERFACES(BindingLayout, IBindingLayout)
    BindingLayoutDesc desc;
    explicit BindingLayout(const BindingLayoutDesc &d) : desc(d)
    {
    }
    const BindingLayoutDesc *getDesc() const noexcept override
    {
        return &desc;
    }
    const BindlessLayoutDesc *getBindlessDesc() const noexcept override
    {
        return nullptr;
    }
};
NVRHI_CLASS_CLSID(BindingSet, "5490c007-09e1-40ad-a1ab-701827580007")
class BindingSet : public ObjectImpl<IBindingSet>
{
  public:
    CUDA_INTERFACES(BindingSet, IBindingSet)
    BindingSetDesc desc;
    BindingLayoutHandle layout;
    std::vector<RHIObjectHandle> resources;
    BindingSet(const BindingSetDesc &d, IBindingLayout *l) : desc(d), layout(l)
    {
        for (auto &b : d.bindings)
            if (b.resourceHandle)
                resources.emplace_back(b.resourceHandle);
    }
    const BindingSetDesc *getDesc() const noexcept override
    {
        return &desc;
    }
    IBindingLayout *getLayout() const noexcept override
    {
        return layout;
    }
};
NVRHI_CLASS_CLSID(ComputePipeline, "5490c008-09e1-40ad-a1ab-701827580008")
class ComputePipeline : public ObjectImpl<IComputePipeline>
{
  public:
    CUDA_INTERFACES(ComputePipeline, IComputePipeline)
    ComputePipelineDesc desc;
    explicit ComputePipeline(const ComputePipelineDesc &d) : desc(d)
    {
    }
    const ComputePipelineDesc &getDesc() const noexcept override
    {
        return desc;
    }
};
NVRHI_CLASS_CLSID(EventQuery, "5490c009-09e1-40ad-a1ab-701827580009")
class EventQuery : public ObjectImpl<IEventQuery>
{
  public:
    CUDA_INTERFACES(EventQuery, IEventQuery)
    ContextPtr context;
    CUevent event = nullptr;
    bool recorded = false;
    explicit EventQuery(ContextPtr c) : context(c)
    {
    }
    ~EventQuery()
    {
        Scope scope(context->handle);
        if (event)
            cuEventDestroy(event);
    }
};
NVRHI_CLASS_CLSID(TimerQuery, "5490c010-09e1-40ad-a1ab-701827580010")
class TimerQuery : public ObjectImpl<ITimerQuery>
{
  public:
    CUDA_INTERFACES(TimerQuery, ITimerQuery)
    ContextPtr context;
    CUevent start = nullptr, end = nullptr;
    bool recorded = false;
    explicit TimerQuery(ContextPtr c) : context(c)
    {
    }
    ~TimerQuery()
    {
        Scope scope(context->handle);
        if (start)
            cuEventDestroy(start);
        if (end)
            cuEventDestroy(end);
    }
};
struct Replay
{
    ContextPtr context;
    CUstream stream;
    std::vector<AllocationPtr> temporaries;
    std::deque<std::vector<uint8_t>> hostBlocks;
    std::vector<CUtexObject> textures;
    std::vector<CUsurfObject> surfaces;
    ~Replay()
    {
        Scope scope(context->handle);
        for (auto v : textures)
            cuTexObjectDestroy(v);
        for (auto v : surfaces)
            cuSurfObjectDestroy(v);
    }
};
using Command = std::function<bool(Replay &)>;
struct Submission
{
    uint64_t id;
    CommandQueue queue;
    CUevent completion = nullptr;
    std::vector<Command> commands;
    std::shared_ptr<Replay> replay;
};
class Device;

NVRHI_CLASS_CLSID(Device, "5490c011-09e1-40ad-a1ab-701827580011")
class Device : public ObjectImpl<IDevice>
{
  public:
    CUDA_INTERFACES(Device, IDevice)
    ContextPtr context;
    void unsupported() const noexcept
    {
        context->error("CUDA backend: unsupported operation");
    }
    explicit Device(ContextPtr c) : context(c)
    {
    }
    ~Device();
    std::mutex mutex;
    uint64_t nextSubmission = 1;
    std::deque<Submission> submissions;
    NativeObject getNativeObject(ObjectType type) noexcept override;
    FRESULT createHeap(const HeapDesc &d, IHeap **ppHeap) noexcept override
    {
        unsupported();
        if (!ppHeap)
            return FE_INVALID_ARGS;
        *ppHeap = nullptr;
        return FE_UNSUPPORTED;
    }
    bool queryTopLevelAccelStructPrebuildInfo(const rt::AccelStructDesc &desc, uint32_t instanceCount,
                                              rt::AccelStructPrebuildInfo &outInfo) noexcept override
    {
        unsupported();
        return false;
    }
    FRESULT createTexture(const TextureDesc &d, ITexture **ppTexture) noexcept override;
    MemoryRequirements &getTextureMemoryRequirements(MemoryRequirements &retVal, ITexture *texture) noexcept override;
    bool bindTextureMemory(ITexture *texture, IHeap *heap, uint64_t offset) noexcept override
    {
        unsupported();
        return false;
    }
    FRESULT createHandleForNativeTexture(ObjectType objectType, NativeObject texture, const TextureDesc &desc,
                                         ITexture **ppTexture) noexcept override;
    FRESULT createStagingTexture(const TextureDesc &d, CpuAccessMode cpuAccess,
                                 IStagingTexture **ppStagingTexture) noexcept override;
    void *mapStagingTexture(IStagingTexture *tex, const TextureSlice &slice, CpuAccessMode cpuAccess,
                            size_t &outRowPitch) noexcept override;
    void unmapStagingTexture(IStagingTexture *tex) noexcept override;
    void getTextureTiling(ITexture *texture, uint32_t *numTiles, PackedMipDesc *desc, TileShape *tileShape,
                          uint32_t *subresourceTilingsNum, SubresourceTiling *subresourceTilings) noexcept override
    {
        unsupported();
    }
    void updateTextureTileMappings(ITexture *texture, const TextureTilesMapping *tileMappings, uint32_t numTileMappings,
                                   CommandQueue executionQueue = CommandQueue::Graphics) noexcept override
    {
        unsupported();
    }
    FRESULT createSamplerFeedbackTexture(ITexture *pairedTexture, const SamplerFeedbackTextureDesc &desc,
                                         ISamplerFeedbackTexture **ppTexture) noexcept override
    {
        unsupported();
        if (!ppTexture)
            return FE_INVALID_ARGS;
        *ppTexture = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createSamplerFeedbackForNativeTexture(ObjectType objectType, NativeObject texture, ITexture *pairedTexture,
                                                  ISamplerFeedbackTexture **ppTexture) noexcept override
    {
        unsupported();
        if (!ppTexture)
            return FE_INVALID_ARGS;
        *ppTexture = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createBuffer(const BufferDesc &d, IBuffer **ppBuffer) noexcept override;
    void *mapBuffer(IBuffer *buffer, CpuAccessMode cpuAccess) noexcept override;
    void unmapBuffer(IBuffer *buffer) noexcept override;
    MemoryRequirements &getBufferMemoryRequirements(MemoryRequirements &retVal, IBuffer *buffer) noexcept override;
    bool bindBufferMemory(IBuffer *buffer, IHeap *heap, uint64_t offset) noexcept override
    {
        unsupported();
        return false;
    }
    FRESULT createHandleForNativeBuffer(ObjectType objectType, NativeObject buffer, const BufferDesc &desc,
                                        IBuffer **ppBuffer) noexcept override;
    FRESULT createShader(const ShaderDesc &d, const void *binary, size_t binarySize,
                         IShader **ppShader) noexcept override;
    FRESULT createShaderSpecialization(IShader *baseShader, const ShaderSpecialization *constants,
                                       uint32_t numConstants, IShader **ppShader) noexcept override
    {
        unsupported();
        if (!ppShader)
            return FE_INVALID_ARGS;
        *ppShader = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createShaderLibrary(const void *binary, size_t binarySize,
                                IShaderLibrary **ppShaderLibrary) noexcept override
    {
        unsupported();
        if (!ppShaderLibrary)
            return FE_INVALID_ARGS;
        *ppShaderLibrary = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createSampler(const SamplerDesc &d, ISampler **ppSampler) noexcept override;
    FRESULT createInputLayout(const VertexAttributeDesc *d, uint32_t attributeCount, IShader *vertexShader,
                              IInputLayout **ppInputLayout) noexcept override
    {
        unsupported();
        if (!ppInputLayout)
            return FE_INVALID_ARGS;
        *ppInputLayout = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createEventQuery(IEventQuery **ppQuery) noexcept override;
    void setEventQuery(IEventQuery *query, CommandQueue queue) noexcept override;
    bool pollEventQuery(IEventQuery *query) noexcept override;
    void waitEventQuery(IEventQuery *query) noexcept override;
    void resetEventQuery(IEventQuery *query) noexcept override;
    FRESULT createTimerQuery(ITimerQuery **ppQuery) noexcept override;
    bool pollTimerQuery(ITimerQuery *query) noexcept override;
    float getTimerQueryTime(ITimerQuery *query) noexcept override;
    void resetTimerQuery(ITimerQuery *query) noexcept override;
    GraphicsAPI getGraphicsAPI() noexcept override;
    FRESULT createFramebuffer(const FramebufferDesc &desc, IFramebuffer **ppFramebuffer) noexcept override
    {
        unsupported();
        if (!ppFramebuffer)
            return FE_INVALID_ARGS;
        *ppFramebuffer = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createGraphicsPipeline1(const GraphicsPipelineDesc &desc, const FramebufferInfo &fbinfo,
                                    IGraphicsPipeline **ppPipeline) noexcept override
    {
        unsupported();
        if (!ppPipeline)
            return FE_INVALID_ARGS;
        *ppPipeline = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createGraphicsPipeline2(const GraphicsPipelineDesc &desc, IFramebuffer *fb,
                                    IGraphicsPipeline **ppPipeline) noexcept override
    {
        unsupported();
        if (!ppPipeline)
            return FE_INVALID_ARGS;
        *ppPipeline = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createComputePipeline(const ComputePipelineDesc &desc, IComputePipeline **ppPipeline) noexcept override;
    FRESULT createMeshletPipeline1(const MeshletPipelineDesc &desc, const FramebufferInfo &fbinfo,
                                   IMeshletPipeline **ppPipeline) noexcept override
    {
        unsupported();
        if (!ppPipeline)
            return FE_INVALID_ARGS;
        *ppPipeline = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createMeshletPipeline2(const MeshletPipelineDesc &desc, IFramebuffer *fb,
                                   IMeshletPipeline **ppPipeline) noexcept override
    {
        unsupported();
        if (!ppPipeline)
            return FE_INVALID_ARGS;
        *ppPipeline = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createRayTracingPipeline(const rt::PipelineDesc &desc, rt::IPipeline **ppPipeline) noexcept override;
    FRESULT createBindingLayout(const BindingLayoutDesc &desc, IBindingLayout **ppLayout) noexcept override;
    FRESULT createBindlessLayout(const BindlessLayoutDesc &desc, IBindingLayout **ppLayout) noexcept override
    {
        unsupported();
        if (!ppLayout)
            return FE_INVALID_ARGS;
        *ppLayout = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createBindingSet(const BindingSetDesc &desc, IBindingLayout *layout,
                             IBindingSet **ppBindingSet) noexcept override;
    FRESULT createDescriptorTable(IBindingLayout *layout, IDescriptorTable **ppDescriptorTable) noexcept override
    {
        unsupported();
        if (!ppDescriptorTable)
            return FE_INVALID_ARGS;
        *ppDescriptorTable = nullptr;
        return FE_UNSUPPORTED;
    }
    void resizeDescriptorTable(IDescriptorTable *descriptorTable, uint32_t newSize,
                               bool keepContents = true) noexcept override
    {
        unsupported();
    }
    bool writeDescriptorTable(IDescriptorTable *descriptorTable, const BindingSetItem &item) noexcept override
    {
        unsupported();
        return false;
    }
    FRESULT createOpacityMicromap(const rt::OpacityMicromapDesc &desc,
                                  rt::IOpacityMicromap **ppOpacityMicromap) noexcept override
    {
        unsupported();
        if (!ppOpacityMicromap)
            return FE_INVALID_ARGS;
        *ppOpacityMicromap = nullptr;
        return FE_UNSUPPORTED;
    }
    FRESULT createAccelStruct(const rt::AccelStructDesc &desc, rt::IAccelStruct **ppAccelStruct) noexcept override;
    MemoryRequirements &getAccelStructMemoryRequirements(MemoryRequirements &retVal,
                                                         rt::IAccelStruct *as) noexcept override;
    rt::cluster::OperationSizeInfo &getClusterOperationSizeInfo(
        rt::cluster::OperationSizeInfo &retVal, const rt::cluster::OperationParams &params) noexcept override
    {
        unsupported();
        return retVal;
    }
    bool bindAccelStructMemory(rt::IAccelStruct *as, IHeap *heap, uint64_t offset) noexcept override
    {
        unsupported();
        return false;
    }
    FRESULT createCommandList(const CommandListParameters &params, ICommandList **ppCommandList) noexcept override;
    uint64_t executeCommandLists(ICommandList *const *pCommandLists, size_t numCommandLists,
                                 CommandQueue executionQueue = CommandQueue::Graphics) noexcept override;
    void queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue,
                                 uint64_t instance) noexcept override;
    bool waitForIdle() noexcept override;
    FRESULT createCommandListLifetimeTracker(CommandQueue executionQueue,
                                             ICommandListLifetimeTracker **ppTracker) noexcept override;
    void runGarbageCollection() noexcept override;
    bool queryFeatureSupport(Feature feature, void *pInfo = nullptr, size_t infoSize = 0) noexcept override;
    FormatSupport queryFormatSupport(Format format) noexcept override;
    coopvec::DeviceFeatures &queryCoopVecFeatures(coopvec::DeviceFeatures &retVal) noexcept override
    {
        unsupported();
        return retVal;
    }
    coopvec::MatMulFormatSupport &queryCoopVecMatMulFormatSupport(
        coopvec::MatMulFormatSupport &retVal, const coopvec::MatMulFormatCombo &combination) noexcept override
    {
        unsupported();
        return retVal;
    }
    coopvec::TrainingFormatSupport &queryCoopVecTrainingFormatSupport(coopvec::TrainingFormatSupport &retVal,
                                                                      coopvec::DataType componentType) noexcept override
    {
        unsupported();
        return retVal;
    }
    size_t getCoopVecMatrixSize(coopvec::DataType type, coopvec::MatrixLayout layout, int rows,
                                int columns) noexcept override
    {
        unsupported();
        return {};
    }
    NativeObject getNativeQueue(ObjectType objectType, CommandQueue queue) noexcept override;
    IMessageCallback *getMessageCallback() noexcept override;
    bool isAftermathEnabled() noexcept override
    {
        return false;
    }
    IAftermathCrashDumpHelper *getAftermathCrashDumpHelper() noexcept override
    {
        return nullptr;
    }
};

NVRHI_CLASS_CLSID(CommandList, "5490c012-09e1-40ad-a1ab-701827580012")
class CommandList : public ObjectImpl<ICommandList>
{
  public:
    CUDA_INTERFACES(CommandList, ICommandList)
    ContextPtr context;
    void unsupported() noexcept
    {
        valid = false;
        context->error("CUDA backend: unsupported operation");
    }
    AutoPtr<IDevice> owner;
    CommandListParameters params;
    bool opened = false;
    bool valid = true;
    std::vector<Command> commands;
    ComputeState computeState;
    std::vector<RHIObjectHandle> stateRefs;
    std::vector<uint8_t> pushConstants;
    rt::State rayState;
    CommandList(ContextPtr c, IDevice *d, const CommandListParameters &p) : context(c), owner(d), params(p)
    {
    }
    void record(Command command)
    {
        if (!opened)
        {
            valid = false;
            context->error("Command list must be open");
            return;
        }
        commands.push_back(std::move(command));
    }
    void open() noexcept override;
    void close() noexcept override;
    void clearState() noexcept override;
    void clearTextureFloat(ITexture *t, const TextureSubresourceSet &subresources,
                           const Color &clearColor) noexcept override;
    void clearDepthStencilTexture(ITexture *t, const TextureSubresourceSet &subresources, bool clearDepth, float depth,
                                  bool clearStencil, uint8_t stencil) noexcept override
    {
        unsupported();
    }
    void clearTextureUInt(ITexture *t, const TextureSubresourceSet &subresources,
                          uint32_t clearColor) noexcept override;
    void copyTexture1(ITexture *dest, const TextureSlice &destSlice, ITexture *src,
                      const TextureSlice &srcSlice) noexcept override;
    void copyTexture2(IStagingTexture *dest, const TextureSlice &destSlice, ITexture *src,
                      const TextureSlice &srcSlice) noexcept override;
    void copyTexture3(ITexture *dest, const TextureSlice &destSlice, IStagingTexture *src,
                      const TextureSlice &srcSlice) noexcept override;
    void writeTexture(ITexture *dest, uint32_t arraySlice, uint32_t mipLevel, const void *data, size_t rowPitch,
                      size_t depthPitch = 0) noexcept override;
    void resolveTexture(ITexture *dest, const TextureSubresourceSet &dstSubresources, ITexture *src,
                        const TextureSubresourceSet &srcSubresources) noexcept override
    {
        unsupported();
    }
    void writeBuffer(IBuffer *b, const void *data, size_t dataSize, uint64_t destOffsetBytes = 0) noexcept override;
    void clearBufferUInt(IBuffer *b, uint32_t clearValue) noexcept override;
    void copyBuffer(IBuffer *dest, uint64_t destOffsetBytes, IBuffer *src, uint64_t srcOffsetBytes,
                    uint64_t dataSizeBytes) noexcept override;
    void clearSamplerFeedbackTexture(ISamplerFeedbackTexture *texture) noexcept override
    {
        unsupported();
    }
    void decodeSamplerFeedbackTexture(IBuffer *buffer, ISamplerFeedbackTexture *texture,
                                      nvrhi::Format format) noexcept override
    {
        unsupported();
    }
    void setSamplerFeedbackTextureState(ISamplerFeedbackTexture *texture, ResourceStates stateBits) noexcept override
    {
        unsupported();
    }
    void setPushConstants(const void *data, size_t byteSize) noexcept override;
    void setGraphicsState(const GraphicsState &state) noexcept override
    {
        unsupported();
    }
    void draw(const DrawArguments &args) noexcept override
    {
        unsupported();
    }
    void drawIndexed(const DrawArguments &args) noexcept override
    {
        unsupported();
    }
    void drawIndirect(uint32_t offsetBytes, uint32_t drawCount = 1) noexcept override
    {
        unsupported();
    }
    void drawIndexedIndirect(uint32_t offsetBytes, uint32_t drawCount = 1) noexcept override
    {
        unsupported();
    }
    void drawIndexedIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes,
                                  uint32_t maxDrawCount) noexcept override
    {
        unsupported();
    }
    void setComputeState(const ComputeState &state) noexcept override;
    void dispatch(uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) noexcept override;
    void dispatchIndirect(uint32_t offsetBytes) noexcept override;
    void setMeshletState(const MeshletState &state) noexcept override
    {
        unsupported();
    }
    void dispatchMesh(uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) noexcept override
    {
        unsupported();
    }
    void dispatchMeshIndirect(uint32_t offsetBytes, uint32_t maxDrawCount) noexcept override
    {
        unsupported();
    }
    void dispatchMeshIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes,
                                   uint32_t maxDrawCount) noexcept override
    {
        unsupported();
    }
    void setRayTracingState(const rt::State &state) noexcept override;
    void dispatchRays(const rt::DispatchRaysArguments &args) noexcept override;
    void buildOpacityMicromap(rt::IOpacityMicromap *omm, const rt::OpacityMicromapDesc &desc) noexcept override
    {
        unsupported();
    }
    void buildBottomLevelAccelStruct(
        rt::IAccelStruct *as, const rt::GeometryDesc *pGeometries, size_t numGeometries,
        rt::AccelStructBuildFlags buildFlags = rt::AccelStructBuildFlags::None) noexcept override;
    void compactBottomLevelAccelStructs() noexcept override;
    void copyRaytracingAccelerationStructure(rt::IAccelStruct *destination, rt::IAccelStruct *source) noexcept override;
    void buildTopLevelAccelStruct(
        rt::IAccelStruct *as, const rt::InstanceDesc *pInstances, size_t numInstances,
        rt::AccelStructBuildFlags buildFlags = rt::AccelStructBuildFlags::None) noexcept override;
    void executeMultiIndirectClusterOperation(const rt::cluster::OperationDesc &desc) noexcept override
    {
        unsupported();
    }
    void buildTopLevelAccelStructFromBuffer(
        rt::IAccelStruct *as, nvrhi::IBuffer *instanceBuffer, uint64_t instanceBufferOffset, size_t numInstances,
        rt::AccelStructBuildFlags buildFlags = rt::AccelStructBuildFlags::None) noexcept override;
    void convertCoopVecMatrices(coopvec::ConvertMatrixLayoutDesc const *convertDescs, size_t numDescs) noexcept override
    {
        unsupported();
    }
    void beginTimerQuery(ITimerQuery *query) noexcept override;
    void endTimerQuery(ITimerQuery *query) noexcept override;
    void beginMarker(const char *name) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void endMarker() noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void setEnableAutomaticBarriers(bool enable) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void setResourceStatesForBindingSet(IBindingSet *bindingSet) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void setEnableUavBarriersForTexture(ITexture *texture, bool enableBarriers) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void setEnableUavBarriersForBuffer(IBuffer *buffer, bool enableBarriers) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void beginTrackingTextureState(ITexture *texture, const TextureSubresourceSet &subresources,
                                   ResourceStates stateBits) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void beginTrackingBufferState(IBuffer *buffer, ResourceStates stateBits) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void setTextureState(ITexture *texture, const TextureSubresourceSet &subresources,
                         ResourceStates stateBits) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void setBufferState(IBuffer *buffer, ResourceStates stateBits) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void setAccelStructState(rt::IAccelStruct *as, ResourceStates stateBits) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void setPermanentTextureState(ITexture *texture, ResourceStates stateBits) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void setPermanentBufferState(IBuffer *buffer, ResourceStates stateBits) noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    void commitBarriers() noexcept override
    { /* Stream submission order provides compute/transfer memory ordering. */
    }
    ResourceStates getTextureSubresourceState(ITexture *texture, ArraySlice arraySlice,
                                              MipLevel mipLevel) noexcept override
    {
        return ResourceStates::Common;
    }
    ResourceStates getBufferState(IBuffer *buffer) noexcept override
    {
        return ResourceStates::Common;
    }
    IDevice *getDevice() noexcept override;
    const CommandListParameters &getDesc() noexcept override;
};
} // namespace nvrhi::cuda
