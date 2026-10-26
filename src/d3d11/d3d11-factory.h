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

#pragma once

// The D3D11 graphics factory and swap chain (ADR 0014). Module-private.

#include <nvrhi/d3d11.h>

#include "../common/dxgi-factory.h"
#include "../common/graphics-factory-base.h"

namespace nvrhi::d3d11
{
    NVRHI_CLASS_CLSID(GraphicsFactory, "8b48fc94-3515-40d8-9eea-f717916bc97c")
    class GraphicsFactory final : public GraphicsFactoryBase<IGraphicsFactory>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(GraphicsFactory)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(GraphicsFactory)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::d3d11::IGraphicsFactory)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IGraphicsFactory)
        NVRHI_IMPLEMENTS_CLASS(GraphicsFactory)
        NVRHI_END_INTERFACE_TABLE()

        explicit GraphicsFactory(const GraphicsFactoryDesc& desc);
        bool initialize(std::string& outError);

        // nvrhi::IGraphicsFactory
        GraphicsAPI getGraphicsAPI() noexcept override { return GraphicsAPI::D3D11; }
        NativeObject getNativeObject(ObjectType objectType) noexcept override;

        // d3d11::IGraphicsFactory
        FRESULT createDevice(const DeviceDesc& desc, nvrhi::IDevice** ppDevice) noexcept override;
        FRESULT createDeviceOnAdapter(const DeviceCreationDesc& desc, nvrhi::IDevice** ppDevice) noexcept override;
        FRESULT createSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device,
            IGraphicsSwapChain** ppSwapChain) noexcept override;
        void reportLiveObjects() noexcept override;

    private:
        GraphicsFactoryDesc m_Desc;
        dxgi::Factory m_Dxgi;
        PFN_D3D11_CREATE_DEVICE m_CreateDevice = nullptr;
    };

    // d3d11-swapchain.cpp
    FRESULT CreateSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device, IDXGIFactory2* factory,
        IGraphicsSwapChain** ppSwapChain);
}
