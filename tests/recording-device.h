// CPU-only recording device for artifact contract tests. Unexpected GPU calls abort.
#pragma once
#include <nvrhi/nvrhi.h>
#include <nvrhi/core/foundation.h>
#include <cstdlib>
#include <vector>

namespace nvrhi_test {
using namespace nvrhi;
class RecordedShader final : public ObjectImpl<IShader> {
    ShaderDesc m_desc;
    std::vector<unsigned char> m_code;
public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(RecordedShader)
    NVRHI_IMPLEMENTS_INTERFACE(IShader)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    RecordedShader(const ShaderDesc& desc, const void* code, size_t size) : m_desc(desc),
        m_code(static_cast<const unsigned char*>(code), static_cast<const unsigned char*>(code) + size) {}
    const ShaderDesc& getDesc() const noexcept override { return m_desc; }
    void getBytecode(const void** code, size_t* size) const noexcept override {
        if (code) *code = m_code.data();
        if (size) *size = m_code.size();
    }
};
class RecordingDevice final : public ObjectImpl<IDevice> {
public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(RecordingDevice)
    NVRHI_IMPLEMENTS_INTERFACE(IDevice)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    GraphicsAPI api;
    unsigned shaderCalls = 0;
    bool failCreation = false;
    explicit RecordingDevice(GraphicsAPI value) : api(value) {}
    FRESULT createHeap(const HeapDesc& d, IHeap** ppHeap) noexcept override { std::abort(); }
    bool queryTopLevelAccelStructPrebuildInfo(const rt::AccelStructDesc& desc, uint32_t instanceCount, rt::AccelStructPrebuildInfo& outInfo) noexcept override { std::abort(); }
    FRESULT createTexture(const TextureDesc& d, ITexture** ppTexture) noexcept override { std::abort(); }
    MemoryRequirements& getTextureMemoryRequirements(MemoryRequirements& retVal, ITexture* texture) noexcept override { std::abort(); }
    bool bindTextureMemory(ITexture* texture, IHeap* heap, uint64_t offset) noexcept override { std::abort(); }
    FRESULT createHandleForNativeTexture(ObjectType objectType, NativeObject texture, const TextureDesc& desc, ITexture** ppTexture) noexcept override { std::abort(); }
    FRESULT createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess, IStagingTexture** ppStagingTexture) noexcept override { std::abort(); }
    void* mapStagingTexture(IStagingTexture* tex, const TextureSlice& slice, CpuAccessMode cpuAccess, size_t& outRowPitch) noexcept override { std::abort(); }
    void unmapStagingTexture(IStagingTexture* tex) noexcept override { std::abort(); }
    void getTextureTiling(ITexture* texture, _Out_opt_ uint32_t* numTiles, _Out_opt_ PackedMipDesc* desc, _Out_opt_ TileShape* tileShape, _Inout_opt_ uint32_t* subresourceTilingsNum, _Out_opt_ SubresourceTiling* subresourceTilings) noexcept override { std::abort(); }
    void updateTextureTileMappings(ITexture* texture, const TextureTilesMapping* tileMappings, uint32_t numTileMappings, CommandQueue executionQueue = CommandQueue::Graphics) noexcept override { std::abort(); }
    FRESULT createSamplerFeedbackTexture(ITexture* pairedTexture, const SamplerFeedbackTextureDesc& desc, ISamplerFeedbackTexture** ppTexture) noexcept override { std::abort(); }
    FRESULT createSamplerFeedbackForNativeTexture(ObjectType objectType, NativeObject texture, ITexture* pairedTexture, ISamplerFeedbackTexture** ppTexture) noexcept override { std::abort(); }
    FRESULT createBuffer(const BufferDesc& d, IBuffer** ppBuffer) noexcept override { std::abort(); }
    void* mapBuffer(IBuffer* buffer, CpuAccessMode cpuAccess) noexcept override { std::abort(); }
    void unmapBuffer(IBuffer* buffer) noexcept override { std::abort(); }
    MemoryRequirements& getBufferMemoryRequirements(MemoryRequirements& retVal, IBuffer* buffer) noexcept override { std::abort(); }
    bool bindBufferMemory(IBuffer* buffer, IHeap* heap, uint64_t offset) noexcept override { std::abort(); }
    FRESULT createHandleForNativeBuffer(ObjectType objectType, NativeObject buffer, const BufferDesc& desc, IBuffer** ppBuffer) noexcept override { std::abort(); }
    FRESULT createShader(const ShaderDesc& d, const void* binary, size_t binarySize, IShader** ppShader) noexcept override { if (!ppShader) return FE_INVALID_ARGS;
        *ppShader = nullptr;
        ++shaderCalls;
        if (failCreation) return FE_GENERIC_ERROR;
        auto result = MAKE_RC_OBJ_PTR(RecordedShader, d, binary, binarySize);
        *ppShader = result.Detach(); return FS_OK; }
    FRESULT createShaderSpecialization(IShader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants, IShader** ppShader) noexcept override { std::abort(); }
    FRESULT createShaderLibrary(const void* binary, size_t binarySize, IShaderLibrary** ppShaderLibrary) noexcept override { std::abort(); }
    FRESULT createSampler(const SamplerDesc& d, ISampler** ppSampler) noexcept override { std::abort(); }
    FRESULT createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, _In_opt_ IShader* vertexShader, IInputLayout** ppInputLayout) noexcept override { std::abort(); }
    FRESULT createEventQuery(IEventQuery** ppQuery) noexcept override { std::abort(); }
    void setEventQuery(IEventQuery* query, CommandQueue queue) noexcept override { std::abort(); }
    bool pollEventQuery(IEventQuery* query) noexcept override { std::abort(); }
    void waitEventQuery(IEventQuery* query) noexcept override { std::abort(); }
    void resetEventQuery(IEventQuery* query) noexcept override { std::abort(); }
    FRESULT createTimerQuery(ITimerQuery** ppQuery) noexcept override { std::abort(); }
    bool pollTimerQuery(ITimerQuery* query) noexcept override { std::abort(); }
    float getTimerQueryTime(ITimerQuery* query) noexcept override { std::abort(); }
    void resetTimerQuery(ITimerQuery* query) noexcept override { std::abort(); }
    GraphicsAPI getGraphicsAPI() noexcept override { return api; }
    FRESULT createFramebuffer(const FramebufferDesc& desc, IFramebuffer** ppFramebuffer) noexcept override { std::abort(); }
    FRESULT createGraphicsPipeline1(const GraphicsPipelineDesc& desc, const FramebufferInfo& fbinfo, IGraphicsPipeline** ppPipeline) noexcept override { std::abort(); }
    FRESULT createGraphicsPipeline2(const GraphicsPipelineDesc& desc, IFramebuffer* fb, IGraphicsPipeline** ppPipeline) noexcept override { std::abort(); }
    FRESULT createComputePipeline(const ComputePipelineDesc& desc, IComputePipeline** ppPipeline) noexcept override { std::abort(); }
    FRESULT createMeshletPipeline1(const MeshletPipelineDesc& desc, const FramebufferInfo& fbinfo, IMeshletPipeline** ppPipeline) noexcept override { std::abort(); }
    FRESULT createMeshletPipeline2(const MeshletPipelineDesc& desc, IFramebuffer* fb, IMeshletPipeline** ppPipeline) noexcept override { std::abort(); }
    FRESULT createRayTracingPipeline(const rt::PipelineDesc& desc, rt::IPipeline** ppPipeline) noexcept override { std::abort(); }
    FRESULT createBindingLayout(const BindingLayoutDesc& desc, IBindingLayout** ppLayout) noexcept override { std::abort(); }
    FRESULT createBindlessLayout(const BindlessLayoutDesc& desc, IBindingLayout** ppLayout) noexcept override { std::abort(); }
    FRESULT createBindingSet(const BindingSetDesc& desc, IBindingLayout* layout, IBindingSet** ppBindingSet) noexcept override { std::abort(); }
    FRESULT createDescriptorTable(IBindingLayout* layout, IDescriptorTable** ppDescriptorTable) noexcept override { std::abort(); }
    void resizeDescriptorTable(IDescriptorTable* descriptorTable, uint32_t newSize, bool keepContents = true) noexcept override { std::abort(); }
    bool writeDescriptorTable(IDescriptorTable* descriptorTable, const BindingSetItem& item) noexcept override { std::abort(); }
    FRESULT createOpacityMicromap(const rt::OpacityMicromapDesc& desc, rt::IOpacityMicromap** ppOpacityMicromap) noexcept override { std::abort(); }
    FRESULT createAccelStruct(const rt::AccelStructDesc& desc, rt::IAccelStruct** ppAccelStruct) noexcept override { std::abort(); }
    MemoryRequirements& getAccelStructMemoryRequirements(MemoryRequirements& retVal, rt::IAccelStruct* as) noexcept override { std::abort(); }
    rt::cluster::OperationSizeInfo& getClusterOperationSizeInfo(rt::cluster::OperationSizeInfo& retVal, const rt::cluster::OperationParams& params) noexcept override { std::abort(); }
    bool bindAccelStructMemory(rt::IAccelStruct* as, IHeap* heap, uint64_t offset) noexcept override { std::abort(); }
    FRESULT createCommandList(const CommandListParameters& params, ICommandList** ppCommandList) noexcept override { std::abort(); }
    uint64_t executeCommandLists(ICommandList* const* pCommandLists, size_t numCommandLists, CommandQueue executionQueue = CommandQueue::Graphics) noexcept override { std::abort(); }
    void queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue, uint64_t instance) noexcept override { std::abort(); }
    bool waitForIdle() noexcept override { std::abort(); }
    FRESULT createCommandListLifetimeTracker(CommandQueue executionQueue, ICommandListLifetimeTracker** ppTracker) noexcept override { std::abort(); }
    void runGarbageCollection() noexcept override { std::abort(); }
    bool queryFeatureSupport(Feature feature, _Inout_updates_bytes_opt_(infoSize) void* pInfo = nullptr, size_t infoSize = 0) noexcept override { std::abort(); }
    FormatSupport queryFormatSupport(Format format) noexcept override { std::abort(); }
    coopvec::DeviceFeatures& queryCoopVecFeatures(coopvec::DeviceFeatures& retVal) noexcept override { std::abort(); }
    coopvec::MatMulFormatSupport& queryCoopVecMatMulFormatSupport(coopvec::MatMulFormatSupport& retVal, const coopvec::MatMulFormatCombo& combination) noexcept override { std::abort(); }
    coopvec::TrainingFormatSupport& queryCoopVecTrainingFormatSupport(coopvec::TrainingFormatSupport& retVal, coopvec::DataType componentType) noexcept override { std::abort(); }
    size_t getCoopVecMatrixSize(coopvec::DataType type, coopvec::MatrixLayout layout, int rows, int columns) noexcept override { std::abort(); }
    NativeObject getNativeQueue(ObjectType objectType, CommandQueue queue) noexcept override { std::abort(); }
    IMessageCallback* getMessageCallback() noexcept override { std::abort(); }
    bool isAftermathEnabled() noexcept override { std::abort(); }
    IAftermathCrashDumpHelper* getAftermathCrashDumpHelper() noexcept override { std::abort(); }
};
}
