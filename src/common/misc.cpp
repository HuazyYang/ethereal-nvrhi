/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
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

#include <nvrhi/nvrhi.h>
#include "utils-internal.h"

namespace nvrhi
{
    // Do not move this function into the header.
    bool nvrhiVerifyHeaderVersion(uint32_t version) noexcept
    {
        return version == c_HeaderVersion;
    }

    size_t coopvec::nvrhiCoopVecGetDataTypeSize(coopvec::DataType type) noexcept
    {
        switch (type)
        {
        case coopvec::DataType::UInt8:
        case coopvec::DataType::SInt8:
            return 1;
        case coopvec::DataType::UInt8Packed:
        case coopvec::DataType::SInt8Packed:
            // Not sure if this is correct or even relevant because packed types
            // cannot be used in matrices accessible from the host side.
            return 1;
        case coopvec::DataType::UInt16:
        case coopvec::DataType::SInt16:
            return 2;
        case coopvec::DataType::UInt32:
        case coopvec::DataType::SInt32:
            return 4;
        case coopvec::DataType::UInt64:
        case coopvec::DataType::SInt64:
            return 8;
        case coopvec::DataType::FloatE4M3:
        case coopvec::DataType::FloatE5M2:
            return 1;
        case coopvec::DataType::Float16:
        case coopvec::DataType::BFloat16:
            return 2;
        case coopvec::DataType::Float32:
            return 4;
        case coopvec::DataType::Float64:
            return 8;
        default:
            utils::InvalidEnum();
            return 0;
        }
    }

    size_t coopvec::nvrhiCoopVecGetOptimalMatrixStride(coopvec::DataType type, coopvec::MatrixLayout layout, uint32_t rows, uint32_t columns) noexcept
    {
        size_t const dataTypeSize = coopvec::nvrhiCoopVecGetDataTypeSize(type);
        
        switch (layout)
        {
        case coopvec::MatrixLayout::RowMajor:
            return dataTypeSize * columns;
            break;
        case coopvec::MatrixLayout::ColumnMajor:
            return dataTypeSize * rows;
            break;
        default:
            return 0;
        }
    }

} // namespace nvrhi