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

#include <d3d11.h>

namespace nvrhi::ObjectTypes
{
    constexpr ObjectType Nvrhi_D3D11_Device = 0x00010101;
};

namespace nvrhi::d3d11
{
    // Wraps a native device that the caller created (IGraphicsFactory::createDevice).
    struct DeviceDesc
    {
        IMessageCallback* messageCallback = nullptr; // the device keeps a reference to it
        ID3D11DeviceContext* context = nullptr;
        bool aftermathEnabled = false;
    };

    // Creates the native device too (IGraphicsFactory::createDeviceOnAdapter): the factory selects the adapter and
    // creates the ID3D11Device and its immediate context.
    struct DeviceCreationDesc
    {
        IMessageCallback* messageCallback = nullptr; // the device keeps a reference to it
        // The adapter index (IGraphicsFactory::getAdapterInfo); -1 selects adapter 0.
        int32_t adapterIndex = -1;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
        bool aftermathEnabled = false;

        DeviceCreationDesc& setMessageCallback(IMessageCallback* value) { messageCallback = value; return *this; }
        DeviceCreationDesc& setAdapterIndex(int32_t value) { adapterIndex = value; return *this; }
        DeviceCreationDesc& setFeatureLevel(D3D_FEATURE_LEVEL value) { featureLevel = value; return *this; }
        DeviceCreationDesc& setAftermathEnabled(bool value) { aftermathEnabled = value; return *this; }
    };

    static_assert(sizeof(DeviceCreationDesc) == 24);

    struct GraphicsFactoryDesc
    {
        // Receives the factory's own messages. Optional; the factory keeps a reference to it.
        IMessageCallback* messageCallback = nullptr;
        // Creates a debug DXGI factory and D3D11 devices with D3D11_CREATE_DEVICE_DEBUG.
        bool enableDebugRuntime = false;
        // The libraries to load; empty for dxgi.dll / d3d11.dll. Streamline passes sl.interposer.dll.
        string dxgiLibraryName;
        string d3d11LibraryName;

        GraphicsFactoryDesc& setMessageCallback(IMessageCallback* value) { messageCallback = value; return *this; }
        GraphicsFactoryDesc& setEnableDebugRuntime(bool value) { enableDebugRuntime = value; return *this; }
    };

    static_assert(offsetof(GraphicsFactoryDesc, dxgiLibraryName) == 16);
    static_assert(sizeof(GraphicsFactoryDesc) == 16 + 2 * sizeof(string));

    // The D3D11 swap chain: a DXGI flip-model swap chain. D3D11 exposes one back buffer (index 0), and beginFrame()
    // does nothing. getNativeObject(ObjectTypes::DXGI_SwapChain) returns the IDXGISwapChain*.
    NVRHI_IID(IGraphicsSwapChain, "877ec110-8a9d-444e-bf33-d99745a2cd9c")
    struct IGraphicsSwapChain : nvrhi::IGraphicsSwapChain
    {
        NVRHI_DECLARE_UUID_TRAITS(IGraphicsSwapChain)
        // Not AddRef'd.
        [[nodiscard]] virtual IDXGISwapChain* getDXGISwapChain() noexcept = 0;
    };

    typedef AutoPtr<IGraphicsSwapChain> GraphicsSwapChainHandle;

    NVRHI_IID(IGraphicsFactory, "71df35e2-ebfe-4a6a-bf07-df7cc3706560")
    struct IGraphicsFactory : nvrhi::IGraphicsFactory
    {
        NVRHI_DECLARE_UUID_TRAITS(IGraphicsFactory)

        // Each returns FS_OK and a new reference in the last parameter, or an FE_* code and nullptr.

        // Wraps the native device of desc.context.
        virtual FRESULT createDevice(const DeviceDesc& desc, nvrhi::IDevice** ppDevice) noexcept = 0;
        // Selects the adapter and creates the native device and context; the NVRHI device owns them.
        virtual FRESULT createDeviceOnAdapter(const DeviceCreationDesc& desc, nvrhi::IDevice** ppDevice) noexcept = 0;
        // A swap chain for 'device' (a D3D11 device, or the validation layer around one) on desc.window.
        virtual FRESULT createSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device,
            IGraphicsSwapChain** ppSwapChain) noexcept = 0;

        // Reports the live DXGI and D3D objects to the debug output (IDXGIDebug::ReportLiveObjects).
        virtual void reportLiveObjects() noexcept = 0;
    };

    typedef AutoPtr<IGraphicsFactory> GraphicsFactoryHandle;

    // Creates the D3D11 graphics factory: loads dxgi.dll and d3d11.dll, creates the DXGI factory and enumerates the
    // adapters. Returns FS_OK and a new reference in *ppFactory, or an FE_* code (FE_UNSUPPORTED if a library does
    // not load) and nullptr.
    NVRHI_C_API FRESULT nvrhiD3D11CreateGraphicsFactory(const GraphicsFactoryDesc* pDesc,
        IGraphicsFactory** ppFactory) noexcept;

    inline GraphicsFactoryHandle createGraphicsFactory(const GraphicsFactoryDesc& desc)
    {
        IGraphicsFactory* factory = nullptr;
        nvrhiD3D11CreateGraphicsFactory(&desc, &factory);
        return TakeOver(factory);
    }

    NVRHI_C_API DXGI_FORMAT nvrhiD3D11ConvertFormat(nvrhi::Format format) noexcept;

    inline DXGI_FORMAT convertFormat(nvrhi::Format format) { return nvrhiD3D11ConvertFormat(format); }
}
