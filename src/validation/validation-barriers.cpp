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

// Validation of ICommandList::barriers, explicit-barrier scopes, fences and IDevice::submit (ADR 0015).

#include "validation-backend.h"
#include "../common/resource-barrier.h"
#include "../common/utils-internal.h"

#include <nvrhi/common/misc.h>
#include <nvrhi/utils.h>
#include <algorithm>
#include <sstream>

namespace nvrhi::validation
{
    static const char* getResourceName(const BarrierResource& kinds)
    {
        if (kinds.texture) return kinds.texture->getDesc().debugName.c_str();
        if (kinds.buffer) return kinds.buffer->getDesc().debugName.c_str();
        if (kinds.accelStruct) return kinds.accelStruct->getDesc().debugName.c_str();
        if (kinds.stagingTexture) return kinds.stagingTexture->getDesc().debugName.c_str();
        return "";
    }

    static std::string statesToString(ResourceStates states)
    {
        std::stringstream ss;
        ss << "0x" << std::hex << uint32_t(states);
        return ss.str();
    }

    static bool sameRegion(const SubresourceRegion& a, const SubresourceRegion& b)
    {
        return a.baseMip == b.baseMip && a.mipCount == b.mipCount && a.baseSlice == b.baseSlice &&
            a.sliceCount == b.sliceCount && a.planes == b.planes;
    }

    void CommandListWrapper::checkExplicitBarrierScope(const char* operation) const
    {
        if (m_ExplicitBarrierScope.empty())
            return;

        std::stringstream ss;
        ss << operation << " was called inside the explicit-barrier scope of '" << m_ExplicitBarrierScope
            << "': only the scope's owner places barriers there (ICommandList::setExplicitBarrierScope)";
        error(ss.str());
    }

    void CommandListWrapper::setExplicitBarrierScope(const char* owner) noexcept
    {
        if (!requireOpenState())
            return;

        if (owner && !m_ExplicitBarrierScope.empty())
        {
            std::stringstream ss;
            ss << "setExplicitBarrierScope('" << owner << "') opens a scope while '" << m_ExplicitBarrierScope
                << "' is still open";
            error(ss.str());
        }

        m_ExplicitBarrierScope = owner ? owner : "";
        m_CommandList->setExplicitBarrierScope(owner);
    }

    bool CommandListWrapper::validateBarrier(const ResourceBarrier& barrier, uint32_t index)
    {
        std::stringstream prefix;
        prefix << "barriers: barrier " << index << ": ";

        if (barrier.type == BarrierType::Aliasing)
        {
            HeapBinding after;
            if (barrier.aliasing.after->queryHeapBinding(after) != FS_OK)
            {
                error(prefix.str() + "the 'after' resource of an aliasing barrier is not bound to a heap");
                return false;
            }
            if (barrier.aliasing.before)
            {
                HeapBinding before;
                if (barrier.aliasing.before->queryHeapBinding(before) != FS_OK)
                {
                    error(prefix.str() + "the 'before' resource of an aliasing barrier is not bound to a heap");
                    return false;
                }
                const bool overlap = before.heap == after.heap &&
                    before.offset < after.offset + after.size && after.offset < before.offset + before.size;
                if (!overlap)
                {
                    error(prefix.str() + "the two resources of an aliasing barrier do not overlap in one heap");
                    return false;
                }
            }
            return true;
        }

        if (barrier.type != BarrierType::Transition)
            return true;

        const TransitionBarrier& transition = barrier.transition;
        const BarrierResource kinds = resolveBarrierResource(transition.resource);
        if (!kinds.valid())
        {
            error(prefix.str() + "the resource is not a texture, buffer, acceleration structure or micromap");
            return false;
        }
        const char* name = getResourceName(kinds);

        // keepInitialState resources: their barriers go through the tracker, which restores them at close().
        const bool keepInitialState = (kinds.texture && kinds.texture->getDesc().keepInitialState) ||
            (kinds.buffer && kinds.buffer->getDesc().keepInitialState);
        if (keepInitialState)
        {
            error(prefix.str() + "resource " + utils::DebugNameToString(name) +
                " has keepInitialState set: use setTextureState/setBufferState for it, not barriers()");
            return false;
        }

        // The 'before' state must agree with the tracker, for a resource this command list tracks. D3D11 and CUDA
        // have no tracker: they report every state as Common.
        const bool discard = (barrier.flags & BarrierFlags::Discard) != 0;
        const bool splitEnd = (barrier.flags & BarrierFlags::SplitEnd) != 0;
        const GraphicsAPI api = m_Device->getGraphicsAPI();
        const bool hasTracker = api == GraphicsAPI::D3D12 || api == GraphicsAPI::VULKAN;
        if (!hasTracker)
        {
            // nothing to compare with
        }
        else if (kinds.texture)
        {
            const TextureDesc& desc = kinds.texture->getDesc();
            const SubresourceRegion region = transition.region.resolve(desc);
            bool tracked = false;
            for (ArraySlice slice = region.baseSlice; slice < region.baseSlice + region.sliceCount && !tracked; ++slice)
            {
                for (MipLevel mip = region.baseMip; mip < region.baseMip + region.mipCount; ++mip)
                {
                    const ResourceStates state = m_CommandList->getTextureSubresourceState(kinds.texture, slice, mip);
                    if (state == ResourceStates::Unknown)
                        continue;
                    tracked = true;
                    if (!discard && !splitEnd && state != transition.before)
                    {
                        std::stringstream ss;
                        ss << prefix.str() << "texture " << utils::DebugNameToString(name) << " mip " << mip << " slice " << slice
                            << " is tracked in state " << statesToString(state) << ", but the barrier's 'before' is "
                            << statesToString(transition.before);
                        error(ss.str());
                        return false;
                    }
                    break;
                }
            }

            if (tracked && transition.region.planes != PlaneMask::All)
            {
                error(prefix.str() + "texture " + utils::DebugNameToString(name) +
                    " is tracked by this command list, and the tracker cannot represent a plane mask other than All");
                return false;
            }
        }
        else if (kinds.buffer)
        {
            const ResourceStates state = m_CommandList->getBufferState(kinds.buffer);
            if (state != ResourceStates::Unknown && !discard && !splitEnd && state != transition.before)
            {
                std::stringstream ss;
                ss << prefix.str() << "buffer " << utils::DebugNameToString(name) << " is tracked in state "
                    << statesToString(state) << ", but the barrier's 'before' is "
                    << statesToString(transition.before);
                error(ss.str());
                return false;
            }
        }

        // Split pairs must match within the command list.
        if ((barrier.flags & BarrierFlags::SplitBegin) != 0)
        {
            for (const OpenSplit& split : m_OpenSplits)
            {
                if (split.resource == transition.resource && sameRegion(split.region, transition.region))
                {
                    error(prefix.str() + "a second SplitBegin for the same resource and region before its SplitEnd");
                    return false;
                }
            }
            m_OpenSplits.push_back({ transition.resource, transition.region, transition.after });
        }
        else if (splitEnd)
        {
            auto it = std::find_if(m_OpenSplits.begin(), m_OpenSplits.end(), [&](const OpenSplit& split)
                { return split.resource == transition.resource && sameRegion(split.region, transition.region); });
            if (it == m_OpenSplits.end())
            {
                error(prefix.str() + "a SplitEnd without a SplitBegin for the same resource and region in this command list");
                return false;
            }
            if (it->after != transition.after)
            {
                error(prefix.str() + "the SplitEnd's 'after' state differs from its SplitBegin's");
                return false;
            }
            m_OpenSplits.erase(it);
        }

        // Queue-ownership transfers are matched when the command lists are submitted.
        if ((barrier.flags & (BarrierFlags::QueueRelease | BarrierFlags::QueueAcquire)) != 0)
        {
            if (transition.otherQueue == m_type)
            {
                error(prefix.str() + "a queue-ownership transfer names the command list's own queue as otherQueue");
                return false;
            }
            if ((barrier.flags & BarrierFlags::QueueRelease) != 0)
                m_QueueReleases.push_back({ transition.resource, m_type, transition.otherQueue });
            else
                m_QueueAcquires.push_back({ transition.resource, transition.otherQueue, m_type });
        }

        return true;
    }

    FRESULT CommandListWrapper::barriers(const ResourceBarrier* barriers, uint32_t count) noexcept
    {
        if (!requireOpenState())
            return FE_INVALID_ARGS;

        checkExplicitBarrierScope("barriers");

        if (validateBarrierArguments(barriers, count, m_MessageCallback) != FS_OK)
            return FE_INVALID_ARGS;

        m_UnwrappedBarriers.assign(barriers, barriers + count);

        for (uint32_t index = 0; index < count; ++index)
        {
            ResourceBarrier& barrier = m_UnwrappedBarriers[index];
            if (!validateBarrier(barrier, index))
                return FE_INVALID_ARGS;

            // Acceleration structures are wrapped by this layer; the backend needs its own objects.
            auto unwrap = [](IResource*& resource)
            {
                if (resource && resource->QueryInterface(uuid_of<AccelStructWrapper>(), nullptr) == FS_OK)
                    resource = checked_cast<rt::IAccelStruct*>(unwrapResource(resource));
            };
            switch (barrier.type)
            {
            case BarrierType::Transition: unwrap(barrier.transition.resource); break;
            case BarrierType::Uav: unwrap(barrier.uav.resource); break;
            case BarrierType::Aliasing: unwrap(barrier.aliasing.before); unwrap(barrier.aliasing.after); break;
            }
        }

        return m_CommandList->barriers(m_UnwrappedBarriers.data(), count);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Fences and submit

    FRESULT DeviceWrapper::createFence(const FenceDesc& desc, IFence** ppFence) noexcept
    {
        if (!ppFence)
        {
            error("createFence: ppFence is NULL");
            return FE_INVALID_ARGS;
        }

        return m_Device->createFence(desc, ppFence);
    }

    bool DeviceWrapper::validateFenceWait(const FenceValue& wait, const char* function)
    {
        if (!wait.fence)
        {
            error(std::string(function) + ": a wait names no fence");
            return false;
        }

        uint64_t scheduled = 0;
        {
            std::lock_guard lock(m_SubmitMutex);
            auto it = m_ScheduledFenceValues.find(wait.fence);
            if (it != m_ScheduledFenceValues.end())
                scheduled = it->second;
        }

        // A value that no submitted signal and no CPU signal will reach would block forever.
        if (wait.value > scheduled && wait.value > wait.fence->getCompletedValue())
        {
            std::stringstream ss;
            ss << function << ": waits for value " << wait.value << " of fence "
                << utils::DebugNameToString(wait.fence->getDesc().debugName)
                << ", above every signal submitted or set from the CPU so far (" << scheduled << ")";
            warning(ss.str());
        }
        return true;
    }

    uint64_t DeviceWrapper::submit(const SubmitDesc& desc) noexcept
    {
        if (desc.numCommandLists != 0 && desc.commandLists == nullptr)
        {
            error("submit: commandLists is NULL");
            return 0;
        }
        if ((desc.numWaits != 0 && !desc.waits) || (desc.numSignals != 0 && !desc.signals))
        {
            error("submit: waits or signals is NULL");
            return 0;
        }

        for (uint32_t index = 0; index < desc.numWaits; ++index)
        {
            if (!validateFenceWait(desc.waits[index], "submit"))
                return 0;
        }

        std::vector<ICommandList*> unwrappedCommandLists(desc.numCommandLists);
        std::vector<CommandListWrapper*> wrappers;

        for (uint32_t i = 0; i < desc.numCommandLists; i++)
        {
            if (desc.commandLists[i] == nullptr)
            {
                std::stringstream ss;
                ss << "submit: commandLists[" << i << "] is NULL";
                error(ss.str());
                return 0;
            }

            const CommandListParameters& listDesc = desc.commandLists[i]->getDesc();
            if (listDesc.queueType != desc.queue)
            {
                std::stringstream ss;
                ss << "submit: The command list [" << i << "] type is " << utils::CommandQueueToString(listDesc.queueType)
                    << ", it cannot be executed on a " << utils::CommandQueueToString(desc.queue) << " queue";
                error(ss.str());
                return 0;
            }

            CommandListWrapper* wrapper = queryWrapper<CommandListWrapper>(desc.commandLists[i]);
            if (wrapper)
            {
                if (!wrapper->requireExecuteState())
                    return 0;

                unwrappedCommandLists[i] = wrapper->getUnderlyingCommandList();
                wrappers.push_back(wrapper);
            }
            else
                unwrappedCommandLists[i] = desc.commandLists[i];
        }

        {
            std::lock_guard lock(m_SubmitMutex);

            // Each acquire needs a release of the same resource from the other queue, submitted earlier.
            for (CommandListWrapper* wrapper : wrappers)
            {
                for (const CommandListWrapper::QueueTransfer& acquire : wrapper->m_QueueAcquires)
                {
                    auto it = std::find_if(m_SubmittedReleases.begin(), m_SubmittedReleases.end(),
                        [&](const CommandListWrapper::QueueTransfer& release)
                        { return release.resource == acquire.resource && release.from == acquire.from && release.to == acquire.to; });
                    if (it == m_SubmittedReleases.end())
                    {
                        std::stringstream ss;
                        ss << "submit: a QueueAcquire on the " << utils::CommandQueueToString(acquire.to)
                            << " queue has no matching QueueRelease from the " << utils::CommandQueueToString(acquire.from)
                            << " queue submitted before it";
                        error(ss.str());
                    }
                    else
                        m_SubmittedReleases.erase(it);
                }
                wrapper->m_QueueAcquires.clear();

                m_SubmittedReleases.insert(m_SubmittedReleases.end(), wrapper->m_QueueReleases.begin(), wrapper->m_QueueReleases.end());
                wrapper->m_QueueReleases.clear();
            }

            for (uint32_t index = 0; index < desc.numSignals; ++index)
            {
                const FenceValue& signal = desc.signals[index];
                if (!signal.fence)
                    continue;
                uint64_t& scheduled = m_ScheduledFenceValues[signal.fence];
                if (signal.value > scheduled)
                    scheduled = signal.value;
            }
        }

        SubmitDesc unwrapped = desc;
        unwrapped.commandLists = unwrappedCommandLists.data();
        return m_Device->submit(unwrapped);
    }

    FRESULT DeviceWrapper::queueWaitFence(CommandQueue queue, IFence* fence, uint64_t value) noexcept
    {
        if (!validateFenceWait(FenceValue{ fence, value }, "queueWaitFence"))
            return FE_INVALID_ARGS;

        return m_Device->queueWaitFence(queue, fence, value);
    }

    FRESULT DeviceWrapper::queueSignalFence(CommandQueue queue, IFence* fence, uint64_t value) noexcept
    {
        if (!fence)
        {
            error("queueSignalFence: fence is NULL");
            return FE_INVALID_ARGS;
        }

        {
            std::lock_guard lock(m_SubmitMutex);
            uint64_t& scheduled = m_ScheduledFenceValues[fence];
            if (value > scheduled)
                scheduled = value;
        }

        return m_Device->queueSignalFence(queue, fence, value);
    }

    FRESULT DeviceWrapper::waitFences(const FenceValue* fences, uint32_t count, bool waitAll, uint64_t timeoutNs) noexcept
    {
        if (count != 0 && !fences)
        {
            error("waitFences: fences is NULL");
            return FE_INVALID_ARGS;
        }

        for (uint32_t index = 0; index < count; ++index)
        {
            if (!fences[index].fence)
            {
                error("waitFences: a wait names no fence");
                return FE_INVALID_ARGS;
            }
        }

        // A wait for all needs every value to be reachable; a wait for any needs one.
        bool anyReachable = false;
        for (uint32_t index = 0; index < count; ++index)
        {
            const FenceValue& wait = fences[index];
            uint64_t scheduled = 0;
            {
                std::lock_guard lock(m_SubmitMutex);
                auto it = m_ScheduledFenceValues.find(wait.fence);
                if (it != m_ScheduledFenceValues.end())
                    scheduled = it->second;
            }
            const bool reachable = wait.value <= scheduled || wait.value <= wait.fence->getCompletedValue();
            anyReachable |= reachable;
            if (waitAll && !reachable)
                validateFenceWait(wait, "waitFences");
        }
        if (!waitAll && !anyReachable)
            validateFenceWait(fences[0], "waitFences (no fence of a wait for any is reachable)");

        return m_Device->waitFences(fences, count, waitAll, timeoutNs);
    }
}
