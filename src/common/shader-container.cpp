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

#include "shader-container.h"

#include <nvrhi/shader-container.h>

#include <cstring>

namespace nvrhi
{
    namespace
    {
        // A bounds-checked view of a byte range.
        struct ByteRange
        {
            const uint8_t* data = nullptr;
            size_t size = 0;

            bool contains(uint64_t offset, uint64_t length) const
            {
                return offset <= size && length <= size - offset;
            }

            template <typename T>
            const T* array(uint64_t offset, uint64_t count) const
            {
                if (count > (UINT64_MAX / sizeof(T)) || !contains(offset, count * sizeof(T)))
                    return nullptr;
                return reinterpret_cast<const T*>(data + offset);
            }
        };

        bool ReadString(const ByteRange& strings, uint32_t offset, string& out)
        {
            if (offset >= strings.size)
                return false;
            const char* begin = reinterpret_cast<const char*>(strings.data + offset);
            const void* end = memchr(begin, 0, strings.size - offset);
            if (!end)
                return false;
            out = string(begin, static_cast<const char*>(end) - begin);
            return true;
        }

        bool ValidIndex(uint32_t index, uint32_t count)
        {
            return index == NVRHI_SC_INVALID_INDEX || index < count;
        }

        bool ValidRange(uint32_t first, uint32_t count, uint32_t total)
        {
            return first <= total && count <= total - first;
        }

        const NvrhiScHeader* ReadHeader(const void* binary, size_t binarySize)
        {
            if (!binary || binarySize < sizeof(NvrhiScHeader))
                return nullptr;
            NvrhiScHeader header;
            memcpy(&header, binary, sizeof(header));
            if (header.magic != NVRHI_SC_MAGIC)
                return nullptr;
            return static_cast<const NvrhiScHeader*>(binary);
        }

        // Validates the header and the section table; returns the sections or nullptr with a message.
        const NvrhiScSection* ReadSections(const void* binary, size_t binarySize, uint32_t& outCount, std::string& error)
        {
            const NvrhiScHeader* header = ReadHeader(binary, binarySize);
            if (!header)
            {
                error = "not an NVRHI shader container";
                return nullptr;
            }
            if (header->versionMajor != NVRHI_SC_VERSION_MAJOR)
            {
                error = "unsupported NVRHI shader container version " + std::to_string(header->versionMajor);
                return nullptr;
            }
            if (header->headerSize < sizeof(NvrhiScHeader) || header->totalSize > binarySize)
            {
                error = "truncated NVRHI shader container";
                return nullptr;
            }

            ByteRange all{ static_cast<const uint8_t*>(binary), size_t(header->totalSize) };
            const NvrhiScSection* sections = all.array<NvrhiScSection>(header->headerSize, header->sectionCount);
            if (!sections)
            {
                error = "NVRHI shader container: bad section table";
                return nullptr;
            }
            for (uint32_t i = 0; i < header->sectionCount; ++i)
            {
                if (!all.contains(sections[i].offset, sections[i].size))
                {
                    error = "NVRHI shader container: section " + std::to_string(i) + " is out of bounds";
                    return nullptr;
                }
            }
            outCount = header->sectionCount;
            return sections;
        }

        const NvrhiScSection* FindSection(const NvrhiScSection* sections, uint32_t count, uint32_t kind, uint32_t target)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                if (sections[i].kind == kind && sections[i].target == target)
                    return &sections[i];
            }
            return nullptr;
        }
    }

    bool ParseShaderReflection(const void* section, size_t sectionSize, ShaderCodeTarget target,
        ShaderLibraryReflection& out, std::string& error)
    {
        const ByteRange range{ static_cast<const uint8_t*>(section), sectionSize };
        const NvrhiScReflectionHeader* header = range.array<NvrhiScReflectionHeader>(0, 1);
        if (!header)
        {
            error = "NVRHI shader container: truncated reflection";
            return false;
        }

        const NvrhiScEntryPoint* entryPoints = range.array<NvrhiScEntryPoint>(header->entryPointOffset, header->entryPointCount);
        const NvrhiScParameter* parameters = range.array<NvrhiScParameter>(header->parameterOffset, header->parameterCount);
        const NvrhiScType* types = range.array<NvrhiScType>(header->typeOffset, header->typeCount);
        const NvrhiScField* fields = range.array<NvrhiScField>(header->fieldOffset, header->fieldCount);
        if ((header->entryPointCount && !entryPoints) || (header->parameterCount && !parameters) ||
            (header->typeCount && !types) || (header->fieldCount && !fields) ||
            !range.contains(header->stringTableOffset, header->stringTableSize) ||
            header->globalParameterCount > header->parameterCount)
        {
            error = "NVRHI shader container: reflection tables out of bounds";
            return false;
        }
        const ByteRange strings{ range.data + header->stringTableOffset, header->stringTableSize };

        out = ShaderLibraryReflection();
        out.target = target;
        out.globalParameterCount = header->globalParameterCount;
        out.globalParamsSize = header->globalParamsSize;
        out.globalParamsAlignment = header->globalParamsAlignment;

        out.entryPoints.resize(header->entryPointCount);
        for (uint32_t i = 0; i < header->entryPointCount; ++i)
        {
            const NvrhiScEntryPoint& src = entryPoints[i];
            ShaderEntryPointReflection& dst = out.entryPoints[i];
            if (!ReadString(strings, src.nameOffset, dst.name) ||
                !ReadString(strings, src.symbolNameOffset, dst.symbolName) ||
                !ValidRange(src.firstParameter, src.parameterCount, header->parameterCount))
            {
                error = "NVRHI shader container: bad entry point " + std::to_string(i);
                return false;
            }
            dst.stage = ShaderType(src.stage);
            for (int axis = 0; axis < 3; ++axis)
                dst.threadGroupSize[axis] = src.threadGroupSize[axis];
            dst.firstParameter = src.firstParameter;
            dst.parameterCount = src.parameterCount;
            dst.paramsSize = src.paramsSize;
            dst.paramsAlignment = src.paramsAlignment;
        }

        out.parameters.resize(header->parameterCount);
        for (uint32_t i = 0; i < header->parameterCount; ++i)
        {
            const NvrhiScParameter& src = parameters[i];
            ShaderParameterReflection& dst = out.parameters[i];
            if (!ReadString(strings, src.nameOffset, dst.name) || !ValidIndex(src.typeIndex, header->typeCount) ||
                src.kind > NVRHI_SC_PARAM_PUSH_CONSTANTS)
            {
                error = "NVRHI shader container: bad parameter " + std::to_string(i);
                return false;
            }
            dst.kind = ShaderParameterKind(src.kind);
            dst.textureDimension = TextureDimension(src.textureDimension);
            dst.isEntryPointParameter = (src.flags & NVRHI_SC_PARAM_FLAG_ENTRY_POINT) != 0;
            dst.typeIndex = src.typeIndex;
            dst.arraySize = src.arraySize;
            dst.registerSpace = src.space;
            dst.registerSlot = src.slot;
            dst.uniformOffset = src.uniformOffset;
            dst.uniformSize = src.uniformSize;
            dst.uniformStride = src.uniformStride;
        }

        out.types.resize(header->typeCount);
        for (uint32_t i = 0; i < header->typeCount; ++i)
        {
            const NvrhiScType& src = types[i];
            ShaderTypeReflection& dst = out.types[i];
            if (!ReadString(strings, src.nameOffset, dst.name) || !ValidIndex(src.elementType, header->typeCount) ||
                !ValidRange(src.firstField, src.fieldCount, header->fieldCount) || src.kind > NVRHI_SC_TYPE_POINTER ||
                src.scalarType > NVRHI_SC_SCALAR_FLOAT64 || src.rowCount > 255 || src.columnCount > 255)
            {
                error = "NVRHI shader container: bad type " + std::to_string(i);
                return false;
            }
            dst.kind = ShaderTypeKind(src.kind);
            dst.scalarType = ShaderScalarType(src.scalarType);
            dst.rowCount = uint8_t(src.rowCount);
            dst.columnCount = uint8_t(src.columnCount);
            dst.size = src.size;
            dst.alignment = src.alignment;
            dst.stride = src.stride;
            dst.elementCount = src.elementCount;
            dst.elementType = src.elementType;
            dst.firstField = src.firstField;
            dst.fieldCount = src.fieldCount;
        }

        out.fields.resize(header->fieldCount);
        for (uint32_t i = 0; i < header->fieldCount; ++i)
        {
            const NvrhiScField& src = fields[i];
            ShaderFieldReflection& dst = out.fields[i];
            if (!ReadString(strings, src.nameOffset, dst.name) || !ValidIndex(src.typeIndex, header->typeCount))
            {
                error = "NVRHI shader container: bad field " + std::to_string(i);
                return false;
            }
            dst.typeIndex = src.typeIndex;
            dst.offset = src.offset;
        }

        return true;
    }

    bool OpenShaderBinary(const void* binary, size_t binarySize, std::initializer_list<ShaderCodeTarget> targets,
        ShaderBinary& out, std::string& error)
    {
        out = ShaderBinary();

        if (!ReadHeader(binary, binarySize))
        {
            out.code = binary;
            out.codeSize = binarySize;
            return true;
        }

        uint32_t sectionCount = 0;
        const NvrhiScSection* sections = ReadSections(binary, binarySize, sectionCount, error);
        if (!sections)
            return false;

        for (ShaderCodeTarget target : targets)
        {
            const NvrhiScSection* code = FindSection(sections, sectionCount, NVRHI_SC_SECTION_CODE, uint32_t(target));
            if (!code)
                continue;

            out.code = static_cast<const uint8_t*>(binary) + code->offset;
            out.codeSize = size_t(code->size);
            out.target = target;

            const NvrhiScSection* reflection = FindSection(sections, sectionCount, NVRHI_SC_SECTION_REFLECTION, uint32_t(target));
            if (reflection)
            {
                out.reflection = MakeMono<ShaderLibraryReflection>();
                if (!ParseShaderReflection(static_cast<const uint8_t*>(binary) + reflection->offset,
                    size_t(reflection->size), target, *out.reflection, error))
                    return false;
            }
            return true;
        }

        error = "the NVRHI shader container has no code for this backend";
        return false;
    }

    const ShaderEntryPointReflection* FindEntryPoint(const ShaderLibraryReflection* reflection, const char* name,
        ShaderType stage)
    {
        if (!reflection || !name)
            return nullptr;
        for (const ShaderEntryPointReflection& entryPoint : reflection->entryPoints)
        {
            if (stage != ShaderType::None && entryPoint.stage != stage)
                continue;
            if (strcmp(entryPoint.name.c_str(), name) == 0 || strcmp(entryPoint.symbolName.c_str(), name) == 0)
                return &entryPoint;
        }
        return nullptr;
    }

    const ShaderParameterReflection* nvrhiFindShaderParameter(const ShaderLibraryReflection* reflection,
        const char* name) noexcept
    {
        if (!reflection || !name)
            return nullptr;
        for (uint32_t i = 0; i < reflection->globalParameterCount && i < reflection->parameters.size(); ++i)
        {
            if (strcmp(reflection->parameters[i].name.c_str(), name) == 0)
                return &reflection->parameters[i];
        }
        return nullptr;
    }

    const ShaderEntryPointReflection* nvrhiFindShaderEntryPoint(const ShaderLibraryReflection* reflection,
        const char* name) noexcept
    {
        return FindEntryPoint(reflection, name);
    }

    const ShaderParameterReflection* nvrhiFindShaderEntryPointParameter(const ShaderLibraryReflection* reflection,
        const ShaderEntryPointReflection* entryPoint, const char* name) noexcept
    {
        if (!reflection || !entryPoint || !name)
            return nullptr;
        for (uint32_t i = 0; i < entryPoint->parameterCount; ++i)
        {
            const uint32_t index = entryPoint->firstParameter + i;
            if (index < reflection->parameters.size() && strcmp(reflection->parameters[index].name.c_str(), name) == 0)
                return &reflection->parameters[index];
        }
        return nullptr;
    }

    const ShaderFieldReflection* nvrhiFindShaderField(const ShaderLibraryReflection* reflection, uint32_t typeIndex,
        const char* name) noexcept
    {
        if (!reflection || !name || typeIndex >= reflection->types.size())
            return nullptr;
        const ShaderTypeReflection& type = reflection->types[typeIndex];
        for (uint32_t i = 0; i < type.fieldCount; ++i)
        {
            const uint32_t index = type.firstField + i;
            if (index < reflection->fields.size() && strcmp(reflection->fields[index].name.c_str(), name) == 0)
                return &reflection->fields[index];
        }
        return nullptr;
    }

    bool nvrhiIsShaderContainer(const void* data, size_t size) noexcept
    {
        return ReadHeader(data, size) != nullptr;
    }

    bool nvrhiGetShaderContainerCode(const void* data, size_t size, ShaderCodeTarget target, const void** ppCode,
        size_t* pCodeSize) noexcept
    {
        if (ppCode) *ppCode = nullptr;
        if (pCodeSize) *pCodeSize = 0;

        try
        {
            std::string error;
            uint32_t sectionCount = 0;
            const NvrhiScSection* sections = ReadSections(data, size, sectionCount, error);
            if (!sections)
                return false;
            const NvrhiScSection* code = FindSection(sections, sectionCount, NVRHI_SC_SECTION_CODE, uint32_t(target));
            if (!code)
                return false;
            if (ppCode) *ppCode = static_cast<const uint8_t*>(data) + code->offset;
            if (pCodeSize) *pCodeSize = size_t(code->size);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
}
