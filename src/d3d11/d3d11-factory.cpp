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

// The D3D11 graphics factory (ADR 0014): DXGI and d3d11.dll loaded at run time, the D3D11 devices.

#include "d3d11-factory.h"

#include <new>

namespace nvrhi::d3d11
{
    GraphicsFactory::GraphicsFactory(const GraphicsFactoryDesc& desc)
        : GraphicsFactoryBase(desc.messageCallback)
        , m_Desc(desc)
    { }

    bool GraphicsFactory::initialize(std::string& outError)
    {
        if (!m_Dxgi.initialize(m_Desc.dxgiLibraryName, m_Desc.enableDebugRuntime, outError))
            return false;

        const HMODULE d3d11 = dxgi::LoadRuntimeLibrary(m_Desc.d3d11LibraryName, "d3d11.dll");
        if (!d3d11)
        {
            outError = std::string("cannot load ") + (m_Desc.d3d11LibraryName.empty() ? "d3d11.dll" : m_Desc.d3d11LibraryName.c_str());
            return false;
        }

        m_CreateDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
            reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")));
        if (!m_CreateDevice)
        {
            outError = "D3D11CreateDevice is not available";
            return false;
        }

        m_Adapters = m_Dxgi.getAdapterInfos();
        return true;
    }

    NativeObject GraphicsFactory::getNativeObject(ObjectType objectType) noexcept
    {
        switch (objectType)
        {
        case ObjectTypes::DXGI_Factory:
            return m_Dxgi.get();
        default:
            return nullptr;
        }
    }

    FRESULT GraphicsFactory::createDevice(const DeviceDesc& desc, nvrhi::IDevice** ppDevice) noexcept
    {
        if (!BeginCreate(ppDevice))
            return FE_INVALID_ARGS;
        if (!desc.context)
            return FE_INVALID_ARGS;
        return d3d11::CreateDevice(desc, ppDevice);
    }

    FRESULT GraphicsFactory::createDeviceOnAdapter(const DeviceCreationDesc& desc, nvrhi::IDevice** ppDevice) noexcept
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

            UINT createFlags = 0;
            if (m_Desc.enableDebugRuntime)
                createFlags |= D3D11_CREATE_DEVICE_DEBUG;

            const D3D_FEATURE_LEVEL featureLevel = desc.featureLevel;
            AutoPtr<ID3D11Device> device;
            AutoPtr<ID3D11DeviceContext> context;
            const HRESULT hr = m_CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, createFlags, &featureLevel, 1,
                D3D11_SDK_VERSION, &device, nullptr, &context);
            if (FAILED(hr))
            {
                char text[96];
                snprintf(text, sizeof(text), "D3D11CreateDevice failed, HRESULT = 0x%08lx", hr);
                error(text);
                return FE_GENERIC_ERROR;
            }

            DeviceDesc deviceDesc;
            deviceDesc.messageCallback = desc.messageCallback;
            deviceDesc.context = context;
            deviceDesc.aftermathEnabled = desc.aftermathEnabled;
            // The NVRHI device keeps references to the context and the device.
            return d3d11::CreateDevice(deviceDesc, ppDevice);
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
            const FRESULT result = CreateSwapChain(desc, device, m_Dxgi.get(), ppSwapChain);
            if (NVRHI_FAILED(result))
                error("Failed to create the D3D11 swap chain");
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

    FRESULT nvrhiD3D11CreateGraphicsFactory(const GraphicsFactoryDesc* pDesc, IGraphicsFactory** ppFactory) noexcept
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
                    pDesc->messageCallback->message(MessageSeverity::Error, ("D3D11 graphics factory: " + errorText).c_str());
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
