/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
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

#include <nvrhi/validation.h>
#include <unordered_set>

namespace nvrhi::validation
{
    class DeviceWrapper;

    struct Range
    {
        uint32_t min = ~0u;
        uint32_t max = 0;

        void add(uint32_t item);
        [[nodiscard]] bool empty() const;
        [[nodiscard]] bool overlapsWith(const Range& other) const;
    };

    enum class GraphicsResourceType : uint32_t
    {
        SRV,
        Sampler,
        UAV,
        CB
    };

    struct BindingLocation
    {
        GraphicsResourceType type = GraphicsResourceType::SRV;
        uint32_t registerSpace = 0;
        uint32_t slot = 0;
        uint32_t arrayElement = 0;

        bool operator==(BindingLocation const& other) const
        {
            return type == other.type
                && registerSpace == other.registerSpace
                && slot == other.slot
                && arrayElement == other.arrayElement;
        }

        bool operator!=(BindingLocation const& other) const
        {
            return !(*this == other);
        }
    };
} // namespace nvrhi::validation

namespace std
{
    template<> struct hash<nvrhi::validation::BindingLocation>
    {
        std::size_t operator()(nvrhi::validation::BindingLocation const& s) const noexcept
        {
            size_t hash = 0;
            nvrhi::hash_combine(hash, uint32_t(s.type));
            nvrhi::hash_combine(hash, s.registerSpace);
            nvrhi::hash_combine(hash, s.slot);
            nvrhi::hash_combine(hash, s.arrayElement);
            return hash;
        }
    };
} // namespace std

namespace nvrhi::validation
{
    typedef std::unordered_set<BindingLocation> BindingLocationSet;

    struct BindingSummary
    {
        BindingLocationSet locations;
        uint32_t numVolatileCBs = 0;
        Range rangeSRV;
        Range rangeSampler;
        Range rangeUAV;
        Range rangeCB;

        [[nodiscard]] bool any() const;
        [[nodiscard]] bool overlapsWith(const BindingSummary& other) const;
    };
    
    std::ostream& operator<<(std::ostream& os, const BindingLocationSet& set);

    enum class CommandListState
    {
        INITIAL,
        OPEN,
        CLOSED
    };

    IRHIObject* unwrapResource(IRHIObject* resource);

    // The validation wrapper behind `object`, or null when it is not one (a failed QueryInterface for the
    // wrapper's class ID). Replaces dynamic_cast, since NVRHI is built without RTTI. Keeps no reference: the
    // caller holds `object`, which keeps the returned wrapper alive.
    template <typename Wrapper, typename Interface>
    Wrapper* queryWrapper(Interface* object)
    {
        if (!object)
            return nullptr;

        AutoPtr<Wrapper> wrapper;
        if (NVRHI_FAILED(object->QueryInterface(uuid_of<Wrapper>(), reinterpret_cast<void**>(wrapper.GetAddressOf()))))
            return nullptr;

        return wrapper.Get();
    }

    NVRHI_CLASS_CLSID(AccelStructWrapper, "b9c9675d-ac40-48f8-8af6-210cd372a85e")
    class AccelStructWrapper : public ObjectImpl<rt::IAccelStruct>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(AccelStructWrapper)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(AccelStructWrapper)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::rt::IAccelStruct)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(AccelStructWrapper)
        NVRHI_END_INTERFACE_TABLE()

        bool isTopLevel = false;
        bool allowCompaction = false;
        bool allowUpdate = false;
        bool wasBuilt = false;

        // BLAS only
        std::vector<rt::GeometryDesc> buildGeometries;

        // TLAS only
        size_t maxInstances = 0;
        size_t buildInstances = 0;

        AccelStructWrapper(IAccelStruct* as) : m_AccelStruct(as) { }
        IAccelStruct* getUnderlyingObject() const { return m_AccelStruct; }

        // IRHIObject

        Object getNativeObject(ObjectType objectType) override { return m_AccelStruct->getNativeObject(objectType); }
        bool queryMemoryRequirements(MemoryRequirements& outRequirements) override
        {
            return m_AccelStruct->queryMemoryRequirements(outRequirements);
        }

        // IAccelStruct

        const rt::AccelStructDesc& getDesc() const override { return m_AccelStruct->getDesc(); }
        bool isCompacted() const override { return m_AccelStruct->isCompacted(); }
        uint64_t getDeviceAddress() const override { return m_AccelStruct->getDeviceAddress(); };
        
    private:
        rt::AccelStructHandle m_AccelStruct;
    };
    
    NVRHI_CLASS_CLSID(CommandListWrapper, "b1cd41fd-ca88-491c-8fe3-f0e9dc5ce85a")
    class CommandListWrapper : public ObjectImpl<ICommandList>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(CommandListWrapper)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(CommandListWrapper)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ICommandList)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(CommandListWrapper)
        NVRHI_END_INTERFACE_TABLE()

        friend class DeviceWrapper;

        CommandListWrapper(DeviceWrapper* device, ICommandList* commandList, bool isImmediate, CommandQueue queueType);

    protected:
        CommandListHandle m_CommandList;
        AutoPtr<DeviceWrapper> m_Device;
        IMessageCallback* m_MessageCallback;
        bool m_IsImmediate;
        CommandQueue m_type;

        CommandListState m_State = CommandListState::INITIAL;
        bool m_GraphicsStateSet = false;
        bool m_ComputeStateSet = false;
        bool m_MeshletStateSet = false;
        bool m_RayTracingStateSet = false;
        GraphicsState m_CurrentGraphicsState;
        ComputeState m_CurrentComputeState;
        MeshletState m_CurrentMeshletState;
        rt::State m_CurrentRayTracingState;

        size_t m_PipelinePushConstantSize = 0;
        bool m_PushConstantsSet = false;

        void error(const std::string& messageText) const;
        void warning(const std::string& messageText) const;

        bool requireOpenState() const;
        bool requireExecuteState();
        bool requireType(CommandQueue queueType, const char* operation) const;
        ICommandList* getUnderlyingCommandList() const { return m_CommandList; }

        void evaluatePushConstantSize(const nvrhi::BindingLayoutVector& bindingLayouts);
        bool validatePushConstants(const char* pipelineType, const char* stateFunctionName) const;
        bool validateBindingSetsAgainstLayouts(const static_vector<BindingLayoutHandle, c_MaxBindingLayouts>& layouts, const static_vector<IBindingSet*, c_MaxBindingLayouts>& sets) const;

        bool validateBuildTopLevelAccelStruct(AccelStructWrapper* wrapper, size_t numInstances, rt::AccelStructBuildFlags buildFlags) const;

    public:

        // IRHIObject implementation

        Object getNativeObject(ObjectType objectType) override;

        // ICommandList implementation

        void open() override;
        void close() override;
        void clearState() override;

        void clearTextureFloat(ITexture* t, TextureSubresourceSet subresources, const Color& clearColor) override;
        void clearDepthStencilTexture(ITexture* t, TextureSubresourceSet subresources, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) override;
        void clearTextureUInt(ITexture* t, TextureSubresourceSet subresources, uint32_t clearColor) override;

        void copyTexture(ITexture* dest, const TextureSlice& destSlice, ITexture* src, const TextureSlice& srcSlice) override;
        void copyTexture(IStagingTexture* dest, const TextureSlice& destSlice, ITexture* src, const TextureSlice& srcSlice) override;
        void copyTexture(ITexture* dest, const TextureSlice& destSlice, IStagingTexture* src, const TextureSlice& srcSlice) override;
        void writeTexture(ITexture* dest, uint32_t arraySlice, uint32_t mipLevel, const void* data, size_t rowPitch, size_t depthPitch) override;
        void resolveTexture(ITexture* dest, const TextureSubresourceSet& dstSubresources, ITexture* src, const TextureSubresourceSet& srcSubresources) override;

        void writeBuffer(IBuffer* b, const void* data, size_t dataSize, uint64_t destOffsetBytes) override;
        void clearBufferUInt(IBuffer* b, uint32_t clearValue) override;
        void copyBuffer(IBuffer* dest, uint64_t destOffsetBytes, IBuffer* src, uint64_t srcOffsetBytes, uint64_t dataSizeBytes) override;

        void clearSamplerFeedbackTexture(ISamplerFeedbackTexture* texture) override;
        void decodeSamplerFeedbackTexture(IBuffer* buffer, ISamplerFeedbackTexture* texture, nvrhi::Format format) override;
        void setSamplerFeedbackTextureState(ISamplerFeedbackTexture* texture, ResourceStates stateBits) override;

        void setPushConstants(const void* data, size_t byteSize) override;

        void setGraphicsState(const GraphicsState& state) override;
        void draw(const DrawArguments& args) override;
        void drawIndexed(const DrawArguments& args) override;
        void drawIndirect(uint32_t offsetBytes, uint32_t drawCount) override;
        void drawIndexedIndirect(uint32_t offsetBytes, uint32_t drawCount) override;
        void drawIndexedIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount) override;

        void setComputeState(const ComputeState& state) override;
        void dispatch(uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) override;
        void dispatchIndirect(uint32_t offsetBytes)  override;

        void setMeshletState(const MeshletState& state) override;
        void dispatchMesh(uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) override;
        void dispatchMeshIndirect(uint32_t offsetBytes, uint32_t maxDrawCount) override;
        void dispatchMeshIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount) override;

        void setRayTracingState(const rt::State& state) override;
        void dispatchRays(const rt::DispatchRaysArguments& args) override;

        void buildOpacityMicromap(rt::IOpacityMicromap* omm, const rt::OpacityMicromapDesc& desc) override;
        void buildBottomLevelAccelStruct(rt::IAccelStruct* as, const rt::GeometryDesc* pGeometries, size_t numGeometries, rt::AccelStructBuildFlags buildFlags) override;
        void compactBottomLevelAccelStructs() override;
        void copyRaytracingAccelerationStructure(rt::IAccelStruct* destination, rt::IAccelStruct* source) override;
        void buildTopLevelAccelStruct(rt::IAccelStruct* as, const rt::InstanceDesc* pInstances, size_t numInstances, rt::AccelStructBuildFlags buildFlags) override;
        void buildTopLevelAccelStructFromBuffer(rt::IAccelStruct* as, nvrhi::IBuffer* instanceBuffer, uint64_t instanceBufferOffset, size_t numInstances,
            rt::AccelStructBuildFlags buildFlags = rt::AccelStructBuildFlags::None) override;
        void executeMultiIndirectClusterOperation(const rt::cluster::OperationDesc& desc) override;

        void convertCoopVecMatrices(coopvec::ConvertMatrixLayoutDesc const* convertDescs, size_t numDescs) override;

        void beginTimerQuery(ITimerQuery* query) override;
        void endTimerQuery(ITimerQuery* query) override;

        void beginMarker(const char* name) override;
        void endMarker() override;

        void setEnableAutomaticBarriers(bool enable) override;
        void setResourceStatesForBindingSet(IBindingSet* bindingSet) override;

        void setEnableUavBarriersForTexture(ITexture* texture, bool enableBarriers) override;
        void setEnableUavBarriersForBuffer(IBuffer* buffer, bool enableBarriers) override;

        void beginTrackingTextureState(ITexture* texture, TextureSubresourceSet subresources, ResourceStates stateBits) override;
        void beginTrackingBufferState(IBuffer* buffer, ResourceStates stateBits) override;

        void setTextureState(ITexture* texture, TextureSubresourceSet subresources, ResourceStates stateBits) override;
        void setBufferState(IBuffer* buffer, ResourceStates stateBits) override;
        void setAccelStructState(rt::IAccelStruct* as, ResourceStates stateBits) override;

        void setPermanentTextureState(ITexture* texture, ResourceStates stateBits) override;
        void setPermanentBufferState(IBuffer* buffer, ResourceStates stateBits) override;

        void commitBarriers() override;
        
        ResourceStates getTextureSubresourceState(ITexture* texture, ArraySlice arraySlice, MipLevel mipLevel) override;
        ResourceStates getBufferState(IBuffer* buffer) override;

        IDevice* getDevice() override;
        const CommandListParameters& getDesc() override;
    };

    NVRHI_CLASS_CLSID(DeviceWrapper, "9301ed69-58b0-454d-9d5d-01bb091e8195")
    class DeviceWrapper : public ObjectImpl<IDevice>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(DeviceWrapper)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DeviceWrapper)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IDevice)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(DeviceWrapper)
        NVRHI_END_INTERFACE_TABLE()

        friend class CommandListWrapper;

        DeviceWrapper(IDevice* device);
        
    protected:
        DeviceHandle m_Device;
        IMessageCallback* m_MessageCallback;
        std::atomic<unsigned int> m_NumOpenImmediateCommandLists = 0;

        void error(const std::string& messageText) const;
        void warning(const std::string& messageText) const;

        bool validateBindingSetItem(const BindingSetItem& binding, IDescriptorTable *pOptDescriptorTable, std::stringstream& errorStream);
        bool validatePipelineBindingLayouts(const static_vector<BindingLayoutHandle, c_MaxBindingLayouts>& bindingLayouts, const std::vector<IShader*>& shaders) const;
        bool validateShaderType(ShaderType expected, const ShaderDesc& shaderDesc, const char* function) const;
        bool validateRenderState(const RenderState& renderState, FramebufferInfo const& fbinfo) const;

        bool validateClusterOperationParams(const rt::cluster::OperationParams& params) const;
    public:

        // IRHIObject implementation

        Object getNativeObject(ObjectType objectType) override;

        // IDevice implementation

        HeapHandle createHeap(const HeapDesc& d) override;

        TextureHandle createTexture(const TextureDesc& d) override;
        MemoryRequirements getTextureMemoryRequirements(ITexture* texture) override;
        bool bindTextureMemory(ITexture* texture, IHeap* heap, uint64_t offset) override;

        TextureHandle createHandleForNativeTexture(ObjectType objectType, Object texture, const TextureDesc& desc) override;

        StagingTextureHandle createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess) override;
        void *mapStagingTexture(IStagingTexture* tex, const TextureSlice& slice, CpuAccessMode cpuAccess, size_t *outRowPitch) override;
        void unmapStagingTexture(IStagingTexture* tex) override;

        void getTextureTiling(ITexture* texture, uint32_t* numTiles, PackedMipDesc* desc, TileShape* tileShape, uint32_t* subresourceTilingsNum, SubresourceTiling* subresourceTilings) override;
        void updateTextureTileMappings(ITexture* texture, const TextureTilesMapping* tileMappings, uint32_t numTileMappings, CommandQueue executionQueue = CommandQueue::Graphics) override;

        SamplerFeedbackTextureHandle createSamplerFeedbackTexture(ITexture* pairedTexture, const SamplerFeedbackTextureDesc& desc) override;
        SamplerFeedbackTextureHandle createSamplerFeedbackForNativeTexture(ObjectType objectType, Object texture, ITexture* pairedTexture) override;

        BufferHandle createBuffer(const BufferDesc& d) override;
        void *mapBuffer(IBuffer* b, CpuAccessMode mapFlags) override;
        void unmapBuffer(IBuffer* b) override;
        MemoryRequirements getBufferMemoryRequirements(IBuffer* buffer) override;
        bool queryTopLevelAccelStructPrebuildInfo(const rt::AccelStructDesc& desc,
            uint32_t instanceCount, rt::AccelStructPrebuildInfo& outInfo) override;
        bool bindBufferMemory(IBuffer* buffer, IHeap* heap, uint64_t offset) override;

        BufferHandle createHandleForNativeBuffer(ObjectType objectType, Object buffer, const BufferDesc& desc) override;

        ShaderHandle createShader(const ShaderDesc& d, const void* binary, size_t binarySize) override;
        ShaderHandle createShaderSpecialization(IShader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants) override;
        ShaderLibraryHandle createShaderLibrary(const void* binary, size_t binarySize) override;

        SamplerHandle createSampler(const SamplerDesc& d) override;

        InputLayoutHandle createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, IShader* vertexShader) override;

        // event queries
        EventQueryHandle createEventQuery() override;
        void setEventQuery(IEventQuery* query, CommandQueue queue) override;
        bool pollEventQuery(IEventQuery* query) override;
        void waitEventQuery(IEventQuery* query) override;
        void resetEventQuery(IEventQuery* query) override;

        // timer queries
        TimerQueryHandle createTimerQuery() override;
        bool pollTimerQuery(ITimerQuery* query) override;
        float getTimerQueryTime(ITimerQuery* query) override;
        void resetTimerQuery(ITimerQuery* query) override;

        GraphicsAPI getGraphicsAPI() override;

        FramebufferHandle createFramebuffer(const FramebufferDesc& desc) override;

        GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc, FramebufferInfo const& fbinfo) override;

        GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc, IFramebuffer* fb) override;

        ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;

        MeshletPipelineHandle createMeshletPipeline(const MeshletPipelineDesc& desc, FramebufferInfo const& fbinfo) override;

        MeshletPipelineHandle createMeshletPipeline(const MeshletPipelineDesc& desc, IFramebuffer* fb) override;

        rt::PipelineHandle createRayTracingPipeline(const rt::PipelineDesc& desc) override;

        BindingLayoutHandle createBindingLayout(const BindingLayoutDesc& desc) override;
        BindingLayoutHandle createBindlessLayout(const BindlessLayoutDesc& desc) override;

        BindingSetHandle createBindingSet(const BindingSetDesc& desc, IBindingLayout* layout) override;
        DescriptorTableHandle createDescriptorTable(IBindingLayout* layout) override;

        void resizeDescriptorTable(IDescriptorTable* descriptorTable, uint32_t newSize, bool keepContents = true) override;
        bool writeDescriptorTable(IDescriptorTable* descriptorTable, const BindingSetItem& item) override;

        rt::OpacityMicromapHandle createOpacityMicromap(const rt::OpacityMicromapDesc& desc)  override;
        rt::AccelStructHandle createAccelStruct(const rt::AccelStructDesc& desc) override;
        MemoryRequirements getAccelStructMemoryRequirements(rt::IAccelStruct* as) override;
        rt::cluster::OperationSizeInfo getClusterOperationSizeInfo(const rt::cluster::OperationParams& params) override;
        bool bindAccelStructMemory(rt::IAccelStruct* as, IHeap* heap, uint64_t offset) override;

        CommandListHandle createCommandList(const CommandListParameters& params = CommandListParameters()) override;
        uint64_t executeCommandLists(ICommandList* const* pCommandLists, size_t numCommandLists, CommandQueue executionQueue = CommandQueue::Graphics) override;
        void queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue, uint64_t instance) override;
        bool waitForIdle() override;
        CommandListLifetimeTrackerHandle createCommandListLifetimeTracker(CommandQueue executionQueue) override;
        void runGarbageCollection() override;
        bool queryFeatureSupport(Feature feature, void* pInfo = nullptr, size_t infoSize = 0) override;
        FormatSupport queryFormatSupport(Format format) override;
        coopvec::DeviceFeatures queryCoopVecFeatures() override;
        coopvec::MatMulFormatSupport queryCoopVecMatMulFormatSupport(const coopvec::MatMulFormatCombo& combination) override;
        coopvec::TrainingFormatSupport queryCoopVecTrainingFormatSupport(coopvec::DataType componentType) override;
        size_t getCoopVecMatrixSize(coopvec::DataType type, coopvec::MatrixLayout layout, int rows, int columns) override;
        Object getNativeQueue(ObjectType objectType, CommandQueue queue) override;
        IMessageCallback* getMessageCallback() override;
        bool isAftermathEnabled() override;
        AftermathCrashDumpHelper& getAftermathCrashDumpHelper() override;
    };

} // namespace nvrhi::validation
