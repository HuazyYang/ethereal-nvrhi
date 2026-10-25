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

// Reflection of Slang code compiled offline into an NVRHI shader container (<nvrhi/shader-container.h>,
// ADR 0012). A shader library or shader created from a container returns it through
// IShaderLibrary::getReflection, IShader::getReflection and IShader::getLibraryReflection; one created from
// plain bytecode returns nullptr.
//
// The reflection describes the code target the backend uses (DXIL on D3D12, SPIR-V on Vulkan, PTX on CUDA):
// offsets and sizes are in that target's layout. The structures are owned by the library or shader that
// returns them and live as long as it does.

#include <nvrhi/nvrhi.h>

namespace nvrhi
{
    // The kind of code a container section holds; the values of NVRHI_SC_TARGET_*.
    enum class ShaderCodeTarget : uint8_t
    {
        Unknown     = 0,
        DXBC        = 1,
        DXIL        = 2,
        SPIRV       = 3,
        CudaPTX     = 4,
        OptixPTX    = 5,
        OptixIR     = 6,
    };

    // The values of NVRHI_SC_PARAM_*.
    enum class ShaderParameterKind : uint8_t
    {
        Uniform                 = 0,    // ordinary data: a global or entry-point uniform
        ConstantBuffer          = 1,    // ConstantBuffer<T>, cbuffer, ParameterBlock<T>
        Texture_SRV             = 2,
        Texture_UAV             = 3,
        TypedBuffer_SRV         = 4,
        TypedBuffer_UAV         = 5,
        StructuredBuffer_SRV    = 6,
        StructuredBuffer_UAV    = 7,
        RawBuffer_SRV           = 8,
        RawBuffer_UAV           = 9,
        Sampler                 = 10,
        RayTracingAccelStruct   = 11,
        PushConstants           = 12,
    };

    // The values of NVRHI_SC_TYPE_*.
    enum class ShaderTypeKind : uint8_t
    {
        Unknown     = 0,
        Scalar      = 1,
        Vector      = 2,
        Matrix      = 3,
        Struct      = 4,
        Array       = 5,
        Resource    = 6,
        Pointer     = 7,
    };

    // The values of NVRHI_SC_SCALAR_*.
    enum class ShaderScalarType : uint8_t
    {
        None    = 0,
        Bool    = 1,
        Int8    = 2,
        UInt8   = 3,
        Int16   = 4,
        UInt16  = 5,
        Int32   = 6,
        UInt32  = 7,
        Int64   = 8,
        UInt64  = 9,
        Float16 = 10,
        Float32 = 11,
        Float64 = 12,
    };

    static constexpr uint32_t c_ShaderReflectionInvalidIndex = 0xFFFFFFFFu;
    static constexpr uint32_t c_ShaderReflectionUnboundedArray = 0xFFFFFFFFu;

    // A field of a struct type.
    struct ShaderFieldReflection
    {
        string name;
        uint32_t typeIndex = c_ShaderReflectionInvalidIndex;   // into ShaderLibraryReflection::types
        uint32_t offset = 0;                                    // bytes from the start of the struct
    };

    // A type, in the layout of the reflected code target.
    struct ShaderTypeReflection
    {
        string name;
        ShaderTypeKind kind = ShaderTypeKind::Unknown;
        ShaderScalarType scalarType = ShaderScalarType::None;  // scalars, vectors and matrices
        uint8_t rowCount = 1;                                   // matrix rows
        uint8_t columnCount = 1;                                // vector elements or matrix columns
        uint32_t size = 0;
        uint32_t alignment = 0;
        uint32_t stride = 0;                                    // array element stride
        uint32_t elementCount = 0;                              // arrays (c_ShaderReflectionUnboundedArray)
        uint32_t elementType = c_ShaderReflectionInvalidIndex;  // arrays and pointers
        uint32_t firstField = 0;                                // structs: into ShaderLibraryReflection::fields
        uint32_t fieldCount = 0;
    };

    // A global or entry-point parameter.
    struct ShaderParameterReflection
    {
        string name;
        ShaderParameterKind kind = ShaderParameterKind::Uniform;
        TextureDimension textureDimension = TextureDimension::Unknown;
        bool isEntryPointParameter = false;
        uint32_t typeIndex = c_ShaderReflectionInvalidIndex;   // element type of buffers, the type of uniforms
        uint32_t arraySize = 0;                                 // 0: not an array (c_ShaderReflectionUnboundedArray)
        uint32_t registerSpace = 0;                             // D3D register space / Vulkan descriptor set
        uint32_t registerSlot = 0;                              // D3D register / Vulkan binding
        // CUDA/OptiX: where the parameter lives in the global parameter block, or for an entry-point parameter in
        // the kernel argument block / SBT record data. Other targets: offset of uniform data in its constant buffer.
        uint32_t uniformOffset = 0;
        uint32_t uniformSize = 0;
        uint32_t uniformStride = 0;                             // between array elements
    };

    struct ShaderEntryPointReflection
    {
        string name;                    // the Slang name
        string symbolName;              // the name in the code (PTX kernel, OptiX program, ...)
        ShaderType stage = ShaderType::None;
        uint32_t threadGroupSize[3] = { 0, 0, 0 };
        uint32_t firstParameter = 0;    // into ShaderLibraryReflection::parameters
        uint32_t parameterCount = 0;
        uint32_t paramsSize = 0;        // CUDA: kernel arguments (exact size); OptiX: SBT record data; others: push constants
        uint32_t paramsAlignment = 0;
    };

    struct ShaderLibraryReflection
    {
        ShaderCodeTarget target = ShaderCodeTarget::Unknown;
        uint32_t globalParameterCount = 0;      // parameters [0, globalParameterCount) are the global ones
        uint32_t globalParamsSize = 0;          // CUDA/OptiX: size of the SLANG_globalParams block
        uint32_t globalParamsAlignment = 0;
        vector<ShaderEntryPointReflection> entryPoints;
        vector<ShaderParameterReflection> parameters;
        vector<ShaderTypeReflection> types;
        vector<ShaderFieldReflection> fields;
    };

    // Lookups. They return a pointer into 'reflection', or nullptr when nothing matches (or reflection is null).

    // A global parameter by name.
    NVRHI_C_API const ShaderParameterReflection* nvrhiFindShaderParameter(const ShaderLibraryReflection* reflection,
        const char* name) noexcept;
    // An entry point by its Slang name.
    NVRHI_C_API const ShaderEntryPointReflection* nvrhiFindShaderEntryPoint(const ShaderLibraryReflection* reflection,
        const char* name) noexcept;
    // An entry-point parameter by name.
    NVRHI_C_API const ShaderParameterReflection* nvrhiFindShaderEntryPointParameter(
        const ShaderLibraryReflection* reflection, const ShaderEntryPointReflection* entryPoint,
        const char* name) noexcept;
    // A field of the struct type 'typeIndex' by name.
    NVRHI_C_API const ShaderFieldReflection* nvrhiFindShaderField(const ShaderLibraryReflection* reflection,
        uint32_t typeIndex, const char* name) noexcept;

    inline const ShaderParameterReflection* findShaderParameter(const ShaderLibraryReflection* reflection,
        const char* name)
    {
        return nvrhiFindShaderParameter(reflection, name);
    }

    inline const ShaderEntryPointReflection* findShaderEntryPoint(const ShaderLibraryReflection* reflection,
        const char* name)
    {
        return nvrhiFindShaderEntryPoint(reflection, name);
    }

    inline const ShaderParameterReflection* findShaderEntryPointParameter(const ShaderLibraryReflection* reflection,
        const ShaderEntryPointReflection* entryPoint, const char* name)
    {
        return nvrhiFindShaderEntryPointParameter(reflection, entryPoint, name);
    }

    inline const ShaderFieldReflection* findShaderField(const ShaderLibraryReflection* reflection,
        uint32_t typeIndex, const char* name)
    {
        return nvrhiFindShaderField(reflection, typeIndex, name);
    }

    // True if the data starts like an NVRHI shader container (the magic number); a cheap test that does not
    // validate the rest.
    NVRHI_C_API bool nvrhiIsShaderContainer(const void* data, size_t size) noexcept;

    // Finds the code section for 'target' in a container. Returns false if the data is not a valid container or
    // has no such section.
    NVRHI_C_API bool nvrhiGetShaderContainerCode(const void* data, size_t size, ShaderCodeTarget target,
        const void** ppCode, size_t* pCodeSize) noexcept;

    inline bool isShaderContainer(const void* data, size_t size) { return nvrhiIsShaderContainer(data, size); }

    //////////////////////////////////////////////////////////////////////////
    // Layout checks (section 4.6 of doc/com-interface-abi.md)
    //////////////////////////////////////////////////////////////////////////

    static_assert(sizeof(ShaderFieldReflection) == sizeof(string) + 8);
    static_assert(offsetof(ShaderFieldReflection, typeIndex) == sizeof(string));
    static_assert(offsetof(ShaderTypeReflection, kind) == sizeof(string));
    static_assert(offsetof(ShaderTypeReflection, size) == sizeof(string) + 4);
    static_assert(sizeof(ShaderTypeReflection) == sizeof(string) + 32);
    static_assert(offsetof(ShaderParameterReflection, kind) == sizeof(string));
    static_assert(offsetof(ShaderParameterReflection, typeIndex) == sizeof(string) + 4);
    static_assert(sizeof(ShaderParameterReflection) == sizeof(string) + 32);
    static_assert(offsetof(ShaderEntryPointReflection, stage) == 2 * sizeof(string));
    static_assert(offsetof(ShaderEntryPointReflection, threadGroupSize) == 2 * sizeof(string) + 4);
    static_assert(sizeof(ShaderEntryPointReflection) == 2 * sizeof(string) + 32);
    static_assert(offsetof(ShaderLibraryReflection, entryPoints) == 16);
    static_assert(offsetof(ShaderLibraryReflection, parameters) == 16 + sizeof(vector<ShaderEntryPointReflection>));
}
