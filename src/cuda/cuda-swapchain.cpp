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

// The CUDA swap chain (ADR 0014): no window. It rotates its back buffers and paces the frames with event queries on
// the graphics queue (one CUDA stream): present() sets the query of the current back buffer and moves on;
// beginFrame() waits for the query of the back buffer it is about to reuse.

#include <nvrhi/cuda.h>
#include <nvrhi/core/foundation.h>

#include <new>
#include <queue>
#include <vector>

namespace nvrhi::cuda
{
    NVRHI_CLASS_CLSID(SwapChain, "69b0f000-7877-44bb-bba6-930579b84d00")
    class SwapChain final : public ObjectImpl<IGraphicsSwapChain>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(SwapChain)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(SwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::cuda::IGraphicsSwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IGraphicsSwapChain)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_IMPLEMENTS_CLASS(SwapChain)
        NVRHI_END_INTERFACE_TABLE()

        SwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device)
            : m_Desc(desc)
            , m_Device(device)
        {
            if (m_Desc.bufferCount == 0)
                m_Desc.bufferCount = 1;
            if (m_Desc.maxFramesInFlight == 0)
                m_Desc.maxFramesInFlight = 1;
        }

        ~SwapChain()
        {
            m_Device->waitForIdle();
        }

        bool initialize()
        {
            m_FrameQueries.resize(m_Desc.bufferCount);
            return createBackBuffers();
        }

        // nvrhi::IGraphicsSwapChain
        const SwapChainDesc& getDesc() const noexcept override { return m_Desc; }
        uint32_t getBackBufferCount() noexcept override { return uint32_t(m_BackBuffers.size()); }

        ITexture* getBackBuffer(uint32_t index) noexcept override
        {
            return index < m_BackBuffers.size() ? m_BackBuffers[index].Get() : nullptr;
        }

        uint32_t getCurrentBackBufferIndex() noexcept override { return m_CurrentIndex; }

        FRESULT beginFrame() noexcept override
        {
            if (IEventQuery* query = m_FrameQueries[m_CurrentIndex])
                m_Device->waitEventQuery(query);
            return FS_OK;
        }

        FRESULT present() noexcept override
        {
            try
            {
                EventQueryHandle& query = m_FrameQueries[m_CurrentIndex];
                if (!query)
                    m_Device->createEventQuery(&query);
                if (!query)
                    return FE_GENERIC_ERROR;
                m_Device->resetEventQuery(query);
                m_Device->setEventQuery(query, CommandQueue::Graphics);

                m_FramesInFlight.push(query);
                while (m_FramesInFlight.size() > m_Desc.maxFramesInFlight)
                {
                    m_Device->waitEventQuery(m_FramesInFlight.front());
                    m_FramesInFlight.pop();
                }

                m_CurrentIndex = (m_CurrentIndex + 1) % uint32_t(m_BackBuffers.size());
                return FS_OK;
            }
            catch (const std::bad_alloc&)
            {
                return FE_OUT_OF_MEMORY;
            }
        }

        FRESULT resize(uint32_t width, uint32_t height) noexcept override
        {
            if (width == 0 || height == 0)
                return FE_INVALID_ARGS;
            try
            {
                m_Device->waitForIdle();
                m_FramesInFlight = {};
                m_BackBuffers.clear();
                m_Desc.width = width;
                m_Desc.height = height;
                m_CurrentIndex = 0;
                return createBackBuffers() ? FS_OK : FE_GENERIC_ERROR;
            }
            catch (const std::bad_alloc&)
            {
                return FE_OUT_OF_MEMORY;
            }
        }

        void setVsyncEnabled(bool enabled) noexcept override { m_Desc.vsync = enabled; }

        // cuda::IGraphicsSwapChain
        IEventQuery* getFrameQuery(uint32_t index) noexcept override
        {
            return index < m_FrameQueries.size() ? m_FrameQueries[index].Get() : nullptr;
        }

    private:
        SwapChainDesc m_Desc;
        AutoPtr<nvrhi::IDevice> m_Device;
        std::vector<TextureHandle> m_BackBuffers;
        std::vector<EventQueryHandle> m_FrameQueries;
        std::queue<EventQueryHandle> m_FramesInFlight;
        uint32_t m_CurrentIndex = 0;

        bool createBackBuffers()
        {
            for (uint32_t index = 0; index < m_Desc.bufferCount; index++)
            {
                TextureDesc textureDesc;
                textureDesc.width = m_Desc.width;
                textureDesc.height = m_Desc.height;
                textureDesc.format = m_Desc.format;
                textureDesc.isUAV = true;
                textureDesc.debugName = "SwapChainBuffer";

                TextureHandle texture;
                m_Device->createTexture(textureDesc, &texture);
                if (!texture)
                    return false;
                m_BackBuffers.push_back(texture);
            }
            return true;
        }
    };

    FRESULT CreateSwapChain(const SwapChainDesc& desc, nvrhi::IDevice* device, IGraphicsSwapChain** ppSwapChain)
    {
        AutoPtr<SwapChain> swapChain = MAKE_RC_OBJ_PTR(SwapChain, desc, device);
        if (!swapChain->initialize())
            return FE_GENERIC_ERROR;
        *ppSwapChain = swapChain.Detach();
        return FS_OK;
    }
}
