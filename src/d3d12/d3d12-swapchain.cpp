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

// The D3D12 swap chain (ADR 0014): a DXGI flip-model swap chain on the graphics queue. present() signals a frame
// fence after the present, with an event per back buffer; beginFrame() waits for the event of the back buffer it
// is about to render into. Ported from donut's DeviceManager_DX12.

#include "d3d12-factory.h"

#include <vector>

namespace nvrhi::d3d12
{
    NVRHI_CLASS_CLSID(SwapChain, "4c7879b1-4d62-4b17-826b-39b501be9659")
    class SwapChain final : public ObjectImpl<IGraphicsSwapChain>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(SwapChain)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::d3d12::IGraphicsSwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IGraphicsSwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(SwapChain)
        NVRHI_END_INTERFACE_TABLE()

        SwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device, bool tearingSupported)
            : m_Desc(desc)
            , m_Device(device)
            , m_TearingSupported(tearingSupported)
        { }

        ~SwapChain()
        {
            releaseRenderTargets();

            for (HANDLE fenceEvent : m_FrameFenceEvents)
            {
                WaitForSingleObject(fenceEvent, INFINITE);
                CloseHandle(fenceEvent);
            }
            m_FrameFenceEvents.clear();

            if (m_SwapChain)
                m_SwapChain->SetFullscreenState(false, nullptr);
        }

        bool initialize(IDXGIFactory2* factory)
        {
            const HWND window = static_cast<HWND>(m_Desc.window);
            if (m_Desc.width == 0 || m_Desc.height == 0)
                dxgi::GetClientSize(window, m_Desc.width, m_Desc.height);

            const auto d3dDevice = static_cast<ID3D12Device*>(m_Device->getNativeObject(ObjectTypes::D3D12_Device));
            m_GraphicsQueue = static_cast<ID3D12CommandQueue*>(
                m_Device->getNativeQueue(ObjectTypes::D3D12_CommandQueue, CommandQueue::Graphics));
            if (!d3dDevice || !m_GraphicsQueue)
                return false;

            DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
            swapChainDesc.Width = m_Desc.width;
            swapChainDesc.Height = m_Desc.height;
            swapChainDesc.SampleDesc.Count = m_Desc.sampleCount;
            swapChainDesc.SampleDesc.Quality = 0;
            swapChainDesc.BufferUsage = m_Desc.dxgiUsage;
            swapChainDesc.BufferCount = m_Desc.bufferCount;
            swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            swapChainDesc.Flags = m_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            swapChainDesc.Format = dxgi::GetSwapChainFormat(m_Desc.format, convertFormat);
            m_DxgiFormat = swapChainDesc.Format;
            m_SwapChainFlags = swapChainDesc.Flags;

            DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc = {};
            fullscreenDesc.RefreshRate.Numerator = m_Desc.refreshRate;
            fullscreenDesc.RefreshRate.Denominator = 1;
            fullscreenDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
            fullscreenDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
            fullscreenDesc.Windowed = TRUE; // DXGI always windowed; the application handles fullscreen

            AutoPtr<IDXGISwapChain1> swapChain1;
            if (FAILED(factory->CreateSwapChainForHwnd(m_GraphicsQueue, window, &swapChainDesc, &fullscreenDesc, nullptr, &swapChain1)))
                return false;
            if (FAILED(swapChain1->QueryInterface(IID_PPV_ARGS(&m_SwapChain))))
                return false;

            factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

            if (!createRenderTargets())
                return false;

            if (FAILED(d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_FrameFence))))
                return false;

            for (uint32_t bufferIndex = 0; bufferIndex < m_Desc.bufferCount; bufferIndex++)
                m_FrameFenceEvents.push_back(CreateEvent(nullptr, false, true, nullptr));

            return true;
        }

        // IRHIObject
        NativeObject getNativeObject(ObjectType objectType) noexcept override
        {
            switch (objectType)
            {
            case ObjectTypes::DXGI_SwapChain:
                return m_SwapChain.Get();
            case ObjectTypes::D3D12_Fence:
                return m_FrameFence.Get();
            default:
                return nullptr;
            }
        }

        // nvrhi::IGraphicsSwapChain
        const SwapChainDesc& getDesc() const noexcept override { return m_Desc; }
        uint32_t getBackBufferCount() noexcept override { return uint32_t(m_BackBuffers.size()); }

        ITexture* getBackBuffer(uint32_t index) noexcept override
        {
            return index < m_BackBuffers.size() ? m_BackBuffers[index].Get() : nullptr;
        }

        uint32_t getCurrentBackBufferIndex() noexcept override { return m_SwapChain->GetCurrentBackBufferIndex(); }

        FRESULT beginFrame() noexcept override
        {
            const uint32_t bufferIndex = m_SwapChain->GetCurrentBackBufferIndex();
            if (bufferIndex < m_FrameFenceEvents.size())
                WaitForSingleObject(m_FrameFenceEvents[bufferIndex], INFINITE);
            return FS_OK;
        }

        FRESULT present() noexcept override
        {
            const uint32_t bufferIndex = m_SwapChain->GetCurrentBackBufferIndex();

            UINT presentFlags = 0;
            if (!m_Desc.vsync && m_TearingSupported)
                presentFlags |= DXGI_PRESENT_ALLOW_TEARING;

            const HRESULT result = m_SwapChain->Present(m_Desc.vsync ? 1 : 0, presentFlags);

            m_FrameFence->SetEventOnCompletion(m_FrameCount, m_FrameFenceEvents[bufferIndex]);
            m_GraphicsQueue->Signal(m_FrameFence, m_FrameCount);
            m_FrameCount++;

            return SUCCEEDED(result) ? FS_OK : FE_GENERIC_ERROR;
        }

        FRESULT resize(uint32_t width, uint32_t height) noexcept override
        {
            try
            {
                releaseRenderTargets();

                if (width == 0 || height == 0)
                    dxgi::GetClientSize(static_cast<HWND>(m_Desc.window), width, height);

                if (FAILED(m_SwapChain->ResizeBuffers(m_Desc.bufferCount, width, height, m_DxgiFormat, m_SwapChainFlags)))
                    return FE_GENERIC_ERROR;

                m_Desc.width = width;
                m_Desc.height = height;
                return createRenderTargets() ? FS_OK : FE_GENERIC_ERROR;
            }
            catch (const std::bad_alloc&)
            {
                return FE_OUT_OF_MEMORY;
            }
        }

        void setVsyncEnabled(bool enabled) noexcept override { m_Desc.vsync = enabled; }

        // d3d12::IGraphicsSwapChain
        IDXGISwapChain3* getDXGISwapChain() noexcept override { return m_SwapChain; }
        bool isTearingSupported() noexcept override { return m_TearingSupported; }

    private:
        SwapChainDesc m_Desc;
        AutoPtr<nvrhi::IDevice> m_Device;
        bool m_TearingSupported = false;

        AutoPtr<ID3D12CommandQueue> m_GraphicsQueue;
        AutoPtr<IDXGISwapChain3> m_SwapChain;
        DXGI_FORMAT m_DxgiFormat = DXGI_FORMAT_UNKNOWN;
        UINT m_SwapChainFlags = 0;

        std::vector<AutoPtr<ID3D12Resource>> m_NativeBackBuffers;
        std::vector<TextureHandle> m_BackBuffers;

        AutoPtr<ID3D12Fence> m_FrameFence;
        std::vector<HANDLE> m_FrameFenceEvents;
        UINT64 m_FrameCount = 1;

        bool createRenderTargets()
        {
            m_NativeBackBuffers.resize(m_Desc.bufferCount);
            m_BackBuffers.resize(m_Desc.bufferCount);

            for (uint32_t n = 0; n < m_Desc.bufferCount; n++)
            {
                if (FAILED(m_SwapChain->GetBuffer(n, IID_PPV_ARGS(&m_NativeBackBuffers[n]))))
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
                textureDesc.initialState = ResourceStates::Present;
                textureDesc.keepInitialState = true;

                m_Device->createHandleForNativeTexture(ObjectTypes::D3D12_Resource, m_NativeBackBuffers[n].Get(),
                    textureDesc, &m_BackBuffers[n]);
                if (!m_BackBuffers[n])
                    return false;
            }

            return true;
        }

        void releaseRenderTargets()
        {
            // Make sure that all frames have finished rendering, and release the in-flight references to the back
            // buffers: ResizeBuffers needs every reference released.
            m_Device->waitForIdle();
            m_Device->runGarbageCollection();

            // Set the events so that beginFrame does not wait for frames that will never be signaled.
            for (HANDLE fenceEvent : m_FrameFenceEvents)
                SetEvent(fenceEvent);

            m_BackBuffers.clear();
            m_NativeBackBuffers.clear();
        }
    };

    FRESULT CreateSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device, IDXGIFactory2* factory,
        bool tearingSupported, IGraphicsSwapChain** ppSwapChain)
    {
        AutoPtr<SwapChain> swapChain = MAKE_RC_OBJ_PTR(SwapChain, desc, device, tearingSupported);
        if (!swapChain->initialize(factory))
            return FE_GENERIC_ERROR;
        *ppSwapChain = swapChain.Detach();
        return FS_OK;
    }
}
