# Runtime loading has no dependency on the Slang compiler or CUDA toolchain.
target_sources(nvrhi PRIVATE src/common/shader-program.cpp include/nvrhi/shader-program.h)
target_compile_definitions(nvrhi PUBLIC NVRHI_WITH_SLANG=$<BOOL:${NVRHI_WITH_SLANG}>)
if(NVRHI_WITH_SLANG)
    if(NOT TARGET ShaderTool::Artifacts)
        find_package(ShaderTool CONFIG REQUIRED COMPONENTS Artifacts)
    endif()
    target_link_libraries(nvrhi PRIVATE ShaderTool::Artifacts)
endif()
