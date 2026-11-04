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

// Timeline fences and IDevice::submit on D3D12 (ADR 0015): an IFence is an ID3D12Fence, and a submission is
// queue->Wait for each wait, ExecuteCommandLists, the queue's own tracking signal, then queue->Signal for each
// signal.

#include "d3d12-backend.h"

#include <nvrhi/common/misc.h>
#include <chrono>
#include <sstream>
#include <vector>

namespace nvrhi::d3d12
{
    DWORD getWaitTimeoutMilliseconds(uint64_t timeoutNs)
    {
        if (timeoutNs == c_InfiniteTimeout)
            return INFINITE;

        const uint64_t milliseconds = (timeoutNs + 999999) / 1000000;
        return milliseconds >= uint64_t(INFINITE) ? INFINITE - 1 : DWORD(milliseconds);
    }

    FRESULT getFenceWaitResult(const Context& context)
    {
        return FAILED(context.device->GetDeviceRemovedReason()) ? FE_GENERIC_ERROR : FS_OK;
    }

    // One auto-reset event per waiting thread. A timed-out wait leaves its registration with the fence, so the
    // event can be set later for an old value: every wait re-checks the fence and waits again until its own
    // condition holds or its deadline passes.
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

    // Waits on 'event' until 'done' holds or the timeout runs out. 'arm' registers the event for the condition.
    template <typename TDone, typename TArm>
    static FRESULT waitWithEvent(const Context& context, uint64_t timeoutNs, TDone&& done, TArm&& arm)
    {
        using Clock = std::chrono::steady_clock;
        const bool infinite = timeoutNs == c_InfiniteTimeout;
        const Clock::time_point deadline = infinite ? (Clock::time_point::max)()
            : Clock::now() + std::chrono::nanoseconds(int64_t(timeoutNs < uint64_t(INT64_MAX) / 2 ? timeoutNs : uint64_t(INT64_MAX) / 2));

        HANDLE event = getThreadWaitEvent();
        if (!event)
            return FE_GENERIC_ERROR;

        while (!done())
        {
            if (FAILED(arm(event)))
                return FE_GENERIC_ERROR;

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

            const DWORD result = WaitForSingleObject(event, getWaitTimeoutMilliseconds(remainingNs));
            if (result == WAIT_FAILED)
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
        const uint64_t completed = fence->GetCompletedValue();
        if (completed >= value)
            return completed == UINT64_MAX ? getFenceWaitResult(m_Context) : FS_OK;

        if (timeoutNs == 0)
            return FE_WAIT_TIMEOUT;

        // A removed device completes every fence with UINT64_MAX, so an infinite wait cannot hang on it.
        if (timeoutNs == c_InfiniteTimeout)
        {
            if (FAILED(fence->SetEventOnCompletion(value, nullptr)))
                return FE_GENERIC_ERROR;
            return getFenceWaitResult(m_Context);
        }

        return waitWithEvent(m_Context, timeoutNs,
            [&] { return fence->GetCompletedValue() >= value; },
            [&](HANDLE event) { return fence->SetEventOnCompletion(value, event); });
    }

    FRESULT Fence::signal(uint64_t value) noexcept
    {
        if (value <= fence->GetCompletedValue())
        {
            m_Context.error("IFence::signal: the value must increase");
            return FE_INVALID_ARGS;
        }

        return SUCCEEDED(fence->Signal(value)) ? FS_OK : FE_GENERIC_ERROR;
    }

    NativeObject Fence::getNativeObject(ObjectType objectType) noexcept
    {
        if (objectType == ObjectTypes::D3D12_Fence)
            return NativeObject(fence.Get());
        return nullptr;
    }

    FenceHandle Device::createFence(const FenceDesc& desc)
    {
        AutoPtr<Fence> fence = TakeOver(MAKE_RC_OBJ(Fence, m_Context));
        fence->desc = desc;

        const HRESULT hr = m_Context.device->CreateFence(desc.initialValue,
            desc.shared ? D3D12_FENCE_FLAG_SHARED : D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence->fence));
        if (FAILED(hr))
        {
            std::stringstream ss;
            ss << "Failed to create fence " << utils::DebugNameToString(desc.debugName) << ", error code = 0x";
            ss.setf(std::ios::hex, std::ios::basefield);
            ss << hr;
            m_Context.error(ss.str());
            return nullptr;
        }

        if (!desc.debugName.empty())
        {
            std::wstring wname(desc.debugName.begin(), desc.debugName.end());
            fence->fence->SetName(wname.c_str());
        }

        return fence;
    }

    uint64_t Device::submit(const SubmitDesc& desc) noexcept
    {
        Queue* pQueue = getQueue(desc.queue);
        if (!pQueue)
        {
            m_Context.error("submit: the queue does not exist on this device");
            return 0;
        }

        for (uint32_t index = 0; index < desc.numWaits; ++index)
        {
            const FenceValue& wait = desc.waits[index];
            pQueue->queue->Wait(checked_cast<Fence*>(wait.fence)->fence, wait.value);
        }

        if (desc.numCommandLists != 0)
        {
            // No heap allocation for the usual handful of lists (the steady-state frame allocates nothing).
            constexpr uint32_t c_InlineLists = 16;
            ID3D12CommandList* inlineLists[c_InlineLists];
            std::vector<ID3D12CommandList*> heapLists;
            ID3D12CommandList** lists = inlineLists;
            if (desc.numCommandLists > c_InlineLists)
            {
                heapLists.resize(desc.numCommandLists);
                lists = heapLists.data();
            }

            for (uint32_t index = 0; index < desc.numCommandLists; ++index)
                lists[index] = checked_cast<CommandList*>(desc.commandLists[index])->getD3D12CommandList();

            pQueue->queue->ExecuteCommandLists(desc.numCommandLists, lists);
        }

        (void)pQueue->Signal();

        for (uint32_t index = 0; index < desc.numSignals; ++index)
        {
            const FenceValue& signal = desc.signals[index];
            pQueue->queue->Signal(checked_cast<Fence*>(signal.fence)->fence, signal.value);
        }

        for (uint32_t index = 0; index < desc.numCommandLists; ++index)
            (void)checked_cast<CommandList*>(desc.commandLists[index])->executed(pQueue);

        HRESULT hr = m_Context.device->GetDeviceRemovedReason();
        if (FAILED(hr))
            m_Context.messageCallback->message(MessageSeverity::Error, "Device Removed!");

        return pQueue->lastSubmittedInstance;
    }

    FRESULT Device::queueWaitFence(CommandQueue queue, IFence* fence, uint64_t value) noexcept
    {
        Queue* pQueue = getQueue(queue);
        if (!pQueue || !fence)
            return FE_INVALID_ARGS;

        return SUCCEEDED(pQueue->queue->Wait(checked_cast<Fence*>(fence)->fence, value)) ? FS_OK : FE_GENERIC_ERROR;
    }

    FRESULT Device::queueSignalFence(CommandQueue queue, IFence* fence, uint64_t value) noexcept
    {
        Queue* pQueue = getQueue(queue);
        if (!pQueue || !fence)
            return FE_INVALID_ARGS;

        return SUCCEEDED(pQueue->queue->Signal(checked_cast<Fence*>(fence)->fence, value)) ? FS_OK : FE_GENERIC_ERROR;
    }

    FRESULT Device::waitFences(const FenceValue* fences, uint32_t count, bool waitAll, uint64_t timeoutNs) noexcept
    {
        if (count == 0)
            return FS_OK;
        if (!fences)
            return FE_INVALID_ARGS;

        std::vector<ID3D12Fence*> d3dFences(count);
        std::vector<UINT64> values(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            if (!fences[index].fence)
                return FE_INVALID_ARGS;
            d3dFences[index] = checked_cast<Fence*>(fences[index].fence)->fence;
            values[index] = fences[index].value;
        }

        auto done = [&]
        {
            for (uint32_t index = 0; index < count; ++index)
            {
                const bool reached = d3dFences[index]->GetCompletedValue() >= values[index];
                if (reached && !waitAll)
                    return true;
                if (!reached && waitAll)
                    return false;
            }
            return waitAll;
        };

        if (done())
            return getFenceWaitResult(m_Context);
        if (timeoutNs == 0)
            return FE_WAIT_TIMEOUT;

        AutoPtr<ID3D12Device1> device1;
        if (FAILED(m_Context.device->QueryInterface(IID_PPV_ARGS(&device1))))
            return FE_UNSUPPORTED;

        const D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags = waitAll ? D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL : D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY;

        if (timeoutNs == c_InfiniteTimeout)
        {
            if (FAILED(device1->SetEventOnMultipleFenceCompletion(d3dFences.data(), values.data(), count, flags, nullptr)))
                return FE_GENERIC_ERROR;
            return getFenceWaitResult(m_Context);
        }

        return waitWithEvent(m_Context, timeoutNs, done, [&](HANDLE event)
        {
            return device1->SetEventOnMultipleFenceCompletion(d3dFences.data(), values.data(), count, flags, event);
        });
    }

} // namespace nvrhi::d3d12
