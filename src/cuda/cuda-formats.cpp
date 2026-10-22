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

#include "cuda-backend.h"

namespace nvrhi::cuda
{
    namespace
    {
        // Integer formats read as integers; UNORM/SNORM formats are stored as the integer formats of the same size
        // and read as normalized floats (no CU_TRSF_READ_AS_INTEGER). SINT formats use the signed array formats.
        // BGRA formats keep their memory order: CUDA has no swizzle, so a texture read returns B in .x.
        constexpr CUarray_format U8 = CU_AD_FORMAT_UNSIGNED_INT8;
        constexpr CUarray_format U16 = CU_AD_FORMAT_UNSIGNED_INT16;
        constexpr CUarray_format U32 = CU_AD_FORMAT_UNSIGNED_INT32;
        constexpr CUarray_format S8 = CU_AD_FORMAT_SIGNED_INT8;
        constexpr CUarray_format S16 = CU_AD_FORMAT_SIGNED_INT16;
        constexpr CUarray_format S32 = CU_AD_FORMAT_SIGNED_INT32;
        constexpr CUarray_format F16 = CU_AD_FORMAT_HALF;
        constexpr CUarray_format F32 = CU_AD_FORMAT_FLOAT;
        constexpr CUarray_format None = CUarray_format(0);

        //                 format                     array format              ch  bytes blk norm   srgb   int
        const FormatMapping c_Formats[] = {
            { Format::UNKNOWN,            None,                         0,  0,  1, false, false, false },
            { Format::R8_UINT,            U8,                           1,  1,  1, false, false, true  },
            { Format::R8_SINT,            S8,                           1,  1,  1, false, false, true  },
            { Format::R8_UNORM,           U8,                           1,  1,  1, true,  false, false },
            { Format::R8_SNORM,           S8,                           1,  1,  1, true,  false, false },
            { Format::RG8_UINT,           U8,                           2,  2,  1, false, false, true  },
            { Format::RG8_SINT,           S8,                           2,  2,  1, false, false, true  },
            { Format::RG8_UNORM,          U8,                           2,  2,  1, true,  false, false },
            { Format::RG8_SNORM,          S8,                           2,  2,  1, true,  false, false },
            { Format::R16_UINT,           U16,                          1,  2,  1, false, false, true  },
            { Format::R16_SINT,           S16,                          1,  2,  1, false, false, true  },
            { Format::R16_UNORM,          U16,                          1,  2,  1, true,  false, false },
            { Format::R16_SNORM,          S16,                          1,  2,  1, true,  false, false },
            { Format::R16_FLOAT,          F16,                          1,  2,  1, false, false, false },
            { Format::BGRA4_UNORM,        None,                         0,  2,  1, false, false, false },
            { Format::B5G6R5_UNORM,       None,                         0,  2,  1, false, false, false },
            { Format::B5G5R5A1_UNORM,     None,                         0,  2,  1, false, false, false },
            { Format::RGBA8_UINT,         U8,                           4,  4,  1, false, false, true  },
            { Format::RGBA8_SINT,         S8,                           4,  4,  1, false, false, true  },
            { Format::RGBA8_UNORM,        U8,                           4,  4,  1, true,  false, false },
            { Format::RGBA8_SNORM,        S8,                           4,  4,  1, true,  false, false },
            { Format::BGRA8_UNORM,        U8,                           4,  4,  1, true,  false, false },
            { Format::BGRX8_UNORM,        U8,                           4,  4,  1, true,  false, false },
            { Format::SRGBA8_UNORM,       U8,                           4,  4,  1, true,  true,  false },
            { Format::SBGRA8_UNORM,       U8,                           4,  4,  1, true,  true,  false },
            { Format::SBGRX8_UNORM,       U8,                           4,  4,  1, true,  true,  false },
            { Format::R10G10B10A2_UNORM,  None,                         0,  4,  1, false, false, false },
            { Format::R11G11B10_FLOAT,    None,                         0,  4,  1, false, false, false },
            { Format::RG16_UINT,          U16,                          2,  4,  1, false, false, true  },
            { Format::RG16_SINT,          S16,                          2,  4,  1, false, false, true  },
            { Format::RG16_UNORM,         U16,                          2,  4,  1, true,  false, false },
            { Format::RG16_SNORM,         S16,                          2,  4,  1, true,  false, false },
            { Format::RG16_FLOAT,         F16,                          2,  4,  1, false, false, false },
            { Format::R32_UINT,           U32,                          1,  4,  1, false, false, true  },
            { Format::R32_SINT,           S32,                          1,  4,  1, false, false, true  },
            { Format::R32_FLOAT,          F32,                          1,  4,  1, false, false, false },
            { Format::RGBA16_UINT,        U16,                          4,  8,  1, false, false, true  },
            { Format::RGBA16_SINT,        S16,                          4,  8,  1, false, false, true  },
            { Format::RGBA16_FLOAT,       F16,                          4,  8,  1, false, false, false },
            { Format::RGBA16_UNORM,       U16,                          4,  8,  1, true,  false, false },
            { Format::RGBA16_SNORM,       S16,                          4,  8,  1, true,  false, false },
            { Format::RG32_UINT,          U32,                          2,  8,  1, false, false, true  },
            { Format::RG32_SINT,          S32,                          2,  8,  1, false, false, true  },
            { Format::RG32_FLOAT,         F32,                          2,  8,  1, false, false, false },
            { Format::RGB32_UINT,         None,                         0, 12,  1, false, false, true  },
            { Format::RGB32_SINT,         None,                         0, 12,  1, false, false, true  },
            { Format::RGB32_FLOAT,        None,                         0, 12,  1, false, false, false },
            { Format::RGBA32_UINT,        U32,                          4, 16,  1, false, false, true  },
            { Format::RGBA32_SINT,        S32,                          4, 16,  1, false, false, true  },
            { Format::RGBA32_FLOAT,       F32,                          4, 16,  1, false, false, false },
            { Format::D16,                None,                         0,  2,  1, false, false, false },
            { Format::D24S8,              None,                         0,  4,  1, false, false, false },
            { Format::X24G8_UINT,         None,                         0,  4,  1, false, false, false },
            { Format::D32,                None,                         0,  4,  1, false, false, false },
            { Format::D32S8,              None,                         0,  8,  1, false, false, false },
            { Format::X32G8_UINT,         None,                         0,  8,  1, false, false, false },
            { Format::BC1_UNORM,          CU_AD_FORMAT_BC1_UNORM,       4,  8,  4, true,  false, false },
            { Format::BC1_UNORM_SRGB,     CU_AD_FORMAT_BC1_UNORM_SRGB,  4,  8,  4, true,  true,  false },
            { Format::BC2_UNORM,          CU_AD_FORMAT_BC2_UNORM,       4, 16,  4, true,  false, false },
            { Format::BC2_UNORM_SRGB,     CU_AD_FORMAT_BC2_UNORM_SRGB,  4, 16,  4, true,  true,  false },
            { Format::BC3_UNORM,          CU_AD_FORMAT_BC3_UNORM,       4, 16,  4, true,  false, false },
            { Format::BC3_UNORM_SRGB,     CU_AD_FORMAT_BC3_UNORM_SRGB,  4, 16,  4, true,  true,  false },
            { Format::BC4_UNORM,          CU_AD_FORMAT_BC4_UNORM,       1,  8,  4, true,  false, false },
            { Format::BC4_SNORM,          CU_AD_FORMAT_BC4_SNORM,       1,  8,  4, true,  false, false },
            { Format::BC5_UNORM,          CU_AD_FORMAT_BC5_UNORM,       2, 16,  4, true,  false, false },
            { Format::BC5_SNORM,          CU_AD_FORMAT_BC5_SNORM,       2, 16,  4, true,  false, false },
            { Format::BC6H_UFLOAT,        CU_AD_FORMAT_BC6H_UF16,       3, 16,  4, false, false, false },
            { Format::BC6H_SFLOAT,        CU_AD_FORMAT_BC6H_SF16,       3, 16,  4, false, false, false },
            { Format::BC7_UNORM,          CU_AD_FORMAT_BC7_UNORM,       4, 16,  4, true,  false, false },
            { Format::BC7_UNORM_SRGB,     CU_AD_FORMAT_BC7_UNORM_SRGB,  4, 16,  4, true,  true,  false },
        };

        static_assert(sizeof(c_Formats) / sizeof(c_Formats[0]) == size_t(Format::COUNT),
            "c_Formats must list every nvrhi::Format");
    }

    const FormatMapping& GetFormatMapping(Format format)
    {
        if (uint32_t(format) >= uint32_t(Format::COUNT))
            return c_Formats[0];
        const FormatMapping& mapping = c_Formats[uint32_t(format)];
        return mapping.format == format ? mapping : c_Formats[0];
    }
}
