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

// A queue is one CUDA stream. Each executeCommandLists call is a submission: the recordings are replayed on the
// stream, an event is recorded after them, and the submission keeps the recordings (their resources and
// transient memory) until the event has signalled.

#include "cuda-backend.h"

namespace nvrhi::cuda
{
    Queue::Queue(const Context& context, Device* device, CommandQueue type)
        : m_Context(context)
        , m_Device(device)
        , m_Type(type)
    {
    }

    Queue::~Queue()
    {
        waitForIdle();
        for (CUevent event : m_FreeEvents)
            g_Cuda.cuEventDestroy(event);
        m_FreeEvents.clear();
        if (m_Stream)
            g_Cuda.cuStreamDestroy(m_Stream);
    }

    bool Queue::initialize()
    {
        // Non-blocking: the streams never synchronize with the legacy default stream.
        return NVRHI_CUDA_CHECK(m_Context, cuStreamCreate(&m_Stream, CU_STREAM_NON_BLOCKING));
    }

    uint64_t Queue::submit(MonoPtr<CommandListInstance>* instances, size_t count)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        Submission submission;
        submission.number = ++m_LastSubmitted;

        for (size_t i = 0; i < count; ++i)
        {
            CommandListInstance& instance = *instances[i];
            instance.submission = submission.number;
            instance.queue = m_Type;
            if (!ExecuteCommands(m_Context, m_Stream, instance))
                m_Context.error("a command list failed to execute; its remaining commands were skipped");
            submission.instances.push_back(std::move(instances[i]));
        }

        if (!m_FreeEvents.empty())
        {
            submission.event = m_FreeEvents.back();
            m_FreeEvents.pop_back();
        }
        else if (!NVRHI_CUDA_CHECK(m_Context, cuEventCreate(&submission.event, CU_EVENT_DISABLE_TIMING)))
        {
            // Without an event the submission cannot be tracked: wait for it here.
            g_Cuda.cuStreamSynchronize(m_Stream);
            m_LastCompleted = submission.number;
            retire(submission);
            return submission.number;
        }

        NVRHI_CUDA_CHECK(m_Context, cuEventRecord(submission.event, m_Stream));
        m_InFlight.push_back(std::move(submission));
        return m_LastSubmitted;
    }

    void Queue::retire(Submission& submission)
    {
        TransientPool& pool = m_Device->getTransientPool();
        for (auto& instance : submission.instances)
        {
            for (TransientChunk* chunk : instance->chunks)
                pool.release(chunk);
            instance->chunks.clear();
            for (CUdeviceptr memory : instance->deferredFrees)
                g_Cuda.cuMemFree(memory);
            instance->deferredFrees.clear();
        }
        submission.instances.clear();   // releases the resources of the recordings
        if (submission.event)
            m_FreeEvents.push_back(submission.event);
        submission.event = nullptr;
    }

    uint64_t Queue::updateCompleted()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        while (!m_InFlight.empty())
        {
            Submission& submission = m_InFlight.front();
            const CUresult result = g_Cuda.cuEventQuery(submission.event);
            if (result == CUDA_ERROR_NOT_READY)
                break;
            if (result != CUDA_SUCCESS)
                m_Context.error("a submission failed: " + ResultToString(result));
            m_LastCompleted = submission.number;
            retire(submission);
            m_InFlight.pop_front();
        }
        return m_LastCompleted;
    }

    uint64_t Queue::getLastSubmitted()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_LastSubmitted;
    }

    CUevent Queue::getSubmissionEvent(uint64_t number)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (number <= m_LastCompleted)
            return nullptr;
        for (const Submission& submission : m_InFlight)
        {
            if (submission.number >= number)
                return submission.event;
        }
        return nullptr;
    }

    void Queue::waitForIdle()
    {
        if (!m_Stream)
            return;
        NVRHI_CUDA_CHECK(m_Context, cuStreamSynchronize(m_Stream));
        updateCompleted();
    }
}
