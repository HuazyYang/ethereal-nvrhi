/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
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

#pragma once

#include <nvrhi/nvrhi.h>

// Aftermath crash dump support, as COM interfaces: the implementation (marker tracking, the shader binary
// lookups) lives in nvrhi, one helper per device (IDevice::getAftermathCrashDumpHelper()).

namespace nvrhi
{
    // Computes the Aftermath shader hash of a shader binary. Stateless: a plain function pointer.
    typedef uint64_t (*PFN_AftermathShaderHashGenerator)(const void* binary, size_t size, GraphicsAPI api);

    // Implemented by clients that load and own shader binaries (e.g. donut's ShaderFactory), and registered
    // with IAftermathCrashDumpHelper::registerShaderBinaryLookup.
    NVRHI_IID(IAftermathShaderBinaryLookup, "4de6fb70-31e0-4b4f-b2b0-29f035808b42")
    struct IAftermathShaderBinaryLookup : IObject
    {
        NVRHI_DECLARE_UUID_TRAITS(IAftermathShaderBinaryLookup)
        // Finds the binary whose hash (computed with hashGenerator) is shaderHash. Returns false, leaving the
        // outputs unchanged, if this lookup has no such binary. The binary stays owned by the lookup.
        virtual bool findShaderBinary(uint64_t shaderHash, PFN_AftermathShaderHashGenerator hashGenerator,
                                      const void*& outBinary, size_t& outSize) noexcept = 0;
    };

    // Tracks the IDevice-level state needed to decode a crash dump: it resolves a marker hash to the original
    // marker string, and finds the shader binary of a shader hash through the registered lookups.
    // Implemented by nvrhi, one per device.
    NVRHI_IID(IAftermathCrashDumpHelper, "c271a497-7fe3-47d5-a348-0002fde05761")
    struct IAftermathCrashDumpHelper : IObject
    {
        NVRHI_DECLARE_UUID_TRAITS(IAftermathCrashDumpHelper)
        // Non-owning, like a raw client key: the client unregisters before it is destroyed (no reference
        // cycle between the device and the client).
        virtual void registerShaderBinaryLookup(IAftermathShaderBinaryLookup* lookup) noexcept = 0;
        virtual void unregisterShaderBinaryLookup(IAftermathShaderBinaryLookup* lookup) noexcept = 0;
        // The string stays owned by nvrhi and is valid until the device is destroyed. When the marker is not
        // found, returns false and an error message string.
        virtual bool resolveMarker(uint64_t markerHash, const char*& outString, size_t& outLength) noexcept = 0;
        // Asks the registered lookups in turn. Returns false, leaving the outputs unchanged, if none has it.
        virtual bool findShaderBinary(uint64_t shaderHash, PFN_AftermathShaderHashGenerator hashGenerator,
                                      const void*& outBinary, size_t& outSize) noexcept = 0;
    };
} // namespace nvrhi
