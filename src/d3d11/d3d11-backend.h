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

#include <nvrhi/d3d11.h>
#include "../common/resourcebindingmap.h"
#include "../common/utils-internal.h"
#include "../common/dxgi-format.h"

#include <d3d11_1.h>
#include <map>
#include <vector>

#ifndef NVRHI_D3D11_WITH_NVAPI
#define NVRHI_D3D11_WITH_NVAPI 0
#endif

#if NVRHI_D3D11_WITH_NVAPI
#include <nvapi.h>
#include <nvShaderExtnEnums.h>
#endif

#include "../common/aftermath.h"
#if NVRHI_WITH_AFTERMATH
#include <GFSDK_Aftermath.h>
#endif

namespace nvrhi::d3d11
{
    void SetDebugName(ID3D11DeviceChild* pObject, const char* name);

    D3D11_BLEND convertBlendValue(BlendFactor value);
    D3D11_BLEND_OP convertBlendOp(BlendOp value);
    D3D11_STENCIL_OP convertStencilOp(StencilOp value);
    D3D11_COMPARISON_FUNC convertComparisonFunc(ComparisonFunc value);
    D3D_PRIMITIVE_TOPOLOGY convertPrimType(PrimitiveType pt, uint32_t controlPoints);
    D3D11_TEXTURE_ADDRESS_MODE convertSamplerAddressMode(SamplerAddressMode mode);
    UINT convertSamplerReductionType(SamplerReductionType reductionType);

    struct Context
    {
        AutoPtr<ID3D11Device> device;
        AutoPtr<ID3D11DeviceContext> immediateContext;
        AutoPtr<ID3D11DeviceContext1> immediateContext1;
        AutoPtr<ID3D11Buffer> pushConstantBuffer;
        AutoPtr<IMessageCallback> messageCallback;
        bool nvapiAvailable = false;
#if NVRHI_WITH_AFTERMATH
        GFSDK_Aftermath_ContextHandle aftermathContext = nullptr;
#endif

        void error(const std::string& message) const;
    };

    NVRHI_CLASS_CLSID(Texture, "0282c219-19ee-4adc-82cd-acd426854be1")
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
        AutoPtr<ID3D11Resource> resource;
        HANDLE sharedHandle = nullptr;

        Texture(const Context& context) : m_Context(context) { }
        const TextureDesc& getDesc() const noexcept override { return desc; }
        bool queryMemoryRequirements(MemoryRequirements&) noexcept override { utils::NotSupported(); return false; }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;
        NativeObject getNativeView(ObjectType objectType, Format format, const TextureSubresourceSet& subresources, TextureDimension dimension, bool isReadOnlyDSV = false,
            _In_opt_ const ComponentMapping* overrideComponentMapping = nullptr) noexcept override;

        ID3D11ShaderResourceView* getSRV(Format format, TextureSubresourceSet subresources, TextureDimension dimension);
        ID3D11RenderTargetView* getRTV(Format format, TextureSubresourceSet subresources);
        ID3D11DepthStencilView* getDSV(TextureSubresourceSet subresources, bool isReadOnly = false);
        ID3D11UnorderedAccessView* getUAV(Format format, TextureSubresourceSet subresources, TextureDimension dimension);

    private:
        const Context& m_Context;
        TextureBindingKey_HashMap<AutoPtr<ID3D11ShaderResourceView>> m_ShaderResourceViews;
        TextureBindingKey_HashMap<AutoPtr<ID3D11RenderTargetView>> m_RenderTargetViews;
        TextureBindingKey_HashMap<AutoPtr<ID3D11DepthStencilView>> m_DepthStencilViews;
        TextureBindingKey_HashMap<AutoPtr<ID3D11UnorderedAccessView>> m_UnorderedAccessViews;
    };

    NVRHI_CLASS_CLSID(StagingTexture, "ffb8847e-569b-4b5c-b918-5e7c44c50433")
    class StagingTexture : public ObjectImpl<IStagingTexture>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(StagingTexture)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(StagingTexture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IStagingTexture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(StagingTexture)
        NVRHI_END_INTERFACE_TABLE()

        AutoPtr<Texture> texture;
        CpuAccessMode cpuAccess = CpuAccessMode::None;
        UINT mappedSubresource = UINT(-1);
        
        const TextureDesc& getDesc() const noexcept override { return texture->getDesc(); }
    };

    NVRHI_CLASS_CLSID(Buffer, "6a3cdf58-6bc7-4f79-9865-9b5e8659021c")
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
        AutoPtr<ID3D11Buffer> resource;
        HANDLE sharedHandle = nullptr;
        
        Buffer(const Context& context) : m_Context(context) { }
        const BufferDesc& getDesc() const noexcept override { return desc; }
        bool queryMemoryRequirements(MemoryRequirements&) noexcept override { utils::NotSupported(); return false; }
        GpuVirtualAddress getGpuVirtualAddress() const noexcept override { nvrhi::utils::NotImplemented(); return 0; }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        ID3D11ShaderResourceView* getSRV(Format format, BufferRange range, ResourceType type);
        ID3D11UnorderedAccessView* getUAV(Format format, BufferRange range, ResourceType type);
        
    private:
        const Context& m_Context;
        std::unordered_map<BufferBindingKey, AutoPtr<ID3D11ShaderResourceView>> m_ShaderResourceViews;
        std::unordered_map<BufferBindingKey, AutoPtr<ID3D11UnorderedAccessView>> m_UnorderedAccessViews;
    };

    NVRHI_CLASS_CLSID(Shader, "f21e0423-fda2-49e8-9ff7-b8f2e6c80b2e")
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
        AutoPtr<ID3D11VertexShader> VS;
        AutoPtr<ID3D11HullShader> HS;
        AutoPtr<ID3D11DomainShader> DS;
        AutoPtr<ID3D11GeometryShader> GS;
        AutoPtr<ID3D11PixelShader> PS;
        AutoPtr<ID3D11ComputeShader> CS;
        std::vector<char> bytecode;
        
        const ShaderDesc& getDesc() const noexcept override { return desc; }

        void getBytecode(const void** ppBytecode, size_t* pSize) const noexcept override;
    };

    NVRHI_CLASS_CLSID(Sampler, "796446d2-798e-45b7-947e-420c7ca04fd1")
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
        AutoPtr<ID3D11SamplerState> sampler;
        
        const SamplerDesc& getDesc() const noexcept override { return desc; }
    };

    NVRHI_CLASS_CLSID(EventQuery, "67e30c28-c4e3-4789-af22-999c7010d120")
    class EventQuery : public ObjectImpl<IEventQuery>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(EventQuery)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(EventQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IEventQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(EventQuery)
        NVRHI_END_INTERFACE_TABLE()

        AutoPtr<ID3D11Query> query;
        bool resolved = false;
    };

    NVRHI_CLASS_CLSID(TimerQuery, "cdfb5d8c-6c78-4927-948f-f481e930caf1")
    class TimerQuery : public ObjectImpl<ITimerQuery>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(TimerQuery)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(TimerQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ITimerQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(TimerQuery)
        NVRHI_END_INTERFACE_TABLE()

        AutoPtr<ID3D11Query> start;
        AutoPtr<ID3D11Query> end;
        AutoPtr<ID3D11Query> disjoint;

        bool resolved = false;
        float time = 0.f;
    };
    
    NVRHI_CLASS_CLSID(InputLayout, "0ae668e0-1886-43b3-b14c-b70a1f714d21")
    class InputLayout : public ObjectImpl<IInputLayout>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(InputLayout)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(InputLayout)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IInputLayout)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(InputLayout)
        NVRHI_END_INTERFACE_TABLE()

        AutoPtr<ID3D11InputLayout> layout;
        std::vector<VertexAttributeDesc> attributes;
        // maps a binding slot number to a stride
        std::unordered_map<uint32_t, uint32_t> elementStrides;

        uint32_t getNumAttributes() const noexcept override { return uint32_t(attributes.size()); }
        const VertexAttributeDesc* getAttributeDesc(uint32_t index) const noexcept override;
    };


    NVRHI_CLASS_CLSID(Framebuffer, "a0b3f4d7-d2e0-447f-9bac-2316d7002f5e")
    class Framebuffer : public ObjectImpl<IFramebuffer>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Framebuffer)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Framebuffer)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IFramebuffer)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Framebuffer)
        NVRHI_END_INTERFACE_TABLE()

        FramebufferDesc desc;
        FramebufferInfoEx framebufferInfo;
        static_vector<AutoPtr<ID3D11RenderTargetView>, c_MaxRenderTargets> RTVs;
        AutoPtr<ID3D11DepthStencilView> DSV;
        
        const FramebufferDesc& getDesc() const noexcept override { return desc; }
        const FramebufferInfoEx& getFramebufferInfo() const noexcept override { return framebufferInfo; }
    };

    struct DX11_ViewportState
    {
        uint32_t numViewports = 0;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_MAX_INDEX] = {};
        uint32_t numScissorRects = 0;
        D3D11_RECT scissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_MAX_INDEX] = {};
    };

    NVRHI_CLASS_CLSID(GraphicsPipeline, "1b0ab4f8-8d83-4f9e-be60-2f3651aaf2d2")
    class GraphicsPipeline : public ObjectImpl<IGraphicsPipeline>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(GraphicsPipeline)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GraphicsPipeline)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IGraphicsPipeline)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(GraphicsPipeline)
        NVRHI_END_INTERFACE_TABLE()

        GraphicsPipelineDesc desc;
        ShaderType shaderMask = ShaderType::None;
        FramebufferInfo framebufferInfo;

        D3D11_PRIMITIVE_TOPOLOGY primitiveTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        InputLayout *inputLayout = nullptr;

        ID3D11RasterizerState *pRS = nullptr;

        ID3D11BlendState *pBlendState = nullptr;
        ID3D11DepthStencilState *pDepthStencilState = nullptr;
        bool requiresBlendFactor = false;
        bool pixelShaderHasUAVs = false;

        AutoPtr<ID3D11VertexShader> pVS;
        AutoPtr<ID3D11HullShader> pHS;
        AutoPtr<ID3D11DomainShader> pDS;
        AutoPtr<ID3D11GeometryShader> pGS;
        AutoPtr<ID3D11PixelShader> pPS;
        
        const GraphicsPipelineDesc& getDesc() const noexcept override { return desc; }
        const FramebufferInfo& getFramebufferInfo() const noexcept override { return framebufferInfo; }
    };

    NVRHI_CLASS_CLSID(ComputePipeline, "25d76448-3ffe-4361-811f-e0490defd50b")
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

        AutoPtr<ID3D11ComputeShader> shader;
        
        const ComputePipelineDesc& getDesc() const noexcept override { return desc; }
    };

    NVRHI_CLASS_CLSID(BindingLayout, "8aa2b216-3465-48e1-a3d8-b8e8e275e812")
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

    NVRHI_CLASS_CLSID(BindingSet, "fa444fcd-90a5-41c7-948c-201afa6ccb92")
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
        ShaderType visibility = ShaderType::None;

        ID3D11ShaderResourceView* SRVs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        uint32_t minSRVSlot = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
        uint32_t maxSRVSlot = 0;

        ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
        uint32_t minSamplerSlot = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
        uint32_t maxSamplerSlot = 0;

        ID3D11Buffer* constantBuffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
        UINT constantBufferOffsets[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
        UINT constantBufferCounts[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
        uint32_t minConstantBufferSlot = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
        uint32_t maxConstantBufferSlot = 0;

        ID3D11UnorderedAccessView* UAVs[D3D11_1_UAV_SLOT_COUNT] = {};
        uint32_t minUAVSlot = D3D11_1_UAV_SLOT_COUNT;
        uint32_t maxUAVSlot = 0;

        std::vector<AutoPtr<IRHIObject>> resources;
        
        const BindingSetDesc* getDesc() const noexcept override { return &desc; }
        IBindingLayout* getLayout() const noexcept override { return layout; }
        bool isSupersetOf(const BindingSet& other) const;
    };

    NVRHI_CLASS_CLSID(CommandList, "8be06890-1fd9-4c8f-9f2e-df9b3e26360b")
    class CommandList : public ObjectImpl<ICommandList>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(CommandList)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(CommandList)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ICommandList)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(CommandList)
        NVRHI_END_INTERFACE_TABLE()

        explicit CommandList(const Context& context, IDevice* device, const CommandListParameters& params);
        ~CommandList();

        // IRHIObject implementation

        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        // ICommandList implementation

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

        void writeBuffer(IBuffer* b, const void* data, size_t dataSize, uint64_t destOffsetBytes = 0) noexcept override;
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

        // perf markers
        void beginMarker(const char* name) noexcept override;
        void endMarker() noexcept override;

        void setEnableAutomaticBarriers(bool enable) noexcept override { (void)enable; }
        void setResourceStatesForBindingSet(IBindingSet* bindingSet) noexcept override { (void)bindingSet; }

        void setEnableUavBarriersForTexture(ITexture* texture, bool enableBarriers) noexcept override;
        void setEnableUavBarriersForBuffer(IBuffer* buffer, bool enableBarriers) noexcept override;

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

        IDevice* getDevice() noexcept override { return m_Device; }
        const CommandListParameters& getDesc() noexcept override { return m_Desc; }

    private:
        const Context& m_Context;
        IDevice* m_Device; // weak reference - to avoid a cyclic reference between Device and its ImmediateCommandList
        CommandListParameters m_Desc;

        AutoPtr<ID3DUserDefinedAnnotation> m_UserDefinedAnnotation;
#if NVRHI_WITH_AFTERMATH
        AftermathMarkerTracker m_AftermathTracker;
#endif

        int m_NumUAVOverlapCommands = 0;
        void enterUAVOverlapSection();
        void leaveUAVOverlapSection();

        // State cache.
        // Use strong references (handles) instead of just a copy of GraphicsState etc.
        // If user code creates some object, draws using it, and releases it, a weak pointer would become invalid.
        // Using strong references in all state objects would solve this problem, but it means there will be an extra AddRef/Release cost everywhere.

        GraphicsPipelineHandle m_CurrentGraphicsPipeline;
        FramebufferHandle m_CurrentFramebuffer;
        ViewportState m_CurrentViewports{};
        static_vector<BindingSetHandle, c_MaxBindingLayouts> m_CurrentBindings;
        static_vector<VertexBufferBinding, c_MaxVertexAttributes> m_CurrentVertexBufferBindings;
        IndexBufferBinding m_CurrentIndexBufferBinding{};
        static_vector<BufferHandle, c_MaxVertexAttributes> m_CurrentVertexBuffers;
        BufferHandle m_CurrentIndexBuffer;
        ComputePipelineHandle m_CurrentComputePipeline;
        SinglePassStereoState m_CurrentSinglePassStereoState{};
        BufferHandle m_CurrentIndirectBuffer;
        Color m_CurrentBlendConstantColor{};
        uint8_t m_CurrentStencilRefValue = 0;
        bool m_CurrentGraphicsStateValid = false;
        bool m_CurrentComputeStateValid = false;

        void copyTexture(ID3D11Resource* dst, const TextureDesc& dstDesc, const TextureSlice& dstSlice,
            ID3D11Resource* src, const TextureDesc& srcDesc, const TextureSlice& srcSlice);
        
        void bindGraphicsPipeline(const GraphicsPipeline* pso) const;

        void prepareToBindGraphicsResourceSets(
            const BindingSetVector& resourceSets,
            const static_vector<BindingSetHandle, c_MaxBindingLayouts>* currentResourceSets,
            const IGraphicsPipeline* currentPipeline,
            const IGraphicsPipeline* newPipeline,
            bool updateFramebuffer,
            BindingSetVector& outSetsToBind) const;
        void bindGraphicsResourceSets(const BindingSetVector& setsToBind, const IGraphicsPipeline* newPipeline) const;
        void bindComputeResourceSets(const BindingSetVector& resourceSets, const static_vector<BindingSetHandle, c_MaxBindingLayouts>* currentResourceSets) const;
    };

    NVRHI_CLASS_CLSID(Device, "b7ce5dbe-5c3f-4caf-8688-33b2818b0944")
    class Device : public ObjectImpl<IDevice>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Device)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Device)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IDevice)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Device)
        NVRHI_END_INTERFACE_TABLE()

        explicit Device(const DeviceDesc& desc);
        ~Device();

        // IRHIObject implementation

        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        // IDevice implementation
        // A method that returns an object is a non-virtual helper that returns a handle (nullptr on failure),
        // which the backend also calls itself, followed by the noexcept IDevice override that returns the object
        // through the COM protocol (utils::ReturnObject). Struct results work the same way, through retVal.


        HeapHandle createHeap(const HeapDesc& d);
        FRESULT createHeap(const HeapDesc& d, IHeap** ppHeap) noexcept override { return utils::ReturnObject(ppHeap, [&] { return createHeap(d); }, FE_UNSUPPORTED); }

        TextureHandle createTexture(const TextureDesc& d);
        FRESULT createTexture(const TextureDesc& d, ITexture** ppTexture) noexcept override { return utils::ReturnObject(ppTexture, [&] { return createTexture(d); }); }
        MemoryRequirements getTextureMemoryRequirements(ITexture* texture);
        MemoryRequirements& getTextureMemoryRequirements(MemoryRequirements& retVal, ITexture* texture) noexcept override { retVal = getTextureMemoryRequirements(texture); return retVal; }
        bool bindTextureMemory(ITexture* texture, IHeap* heap, uint64_t offset) noexcept override;

        TextureHandle createHandleForNativeTexture(ObjectType objectType, NativeObject texture, const TextureDesc& desc);
        FRESULT createHandleForNativeTexture(ObjectType objectType, NativeObject texture, const TextureDesc& desc, ITexture** ppTexture) noexcept override { return utils::ReturnObject(ppTexture, [&] { return createHandleForNativeTexture(objectType, texture, desc); }); }

        StagingTextureHandle createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess);
        FRESULT createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess, IStagingTexture** ppStagingTexture) noexcept override { return utils::ReturnObject(ppStagingTexture, [&] { return createStagingTexture(d, cpuAccess); }); }
        void *mapStagingTexture(IStagingTexture* tex, const TextureSlice& slice, CpuAccessMode cpuAccess, size_t& outRowPitch) noexcept override;
        void unmapStagingTexture(IStagingTexture* tex) noexcept override;

        void getTextureTiling(ITexture* texture, uint32_t* numTiles, PackedMipDesc* desc, TileShape* tileShape, uint32_t* subresourceTilingsNum, SubresourceTiling* subresourceTilings) noexcept override;
        void updateTextureTileMappings(ITexture* texture, const TextureTilesMapping* tileMappings, uint32_t numTileMappings, CommandQueue executionQueue = CommandQueue::Graphics) noexcept override;

        SamplerFeedbackTextureHandle createSamplerFeedbackTexture(ITexture* pairedTexture, const SamplerFeedbackTextureDesc& desc);
        FRESULT createSamplerFeedbackTexture(ITexture* pairedTexture, const SamplerFeedbackTextureDesc& desc, ISamplerFeedbackTexture** ppTexture) noexcept override { return utils::ReturnObject(ppTexture, [&] { return createSamplerFeedbackTexture(pairedTexture, desc); }, FE_UNSUPPORTED); }
        SamplerFeedbackTextureHandle createSamplerFeedbackForNativeTexture(ObjectType objectType, NativeObject texture, ITexture* pairedTexture);
        FRESULT createSamplerFeedbackForNativeTexture(ObjectType objectType, NativeObject texture, ITexture* pairedTexture, ISamplerFeedbackTexture** ppTexture) noexcept override { return utils::ReturnObject(ppTexture, [&] { return createSamplerFeedbackForNativeTexture(objectType, texture, pairedTexture); }, FE_UNSUPPORTED); }

        BufferHandle createBuffer(const BufferDesc& d);
        FRESULT createBuffer(const BufferDesc& d, IBuffer** ppBuffer) noexcept override { return utils::ReturnObject(ppBuffer, [&] { return createBuffer(d); }); }
        void *mapBuffer(IBuffer* b, CpuAccessMode mapFlags) noexcept override;
        void unmapBuffer(IBuffer* b) noexcept override;
        MemoryRequirements getBufferMemoryRequirements(IBuffer* buffer);
        MemoryRequirements& getBufferMemoryRequirements(MemoryRequirements& retVal, IBuffer* buffer) noexcept override { retVal = getBufferMemoryRequirements(buffer); return retVal; }
        bool queryTopLevelAccelStructPrebuildInfo(const rt::AccelStructDesc& desc,
            uint32_t instanceCount, rt::AccelStructPrebuildInfo& outInfo) noexcept override;
        bool bindBufferMemory(IBuffer* buffer, IHeap* heap, uint64_t offset) noexcept override;

        BufferHandle createHandleForNativeBuffer(ObjectType objectType, NativeObject buffer, const BufferDesc& desc);
        FRESULT createHandleForNativeBuffer(ObjectType objectType, NativeObject buffer, const BufferDesc& desc, IBuffer** ppBuffer) noexcept override { return utils::ReturnObject(ppBuffer, [&] { return createHandleForNativeBuffer(objectType, buffer, desc); }); }

        ShaderHandle createShader(const ShaderDesc& d, const void* binary, const size_t binarySize);
        FRESULT createShader(const ShaderDesc& d, const void* binary, const size_t binarySize, IShader** ppShader) noexcept override { return utils::ReturnObject(ppShader, [&] { return createShader(d, binary, binarySize); }); }
        ShaderHandle createShaderSpecialization(IShader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants);
        FRESULT createShaderSpecialization(IShader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants, IShader** ppShader) noexcept override { return utils::ReturnObject(ppShader, [&] { return createShaderSpecialization(baseShader, constants, numConstants); }); }
        ShaderLibraryHandle createShaderLibrary(const void* binary, const size_t binarySize) { (void)binary; (void)binarySize; return nullptr; }
        FRESULT createShaderLibrary(const void* binary, const size_t binarySize, IShaderLibrary** ppShaderLibrary) noexcept override { return utils::ReturnObject(ppShaderLibrary, [&] { return createShaderLibrary(binary, binarySize); }, FE_UNSUPPORTED); }

        SamplerHandle createSampler(const SamplerDesc& d);
        FRESULT createSampler(const SamplerDesc& d, ISampler** ppSampler) noexcept override { return utils::ReturnObject(ppSampler, [&] { return createSampler(d); }); }

        InputLayoutHandle createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, IShader* vertexShader);
        FRESULT createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, IShader* vertexShader, IInputLayout** ppInputLayout) noexcept override { return utils::ReturnObject(ppInputLayout, [&] { return createInputLayout(d, attributeCount, vertexShader); }); }

        // event queries
        EventQueryHandle createEventQuery(void);
        FRESULT createEventQuery(IEventQuery** ppQuery) noexcept override { return utils::ReturnObject(ppQuery, [&] { return createEventQuery(); }); }
        void setEventQuery(IEventQuery* query, CommandQueue queue) noexcept override;
        bool pollEventQuery(IEventQuery* query) noexcept override;
        void waitEventQuery(IEventQuery* query) noexcept override;
        void resetEventQuery(IEventQuery* query) noexcept override;

        // timer queries
        TimerQueryHandle createTimerQuery(void);
        FRESULT createTimerQuery(ITimerQuery** ppQuery) noexcept override { return utils::ReturnObject(ppQuery, [&] { return createTimerQuery(); }); }
        bool pollTimerQuery(ITimerQuery* query) noexcept override;
        float getTimerQueryTime(ITimerQuery* query) noexcept override;
        void resetTimerQuery(ITimerQuery* query) noexcept override;

        GraphicsAPI getGraphicsAPI() noexcept override;

        FramebufferHandle createFramebuffer(const FramebufferDesc& desc);
        FRESULT createFramebuffer(const FramebufferDesc& desc, IFramebuffer** ppFramebuffer) noexcept override { return utils::ReturnObject(ppFramebuffer, [&] { return createFramebuffer(desc); }); }

        GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc, FramebufferInfo const& fbinfo);
        FRESULT createGraphicsPipeline1(const GraphicsPipelineDesc& desc, FramebufferInfo const& fbinfo, IGraphicsPipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createGraphicsPipeline(desc, fbinfo); }); }

        GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc, IFramebuffer* fb);
        FRESULT createGraphicsPipeline2(const GraphicsPipelineDesc& desc, IFramebuffer* fb, IGraphicsPipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createGraphicsPipeline(desc, fb); }); }

        ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc);
        FRESULT createComputePipeline(const ComputePipelineDesc& desc, IComputePipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createComputePipeline(desc); }); }

        MeshletPipelineHandle createMeshletPipeline(const MeshletPipelineDesc& desc, FramebufferInfo const& fbinfo);
        FRESULT createMeshletPipeline1(const MeshletPipelineDesc& desc, FramebufferInfo const& fbinfo, IMeshletPipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createMeshletPipeline(desc, fbinfo); }, FE_UNSUPPORTED); }

        MeshletPipelineHandle createMeshletPipeline(const MeshletPipelineDesc& desc, IFramebuffer* fb);
        FRESULT createMeshletPipeline2(const MeshletPipelineDesc& desc, IFramebuffer* fb, IMeshletPipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createMeshletPipeline(desc, fb); }, FE_UNSUPPORTED); }

        rt::PipelineHandle createRayTracingPipeline(const rt::PipelineDesc& desc);
        FRESULT createRayTracingPipeline(const rt::PipelineDesc& desc, rt::IPipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createRayTracingPipeline(desc); }, FE_UNSUPPORTED); }

        BindingLayoutHandle createBindingLayout(const BindingLayoutDesc& desc);
        FRESULT createBindingLayout(const BindingLayoutDesc& desc, IBindingLayout** ppLayout) noexcept override { return utils::ReturnObject(ppLayout, [&] { return createBindingLayout(desc); }); }
        BindingLayoutHandle createBindlessLayout(const BindlessLayoutDesc& desc);
        FRESULT createBindlessLayout(const BindlessLayoutDesc& desc, IBindingLayout** ppLayout) noexcept override { return utils::ReturnObject(ppLayout, [&] { return createBindlessLayout(desc); }); }

        BindingSetHandle createBindingSet(const BindingSetDesc& desc, IBindingLayout* layout);
        FRESULT createBindingSet(const BindingSetDesc& desc, IBindingLayout* layout, IBindingSet** ppBindingSet) noexcept override { return utils::ReturnObject(ppBindingSet, [&] { return createBindingSet(desc, layout); }); }
        DescriptorTableHandle createDescriptorTable(IBindingLayout* layout);
        FRESULT createDescriptorTable(IBindingLayout* layout, IDescriptorTable** ppDescriptorTable) noexcept override { return utils::ReturnObject(ppDescriptorTable, [&] { return createDescriptorTable(layout); }); }

        void resizeDescriptorTable(IDescriptorTable* descriptorTable, uint32_t newSize, bool keepContents = true) noexcept override;
        bool writeDescriptorTable(IDescriptorTable* descriptorTable, const BindingSetItem& item) noexcept override;

        rt::OpacityMicromapHandle createOpacityMicromap(const rt::OpacityMicromapDesc& desc);
        FRESULT createOpacityMicromap(const rt::OpacityMicromapDesc& desc, rt::IOpacityMicromap** ppOpacityMicromap) noexcept override { return utils::ReturnObject(ppOpacityMicromap, [&] { return createOpacityMicromap(desc); }, FE_UNSUPPORTED); }
        rt::AccelStructHandle createAccelStruct(const rt::AccelStructDesc& desc);
        FRESULT createAccelStruct(const rt::AccelStructDesc& desc, rt::IAccelStruct** ppAccelStruct) noexcept override { return utils::ReturnObject(ppAccelStruct, [&] { return createAccelStruct(desc); }, FE_UNSUPPORTED); }
        MemoryRequirements getAccelStructMemoryRequirements(rt::IAccelStruct* as);
        MemoryRequirements& getAccelStructMemoryRequirements(MemoryRequirements& retVal, rt::IAccelStruct* as) noexcept override { retVal = getAccelStructMemoryRequirements(as); return retVal; }
        rt::cluster::OperationSizeInfo getClusterOperationSizeInfo(const rt::cluster::OperationParams& params);
        rt::cluster::OperationSizeInfo& getClusterOperationSizeInfo(rt::cluster::OperationSizeInfo& retVal, const rt::cluster::OperationParams& params) noexcept override { retVal = getClusterOperationSizeInfo(params); return retVal; }
        bool bindAccelStructMemory(rt::IAccelStruct* as, IHeap* heap, uint64_t offset) noexcept override;

        CommandListHandle createCommandList(const CommandListParameters& params = CommandListParameters());
        FRESULT createCommandList(const CommandListParameters& params, nvrhi::ICommandList** ppCommandList) noexcept override { return utils::ReturnObject(ppCommandList, [&] { return createCommandList(params); }); }
        uint64_t executeCommandLists(ICommandList* const* pCommandLists, size_t numCommandLists, CommandQueue executionQueue = CommandQueue::Graphics) noexcept override { (void)pCommandLists; (void)numCommandLists; (void)executionQueue; return 0; }
        void queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue, uint64_t instance) noexcept override { (void)waitQueue; (void)executionQueue; (void)instance; }
        bool waitForIdle() noexcept override;
        CommandListLifetimeTrackerHandle createCommandListLifetimeTracker(CommandQueue executionQueue);
        FRESULT createCommandListLifetimeTracker(CommandQueue executionQueue, ICommandListLifetimeTracker** ppTracker) noexcept override { return utils::ReturnObject(ppTracker, [&] { return createCommandListLifetimeTracker(executionQueue); }); }
        void runGarbageCollection() noexcept override { }
        bool queryFeatureSupport(Feature feature, void* pInfo = nullptr, size_t infoSize = 0) noexcept override;
        FormatSupport queryFormatSupport(Format format) noexcept override;
        coopvec::DeviceFeatures queryCoopVecFeatures();
        coopvec::DeviceFeatures& queryCoopVecFeatures(coopvec::DeviceFeatures& retVal) noexcept override { retVal = queryCoopVecFeatures(); return retVal; }
        coopvec::MatMulFormatSupport queryCoopVecMatMulFormatSupport(const coopvec::MatMulFormatCombo& combination);
        coopvec::MatMulFormatSupport& queryCoopVecMatMulFormatSupport(coopvec::MatMulFormatSupport& retVal, const coopvec::MatMulFormatCombo& combination) noexcept override { retVal = queryCoopVecMatMulFormatSupport(combination); return retVal; }
        coopvec::TrainingFormatSupport queryCoopVecTrainingFormatSupport(coopvec::DataType componentType);
        coopvec::TrainingFormatSupport& queryCoopVecTrainingFormatSupport(coopvec::TrainingFormatSupport& retVal, coopvec::DataType componentType) noexcept override { retVal = queryCoopVecTrainingFormatSupport(componentType); return retVal; }
        size_t getCoopVecMatrixSize(coopvec::DataType type, coopvec::MatrixLayout layout, int rows, int columns) noexcept override;
        NativeObject getNativeQueue(ObjectType objectType, CommandQueue queue) noexcept override { (void)objectType; (void)queue;  return nullptr; }
        IMessageCallback* getMessageCallback() noexcept override { return m_Context.messageCallback; }
        bool isAftermathEnabled() noexcept override { return m_AftermathEnabled; }
        IAftermathCrashDumpHelper* getAftermathCrashDumpHelper() noexcept override { return m_AftermathEnabled ? m_AftermathCrashDumpHelper.Get() : nullptr; }
        // The concrete helper, for the command lists' marker trackers.
        AftermathCrashDumpHelper& getAftermathCrashDumpHelperImpl() { return *m_AftermathCrashDumpHelper; }

    private:
        Context m_Context;
        EventQueryHandle m_WaitForIdleQuery;
        CommandListHandle m_ImmediateCommandList;

        std::unordered_map<size_t, AutoPtr<ID3D11BlendState>> m_BlendStates;
        std::unordered_map<size_t, AutoPtr<ID3D11DepthStencilState>> m_DepthStencilStates;
        std::unordered_map<size_t, AutoPtr<ID3D11RasterizerState>> m_RasterizerStates;

        bool m_SinglePassStereoSupported = false;
        bool m_HlslExtensionsSupported = false;
        bool m_FastGeometryShaderSupported = false;

        TextureHandle createTexture(const TextureDesc& d, CpuAccessMode cpuAccess) const;

        ID3D11RenderTargetView* getRTVForAttachment(const FramebufferAttachment& attachment);
        ID3D11DepthStencilView* getDSVForAttachment(const FramebufferAttachment& attachment);

        ID3D11BlendState* getBlendState(const BlendState& blendState);
        ID3D11DepthStencilState* getDepthStencilState(const DepthStencilState& depthStencilState);
        ID3D11RasterizerState* getRasterizerState(const RasterState& rasterState);

        bool m_AftermathEnabled = false;
        AutoPtr<AftermathCrashDumpHelper> m_AftermathCrashDumpHelper = MAKE_RC_OBJ_PTR(AftermathCrashDumpHelper);
    };

} // namespace nvrhi::d3d11
