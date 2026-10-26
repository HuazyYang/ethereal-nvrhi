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

// The swap chain (ADR 0014): back buffers that are presented to a window, and the frame pacing between the
// command queue and presentation. Each backend implements it and derives its own interface from it
// (d3d11::IGraphicsSwapChain, d3d12::IGraphicsSwapChain, vulkan::IGraphicsSwapChain, cuda::IGraphicsSwapChain).
// A swap chain is created by the graphics factory of its backend (<nvrhi/factory.h>).
//
// One frame:
//
//     if (swapChain->beginFrame() == nvrhi::FE_OUT_OF_DATE) { /* release framebuffers, resize(0, 0), retry */ }
//     nvrhi::ITexture* backBuffer = swapChain->getBackBuffer(swapChain->getCurrentBackBufferIndex());
//     // ... record and execute command lists that render into backBuffer ...
//     swapChain->present();

#include <nvrhi/nvrhi.h>

namespace nvrhi
{
    struct SwapChainDesc
    {
        // The window to present to: HWND on Windows, an X11 Window elsewhere (with the X11 Display* in 'display',
        // which is unused on Windows). nullptr for a CUDA swap chain, which has no window.
        NativeObject window = nullptr;
        NativeObject display = nullptr;

        // The size of the back buffers. 0 takes the size of the window's client area.
        uint32_t width = 0;
        uint32_t height = 0;

        uint32_t bufferCount = 3;
        // sRGB formats are supported: DXGI swap chains use the UNORM format and the back-buffer textures the sRGB
        // one. Vulkan swaps RGBA8 for BGRA8, which is what surfaces offer; getDesc() reports the format in use.
        Format format = Format::SRGBA8_UNORM;
        uint32_t sampleCount = 1;
        uint32_t sampleQuality = 0;

        // Vulkan and CUDA: how many frames present() lets the GPU run ahead.
        uint32_t maxFramesInFlight = 2;
        // The refresh rate in Hz, for fullscreen modes; 0 leaves it to the system.
        uint32_t refreshRate = 0;
        // D3D: the DXGI_USAGE flags of the back buffers (default DXGI_USAGE_SHADER_INPUT | RENDER_TARGET_OUTPUT).
        uint32_t dxgiUsage = 0x10 | 0x20;

        bool vsync = false;

        SwapChainDesc& setWindow(NativeObject value) { window = value; return *this; }
        SwapChainDesc& setDisplay(NativeObject value) { display = value; return *this; }
        SwapChainDesc& setWidth(uint32_t value) { width = value; return *this; }
        SwapChainDesc& setHeight(uint32_t value) { height = value; return *this; }
        SwapChainDesc& setBufferCount(uint32_t value) { bufferCount = value; return *this; }
        SwapChainDesc& setFormat(Format value) { format = value; return *this; }
        SwapChainDesc& setSampleCount(uint32_t value) { sampleCount = value; return *this; }
        SwapChainDesc& setSampleQuality(uint32_t value) { sampleQuality = value; return *this; }
        SwapChainDesc& setMaxFramesInFlight(uint32_t value) { maxFramesInFlight = value; return *this; }
        SwapChainDesc& setRefreshRate(uint32_t value) { refreshRate = value; return *this; }
        SwapChainDesc& setDxgiUsage(uint32_t value) { dxgiUsage = value; return *this; }
        SwapChainDesc& setVsync(bool value) { vsync = value; return *this; }
    };

    static_assert(offsetof(SwapChainDesc, width) == 16);
    static_assert(offsetof(SwapChainDesc, format) == 28);
    static_assert(offsetof(SwapChainDesc, vsync) == 52);
    static_assert(sizeof(SwapChainDesc) == 56);

    NVRHI_IID(IGraphicsSwapChain, "728da59b-011f-4e34-860f-3d90101fc423")
    struct IGraphicsSwapChain : IRHIObject
    {
        NVRHI_DECLARE_UUID_TRAITS(IGraphicsSwapChain)

        // The description in use: the actual size and format, which can differ from the requested ones.
        [[nodiscard]] virtual const SwapChainDesc& getDesc() const noexcept = 0;

        virtual uint32_t getBackBufferCount() noexcept = 0;
        // The back buffer at index, not AddRef'd: it stays valid until the next resize() or the swap chain's
        // destruction. nullptr if index >= getBackBufferCount().
        virtual ITexture* getBackBuffer(uint32_t index) noexcept = 0;
        // The back buffer that the current frame renders into. Valid after beginFrame().
        virtual uint32_t getCurrentBackBufferIndex() noexcept = 0;

        // Starts a frame:
        // - D3D11: does nothing.
        // - D3D12: waits on the CPU for the frame fence's sync point of the current back buffer, i.e. until the GPU
        //   has finished the frame that last presented it.
        // - Vulkan: acquires the next image and queues a wait for its semaphore on the graphics queue; the wait is
        //   submitted with the next command list.
        // - CUDA: waits for the in-flight stream work of the frame slot that is reused.
        // Returns FS_OK, FE_OUT_OF_DATE if the swap chain must be resized first (Vulkan), or another FE_* code.
        virtual FRESULT beginFrame() noexcept = 0;

        // Presents the current back buffer and paces the frames: signals the frame fence (D3D12), the present
        // semaphore and the frames-in-flight query (Vulkan), or the frame slot (CUDA).
        // Returns FS_OK, FE_OUT_OF_DATE (Vulkan; the frame was presented or dropped, resize before the next one), or
        // another FE_* code.
        virtual FRESULT present() noexcept = 0;

        // Resizes the back buffers; width or height 0 takes the size of the window (Vulkan: the surface extent).
        // Waits until the device is idle. The caller must release its own references to the back buffers
        // (framebuffers, binding sets) first: the old back buffers are destroyed.
        virtual FRESULT resize(uint32_t width, uint32_t height) noexcept = 0;

        // Takes effect at the next present() (D3D) or the next resize() (Vulkan: the present mode).
        virtual void setVsyncEnabled(bool enabled) noexcept = 0;
    };

    typedef AutoPtr<IGraphicsSwapChain> GraphicsSwapChainHandle;
}
