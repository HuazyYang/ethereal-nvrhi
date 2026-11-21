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

// Helpers for ICommandList::barriers that the backends share (ADR 0015).

#pragma once

#include <nvrhi/nvrhi.h>

namespace nvrhi
{
    // The derived interface of a barrier's resource. QueryInterface with a null ppv only reports whether the
    // object answers the IID: it returns no pointer and adds no reference (ADR 0006), so resolving a barrier
    // costs one virtual call and a short table walk. nullptr when the resource is of another kind.
    template <typename TInterface>
    TInterface* queryResource(IResource* resource)
    {
        if (resource && resource->QueryInterface(uuid_of<TInterface>(), nullptr) == FS_OK)
            return static_cast<TInterface*>(resource);
        return nullptr;
    }

    // The kinds of resource a barrier can name, resolved once per barrier. Textures are tested first: they are
    // most barriers.
    struct BarrierResource
    {
        ITexture* texture = nullptr;
        IBuffer* buffer = nullptr;
        ISamplerFeedbackTexture* samplerFeedback = nullptr;
        rt::IAccelStruct* accelStruct = nullptr;
        rt::IOpacityMicromap* opacityMicromap = nullptr;
        IStagingTexture* stagingTexture = nullptr;

        [[nodiscard]] bool valid() const
        {
            return texture || buffer || samplerFeedback || accelStruct || opacityMicromap || stagingTexture;
        }
    };

    BarrierResource resolveBarrierResource(IResource* resource);

    // The checks every backend makes before recording: a known type, a resource on every transition and on the
    // 'after' side of an aliasing barrier, at most one of the split flags and one of the queue flags.
    // FS_OK, or FE_INVALID_ARGS with a message through 'messageCallback'.
    FRESULT validateBarrierArguments(const ResourceBarrier* barriers, uint32_t count, IMessageCallback* messageCallback);

    // The plane slices of a texture format that a plane mask selects, as a bit mask (bit 0: plane slice 0,
    // bit 1: plane slice 1). DX12 puts depth in plane 0 and stencil in plane 1.
    uint32_t getPlaneSliceMask(PlaneMask planes, bool formatHasDepth, bool formatHasStencil);
}
