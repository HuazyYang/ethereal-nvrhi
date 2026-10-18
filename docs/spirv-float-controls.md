# Explicit SPIR-V rounding modes

Vulkan shader and shader-library creation checks explicit `RoundingModeRTE` and
`RoundingModeRTZ` execution modes against the physical device's advertised
16/32/64-bit float-control properties. An unsupported requested mode returns
`FE_UNSUPPORTED` and clears the output handle before creating a shader module.
Malformed instruction lengths or headers return `FE_INVALID_ARGS`. Public
interfaces, IIDs and descriptor layouts are unchanged.

The implementation scans bounded instruction spans with unaligned-safe reads.
It does not replace SPIR-V validation or check every execution-mode combination;
Vulkan still validates the module's other capabilities and independence rules.
Shaders without these explicit rounding modes retain the device's normal behavior.
The runtime does not rewrite bytecode or silently substitute a different mode.

A kernel whose serial arithmetic depends on round-to-nearest-even must request
RTE explicitly; `precise`/NoContraction alone does not select a Vulkan rounding
mode. GVDB dense regional downsampling requests RTE for 32-bit arithmetic. Both
AMD Radeon 780M and NVIDIA RTX 4050 hardware advertise that support in the local
validation environment. GPU division accuracy remains a separate concern, so the
query uses exact integer quotient/remainder division at sampling boundaries.

The CPU-only `nvrhi_spirv_float_controls` regression covers unsupported width/mode
requirements, RTE/RTZ combinations, unchanged outputs on parse failure, truncated
instructions, zero instruction lengths and unaligned artifact storage. Native
regional-query tests exercise the supported path on both local Vulkan adapters.

See the [Vulkan float-controls extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_float_controls.html)
and [SPIR-V environment rules](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).
