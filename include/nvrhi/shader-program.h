// SPDX-License-Identifier: MIT
#pragma once

#include <nvrhi/nvrhi.h>

namespace nvrhi
{
    // The executable artifact contains target-specific code and a snapshot of the
    // linked Slang program's reflection. No compiler is needed by this interface.
    enum class ShaderParameterScope : uint8_t { Global, EntryPoint, ConstantBuffer, ShaderRecord };

    struct ShaderEntryPointDesc
    {
        string name;
        string symbol;
        ShaderType shaderType = ShaderType::None;
        uint32_t threadGroupSize[3] = { 1, 1, 1 };
        uint32_t globalParamsSize = 0;
        // Compute: packed launch arguments. Vulkan/OptiX ray stages: SBT payload bytes.
        uint32_t entryParamsSize = 0;
    };

    struct ShaderParameterDesc
    {
        string name;
        ResourceType resourceType = ResourceType::None;
        ShaderParameterScope scope = ShaderParameterScope::Global;
        uint32_t registerIndex = 0;
        uint32_t registerSpace = 0;
        uint32_t targetIndex = 0;
        uint32_t targetSpace = 0;
        uint32_t parentParameter = ~0u;
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t elementStride = 0;
        uint32_t count = 1;
        // A distinct texture/sampler pair requires a distinct texture binding on CUDA.
        uint32_t samplerRegisterIndex = ~0u;
        uint32_t samplerRegisterSpace = 0;
    };

    NVRHI_IID(IShaderProgram, "7e3243b0-49f0-4267-832b-4b8d649d1933")
    struct IShaderProgram : IRHIObject
    {
        NVRHI_DECLARE_UUID_TRAITS(IShaderProgram)

        virtual uint32_t getEntryPointCount() const noexcept = 0;
        virtual FRESULT getEntryPointDesc(uint32_t index, ShaderEntryPointDesc& desc) const noexcept = 0;
        virtual uint32_t getParameterCount(uint32_t entryPointIndex) const noexcept = 0;
        virtual FRESULT getParameterDesc(uint32_t entryPointIndex, uint32_t parameterIndex,
            ShaderParameterDesc& desc) const noexcept = 0;

        // A new owned reference is returned on success; failed creation clears output.
        virtual FRESULT createShader(uint32_t entryPointIndex, IShader** shader) noexcept = 0;
        // One global layout per logical register space; shader-record data is excluded.
        // Vulkan offsets and descriptor set are derived from native reflection. Remaps
        // that cannot be represented by BindingLayoutDesc return FE_UNSUPPORTED.
        // The descriptor is replaced only on success.
        virtual FRESULT getBindingLayoutDesc(uint32_t entryPointIndex, uint32_t registerSpace,
            BindingLayoutDesc& desc) const noexcept = 0;

        // Source names take precedence over native symbols. An empty name is allowed
        // only when exactly one entry matches type (None matches every stage).
        // Failure sets index to ~0u; ambiguous names return FE_INVALID_ARGS.
        virtual FRESULT findEntryPoint(const char* name, ShaderType type, uint32_t* index) const noexcept = 0;
        // Preserves every caller option. A non-None shaderType must match the entry.
        // entryName is replaced with the native symbol; empty debugName uses the source name.
        virtual FRESULT createShaderWithDesc(uint32_t entryPointIndex, const ShaderDesc& desc,
            IShader** shader) noexcept = 0;
    };

    using ShaderProgramHandle = AutoPtr<IShaderProgram>;

    // Copies the artifact and retains device. Target and version are checked before
    // creating GPU objects. The input bytes may be released after this call.
    NVRHI_C_API FRESULT nvrhiCreateShaderProgram(IDevice* device, const void* artifact,
        size_t artifactSize, IShaderProgram** program) noexcept;

    inline FRESULT createShaderProgram(IDevice* device, const void* artifact,
        size_t artifactSize, IShaderProgram** program) noexcept
    {
        return nvrhiCreateShaderProgram(device, artifact, artifactSize, program);
    }
}
