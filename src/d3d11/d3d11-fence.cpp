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

// Explicit barriers and timeline fences on D3D11 (ADR 0015). The immediate context orders the work, so barriers
// only checks its arguments. Fences are ID3D11Fence objects (D3D11.4); D3D11 has one queue, so a submission's
// waits and signals are ID3D11DeviceContext4::Wait and Signal around work that has already been recorded.

#include "d3d11-backend.h"
#include "../common/resource-barrier.h"

#include <nvrhi/common/misc.h>
#include <chrono>
#include <sstream>
#include <vector>

namespace nvrhi::d3d11
{
    FRESULT CommandList::barriers(const ResourceBarrier* barriers, uint32_t count) noexcept
    {
        return validateBarrierArguments(barriers, count, m_Context.messageCallback);
    }

    static DWORD getWaitTimeoutMilliseconds(uint64_t timeoutNs)
    {
        if (timeoutNs == c_InfiniteTimeout)
            return INFINITE;
        const uint64_t milliseconds = (timeoutNs + 999999) / 1000000;
        return milliseconds >= uint64_t(INFINITE) ? INFINITE - 1 : DWORD(milliseconds);
    }

    // One auto-reset event per waiting thread; every wait re-checks its own condition, because a timed-out
    // registration can still set the event later.
    static HANDLE getThreadWaitEvent()
    {
        struct ThreadEvent
        {
            HANDLE handle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            ~ThreadEvent() { if (handle) CloseHandle(handle); }
        };
        thread_local ThreadEvent event;
        return event.handle;
    }

    static FRESULT getFenceWaitResult(const Context& context)
    {
        return FAILED(context.device->GetDeviceRemovedReason()) ? FE_GENERIC_ERROR : FS_OK;
    }

    // Waits until every fence (waitAll) or any fence reaches its value, or the timeout runs out.
    static FRESULT waitForFences(const Context& context, ID3D11Fence* const* fences, const uint64_t* values,
        uint32_t count, bool waitAll, uint64_t timeoutNs)
    {
        auto done = [&]
        {
            for (uint32_t index = 0; index < count; ++index)
            {
                const bool reached = fences[index]->GetCompletedValue() >= values[index];
                if (reached && !waitAll)
                    return true;
                if (!reached && waitAll)
                    return false;
            }
            return waitAll;
        };

        if (done())
            return getFenceWaitResult(context);
        if (timeoutNs == 0)
            return FE_WAIT_TIMEOUT;

        using Clock = std::chrono::steady_clock;
        const bool infinite = timeoutNs == c_InfiniteTimeout;
        const Clock::time_point deadline = infinite ? (Clock::time_point::max)()
            : Clock::now() + std::chrono::nanoseconds(int64_t(timeoutNs < uint64_t(INT64_MAX) / 2 ? timeoutNs : uint64_t(INT64_MAX) / 2));

        HANDLE event = getThreadWaitEvent();
        if (!event)
            return FE_GENERIC_ERROR;

        // A wait for a GPU signal still queued in the immediate context would never end.
        context.immediateContext->Flush();

        while (!done())
        {
            // The first fence that has not reached its value sets the event; the loop then checks them all again.
            for (uint32_t index = 0; index < count; ++index)
            {
                if (fences[index]->GetCompletedValue() < values[index])
                {
                    if (FAILED(fences[index]->SetEventOnCompletion(values[index], event)))
                        return FE_GENERIC_ERROR;
                    break;
                }
            }

            if (done())
                break;

            uint64_t remainingNs = c_InfiniteTimeout;
            if (!infinite)
            {
                const Clock::time_point now = Clock::now();
                if (now >= deadline)
                    return FE_WAIT_TIMEOUT;
                remainingNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count());
            }

            if (WaitForSingleObject(event, getWaitTimeoutMilliseconds(remainingNs)) == WAIT_FAILED)
                return FE_GENERIC_ERROR;
        }

        return getFenceWaitResult(context);
    }

    uint64_t Fence::getCompletedValue() noexcept
    {
        return fence->GetCompletedValue();
    }

    FRESULT Fence::wait(uint64_t value, uint64_t timeoutNs) noexcept
    {
        ID3D11Fence* const fences[] = { fence.Get() };
        const uint64_t values[] = { value };
        return waitForFences(m_Context, fences, values, 1, true, timeoutNs);
    }

    FRESULT Fence::signal(uint64_t value) noexcept
    {
        if (value <= fence->GetCompletedValue())
        {
            m_Context.error("IFence::signal: the value must increase");
            return FE_INVALID_ARGS;
        }

        // A CPU signal: ID3D11Fence has none, so the immediate context signals it at once and flushes.
        AutoPtr<ID3D11DeviceContext4> context4;
        if (FAILED(m_Context.immediateContext->QueryInterface(IID_PPV_ARGS(&context4))))
            return FE_UNSUPPORTED;
        if (FAILED(context4->Signal(fence, value)))
            return FE_GENERIC_ERROR;
        context4->Flush();
        return FS_OK;
    }

    NativeObject Fence::getNativeObject(ObjectType objectType) noexcept
    {
        if (objectType == ObjectTypes::D3D11_Fence)
            return NativeObject(fence.Get());
        return nullptr;
    }

    FenceHandle Device::createFence(const FenceDesc& desc)
    {
        AutoPtr<ID3D11Device5> device5;
        if (FAILED(m_Context.device->QueryInterface(IID_PPV_ARGS(&device5))))
            return nullptr;   // before D3D11.4: FE_UNSUPPORTED

        AutoPtr<Fence> fence = TakeOver(MAKE_RC_OBJ(Fence, m_Context));
        fence->desc = desc;

        const HRESULT hr = device5->CreateFence(desc.initialValue,
            desc.shared ? D3D11_FENCE_FLAG_SHARED : D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence->fence));
        if (FAILED(hr))
        {
            std::stringstream ss;
            ss << "Failed to create fence " << utils::DebugNameToString(desc.debugName) << ", error code = 0x";
            ss.setf(std::ios::hex, std::ios::basefield);
            ss << hr;
            m_Context.error(ss.str());
            return nullptr;
        }

        return fence;
    }

    uint64_t Device::submit(const SubmitDesc& desc) noexcept
    {
        // The command lists ran on the immediate context while they were recorded.
        if (desc.numWaits == 0 && desc.numSignals == 0)
            return 0;

        AutoPtr<ID3D11DeviceContext4> context4;
        if (FAILED(m_Context.immediateContext->QueryInterface(IID_PPV_ARGS(&context4))))
        {
            m_Context.error("submit: fences need D3D11.4 (ID3D11DeviceContext4)");
            return 0;
        }

        for (uint32_t index = 0; index < desc.numWaits; ++index)
            context4->Wait(checked_cast<Fence*>(desc.waits[index].fence)->fence, desc.waits[index].value);
        for (uint32_t index = 0; index < desc.numSignals; ++index)
            context4->Signal(checked_cast<Fence*>(desc.signals[index].fence)->fence, desc.signals[index].value);

        return 0;
    }

    FRESULT Device::queueWaitFence(CommandQueue queue, IFence* fence, uint64_t value) noexcept
    {
        if (queue != CommandQueue::Graphics || !fence)
            return FE_INVALID_ARGS;

        const FenceValue wait{ fence, value };
        SubmitDesc desc;
        desc.waits = &wait;
        desc.numWaits = 1;
        submit(desc);
        return FS_OK;
    }

    FRESULT Device::queueSignalFence(CommandQueue queue, IFence* fence, uint64_t value) noexcept
    {
        if (queue != CommandQueue::Graphics || !fence)
            return FE_INVALID_ARGS;

        const FenceValue signal{ fence, value };
        SubmitDesc desc;
        desc.signals = &signal;
        desc.numSignals = 1;
        submit(desc);
        return FS_OK;
    }

    FRESULT Device::waitFences(const FenceValue* fences, uint32_t count, bool waitAll, uint64_t timeoutNs) noexcept
    {
        if (count == 0)
            return FS_OK;
        if (!fences)
            return FE_INVALID_ARGS;

        std::vector<ID3D11Fence*> d3dFences(count);
        std::vector<uint64_t> values(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            if (!fences[index].fence)
                return FE_INVALID_ARGS;
            d3dFences[index] = checked_cast<Fence*>(fences[index].fence)->fence;
            values[index] = fences[index].value;
        }

        return waitForFences(m_Context, d3dFences.data(), values.data(), count, waitAll, timeoutNs);
    }

} // namespace nvrhi::d3d11
