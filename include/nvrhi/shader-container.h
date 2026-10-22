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

/*
 * The NVRHI shader container: one file that holds the code of a Slang module for one or more targets and,
 * per target, the reflection of its parameters (ADR 0012).
 *
 * This header is the definition of the format. It is plain C and includes nothing from NVRHI, so that the
 * producer (ShaderTool) can vendor an identical copy; the aggregate build checks that the copies match.
 * Consumers inside NVRHI parse it in src/common/shader-container.cpp and expose the result through
 * <nvrhi/shader-reflection.h>.
 *
 * Layout (all integers little-endian, all offsets in bytes from the start of the container, every record
 * aligned to 8 bytes):
 *
 *   NvrhiScHeader
 *   NvrhiScSection[header.sectionCount]
 *   section payloads
 *
 * A CODE section holds the compiled code for one target (DXIL, SPIR-V, DXBC, PTX for CUDA, PTX or OptiX-IR
 * for OptiX). A REFLECTION section describes the code section with the same target:
 *
 *   NvrhiScReflectionHeader
 *   NvrhiScEntryPoint[entryPointCount]
 *   NvrhiScParameter[parameterCount]        global parameters first, then the entry points' parameters
 *   NvrhiScType[typeCount]
 *   NvrhiScField[fieldCount]
 *   string table (NUL-terminated UTF-8 strings, referenced by offset from the start of the table)
 *
 * Indices that refer to nothing are NVRHI_SC_INVALID_INDEX.
 */

#ifndef NVRHI_SHADER_CONTAINER_H
#define NVRHI_SHADER_CONTAINER_H

#include <stdint.h>

#define NVRHI_SC_MAGIC                  0x4353564Eu     /* "NVSC" */
#define NVRHI_SC_VERSION_MAJOR          1u              /* incompatible layout changes */
#define NVRHI_SC_VERSION_MINOR          0u              /* compatible additions */
#define NVRHI_SC_INVALID_INDEX          0xFFFFFFFFu
#define NVRHI_SC_UNBOUNDED_ARRAY        0xFFFFFFFFu

/* NvrhiScSection.kind */
#define NVRHI_SC_SECTION_CODE           1u
#define NVRHI_SC_SECTION_REFLECTION     2u

/* NvrhiScSection.target */
#define NVRHI_SC_TARGET_DXBC            1u
#define NVRHI_SC_TARGET_DXIL            2u
#define NVRHI_SC_TARGET_SPIRV           3u
#define NVRHI_SC_TARGET_CUDA_PTX        4u              /* compute kernels for the CUDA backend */
#define NVRHI_SC_TARGET_OPTIX_PTX       5u              /* OptiX programs as PTX */
#define NVRHI_SC_TARGET_OPTIX_IR        6u              /* OptiX programs as OptiX-IR */

/* NvrhiScEntryPoint.stage: the values of nvrhi::ShaderType */
#define NVRHI_SC_STAGE_VERTEX           0x0001u
#define NVRHI_SC_STAGE_HULL             0x0002u
#define NVRHI_SC_STAGE_DOMAIN           0x0004u
#define NVRHI_SC_STAGE_GEOMETRY         0x0008u
#define NVRHI_SC_STAGE_PIXEL            0x0010u
#define NVRHI_SC_STAGE_COMPUTE          0x0020u
#define NVRHI_SC_STAGE_AMPLIFICATION    0x0040u
#define NVRHI_SC_STAGE_MESH             0x0080u
#define NVRHI_SC_STAGE_RAY_GENERATION   0x0100u
#define NVRHI_SC_STAGE_ANY_HIT          0x0200u
#define NVRHI_SC_STAGE_CLOSEST_HIT      0x0400u
#define NVRHI_SC_STAGE_MISS             0x0800u
#define NVRHI_SC_STAGE_INTERSECTION     0x1000u
#define NVRHI_SC_STAGE_CALLABLE         0x2000u

/* NvrhiScParameter.kind: the values of nvrhi::ShaderParameterKind */
#define NVRHI_SC_PARAM_UNIFORM              0u      /* ordinary data: a global or entry-point uniform */
#define NVRHI_SC_PARAM_CONSTANT_BUFFER      1u      /* ConstantBuffer<T>, cbuffer, ParameterBlock<T> */
#define NVRHI_SC_PARAM_TEXTURE_SRV          2u      /* Texture*<T> */
#define NVRHI_SC_PARAM_TEXTURE_UAV          3u      /* RWTexture*<T> */
#define NVRHI_SC_PARAM_TYPED_BUFFER_SRV     4u      /* Buffer<T> */
#define NVRHI_SC_PARAM_TYPED_BUFFER_UAV     5u      /* RWBuffer<T> */
#define NVRHI_SC_PARAM_STRUCTURED_BUFFER_SRV 6u     /* StructuredBuffer<T> */
#define NVRHI_SC_PARAM_STRUCTURED_BUFFER_UAV 7u     /* RWStructuredBuffer<T> */
#define NVRHI_SC_PARAM_RAW_BUFFER_SRV       8u      /* ByteAddressBuffer */
#define NVRHI_SC_PARAM_RAW_BUFFER_UAV       9u      /* RWByteAddressBuffer */
#define NVRHI_SC_PARAM_SAMPLER              10u     /* SamplerState */
#define NVRHI_SC_PARAM_ACCEL_STRUCT         11u     /* RaytracingAccelerationStructure */
#define NVRHI_SC_PARAM_PUSH_CONSTANTS       12u     /* [[vk::push_constant]] ConstantBuffer<T> */

/* NvrhiScParameter.flags */
#define NVRHI_SC_PARAM_FLAG_ENTRY_POINT     0x1u    /* an entry-point parameter (kernel argument or SBT data) */

/* NvrhiScParameter.textureDimension: the values of nvrhi::TextureDimension */

/* NvrhiScType.kind: the values of nvrhi::ShaderTypeKind */
#define NVRHI_SC_TYPE_UNKNOWN           0u
#define NVRHI_SC_TYPE_SCALAR            1u
#define NVRHI_SC_TYPE_VECTOR            2u
#define NVRHI_SC_TYPE_MATRIX            3u
#define NVRHI_SC_TYPE_STRUCT            4u
#define NVRHI_SC_TYPE_ARRAY             5u
#define NVRHI_SC_TYPE_RESOURCE          6u      /* a texture, buffer, sampler or acceleration structure */
#define NVRHI_SC_TYPE_POINTER           7u      /* T* (CUDA, SPIR-V physical storage buffers) */

/* NvrhiScType.scalarType: the values of nvrhi::ShaderScalarType */
#define NVRHI_SC_SCALAR_NONE            0u
#define NVRHI_SC_SCALAR_BOOL            1u
#define NVRHI_SC_SCALAR_INT8            2u
#define NVRHI_SC_SCALAR_UINT8           3u
#define NVRHI_SC_SCALAR_INT16           4u
#define NVRHI_SC_SCALAR_UINT16          5u
#define NVRHI_SC_SCALAR_INT32           6u
#define NVRHI_SC_SCALAR_UINT32          7u
#define NVRHI_SC_SCALAR_INT64           8u
#define NVRHI_SC_SCALAR_UINT64          9u
#define NVRHI_SC_SCALAR_FLOAT16         10u
#define NVRHI_SC_SCALAR_FLOAT32         11u
#define NVRHI_SC_SCALAR_FLOAT64         12u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NvrhiScHeader
{
    uint32_t magic;             /* NVRHI_SC_MAGIC */
    uint16_t versionMajor;      /* NVRHI_SC_VERSION_MAJOR */
    uint16_t versionMinor;      /* NVRHI_SC_VERSION_MINOR */
    uint32_t headerSize;        /* sizeof(NvrhiScHeader) */
    uint32_t sectionCount;      /* NvrhiScSection records that follow the header */
    uint64_t totalSize;         /* size of the whole container */
} NvrhiScHeader;

typedef struct NvrhiScSection
{
    uint32_t kind;              /* NVRHI_SC_SECTION_* */
    uint32_t target;            /* NVRHI_SC_TARGET_* */
    uint64_t offset;            /* from the start of the container */
    uint64_t size;
} NvrhiScSection;

typedef struct NvrhiScReflectionHeader
{
    uint32_t entryPointCount;
    uint32_t entryPointOffset;  /* offsets from the start of the reflection section */
    uint32_t parameterCount;
    uint32_t parameterOffset;
    uint32_t typeCount;
    uint32_t typeOffset;
    uint32_t fieldCount;
    uint32_t fieldOffset;
    uint32_t stringTableSize;
    uint32_t stringTableOffset;
    uint32_t globalParameterCount; /* parameters [0, globalParameterCount) are the global ones */
    uint32_t globalParamsSize;  /* CUDA/OptiX: size of the SLANG_globalParams block; 0 for other targets */
    uint32_t globalParamsAlignment;
    uint32_t reserved;
} NvrhiScReflectionHeader;

typedef struct NvrhiScEntryPoint
{
    uint32_t nameOffset;        /* the Slang name of the entry point */
    uint32_t symbolNameOffset;  /* the name in the code: PTX kernel (main_0), OptiX program (__raygen__main) */
    uint32_t stage;             /* NVRHI_SC_STAGE_* */
    uint32_t threadGroupSize[3];
    uint32_t firstParameter;    /* entry-point uniform parameters: indices into the parameter table */
    uint32_t parameterCount;
    uint32_t paramsSize;        /* CUDA: size of the kernel argument block; OptiX: size of the SBT record data */
    uint32_t paramsAlignment;
} NvrhiScEntryPoint;

typedef struct NvrhiScParameter
{
    uint32_t nameOffset;
    uint32_t kind;              /* NVRHI_SC_PARAM_* */
    uint32_t typeIndex;         /* the type of the parameter (for a constant buffer or a structured buffer: of
                                   its element), or NVRHI_SC_INVALID_INDEX */
    uint32_t arraySize;         /* 0: not an array; NVRHI_SC_UNBOUNDED_ARRAY: unbounded */
    uint32_t space;             /* D3D register space / Vulkan descriptor set */
    uint32_t slot;              /* D3D register / Vulkan binding */
    uint32_t uniformOffset;     /* CUDA/OptiX: offset in the global block (or in the entry-point block);
                                   other targets: offset of uniform data in its constant buffer */
    uint32_t uniformSize;       /* bytes the parameter occupies in that block (all array elements) */
    uint32_t uniformStride;     /* bytes between array elements in that block */
    uint32_t textureDimension;  /* nvrhi::TextureDimension of a texture, 0 otherwise */
    uint32_t flags;             /* NVRHI_SC_PARAM_FLAG_* */
    uint32_t reserved;
} NvrhiScParameter;

typedef struct NvrhiScType
{
    uint32_t nameOffset;
    uint32_t kind;              /* NVRHI_SC_TYPE_* */
    uint32_t scalarType;        /* NVRHI_SC_SCALAR_* of a scalar, vector or matrix */
    uint32_t rowCount;          /* matrix rows; 1 for vectors and scalars */
    uint32_t columnCount;       /* matrix columns or vector elements; 1 for scalars */
    uint32_t size;              /* bytes in the target's layout */
    uint32_t alignment;
    uint32_t stride;            /* array element stride */
    uint32_t elementCount;      /* arrays: element count (NVRHI_SC_UNBOUNDED_ARRAY: unbounded) */
    uint32_t elementType;       /* arrays and pointers: the element type, or NVRHI_SC_INVALID_INDEX */
    uint32_t firstField;        /* structs: index into the field table */
    uint32_t fieldCount;
} NvrhiScType;

typedef struct NvrhiScField
{
    uint32_t nameOffset;
    uint32_t typeIndex;
    uint32_t offset;            /* byte offset in the struct, in the target's layout */
    uint32_t reserved;
} NvrhiScField;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NVRHI_SHADER_CONTAINER_H */
