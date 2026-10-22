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

#pragma once

// Reading NVRHI shader containers (<nvrhi/shader-container.h>, ADR 0012) in the backends.

#include <nvrhi/shader-reflection.h>
#include <nvrhi/core/autoptr.h>

#include <initializer_list>
#include <string>

namespace nvrhi
{
    // A shader binary as a backend sees it: plain bytecode, or the code section of a container for one of the
    // backend's targets together with the reflection of that target.
    struct ShaderBinary
    {
        const void* code = nullptr;     // points into the binary passed to OpenShaderBinary
        size_t codeSize = 0;
        ShaderCodeTarget target = ShaderCodeTarget::Unknown;   // Unknown: plain bytecode
        MonoPtr<ShaderLibraryReflection> reflection;           // null: plain bytecode, or no reflection section
    };

    // If 'binary' is a container, selects the code section of the first of 'targets' that it has and parses the
    // reflection of that target. Anything else is returned as plain bytecode. Returns false with a message if
    // the container is malformed or has none of the targets.
    bool OpenShaderBinary(const void* binary, size_t binarySize, std::initializer_list<ShaderCodeTarget> targets,
        ShaderBinary& outBinary, std::string& outError);

    // Parses one reflection section.
    bool ParseShaderReflection(const void* section, size_t sectionSize, ShaderCodeTarget target,
        ShaderLibraryReflection& outReflection, std::string& outError);

    // The entry point by Slang name (or by its symbol name in the code), with the given stage unless stage is
    // ShaderType::None. nullptr if there is none.
    const ShaderEntryPointReflection* FindEntryPoint(const ShaderLibraryReflection* reflection, const char* name,
        ShaderType stage = ShaderType::None);
}
