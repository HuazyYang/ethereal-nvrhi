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

include_guard(GLOBAL)

get_filename_component(nvrhi_core_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(include_core
    ${nvrhi_core_root}/include/nvrhi/core/AutoPtr.h
    ${nvrhi_core_root}/include/nvrhi/core/DataBlob.h
    ${nvrhi_core_root}/include/nvrhi/core/Foundation.h
    ${nvrhi_core_root}/include/nvrhi/core/Memory.h
    ${nvrhi_core_root}/include/nvrhi/core/Threading.h
    ${nvrhi_core_root}/include/nvrhi/core/Types.h)
set(src_core
    ${nvrhi_core_root}/src/core/core.cpp)

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

add_library(nvrhi_core STATIC ${include_core} ${src_core})
add_library(nvrhi::core ALIAS nvrhi_core)

target_include_directories(nvrhi_core PUBLIC
    $<BUILD_INTERFACE:${nvrhi_core_root}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(nvrhi_core PUBLIC cxx_std_17)
nvrhi_disable_rtti(nvrhi_core)
target_link_libraries(nvrhi_core PUBLIC Threads::Threads)

set_target_properties(nvrhi_core PROPERTIES FOLDER "NVRHI")
source_group(TREE "${nvrhi_core_root}" FILES ${include_core} ${src_core})
