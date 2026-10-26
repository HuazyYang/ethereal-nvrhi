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
#include <nvrhi/factory.h>
#include <nvrhi/swapchain.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <directx/d3d12.h>
#include <dxgi1_4.h>

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

    // DeviceDesc wraps a native device that the caller created (IGraphicsFactory::createDevice).

    // Creates the native device too (IGraphicsFactory::createDeviceOnAdapter): the factory selects the adapter,
    // creates the ID3D12Device (through the Agility SDK device factory, if the factory loaded one) and its queues.
    // The other fields mean what they mean in DeviceDesc.
    struct DeviceCreationDesc
    {
        IMessageCallback* errorCB = nullptr; // the device keeps a reference to it

        // The adapter index (IGraphicsFactory::getAdapterInfo); -1 selects adapter 0.
        int32_t adapterIndex = -1;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
        // The graphics queue is always created; these add a compute and a copy queue.
        bool enableComputeQueue = false;
        bool enableCopyQueue = false;

        uint32_t renderTargetViewHeapSize = 1024;
        uint32_t depthStencilViewHeapSize = 1024;
        uint32_t shaderResourceViewHeapSize = 16384;
        uint32_t samplerHeapSize = 1024;
        uint32_t maxTimerQueries = 256;

        bool enableHeapDirectlyIndexed = false;
        bool aftermathEnabled = false;
        bool logBufferLifetime = false;
        bool enableRayTracingValidation = false;
        bool enableEnhancedBarriers = true;

        DeviceCreationDesc& setErrorCallback(IMessageCallback* value) { errorCB = value; return *this; }
        DeviceCreationDesc& setAdapterIndex(int32_t value) { adapterIndex = value; return *this; }
        DeviceCreationDesc& setFeatureLevel(D3D_FEATURE_LEVEL value) { featureLevel = value; return *this; }
        DeviceCreationDesc& setEnableComputeQueue(bool value) { enableComputeQueue = value; return *this; }
        DeviceCreationDesc& setEnableCopyQueue(bool value) { enableCopyQueue = value; return *this; }
        DeviceCreationDesc& setEnableHeapDirectlyIndexed(bool value) { enableHeapDirectlyIndexed = value; return *this; }
        DeviceCreationDesc& setAftermathEnabled(bool value) { aftermathEnabled = value; return *this; }
        DeviceCreationDesc& setLogBufferLifetime(bool value) { logBufferLifetime = value; return *this; }
        DeviceCreationDesc& setEnableRayTracingValidation(bool value) { enableRayTracingValidation = value; return *this; }
        DeviceCreationDesc& setEnableEnhancedBarriers(bool value) { enableEnhancedBarriers = value; return *this; }
    };

    static_assert(offsetof(DeviceCreationDesc, renderTargetViewHeapSize) == 20);
    static_assert(offsetof(DeviceCreationDesc, enableHeapDirectlyIndexed) == 40);
    static_assert(sizeof(DeviceCreationDesc) == 48);

    struct GraphicsFactoryDesc
    {
        // Receives the factory's own messages. Optional; the factory keeps a reference to it.
        IMessageCallback* messageCallback = nullptr;

        // Creates a debug DXGI factory and enables the D3D12 debug layer for the devices of this factory, with the
        // InfoQueue filter and break-on-error settings of createDeviceOnAdapter.
        bool enableDebugRuntime = false;
        // GPU-based validation (ID3D12Debug3::SetEnableGPUBasedValidation). Needs enableDebugRuntime.
        bool enableGPUValidation = false;
        // Debug builds: break on D3D12 debug-layer warnings too, not only on errors and corruption.
        bool enableWarningsAsErrors = false;

        // The Agility SDK: the D3D12_SDK_VERSION of the D3D12Core.dll in sdkPath. The factory loads it with
        // ID3D12SDKConfiguration1::CreateDeviceFactory, so the application does not export D3D12SDKVersion.
        // 0 uses the D3D12 runtime of the system. sdkPath is relative to the directory of the executable, or
        // absolute, e.g. ".\\D3D12\\".
        uint32_t sdkVersion = 0;
        string sdkPath;

        // The libraries to load; empty for dxgi.dll / d3d12.dll. Streamline passes sl.interposer.dll.
        string dxgiLibraryName;
        string d3d12LibraryName;

        GraphicsFactoryDesc& setMessageCallback(IMessageCallback* value) { messageCallback = value; return *this; }
        GraphicsFactoryDesc& setEnableDebugRuntime(bool value) { enableDebugRuntime = value; return *this; }
        GraphicsFactoryDesc& setEnableGPUValidation(bool value) { enableGPUValidation = value; return *this; }
        GraphicsFactoryDesc& setEnableWarningsAsErrors(bool value) { enableWarningsAsErrors = value; return *this; }
        GraphicsFactoryDesc& setSdkVersion(uint32_t value) { sdkVersion = value; return *this; }
    };

    static_assert(offsetof(GraphicsFactoryDesc, sdkVersion) == 12);
    static_assert(offsetof(GraphicsFactoryDesc, sdkPath) == 16);
    static_assert(sizeof(GraphicsFactoryDesc) == 16 + 3 * sizeof(string));

    // The D3D12 swap chain: a DXGI flip-model swap chain on the graphics queue, with a frame fence that beginFrame()
    // waits on. getNativeObject returns the IDXGISwapChain3* (ObjectTypes::DXGI_SwapChain) and the ID3D12Fence*
    // (ObjectTypes::D3D12_Fence).
    NVRHI_IID(IGraphicsSwapChain, "c6141ea2-142d-4c52-94e0-6d959779f167")
    struct IGraphicsSwapChain : nvrhi::IGraphicsSwapChain
    {
        NVRHI_DECLARE_UUID_TRAITS(IGraphicsSwapChain)
        // Not AddRef'd.
        [[nodiscard]] virtual IDXGISwapChain3* getDXGISwapChain() noexcept = 0;
        // True if DXGI supports tearing (DXGI_FEATURE_PRESENT_ALLOW_TEARING): presents without vsync use it.
        [[nodiscard]] virtual bool isTearingSupported() noexcept = 0;
    };

    typedef AutoPtr<IGraphicsSwapChain> GraphicsSwapChainHandle;

    NVRHI_IID(IGraphicsFactory, "c508b85c-2ee1-45b2-9136-eb23ffc9d8ee")
    struct IGraphicsFactory : nvrhi::IGraphicsFactory
    {
        NVRHI_DECLARE_UUID_TRAITS(IGraphicsFactory)

        // Each returns FS_OK and a new reference in the last parameter, or an FE_* code and nullptr.

        // Wraps the native device and queues of desc.
        virtual FRESULT createDevice(const DeviceDesc& desc, IDevice** ppDevice) noexcept = 0;
        // Selects the adapter and creates the native device and queues; the NVRHI device owns them.
        virtual FRESULT createDeviceOnAdapter(const DeviceCreationDesc& desc, IDevice** ppDevice) noexcept = 0;
        // A swap chain for 'device' (a D3D12 device, or the validation layer around one) on desc.window, presenting
        // from the device's graphics queue.
        virtual FRESULT createSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device,
            IGraphicsSwapChain** ppSwapChain) noexcept = 0;

        // Reports the live DXGI and D3D objects to the debug output (IDXGIDebug::ReportLiveObjects).
        virtual void reportLiveObjects() noexcept = 0;
    };

    typedef AutoPtr<IGraphicsFactory> GraphicsFactoryHandle;

    // Creates the D3D12 graphics factory: loads dxgi.dll and d3d12.dll, creates the Agility SDK device factory (if
    // desc.sdkVersion is set) and the DXGI factory, and enumerates the adapters. Returns FS_OK and a new reference
    // in *ppFactory, or an FE_* code (FE_UNSUPPORTED if a library or the Agility SDK does not load) and nullptr.
    NVRHI_C_API FRESULT nvrhiD3D12CreateGraphicsFactory(const GraphicsFactoryDesc* pDesc,
        IGraphicsFactory** ppFactory) noexcept;

    inline GraphicsFactoryHandle createGraphicsFactory(const GraphicsFactoryDesc& desc)
    {
        IGraphicsFactory* factory = nullptr;
        nvrhiD3D12CreateGraphicsFactory(&desc, &factory);
        return TakeOver(factory);
    }

    NVRHI_C_API DXGI_FORMAT nvrhiD3D12ConvertFormat(nvrhi::Format format) noexcept;

    inline DXGI_FORMAT convertFormat(nvrhi::Format format) { return nvrhiD3D12ConvertFormat(format); }
}