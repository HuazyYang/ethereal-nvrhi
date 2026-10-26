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

// The D3D12 graphics factory (ADR 0014): DXGI and d3d12.dll loaded at run time, the Agility SDK through
// ID3D12SDKConfiguration1::CreateDeviceFactory, the D3D12 devices and their queues. The device creation is ported
// from donut's DeviceManager_DX12.

#include "d3d12-factory.h"

#include <directx/d3d12sdklayers.h>

#include <new>

namespace nvrhi::d3d12
{
    namespace
    {
        std::string HResultText(const char* what, HRESULT hr)
        {
            char text[160];
            snprintf(text, sizeof(text), "%s failed, HRESULT = 0x%08lx", what, hr);
            return text;
        }
    }

    GraphicsFactory::GraphicsFactory(const GraphicsFactoryDesc& desc)
        : GraphicsFactoryBase(desc.messageCallback)
        , m_Desc(desc)
    { }

    bool GraphicsFactory::initialize(std::string& outError)
    {
        if (!m_Dxgi.initialize(m_Desc.dxgiLibraryName, m_Desc.enableDebugRuntime, outError))
            return false;

        const HMODULE d3d12 = dxgi::LoadRuntimeLibrary(m_Desc.d3d12LibraryName, "d3d12.dll");
        if (!d3d12)
        {
            outError = std::string("cannot load ") + (m_Desc.d3d12LibraryName.empty() ? "d3d12.dll" : m_Desc.d3d12LibraryName.c_str());
            return false;
        }

        m_CreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
            reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")));
        if (!m_CreateDevice)
        {
            outError = "D3D12CreateDevice is not available";
            return false;
        }

        if (m_Desc.sdkVersion != 0)
        {
            const auto getInterface = reinterpret_cast<PFN_D3D12_GET_INTERFACE>(
                reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12GetInterface")));
            if (!getInterface)
            {
                outError = "the Agility SDK needs D3D12GetInterface, which this D3D12 runtime does not have";
                return false;
            }

            AutoPtr<ID3D12SDKConfiguration1> configuration;
            HRESULT hr = getInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&configuration));
            if (FAILED(hr))
            {
                outError = HResultText("D3D12GetInterface(ID3D12SDKConfiguration1)", hr);
                return false;
            }

            const char* sdkPath = m_Desc.sdkPath.empty() ? ".\\D3D12\\" : m_Desc.sdkPath.c_str();
            hr = configuration->CreateDeviceFactory(m_Desc.sdkVersion, sdkPath, IID_PPV_ARGS(&m_DeviceFactory));
            if (FAILED(hr))
            {
                outError = HResultText("ID3D12SDKConfiguration1::CreateDeviceFactory", hr) + " (Agility SDK " +
                    std::to_string(m_Desc.sdkVersion) + " in \"" + sdkPath + "\")";
                return false;
            }

            // Like D3D12CreateDevice: a second device on the same adapter is the existing one.
            m_DeviceFactory->SetFlags(D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_EXISTING_DEVICE);
        }

        if (!enableDebugLayer(outError))
            return false;

        m_Adapters = m_Dxgi.getAdapterInfos();
        return true;
    }

    bool GraphicsFactory::enableDebugLayer(std::string& outError)
    {
        (void)outError;

        if (!m_Desc.enableDebugRuntime && !m_Desc.enableGPUValidation)
            return true;

        AutoPtr<ID3D12Debug> debug;
        if (m_DeviceFactory)
        {
            m_DeviceFactory->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&debug));
        }
        else
        {
            const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
            const auto getDebugInterface = d3d12 ? reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(
                reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12GetDebugInterface"))) : nullptr;
            if (getDebugInterface)
                getDebugInterface(IID_PPV_ARGS(&debug));
        }

        if (m_Desc.enableDebugRuntime)
        {
            if (debug)
                debug->EnableDebugLayer();
            else
                warning("Cannot enable the D3D12 debug runtime, ID3D12Debug is not available.");
        }

        if (m_Desc.enableGPUValidation)
        {
            AutoPtr<ID3D12Debug3> debug3;
            if (debug)
                debug->QueryInterface(IID_PPV_ARGS(&debug3));
            if (debug3)
                debug3->SetEnableGPUBasedValidation(true);
            else
                warning("Cannot enable GPU-based validation, ID3D12Debug3 is not available.");
        }

        return true;
    }

    void GraphicsFactory::configureInfoQueue(ID3D12Device* device) const
    {
        AutoPtr<ID3D12InfoQueue> infoQueue;
        device->QueryInterface(IID_PPV_ARGS(&infoQueue));
        if (!infoQueue)
            return;

#ifdef _DEBUG
        if (m_Desc.enableWarningsAsErrors)
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
#endif

        D3D12_MESSAGE_ID disableMessageIDs[] = {
            D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_COMMAND_LIST_STATIC_DESCRIPTOR_RESOURCE_DIMENSION_MISMATCH, // descriptor validation doesn't understand acceleration structures
            D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED, // NGX currently generates benign resource creation warnings
            D3D12_MESSAGE_ID_NON_OPTIMAL_BARRIER_ONLY_EXECUTE_COMMAND_LISTS, // NVRHI cannot track if a command list only contains barriers when the barriers are submitted
            D3D12_MESSAGE_ID_NON_RETAIL_SHADER_MODEL_WONT_VALIDATE,
        };

        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.pIDList = disableMessageIDs;
        filter.DenyList.NumIDs = UINT(std::size(disableMessageIDs));
        infoQueue->AddStorageFilterEntries(&filter);
    }

    NativeObject GraphicsFactory::getNativeObject(ObjectType objectType) noexcept
    {
        switch (objectType)
        {
        case ObjectTypes::DXGI_Factory:
            return m_Dxgi.get();
        case ObjectTypes::D3D12_DeviceFactory:
            return m_DeviceFactory.Get();
        default:
            return nullptr;
        }
    }

    FRESULT GraphicsFactory::createDevice(const DeviceDesc& desc, IDevice** ppDevice) noexcept
    {
        if (!BeginCreate(ppDevice))
            return FE_INVALID_ARGS;
        if (!desc.pDevice || !desc.pGraphicsCommandQueue)
            return FE_INVALID_ARGS;
        return d3d12::CreateDevice(desc, ppDevice);
    }

    FRESULT GraphicsFactory::createDeviceOnAdapter(const DeviceCreationDesc& desc, IDevice** ppDevice) noexcept
    {
        if (!BeginCreate(ppDevice))
            return FE_INVALID_ARGS;

        try
        {
            std::string errorText;
            IDXGIAdapter1* adapter = m_Dxgi.selectAdapter(desc.adapterIndex, errorText);
            if (!adapter)
            {
                error(errorText);
                return FE_NOT_FOUND;
            }

            AutoPtr<ID3D12Device> device;
            const HRESULT hr = m_DeviceFactory
                ? m_DeviceFactory->CreateDevice(adapter, desc.featureLevel, IID_PPV_ARGS(&device))
                : m_CreateDevice(adapter, desc.featureLevel, IID_PPV_ARGS(&device));
            if (FAILED(hr))
            {
                error(HResultText("D3D12CreateDevice", hr));
                return FE_GENERIC_ERROR;
            }

            if (m_Desc.enableDebugRuntime)
                configureInfoQueue(device);

            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            queueDesc.NodeMask = 1;

            AutoPtr<ID3D12CommandQueue> graphicsQueue;
            AutoPtr<ID3D12CommandQueue> computeQueue;
            AutoPtr<ID3D12CommandQueue> copyQueue;
            if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&graphicsQueue))))
                return FE_GENERIC_ERROR;
            graphicsQueue->SetName(L"Graphics Queue");

            if (desc.enableComputeQueue)
            {
                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
                if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&computeQueue))))
                    return FE_GENERIC_ERROR;
                computeQueue->SetName(L"Compute Queue");
            }

            if (desc.enableCopyQueue)
            {
                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
                if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&copyQueue))))
                    return FE_GENERIC_ERROR;
                copyQueue->SetName(L"Copy Queue");
            }

            DeviceDesc deviceDesc;
            deviceDesc.errorCB = desc.errorCB;
            deviceDesc.pDevice = device;
            deviceDesc.pGraphicsCommandQueue = graphicsQueue;
            deviceDesc.pComputeCommandQueue = computeQueue;
            deviceDesc.pCopyCommandQueue = copyQueue;
            deviceDesc.renderTargetViewHeapSize = desc.renderTargetViewHeapSize;
            deviceDesc.depthStencilViewHeapSize = desc.depthStencilViewHeapSize;
            deviceDesc.shaderResourceViewHeapSize = desc.shaderResourceViewHeapSize;
            deviceDesc.samplerHeapSize = desc.samplerHeapSize;
            deviceDesc.maxTimerQueries = desc.maxTimerQueries;
            deviceDesc.enableHeapDirectlyIndexed = desc.enableHeapDirectlyIndexed;
            deviceDesc.aftermathEnabled = desc.aftermathEnabled;
            deviceDesc.logBufferLifetime = desc.logBufferLifetime;
            deviceDesc.enableRayTracingValidation = desc.enableRayTracingValidation;
            deviceDesc.enableEnhancedBarriers = desc.enableEnhancedBarriers;
            // The NVRHI device keeps references to the device and the queues.
            return d3d12::CreateDevice(deviceDesc, ppDevice);
        }
        catch (const std::bad_alloc&)
        {
            return FE_OUT_OF_MEMORY;
        }
    }

    FRESULT GraphicsFactory::createSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device,
        IGraphicsSwapChain** ppSwapChain) noexcept
    {
        if (!BeginCreate(ppSwapChain))
            return FE_INVALID_ARGS;
        if (!device || !desc.window)
            return FE_INVALID_ARGS;

        try
        {
            const FRESULT result = CreateSwapChain(desc, device, m_Dxgi.get(), m_Dxgi.isTearingSupported(), ppSwapChain);
            if (NVRHI_FAILED(result))
                error("Failed to create the D3D12 swap chain");
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return FE_OUT_OF_MEMORY;
        }
    }

    void GraphicsFactory::reportLiveObjects() noexcept
    {
        m_Dxgi.reportLiveObjects();
    }

    FRESULT nvrhiD3D12CreateGraphicsFactory(const GraphicsFactoryDesc* pDesc, IGraphicsFactory** ppFactory) noexcept
    {
        if (!BeginCreate(ppFactory))
            return FE_INVALID_ARGS;
        if (!pDesc)
            return FE_INVALID_ARGS;

        try
        {
            AutoPtr<GraphicsFactory> factory = MAKE_RC_OBJ_PTR(GraphicsFactory, *pDesc);
            std::string errorText;
            if (!factory->initialize(errorText))
            {
                if (pDesc->messageCallback)
                    pDesc->messageCallback->message(MessageSeverity::Error, ("D3D12 graphics factory: " + errorText).c_str());
                return FE_UNSUPPORTED;
            }
            *ppFactory = factory.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc&)
        {
            return FE_OUT_OF_MEMORY;
        }
    }
}
