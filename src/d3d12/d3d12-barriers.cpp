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

// ICommandList::barriers on D3D12 (ADR 0015): one ResourceBarrier call with legacy barriers, one Barrier call with
// enhanced barriers. Queue-ownership flags have no D3D12 equivalent and are ignored.

#include "d3d12-backend.h"
#include "../common/resource-barrier.h"

#include <nvrhi/common/misc.h>

namespace nvrhi::d3d12
{
    namespace
    {
        // The D3D12 objects behind a barrier's resource.
        struct BarrierTarget
        {
            ID3D12Resource* resource = nullptr;
            const TextureDesc* textureDesc = nullptr;   // textures and sampler feedback
            uint8_t planeCount = 1;
            bool hasDepth = false;
            bool hasStencil = false;
            TextureStateExtension* textureState = nullptr;
            BufferStateExtension* bufferState = nullptr;
            bool isAccelStruct = false;
        };

        BarrierTarget getBarrierTarget(IResource* resource)
        {
            BarrierTarget target;
            const BarrierResource kinds = resolveBarrierResource(resource);

            if (kinds.texture)
            {
                Texture* texture = checked_cast<Texture*>(kinds.texture);
                const FormatInfo& formatInfo = getFormatInfo(texture->desc.format);
                target.resource = texture->resource;
                target.textureDesc = &texture->desc;
                target.planeCount = texture->planeCount;
                target.hasDepth = formatInfo.hasDepth;
                target.hasStencil = formatInfo.hasStencil;
                target.textureState = texture;
            }
            else if (kinds.buffer)
            {
                Buffer* buffer = checked_cast<Buffer*>(kinds.buffer);
                target.resource = buffer->resource;
                target.bufferState = buffer;
            }
            else if (kinds.accelStruct)
            {
                AccelStruct* as = checked_cast<AccelStruct*>(kinds.accelStruct);
                if (as->dataBuffer)
                {
                    target.resource = as->dataBuffer->resource;
                    target.bufferState = as->dataBuffer;
                }
                target.isAccelStruct = true;
            }
            else if (kinds.samplerFeedback)
            {
                SamplerFeedbackTexture* texture = checked_cast<SamplerFeedbackTexture*>(kinds.samplerFeedback);
                target.resource = texture->resource;
                target.textureDesc = &texture->textureDesc;
                target.textureState = texture;
            }
            else if (kinds.opacityMicromap)
            {
                OpacityMicromap* omm = checked_cast<OpacityMicromap*>(kinds.opacityMicromap);
                if (omm->dataBuffer)
                {
                    target.resource = omm->dataBuffer->resource;
                    target.bufferState = omm->dataBuffer;
                }
                target.isAccelStruct = true;
            }
            else if (kinds.stagingTexture)
            {
                StagingTexture* texture = checked_cast<StagingTexture*>(kinds.stagingTexture);
                if (texture->buffer)
                    target.resource = texture->buffer->resource;
            }

            return target;
        }

        uint32_t getArraySize(const TextureDesc& desc)
        {
            return desc.dimension == TextureDimension::Texture3D ? 1 : desc.arraySize;
        }

        uint32_t getPlaneMask(const BarrierTarget& target, PlaneMask planes)
        {
            if (target.planeCount <= 1 && !target.hasStencil)
                return 0x1;
            return getPlaneSliceMask(planes, target.hasDepth, target.hasStencil);
        }

        bool coversAllPlanes(const BarrierTarget& target, uint32_t planeMask)
        {
            return planeMask == (1u << target.planeCount) - 1u;
        }

        // A texture state D3D12 can hold: one layout must admit every access of the set. Read sets fit
        // (DIRECT_QUEUE_GENERIC_READ at most); a write or UAV access combined with another one does not, and a
        // legacy write state cannot be combined either. Checked on both paths, so that the two agree.
        bool isTextureStateRepresentable(ResourceStates state)
        {
            if (state == ResourceStates::Unknown)
                return true;
            return convertResourceStatesForExplicitBarriers(state, true, ShaderType::None).layout != D3D12_BARRIER_LAYOUT_UNDEFINED;
        }
    }

    FRESULT CommandList::barriers(const ResourceBarrier* barriers, uint32_t count) noexcept
    {
        const FRESULT valid = validateBarrierArguments(barriers, count, m_Context.messageCallback);
        if (valid != FS_OK)
            return valid;

        if (count == 0)
            return FS_OK;

        for (uint32_t index = 0; index < count; ++index)
        {
            const ResourceBarrier& barrier = barriers[index];
            if (barrier.type != BarrierType::Transition)
                continue;
            const TransitionBarrier& transition = barrier.transition;
            if (!getBarrierTarget(transition.resource).textureDesc)
                continue;
            if (!isTextureStateRepresentable(transition.before) || !isTextureStateRepresentable(transition.after))
            {
                m_Context.error("ICommandList::barriers: barrier " + std::to_string(index) + " combines texture states "
                    "that no D3D12 layout serves together (a write or UnorderedAccess with another state); transition "
                    "between them instead");
                return FE_INVALID_ARGS;
            }
        }

        commitBarriers();

        if (m_Device->GetEnhancedBarriersSupported())
            recordEnhancedBarriers(barriers, count);
        else
            recordLegacyBarriers(barriers, count);

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

    void CommandList::recordLegacyBarriers(const ResourceBarrier* barriers, uint32_t count)
    {
        m_D3DBarriers.clear();
        m_D3DDiscards.clear();

        for (uint32_t index = 0; index < count; ++index)
        {
            const ResourceBarrier& barrier = barriers[index];
            D3D12_RESOURCE_BARRIER d3dbarrier{};

            if (barrier.type == BarrierType::Uav)
            {
                d3dbarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                d3dbarrier.UAV.pResource = barrier.uav.resource ? getBarrierTarget(barrier.uav.resource).resource : nullptr;
                m_D3DBarriers.push_back(d3dbarrier);
                continue;
            }

            if (barrier.type == BarrierType::Aliasing)
            {
                d3dbarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                d3dbarrier.Aliasing.pResourceBefore = barrier.aliasing.before ? getBarrierTarget(barrier.aliasing.before).resource : nullptr;
                d3dbarrier.Aliasing.pResourceAfter = getBarrierTarget(barrier.aliasing.after).resource;
                m_D3DBarriers.push_back(d3dbarrier);
                continue;
            }

            const TransitionBarrier& transition = barrier.transition;
            const BarrierTarget target = getBarrierTarget(transition.resource);
            if (!target.resource)
                continue;

            const D3D12_RESOURCE_STATES stateBefore = convertResourceStates(transition.before);
            const D3D12_RESOURCE_STATES stateAfter = convertResourceStates(transition.after);
            const bool discard = (barrier.flags & BarrierFlags::Discard) != 0;

            // Legacy acceleration-structure buffers never leave their state; the caller orders them with UAV barriers.
            const bool fixedState = (stateBefore & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) != 0 ||
                (stateAfter & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) != 0;
            const bool transitionNeeded = stateBefore != stateAfter && !fixedState;

            d3dbarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            if ((barrier.flags & BarrierFlags::SplitBegin) != 0)
                d3dbarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
            else if ((barrier.flags & BarrierFlags::SplitEnd) != 0)
                d3dbarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
            d3dbarrier.Transition.StateBefore = stateBefore;
            d3dbarrier.Transition.StateAfter = stateAfter;
            d3dbarrier.Transition.pResource = target.resource;

            if (!target.textureDesc)
            {
                if (transitionNeeded)
                {
                    d3dbarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    m_D3DBarriers.push_back(d3dbarrier);
                }
                if (discard && (barrier.flags & BarrierFlags::SplitBegin) == 0)
                    m_D3DDiscards.push_back({ target.resource, 0, 0, true });
                continue;
            }

            const TextureDesc& desc = *target.textureDesc;
            const SubresourceRegion region = transition.region.resolve(desc);
            const uint32_t planeMask = getPlaneMask(target, transition.region.planes);
            const uint32_t arraySize = getArraySize(desc);
            const bool wholeResource = region.coversAllSubresources(desc) && coversAllPlanes(target, planeMask);

            if (transitionNeeded)
            {
                if (wholeResource)
                {
                    d3dbarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    m_D3DBarriers.push_back(d3dbarrier);
                }
                else
                {
                    for (uint32_t plane = 0; plane < target.planeCount; ++plane)
                    {
                        if ((planeMask & (1u << plane)) == 0)
                            continue;
                        for (ArraySlice slice = region.baseSlice; slice < region.baseSlice + region.sliceCount; ++slice)
                        {
                            for (MipLevel mip = region.baseMip; mip < region.baseMip + region.mipCount; ++mip)
                            {
                                d3dbarrier.Transition.Subresource = calcSubresource(mip, slice, plane, desc.mipLevels, arraySize);
                                m_D3DBarriers.push_back(d3dbarrier);
                            }
                        }
                    }
                }
            }

            // DiscardResource after the transition, one contiguous run of mips per slice and plane.
            if (discard && (barrier.flags & BarrierFlags::SplitBegin) == 0)
            {
                if (wholeResource)
                {
                    m_D3DDiscards.push_back({ target.resource, 0, 0, true });
                }
                else
                {
                    for (uint32_t plane = 0; plane < target.planeCount; ++plane)
                    {
                        if ((planeMask & (1u << plane)) == 0)
                            continue;
                        for (ArraySlice slice = region.baseSlice; slice < region.baseSlice + region.sliceCount; ++slice)
                        {
                            const uint32_t first = calcSubresource(region.baseMip, slice, plane, desc.mipLevels, arraySize);
                            m_D3DDiscards.push_back({ target.resource, first, region.mipCount, false });
                        }
                    }
                }
            }
        }

        if (!m_D3DBarriers.empty())
            m_ActiveCommandList->commandList->ResourceBarrier(uint32_t(m_D3DBarriers.size()), m_D3DBarriers.data());

        for (const PendingDiscard& discard : m_D3DDiscards)
        {
            if (discard.wholeResource)
            {
                m_ActiveCommandList->commandList->DiscardResource(discard.resource, nullptr);
            }
            else
            {
                D3D12_DISCARD_REGION region{ 0, nullptr, discard.firstSubresource, discard.numSubresources };
                m_ActiveCommandList->commandList->DiscardResource(discard.resource, &region);
            }
        }
    }

    void CommandList::recordEnhancedBarriers(const ResourceBarrier* barriers, uint32_t count)
    {
        assert(m_ActiveCommandList->commandList7);

        m_D3DTextureBarriers.clear();
        m_D3DBufferBarriers.clear();
        m_D3DGlobalBarriers.clear();

        for (uint32_t index = 0; index < count; ++index)
        {
            const ResourceBarrier& barrier = barriers[index];

            if (barrier.type == BarrierType::Uav)
            {
                const BarrierTarget target = barrier.uav.resource ? getBarrierTarget(barrier.uav.resource) : BarrierTarget();
                D3D12_BARRIER_SYNC const sync = target.isAccelStruct
                    ? D3D12_BARRIER_SYNC_ALL_SHADING | D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE
                    : D3D12_BARRIER_SYNC_ALL_SHADING;
                D3D12_BARRIER_ACCESS const access = target.isAccelStruct
                    ? D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE
                    : D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;

                if (!target.resource)
                {
                    D3D12_GLOBAL_BARRIER& d3dbarrier = m_D3DGlobalBarriers.emplace_back();
                    d3dbarrier.SyncBefore = sync;
                    d3dbarrier.SyncAfter = sync;
                    d3dbarrier.AccessBefore = access;
                    d3dbarrier.AccessAfter = access;
                }
                else if (target.textureDesc)
                {
                    D3D12_TEXTURE_BARRIER& d3dbarrier = m_D3DTextureBarriers.emplace_back();
                    d3dbarrier.SyncBefore = sync;
                    d3dbarrier.SyncAfter = sync;
                    d3dbarrier.AccessBefore = access;
                    d3dbarrier.AccessAfter = access;
                    d3dbarrier.LayoutBefore = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
                    d3dbarrier.LayoutAfter = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
                    d3dbarrier.pResource = target.resource;
                    d3dbarrier.Subresources.IndexOrFirstMipLevel = 0xffffffff;   // all subresources
                }
                else
                {
                    D3D12_BUFFER_BARRIER& d3dbarrier = m_D3DBufferBarriers.emplace_back();
                    d3dbarrier.SyncBefore = sync;
                    d3dbarrier.SyncAfter = sync;
                    d3dbarrier.AccessBefore = access;
                    d3dbarrier.AccessAfter = access;
                    d3dbarrier.pResource = target.resource;
                    d3dbarrier.Offset = 0;
                    d3dbarrier.Size = UINT64_MAX;
                }
                continue;
            }

            if (barrier.type == BarrierType::Aliasing)
            {
                // An execution dependency on all earlier work. The memory dependency comes from the outgoing
                // resources' transitions to ResourceStates::Unknown (no access), which the caller places before
                // it, and the incoming resource starts with a Discard transition (design 6.7).
                D3D12_GLOBAL_BARRIER& d3dbarrier = m_D3DGlobalBarriers.emplace_back();
                d3dbarrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
                d3dbarrier.SyncAfter = D3D12_BARRIER_SYNC_ALL;
                d3dbarrier.AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
                d3dbarrier.AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS;
                continue;
            }

            const TransitionBarrier& transition = barrier.transition;
            const BarrierTarget target = getBarrierTarget(transition.resource);
            if (!target.resource)
                continue;

            const bool isTexture = target.textureDesc != nullptr;
            EnhancedResourceStateMapping before = convertResourceStatesForExplicitBarriers(transition.before, isTexture, transition.stagesBefore);
            EnhancedResourceStateMapping after = convertResourceStatesForExplicitBarriers(transition.after, isTexture, transition.stagesAfter);
            const bool discard = (barrier.flags & BarrierFlags::Discard) != 0;

            // The contents before are undefined. The real 'before' sync stays, so the discard still waits for
            // earlier work on the memory; the access and layout go.
            if (discard)
            {
                before.access = D3D12_BARRIER_ACCESS_NO_ACCESS;
                before.layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
            }

            if ((barrier.flags & BarrierFlags::SplitBegin) != 0)
                after.sync = D3D12_BARRIER_SYNC_SPLIT;
            else if ((barrier.flags & BarrierFlags::SplitEnd) != 0)
                before.sync = D3D12_BARRIER_SYNC_SPLIT;

            if (!isTexture)
            {
                D3D12_BUFFER_BARRIER& d3dbarrier = m_D3DBufferBarriers.emplace_back();
                d3dbarrier.SyncBefore = before.sync;
                d3dbarrier.SyncAfter = after.sync;
                d3dbarrier.AccessBefore = before.access;
                d3dbarrier.AccessAfter = after.access;
                d3dbarrier.pResource = target.resource;
                d3dbarrier.Offset = 0;
                d3dbarrier.Size = UINT64_MAX;
                continue;
            }

            const TextureDesc& desc = *target.textureDesc;
            const SubresourceRegion region = transition.region.resolve(desc);
            const uint32_t planeMask = getPlaneMask(target, transition.region.planes);
            if (planeMask == 0)
                continue;

            D3D12_TEXTURE_BARRIER& d3dbarrier = m_D3DTextureBarriers.emplace_back();
            d3dbarrier.SyncBefore = before.sync;
            d3dbarrier.SyncAfter = after.sync;
            d3dbarrier.AccessBefore = before.access;
            d3dbarrier.AccessAfter = after.access;
            d3dbarrier.LayoutBefore = before.layout;
            d3dbarrier.LayoutAfter = after.layout;
            d3dbarrier.pResource = target.resource;
            d3dbarrier.Subresources.IndexOrFirstMipLevel = region.baseMip;
            d3dbarrier.Subresources.NumMipLevels = region.mipCount;
            d3dbarrier.Subresources.FirstArraySlice = region.baseSlice;
            d3dbarrier.Subresources.NumArraySlices = region.sliceCount;
            d3dbarrier.Subresources.FirstPlane = (planeMask & 0x1) ? 0 : 1;
            d3dbarrier.Subresources.NumPlanes = (planeMask == 0x3) ? 2 : 1;
            d3dbarrier.Flags = discard ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD : D3D12_TEXTURE_BARRIER_FLAG_NONE;
        }

        static_vector<D3D12_BARRIER_GROUP, 3> barrierGroups;

        if (!m_D3DGlobalBarriers.empty())
        {
            D3D12_BARRIER_GROUP& barrierGroup = barrierGroups.emplace_back();
            barrierGroup.Type = D3D12_BARRIER_TYPE_GLOBAL;
            barrierGroup.NumBarriers = uint32_t(m_D3DGlobalBarriers.size());
            barrierGroup.pGlobalBarriers = m_D3DGlobalBarriers.data();
        }

        if (!m_D3DTextureBarriers.empty())
        {
            D3D12_BARRIER_GROUP& barrierGroup = barrierGroups.emplace_back();
            barrierGroup.Type = D3D12_BARRIER_TYPE_TEXTURE;
            barrierGroup.NumBarriers = uint32_t(m_D3DTextureBarriers.size());
            barrierGroup.pTextureBarriers = m_D3DTextureBarriers.data();
        }

        if (!m_D3DBufferBarriers.empty())
        {
            D3D12_BARRIER_GROUP& barrierGroup = barrierGroups.emplace_back();
            barrierGroup.Type = D3D12_BARRIER_TYPE_BUFFER;
            barrierGroup.NumBarriers = uint32_t(m_D3DBufferBarriers.size());
            barrierGroup.pBufferBarriers = m_D3DBufferBarriers.data();
        }

        if (!barrierGroups.empty())
            m_ActiveCommandList->commandList7->Barrier(uint32_t(barrierGroups.size()), barrierGroups.data());
    }

} // namespace nvrhi::d3d12
