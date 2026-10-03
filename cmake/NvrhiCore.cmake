#
# Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

# nvrhi::core: the object model (IObject, AutoPtr, ObjectImpl, ...) under include/nvrhi/core.
#
# nvrhi's CMakeLists.txt includes this file. A project that needs only the object model, without the
# rest of NVRHI, may include it directly:
#
#     include(<nvrhi>/cmake/NvrhiCore.cmake)
#     target_link_libraries(my_target PUBLIC nvrhi::core)
#
# The object model templates are header only. The library itself owns what must exist once per process:
# the default allocator (nvrhiCoreGetDefaultMemAllocator) and the IDataBlob implementations
# (nvrhiCoreCreate*Blob). As a shared library (the default) it gives every module that links it one heap,
# whatever CRT each module uses; it exports C functions only (include/nvrhi/core/export.h).

include_guard(GLOBAL)

option(NVRHI_CORE_BUILD_SHARED "Build nvrhi_core as a shared library (DLL or .so)" ON)

# A static nvrhi_core inside a shared nvrhi would give nvrhi.dll its own default allocator on its own CRT
# heap (and, on MSVC, a static CRT inside a DLL built for the DLL CRT).
if (NVRHI_BUILD_SHARED AND NOT NVRHI_CORE_BUILD_SHARED)
    message(FATAL_ERROR "NVRHI_BUILD_SHARED=ON requires NVRHI_CORE_BUILD_SHARED=ON: a shared nvrhi links the shared nvrhi_core")
endif()

get_filename_component(nvrhi_core_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(include_core
    ${nvrhi_core_root}/include/nvrhi/core/autoptr.h
    ${nvrhi_core_root}/include/nvrhi/core/containers.h
    ${nvrhi_core_root}/include/nvrhi/core/datablob.h
    ${nvrhi_core_root}/include/nvrhi/core/export.h
    ${nvrhi_core_root}/include/nvrhi/core/foundation.h
    ${nvrhi_core_root}/include/nvrhi/core/memory.h
    ${nvrhi_core_root}/include/nvrhi/core/threading.h
    ${nvrhi_core_root}/include/nvrhi/core/types.h)
set(src_core
    ${nvrhi_core_root}/src/core/core.cpp
    ${nvrhi_core_root}/src/core/datablob.cpp
    ${nvrhi_core_root}/src/core/memory.cpp)

find_package(Threads REQUIRED)

# NVRHI is compiled without RTTI (ADR 0006): type tests use QueryInterface, never dynamic_cast or typeid.
# The option is PRIVATE: targets that link NVRHI keep their own setting, and NVRHI's public headers need
# no RTTI either way.
function(nvrhi_disable_rtti target)
    if (MSVC)  # cl and clang-cl
        target_compile_options(${target} PRIVATE /GR-)
    else()
        target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
endfunction()

if (NVRHI_CORE_BUILD_SHARED)
    add_library(nvrhi_core SHARED ${include_core} ${src_core})
else()
    add_library(nvrhi_core STATIC ${include_core} ${src_core})
endif()
add_library(nvrhi::core ALIAS nvrhi_core)

target_include_directories(nvrhi_core PUBLIC
    $<BUILD_INTERFACE:${nvrhi_core_root}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(nvrhi_core PUBLIC cxx_std_17)
nvrhi_disable_rtti(nvrhi_core)
target_link_libraries(nvrhi_core PUBLIC Threads::Threads)

if (NVRHI_CORE_BUILD_SHARED)
    target_compile_definitions(nvrhi_core
        PRIVATE NVRHI_CORE_SHARED_LIBRARY_BUILD=1
        INTERFACE NVRHI_CORE_SHARED_LIBRARY_INCLUDE=1)
    # The DLL CRT, whatever runtime the modules that link it use: they reach it through C exports and
    # IMemoryAllocator / IDataBlob vtables only, which is valid across CRTs.
    # Only the C exports are visible; inline and template code stays private.
    set_target_properties(nvrhi_core PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    # The DLL lands in CMAKE_RUNTIME_OUTPUT_DIRECTORY when the parent project sets one (next to its
    # executables). install(): RUNTIME DESTINATION bin, in nvrhi's CMakeLists.txt.
elseif (MSVC)
    # core.cpp holds only compile-time checks: no public symbols in its object file.
    set_property(TARGET nvrhi_core APPEND PROPERTY STATIC_LIBRARY_OPTIONS /IGNORE:4221)
endif()

# Every module that implements COM objects with foundation.h instantiates ObjectImpl<...>, MakeNewRCObj<...>
# and the interface tables itself. On ELF, default visibility would let the dynamic linker merge those vague-
# linkage copies across modules, even when they were built differently (compiler, NVRHI_DEBUG, header
# version). Hidden visibility keeps each module's instantiations private. Windows DLLs keep separate copies.
if (NOT MSVC)
    target_compile_options(nvrhi_core INTERFACE -fvisibility=hidden -fvisibility-inlines-hidden)
endif()

set_target_properties(nvrhi_core PROPERTIES FOLDER "NVRHI")
source_group(TREE "${nvrhi_core_root}" FILES ${include_core} ${src_core})
