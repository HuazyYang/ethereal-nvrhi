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

#include <nvrhi/nvrhi.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <directx/d3d12.h>

namespace nvrhi
{
    namespace ObjectTypes
    {
        constexpr ObjectType Nvrhi_D3D12_Device         = 0x00020101;
        constexpr ObjectType Nvrhi_D3D12_CommandList    = 0x00020102;
    };
}

namespace nvrhi::d3d12
{
    NVRHI_IID(IRootSignature, "0af8f668-e2f0-41ed-bb33-50a9d2cb2d87")
    struct IRootSignature : IRHIObject
    {
        NVRHI_DECLARE_UUID_TRAITS(IRootSignature)
    };

    typedef AutoPtr<IRootSignature> RootSignatureHandle;

    NVRHI_IID(ICommandList, "3d064428-ad83-495d-b264-056c7900a1f5")
    struct ICommandList : nvrhi::ICommandList
    {
        NVRHI_DECLARE_UUID_TRAITS(ICommandList)
        virtual bool allocateUploadBuffer(size_t size, _Out_opt_ void** pCpuAddress, _Out_opt_ D3D12_GPU_VIRTUAL_ADDRESS* pGpuAddress) noexcept = 0;
        virtual bool commitDescriptorHeaps() noexcept = 0;
        virtual D3D12_GPU_VIRTUAL_ADDRESS getBufferGpuVA(IBuffer* buffer) noexcept = 0;

        virtual void updateGraphicsVolatileBuffers() noexcept = 0;
        virtual void updateComputeVolatileBuffers() noexcept = 0;
    };

    typedef AutoPtr<ICommandList> CommandListHandle;

    typedef uint32_t DescriptorIndex;

    // A descriptor heap of the device (see IDevice::getDescriptorHeap). The descriptor handles are returned in
    // retVal (D3D12_*_DESCRIPTOR_HANDLE are structs, which MSVC and MinGW return differently by value).
    NVRHI_IID(IDescriptorHeap, "6dfe4c58-058c-489a-90fe-a0f61d650736")
    struct IDescriptorHeap : IRHIObject
    {
        NVRHI_DECLARE_UUID_TRAITS(IDescriptorHeap)
        virtual DescriptorIndex allocateDescriptors(uint32_t count) noexcept = 0;
        virtual DescriptorIndex allocateDescriptor() noexcept = 0;
        virtual void releaseDescriptors(DescriptorIndex baseIndex, uint32_t count) noexcept = 0;
        virtual void releaseDescriptor(DescriptorIndex index) noexcept = 0;
        virtual D3D12_CPU_DESCRIPTOR_HANDLE& getCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE& retVal, DescriptorIndex index) noexcept = 0;
        virtual D3D12_CPU_DESCRIPTOR_HANDLE& getCpuHandleShaderVisible(D3D12_CPU_DESCRIPTOR_HANDLE& retVal, DescriptorIndex index) noexcept = 0;
        virtual D3D12_GPU_DESCRIPTOR_HANDLE& getGpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE& retVal, DescriptorIndex index) noexcept = 0;
        [[nodiscard]] virtual ID3D12DescriptorHeap* getHeap() const noexcept = 0;
        [[nodiscard]] virtual ID3D12DescriptorHeap* getShaderVisibleHeap() const noexcept = 0;
    };

    enum class DescriptorHeapType
    {
        RenderTargetView,
        DepthStencilView,
        ShaderResourceView,
        Sampler
    };

    NVRHI_IID(IDevice, "5f3ccc09-1a65-4dc5-a5bf-bac3ed912f6c")
    struct IDevice : nvrhi::IDevice
    {
        NVRHI_DECLARE_UUID_TRAITS(IDevice)
        // D3D12-specific methods
        // These return FS_OK and a new reference in the last parameter, or an FE_* code and nullptr.
        virtual FRESULT buildRootSignature(const static_vector<BindingLayoutHandle, c_MaxBindingLayouts>& pipelineLayouts, bool allowInputLayout, bool isLocal, _In_reads_opt_(numCustomParameters) const D3D12_ROOT_PARAMETER1* pCustomParameters, uint32_t numCustomParameters, IRootSignature** ppRootSignature) noexcept = 0;
        virtual FRESULT createHandleForNativeGraphicsPipeline(IRootSignature* rootSignature, ID3D12PipelineState* pipelineState, const GraphicsPipelineDesc& desc, const FramebufferInfo& framebufferInfo, IGraphicsPipeline** ppPipeline) noexcept = 0;
        virtual FRESULT createHandleForNativeMeshletPipeline(IRootSignature* rootSignature, ID3D12PipelineState* pipelineState, const MeshletPipelineDesc& desc, const FramebufferInfo& framebufferInfo, IMeshletPipeline** ppPipeline) noexcept = 0;
        // Returns the heap, not AddRef'd: it lives as long as the device.
        [[nodiscard]] virtual IDescriptorHeap* getDescriptorHeap(DescriptorHeapType heapType) noexcept = 0;
    };

    typedef AutoPtr<IDevice> DeviceHandle;

    struct DeviceDesc
    {
        IMessageCallback* errorCB = nullptr; // the device keeps a reference to it
        ID3D12Device* pDevice = nullptr;
        ID3D12CommandQueue* pGraphicsCommandQueue = nullptr;
        ID3D12CommandQueue* pComputeCommandQueue = nullptr;
        ID3D12CommandQueue* pCopyCommandQueue = nullptr;

        uint32_t renderTargetViewHeapSize = 1024;
        uint32_t depthStencilViewHeapSize = 1024;
        uint32_t shaderResourceViewHeapSize = 16384;
        uint32_t samplerHeapSize = 1024;
        uint32_t maxTimerQueries = 256;

        // If enabled and the device has the capability,
        // create RootSignatures with D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED 
        // and D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED
        bool enableHeapDirectlyIndexed = false;

        bool aftermathEnabled = false;

        // Enable logging the buffer lifetime to IMessageCallback
        // Useful for debugging resource lifetimes
        bool logBufferLifetime = false;

        // Enable NVAPI ray tracing validation (NvAPI_D3D12_EnableRaytracingValidation).
        // Requires NVAPI. The nvrhi::Device constructor sets the NV_ALLOW_RAYTRACING_VALIDATION=1
        // environment variable automatically, so callers don't need to export it beforehand.
        // Validation must be enabled before any other ray tracing call (including capability
        // queries).
        bool enableRayTracingValidation = false;

        // Enable D3D12 Enhanced Barriers, if supported.
        // Use device->queryFeatureSupport(Feature::EnhancedBarriers) to query the actual support.
        bool enableEnhancedBarriers = true;
    };

    // Creates a D3D12 device. Returns FS_OK and a new reference in *ppDevice, or an FE_* code and nullptr.
    NVRHI_C_API FRESULT nvrhiD3D12CreateDevice(const DeviceDesc* pDesc, IDevice** ppDevice) noexcept;

    NVRHI_C_API DXGI_FORMAT nvrhiD3D12ConvertFormat(nvrhi::Format format) noexcept;

    inline DeviceHandle createDevice(const DeviceDesc& desc)
    {
        IDevice* device = nullptr;
        nvrhiD3D12CreateDevice(&desc, &device);
        return TakeOver(device);
    }

    inline DXGI_FORMAT convertFormat(nvrhi::Format format) { return nvrhiD3D12ConvertFormat(format); }
}