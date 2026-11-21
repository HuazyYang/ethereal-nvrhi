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

#include "resource-barrier.h"

#include <cstddef>
#include <sstream>
#include <type_traits>

namespace nvrhi
{
    // Layout of the barrier types (ADR 0015). Plans copy barrier arrays with memcpy, so the types must stay
    // trivially copyable; the user-provided default constructor of ResourceBarrier does not affect that.
    static_assert(std::is_trivially_copyable_v<SubresourceRegion>);
    static_assert(std::is_trivially_copyable_v<TransitionBarrier>);
    static_assert(std::is_trivially_copyable_v<ResourceBarrier>);

    static_assert(sizeof(SubresourceRegion) == 20);
    static_assert(offsetof(SubresourceRegion, mipCount) == 4);
    static_assert(offsetof(SubresourceRegion, baseSlice) == 8);
    static_assert(offsetof(SubresourceRegion, sliceCount) == 12);
    static_assert(offsetof(SubresourceRegion, planes) == 16);

    static_assert(sizeof(TransitionBarrier) == 48);
    static_assert(offsetof(TransitionBarrier, region) == 8);
    static_assert(offsetof(TransitionBarrier, before) == 28);
    static_assert(offsetof(TransitionBarrier, after) == 32);
    static_assert(offsetof(TransitionBarrier, stagesBefore) == 36);
    static_assert(offsetof(TransitionBarrier, stagesAfter) == 38);
    static_assert(offsetof(TransitionBarrier, otherQueue) == 40);

    static_assert(sizeof(UavBarrier) == 8);
    static_assert(sizeof(AliasingBarrier) == 16);
    static_assert(offsetof(AliasingBarrier, after) == 8);

    static_assert(sizeof(ResourceBarrier) == 56);
    static_assert(offsetof(ResourceBarrier, flags) == 1);
    static_assert(offsetof(ResourceBarrier, transition) == 8);
    static_assert(offsetof(ResourceBarrier, uav) == 8);
    static_assert(offsetof(ResourceBarrier, aliasing) == 8);

    BarrierResource resolveBarrierResource(IResource* resource)
    {
        BarrierResource result;
        if (!resource)
            return result;

        if ((result.texture = queryResource<ITexture>(resource)) != nullptr)
            return result;
        if ((result.buffer = queryResource<IBuffer>(resource)) != nullptr)
            return result;
        if ((result.accelStruct = queryResource<rt::IAccelStruct>(resource)) != nullptr)
            return result;
        if ((result.samplerFeedback = queryResource<ISamplerFeedbackTexture>(resource)) != nullptr)
            return result;
        if ((result.opacityMicromap = queryResource<rt::IOpacityMicromap>(resource)) != nullptr)
            return result;
        result.stagingTexture = queryResource<IStagingTexture>(resource);
        return result;
    }

    static bool isSingleFlagOrNone(BarrierFlags flags, BarrierFlags a, BarrierFlags b)
    {
        return (flags & (a | b)) != (a | b);
    }

    FRESULT validateBarrierArguments(const ResourceBarrier* barriers, uint32_t count, IMessageCallback* messageCallback)
    {
        if (count != 0 && !barriers)
        {
            messageCallback->message(MessageSeverity::Error, "barriers: the barrier array is null");
            return FE_INVALID_ARGS;
        }

        for (uint32_t index = 0; index < count; ++index)
        {
            const ResourceBarrier& barrier = barriers[index];
            const char* problem = nullptr;

            switch (barrier.type)
            {
            case BarrierType::Transition:
                if (!barrier.transition.resource)
                    problem = "a transition names no resource";
                else if (!isSingleFlagOrNone(barrier.flags, BarrierFlags::SplitBegin, BarrierFlags::SplitEnd))
                    problem = "a transition is both SplitBegin and SplitEnd";
                else if (!isSingleFlagOrNone(barrier.flags, BarrierFlags::QueueRelease, BarrierFlags::QueueAcquire))
                    problem = "a transition is both QueueRelease and QueueAcquire";
                else if (barrier.transition.otherQueue >= CommandQueue::Count)
                    problem = "a transition names an unknown otherQueue";
                break;

            case BarrierType::Uav:
                if (barrier.flags != BarrierFlags::None)
                    problem = "a UAV barrier carries flags";
                break;

            case BarrierType::Aliasing:
                if (!barrier.aliasing.after)
                    problem = "an aliasing barrier names no 'after' resource";
                else if (barrier.flags != BarrierFlags::None)
                    problem = "an aliasing barrier carries flags";
                break;

            default:
                problem = "unknown BarrierType";
                break;
            }

            if (problem)
            {
                std::stringstream ss;
                ss << "barriers: barrier " << index << " is malformed: " << problem << "; nothing was recorded";
                messageCallback->message(MessageSeverity::Error, ss.str().c_str());
                return FE_INVALID_ARGS;
            }
        }

        return FS_OK;
    }

    uint32_t getPlaneSliceMask(PlaneMask planes, bool formatHasDepth, bool formatHasStencil)
    {
        if (!formatHasDepth && !formatHasStencil)
            return (planes & PlaneMask::Color) != 0 ? 0x1 : 0x0;

        uint32_t mask = 0;
        if (formatHasDepth && (planes & PlaneMask::Depth) != 0)
            mask |= 0x1;
        if (formatHasStencil && (planes & PlaneMask::Stencil) != 0)
            mask |= formatHasDepth ? 0x2 : 0x1;
        return mask;
    }
}
