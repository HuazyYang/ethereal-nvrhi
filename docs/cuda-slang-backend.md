# CUDA and OptiX backend

The CUDA backend implements the existing NVRHI device, command list, buffer,
texture, binding, compute pipeline and ray tracing interfaces. Public device
creation is in `include/nvrhi/cuda.h`; native interoperability is in
`include/nvrhi/interop.h`. It uses the CUDA Driver API and OptiX SDK, with no
CUDA Runtime or runtime Slang/NVRTC compiler dependency.

## Build and execution

Enable `NVRHI_WITH_SLANG`, `NVRHI_WITH_CUDA`, and optionally `NVRHI_WITH_OPTIX`.
Link `nvrhi::cuda`. Set `OPTIX_ROOT` to the OptiX SDK when enabling ray tracing.
Set `NVRHI_CUDA_SHADER_TOOL` to an offline ShaderTool executable when building
standalone GPU integration tests; aggregate builds use `ShaderTool::ShaderTool`.
On Windows with localized MSVC, use a UTF-8 console (`chcp 65001` after Visual
Studio setup) before CMake configure and build. Verify `ninja -t deps` lists
headers; changing `VSLANG` alone does not work without that language pack.

`cuda::DeviceDesc::deviceOrdinal` explicitly selects the CUDA adapter. Each device
retains its primary context and owns three streams. Every native operation pushes
and restores that context. Command lists record CPU closures; submission snapshots
retain resource handles, host uploads, texture objects and temporary allocations
until the queue completion event finishes. Reopening a submitted list is safe.
Cross-queue users must express resource dependencies with queue waits.

Slang artifacts use the measured Slang 2026.10.2 CUDA ABI: module constant data in
`SLANG_globalParams`, and native packed by-value compute entry arguments. Global
updates and launches sharing a shader module are serialized across streams.
Structured buffers carry pointer and element count; byte-address buffers carry
pointer and byte count. A constant-buffer parameter carries a device pointer.
Texture/sampler pairing is explicit in the offline artifact. Declare distinct
texture parameters to bind one resource with different sampler states.

## Ray tracing and local material bindings

Use ordinary `rt::PipelineDesc`, `rt::ShaderTableDesc`, `rt::State`,
`rt::GeometryDesc` and `rt::InstanceDesc`. Supported geometry includes indexed and
non-indexed triangles and procedural AABBs. Triangle `GeometryDesc::transform`
becomes a native OptiX pre-transform. BLAS/TLAS build and update, automatic
compaction, instance hit-group offsets and same-device acceleration-structure
clone are implemented. Clones use a device copy plus OptiX relocation and retain
referenced child structures independently of the source object.

Local shader data comes from ray-stage entry parameters:

```slang
struct Material { uint value; uint3 padding; };
[shader("closesthit")]
void shade(inout Payload payload, BuiltInTriangleIntersectionAttributes hit,
           uniform ConstantBuffer<Material> material)
{
    payload.value = material.value;
}
```

The compiler assigns the parameter's logical binding slot (a sole constant buffer
uses `b0`). Set the shader/hit-group `bindingLayout`, create a matching binding set,
and supply it to `setRayGenerationShader`, `addMissShader`, `addHitGroup` or
`addCallableShader`. Reflected `ST_SCOPE_SHADER_RECORD` offsets are packed after
the OptiX SBT header and records are padded to OptiX alignment. Raygen, miss and hit
records are GPU tested. All nonempty local layouts in a hit group must agree.
Inline uniform structs are rejected because the existing NVRHI shader-table API
has no raw local-constant setter; use constant-buffer/resource parameters.

The pinned Slang compiler requires `ReportHitOptix` for portable procedural
intersection code; its ordinary `ReportHit` intrinsic does not target CUDA.
The integration fixtures show the integer attribute representation.

## Textures and interoperability

Native CUDA arrays support uncompressed scalar/two/four-channel integer,
normalized and floating formats. Sampling supports bounded mip/layer views and
explicit sampler state. Float and integer clears support selected subresources;
half/normalized conversion is handled on the CPU, retaining one plane per mip
rather than a whole volume for 3D clears. Clear uploads are correct but have not
been tuned as a GPU fill implementation.

External imports retain borrowed native owners or duplicate OS handles. Tests
cover D3D12 placed buffers at nonzero heap offsets, D3D11 textures/keyed mutexes
and fences, Vulkan memory at nonzero binding offsets and timeline semaphores, and
separate CUDA devices on one physical adapter. Multi-GPU peer tests execute only
when a compatible second adapter is available; there is no hidden host-copy
fallback.

## Explicit limitations

- Graphics/raster pipelines, bindless descriptors, sparse/virtual textures and
  depth/stencil operations are unsupported.
- Typed `Buffer`/`RWBuffer` parameters are rejected: pinned Slang CUDA codegen can
  silently emit an empty kernel for them. Use structured/raw buffers.
- Writable partial layer views, texture dimension/format reinterpretation and
  nonidentity swizzles are unsupported. CUDA surface objects have no layer-view
  offset. Normalized surface stores require shader conversion and are rejected.
- Motion/advanced ray geometry, micromaps, and AABB geometry pre-transforms are
  unsupported. Callable groups are created but not GPU covered yet.
- Only Windows on an RTX 4050 has been exercised; Linux and true multi-GPU
  interoperability remain hardware/platform validation work.

## Verification

`ctest -R '^cuda_'` covers manual PTX compute, actual Slang compute/texture ABI,
triangles and procedural rays, local SBT materials, geometry transforms, BLAS/TLAS
clones, native cross-API interop and texture views/clears. Static and shared builds
pass all six tests. Compute Sanitizer reports zero errors for local SBT/clone and
texture-view/clear tests (as well as the earlier compute/ray suite).

Reference contracts: [OptiX acceleration structures](https://raytracing-docs.nvidia.com/optix9/api/group__optix__host__api__acceleration__structures.html)
and [CUDA texture objects](https://docs.nvidia.com/cuda/archive/12.9.0/cuda-driver-api/group__CUDA__TEXOBJECT.html).
