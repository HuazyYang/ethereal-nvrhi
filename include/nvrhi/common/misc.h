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

#pragma once

#include <nvrhi/core/types.h>
#include <nvrhi/core/memory.h>

#include <cstdint>
#include <cassert>
#include <type_traits>

namespace nvrhi 
{
    template<typename T> T align(T size, T alignment)
    {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    template<typename T, typename U> [[nodiscard]] bool arraysAreDifferent(const T& a, const U& b)
    {
        if (a.size() != b.size())
            return true;

        for (uint32_t i = 0; i < uint32_t(a.size()); i++)
        {
            if (a[i] != b[i])
                return true;
        }

        return false;
    }

    template<typename T, typename U> [[nodiscard]] uint32_t arrayDifferenceMask(const T& a, const U& b)
    {
        assert(a.size() <= 32);
        assert(b.size() <= 32);

        if (a.size() != b.size())
            return ~0u;

        uint32_t mask = 0;
        for (uint32_t i = 0; i < uint32_t(a.size()); i++)
        {
            if (a[i] != b[i])
                mask |= (1 << i);
        }

        return mask;
    }

    inline uint32_t hash_to_u32(size_t hash)
    {
        if constexpr (sizeof(size_t) == 8)
            return uint32_t(hash) ^ uint32_t(uint64_t(hash) >> 32);
        else
            return uint32_t(hash);
    }

    // A type cast that is safer than static_cast in debug builds, and is a simple static_cast in release builds.
    // Used for downcasting various ISomething* pointers to their implementation classes in the backends.
    //
    // NVRHI is built without RTTI (ADR 0006), so the Debug check does not use dynamic_cast: it asks the object,
    // through QueryInterface, for uuid_of<T>() (the IID of an interface, or the class ID of an implementation
    // class: NVRHI_CLASS_CLSID) and checks that the answer is the pointer static_cast gives. The check never adds
    // a reference to an object that is being destroyed (see nvrhi::details::QICastMatches). Both types must be
    // nvrhi::IObject types with an ID; for anything else use unchecked_cast.
    template <typename T, typename U>
    T checked_cast(U u)
    {
        static_assert(!std::is_same<T, U>::value, "Redundant checked_cast");
        static_assert(std::is_pointer<T>::value && std::is_pointer<U>::value, "checked_cast casts pointers");
        static_assert(std::is_base_of<IObject, std::remove_cv_t<std::remove_pointer_t<T>>>::value &&
                          std::is_base_of<IObject, std::remove_cv_t<std::remove_pointer_t<U>>>::value,
                      "checked_cast verifies through QueryInterface: both types must be nvrhi::IObject types "
                      "(use unchecked_cast for other types)");
#if NVRHI_DEBUG
        if (!u) return nullptr;
        T t = static_cast<T>(u);
        if (!details::QICastMatches(u, t)) assert(!"Invalid type cast");  // NOLINT(clang-diagnostic-string-conversion)
        return t;
#else
        return static_cast<T>(u);
#endif
    }

    // A plain static_cast, for the casts checked_cast cannot verify: the source or the target is not an
    // nvrhi::IObject type (a helper base such as TextureStateExtension or MemoryResource, or a plain struct).
    // Every use says why the cast is valid.
    template <typename T, typename U>
    T unchecked_cast(U u)
    {
        static_assert(!std::is_same<T, U>::value, "Redundant unchecked_cast");
        return static_cast<T>(u);
    }
} // namespace nvrhi
