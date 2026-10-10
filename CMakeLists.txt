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


cmake_minimum_required(VERSION 3.11)

project(nvrhi VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CMakeDependentOption)

option(NVRHI_BUILD_SHARED "Build NVRHI as a shared library (DLL or .so)" OFF)
option(NVRHI_BUILD_TESTS "Build focused backend memory query tests" OFF)
option(NVRHI_INSTALL "Generate install rules for NVRHI" ON)
cmake_dependent_option(NVRHI_INSTALL_EXPORTS "Install CMake exports" OFF "NVRHI_INSTALL" OFF)

option(NVRHI_WITH_VALIDATION "Build NVRHI the validation layer" ON)
option(NVRHI_WITH_VULKAN "Build the NVRHI Vulkan backend" ON)
option(NVRHI_WITH_RTXMU "Use RTXMU for acceleration structure management" OFF)
option(NVRHI_WITH_AFTERMATH "Include Aftermath support (requires NSight Aftermath SDK)" OFF)

cmake_dependent_option(NVRHI_WITH_NVAPI "Include NVAPI support (requires NVAPI SDK)" OFF "WIN32" OFF)
cmake_dependent_option(NVRHI_WITH_DX11 "Build the NVRHI D3D11 backend" ON "WIN32" OFF)
cmake_dependent_option(NVRHI_WITH_DX12 "Build the NVRHI D3D12 backend" ON "WIN32" OFF)


list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# EPM: the package manager shared by every project (this is its only copy).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/EPM.cmake")

if(NVRHI_WITH_VULKAN)
    epm_add_package(
        NAME Vulkan-Headers PACKAGE VulkanHeaders VERSION "1.4.352"
        FIND_PACKAGE_ARGUMENTS CONFIG
        GIT_REPOSITORY "https://github.com/KhronosGroup/Vulkan-Headers.git"
        GIT_TAG "v1.4.352")
endif()

if(NVRHI_WITH_DX12)
    epm_add_package(
        NAME DirectX-Headers PACKAGE directx-headers VERSION "1.717.0"
        FIND_PACKAGE_ARGUMENTS CONFIG
        GIT_REPOSITORY "https://github.com/microsoft/DirectX-Headers.git"
        GIT_TAG "v1.717.0-preview")
endif()

# The targets that carry the Vulkan and DirectX headers, whichever way EPM resolved them (an installed package or a
# fetched tree both export these names); the tests link them to compile against the headers alone.
set(nvrhi_vulkan_headers_target Vulkan::Headers)
set(nvrhi_directx_headers_target Microsoft::DirectX-Headers)

if(NVRHI_WITH_RTXMU)
    set(RTXMU_WITH_VULKAN $<BOOL:NVRHI_WITH_DX12> CACHE BOOL "" FORCE)
    set(RTXMU_WITH_DX12 $<BOOL:NVRHI_WITH_DX12> CACHE BOOL "" FORCE)

    # RTXMU compiles against the Vulkan headers that nvrhi uses.
    foreach(_vulkan_headers Vulkan::Headers Vulkan-Headers)
        if(TARGET ${_vulkan_headers})
            get_target_property(RTXMU_VULKAN_INCLUDE_DIR ${_vulkan_headers} INTERFACE_INCLUDE_DIRECTORIES)
            break()
        endif()
    endforeach()

    epm_add_package(
        NAME rtxmu
        GIT_REPOSITORY "https://github.com/NVIDIA-RTX/RTXMU.git"
        GIT_TAG "v1.4"
    )
endif()

if (NVRHI_BUILD_SHARED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if (MSVC)
    # The NVRHI targets add /GR- (nvrhi_disable_rtti). Older CMake policies (CMP0117) put /GR into
    # CMAKE_CXX_FLAGS, which would make cl warn about the override (D9025). /GR is cl's default anyway.
    string(REGEX REPLACE "(^| )/GR( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

if (NVRHI_WITH_NVAPI AND NOT TARGET nvapi)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NvrhiTargetArch.cmake")

    # NVAPI SDK layout: <arch folder>/<library>.lib
    if (NVRHI_TARGET_ARCH STREQUAL "arm64")
        set(_nvapi_library_name nvapia64)   # R615 and later
        set(_nvapi_library_dir aarch64)
    elseif (NVRHI_TARGET_ARCH STREQUAL "x64")
        set(_nvapi_library_name nvapi64)
        set(_nvapi_library_dir amd64)
    else()
        set(_nvapi_library_name nvapi)
        set(_nvapi_library_dir x86)
    endif()

    # Looks for nvapi.h and the import library in the given SDK directories (and the usual system
    # locations); sets the cache variables NVAPI_INCLUDE_DIR and NVAPI_LIBRARY.
    function(_nvrhi_find_nvapi)
        find_path(NVAPI_INCLUDE_DIR nvapi.h PATHS ${ARGN})
        find_library(NVAPI_LIBRARY ${_nvapi_library_name} PATHS ${ARGN} PATH_SUFFIXES ${_nvapi_library_dir})
    endfunction()

    # An SDK on NVAPI_SEARCH_PATHS (or in <source>/nvapi) wins, unless EPM_NVAPI_SOURCE names one;
    # otherwise NVIDIA's public repository is fetched.
    if (NOT EPM_NVAPI_SOURCE AND NOT DEFINED ENV{EPM_NVAPI_SOURCE})
        _nvrhi_find_nvapi(${NVAPI_SEARCH_PATHS} "${CMAKE_SOURCE_DIR}/nvapi")
    endif()

    if (NOT NVAPI_INCLUDE_DIR OR NOT NVAPI_LIBRARY)
        epm_add_package(
            NAME NVAPI
            GITHUB_REPOSITORY NVIDIA/nvapi
            GIT_TAG 9b181ea572f680327fe01a14a0f1f41c78034104
            DOWNLOAD_ONLY)
        _nvrhi_find_nvapi("${NVAPI_SOURCE_DIR}")
    endif()

    if (NOT NVAPI_INCLUDE_DIR OR NOT NVAPI_LIBRARY)
        message(FATAL_ERROR "NVAPI: nvapi.h or ${_nvapi_library_name}.lib not found "
            "(include dir: ${NVAPI_INCLUDE_DIR}, library: ${NVAPI_LIBRARY}); set NVAPI_SEARCH_PATHS to an SDK")
    endif()
    message(STATUS "NVAPI: ${NVAPI_LIBRARY}")

    add_library(nvapi STATIC IMPORTED GLOBAL)
    target_include_directories(nvapi INTERFACE "${NVAPI_INCLUDE_DIR}")
    set_property(TARGET nvapi PROPERTY IMPORTED_LOCATION "${NVAPI_LIBRARY}")
endif()

if(NVRHI_WITH_AFTERMATH AND NOT TARGET aftermath)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchAftermath.cmake")
endif()

# The object model: nvrhi_core / nvrhi::core (include/nvrhi/core, defines include_core)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/NvrhiCore.cmake)

set(include_common
    include/nvrhi/nvrhi.h
    include/nvrhi/nvrhiHLSL.h
    include/nvrhi/utils.h
    include/nvrhi/core/salieri.h
    include/nvrhi/common/aftermath.h
    include/nvrhi/common/export.h
    include/nvrhi/common/misc.h
    include/nvrhi/common/resource.h)
set(src_common
    src/common/aftermath.cpp
    src/common/aftermath.h
    src/common/bitset-allocator.h
    src/common/format-info.cpp
    src/common/misc.cpp
    src/common/resourcebindingmap.h
    src/common/state-tracking.cpp
    src/common/state-tracking.h
    src/common/utils.cpp
    src/common/utils-internal.h)

if(MSVC)
    set(misc_common 
        tools/nvrhi.natvis)
else()
    set(misc_common "")
endif()

set(include_validation
    include/nvrhi/validation.h)
set(src_validation
    src/validation/validation-commandlist.cpp
    src/validation/validation-device.cpp
    src/validation/validation-backend.h)

set(include_d3d11
    include/nvrhi/d3d11.h)
set(src_d3d11
    src/common/dxgi-format.h
    src/common/dxgi-format.cpp
    src/d3d11/d3d11-buffer.cpp
    src/d3d11/d3d11-commandlist.cpp
    src/d3d11/d3d11-compute.cpp
    src/d3d11/d3d11-constants.cpp
    src/d3d11/d3d11-backend.h
    src/d3d11/d3d11-device.cpp
    src/d3d11/d3d11-graphics.cpp
    src/d3d11/d3d11-queries.cpp
    src/d3d11/d3d11-resource-bindings.cpp
    src/d3d11/d3d11-shader.cpp
    src/d3d11/d3d11-texture.cpp)

set(include_d3d12
    include/nvrhi/d3d12.h)
set(src_d3d12
    src/common/dxgi-format.h
    src/common/dxgi-format.cpp
    src/common/versioning.h
    src/d3d12/d3d12-buffer.cpp
    src/d3d12/d3d12-commandlist.cpp
    src/d3d12/d3d12-compute.cpp
    src/d3d12/d3d12-constants.cpp
    src/d3d12/d3d12-backend.h
    src/d3d12/d3d12-descriptor-heap.cpp
    src/d3d12/d3d12-device.cpp
    src/d3d12/d3d12-graphics.cpp
    src/d3d12/d3d12-meshlets.cpp
    src/d3d12/d3d12-queries.cpp
    src/d3d12/d3d12-raytracing.cpp
    src/d3d12/d3d12-resource-bindings.cpp
    src/d3d12/d3d12-shader.cpp
    src/d3d12/d3d12-state-tracking.cpp
    src/d3d12/d3d12-texture.cpp
    src/d3d12/d3d12-upload.cpp)

set(include_vk
    include/nvrhi/vulkan.h)
set(src_vk
    src/common/versioning.h
    src/vulkan/vulkan-allocator.cpp
    src/vulkan/vulkan-buffer.cpp
    src/vulkan/vulkan-commandlist.cpp
    src/vulkan/vulkan-compute.cpp
    src/vulkan/vulkan-constants.cpp
    src/vulkan/vulkan-device.cpp
    src/vulkan/vulkan-graphics.cpp
    src/vulkan/vulkan-meshlets.cpp
    src/vulkan/vulkan-queries.cpp
    src/vulkan/vulkan-queue.cpp
    src/vulkan/vulkan-raytracing.cpp
    src/vulkan/vulkan-resource-bindings.cpp
    src/vulkan/vulkan-shader.cpp
    src/vulkan/vulkan-staging-texture.cpp
    src/vulkan/vulkan-state-tracking.cpp
    src/vulkan/vulkan-texture.cpp
    src/vulkan/vulkan-upload.cpp
    src/vulkan/vulkan-backend.h)

# NVRHI interface and common implementation functions

if (NVRHI_BUILD_SHARED)
    add_library(nvrhi SHARED
        ${include_common}
        ${src_common}
        ${misc_common})
else()
    add_library(nvrhi STATIC
        ${include_common}
        ${src_common}
        ${misc_common})
endif()


if (NVRHI_BUILD_SHARED)
    # nvrhi.dll exports only the extern "C" functions declared with NVRHI_C_API (include/nvrhi/common/export.h):
    # every other symbol is hidden, and clients see the objects through their interface vtables only.
    target_compile_definitions(nvrhi
        PRIVATE NVRHI_SHARED_LIBRARY_BUILD=1
        INTERFACE NVRHI_SHARED_LIBRARY_INCLUDE=1)
    # The DLL CRT, whatever runtime the client uses: nothing CRT-owned crosses the boundary (nvrhi containers
    # allocate from nvrhi_core's heap, which nvrhi.dll links as a DLL: cmake/NvrhiCore.cmake).
    set_target_properties(nvrhi PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
endif()

if (NVRHI_WITH_VALIDATION)
    target_sources(nvrhi PRIVATE
        ${include_validation}
        ${src_validation})
endif()

target_include_directories(nvrhi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:$<INSTALL_PREFIX>/include>)

target_link_libraries(nvrhi PUBLIC nvrhi_core)
nvrhi_disable_rtti(nvrhi)  # with NVRHI_BUILD_SHARED, this target also holds the backends

set_target_properties(nvrhi PROPERTIES FOLDER "NVRHI")

target_compile_definitions(nvrhi PRIVATE NVRHI_WITH_AFTERMATH=$<BOOL:${NVRHI_WITH_AFTERMATH}>)

# implementations

if (NVRHI_WITH_DX11)
    if (NVRHI_BUILD_SHARED)
        set(nvrhi_d3d11_target nvrhi)

        target_sources(${nvrhi_d3d11_target} PRIVATE
            ${include_d3d11}
            ${src_d3d11})
    else()
        set(nvrhi_d3d11_target nvrhi_d3d11)

        add_library(${nvrhi_d3d11_target} STATIC
            ${include_d3d11}
            ${src_d3d11})

        set_target_properties(${nvrhi_d3d11_target} PROPERTIES FOLDER "NVRHI")
        target_include_directories(${nvrhi_d3d11_target} PRIVATE include)
        target_link_libraries(${nvrhi_d3d11_target} PUBLIC nvrhi_core)
        nvrhi_disable_rtti(${nvrhi_d3d11_target})
    endif()

    target_link_libraries(${nvrhi_d3d11_target} PUBLIC d3d11 dxguid)

    if (NVRHI_WITH_NVAPI)
        target_link_libraries(${nvrhi_d3d11_target} PUBLIC nvapi)
    endif()
    target_compile_definitions(${nvrhi_d3d11_target} PRIVATE NVRHI_D3D11_WITH_NVAPI=$<BOOL:${NVRHI_WITH_NVAPI}>)

    if (NVRHI_WITH_AFTERMATH)
        target_link_libraries(${nvrhi_d3d11_target} PUBLIC aftermath)
    endif()
    target_compile_definitions(${nvrhi_d3d11_target} PRIVATE NVRHI_WITH_AFTERMATH=$<BOOL:${NVRHI_WITH_AFTERMATH}>)
endif()

if (NVRHI_WITH_DX12)
    if (NVRHI_BUILD_SHARED)
        set(nvrhi_d3d12_target nvrhi)

        target_sources(${nvrhi_d3d12_target} PRIVATE
            ${include_d3d12}
            ${src_d3d12})
    else()
        set(nvrhi_d3d12_target nvrhi_d3d12)

        add_library(${nvrhi_d3d12_target} STATIC
            ${include_d3d12}
            ${src_d3d12})

        set_target_properties(${nvrhi_d3d12_target} PROPERTIES FOLDER "NVRHI")
        target_include_directories(${nvrhi_d3d12_target} PRIVATE include)
        target_link_libraries(${nvrhi_d3d12_target} PUBLIC nvrhi_core)
        nvrhi_disable_rtti(${nvrhi_d3d12_target})
    endif()

    if (NVRHI_WITH_RTXMU)
        target_compile_definitions(${nvrhi_d3d12_target} PUBLIC NVRHI_WITH_RTXMU=1)
        target_link_libraries(${nvrhi_d3d12_target} PUBLIC rtxmu)
    endif()

    target_compile_definitions(${nvrhi_d3d12_target} PRIVATE NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=$<BOOL:${NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP}>)

    target_link_libraries(${nvrhi_d3d12_target} PUBLIC Microsoft::DirectX-Headers Microsoft::DirectX-Guids d3d12)

    if (NVRHI_WITH_NVAPI)
        target_link_libraries(${nvrhi_d3d12_target} PUBLIC nvapi)
    endif()
    target_compile_definitions(${nvrhi_d3d12_target} PRIVATE NVRHI_D3D12_WITH_NVAPI=$<BOOL:${NVRHI_WITH_NVAPI}>)

    if (NVRHI_WITH_AFTERMATH)
        target_link_libraries(${nvrhi_d3d12_target} PUBLIC aftermath)
    endif()
    target_compile_definitions(${nvrhi_d3d12_target} PRIVATE NVRHI_WITH_AFTERMATH=$<BOOL:${NVRHI_WITH_AFTERMATH}>)
endif()

if (NVRHI_WITH_VULKAN)
    if (NVRHI_BUILD_SHARED)
        set(nvrhi_vulkan_target nvrhi)

        target_sources(${nvrhi_vulkan_target} PRIVATE
            ${include_vk}
            ${src_vk})
    else()
        set(nvrhi_vulkan_target nvrhi_vk)

        add_library(${nvrhi_vulkan_target} STATIC
            ${include_vk}
            ${src_vk})

        set_target_properties(${nvrhi_vulkan_target} PROPERTIES FOLDER "NVRHI")
        target_include_directories(${nvrhi_vulkan_target} PRIVATE include)
        target_link_libraries(${nvrhi_vulkan_target} PUBLIC nvrhi_core)
        nvrhi_disable_rtti(${nvrhi_vulkan_target})
    endif()
    
    if(WIN32)
        target_compile_definitions(${nvrhi_vulkan_target} PUBLIC VK_USE_PLATFORM_WIN32_KHR)
        target_compile_definitions(${nvrhi_vulkan_target} PRIVATE NOMINMAX)
    endif()

    if (NVRHI_WITH_RTXMU)
        target_compile_definitions(${nvrhi_vulkan_target} PUBLIC NVRHI_WITH_RTXMU=1)
        target_link_libraries(${nvrhi_vulkan_target} PUBLIC rtxmu)
    endif()

    target_link_libraries(${nvrhi_vulkan_target} PRIVATE Vulkan::Headers)

    if (NVRHI_WITH_AFTERMATH)
        target_link_libraries(${nvrhi_vulkan_target} PUBLIC aftermath)
    endif()
    target_compile_definitions(${nvrhi_vulkan_target} PRIVATE NVRHI_WITH_AFTERMATH=$<BOOL:${NVRHI_WITH_AFTERMATH}>)
endif()


if(NVRHI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if (NVRHI_INSTALL)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/nvrhi
        DESTINATION ${CMAKE_INSTALL_PREFIX}/include)

    install(TARGETS nvrhi nvrhi_core
        EXPORT "nvrhiTargets"
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)

    if (NOT NVRHI_BUILD_SHARED)
        if (NVRHI_WITH_DX11)
            install(TARGETS ${nvrhi_d3d11_target} DESTINATION "lib" EXPORT "nvrhiTargets")
        endif()

        if (NVRHI_WITH_DX12)
            install(TARGETS ${nvrhi_d3d12_target} DESTINATION "lib" EXPORT "nvrhiTargets")
        endif()

        if (NVRHI_WITH_VULKAN)
            install(TARGETS ${nvrhi_vulkan_target} DESTINATION "lib" EXPORT "nvrhiTargets")
        endif()
    endif()

    if (NVRHI_INSTALL_EXPORTS)
        set(nvrhi_CONFIG_PATH "lib/cmake/nvrhi")

        include(CMakePackageConfigHelpers)

        configure_package_config_file(
            src/nvrhiConfig.cmake.in
            src/nvrhiConfig.cmake
            INSTALL_DESTINATION "${nvrhi_CONFIG_PATH}"
            NO_CHECK_REQUIRED_COMPONENTS_MACRO)

        write_basic_package_version_file(
            src/nvrhiConfigVersion.cmake 
            VERSION ${nvrhi_VERSION} 
            COMPATIBILITY ExactVersion)

        install(FILES
            "${nvrhi_BINARY_DIR}/src/nvrhiConfig.cmake"
            "${nvrhi_BINARY_DIR}/src/nvrhiConfigVersion.cmake"
            DESTINATION "${nvrhi_CONFIG_PATH}")

        install(EXPORT "nvrhiTargets" 
            FILE "nvrhiTargets.cmake"
            EXPORT_LINK_INTERFACE_LIBRARIES
            DESTINATION "${nvrhi_CONFIG_PATH}")
    endif()
endif()