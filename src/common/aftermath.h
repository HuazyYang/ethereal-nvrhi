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

#include <nvrhi/common/aftermath.h>
#include <nvrhi/core/foundation.h>
#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace nvrhi
{
    // Aftermath will return the payload of the last marker the GPU executed, so in cases of nested regimes,
    // we want the marker payloads to represent the whole "stack" of regimes, not just the last one
    // AftermathMarkerTracker pushes/pops regimes to this stack
    // The payload itself is a 64bit value, so AftermathMarkerTracker stores the mappings of strings<->hashes
    // There should be one AftermathMarkerTracker per graphics API-level command list
    class AftermathMarkerTracker
    {
    public:
        AftermathMarkerTracker();

        size_t pushEvent(const char* name);
        void popEvent();

        std::pair<bool, std::reference_wrapper<const std::string>> getEventString(size_t hash);
    private:
        // using a filesystem path to track the event stack since that automatically inserts "/" separators
        // and is easy to push/pop entries
        std::filesystem::path m_EventStack;

        // Some apps have unique marker text on every frame (for example, by appending the frame number to the marker)
        // In these cases, we want to cap the max number of strings stored to prevent memory usage from growing
        const static size_t MaxEventStrings = 128;
        std::array<size_t, MaxEventStrings> m_EventHashes;
        size_t m_OldestHashIndex;
        std::unordered_map<size_t, std::string> m_EventStrings;
    };

    // AftermathCrashDumpHelper tracks all nvrhi::IDevice-level constructs that we need when generating a crash dump
    // It provides two services: resolving a marker hash to the original string, and getting the specific shader bytecode
    // of a requested shader hash
    // There should be one AftermathCrashDumpHelper per nvrhi::IDevice, which holds it in an AutoPtr
    // All command lists will register their AftermathMarkerTrackers with the AftermathCrashDumpHelper
    // Any shader bytecode loading and management code (e.g. donut's ShaderFactory) should register an
    // IAftermathShaderBinaryLookup
    NVRHI_CLASS_CLSID(AftermathCrashDumpHelper, "b1ee6924-3a06-4cfa-95fc-acaac150201a")
    class AftermathCrashDumpHelper final : public ObjectImpl<IAftermathCrashDumpHelper>
    {
    public:
        NVRHI_DECLARE_UUID_TRAITS(AftermathCrashDumpHelper)
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(AftermathCrashDumpHelper)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IAftermathCrashDumpHelper)
        NVRHI_IMPLEMENTS_CLASS(AftermathCrashDumpHelper)
        NVRHI_END_INTERFACE_TABLE()

        AftermathCrashDumpHelper();

        void registerAftermathMarkerTracker(AftermathMarkerTracker* tracker);
        void unRegisterAftermathMarkerTracker(AftermathMarkerTracker* tracker);

        // IAftermathCrashDumpHelper implementation

        void registerShaderBinaryLookup(IAftermathShaderBinaryLookup* lookup) noexcept override;
        void unregisterShaderBinaryLookup(IAftermathShaderBinaryLookup* lookup) noexcept override;
        bool resolveMarker(uint64_t markerHash, const char*& outString, size_t& outLength) noexcept override;
        bool findShaderBinary(uint64_t shaderHash, PFN_AftermathShaderHashGenerator hashGenerator,
                              const void*& outBinary, size_t& outSize) noexcept override;
    private:
        std::set<AftermathMarkerTracker*> m_MarkerTrackers;
        // Command lists that are deleted on the CPU-side could still be executing (and crashing) GPU side,
        // so we keep around a small number of recently destroyed marker trackers just in case
        std::deque<AftermathMarkerTracker> m_DestroyedMarkerTrackers;
        // Keyed by the interface pointer; the lookups are not owned (see registerShaderBinaryLookup).
        std::set<IAftermathShaderBinaryLookup*> m_ShaderBinaryLookups;
    };
} // namespace nvrhi
