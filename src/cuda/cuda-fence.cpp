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

// Explicit barriers and timeline fences on CUDA (ADR 0015). A stream executes in order, so barriers only checks its
// arguments. A fence is a ring of (value, CUevent): each GPU signal records an event on its stream, a GPU wait is
// cuStreamWaitEvent on the earliest recorded event whose value is at least the target, and a CPU wait synchronizes
// with that event. The design keeps this ring unless stream memory operations (cuStreamWaitValue64) are adopted.

#include "cuda-backend.h"
#include "../common/resource-barrier.h"

#include <nvrhi/common/misc.h>
#include <chrono>
#include <thread>

namespace nvrhi::cuda
{
    FRESULT CommandList::barriers(const ResourceBarrier* barriers, uint32_t count) noexcept
    {
        return validateBarrierArguments(barriers, count, m_Context.messageCallback);
    }

    Fence::Fence(const Context& context, const FenceDesc& desc)
        : m_Context(context)
        , m_Desc(desc)
        , m_CompletedValue(desc.initialValue)
        , m_RecordedValue(desc.initialValue)
    {
    }

    Fence::~Fence()
    {
        ContextScope scope(m_Context);
        for (const Entry& entry : m_Ring)
            g_Cuda.cuEventDestroy(entry.event);
        for (CUevent event : m_FreeEvents)
            g_Cuda.cuEventDestroy(event);
        for (CUevent event : m_RetiredEvents)
            g_Cuda.cuEventDestroy(event);
    }

    uint64_t Fence::pollLocked()
    {
        while (!m_Ring.empty())
        {
            const Entry& entry = m_Ring.front();
            const CUresult result = g_Cuda.cuEventQuery(entry.event);
            if (result == CUDA_ERROR_NOT_READY)
                break;
            if (result != CUDA_SUCCESS)
                m_Context.error("a fence signal failed: " + ResultToString(result));

            if (entry.value > m_CompletedValue)
                m_CompletedValue = entry.value;
            // A CPU wait may still synchronize with the event: it is reused only when no one waits.
            (m_Waiters == 0 ? m_FreeEvents : m_RetiredEvents).push_back(entry.event);
            m_Ring.pop_front();
        }

        if (m_Waiters == 0 && !m_RetiredEvents.empty())
        {
            m_FreeEvents.insert(m_FreeEvents.end(), m_RetiredEvents.begin(), m_RetiredEvents.end());
            m_RetiredEvents.clear();
        }

        return m_CompletedValue;
    }

    uint64_t Fence::getCompletedValue() noexcept
    {
        ContextScope scope(m_Context);
        std::lock_guard lock(m_Mutex);
        return pollLocked();
    }

    FRESULT Fence::wait(uint64_t value, uint64_t timeoutNs) noexcept
    {
        try
        {
            using Clock = std::chrono::steady_clock;
            ContextScope scope(m_Context);
            std::unique_lock lock(m_Mutex);

            if (pollLocked() >= value)
                return FS_OK;
            if (timeoutNs == 0)
                return FE_WAIT_TIMEOUT;

            const bool infinite = timeoutNs == c_InfiniteTimeout;
            const Clock::time_point deadline = infinite ? (Clock::time_point::max)()
                : Clock::now() + std::chrono::nanoseconds(int64_t(timeoutNs < uint64_t(INT64_MAX) / 2 ? timeoutNs : uint64_t(INT64_MAX) / 2));

            // Wait until a signal of at least 'value' is recorded or set from the CPU.
            auto recorded = [&] { return m_RecordedValue >= value; };
            if (infinite)
                m_Recorded.wait(lock, recorded);
            else if (!m_Recorded.wait_until(lock, deadline, recorded))
                return FE_WAIT_TIMEOUT;

            if (pollLocked() >= value)
                return FS_OK;

            CUevent event = nullptr;
            for (const Entry& entry : m_Ring)
            {
                if (entry.value >= value)
                {
                    event = entry.event;
                    break;
                }
            }
            if (!event)
                return pollLocked() >= value ? FS_OK : FE_GENERIC_ERROR;

            ++m_Waiters;
            lock.unlock();

            FRESULT result = FS_OK;
            if (infinite)
            {
                const CUresult status = g_Cuda.cuEventSynchronize(event);
                if (status != CUDA_SUCCESS)
                {
                    m_Context.error("IFence::wait: " + ResultToString(status));
                    result = FE_GENERIC_ERROR;
                }
            }
            else
            {
                // Poll with back-off until the deadline.
                auto sleep = std::chrono::microseconds(20);
                for (;;)
                {
                    const CUresult status = g_Cuda.cuEventQuery(event);
                    if (status == CUDA_SUCCESS)
                        break;
                    if (status != CUDA_ERROR_NOT_READY)
                    {
                        m_Context.error("IFence::wait: " + ResultToString(status));
                        result = FE_GENERIC_ERROR;
                        break;
                    }
                    if (Clock::now() >= deadline)
                    {
                        result = FE_WAIT_TIMEOUT;
                        break;
                    }
                    std::this_thread::sleep_for(sleep);
                    if (sleep < std::chrono::milliseconds(1))
                        sleep *= 2;
                }
            }

            lock.lock();
            --m_Waiters;
            pollLocked();
            return result;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }

    FRESULT Fence::signal(uint64_t value) noexcept
    {
        ContextScope scope(m_Context);
        {
            std::lock_guard lock(m_Mutex);
            if (value <= pollLocked())
            {
                m_Context.error("IFence::signal: the value must increase");
                return FE_INVALID_ARGS;
            }
            m_CompletedValue = value;
            if (value > m_RecordedValue)
                m_RecordedValue = value;
        }
        m_Recorded.notify_all();
        return FS_OK;
    }

    bool Fence::recordSignal(CUstream stream, uint64_t value)
    {
        {
            std::lock_guard lock(m_Mutex);
            pollLocked();

            CUevent event = nullptr;
            if (!m_FreeEvents.empty())
            {
                event = m_FreeEvents.back();
                m_FreeEvents.pop_back();
            }
            else if (!NVRHI_CUDA_CHECK(m_Context, cuEventCreate(&event, CU_EVENT_DISABLE_TIMING)))
            {
                return false;
            }

            if (!NVRHI_CUDA_CHECK(m_Context, cuEventRecord(event, stream)))
            {
                m_FreeEvents.push_back(event);
                return false;
            }

            m_Ring.push_back({ value, event });
            if (value > m_RecordedValue)
                m_RecordedValue = value;
        }
        m_Recorded.notify_all();
        return true;
    }

    bool Fence::enqueueWait(CUstream stream, uint64_t value)
    {
        std::lock_guard lock(m_Mutex);
        if (pollLocked() >= value)
            return true;

        for (const Entry& entry : m_Ring)
        {
            if (entry.value >= value)
                return NVRHI_CUDA_CHECK(m_Context, cuStreamWaitEvent(stream, entry.event, 0));
        }

        return false;
    }

    FRESULT Device::createFence(const FenceDesc& desc, IFence** ppFence) noexcept
    {
        if (desc.shared)
            return unsupported(ppFence, "createFence with FenceDesc::shared (exported CUDA fences are a follow-up)");

        return utils::ReturnObject(ppFence, [&] { return FenceHandle(TakeOver(MAKE_RC_OBJ(Fence, m_Context, desc))); });
    }

    uint64_t Device::submit(const SubmitDesc& desc) noexcept
    {
        try
        {
            Queue* queue = uint32_t(desc.queue) < c_NumQueues ? m_Queues[uint32_t(desc.queue)].Get() : nullptr;
            if (!queue)
            {
                m_Context.error("submit: the queue does not exist on this device");
                return 0;
            }

            std::vector<MonoPtr<CommandListInstance>> instances;
            instances.reserve(desc.numCommandLists);
            for (uint32_t i = 0; i < desc.numCommandLists; ++i)
            {
                CommandList* commandList = checked_cast<CommandList*>(desc.commandLists[i]);
                if (!commandList)
                    continue;
                MonoPtr<CommandListInstance> instance = commandList->takeInstance();
                if (instance)
                    instances.push_back(std::move(instance));
            }

            ContextScope scope(m_Context);
            return queue->submit(instances.data(), instances.size(), desc.waits, desc.numWaits, desc.signals, desc.numSignals);
        }
        catch (...)
        {
            m_Context.error("submit failed");
            return 0;
        }
    }

    FRESULT Device::queueWaitFence(CommandQueue queue, IFence* fence, uint64_t value) noexcept
    {
        if (!fence)
            return FE_INVALID_ARGS;

        const FenceValue wait{ fence, value };
        SubmitDesc desc;
        desc.queue = queue;
        desc.waits = &wait;
        desc.numWaits = 1;
        return submit(desc) != 0 ? FS_OK : FE_GENERIC_ERROR;
    }

    FRESULT Device::queueSignalFence(CommandQueue queue, IFence* fence, uint64_t value) noexcept
    {
        if (!fence)
            return FE_INVALID_ARGS;

        const FenceValue signal{ fence, value };
        SubmitDesc desc;
        desc.queue = queue;
        desc.signals = &signal;
        desc.numSignals = 1;
        return submit(desc) != 0 ? FS_OK : FE_GENERIC_ERROR;
    }

    FRESULT Device::waitFences(const FenceValue* fences, uint32_t count, bool waitAll, uint64_t timeoutNs) noexcept
    {
        if (count == 0)
            return FS_OK;
        if (!fences)
            return FE_INVALID_ARGS;

        using Clock = std::chrono::steady_clock;
        const bool infinite = timeoutNs == c_InfiniteTimeout;
        const Clock::time_point start = Clock::now();
        auto remaining = [&]() -> uint64_t
        {
            if (infinite)
                return c_InfiniteTimeout;
            const uint64_t elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
            return elapsed >= timeoutNs ? 0 : timeoutNs - elapsed;
        };

        if (waitAll)
        {
            // Each fence in turn, within the one deadline.
            for (uint32_t index = 0; index < count; ++index)
            {
                if (!fences[index].fence)
                    return FE_INVALID_ARGS;
                const FRESULT result = fences[index].fence->wait(fences[index].value, remaining());
                if (result != FS_OK)
                    return result;
            }
            return FS_OK;
        }

        // Any one: poll with back-off.
        auto sleep = std::chrono::microseconds(20);
        for (;;)
        {
            for (uint32_t index = 0; index < count; ++index)
            {
                if (!fences[index].fence)
                    return FE_INVALID_ARGS;
                if (fences[index].fence->getCompletedValue() >= fences[index].value)
                    return FS_OK;
            }
            if (remaining() == 0)
                return FE_WAIT_TIMEOUT;
            std::this_thread::sleep_for(sleep);
            if (sleep < std::chrono::milliseconds(1))
                sleep *= 2;
        }
    }

} // namespace nvrhi::cuda
