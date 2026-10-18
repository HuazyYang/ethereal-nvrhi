// SPDX-License-Identifier: MIT
#include "cuda-backend.h"

namespace nvrhi::cuda
{
FRESULT Device::createCommandList(const CommandListParameters &p, ICommandList **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (unsigned(p.queueType) >= 3)
        return FE_INVALID_ARGS;
    try
    {
        *out = MAKE_RC_OBJ(CommandList, context, this, p);
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
uint64_t Device::executeCommandLists(ICommandList *const *lists, size_t count, CommandQueue queue) noexcept
{
    if ((!lists && count) || unsigned(queue) >= 3)
        return 0;
    std::lock_guard<std::mutex> lock(mutex);
    Scope scope(context->handle);
    Submission submission{};
    submission.queue = queue;
    submission.id = nextSubmission++;
    try
    {
        for (size_t i = 0; i < count; ++i)
        {
            auto c = static_cast<CommandList *>(lists[i]);
            if (!c || c->context != context || c->opened || !c->valid || c->params.queueType != queue)
            {
                context->error("Invalid CUDA command list submission");
                return 0;
            }
            submission.commands.insert(submission.commands.end(), c->commands.begin(), c->commands.end());
        }
        submission.replay = std::make_shared<Replay>();
        submission.replay->context = context;
        submission.replay->stream = context->streams[unsigned(queue)];
        for (auto &command : submission.commands)
            if (!command(*submission.replay))
            {
                cuStreamSynchronize(submission.replay->stream);
                return 0;
            }
        if (!context->check(cuEventCreate(&submission.completion, CU_EVENT_DISABLE_TIMING), "cuEventCreate") ||
            !context->check(cuEventRecord(submission.completion, submission.replay->stream), "cuEventRecord"))
        {
            cuStreamSynchronize(submission.replay->stream);
            if (submission.completion)
                cuEventDestroy(submission.completion);
            return 0;
        }
        uint64_t id = submission.id;
        submissions.push_back(std::move(submission));
        return id;
    }
    catch (...)
    {
        cuStreamSynchronize(context->streams[unsigned(queue)]);
        if (submission.completion)
            cuEventDestroy(submission.completion);
        context->error("CUDA submission allocation failed");
        return 0;
    }
}
void Device::queueWaitForCommandList(CommandQueue wait, CommandQueue execute, uint64_t instance) noexcept
{
    if (unsigned(wait) >= 3 || unsigned(execute) >= 3)
        return;
    std::lock_guard<std::mutex> lock(mutex);
    Scope scope(context->handle);
    for (auto &s : submissions)
        if (s.id == instance)
        {
            if (s.queue != execute)
            {
                context->error("Submission queue mismatch");
                return;
            }
            context->check(cuStreamWaitEvent(context->streams[unsigned(wait)], s.completion, 0), "cuStreamWaitEvent");
            return;
        }
    if (instance >= nextSubmission)
        context->error("Unknown CUDA submission");
}
void Device::runGarbageCollection() noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    Scope scope(context->handle);
    for (auto it = submissions.begin(); it != submissions.end();)
    {
        auto status = cuEventQuery(it->completion);
        if (status == CUDA_SUCCESS)
        {
            cuEventDestroy(it->completion);
            it = submissions.erase(it);
        }
        else
        {
            if (status != CUDA_ERROR_NOT_READY)
                context->check(status, "cuEventQuery");
            ++it;
        }
    }
}
bool Device::waitForIdle() noexcept
{
    bool success = true;
    {
        std::lock_guard<std::mutex> lock(mutex);
        Scope scope(context->handle);
        for (auto s : context->streams)
            if (s)
                success = context->check(cuStreamSynchronize(s), "cuStreamSynchronize") && success;
        for (auto &s : submissions)
            if (s.completion)
                cuEventDestroy(s.completion);
        submissions.clear();
    }
    return success;
}
NVRHI_CLASS_CLSID(LifetimeTracker, "5490c013-09e1-40ad-a1ab-701827580013")
class LifetimeTracker : public ObjectImpl<ICommandListLifetimeTracker>
{
  public:
    CUDA_INTERFACES(LifetimeTracker, ICommandListLifetimeTracker)
    AutoPtr<IDevice> device;
    explicit LifetimeTracker(IDevice *d) : device(d)
    {
    }
    void runGarbageCollection() noexcept override
    {
        device->runGarbageCollection();
    }
};
FRESULT Device::createCommandListLifetimeTracker(CommandQueue queue, ICommandListLifetimeTracker **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    if (unsigned(queue) >= 3)
        return FE_INVALID_ARGS;
    try
    {
        *out = MAKE_RC_OBJ(LifetimeTracker, this);
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
void CommandList::open() noexcept
{
    commands.clear();
    opened = true;
    valid = true;
    clearState();
}
void CommandList::close() noexcept
{
    if (!opened)
    {
        valid = false;
        context->error("CUDA command list is not open");
    }
    opened = false;
    clearState();
}
void CommandList::clearState() noexcept
{
    computeState = {};
    rayState = {};
    stateRefs.clear();
    pushConstants.clear();
}
IDevice *CommandList::getDevice() noexcept
{
    return owner;
}
const CommandListParameters &CommandList::getDesc() noexcept
{
    return params;
}
void CommandList::writeBuffer(IBuffer *buffer, const void *data, size_t size, uint64_t offset) noexcept
{
    if (!size)
        return;
    auto b = static_cast<Buffer *>(buffer);
    if (!b || (!data && size) || offset > b->desc.byteSize || size > b->desc.byteSize - offset)
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<Buffer> keep(b);
        std::vector<uint8_t> bytes(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
        record([keep, bytes = std::move(bytes), offset](Replay &r) {
            return r.context->check(cuMemcpyHtoDAsync(keep->memory->ptr + offset, bytes.data(), bytes.size(), r.stream),
                                    "cuMemcpyHtoDAsync");
        });
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::clearBufferUInt(IBuffer *buffer, uint32_t value) noexcept
{
    auto b = static_cast<Buffer *>(buffer);
    if (!b || b->desc.byteSize % 4)
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<Buffer> keep(b);
        record([keep, value](Replay &r) {
            return r.context->check(
                cuMemsetD32Async(keep->memory->ptr, value, size_t(keep->desc.byteSize / 4), r.stream),
                "cuMemsetD32Async");
        });
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::copyBuffer(IBuffer *dst, uint64_t dstOffset, IBuffer *src, uint64_t srcOffset, uint64_t size) noexcept
{
    auto d = static_cast<Buffer *>(dst);
    auto s = static_cast<Buffer *>(src);
    if (!d || !s || dstOffset > d->desc.byteSize || size > d->desc.byteSize - dstOffset ||
        srcOffset > s->desc.byteSize || size > s->desc.byteSize - srcOffset)
    {
        valid = false;
        return;
    }
    if (d->context != context || s->context != context)
    {
        context->error("Cross-device copies require the interop API");
        valid = false;
        return;
    }
    try
    {
        AutoPtr<Buffer> dk(d), sk(s);
        record([dk, sk, dstOffset, srcOffset, size](Replay &r) {
            return r.context->check(
                cuMemcpyDtoDAsync(dk->memory->ptr + dstOffset, sk->memory->ptr + srcOffset, size_t(size), r.stream),
                "cuMemcpyDtoDAsync");
        });
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::setComputeState(const ComputeState &state) noexcept
{
    clearState();
    computeState = state;
    try
    {
        if (state.pipeline)
            stateRefs.emplace_back(state.pipeline);
        for (auto b : state.bindings)
            if (b)
                stateRefs.emplace_back(b);
        if (state.indirectParams)
            stateRefs.emplace_back(state.indirectParams);
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::setPushConstants(const void *data, size_t size) noexcept
{
    if (!size)
    {
        pushConstants.clear();
        return;
    }
    if (!data && size)
    {
        valid = false;
        return;
    }
    try
    {
        pushConstants.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    }
    catch (...)
    {
        valid = false;
    }
}

FRESULT Device::createEventQuery(IEventQuery **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    try
    {
        Scope scope(context->handle);
        auto q = TakeOver(MAKE_RC_OBJ(EventQuery, context));
        if (!context->check(cuEventCreate(&q->event, CU_EVENT_DISABLE_TIMING), "cuEventCreate"))
            return FE_GENERIC_ERROR;
        *out = q.Detach();
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
void Device::setEventQuery(IEventQuery *query, CommandQueue queue) noexcept
{
    if (!query || unsigned(queue) >= 3)
        return;
    std::lock_guard<std::mutex> lock(mutex);
    Scope scope(context->handle);
    auto q = static_cast<EventQuery *>(query);
    q->recorded = context->check(cuEventRecord(q->event, context->streams[unsigned(queue)]), "cuEventRecord");
}
bool Device::pollEventQuery(IEventQuery *query) noexcept
{
    if (!query)
        return false;
    Scope scope(context->handle);
    auto q = static_cast<EventQuery *>(query);
    return q->recorded && cuEventQuery(q->event) == CUDA_SUCCESS;
}
void Device::waitEventQuery(IEventQuery *query) noexcept
{
    if (!query)
        return;
    Scope scope(context->handle);
    auto q = static_cast<EventQuery *>(query);
    if (q->recorded)
        context->check(cuEventSynchronize(q->event), "cuEventSynchronize");
}
void Device::resetEventQuery(IEventQuery *query) noexcept
{
    if (query)
        static_cast<EventQuery *>(query)->recorded = false;
}
FRESULT Device::createTimerQuery(ITimerQuery **out) noexcept
{
    if (!out)
        return FE_INVALID_ARGS;
    *out = nullptr;
    try
    {
        Scope scope(context->handle);
        auto q = TakeOver(MAKE_RC_OBJ(TimerQuery, context));
        if (!context->check(cuEventCreate(&q->start, 0), "cuEventCreate") ||
            !context->check(cuEventCreate(&q->end, 0), "cuEventCreate"))
            return FE_GENERIC_ERROR;
        *out = q.Detach();
        return FS_OK;
    }
    catch (...)
    {
        return FE_OUT_OF_MEMORY;
    }
}
bool Device::pollTimerQuery(ITimerQuery *query) noexcept
{
    if (!query)
        return false;
    Scope scope(context->handle);
    auto q = static_cast<TimerQuery *>(query);
    return q->recorded && cuEventQuery(q->end) == CUDA_SUCCESS;
}
float Device::getTimerQueryTime(ITimerQuery *query) noexcept
{
    if (!query)
        return 0;
    Scope scope(context->handle);
    auto q = static_cast<TimerQuery *>(query);
    if (!q->recorded)
        return 0;
    float ms = 0;
    context->check(cuEventSynchronize(q->end), "cuEventSynchronize");
    context->check(cuEventElapsedTime(&ms, q->start, q->end), "cuEventElapsedTime");
    return ms / 1000.f;
}
void Device::resetTimerQuery(ITimerQuery *query) noexcept
{
    if (query)
        static_cast<TimerQuery *>(query)->recorded = false;
}
void CommandList::beginTimerQuery(ITimerQuery *query) noexcept
{
    if (!query)
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<TimerQuery> q(static_cast<TimerQuery *>(query));
        record([q](Replay &r) {
            q->recorded = false;
            return r.context->check(cuEventRecord(q->start, r.stream), "cuEventRecord");
        });
    }
    catch (...)
    {
        valid = false;
    }
}
void CommandList::endTimerQuery(ITimerQuery *query) noexcept
{
    if (!query)
    {
        valid = false;
        return;
    }
    try
    {
        AutoPtr<TimerQuery> q(static_cast<TimerQuery *>(query));
        record([q](Replay &r) {
            q->recorded = r.context->check(cuEventRecord(q->end, r.stream), "cuEventRecord");
            return q->recorded;
        });
    }
    catch (...)
    {
        valid = false;
    }
}
} // namespace nvrhi::cuda
