# The dependencies of the CUDA backend (ADR 0011).
#
# - The CUDA toolkit (12.0 or newer) for its headers only: the driver is loaded at run time, nothing is linked.
#   Sets NVRHI_CUDA_INCLUDE_DIRS.
# - With NVRHI_WITH_OPTIX, the OptiX SDK (9.0 or newer), which is headers only as well. It is found through the
#   OPTIX_ROOT or OptiX_INSTALL_DIR variables or environment variables, or in the default install locations.
#   Sets NVRHI_OPTIX_INCLUDE_DIR.

find_package(CUDAToolkit 12.0 REQUIRED)
set(NVRHI_CUDA_INCLUDE_DIRS ${CUDAToolkit_INCLUDE_DIRS})
message(STATUS "NVRHI: CUDA ${CUDAToolkit_VERSION} headers in ${CUDAToolkit_INCLUDE_DIRS}")

if (NVRHI_WITH_OPTIX)
    set(_nvrhi_optix_hints "")
    foreach(_nvrhi_optix_root "${OPTIX_ROOT}" "$ENV{OPTIX_ROOT}" "${OptiX_INSTALL_DIR}" "$ENV{OptiX_INSTALL_DIR}")
        if (_nvrhi_optix_root)
            list(APPEND _nvrhi_optix_hints "${_nvrhi_optix_root}")
        endif()
    endforeach()

    # The newest SDK in the default install locations.
    file(TO_CMAKE_PATH "$ENV{ProgramFiles}" _nvrhi_program_files)
    file(TO_CMAKE_PATH "$ENV{ProgramData}" _nvrhi_program_data)
    file(GLOB _nvrhi_optix_installed
        "${_nvrhi_program_files}/NVIDIA GPU Computing Toolkit/OptiX SDK *"
        "${_nvrhi_program_data}/NVIDIA Corporation/OptiX SDK *"
        "/usr/local/NVIDIA-OptiX-SDK-*")
    list(SORT _nvrhi_optix_installed COMPARE NATURAL ORDER DESCENDING)
    list(APPEND _nvrhi_optix_hints ${_nvrhi_optix_installed})

    find_path(NVRHI_OPTIX_INCLUDE_DIR optix.h HINTS ${_nvrhi_optix_hints} PATH_SUFFIXES include)
    if (NOT NVRHI_OPTIX_INCLUDE_DIR)
        message(FATAL_ERROR "NVRHI_WITH_OPTIX: optix.h not found; set OPTIX_ROOT or OptiX_INSTALL_DIR to an OptiX SDK "
            "9.0 or newer, or turn NVRHI_WITH_OPTIX off")
    endif()

    file(STRINGS "${NVRHI_OPTIX_INCLUDE_DIR}/optix.h" _nvrhi_optix_version_line REGEX "^#define OPTIX_VERSION [0-9]+")
    string(REGEX REPLACE "^#define OPTIX_VERSION ([0-9]+).*" "\\1" _nvrhi_optix_version "${_nvrhi_optix_version_line}")
    if (NOT _nvrhi_optix_version OR _nvrhi_optix_version LESS 90000)
        message(FATAL_ERROR "NVRHI_WITH_OPTIX: ${NVRHI_OPTIX_INCLUDE_DIR} is OptiX ${_nvrhi_optix_version}; 9.0 (90000) "
            "or newer is required")
    endif()
    message(STATUS "NVRHI: OptiX ${_nvrhi_optix_version} headers in ${NVRHI_OPTIX_INCLUDE_DIR}")
endif()
