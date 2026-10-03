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

// Checks that every core header compiles on its own and together with the others, and the compile-time
// properties of the object model. The library's code is in memory.cpp (the default allocator) and
// datablob.cpp (the IDataBlob implementations).

#include <nvrhi/core/export.h>
#include <nvrhi/core/types.h>
#include <nvrhi/core/memory.h>
#include <nvrhi/core/threading.h>
#include <nvrhi/core/foundation.h>
#include <nvrhi/core/autoptr.h>
#include <nvrhi/core/datablob.h>

namespace nvrhi::details
{
    static_assert(sizeof(GUID) == 16, "GUID must match the layout of the Windows GUID");
    static_assert("f578ff0d-abd2-4514-9d32-7cb454d4a73b"_nvrhi_guid.Data1 == 0xf578ff0du);
    static_assert("f578ff0d-abd2-4514-9d32-7cb454d4a73b"_nvrhi_guid.Data4[7] == 0x3bu);
    static_assert(uuid_of<IDataBlob>().Data1 == 0xf578ff0du);
    static_assert(uuid_of<IObject>().Data1 == 0u);
    static_assert(sizeof(AutoPtr<IObject>) == sizeof(IObject*));

    // GUID literals are case-insensitive: two modules that spell the same IID in different case agree on it.
    constexpr bool SameGuid(const GUID& a, const GUID& b)
    {
        if (a.Data1 != b.Data1 || a.Data2 != b.Data2 || a.Data3 != b.Data3)
            return false;
        for (int i = 0; i < 8; ++i)
            if (a.Data4[i] != b.Data4[i])
                return false;
        return true;
    }
    static_assert(SameGuid("F578FF0D-ABD2-4514-9D32-7CB454D4A73B"_nvrhi_guid,
                           "f578ff0d-abd2-4514-9d32-7cb454d4a73b"_nvrhi_guid));
    static_assert(SameGuid("F578ff0D-aBd2-4514-9D32-7cB454d4A73b"_nvrhi_guid, IID_IDataBlob));
    static_assert("ABCDEF01-0000-0000-0000-000000000000"_nvrhi_guid.Data1 == 0xabcdef01u);
    static_assert("00000000-0000-0000-0000-0000000000Ff"_nvrhi_guid.Data4[7] == 0xffu);

    // Every status code is distinct.
    constexpr FRESULT StatusCodes[] = {FS_OK, FE_GENERIC_ERROR, FE_NOINTERFACE, FE_NOT_IMPLEMENT, FE_INVALID_ARGS,
                                       FE_NOT_ALIVE_OBJECT, FE_NOT_FOUND, FE_WAIT_TIMEOUT, FE_OUT_OF_MEMORY,
                                       FE_UNSUPPORTED};
    constexpr bool StatusCodesAreDistinct()
    {
        constexpr size_t count = sizeof(StatusCodes) / sizeof(StatusCodes[0]);
        for (size_t i = 0; i < count; ++i)
            for (size_t j = i + 1; j < count; ++j)
                if (StatusCodes[i] == StatusCodes[j])
                    return false;
        return true;
    }
    static_assert(StatusCodesAreDistinct(), "FRESULT codes must be distinct");
    static_assert(NVRHI_FAILED(FE_OUT_OF_MEMORY) && NVRHI_FAILED(FE_UNSUPPORTED) && NVRHI_SUCCEEDED(FS_OK));
}
