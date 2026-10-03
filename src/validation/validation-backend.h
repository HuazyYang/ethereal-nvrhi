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
#include <nvrhi/core/foundation.h>
#include <string>
#include <unordered_set>
#include <vector>

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

        NativeObject getNativeObject(ObjectType objectType) noexcept override { return m_AccelStruct->getNativeObject(objectType); }
        bool queryMemoryRequirements(MemoryRequirements& outRequirements) noexcept override
        {
            return m_AccelStruct->queryMemoryRequirements(outRequirements);
        }

        // IAccelStruct

        const rt::AccelStructDesc& getDesc() const noexcept override { return m_AccelStruct->getDesc(); }
        bool isCompacted() const noexcept override { return m_AccelStruct->isCompacted(); }
        uint64_t getDeviceAddress() const noexcept override { return m_AccelStruct->getDeviceAddress(); };
        
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
        AutoPtr<IMessageCallback> m_MessageCallback;
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

        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        // ICommandList implementation

        void open() noexcept override;
        void close() noexcept override;
        void clearState() noexcept override;

        void clearTextureFloat(ITexture* t, const TextureSubresourceSet& subresources, const Color& clearColor) noexcept override;
        void clearDepthStencilTexture(ITexture* t, const TextureSubresourceSet& subresources, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) noexcept override;
        void clearTextureUInt(ITexture* t, const TextureSubresourceSet& subresources, uint32_t clearColor) noexcept override;

        void copyTexture1(ITexture* dest, const TextureSlice& destSlice, ITexture* src, const TextureSlice& srcSlice) noexcept override;
        void copyTexture2(IStagingTexture* dest, const TextureSlice& destSlice, ITexture* src, const TextureSlice& srcSlice) noexcept override;
        void copyTexture3(ITexture* dest, const TextureSlice& destSlice, IStagingTexture* src, const TextureSlice& srcSlice) noexcept override;
        void writeTexture(ITexture* dest, uint32_t arraySlice, uint32_t mipLevel, const void* data, size_t rowPitch, size_t depthPitch) noexcept override;
        void resolveTexture(ITexture* dest, const TextureSubresourceSet& dstSubresources, ITexture* src, const TextureSubresourceSet& srcSubresources) noexcept override;

        void writeBuffer(IBuffer* b, const void* data, size_t dataSize, uint64_t destOffsetBytes) noexcept override;
        void clearBufferUInt(IBuffer* b, uint32_t clearValue) noexcept override;
        void copyBuffer(IBuffer* dest, uint64_t destOffsetBytes, IBuffer* src, uint64_t srcOffsetBytes, uint64_t dataSizeBytes) noexcept override;

        void clearSamplerFeedbackTexture(ISamplerFeedbackTexture* texture) noexcept override;
        void decodeSamplerFeedbackTexture(IBuffer* buffer, ISamplerFeedbackTexture* texture, nvrhi::Format format) noexcept override;
        void setSamplerFeedbackTextureState(ISamplerFeedbackTexture* texture, ResourceStates stateBits) noexcept override;

        void setPushConstants(const void* data, size_t byteSize) noexcept override;

        void setGraphicsState(const GraphicsState& state) noexcept override;
        void draw(const DrawArguments& args) noexcept override;
        void drawIndexed(const DrawArguments& args) noexcept override;
        void drawIndirect(uint32_t offsetBytes, uint32_t drawCount) noexcept override;
        void drawIndexedIndirect(uint32_t offsetBytes, uint32_t drawCount) noexcept override;
        void drawIndexedIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount) noexcept override;

        void setComputeState(const ComputeState& state) noexcept override;
        void dispatch(uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) noexcept override;
        void dispatchIndirect(uint32_t offsetBytes)  noexcept override;

        void setMeshletState(const MeshletState& state) noexcept override;
        void dispatchMesh(uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) noexcept override;
        void dispatchMeshIndirect(uint32_t offsetBytes, uint32_t maxDrawCount) noexcept override;
        void dispatchMeshIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount) noexcept override;

        void setRayTracingState(const rt::State& state) noexcept override;
        void dispatchRays(const rt::DispatchRaysArguments& args) noexcept override;

        void buildOpacityMicromap(rt::IOpacityMicromap* omm, const rt::OpacityMicromapDesc& desc) noexcept override;
        void buildBottomLevelAccelStruct(rt::IAccelStruct* as, const rt::GeometryDesc* pGeometries, size_t numGeometries, rt::AccelStructBuildFlags buildFlags) noexcept override;
        void compactBottomLevelAccelStructs() noexcept override;
        void copyRaytracingAccelerationStructure(rt::IAccelStruct* destination, rt::IAccelStruct* source) noexcept override;
        void buildTopLevelAccelStruct(rt::IAccelStruct* as, const rt::InstanceDesc* pInstances, size_t numInstances, rt::AccelStructBuildFlags buildFlags) noexcept override;
        void buildTopLevelAccelStructFromBuffer(rt::IAccelStruct* as, nvrhi::IBuffer* instanceBuffer, uint64_t instanceBufferOffset, size_t numInstances,
            rt::AccelStructBuildFlags buildFlags = rt::AccelStructBuildFlags::None) noexcept override;
        void executeMultiIndirectClusterOperation(const rt::cluster::OperationDesc& desc) noexcept override;

        void convertCoopVecMatrices(coopvec::ConvertMatrixLayoutDesc const* convertDescs, size_t numDescs) noexcept override;

        void beginTimerQuery(ITimerQuery* query) noexcept override;
        void endTimerQuery(ITimerQuery* query) noexcept override;

        void beginMarker(const char* name) noexcept override;
        void endMarker() noexcept override;

        void setEnableAutomaticBarriers(bool enable) noexcept override;
        void setResourceStatesForBindingSet(IBindingSet* bindingSet) noexcept override;

        void setEnableUavBarriersForTexture(ITexture* texture, bool enableBarriers) noexcept override;
        void setEnableUavBarriersForBuffer(IBuffer* buffer, bool enableBarriers) noexcept override;

        void beginTrackingTextureState(ITexture* texture, const TextureSubresourceSet& subresources, ResourceStates stateBits) noexcept override;
        void beginTrackingBufferState(IBuffer* buffer, ResourceStates stateBits) noexcept override;

        void setTextureState(ITexture* texture, const TextureSubresourceSet& subresources, ResourceStates stateBits) noexcept override;
        void setBufferState(IBuffer* buffer, ResourceStates stateBits) noexcept override;
        void setAccelStructState(rt::IAccelStruct* as, ResourceStates stateBits) noexcept override;

        void setPermanentTextureState(ITexture* texture, ResourceStates stateBits) noexcept override;
        void setPermanentBufferState(IBuffer* buffer, ResourceStates stateBits) noexcept override;

        void commitBarriers() noexcept override;
        
        ResourceStates getTextureSubresourceState(ITexture* texture, ArraySlice arraySlice, MipLevel mipLevel) noexcept override;
        ResourceStates getBufferState(IBuffer* buffer) noexcept override;

        IDevice* getDevice() noexcept override;
        const CommandListParameters& getDesc() noexcept override;
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
        AutoPtr<IMessageCallback> m_MessageCallback;
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

        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        // IDevice implementation

        FRESULT createHeap(const HeapDesc& d, IHeap** ppHeap) noexcept override;

        FRESULT createTexture(const TextureDesc& d, ITexture** ppTexture) noexcept override;
        MemoryRequirements getTextureMemoryRequirements(ITexture* texture);
        MemoryRequirements& getTextureMemoryRequirements(MemoryRequirements& retVal, ITexture* texture) noexcept override { retVal = getTextureMemoryRequirements(texture); return retVal; }
        bool bindTextureMemory(ITexture* texture, IHeap* heap, uint64_t offset) noexcept override;

        FRESULT createHandleForNativeTexture(ObjectType objectType, NativeObject texture, const TextureDesc& desc, ITexture** ppTexture) noexcept override;

        FRESULT createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess, IStagingTexture** ppStagingTexture) noexcept override;
        void *mapStagingTexture(IStagingTexture* tex, const TextureSlice& slice, CpuAccessMode cpuAccess, size_t& outRowPitch) noexcept override;
        void unmapStagingTexture(IStagingTexture* tex) noexcept override;

        void getTextureTiling(ITexture* texture, uint32_t* numTiles, PackedMipDesc* desc, TileShape* tileShape, uint32_t* subresourceTilingsNum, SubresourceTiling* subresourceTilings) noexcept override;
        void updateTextureTileMappings(ITexture* texture, const TextureTilesMapping* tileMappings, uint32_t numTileMappings, CommandQueue executionQueue = CommandQueue::Graphics) noexcept override;

        FRESULT createSamplerFeedbackTexture(ITexture* pairedTexture, const SamplerFeedbackTextureDesc& desc, ISamplerFeedbackTexture** ppTexture) noexcept override;
        FRESULT createSamplerFeedbackForNativeTexture(ObjectType objectType, NativeObject texture, ITexture* pairedTexture, ISamplerFeedbackTexture** ppTexture) noexcept override;

        FRESULT createBuffer(const BufferDesc& d, IBuffer** ppBuffer) noexcept override;
        void *mapBuffer(IBuffer* b, CpuAccessMode mapFlags) noexcept override;
        void unmapBuffer(IBuffer* b) noexcept override;
        MemoryRequirements getBufferMemoryRequirements(IBuffer* buffer);
        MemoryRequirements& getBufferMemoryRequirements(MemoryRequirements& retVal, IBuffer* buffer) noexcept override { retVal = getBufferMemoryRequirements(buffer); return retVal; }
        bool queryTopLevelAccelStructPrebuildInfo(const rt::AccelStructDesc& desc,
            uint32_t instanceCount, rt::AccelStructPrebuildInfo& outInfo) noexcept override;
        bool bindBufferMemory(IBuffer* buffer, IHeap* heap, uint64_t offset) noexcept override;

        FRESULT createHandleForNativeBuffer(ObjectType objectType, NativeObject buffer, const BufferDesc& desc, IBuffer** ppBuffer) noexcept override;

        FRESULT createShader(const ShaderDesc& d, const void* binary, size_t binarySize, IShader** ppShader) noexcept override;
        FRESULT createShaderSpecialization(IShader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants, IShader** ppShader) noexcept override;
        FRESULT createShaderLibrary(const void* binary, size_t binarySize, IShaderLibrary** ppShaderLibrary) noexcept override;

        FRESULT createSampler(const SamplerDesc& d, ISampler** ppSampler) noexcept override;

        FRESULT createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, IShader* vertexShader, IInputLayout** ppInputLayout) noexcept override;

        // event queries
        FRESULT createEventQuery(IEventQuery** ppQuery) noexcept override;
        void setEventQuery(IEventQuery* query, CommandQueue queue) noexcept override;
        bool pollEventQuery(IEventQuery* query) noexcept override;
        void waitEventQuery(IEventQuery* query) noexcept override;
        void resetEventQuery(IEventQuery* query) noexcept override;

        // timer queries
        FRESULT createTimerQuery(ITimerQuery** ppQuery) noexcept override;
        bool pollTimerQuery(ITimerQuery* query) noexcept override;
        float getTimerQueryTime(ITimerQuery* query) noexcept override;
        void resetTimerQuery(ITimerQuery* query) noexcept override;

        GraphicsAPI getGraphicsAPI() noexcept override;

        FRESULT createFramebuffer(const FramebufferDesc& desc, IFramebuffer** ppFramebuffer) noexcept override;

        FRESULT createGraphicsPipeline1(const GraphicsPipelineDesc& desc, FramebufferInfo const& fbinfo, IGraphicsPipeline** ppPipeline) noexcept override;

        FRESULT createGraphicsPipeline2(const GraphicsPipelineDesc& desc, IFramebuffer* fb, IGraphicsPipeline** ppPipeline) noexcept override;

        FRESULT createComputePipeline(const ComputePipelineDesc& desc, IComputePipeline** ppPipeline) noexcept override;

        FRESULT createMeshletPipeline1(const MeshletPipelineDesc& desc, FramebufferInfo const& fbinfo, IMeshletPipeline** ppPipeline) noexcept override;

        FRESULT createMeshletPipeline2(const MeshletPipelineDesc& desc, IFramebuffer* fb, IMeshletPipeline** ppPipeline) noexcept override;

        FRESULT createRayTracingPipeline(const rt::PipelineDesc& desc, rt::IPipeline** ppPipeline) noexcept override;

        FRESULT createBindingLayout(const BindingLayoutDesc& desc, IBindingLayout** ppLayout) noexcept override;
        FRESULT createBindlessLayout(const BindlessLayoutDesc& desc, IBindingLayout** ppLayout) noexcept override;

        FRESULT createBindingSet(const BindingSetDesc& desc, IBindingLayout* layout, IBindingSet** ppBindingSet) noexcept override;
        FRESULT createDescriptorTable(IBindingLayout* layout, IDescriptorTable** ppDescriptorTable) noexcept override;

        void resizeDescriptorTable(IDescriptorTable* descriptorTable, uint32_t newSize, bool keepContents = true) noexcept override;
        bool writeDescriptorTable(IDescriptorTable* descriptorTable, const BindingSetItem& item) noexcept override;

        FRESULT createOpacityMicromap(const rt::OpacityMicromapDesc& desc, rt::IOpacityMicromap** ppOpacityMicromap) noexcept override;
        FRESULT createAccelStruct(const rt::AccelStructDesc& desc, rt::IAccelStruct** ppAccelStruct) noexcept override;
        MemoryRequirements getAccelStructMemoryRequirements(rt::IAccelStruct* as);
        MemoryRequirements& getAccelStructMemoryRequirements(MemoryRequirements& retVal, rt::IAccelStruct* as) noexcept override { retVal = getAccelStructMemoryRequirements(as); return retVal; }
        rt::cluster::OperationSizeInfo getClusterOperationSizeInfo(const rt::cluster::OperationParams& params);
        rt::cluster::OperationSizeInfo& getClusterOperationSizeInfo(rt::cluster::OperationSizeInfo& retVal, const rt::cluster::OperationParams& params) noexcept override { retVal = getClusterOperationSizeInfo(params); return retVal; }
        bool bindAccelStructMemory(rt::IAccelStruct* as, IHeap* heap, uint64_t offset) noexcept override;

        FRESULT createCommandList(const CommandListParameters& params, nvrhi::ICommandList** ppCommandList) noexcept override;
        uint64_t executeCommandLists(ICommandList* const* pCommandLists, size_t numCommandLists, CommandQueue executionQueue = CommandQueue::Graphics) noexcept override;
        void queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue, uint64_t instance) noexcept override;
        bool waitForIdle() noexcept override;
        FRESULT createCommandListLifetimeTracker(CommandQueue executionQueue, ICommandListLifetimeTracker** ppTracker) noexcept override;
        void runGarbageCollection() noexcept override;
        bool queryFeatureSupport(Feature feature, void* pInfo = nullptr, size_t infoSize = 0) noexcept override;
        FormatSupport queryFormatSupport(Format format) noexcept override;
        coopvec::DeviceFeatures queryCoopVecFeatures();
        coopvec::DeviceFeatures& queryCoopVecFeatures(coopvec::DeviceFeatures& retVal) noexcept override { retVal = queryCoopVecFeatures(); return retVal; }
        coopvec::MatMulFormatSupport queryCoopVecMatMulFormatSupport(const coopvec::MatMulFormatCombo& combination);
        coopvec::MatMulFormatSupport& queryCoopVecMatMulFormatSupport(coopvec::MatMulFormatSupport& retVal, const coopvec::MatMulFormatCombo& combination) noexcept override { retVal = queryCoopVecMatMulFormatSupport(combination); return retVal; }
        coopvec::TrainingFormatSupport queryCoopVecTrainingFormatSupport(coopvec::DataType componentType);
        coopvec::TrainingFormatSupport& queryCoopVecTrainingFormatSupport(coopvec::TrainingFormatSupport& retVal, coopvec::DataType componentType) noexcept override { retVal = queryCoopVecTrainingFormatSupport(componentType); return retVal; }
        size_t getCoopVecMatrixSize(coopvec::DataType type, coopvec::MatrixLayout layout, int rows, int columns) noexcept override;
        NativeObject getNativeQueue(ObjectType objectType, CommandQueue queue) noexcept override;
        IMessageCallback* getMessageCallback() noexcept override;
        bool isAftermathEnabled() noexcept override;
        IAftermathCrashDumpHelper* getAftermathCrashDumpHelper() noexcept override;
    };

} // namespace nvrhi::validation
