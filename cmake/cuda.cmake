# CUDA backend is host C++ and uses the Driver API only: no CUDA language/NVCC or cudart.
find_package(CUDAToolkit 12.0 REQUIRED)
set(nvrhi_cuda_sources src/cuda/cuda-device.cpp src/cuda/cuda-command.cpp src/cuda/cuda-texture.cpp src/cuda/cuda-dispatch.cpp src/cuda/cuda-optix.cpp src/cuda/cuda-backend.h include/nvrhi/cuda.h)
if(NVRHI_BUILD_SHARED)
    set(nvrhi_cuda_target nvrhi)
    target_sources(nvrhi PRIVATE ${nvrhi_cuda_sources})
else()
    set(nvrhi_cuda_target nvrhi_cuda)
    add_library(nvrhi_cuda STATIC ${nvrhi_cuda_sources})
    target_include_directories(nvrhi_cuda PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
    target_link_libraries(nvrhi_cuda PUBLIC nvrhi)
    nvrhi_disable_rtti(nvrhi_cuda)
endif()
add_library(nvrhi::cuda ALIAS ${nvrhi_cuda_target})
target_link_libraries(${nvrhi_cuda_target} PRIVATE CUDA::cuda_driver ShaderTool::Artifacts)
target_compile_definitions(${nvrhi_cuda_target} PRIVATE NVRHI_WITH_OPTIX=$<BOOL:${NVRHI_WITH_OPTIX}>)
if(NVRHI_WITH_OPTIX)
    find_path(NVRHI_OPTIX_INCLUDE_DIR optix.h HINTS ${OPTIX_ROOT} $ENV{OPTIX_ROOT} $ENV{OptiX_INSTALL_DIR} PATH_SUFFIXES include)
    if(NOT NVRHI_OPTIX_INCLUDE_DIR)
        message(FATAL_ERROR "NVRHI_WITH_OPTIX requires OptiX SDK headers; set OPTIX_ROOT")
    endif()
    target_include_directories(${nvrhi_cuda_target} PRIVATE "${NVRHI_OPTIX_INCLUDE_DIR}")
    target_link_libraries(${nvrhi_cuda_target} PRIVATE ${CMAKE_DL_LIBS})
endif()
set_target_properties(${nvrhi_cuda_target} PROPERTIES FOLDER "NVRHI")
if(NVRHI_BUILD_TESTS)
    add_executable(nvrhi_cuda_compute tests/cuda-compute.cpp)
    target_link_libraries(nvrhi_cuda_compute PRIVATE ${nvrhi_cuda_target} ShaderTool::Artifacts)
    add_executable(nvrhi_cuda_device_desc tests/cuda-device-desc.cpp)
    target_link_libraries(nvrhi_cuda_device_desc PRIVATE ${nvrhi_cuda_target})
    target_compile_definitions(nvrhi_cuda_device_desc PRIVATE TEST_CUDA_OPTIX_SUPPORTED=$<BOOL:${NVRHI_WITH_OPTIX}>)
    add_test(NAME cuda_device_desc COMMAND nvrhi_cuda_device_desc)
    add_test(NAME cuda_compute COMMAND nvrhi_cuda_compute)
    set_tests_properties(cuda_compute PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

if(WIN32)
    target_compile_definitions(${nvrhi_cuda_target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(NVRHI_BUILD_TESTS AND NVRHI_WITH_OPTIX)
    add_executable(nvrhi_cuda_slang tests/cuda-slang.cpp)
    target_link_libraries(nvrhi_cuda_slang PRIVATE ${nvrhi_cuda_target})
endif()

# Compile real Slang fixtures offline when an offline compiler is available.
set(NVRHI_CUDA_SHADER_TOOL "" CACHE FILEPATH "ShaderTool executable for CUDA/OptiX integration fixtures")
if(NVRHI_BUILD_TESTS)
    if(TARGET ShaderTool::ShaderTool)
        set(_cuda_shader_tool "$<TARGET_FILE:ShaderTool::ShaderTool>")
    elseif(NVRHI_CUDA_SHADER_TOOL)
        set(_cuda_shader_tool "${NVRHI_CUDA_SHADER_TOOL}")
    endif()
endif()
if(NVRHI_BUILD_TESTS AND NVRHI_WITH_OPTIX)
    if(_cuda_shader_tool)
        set(_cuda_compute_artifact "${CMAKE_CURRENT_BINARY_DIR}/cuda-compute.nsta")
        set(_cuda_ray_artifact "${CMAKE_CURRENT_BINARY_DIR}/cuda-raytracing.nsta")
        add_custom_command(OUTPUT "${_cuda_compute_artifact}"
            COMMAND "${_cuda_shader_tool}" slang-ptx --artifact -T cs_6_5 -E main
                -Fo "${_cuda_compute_artifact}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-slang-compute.slang"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-slang-compute.slang" "${_cuda_shader_tool}"
            VERBATIM)
        add_custom_command(OUTPUT "${_cuda_ray_artifact}"
            COMMAND "${_cuda_shader_tool}" slang-optix --artifact -T lib_6_5 -I "${NVRHI_OPTIX_INCLUDE_DIR}"
                -Fo "${_cuda_ray_artifact}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-slang-raytracing.slang"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-slang-raytracing.slang" "${_cuda_shader_tool}"
            VERBATIM)
        set(_cuda_entry_artifact "${CMAKE_CURRENT_BINARY_DIR}/cuda-entry.nsta")
        set(_cuda_texture_artifact "${CMAKE_CURRENT_BINARY_DIR}/cuda-texture.nsta")
        add_custom_command(OUTPUT "${_cuda_entry_artifact}"
            COMMAND "${_cuda_shader_tool}" slang-ptx --artifact -T cs_6_5 -E entryParameters
                -Fo "${_cuda_entry_artifact}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-slang-entry.slang"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-slang-entry.slang" "${_cuda_shader_tool}" VERBATIM)
        add_custom_command(OUTPUT "${_cuda_texture_artifact}"
            COMMAND "${_cuda_shader_tool}" slang-ptx --artifact -T cs_6_5 -E textureSampling
                -texture-sampler pointTexture=pointSampler -texture-sampler linearTexture=linearSampler
                -Fo "${_cuda_texture_artifact}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-slang-texture.slang"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-slang-texture.slang" "${_cuda_shader_tool}" VERBATIM)
        add_custom_target(nvrhi_cuda_test_artifacts DEPENDS "${_cuda_compute_artifact}" "${_cuda_ray_artifact}" "${_cuda_entry_artifact}" "${_cuda_texture_artifact}")
        add_dependencies(nvrhi_cuda_slang nvrhi_cuda_test_artifacts)
        add_test(NAME cuda_slang_optix COMMAND nvrhi_cuda_slang "${_cuda_compute_artifact}" "${_cuda_ray_artifact}" "${_cuda_entry_artifact}" "${_cuda_texture_artifact}")
        set_tests_properties(cuda_slang_optix PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
    else()
        message(STATUS "CUDA: set NVRHI_CUDA_SHADER_TOOL to enable Slang/OptiX GPU integration tests")
    endif()
endif()

if(NVRHI_BUILD_TESTS)
    add_executable(nvrhi_cuda_interop tests/cuda-interop.cpp)
    target_link_libraries(nvrhi_cuda_interop PRIVATE nvrhi::cuda CUDA::cuda_driver)
    if(WIN32)
        target_link_libraries(nvrhi_cuda_interop PRIVATE d3d12 d3d11 dxgi)
    endif()
    add_test(NAME cuda_native_interop COMMAND nvrhi_cuda_interop)
    set_tests_properties(cuda_native_interop PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
endif()

if(NVRHI_BUILD_TESTS)
    find_package(Vulkan QUIET)
    if(TARGET Vulkan::Vulkan)
        add_executable(nvrhi_cuda_vulkan_interop tests/cuda-vulkan-interop.cpp)
        target_link_libraries(nvrhi_cuda_vulkan_interop PRIVATE nvrhi::cuda Vulkan::Vulkan)
        add_test(NAME cuda_vulkan_interop COMMAND nvrhi_cuda_vulkan_interop)
        set_tests_properties(cuda_vulkan_interop PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    endif()
endif()

if(NVRHI_BUILD_TESTS AND NVRHI_WITH_OPTIX AND _cuda_shader_tool)
    set(_cuda_local_artifact "${CMAKE_CURRENT_BINARY_DIR}/cuda-optix-local.nsta")
    add_custom_command(OUTPUT "${_cuda_local_artifact}"
        COMMAND "${_cuda_shader_tool}" slang-optix --artifact -T lib_6_5 -I "${NVRHI_OPTIX_INCLUDE_DIR}"
            -Fo "${_cuda_local_artifact}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-optix-local.slang"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-optix-local.slang" "${_cuda_shader_tool}" VERBATIM)
    add_executable(nvrhi_cuda_optix_local tests/cuda-optix-local.cpp "${_cuda_local_artifact}")
    target_link_libraries(nvrhi_cuda_optix_local PRIVATE nvrhi::cuda)
    add_test(NAME cuda_optix_local COMMAND nvrhi_cuda_optix_local "${_cuda_local_artifact}")
    add_test(NAME cuda_optix_validation COMMAND nvrhi_cuda_optix_local "${_cuda_local_artifact}" --validation)
    add_test(NAME cuda_optix_trace_depth COMMAND nvrhi_cuda_optix_local "${_cuda_local_artifact}" --trace-depth-fault)
    set_tests_properties(cuda_optix_local cuda_optix_validation cuda_optix_trace_depth PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90 RUN_SERIAL TRUE)
endif()

if(NVRHI_BUILD_TESTS AND _cuda_shader_tool)
    set(_cuda_texture_views_artifact "${CMAKE_CURRENT_BINARY_DIR}/cuda-texture-views.nsta")
    add_custom_command(OUTPUT "${_cuda_texture_views_artifact}"
        COMMAND "${_cuda_shader_tool}" slang-ptx --artifact -T cs_6_5 -E main -texture-sampler source=pointSampler
            -Fo "${_cuda_texture_views_artifact}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-texture-views.slang"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda-texture-views.slang" "${_cuda_shader_tool}" VERBATIM)
    add_executable(nvrhi_cuda_texture_views tests/cuda-texture-views.cpp "${_cuda_texture_views_artifact}")
    target_link_libraries(nvrhi_cuda_texture_views PRIVATE nvrhi::cuda)
    add_test(NAME cuda_texture_views COMMAND nvrhi_cuda_texture_views "${_cuda_texture_views_artifact}")
    set_tests_properties(cuda_texture_views PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
endif()
