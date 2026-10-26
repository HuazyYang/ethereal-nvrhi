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

// The D3D11 swap chain (ADR 0014): a DXGI flip-model swap chain. D3D11 exposes buffer 0 only, and presenting needs
// no synchronization with the immediate context. Ported from donut's DeviceManager_DX11.

#include "d3d11-factory.h"

namespace nvrhi::d3d11
{
    NVRHI_CLASS_CLSID(SwapChain, "a7fbc302-1b15-47d3-84f3-7bf8655e7d24")
    class SwapChain final : public ObjectImpl<IGraphicsSwapChain>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(SwapChain)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::d3d11::IGraphicsSwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IGraphicsSwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(SwapChain)
        NVRHI_END_INTERFACE_TABLE()

        SwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device)
            : m_Desc(desc)
            , m_Device(device)
        { }

        ~SwapChain()
        {
            releaseBackBuffer();
            if (m_SwapChain)
                m_SwapChain->SetFullscreenState(false, nullptr);
        }

        bool initialize(IDXGIFactory2* factory)
        {
            const HWND window = static_cast<HWND>(m_Desc.window);
            if (m_Desc.width == 0 || m_Desc.height == 0)
                dxgi::GetClientSize(window, m_Desc.width, m_Desc.height);

            const auto d3dDevice = static_cast<ID3D11Device*>(m_Device->getNativeObject(ObjectTypes::D3D11_Device));
            if (!d3dDevice)
                return false;

            DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
            swapChainDesc.BufferCount = m_Desc.bufferCount;
            swapChainDesc.BufferDesc.Width = m_Desc.width;
            swapChainDesc.BufferDesc.Height = m_Desc.height;
            swapChainDesc.BufferDesc.RefreshRate.Numerator = m_Desc.refreshRate;
            swapChainDesc.BufferDesc.RefreshRate.Denominator = 0;
            swapChainDesc.BufferDesc.Format = dxgi::GetSwapChainFormat(m_Desc.format, convertFormat);
            swapChainDesc.BufferUsage = m_Desc.dxgiUsage;
            swapChainDesc.OutputWindow = window;
            swapChainDesc.SampleDesc.Count = m_Desc.sampleCount;
            swapChainDesc.SampleDesc.Quality = m_Desc.sampleQuality;
            swapChainDesc.Windowed = TRUE; // DXGI always windowed; the application handles fullscreen
            swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            swapChainDesc.Flags = 0;
            m_DxgiFormat = swapChainDesc.BufferDesc.Format;

            if (FAILED(factory->CreateSwapChain(d3dDevice, &swapChainDesc, &m_SwapChain)))
                return false;

            factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

            return createBackBuffer();
        }

        // IRHIObject
        NativeObject getNativeObject(ObjectType objectType) noexcept override
        {
            return objectType == ObjectTypes::DXGI_SwapChain ? NativeObject(m_SwapChain.Get()) : nullptr;
        }

        // nvrhi::IGraphicsSwapChain
        const SwapChainDesc& getDesc() const noexcept override { return m_Desc; }
        uint32_t getBackBufferCount() noexcept override { return 1; }
        ITexture* getBackBuffer(uint32_t index) noexcept override { return index == 0 ? m_BackBuffer.Get() : nullptr; }
        uint32_t getCurrentBackBufferIndex() noexcept override { return 0; }

        FRESULT beginFrame() noexcept override { return FS_OK; }

        FRESULT present() noexcept override
        {
            const HRESULT hr = m_SwapChain->Present(m_Desc.vsync ? 1 : 0, 0);
            return SUCCEEDED(hr) ? FS_OK : FE_GENERIC_ERROR;
        }

        FRESULT resize(uint32_t width, uint32_t height) noexcept override
        {
            try
            {
                releaseBackBuffer();

                if (width == 0 || height == 0)
                    dxgi::GetClientSize(static_cast<HWND>(m_Desc.window), width, height);

                if (FAILED(m_SwapChain->ResizeBuffers(m_Desc.bufferCount, width, height, m_DxgiFormat, 0)))
                    return FE_GENERIC_ERROR;

                m_Desc.width = width;
                m_Desc.height = height;
                return createBackBuffer() ? FS_OK : FE_GENERIC_ERROR;
            }
            catch (const std::bad_alloc&)
            {
                return FE_OUT_OF_MEMORY;
            }
        }

        void setVsyncEnabled(bool enabled) noexcept override { m_Desc.vsync = enabled; }

        // d3d11::IGraphicsSwapChain
        IDXGISwapChain* getDXGISwapChain() noexcept override { return m_SwapChain; }

    private:
        SwapChainDesc m_Desc;
        AutoPtr<nvrhi::IDevice> m_Device;
        AutoPtr<IDXGISwapChain> m_SwapChain;
        DXGI_FORMAT m_DxgiFormat = DXGI_FORMAT_UNKNOWN;
        AutoPtr<ID3D11Texture2D> m_NativeBackBuffer;
        TextureHandle m_BackBuffer;

        bool createBackBuffer()
        {
            if (FAILED(m_SwapChain->GetBuffer(0, IID_PPV_ARGS(&m_NativeBackBuffer))))
                return false;

            TextureDesc textureDesc;
            textureDesc.width = m_Desc.width;
            textureDesc.height = m_Desc.height;
            textureDesc.sampleCount = m_Desc.sampleCount;
            textureDesc.sampleQuality = m_Desc.sampleQuality;
            textureDesc.format = m_Desc.format;
            textureDesc.debugName = "SwapChainBuffer";
            textureDesc.isRenderTarget = true;
            textureDesc.isUAV = false;

            m_Device->createHandleForNativeTexture(ObjectTypes::D3D11_Resource,
                static_cast<ID3D11Resource*>(m_NativeBackBuffer.Get()), textureDesc, &m_BackBuffer);
            return m_BackBuffer != nullptr;
        }

        void releaseBackBuffer()
        {
            m_BackBuffer = nullptr;
            m_NativeBackBuffer = nullptr;
        }
    };

    FRESULT CreateSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device, IDXGIFactory2* factory,
        IGraphicsSwapChain** ppSwapChain)
    {
        AutoPtr<SwapChain> swapChain = MAKE_RC_OBJ_PTR(SwapChain, desc, device);
        if (!swapChain->initialize(factory))
            return FE_GENERIC_ERROR;
        *ppSwapChain = swapChain.Detach();
        return FS_OK;
    }
}
