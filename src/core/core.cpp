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

// nvrhi::core is header only. This translation unit anchors the nvrhi_core library and checks that every
// core header compiles on its own and together with the others.

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

    // The one public symbol of the library: without it, MSVC warns LNK4221 (object file has no public symbols).
    const char* GetCoreLibraryName() noexcept;
    const char* GetCoreLibraryName() noexcept { return "nvrhi_core"; }
}
