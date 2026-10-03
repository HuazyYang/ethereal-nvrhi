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

#include <nvrhi/d3d12.h>

#ifndef NVRHI_D3D12_WITH_NVAPI
#define NVRHI_D3D12_WITH_NVAPI 0
#endif

#if NVRHI_D3D12_WITH_NVAPI
#include <dxgi.h>
#include <nvapi.h>
#endif

#include "../common/aftermath.h"
#if NVRHI_WITH_AFTERMATH
#include <GFSDK_Aftermath.h>
#endif

// If using the Agility SDK version of OMM, ignore the NVAPI version
#if NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP
    #define NVRHI_WITH_NVAPI_OPACITY_MICROMAP (0)
#else
    // There's no version check available in the nvapi header,
    // instead to check if the NvAPI linked is OMM compatible version (>520) we look for one of the defines it adds...
    #if NVRHI_D3D12_WITH_NVAPI && defined(NVAPI_GET_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO_PARAMS_VER)
        #define NVRHI_WITH_NVAPI_OPACITY_MICROMAP (1)
    #else
        #define NVRHI_WITH_NVAPI_OPACITY_MICROMAP (0)
    #endif
#endif

// ... same for DMM compatible versions (>=535) we look for one of the defines it adds
#if NVRHI_D3D12_WITH_NVAPI && defined(NVAPI_GET_RAYTRACING_DISPLACEMENT_MICROMAP_ARRAY_PREBUILD_INFO_PARAMS_VER)
#define NVRHI_WITH_NVAPI_DISPLACEMENT_MICROMAP (1)
#else
#define NVRHI_WITH_NVAPI_DISPLACEMENT_MICROMAP (0)
#endif

#if NVRHI_D3D12_WITH_NVAPI && defined(NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS_VER)
#define NVRHI_WITH_NVAPI_CLUSTERS (1)
#else
#define NVRHI_WITH_NVAPI_CLUSTERS (0)
#endif

// Line-Swept Spheres were added in NVAPI SDK 572.18
#if NVRHI_D3D12_WITH_NVAPI && !NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP && (NVAPI_SDK_VERSION >= 57218)
#define NVRHI_WITH_NVAPI_LSS (1)
#else
#define NVRHI_WITH_NVAPI_LSS (0)
#endif

// Preview 717 exposes cooperative-vector feature queries (D3D12_FEATURE_COOPERATIVE_VECTOR).
// Preview 720+ uses the Linear Algebra feature tier and matrix-operation queries when DIRECT3D_LINEAR_ALGEBRA is defined.
#if (D3D12_PREVIEW_SDK_VERSION == 717) || (defined(DIRECT3D_LINEAR_ALGEBRA) && D3D12_PREVIEW_SDK_VERSION >= 720)
#define NVRHI_D3D12_WITH_COOP_VECTOR_COMMON (1)
#else
#define NVRHI_D3D12_WITH_COOP_VECTOR_COMMON (0)
#endif

#if defined(DIRECT3D_LINEAR_ALGEBRA) && D3D12_PREVIEW_SDK_VERSION >= 720
#define NVRHI_D3D12_WITH_LINALG (1)
#else
#define NVRHI_D3D12_WITH_LINALG (0)
#endif

// Deprecated: downstream code should use NVRHI_D3D12_WITH_COOP_VECTOR_COMMON; alias will be removed after a deprecation window.
#define NVRHI_D3D12_WITH_COOPVEC NVRHI_D3D12_WITH_COOP_VECTOR_COMMON

#include <bitset>
#include <memory>
#include <queue>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "../common/resourcebindingmap.h"
#include "../common/utils-internal.h"
#include "../common/bitset-allocator.h"
#include "../common/state-tracking.h"
#include "../common/dxgi-format.h"
#include "../common/versioning.h"

#ifdef NVRHI_WITH_RTXMU
#include <rtxmu/D3D12AccelStructManager.h>
#endif

namespace nvrhi::d3d12
{
    class RootSignature;
    class Buffer;
    class CommandList;
    class Device;
    struct Context;

    typedef uint32_t RootParameterIndex;
    typedef uint32_t OptionalResourceState; // D3D12_RESOURCE_STATES + unknown value

    constexpr RootParameterIndex c_InvalidRootParameterIndex = ~0u; // Used to skip mutable descriptor set
    constexpr DescriptorIndex c_InvalidDescriptorIndex = ~0u;
    constexpr OptionalResourceState c_ResourceStateUnknown = ~0u;
    
    D3D12_SHADER_VISIBILITY convertShaderStage(ShaderType s);
    D3D12_BLEND convertBlendValue(BlendFactor value);
    D3D12_BLEND_OP convertBlendOp(BlendOp value);
    D3D12_STENCIL_OP convertStencilOp(StencilOp value);
    D3D12_COMPARISON_FUNC convertComparisonFunc(ComparisonFunc value);
    D3D_PRIMITIVE_TOPOLOGY convertPrimitiveType(PrimitiveType pt, uint32_t controlPoints);
    D3D12_TEXTURE_ADDRESS_MODE convertSamplerAddressMode(SamplerAddressMode mode);
    UINT convertSamplerReductionType(SamplerReductionType reductionType);
    D3D12_SHADING_RATE convertPixelShadingRate(VariableShadingRate shadingRate);
    D3D12_SHADING_RATE_COMBINER convertShadingRateCombiner(ShadingRateCombiner combiner);
#if NVRHI_D3D12_WITH_COOP_VECTOR_COMMON
    D3D12_LINEAR_ALGEBRA_DATATYPE convertCoopVecDataType(coopvec::DataType type);
    coopvec::DataType convertCoopVecDataType(D3D12_LINEAR_ALGEBRA_DATATYPE type);
    D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT convertCoopVecMatrixLayout(coopvec::MatrixLayout layout);
#endif

    void WaitForFence(ID3D12Fence* fence, uint64_t value, HANDLE event);
    uint32_t calcSubresource(uint32_t MipSlice, uint32_t ArraySlice, uint32_t PlaneSlice, uint32_t MipLevels, uint32_t ArraySize);
    void TranslateBlendState(const BlendState& inState, D3D12_BLEND_DESC& outState);
    void TranslateDepthStencilState(const DepthStencilState& inState, D3D12_DEPTH_STENCIL_DESC& outState);
    void TranslateRasterizerState(const RasterState& inState, D3D12_RASTERIZER_DESC& outState);
    
    struct Context
    {
        AutoPtr<ID3D12Device> device;
        AutoPtr<ID3D12Device2> device2;
        AutoPtr<ID3D12Device5> device5;
        AutoPtr<ID3D12Device8> device8;
        AutoPtr<ID3D12Device10> device10;
#if NVRHI_D3D12_WITH_COOP_VECTOR_COMMON
        AutoPtr<ID3D12DevicePreview> devicePreview;
#endif
#ifdef NVRHI_WITH_RTXMU
        MonoPtr<rtxmu::DxAccelStructManager> rtxMemUtil;
#endif

        AutoPtr<ID3D12CommandSignature> drawIndirectSignature;
        AutoPtr<ID3D12CommandSignature> drawIndexedIndirectSignature;
        AutoPtr<ID3D12CommandSignature> dispatchIndirectSignature;
        AutoPtr<ID3D12CommandSignature> dispatchMeshIndirectSignature;
        AutoPtr<ID3D12QueryHeap> timerQueryHeap;
        AutoPtr<Buffer> timerQueryResolveBuffer;

        bool logBufferLifetime = false;
        AutoPtr<IMessageCallback> messageCallback;
        void error(const std::string& message) const;
        void info(const std::string& message) const;
    };

    // One of the device's descriptor heaps. Reference counted (MAKE_RC_OBJ); DeviceResources owns the four
    // heaps, and IDevice::getDescriptorHeap hands them out without a reference.
    NVRHI_CLASS_CLSID(StaticDescriptorHeap, "8ec56a70-7452-4f25-ae57-015eef6b4f91")
    class StaticDescriptorHeap : public ObjectImpl<IDescriptorHeap>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(StaticDescriptorHeap)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(StaticDescriptorHeap)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::d3d12::IDescriptorHeap)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(StaticDescriptorHeap)
        NVRHI_END_INTERFACE_TABLE()

    private:
        const Context& m_Context;
        AutoPtr<ID3D12DescriptorHeap> m_Heap;
        AutoPtr<ID3D12DescriptorHeap> m_ShaderVisibleHeap;
        D3D12_DESCRIPTOR_HEAP_TYPE m_HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        D3D12_CPU_DESCRIPTOR_HANDLE m_StartCpuHandle = { 0 };
        D3D12_CPU_DESCRIPTOR_HANDLE m_StartCpuHandleShaderVisible = { 0 };
        D3D12_GPU_DESCRIPTOR_HANDLE m_StartGpuHandleShaderVisible = { 0 };
        uint32_t m_Stride = 0;
        uint32_t m_NumDescriptors = 0;
        std::vector<bool> m_AllocatedDescriptors;
        DescriptorIndex m_SearchStart = 0;
        uint32_t m_NumAllocatedDescriptors = 0;
        std::mutex m_Mutex;

        HRESULT Grow(uint32_t minRequiredSize);
    public:
        explicit StaticDescriptorHeap(const Context& context);

        HRESULT allocateResources(D3D12_DESCRIPTOR_HEAP_TYPE heapType, uint32_t numDescriptors, bool shaderVisible);
        void copyToShaderVisibleHeap(DescriptorIndex index, uint32_t count = 1);
        D3D12_DESCRIPTOR_HEAP_TYPE getHeapType() const { return m_HeapType; }
        
        DescriptorIndex allocateDescriptors(uint32_t count) noexcept override;
        DescriptorIndex allocateDescriptor() noexcept override;
        void releaseDescriptors(DescriptorIndex baseIndex, uint32_t count) noexcept override;
        void releaseDescriptor(DescriptorIndex index) noexcept override;
        D3D12_CPU_DESCRIPTOR_HANDLE getCpuHandle(DescriptorIndex index);
        D3D12_CPU_DESCRIPTOR_HANDLE& getCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE& retVal, DescriptorIndex index) noexcept override { retVal = getCpuHandle(index); return retVal; }
        D3D12_CPU_DESCRIPTOR_HANDLE getCpuHandleShaderVisible(DescriptorIndex index);
        D3D12_CPU_DESCRIPTOR_HANDLE& getCpuHandleShaderVisible(D3D12_CPU_DESCRIPTOR_HANDLE& retVal, DescriptorIndex index) noexcept override { retVal = getCpuHandleShaderVisible(index); return retVal; }
        D3D12_GPU_DESCRIPTOR_HANDLE getGpuHandle(DescriptorIndex index);
        D3D12_GPU_DESCRIPTOR_HANDLE& getGpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE& retVal, DescriptorIndex index) noexcept override { retVal = getGpuHandle(index); return retVal; }
        [[nodiscard]] ID3D12DescriptorHeap* getHeap() const noexcept override;
        [[nodiscard]] ID3D12DescriptorHeap* getShaderVisibleHeap() const noexcept override;
    };

    class DeviceResources
    {
    public:
        AutoPtr<StaticDescriptorHeap> renderTargetViewHeap;
        AutoPtr<StaticDescriptorHeap> depthStencilViewHeap;
        AutoPtr<StaticDescriptorHeap> shaderResourceViewHeap;
        AutoPtr<StaticDescriptorHeap> samplerHeap;
        utils::BitSetAllocator timerQueries;
#ifdef NVRHI_WITH_RTXMU
        std::mutex asListMutex;
        std::vector<uint64_t> asBuildsCompleted;
#endif

        // The cache does not own the RS objects, so store weak references
        std::unordered_map<size_t, RootSignature*> rootsigCache;

        explicit DeviceResources(const Context& context, const DeviceDesc& desc);

        uint8_t getFormatPlaneCount(DXGI_FORMAT format);

    private:
        const Context& m_Context;
        std::unordered_map<DXGI_FORMAT, uint8_t> m_DxgiFormatPlaneCounts;
    };


    NVRHI_CLASS_CLSID(Shader, "6526f6ac-c070-409f-bc2c-b053f53bc32f")
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
        std::vector<char> bytecode;
    #if NVRHI_D3D12_WITH_NVAPI
        std::vector<NVAPI_D3D12_PSO_EXTENSION_DESC*> extensions;
        std::vector<NV_CUSTOM_SEMANTIC> customSemantics;
        std::vector<uint32_t> coordinateSwizzling;
    #endif
        
        const ShaderDesc& getDesc() const noexcept override { return desc; }
        void getBytecode(const void** ppBytecode, size_t* pSize) const noexcept override;
    };

    class ShaderLibrary;

    NVRHI_CLASS_CLSID(ShaderLibraryEntry, "6084b252-d5cc-4ff6-aaed-2a6eadb5e32b")
    class ShaderLibraryEntry : public ObjectImpl<IShader>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(ShaderLibraryEntry)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ShaderLibraryEntry)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IShader)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(ShaderLibraryEntry)
        NVRHI_END_INTERFACE_TABLE()

        ShaderDesc desc;
        AutoPtr<IShaderLibrary> library;

        ShaderLibraryEntry(IShaderLibrary* pLibrary, const char* entryName, ShaderType shaderType)
            : library(pLibrary)
        {
            desc.shaderType = shaderType;
            desc.entryName = entryName;
        }

        const ShaderDesc& getDesc() const noexcept override { return desc; }
        void getBytecode(const void** ppBytecode, size_t* pSize) const noexcept override;
    };

    NVRHI_CLASS_CLSID(ShaderLibrary, "0ffc4819-c43f-47d0-a6fd-0222b7f2a120")
    class ShaderLibrary : public ObjectImpl<IShaderLibrary>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(ShaderLibrary)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ShaderLibrary)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IShaderLibrary)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(ShaderLibrary)
        NVRHI_END_INTERFACE_TABLE()

        std::vector<char> bytecode;

        void getBytecode(const void** ppBytecode, size_t* pSize) const noexcept override;
        ShaderHandle getShader(const char* entryName, ShaderType shaderType);
        FRESULT getShader(const char* entryName, ShaderType shaderType, IShader** ppShader) noexcept override { return utils::ReturnObject(ppShader, [&] { return getShader(entryName, shaderType); }); }
    };

    NVRHI_CLASS_CLSID(Heap, "3d3c451e-be2e-48ec-89dc-fa7d8c2e6802")
    class Heap : public ObjectImpl<IHeap>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Heap)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Heap)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IHeap)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Heap)
        NVRHI_END_INTERFACE_TABLE()

        HeapDesc desc;
        AutoPtr<ID3D12Heap> heap;

        const HeapDesc& getDesc() noexcept override { return desc; }
    };

    NVRHI_CLASS_CLSID(Texture, "98601210-9058-416e-af5f-f20b8a5e23b5")
    class Texture : public ObjectImpl<ITexture>, public TextureStateExtension
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Texture)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Texture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ITexture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Texture)
        NVRHI_END_INTERFACE_TABLE()

        const TextureDesc desc;
        const D3D12_RESOURCE_DESC1 resourceDesc;
        AutoPtr<ID3D12Resource> resource;
        uint8_t planeCount = 1;
        HANDLE sharedHandle = nullptr;
        HeapHandle heap;


        Texture(const Context& context, DeviceResources& resources, TextureDesc desc, const D3D12_RESOURCE_DESC1& resourceDesc)
            : TextureStateExtension(this->desc)
            , desc(std::move(desc))
            , resourceDesc(resourceDesc)
            , m_Context(context)
            , m_Resources(resources)
        {
            TextureStateExtension::stateInitialized = true;
        }

        ~Texture();

        const TextureDesc& getDesc() const noexcept override { return desc; }
        bool queryMemoryRequirements(MemoryRequirements&) noexcept override { utils::NotSupported(); return false; }

        NativeObject getNativeObject(ObjectType objectType) noexcept override;
        NativeObject getNativeView(ObjectType objectType, Format format, const TextureSubresourceSet& subresources, TextureDimension dimension, bool isReadOnlyDSV = false,
            _In_opt_ const ComponentMapping* overrideComponentMapping = nullptr) noexcept override;

        void postCreate();
        void createSRV(size_t descriptor, Format format, TextureDimension dimension, TextureSubresourceSet subresources,
            ComponentMapping componentMapping) const;
        void createUAV(size_t descriptor, Format format, TextureDimension dimension, TextureSubresourceSet subresources) const;
        void createRTV(size_t descriptor, Format format, TextureSubresourceSet subresources) const;
        void createDSV(size_t descriptor, TextureSubresourceSet subresources, bool isReadOnly = false) const;
        DescriptorIndex getClearMipLevelUAV(uint32_t mipLevel, Format interpretFormat);

    private:
        const Context& m_Context;
        DeviceResources& m_Resources;

        TextureBindingKey_HashMap<DescriptorIndex> m_RenderTargetViews;
        TextureBindingKey_HashMap<DescriptorIndex> m_DepthStencilViews;
        TextureBindingKey_HashMap<DescriptorIndex> m_CustomSRVs;
        TextureBindingKey_HashMap<DescriptorIndex> m_CustomUAVs;
        std::vector<DescriptorIndex> m_ClearMipLevelUAVs;
    };

    NVRHI_CLASS_CLSID(Buffer, "21664363-1c74-4957-9697-04f56fdba1f7")
    class Buffer : public ObjectImpl<IBuffer>, public BufferStateExtension
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Buffer)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Buffer)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IBuffer)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Buffer)
        NVRHI_END_INTERFACE_TABLE()

        const BufferDesc desc;
        AutoPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS gpuVA{};
        D3D12_RESOURCE_DESC1 resourceDesc{};

        HeapHandle heap;

        AutoPtr<ID3D12Fence> lastUseFence;
        uint64_t lastUseFenceValue = 0;
        HANDLE sharedHandle = nullptr;

        Buffer(const Context& context, DeviceResources& resources, BufferDesc desc, bool enhancedBarriersSupported)
            : BufferStateExtension(this->desc)
            , desc(std::move(desc))
            , m_Context(context)
            , m_Resources(resources)
            , m_EnhancedBarriersSupported(enhancedBarriersSupported)
        { }

        ~Buffer();
        
        const BufferDesc& getDesc() const noexcept override { return desc; }
        GpuVirtualAddress getGpuVirtualAddress() const noexcept override { return gpuVA; }
        bool queryMemoryRequirements(MemoryRequirements& outRequirements) noexcept override;
        MemoryRequirements getMemoryRequirements() const;

        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        void postCreate();
        DescriptorIndex getClearUAV();
        void createCBV(size_t descriptor, BufferRange range) const;
        void createSRV(size_t descriptor, Format format, BufferRange range, ResourceType type) const;
        void createUAV(size_t descriptor, Format format, BufferRange range, ResourceType type) const;
        static void createNullSRV(size_t descriptor, Format format, const Context& context);
        static void createNullUAV(size_t descriptor, Format format, const Context& context);

    private:
        const Context& m_Context;
        DeviceResources& m_Resources;
        DescriptorIndex m_ClearUAV = c_InvalidDescriptorIndex;
        const bool m_EnhancedBarriersSupported;
    };

    NVRHI_CLASS_CLSID(StagingTexture, "d7e41d38-2cff-477a-8614-9d6c857bd6fa")
    class StagingTexture : public ObjectImpl<IStagingTexture>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(StagingTexture)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(StagingTexture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IStagingTexture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(StagingTexture)
        NVRHI_END_INTERFACE_TABLE()

        TextureDesc desc;
        D3D12_RESOURCE_DESC1 resourceDesc{};
        AutoPtr<Buffer> buffer;
        CpuAccessMode cpuAccess = CpuAccessMode::None;
        std::vector<UINT64> subresourceOffsets;

        AutoPtr<ID3D12Fence> lastUseFence;
        uint64_t lastUseFenceValue = 0;

        struct SliceRegion
        {
            // offset and size in bytes of this region inside the buffer
            off_t offset = 0;
            size_t size = 0;

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        };

        SliceRegion mappedRegion;
        CpuAccessMode mappedAccess = CpuAccessMode::None;

        // returns a SliceRegion struct corresponding to the subresource that slice points at
        // note that this always returns the entire subresource
        SliceRegion getSliceRegion(ID3D12Device *device, const TextureSlice& slice);

        // returns the total size in bytes required for this staging texture
        size_t getSizeInBytes(ID3D12Device *device);

        void computeSubresourceOffsets(ID3D12Device *device);
        
        const TextureDesc& getDesc() const noexcept override { return desc; }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;
    };

    NVRHI_CLASS_CLSID(SamplerFeedbackTexture, "283d4b1c-6d3e-4983-abd9-c15c940169c8")
    class SamplerFeedbackTexture : public ObjectImpl<ISamplerFeedbackTexture>, public TextureStateExtension
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(SamplerFeedbackTexture)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SamplerFeedbackTexture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ISamplerFeedbackTexture)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(SamplerFeedbackTexture)
        NVRHI_END_INTERFACE_TABLE()

        const SamplerFeedbackTextureDesc desc;
        const TextureDesc textureDesc; // used with state tracking
        AutoPtr<ID3D12Resource> resource;
        TextureHandle pairedTexture;
        DescriptorIndex clearDescriptorIndex = c_InvalidDescriptorIndex;

        SamplerFeedbackTexture(const Context& context, SamplerFeedbackTextureDesc desc, TextureDesc textureDesc, ITexture* pairedTexture)
            : TextureStateExtension(SamplerFeedbackTexture::textureDesc)
            , desc(std::move(desc))
            , textureDesc(std::move(textureDesc))
            , pairedTexture(pairedTexture)
            , m_Context(context)
        {
            TextureStateExtension::stateInitialized = true;
            TextureStateExtension::isSamplerFeedback = true;
        }

        const SamplerFeedbackTextureDesc& getDesc() const noexcept override { return desc; }
        TextureHandle getPairedTexture() { return pairedTexture; }
        FRESULT getPairedTexture(ITexture** ppTexture) noexcept override { return utils::ReturnObject(ppTexture, [&] { return getPairedTexture(); }); }

        void createUAV(size_t descriptor) const;

        NativeObject getNativeObject(ObjectType objectType) noexcept override;

    private:
        const Context& m_Context;
    };

    NVRHI_CLASS_CLSID(Sampler, "004ddf5d-40d2-4a51-8826-77e859303fa7")
    class Sampler : public ObjectImpl<ISampler>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Sampler)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Sampler)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ISampler)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(Sampler)
        NVRHI_END_INTERFACE_TABLE()

        Sampler(const Context& context, const SamplerDesc& desc);
        
        void createDescriptor(size_t descriptor) const;

        const SamplerDesc& getDesc() const noexcept override { return m_Desc; }

    private:
        const Context& m_Context;
        const SamplerDesc m_Desc;
        D3D12_SAMPLER_DESC m_d3d12desc;
    };

    NVRHI_CLASS_CLSID(InputLayout, "2657309b-f0f2-4e3b-a041-c675ad308e9e")
    class InputLayout : public ObjectImpl<IInputLayout>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(InputLayout)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(InputLayout)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IInputLayout)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(InputLayout)
        NVRHI_END_INTERFACE_TABLE()

        std::vector<VertexAttributeDesc> attributes;
        std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;

        // maps a binding slot to an element stride
        std::unordered_map<uint32_t, uint32_t> elementStrides;
        
        uint32_t getNumAttributes() const noexcept override;
        const VertexAttributeDesc* getAttributeDesc(uint32_t index) const noexcept override;
    };

    NVRHI_CLASS_CLSID(EventQuery, "a7c1bd43-c37f-4240-8e33-2c95a9cdd4fa")
    class EventQuery : public ObjectImpl<IEventQuery>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(EventQuery)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(EventQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IEventQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(EventQuery)
        NVRHI_END_INTERFACE_TABLE()

        AutoPtr<ID3D12Fence> fence;
        uint64_t fenceCounter = 0;
        bool started = false;
        bool resolved = false;
    };

    NVRHI_CLASS_CLSID(TimerQuery, "38e37193-2e0c-41fa-997f-7b67591f3307")
    class TimerQuery : public ObjectImpl<ITimerQuery>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(TimerQuery)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(TimerQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ITimerQuery)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(TimerQuery)
        NVRHI_END_INTERFACE_TABLE()

        uint32_t beginQueryIndex = 0;
        uint32_t endQueryIndex = 0;

        AutoPtr<ID3D12Fence> fence;
        uint64_t fenceCounter = 0;

        bool started = false;
        bool resolved = false;
        float time = 0.f;

        TimerQuery(DeviceResources& resources)
            : m_Resources(resources)
        { }

        ~TimerQuery();

    private:
        DeviceResources& m_Resources;
    };

    NVRHI_CLASS_CLSID(BindingLayout, "8055d454-f62e-4d7a-943b-cc6924756e66")
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
        uint32_t pushConstantByteSize = 0;
        RootParameterIndex rootParameterPushConstants = ~0u;
        RootParameterIndex rootParameterSRVetc = ~0u;
        RootParameterIndex rootParameterSamplers = ~0u;
        int descriptorTableSizeSRVetc = 0;
        int descriptorTableSizeSamplers = 0;
        std::vector<D3D12_DESCRIPTOR_RANGE1> descriptorRangesSRVetc;
        std::vector<D3D12_DESCRIPTOR_RANGE1> descriptorRangesSamplers;
        std::vector<BindingLayoutItem> bindingLayoutsSRVetc;
        static_vector<std::pair<RootParameterIndex, D3D12_ROOT_DESCRIPTOR1>, c_MaxVolatileConstantBuffersPerLayout> rootParametersVolatileCB;
        static_vector<D3D12_ROOT_PARAMETER1, 32> rootParameters;

        BindingLayout(const BindingLayoutDesc& desc);

        const BindingLayoutDesc* getDesc() const noexcept override { return &desc; }
        const BindlessLayoutDesc* getBindlessDesc() const noexcept override { return nullptr; }
    };

    NVRHI_CLASS_CLSID(BindlessLayout, "4eb4e730-dd1c-4e78-9d98-f8a4f620f67b")
    class BindlessLayout : public ObjectImpl<IBindingLayout>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(BindlessLayout)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(BindlessLayout)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IBindingLayout)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(BindlessLayout)
        NVRHI_END_INTERFACE_TABLE()

        BindlessLayoutDesc desc;
        static_vector<D3D12_DESCRIPTOR_RANGE1, 32> descriptorRanges;
        D3D12_ROOT_PARAMETER1 rootParameter{};

        BindlessLayout(const BindlessLayoutDesc& desc);

        const BindingLayoutDesc* getDesc() const noexcept override { return nullptr; }
        const BindlessLayoutDesc* getBindlessDesc() const noexcept override { return &desc; }
    };

    NVRHI_CLASS_CLSID(RootSignature, "d412e1f2-46a8-4eff-bbf8-0d01333d8f0f")
    class RootSignature : public ObjectImpl<IRootSignature>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(RootSignature)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(RootSignature)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::d3d12::IRootSignature)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(RootSignature)
        NVRHI_END_INTERFACE_TABLE()

        size_t hash = 0;
        static_vector<std::pair<BindingLayoutHandle, RootParameterIndex>, c_MaxBindingLayouts> pipelineLayouts;
        AutoPtr<ID3D12RootSignature> handle;
        uint32_t pushConstantByteSize = 0;
        RootParameterIndex rootParameterPushConstants = ~0u;
        
        RootSignature(DeviceResources& resources)
            : m_Resources(resources)
        { }

        ~RootSignature();
        NativeObject getNativeObject(ObjectType objectType) noexcept override;

    private:
        DeviceResources& m_Resources;
    };

    NVRHI_CLASS_CLSID(Framebuffer, "8392197d-e14e-49cf-924b-8f58723b053e")
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

        static_vector<TextureHandle, c_MaxRenderTargets + 1> textures;
        static_vector<DescriptorIndex, c_MaxRenderTargets> RTVs;
        DescriptorIndex DSV = c_InvalidDescriptorIndex;
        uint32_t rtWidth = 0;
        uint32_t rtHeight = 0;

        Framebuffer(DeviceResources& resources)
            : m_Resources(resources)
        { }

        ~Framebuffer();

        const FramebufferDesc& getDesc() const noexcept override { return desc; }
        const FramebufferInfoEx& getFramebufferInfo() const noexcept override { return framebufferInfo; }

    private:
        DeviceResources& m_Resources;
    };

    struct DX12_ViewportState
    {
        UINT numViewports = 0;
        D3D12_VIEWPORT viewports[16] = {};
        UINT numScissorRects = 0;
        D3D12_RECT scissorRects[16] = {};
    };

    NVRHI_CLASS_CLSID(GraphicsPipeline, "5c461639-54ea-4822-8454-6e7f24059ef6")
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
        FramebufferInfo framebufferInfo;

        AutoPtr<RootSignature> rootSignature;
        AutoPtr<ID3D12PipelineState> pipelineState;

        bool requiresBlendFactor = false;
        
        const GraphicsPipelineDesc& getDesc() const noexcept override { return desc; }
        const FramebufferInfo& getFramebufferInfo() const noexcept override { return framebufferInfo; }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;
    };

    NVRHI_CLASS_CLSID(ComputePipeline, "0fdca282-7d9e-4e69-a0c1-278d2321d0a5")
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

        AutoPtr<RootSignature> rootSignature;
        AutoPtr<ID3D12PipelineState> pipelineState;
        
        const ComputePipelineDesc& getDesc() const noexcept override { return desc; }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;
    };

    NVRHI_CLASS_CLSID(MeshletPipeline, "1f45a754-6866-47a8-99bb-1f50b3e4e80c")
    class MeshletPipeline : public ObjectImpl<IMeshletPipeline>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(MeshletPipeline)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(MeshletPipeline)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IMeshletPipeline)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(MeshletPipeline)
        NVRHI_END_INTERFACE_TABLE()

        MeshletPipelineDesc desc;
        FramebufferInfo framebufferInfo;

        AutoPtr<RootSignature> rootSignature;
        AutoPtr<ID3D12PipelineState> pipelineState;

        DX12_ViewportState viewportState;

        bool requiresBlendFactor = false;
        
        const MeshletPipelineDesc& getDesc() const noexcept override { return desc; }
        const FramebufferInfo& getFramebufferInfo() const noexcept override { return framebufferInfo; }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;
    };
    
    NVRHI_CLASS_CLSID(BindingSet, "013a8656-a296-466f-8ed7-bdcbfa0524f7")
    class BindingSet : public ObjectImpl<IBindingSet>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(BindingSet)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(BindingSet)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IBindingSet)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(BindingSet)
        NVRHI_END_INTERFACE_TABLE()

        AutoPtr<BindingLayout> layout;
        BindingSetDesc desc;

        // ShaderType -> DescriptorIndex
        DescriptorIndex descriptorTableSRVetc = 0;
        DescriptorIndex descriptorTableSamplers = 0;
        RootParameterIndex rootParameterIndexSRVetc = 0;
        RootParameterIndex rootParameterIndexSamplers = 0;
        bool descriptorTableValidSRVetc = false;
        bool descriptorTableValidSamplers = false;
        bool hasUavBindings = false;

        static_vector<std::pair<RootParameterIndex, IBuffer*>, c_MaxVolatileConstantBuffersPerLayout> rootParametersVolatileCB;
        
        std::vector<AutoPtr<IRHIObject>> resources;

        std::vector<uint16_t> bindingsThatNeedTransitions;

        BindingSet(const Context& context, DeviceResources& resources)
            : m_Context(context)
            , m_Resources(resources)
        { }

        ~BindingSet();

        void createDescriptors();

        const BindingSetDesc* getDesc() const noexcept override { return &desc; }
        IBindingLayout* getLayout() const noexcept override { return layout; }

    private:
        const Context& m_Context;
        DeviceResources& m_Resources;
    };

    NVRHI_CLASS_CLSID(DescriptorTable, "08810f03-d563-4aee-a118-cb696b15403d")
    class DescriptorTable : public ObjectImpl<IDescriptorTable>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(DescriptorTable)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DescriptorTable)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IDescriptorTable)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IBindingSet)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(DescriptorTable)
        NVRHI_END_INTERFACE_TABLE()

        uint32_t capacity = 0;
        DescriptorIndex firstDescriptor = 0;
        BindingLayoutHandle layout;

        DescriptorTable(DeviceResources& resources)
            : m_Resources(resources)
        { }

        ~DescriptorTable();

        const BindingSetDesc* getDesc() const noexcept override { return nullptr; }
        IBindingLayout* getLayout() const noexcept override { return layout; }
        uint32_t getCapacity() const noexcept override { return capacity; }
        uint32_t getFirstDescriptorIndexInHeap() const noexcept override { return firstDescriptor; }
        
        bool isSamplerTable() const;
        StaticDescriptorHeap& getDescriptorHeap() const;

    private:
        DeviceResources& m_Resources;
    };

    DX12_ViewportState convertViewportState(const RasterState& rasterState, const FramebufferInfoEx& framebufferInfo, const ViewportState& vpState);

    class TextureState
    {
    public:
        std::vector<OptionalResourceState> subresourceStates;
        bool enableUavBarriers = true;
        bool firstUavBarrierPlaced = false;
        bool permanentTransition = false;

        TextureState(uint32_t numSubresources)
        {
            subresourceStates.resize(numSubresources, c_ResourceStateUnknown);
        }
    };

    class BufferState
    {
    public:
        OptionalResourceState state = c_ResourceStateUnknown;
        bool enableUavBarriers = true;
        bool firstUavBarrierPlaced = false;
        D3D12_GPU_VIRTUAL_ADDRESS volatileData = 0;
        bool permanentTransition = false;
    };

    D3D12_RESOURCE_STATES convertResourceStates(ResourceStates stateBits);
    
    struct EnhancedResourceStateMapping
    {
        ResourceStates nvrhiState;
        D3D12_BARRIER_SYNC sync;
        D3D12_BARRIER_ACCESS access;
        D3D12_BARRIER_LAYOUT layout;
    };

    EnhancedResourceStateMapping convertResourceStatesForEnhancedBarriers(ResourceStates state, bool isTexture);
    
    // Upload / scratch buffer chunk. Shared between UploadManager's pool and its current chunk,
    // so it is reference counted: create with MAKE_RC_OBJ / MAKE_RC_OBJ_PTR only.
    class BufferChunk;
    NVRHI_CCLSID(BufferChunk, "7326d791-307a-43f5-b118-f4455abb2c82")
    class BufferChunk final : public ObjectImpl<IObject>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(BufferChunk)

        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(BufferChunk)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IObject)
        NVRHI_IMPLEMENTS_CLASS(BufferChunk)
        NVRHI_END_INTERFACE_TABLE()

        static const uint64_t c_sizeAlignment = 4096; // GPU page size

        AutoPtr<ID3D12Resource> buffer;
        uint64_t version = 0;
        uint64_t bufferSize = 0;
        uint64_t writePointer = 0;
        void* cpuVA = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
        uint32_t identifier = 0;

        ~BufferChunk();
    };

    class UploadManager
    {
    public:
        UploadManager(const Context& context, class Queue* pQueue, size_t defaultChunkSize, uint64_t memoryLimit, bool isScratchBuffer);

        bool suballocateBuffer(uint64_t size, ID3D12GraphicsCommandList* pCommandList, ID3D12Resource** pBuffer, size_t* pOffset, void** pCpuVA,
            D3D12_GPU_VIRTUAL_ADDRESS* pGpuVA, uint64_t currentVersion, uint32_t alignment = 256);

        void submitChunks(uint64_t currentVersion, uint64_t submittedVersion);

    private:
        const Context& m_Context;
        Queue* m_Queue;
        size_t m_DefaultChunkSize = 0;
        uint64_t m_MemoryLimit = 0;
        uint64_t m_AllocatedMemory = 0;
        bool m_IsScratchBuffer = false;

        std::list<AutoPtr<BufferChunk>> m_ChunkPool;
        AutoPtr<BufferChunk> m_CurrentChunk;

        [[nodiscard]] AutoPtr<BufferChunk> createChunk(size_t size) const;
    };

    NVRHI_CLASS_CLSID(OpacityMicromap, "2da5bb9c-c750-479c-aa58-ec3a368cd182")
    class OpacityMicromap : public ObjectImpl<rt::IOpacityMicromap>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(OpacityMicromap)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(OpacityMicromap)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::rt::IOpacityMicromap)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(OpacityMicromap)
        NVRHI_END_INTERFACE_TABLE()

        AutoPtr<d3d12::Buffer> dataBuffer;
        rt::OpacityMicromapDesc desc;
        bool allowUpdate = false;
        bool compacted = false;

        OpacityMicromap()
        { }

        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        const rt::OpacityMicromapDesc& getDesc() const noexcept override { return desc; }
        bool queryMemoryRequirements(MemoryRequirements& outRequirements) noexcept override;
        bool isCompacted() const noexcept override { return compacted; }
        uint64_t getDeviceAddress() const noexcept override;
    };

    NVRHI_CLASS_CLSID(AccelStruct, "7af7f43f-e667-4bc4-8e17-949478be31b1")
    class AccelStruct : public ObjectImpl<rt::IAccelStruct>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(AccelStruct)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(AccelStruct)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::rt::IAccelStruct)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(AccelStruct)
        NVRHI_END_INTERFACE_TABLE()

        AutoPtr<d3d12::Buffer> dataBuffer;
        std::vector<rt::AccelStructHandle> bottomLevelASes;
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> dxrInstances;
        rt::AccelStructDesc desc;
        bool allowUpdate = false;
        bool compacted = false;
        size_t rtxmuId = ~0ull;
#ifdef NVRHI_WITH_RTXMU
        D3D12_GPU_VIRTUAL_ADDRESS rtxmuGpuVA = 0;
#endif

        AccelStruct(const Context& context)
            : m_Context(context)
        { }

        ~AccelStruct();

        void createSRV(size_t descriptor) const;

        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        const rt::AccelStructDesc& getDesc() const noexcept override { return desc; }
        bool queryMemoryRequirements(MemoryRequirements& outRequirements) noexcept override;
        bool isCompacted() const noexcept override { return compacted; }
        uint64_t getDeviceAddress() const noexcept override;
        
    private:
        const Context& m_Context;
    };

    NVRHI_CLASS_CLSID(RayTracingPipeline, "05584446-71dd-4e7d-b012-46c787707d34")
    class RayTracingPipeline : public ObjectImpl<rt::IPipeline>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(RayTracingPipeline)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(RayTracingPipeline)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::rt::IPipeline)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(RayTracingPipeline)
        NVRHI_END_INTERFACE_TABLE()

        rt::PipelineDesc desc;

        std::unordered_map<IBindingLayout*, RootSignatureHandle> localRootSignatures;
        AutoPtr<RootSignature> globalRootSignature;
        AutoPtr<ID3D12StateObject> pipelineState;
        AutoPtr<ID3D12StateObjectProperties> pipelineInfo;

        struct ExportTableEntry
        {
            IBindingLayout* bindingLayout;
            const void* pShaderIdentifier;
        };

        std::unordered_map<std::string, ExportTableEntry> exports;
        uint32_t maxLocalRootParameters = 0;

        RayTracingPipeline(const Context& context, Device* device)
            : m_Context(context)
            , m_Device(device)
        { }

        const ExportTableEntry* getExport(const char* name);
        uint32_t getShaderTableEntrySize() const;
        bool hasLocalResources() const { return maxLocalRootParameters != 0; }

        const rt::PipelineDesc& getDesc() const noexcept override { return desc; }
        rt::ShaderTableHandle createShaderTable(rt::ShaderTableDesc const& stDesc);
        FRESULT createShaderTable(rt::ShaderTableDesc const& stDesc, rt::IShaderTable** ppShaderTable) noexcept override { return utils::ReturnObject(ppShaderTable, [&] { return createShaderTable(stDesc); }); }

    private:
        const Context& m_Context;
        Device* m_Device;
    };


    class ShaderTableState
    {
    public:
        uint32_t committedVersion = 0;
        ID3D12DescriptorHeap* descriptorHeapSRV = nullptr;
        ID3D12DescriptorHeap* descriptorHeapSamplers = nullptr;
        D3D12_DISPATCH_RAYS_DESC dispatchRaysTemplate = {};
    };

    NVRHI_CLASS_CLSID(ShaderTable, "8cf8f8cc-b895-4f66-9e00-bb178e0ba960")
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
            const void* pShaderIdentifier;
            BindingSetHandle localBindings;
        };

        AutoPtr<RayTracingPipeline> pipeline;

        Entry rayGenerationShader = {};
        std::vector<Entry> missShaders;
        std::vector<Entry> callableShaders;
        std::vector<Entry> hitGroups;

        uint32_t version = 0;

        BufferHandle cache;
        ShaderTableState cacheState;
        
        ShaderTable(const Context& context, RayTracingPipeline* _pipeline, rt::ShaderTableDesc const& desc)
            : pipeline(_pipeline)
            , m_Context(context)
            , m_Desc(desc)
        { }

        size_t getUploadSize() const { return pipeline->getShaderTableEntrySize() * size_t(getNumEntries()); }
        bool isStateValid(ShaderTableState const& state, DeviceResources const& resources) const;
        void bake(uint8_t* cpuVA, D3D12_GPU_VIRTUAL_ADDRESS gpuVA, DeviceResources& resources,
            ShaderTableState& state);
        
        rt::ShaderTableDesc const& getDesc() const noexcept override { return m_Desc; }
        uint32_t getNumEntries() const noexcept override;
        rt::IPipeline* getPipeline() const noexcept override { return pipeline; }
        void setRayGenerationShader(const char* exportName, IBindingSet* bindings = nullptr) noexcept override;
        int addMissShader(const char* exportName, IBindingSet* bindings = nullptr) noexcept override;
        int addHitGroup(const char* exportName, IBindingSet* bindings = nullptr) noexcept override;
        int addCallableShader(const char* exportName, IBindingSet* bindings = nullptr) noexcept override;
        void clearMissShaders() noexcept override;
        void clearHitShaders() noexcept override;
        void clearCallableShaders() noexcept override;

    private:
        const Context& m_Context;
        rt::ShaderTableDesc const m_Desc;

        bool verifyExport(const RayTracingPipeline::ExportTableEntry* pExport, IBindingSet* bindings) const;
    };


    class Queue
    {
    public:
        AutoPtr<ID3D12CommandQueue> queue;
        AutoPtr<ID3D12Fence> fence;
        CommandListLifetimeTrackerHandle lifetimeTracker;

        std::atomic<uint64_t> lastSubmittedInstance = 0;
        std::atomic<uint64_t> lastCompletedInstance = 0;
        std::atomic<uint64_t> recordingInstance = 1;

        explicit Queue(const Context& context, ID3D12CommandQueue* queue, CommandListLifetimeTrackerHandle&& lifetimeTracker);
        uint64_t updateLastCompletedInstance();
        uint64_t Signal();

    private:
        const Context& m_Context;
    };

    class CommandListInstance;

    NVRHI_CLASS_CLSID(CommandListLifetimeTracker, "8503a04c-0818-4f51-8b59-5c6d555ecb1c")
    class CommandListLifetimeTracker final : public ObjectImpl<ICommandListLifetimeTracker>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(CommandListLifetimeTracker)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(CommandListLifetimeTracker)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ICommandListLifetimeTracker)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(CommandListLifetimeTracker)
        NVRHI_END_INTERFACE_TABLE()

        CommandListLifetimeTracker(Device* device, const Context& context, DeviceResources& resources, CommandQueue executionQueue);

        // ICommandListTracker implementation
        virtual void runGarbageCollection() noexcept override;

        // D3D12 specific methods
        void push(AutoPtr<CommandListInstance> commandList);

    private:
        Device* m_Device;
        const Context& m_Context;
        DeviceResources& m_Resources;
        CommandQueue m_ExecutionQueue;
        std::deque<AutoPtr<CommandListInstance>> m_CommandListsInFlight;
    };
    
    // Pooled D3D12 command allocator + command list pair, shared between CommandList's pool and its
    // active list. Reference counted: create with MAKE_RC_OBJ / MAKE_RC_OBJ_PTR only.
    class InternalCommandList;
    NVRHI_CCLSID(InternalCommandList, "c9eb2d66-e4e1-4fd9-bcef-61148f3cd821")
    class InternalCommandList final : public ObjectImpl<IObject>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(InternalCommandList)

        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(InternalCommandList)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IObject)
        NVRHI_IMPLEMENTS_CLASS(InternalCommandList)
        NVRHI_END_INTERFACE_TABLE()

        AutoPtr<ID3D12CommandAllocator> allocator;
        AutoPtr<ID3D12GraphicsCommandList> commandList;
        AutoPtr<ID3D12GraphicsCommandList4> commandList4;
        AutoPtr<ID3D12GraphicsCommandList6> commandList6;
        AutoPtr<ID3D12GraphicsCommandList7> commandList7;
#if NVRHI_D3D12_WITH_COOP_VECTOR_COMMON
        AutoPtr<ID3D12GraphicsCommandListPreview> commandListPreview;
#endif
        uint64_t lastSubmittedInstance = 0;
#if NVRHI_WITH_AFTERMATH
        GFSDK_Aftermath_ContextHandle aftermathContext;
#endif
    };

    // One submission of a CommandList, kept alive by the lifetime tracker until the GPU is done with it.
    // Reference counted: create with MAKE_RC_OBJ / MAKE_RC_OBJ_PTR only.
    NVRHI_CCLSID(CommandListInstance, "177246c6-0d12-41b1-9f5a-dd859f9300e6")
    class CommandListInstance final : public ObjectImpl<IObject>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(CommandListInstance)

        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(CommandListInstance)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IObject)
        NVRHI_IMPLEMENTS_CLASS(CommandListInstance)
        NVRHI_END_INTERFACE_TABLE()

        uint64_t submittedInstance = 0;
        CommandQueue commandQueue = CommandQueue::Graphics;
        AutoPtr<ID3D12Fence> fence;
        AutoPtr<ID3D12CommandAllocator> commandAllocator;
        AutoPtr<ID3D12CommandList> commandList;
        std::vector<AutoPtr<IRHIObject>> referencedResources;
        std::vector<AutoPtr<IUnknown>> referencedNativeResources;
        std::vector<AutoPtr<StagingTexture>> referencedStagingTextures;
        std::vector<AutoPtr<Buffer>> referencedStagingBuffers;
        std::vector<AutoPtr<TimerQuery>> referencedTimerQueries;
#ifdef NVRHI_WITH_RTXMU
        std::vector<uint64_t> rtxmuBuildIds;
        std::vector<uint64_t> rtxmuCompactionIds;
#endif
    };

    NVRHI_CLASS_CLSID(CommandList, "492c8ee9-1fe6-4ae7-83ef-339279a67f85")
    class CommandList final : public ObjectImpl<nvrhi::d3d12::ICommandList>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(CommandList)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(CommandList)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::d3d12::ICommandList)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ICommandList)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(CommandList)
        NVRHI_END_INTERFACE_TABLE()

        // Internal interface functions

        CommandList(class Device* device, const Context& context, DeviceResources& resources, const CommandListParameters& params);
        ~CommandList();
        AutoPtr<CommandListInstance> executed(Queue* pQueue);
        void requireTextureState(ITexture* texture, TextureSubresourceSet subresources, ResourceStates state);
        void requireSamplerFeedbackTextureState(ISamplerFeedbackTexture* texture, ResourceStates state);
        void requireBufferState(IBuffer* buffer, ResourceStates state);
        ID3D12CommandList* getD3D12CommandList() const { return m_ActiveCommandList->commandList; }

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
        void dispatchIndirect(uint32_t offsetBytes) noexcept override;

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

        void beginMarker(const char *name) noexcept override;
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

        nvrhi::IDevice* getDevice() noexcept override;
        const CommandListParameters& getDesc() noexcept override { return m_Desc; }

        // D3D12 specific methods

        bool allocateUploadBuffer(size_t size, void** pCpuAddress, D3D12_GPU_VIRTUAL_ADDRESS* pGpuAddress) noexcept override;
        bool allocateDxrScratchBuffer(size_t size, void** pCpuAddress, D3D12_GPU_VIRTUAL_ADDRESS* pGpuAddress);
        bool commitDescriptorHeaps() noexcept override;
        D3D12_GPU_VIRTUAL_ADDRESS getBufferGpuVA(IBuffer* buffer) noexcept override;

        void updateGraphicsVolatileBuffers() noexcept override;
        void updateComputeVolatileBuffers() noexcept override;

        void setComputeBindings(
            const BindingSetVector& bindings,
            uint32_t bindingUpdateMask,
            IBuffer* indirectParams,
            bool updateIndirectParams,
            const RootSignature* rootSignature);

        void setGraphicsBindings(
            const BindingSetVector& bindings,
            uint32_t bindingUpdateMask,
            IBuffer* indirectParams,
            bool updateIndirectParams,
            IBuffer* indirectCountBuffer,
            bool updateIndirectCountBuffer,
            const RootSignature* rootSignature);
        
    private:
        const Context& m_Context;
        DeviceResources& m_Resources;
        
        struct VolatileConstantBufferBinding
        {
            uint32_t bindingPoint; // RootParameterIndex
            Buffer* buffer;
            D3D12_GPU_VIRTUAL_ADDRESS address;
        };
        
        Device* m_Device;
        Queue* m_Queue;
        CommandListLifetimeTrackerHandle m_LifetimeTracker;
        UploadManager m_UploadManager;
        UploadManager m_DxrScratchManager;
        CommandListResourceStateTracker m_StateTracker;
        bool m_EnableAutomaticBarriers = true;
        
        CommandListParameters m_Desc;

        AutoPtr<InternalCommandList> m_ActiveCommandList;
        std::list<AutoPtr<InternalCommandList>> m_CommandListPool;
        AutoPtr<CommandListInstance> m_Instance;
        uint64_t m_RecordingVersion = 0;
#if NVRHI_WITH_AFTERMATH
        AftermathMarkerTracker m_AftermathTracker;
#endif

        // Cache for user-provided state

        GraphicsState m_CurrentGraphicsState;
        ComputeState m_CurrentComputeState;
        MeshletState m_CurrentMeshletState;
        rt::State m_CurrentRayTracingState;
        bool m_CurrentGraphicsStateValid = false;
        bool m_CurrentComputeStateValid = false;
        bool m_CurrentMeshletStateValid = false;
        bool m_CurrentRayTracingStateValid = false;
        bool m_BindingStatesDirty = false;

        // Cache for internal state

        ID3D12DescriptorHeap* m_CurrentHeapSRVetc = nullptr;
        ID3D12DescriptorHeap* m_CurrentHeapSamplers = nullptr;
        ID3D12Resource* m_CurrentUploadBuffer = nullptr;
        SinglePassStereoState m_CurrentSinglePassStereoState;
        
        std::unordered_map<IBuffer*, D3D12_GPU_VIRTUAL_ADDRESS> m_VolatileConstantBufferAddresses;
        bool m_AnyVolatileBufferWrites = false;

        // The barrier vectors are only used locally in commitBarriers. They are class members to avoid re-allocations.
        std::vector<D3D12_RESOURCE_BARRIER> m_D3DBarriers;
        std::vector<D3D12_TEXTURE_BARRIER> m_D3DTextureBarriers;
        std::vector<D3D12_BUFFER_BARRIER> m_D3DBufferBarriers;

        // Bound volatile buffer state. Saves currently bound volatile buffers and their current GPU VAs.
        // Necessary to patch the bound VAs when a buffer is updated between setGraphicsState and draw, or between draws.

        static_vector<VolatileConstantBufferBinding, c_MaxVolatileConstantBuffers> m_CurrentGraphicsVolatileCBs;
        static_vector<VolatileConstantBufferBinding, c_MaxVolatileConstantBuffers> m_CurrentComputeVolatileCBs;

        std::unordered_map<rt::IShaderTable*, MonoPtr<ShaderTableState>> m_UncachedShaderTableStates;
        ShaderTableState& getShaderTableState(rt::IShaderTable* shaderTable);
        
        void clearStateCache();

        void bindGraphicsPipeline(GraphicsPipeline* pso, bool updateRootSignature) const;
        void bindMeshletPipeline(MeshletPipeline* pso, bool updateRootSignature) const;
        void bindFramebuffer(Framebuffer* fb);
        void unbindShadingRateState();
        
        AutoPtr<InternalCommandList> createInternalCommandList() const;

        void buildTopLevelAccelStructInternal(AccelStruct* as, D3D12_GPU_VIRTUAL_ADDRESS instanceData, size_t numInstances, rt::AccelStructBuildFlags buildFlags);
    };

    NVRHI_CLASS_CLSID(Device, "a396b171-cf43-4e48-ad04-a9ad0233de55")
    class Device final : public ObjectImpl<IDevice>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(Device)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Device)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::d3d12::IDevice)
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
        FRESULT createHeap(const HeapDesc& d, IHeap** ppHeap) noexcept override { return utils::ReturnObject(ppHeap, [&] { return createHeap(d); }); }

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
        FRESULT createSamplerFeedbackTexture(ITexture* pairedTexture, const SamplerFeedbackTextureDesc& desc, ISamplerFeedbackTexture** ppTexture) noexcept override { return utils::ReturnObject(ppTexture, [&] { return createSamplerFeedbackTexture(pairedTexture, desc); }); }
        SamplerFeedbackTextureHandle createSamplerFeedbackForNativeTexture(ObjectType objectType, NativeObject texture, ITexture* pairedTexture);
        FRESULT createSamplerFeedbackForNativeTexture(ObjectType objectType, NativeObject texture, ITexture* pairedTexture, ISamplerFeedbackTexture** ppTexture) noexcept override { return utils::ReturnObject(ppTexture, [&] { return createSamplerFeedbackForNativeTexture(objectType, texture, pairedTexture); }); }

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

        ShaderHandle createShader(const ShaderDesc& d, const void* binary, size_t binarySize);
        FRESULT createShader(const ShaderDesc& d, const void* binary, size_t binarySize, IShader** ppShader) noexcept override { return utils::ReturnObject(ppShader, [&] { return createShader(d, binary, binarySize); }); }
        ShaderHandle createShaderSpecialization(IShader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants);
        FRESULT createShaderSpecialization(IShader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants, IShader** ppShader) noexcept override { return utils::ReturnObject(ppShader, [&] { return createShaderSpecialization(baseShader, constants, numConstants); }); }
        ShaderLibraryHandle createShaderLibrary(const void* binary, size_t binarySize);
        FRESULT createShaderLibrary(const void* binary, size_t binarySize, IShaderLibrary** ppShaderLibrary) noexcept override { return utils::ReturnObject(ppShaderLibrary, [&] { return createShaderLibrary(binary, binarySize); }); }

        SamplerHandle createSampler(const SamplerDesc& d);
        FRESULT createSampler(const SamplerDesc& d, ISampler** ppSampler) noexcept override { return utils::ReturnObject(ppSampler, [&] { return createSampler(d); }); }

        InputLayoutHandle createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, IShader* vertexShader);
        FRESULT createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, IShader* vertexShader, IInputLayout** ppInputLayout) noexcept override { return utils::ReturnObject(ppInputLayout, [&] { return createInputLayout(d, attributeCount, vertexShader); }); }

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
        FRESULT createMeshletPipeline1(const MeshletPipelineDesc& desc, FramebufferInfo const& fbinfo, IMeshletPipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createMeshletPipeline(desc, fbinfo); }); }

        MeshletPipelineHandle createMeshletPipeline(const MeshletPipelineDesc& desc, IFramebuffer* fb);
        FRESULT createMeshletPipeline2(const MeshletPipelineDesc& desc, IFramebuffer* fb, IMeshletPipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createMeshletPipeline(desc, fb); }); }

        rt::PipelineHandle createRayTracingPipeline(const rt::PipelineDesc& desc);
        FRESULT createRayTracingPipeline(const rt::PipelineDesc& desc, rt::IPipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createRayTracingPipeline(desc); }); }

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
        FRESULT createOpacityMicromap(const rt::OpacityMicromapDesc& desc, rt::IOpacityMicromap** ppOpacityMicromap) noexcept override { return utils::ReturnObject(ppOpacityMicromap, [&] { return createOpacityMicromap(desc); }); }
        rt::AccelStructHandle createAccelStruct(const rt::AccelStructDesc& desc);
        FRESULT createAccelStruct(const rt::AccelStructDesc& desc, rt::IAccelStruct** ppAccelStruct) noexcept override { return utils::ReturnObject(ppAccelStruct, [&] { return createAccelStruct(desc); }); }
        MemoryRequirements getAccelStructMemoryRequirements(rt::IAccelStruct* as);
        MemoryRequirements& getAccelStructMemoryRequirements(MemoryRequirements& retVal, rt::IAccelStruct* as) noexcept override { retVal = getAccelStructMemoryRequirements(as); return retVal; }
        rt::cluster::OperationSizeInfo getClusterOperationSizeInfo(const rt::cluster::OperationParams& params);
        rt::cluster::OperationSizeInfo& getClusterOperationSizeInfo(rt::cluster::OperationSizeInfo& retVal, const rt::cluster::OperationParams& params) noexcept override { retVal = getClusterOperationSizeInfo(params); return retVal; }

        bool bindAccelStructMemory(rt::IAccelStruct* as, IHeap* heap, uint64_t offset) noexcept override;

        nvrhi::CommandListHandle createCommandList(const CommandListParameters& params = CommandListParameters());
        FRESULT createCommandList(const CommandListParameters& params, nvrhi::ICommandList** ppCommandList) noexcept override { return utils::ReturnObject(ppCommandList, [&] { return createCommandList(params); }); }
        uint64_t executeCommandLists(nvrhi::ICommandList* const* pCommandLists, size_t numCommandLists, CommandQueue executionQueue = CommandQueue::Graphics) noexcept override;
        void queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue, uint64_t instance) noexcept override;
        bool waitForIdle() noexcept override;
        CommandListLifetimeTrackerHandle createCommandListLifetimeTracker(CommandQueue executionQueue);
        FRESULT createCommandListLifetimeTracker(CommandQueue executionQueue, ICommandListLifetimeTracker** ppTracker) noexcept override { return utils::ReturnObject(ppTracker, [&] { return createCommandListLifetimeTracker(executionQueue); }); }
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
        IMessageCallback* getMessageCallback() noexcept override { return m_Context.messageCallback; }
        bool isAftermathEnabled() noexcept override { return m_AftermathEnabled; }
        IAftermathCrashDumpHelper* getAftermathCrashDumpHelper() noexcept override { return m_AftermathEnabled ? m_AftermathCrashDumpHelper.Get() : nullptr; }
        // The concrete helper, for the command lists' marker trackers.
        AftermathCrashDumpHelper& getAftermathCrashDumpHelperImpl() { return *m_AftermathCrashDumpHelper; }

        // d3d12::IDevice implementation

        RootSignatureHandle buildRootSignature(const static_vector<BindingLayoutHandle, c_MaxBindingLayouts>& pipelineLayouts, bool allowInputLayout, bool isLocal, const D3D12_ROOT_PARAMETER1* pCustomParameters = nullptr, uint32_t numCustomParameters = 0);
        FRESULT buildRootSignature(const static_vector<BindingLayoutHandle, c_MaxBindingLayouts>& pipelineLayouts, bool allowInputLayout, bool isLocal, const D3D12_ROOT_PARAMETER1* pCustomParameters, uint32_t numCustomParameters, IRootSignature** ppRootSignature) noexcept override { return utils::ReturnObject(ppRootSignature, [&] { return buildRootSignature(pipelineLayouts, allowInputLayout, isLocal, pCustomParameters, numCustomParameters); }); }
        GraphicsPipelineHandle createHandleForNativeGraphicsPipeline(IRootSignature* rootSignature, ID3D12PipelineState* pipelineState, const GraphicsPipelineDesc& desc, const FramebufferInfo& framebufferInfo);
        FRESULT createHandleForNativeGraphicsPipeline(IRootSignature* rootSignature, ID3D12PipelineState* pipelineState, const GraphicsPipelineDesc& desc, const FramebufferInfo& framebufferInfo, IGraphicsPipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createHandleForNativeGraphicsPipeline(rootSignature, pipelineState, desc, framebufferInfo); }); }
        MeshletPipelineHandle createHandleForNativeMeshletPipeline(IRootSignature* rootSignature, ID3D12PipelineState* pipelineState, const MeshletPipelineDesc& desc, const FramebufferInfo& framebufferInfo);
        FRESULT createHandleForNativeMeshletPipeline(IRootSignature* rootSignature, ID3D12PipelineState* pipelineState, const MeshletPipelineDesc& desc, const FramebufferInfo& framebufferInfo, IMeshletPipeline** ppPipeline) noexcept override { return utils::ReturnObject(ppPipeline, [&] { return createHandleForNativeMeshletPipeline(rootSignature, pipelineState, desc, framebufferInfo); }); }
        IDescriptorHeap* getDescriptorHeap(DescriptorHeapType heapType) noexcept override;

        // Internal interface
        Queue* getQueue(CommandQueue type) { return m_Queues[int(type)].Get(); }

        Context& getContext() { return m_Context; }

        bool setHlslExtensionsUAV(uint32_t slot);

        bool GetAccelStructPreBuildInfo(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& outPreBuildInfo, const rt::AccelStructDesc& desc) const;

        bool GetNvapiIsInitialized() const { return m_NvapiIsInitialized; }
        bool GetOpacityMicromapSupported() const { return m_OpacityMicromapSupported; }
        bool GetLinearSweptSpheresSupported( ) const { return m_LinearSweptSpheresSupported; }
        bool GetEnhancedBarriersSupported() const { return m_EnhancedBarriersSupported; }

    private:
        Context m_Context;
        DeviceResources m_Resources;

        std::array<MonoPtr<Queue>, (int)CommandQueue::Count> m_Queues;
        HANDLE m_FenceEvent;

        std::mutex m_Mutex;

        bool m_NvapiIsInitialized = false;
        bool m_SinglePassStereoSupported = false;
        bool m_HlslExtensionsSupported = false;
        bool m_FastGeometryShaderSupported = false;
        bool m_RayTracingSupported = false;
        bool m_TraceRayInlineSupported = false;
        bool m_MeshletsSupported = false;
        bool m_VariableRateShadingSupported = false;
        bool m_OpacityMicromapSupported = false;
        bool m_RayTracingClustersSupported = false;
        bool m_LinearSweptSpheresSupported = false;
        bool m_SpheresSupported = false;
        bool m_ShaderExecutionReorderingSupported = false;
        bool m_SamplerFeedbackSupported = false;
        bool m_EnhancedBarriersSupported = false;
        bool m_AftermathEnabled = false;
        bool m_RayTracingValidationEnabled = false;
        void* m_RayTracingValidationCallbackHandle = nullptr;
        bool m_HeapDirectlyIndexedEnabled = false;
#if NVRHI_D3D12_WITH_LINALG
        bool m_LinearAlgebraSupported = false;
#else
        bool m_CoopVecInferencingSupported = false;
        bool m_CoopVecTrainingSupported = false;
#endif
        AutoPtr<AftermathCrashDumpHelper> m_AftermathCrashDumpHelper = MAKE_RC_OBJ_PTR(AftermathCrashDumpHelper);


        D3D12_FEATURE_DATA_D3D12_OPTIONS  m_Options = {};
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 m_Options1 = {};
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 m_Options5 = {};
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 m_Options6 = {};
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 m_Options7 = {};
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 m_Options12 = {};

        AutoPtr<RootSignature> getRootSignature(const static_vector<BindingLayoutHandle, c_MaxBindingLayouts>& pipelineLayouts, bool allowInputLayout);
        AutoPtr<ID3D12PipelineState> createPipelineState(const GraphicsPipelineDesc& desc, RootSignature* pRS, const FramebufferInfo& fbinfo) const;
        AutoPtr<ID3D12PipelineState> createPipelineState(const ComputePipelineDesc& desc, RootSignature* pRS) const;
        AutoPtr<ID3D12PipelineState> createPipelineState(const MeshletPipelineDesc& desc, RootSignature* pRS, const FramebufferInfo& fbinfo) const;
    
    };

} // namespace nvrhi::d3d12
