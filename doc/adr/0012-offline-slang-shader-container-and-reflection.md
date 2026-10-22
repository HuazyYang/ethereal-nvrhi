# 0012. Offline Slang shader containers; reflection on `IShaderLibrary` and `IShader`; the version policy

- Status: Accepted
- Date: 2026-10-22
- Plan: [`../plans/2026-10-22-slang-cuda-optix-backend.md`](../plans/2026-10-22-slang-cuda-optix-backend.md), steps 3, 4, 10

## Context

The CUDA backend ([ADR 0011](0011-cuda-optix-backend.md)) has to know where each parameter of a Slang kernel lives in
its `SLANG_globalParams` block, and which HLSL register an NVRHI binding of that parameter uses. The user also wants
applications to see Slang's reflection through `IShaderLibrary` and `IShader`, with no separate `I*Program` interface.
They chose reflection that is produced **offline**: no `slang.dll` at run time. NVRHI keeps its own ABI-safe copy of
the information.

They also decided that the work does not bump `c_HeaderVersion`, and that existing interfaces may change.

## Decision

**The container.** `include/nvrhi/shader-container.h` defines the NVRHI shader container. It is plain C and includes
nothing from NVRHI, so ShaderTool vendors an identical copy (`include/nvrhi-shader-container.h` in the shader-tool
repository).

- A header (magic `NVSC`, version 1.0) is followed by a section table.
- A CODE section holds one target's code: DXBC, DXIL, SPIR-V, CUDA PTX (NUL-terminated), OptiX PTX or OptiX-IR.
- A REFLECTION section with the same target describes that code. It holds tables of entry points, parameters (global
  ones first, then the entry points' own), types and fields, plus a string table. All offsets and sizes are in that
  target's layout.

On CUDA and OptiX targets a parameter carries both:
- its offset and size in the global block (or in the kernel argument block / SBT record data), and
- the HLSL register (space, slot) that the same parameter gets in an HLSL layout of the program. ShaderTool lays the
  program out a second time for that, and matches the parameters by name.

This cross-map is what NVRHI binding sets use.

**Producing containers.** ShaderTool's `container` mode (`ShaderTool container --container-targets dxil,spirv,ptx,optix`)
compiles one module with every `[shader]` entry point into one `.nvsc`. The CMake functions are
`shadertool_add_shader_containers()` and, in the aggregate, `ethereal_compile_shader_containers()`. A DXIL or DXBC
section holds one non-ray-tracing entry point, or a library of ray-tracing entry points.

**Reading containers.** `src/common/shader-container.cpp` validates every offset and index. It is used by:
- `createShaderLibrary` and `createShader` of every backend: D3D11 takes the DXBC section, D3D12 DXIL, Vulkan SPIR-V,
  and CUDA its PTX sections;
- the C lookups `nvrhiFindShaderParameter`, `nvrhiFindShaderEntryPoint`, `nvrhiFindShaderEntryPointParameter`,
  `nvrhiFindShaderField`, `nvrhiIsShaderContainer` and `nvrhiGetShaderContainerCode`.

Plain bytecode keeps working, without reflection.

**The API** (`include/nvrhi/shader-reflection.h`). `ShaderLibraryReflection`, `ShaderEntryPointReflection`,
`ShaderParameterReflection`, `ShaderTypeReflection` and `ShaderFieldReflection` are data structs with
`nvrhi::string` / `nvrhi::vector` members and layout asserts. New virtual methods return non-owning views of them;
each returns `nullptr` without reflection:

```cpp
// IShaderLibrary
const ShaderLibraryReflection* getReflection() const noexcept;
// IShader
const ShaderEntryPointReflection* getReflection() const noexcept;
const ShaderLibraryReflection* getLibraryReflection() const noexcept;   // its types, parameters, other entry points
```

**The version policy.**
- `c_HeaderVersion` stays **28**.
- Existing interfaces gain methods in place (`IShader`, `IShaderLibrary`), `GraphicsAPI` gains `CUDA`, and the
  per-backend creation exports are removed ([ADR 0010](0010-graphics-factory.md)).
- [`../com-interface-abi.md`](../com-interface-abi.md) §3.4 is amended: NVRHI and its consumers are built from one
  source tree, so a change to an interface or a public struct no longer requires a new version or a derived `IFoo1`
  interface. Every other rule of that document still applies and is still checked (`nvrhi_abi_lint`,
  `nvrhi_header_hygiene`, `nvrhi_header_tus`).
- The CMake package keeps `COMPATIBILITY ExactVersion`, so an installed package is only used with the headers it was
  built from.

## Consequences

- Applications can build binding layouts from the reflection (`tests/cuda-tests-shaders.inl` does, for an OptiX
  record), and on every backend see where a constant-buffer field lives.
- The containers record what ShaderTool cannot describe (reported with the ShaderTool work):
  - resources nested in parameter blocks or structs get no register;
  - `$Globals` offsets of loose uniforms are missing on PTX targets;
  - there is no marker for "no binding" (push constants and record data report 0/0);
  - there is no matrix-layout flag.

  These are additions for a later minor version of the format (`NVRHI_SC_VERSION_MINOR`).
- **Vulkan bindings.** SPIR-V reflection reports the shifted Vulkan bindings the module was compiled with (NVRHI's
  default offsets t 0, s 128, b 256, u 384 in the CMake functions).
- **ABI between builds.** Applications built against an older NVRHI must be rebuilt. That was already the case for
  every change in this tree, and `verifyHeaderVersion` cannot detect it any more.

## Alternatives

- **Slang at run time** (the slang-rhi model: libraries made from a linked `slang::IComponentType`, Slang's own
  reflection objects). Rejected by the user: it needs `slang.dll` and NVRTC at run time, and Slang's types would cross
  NVRHI's ABI.
- **Derived interfaces (`IShaderLibrary1`) and a version bump.** This is the documented rule, set aside by the user's
  decision for this branch.
- **A separate `IShaderProgram` interface** (the earlier `slang-next` attempt). Rejected by the user.
