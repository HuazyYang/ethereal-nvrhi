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

// ICommandList::barriers on Vulkan (ADR 0015): one vkCmdPipelineBarrier2 for the joined barriers, and
// vkCmdSetEvent2 / vkCmdWaitEvents2 for each split pair. Planes are aspect masks, queue-ownership transfers use
// the real queue-family indices, and like commitBarriers the call ends the dynamic-rendering pass.

#include "vulkan-backend.h"
#include "../common/resource-barrier.h"

#include <nvrhi/common/misc.h>
#include <algorithm>

namespace nvrhi::vulkan
{
    vk::Event TrackedCommandBuffer::acquireSplitEvent()
    {
        if (splitEventsInUse == splitEvents.size())
        {
            auto info = vk::EventCreateInfo().setFlags(vk::EventCreateFlagBits::eDeviceOnly);
            splitEvents.push_back(m_Context.device.createEvent(info, m_Context.allocationCallbacks));
        }
        return splitEvents[splitEventsInUse++];
    }

    namespace
    {
        // The Vulkan objects behind a barrier's resource.
        struct BarrierTarget
        {
            Texture* texture = nullptr;
            vk::Buffer buffer;
            TextureStateExtension* textureState = nullptr;
            BufferStateExtension* bufferState = nullptr;
        };

        BarrierTarget getBarrierTarget(IResource* resource)
        {
            BarrierTarget target;
            const BarrierResource kinds = resolveBarrierResource(resource);

            if (kinds.texture)
            {
                target.texture = checked_cast<Texture*>(kinds.texture);
                target.textureState = target.texture;
            }
            else if (kinds.buffer)
            {
                Buffer* buffer = checked_cast<Buffer*>(kinds.buffer);
                target.buffer = buffer->buffer;
                target.bufferState = buffer;
            }
            else if (kinds.accelStruct)
            {
                AccelStruct* as = checked_cast<AccelStruct*>(kinds.accelStruct);
                if (as->dataBuffer)
                {
                    Buffer* buffer = checked_cast<Buffer*>(as->dataBuffer.Get());
                    target.buffer = buffer->buffer;
                    target.bufferState = buffer;
                }
            }
            else if (kinds.opacityMicromap)
            {
                OpacityMicromap* omm = checked_cast<OpacityMicromap*>(kinds.opacityMicromap);
                if (omm->dataBuffer)
                {
                    Buffer* buffer = checked_cast<Buffer*>(omm->dataBuffer.Get());
                    target.buffer = buffer->buffer;
                    target.bufferState = buffer;
                }
            }

            return target;
        }

        vk::ImageAspectFlags getAspectMask(const TextureDesc& desc, PlaneMask planes)
        {
            const FormatInfo& formatInfo = getFormatInfo(desc.format);
            vk::ImageAspectFlags mask = vk::ImageAspectFlags();
            if (formatInfo.hasDepth || formatInfo.hasStencil)
            {
                if (formatInfo.hasDepth && (planes & PlaneMask::Depth) != 0)
                    mask |= vk::ImageAspectFlagBits::eDepth;
                if (formatInfo.hasStencil && (planes & PlaneMask::Stencil) != 0)
                    mask |= vk::ImageAspectFlagBits::eStencil;
            }
            else if ((planes & PlaneMask::Color) != 0)
            {
                mask = vk::ImageAspectFlagBits::eColor;
            }
            return mask;
        }
    }

    FRESULT CommandList::barriers(const ResourceBarrier* barriers, uint32_t count) noexcept
    {
        const FRESULT valid = validateBarrierArguments(barriers, count, m_Context.messageCallback);
        if (valid != FS_OK)
            return valid;

        if (count == 0)
            return FS_OK;

        endRenderPass();

        if (anyBarriers())
            commitBarriersInternal();

        m_ExplicitImageBarriers.clear();
        m_ExplicitBufferBarriers.clear();
        m_ExplicitMemoryBarriers.clear();

        const uint32_t ownFamily = m_Device->getQueue(m_CommandListParameters.queueType)->getQueueFamilyIndex();

        for (uint32_t index = 0; index < count; ++index)
        {
            const ResourceBarrier& barrier = barriers[index];

            if (barrier.type == BarrierType::Uav)
            {
                // A memory dependency between shader writes and later shader reads and writes.
                m_ExplicitMemoryBarriers.push_back(vk::MemoryBarrier2()
                    .setSrcStageMask(vk::PipelineStageFlagBits2::eAllCommands)
                    .setSrcAccessMask(vk::AccessFlagBits2::eShaderWrite)
                    .setDstStageMask(vk::PipelineStageFlagBits2::eAllCommands)
                    .setDstAccessMask(vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite));
                continue;
            }

            if (barrier.type == BarrierType::Aliasing)
            {
                // The first scope covers every stage and access the outgoing resources may have used (design 6.7).
                m_ExplicitMemoryBarriers.push_back(vk::MemoryBarrier2()
                    .setSrcStageMask(vk::PipelineStageFlagBits2::eAllCommands)
                    .setSrcAccessMask(vk::AccessFlagBits2::eMemoryWrite)
                    .setDstStageMask(vk::PipelineStageFlagBits2::eAllCommands)
                    .setDstAccessMask(vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite));
                continue;
            }

            const TransitionBarrier& transition = barrier.transition;
            const BarrierTarget target = getBarrierTarget(transition.resource);
            const bool isImage = target.texture != nullptr;
            if (!isImage && !target.buffer)
                continue;

            ResourceStateMapping before = convertResourceStateForExplicitBarriers(transition.before, isImage, transition.stagesBefore);
            ResourceStateMapping after = convertResourceStateForExplicitBarriers(transition.after, isImage, transition.stagesAfter);
            const bool discard = (barrier.flags & BarrierFlags::Discard) != 0;

            // A transition to no access (ResourceStates::Unknown) ends the resource's use, as the outgoing side of an
            // aliasing barrier does. Vulkan has no layout for that (newLayout must not be UNDEFINED), so it is a
            // memory dependency from the last accesses, and the layout stays.
            if (transition.after == ResourceStates::Unknown &&
                (barrier.flags & (BarrierFlags::QueueRelease | BarrierFlags::QueueAcquire | BarrierFlags::SplitBegin | BarrierFlags::SplitEnd)) == 0)
            {
                m_ExplicitMemoryBarriers.push_back(vk::MemoryBarrier2()
                    .setSrcStageMask(before.stageFlags ? before.stageFlags : vk::PipelineStageFlagBits2::eAllCommands)
                    .setSrcAccessMask(before.accessMask)
                    .setDstStageMask(vk::PipelineStageFlagBits2::eAllCommands)
                    .setDstAccessMask(vk::AccessFlags2()));
                continue;
            }

            if (discard)
            {
                before.imageLayout = vk::ImageLayout::eUndefined;
                // Unknown prior use: wait for everything, so the discard never races earlier work on the memory.
                if (transition.before == ResourceStates::Unknown)
                {
                    before.stageFlags = vk::PipelineStageFlagBits2::eAllCommands;
                    before.accessMask = vk::AccessFlagBits2::eMemoryWrite;
                }
            }

            uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED;
            uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED;
            if ((barrier.flags & (BarrierFlags::QueueRelease | BarrierFlags::QueueAcquire)) != 0)
            {
                const uint32_t otherFamily = m_Device->getQueue(transition.otherQueue)
                    ? m_Device->getQueue(transition.otherQueue)->getQueueFamilyIndex() : ownFamily;

                if (otherFamily == ownFamily)
                {
                    // One family: no ownership to transfer. The release half does the layout change and the semaphore
                    // between the queues makes it visible; the acquire half has nothing to do.
                    if ((barrier.flags & BarrierFlags::QueueAcquire) != 0)
                        continue;
                    after.stageFlags = vk::PipelineStageFlagBits2::eNone;
                    after.accessMask = vk::AccessFlags2();
                }
                else if ((barrier.flags & BarrierFlags::QueueRelease) != 0)
                {
                    srcFamily = ownFamily;
                    dstFamily = otherFamily;
                    // The release half's second scope is ignored; it names no stage of the other queue.
                    after.stageFlags = vk::PipelineStageFlagBits2::eNone;
                    after.accessMask = vk::AccessFlags2();
                }
                else
                {
                    srcFamily = otherFamily;
                    dstFamily = ownFamily;
                    before.stageFlags = vk::PipelineStageFlagBits2::eNone;
                    before.accessMask = vk::AccessFlags2();
                }
            }

            vk::ImageMemoryBarrier2 imageBarrier;
            vk::BufferMemoryBarrier2 bufferBarrier;

            if (isImage)
            {
                const TextureDesc& desc = target.texture->desc;
                const SubresourceRegion region = transition.region.resolve(desc);
                vk::ImageAspectFlags aspectMask = getAspectMask(desc, transition.region.planes);
                if (!aspectMask)
                    continue;

                // One plane of a depth-stencil image needs separateDepthStencilLayouts; without it the barrier must
                // name both aspects.
                const FormatInfo& formatInfo = getFormatInfo(desc.format);
                const vk::ImageAspectFlags bothPlanes = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
                if (formatInfo.hasDepth && formatInfo.hasStencil && aspectMask != bothPlanes && !m_Context.separateDepthStencilLayouts)
                {
                    m_Context.error("barriers: a barrier on one plane of a depth-stencil texture needs the "
                        "separateDepthStencilLayouts feature; it was widened to both planes");
                    aspectMask = bothPlanes;
                }

                imageBarrier = vk::ImageMemoryBarrier2()
                    .setSrcStageMask(before.stageFlags)
                    .setSrcAccessMask(before.accessMask)
                    .setDstStageMask(after.stageFlags)
                    .setDstAccessMask(after.accessMask)
                    .setOldLayout(before.imageLayout)
                    .setNewLayout(after.imageLayout)
                    .setSrcQueueFamilyIndex(srcFamily)
                    .setDstQueueFamilyIndex(dstFamily)
                    .setImage(target.texture->image)
                    .setSubresourceRange(vk::ImageSubresourceRange()
                        .setAspectMask(aspectMask)
                        .setBaseMipLevel(region.baseMip)
                        .setLevelCount(region.mipCount)
                        .setBaseArrayLayer(region.baseSlice)
                        .setLayerCount(region.sliceCount));
            }
            else
            {
                bufferBarrier = vk::BufferMemoryBarrier2()
                    .setSrcStageMask(before.stageFlags)
                    .setSrcAccessMask(before.accessMask)
                    .setDstStageMask(after.stageFlags)
                    .setDstAccessMask(after.accessMask)
                    .setSrcQueueFamilyIndex(srcFamily)
                    .setDstQueueFamilyIndex(dstFamily)
                    .setBuffer(target.buffer)
                    .setOffset(0)
                    .setSize(VK_WHOLE_SIZE);
            }

            const bool splitBegin = (barrier.flags & BarrierFlags::SplitBegin) != 0;
            const bool splitEnd = (barrier.flags & BarrierFlags::SplitEnd) != 0;
            if (!splitBegin && !splitEnd)
            {
                if (isImage)
                    m_ExplicitImageBarriers.push_back(imageBarrier);
                else
                    m_ExplicitBufferBarriers.push_back(bufferBarrier);
                continue;
            }

            // A split pair: vkCmdSetEvent2 and vkCmdWaitEvents2 take the same dependency.
            vk::DependencyInfo dependency;
            if (isImage)
                dependency.setImageMemoryBarriers(imageBarrier);
            else
                dependency.setBufferMemoryBarriers(bufferBarrier);

            if (splitBegin)
            {
                const vk::Event event = m_CurrentCmdBuf->acquireSplitEvent();
                m_CurrentCmdBuf->cmdBuf.setEvent2(event, dependency);
                m_OpenSplits.push_back({ transition.resource, transition.region, event });
            }
            else
            {
                auto it = std::find_if(m_OpenSplits.begin(), m_OpenSplits.end(), [&](const OpenSplit& split)
                {
                    return split.resource == transition.resource && split.region.baseMip == transition.region.baseMip &&
                        split.region.mipCount == transition.region.mipCount && split.region.baseSlice == transition.region.baseSlice &&
                        split.region.sliceCount == transition.region.sliceCount && split.region.planes == transition.region.planes;
                });

                if (it == m_OpenSplits.end())
                {
                    // No matching begin in this recording (the validation layer reports it): a joined barrier.
                    if (isImage)
                        m_ExplicitImageBarriers.push_back(imageBarrier);
                    else
                        m_ExplicitBufferBarriers.push_back(bufferBarrier);
                    continue;
                }

                m_CurrentCmdBuf->cmdBuf.waitEvents2(it->event, dependency);
                m_CurrentCmdBuf->cmdBuf.resetEvent2(it->event, after.stageFlags ? after.stageFlags : vk::PipelineStageFlagBits2::eAllCommands);
                m_OpenSplits.erase(it);
            }
        }

        if (!m_ExplicitImageBarriers.empty() || !m_ExplicitBufferBarriers.empty() || !m_ExplicitMemoryBarriers.empty())
        {
            vk::DependencyInfo dependency;
            dependency.setImageMemoryBarriers(m_ExplicitImageBarriers);
            dependency.setBufferMemoryBarriers(m_ExplicitBufferBarriers);
            dependency.setMemoryBarriers(m_ExplicitMemoryBarriers);
            m_CurrentCmdBuf->cmdBuf.pipelineBarrier2(dependency);
        }

        // The tracker follows the barriers for the resources it tracks.
        for (uint32_t index = 0; index < count; ++index)
        {
            const ResourceBarrier& barrier = barriers[index];

            if (barrier.type == BarrierType::Aliasing)
            {
                if (barrier.aliasing.before)
                {
                    const BarrierTarget before = getBarrierTarget(barrier.aliasing.before);
                    if (before.textureState)
                        m_StateTracker.forgetTextureState(before.textureState);
                    else if (before.bufferState)
                        m_StateTracker.forgetBufferState(before.bufferState);
                }
                continue;
            }

            if (barrier.type != BarrierType::Transition || (barrier.flags & BarrierFlags::SplitBegin) != 0)
                continue;

            const BarrierTarget target = getBarrierTarget(barrier.transition.resource);
            if (target.textureState)
                m_StateTracker.setTrackedTextureState(target.textureState, barrier.transition.region.toSubresourceSet(),
                    barrier.transition.after);
            else if (target.bufferState)
                m_StateTracker.setTrackedBufferState(target.bufferState, barrier.transition.after);
        }

        return FS_OK;
    }

} // namespace nvrhi::vulkan
