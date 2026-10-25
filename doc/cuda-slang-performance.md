# Slang kernels on the CUDA backend: performance pitfalls

The CUDA and OptiX code of the CUDA backend comes from Slang: ShaderTool compiles a `.slang` module to
CUDA C++ and NVRTC compiles that to PTX (`slangc -target ptx` takes the same path; Slang has no other
route to PTX). NVRTC is the same compiler as nvcc of the same CUDA release: the same NVVM optimizer and
the same `ptxas`. What differs from hand-written CUDA is the input. Slang's CUDA has its own shape, and
the optimizer does less with it. Slang's optimization level does not help: `-O0` to `-O3` emit
byte-identical CUDA, the PTX with and without `-O3` is byte-identical, and no optimization flag reaches
NVRTC.

So the Slang source decides. The two cases below were found when the GVDB samples moved from nvcc-built
CUDA to Slang (plan `2026-10-22-slang-cuda-optix-backend`): the same algorithms ran 25-48% slower, in the
compute raycast kernels and in the OptiX intersection programs alike. Both are fixed in
`ethereal-samples/src/gvdb_samples/samples/sample-utils/kernels/GVDBRaycast.slangh`.

## Case 1: loop-invariant reads stay inside the loop

**Symptom.** A marching loop reads the same fields of a struct behind a pointer on every iteration:
its bounds, a texture handle, a scale factor.

```slang
for (int iter = 0; iter < MAX_ITER && p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < gvdb->res[0] &&
                   p.y < gvdb->res[0] && p.z < gvdb->res[0]; iter++)
{
    if (texel(gvdb, chan, p + o) >= ctx.thresh.x) { ... }   // texel(): gvdb->volIn[chan], gvdb->atlas_rcp
    p += ctx.steps.x * dir;
}
```

**Cause.** Slang lowers each `&&` of the condition to a branch over a `bool` temporary:

```cpp
bool _S84;
if (_S83) { _S84 = ((&dda_1)->p_0.x) <= gvdb_10->res_0[lev_3]; } else { _S84 = false; }
```

The reads of `gvdb->res` then sit in conditionally executed blocks, and the optimizer does not hoist
them out of the loop. nvcc hoisted them from the reference's identical C++ condition. In the machine code
the loop reloads the bound up to twice per iteration, and the bools turn into byte selects
(`PRMT` / `SEL`).

A second cost is new work, not a missed optimization. Slang's `Sample` takes normalized coordinates, so
the backend creates texture objects with `CU_TRSF_NORMALIZED_COORDINATES`, and a helper that converts
voxel coordinates reads the scale (`gvdb->atlas_rcp`, three floats) and multiplies on every sample. The
reference's `tex3D(handle, x, y, z)` took voxel coordinates.

**Fix.** Read loop invariants into locals before the loop, and test ranges without a `&&` chain that
contains reads:

```slang
// texel() of one channel with the texture handle and the atlas scale read once.
struct ChannelTexels
{
    Sampler3D<float>.Handle texture;
    float3 scale;
    __init(VDBInfo* gvdb, uint8_t chan) { texture = Sampler3D<float>.Handle(gvdb->volIn[chan]); scale = gvdb->atlas_rcp; }
    float at(float3 p) { return texture.Sample(p * scale); }
};

bool insideRangeExclusive(float3 p, float limit) { return all(p >= 0.f) && all(p < limit); }

const ChannelTexels texels = ChannelTexels(gvdb, chan);
const float res = float(gvdb->res[0]);
for (int iter = 0; iter < MAX_ITER && insideRangeExclusive(p, res); iter++)
{
    if (texels.at(p + o) >= ctx.thresh.x) { ... }
    p += ctx.steps.x * dir;
}
```

Values that change only at known points are cached and refreshed there. The hierarchical DDA of
`rayCast` keeps `res` and `dim` of the current level in locals and reloads them only when it steps down
to a child or back up a level. The comparisons are the same, so results are unchanged (a NaN still
fails them). The multiply by the scale remains, but its operand is in a register.

## Case 2: a repeated chain of dependent reads is not merged

**Symptom.** The same lookup is written twice, once as a test and once for its value:

```slang
if (inside && isBitOn(gvdb, node, b)) {        // isBitOn(): getChild(gvdb, node, b) != -1
    if (lev == 1) {
        brickFunc.traceBrick(gvdb, chan, getChild(gvdb, node, b), ...);   // the same lookup again
    } else {
        nodeid[lev] = getChild(gvdb, node, b);                            // or here
```

`getChild()` is a chain of dependent reads: the node's child-list id, then the level's child-list base and
stride in `VDBInfo`, then the child entry. Each step waits for the one before it.

**Cause.** The reference's C++ had the same duplicate. nvcc shared the first read of the chain
(`node->mChildList`) between the two lookups. The Slang build ran the whole chain again in each branch,
three times per DDA step in the machine code, as generic `LD` loads where nvcc emitted `LDG`. The
`inside &&` guard, which puts the first lookup in a branch of its own, probably makes the merge harder;
the exact reason in the optimizer was not established.

**Fix.** Do the lookup once and test its value:

```slang
const int child = inside ? getChild(gvdb, node, b) : -1;   // -1 exactly when isBitOn() is false
if (child != -1) {
    if (lev == 1) brickFunc.traceBrick(gvdb, chan, child, ...);
    else { lev--; nodeid[lev] = child; node = getNode(gvdb, lev, child, vmin); ... }
}
```

## Result

Median kernel times (Nsight Systems, D3D12, RTX 4050 Laptop GPU), against the nvcc build of the same
samples before the conversion:

| Kernel | nvcc build | Slang, before | Slang, after |
|---|---|---|---|
| fluid-surface, OptiX launch | 10.56 ms | 13.9 ms | 10.40 ms |
| interactive-optix trilinear, OptiX launch | 4.21 ms | 5.71 ms | 4.33 ms |
| interactive-optix volume, OptiX launch | 5.59 ms | 7.22 ms | 5.82 ms |
| 3d-print, `gvdbRaySurfaceVoxel` | 0.294 ms | 0.436 ms | 0.301 ms |

On `gvdbRaySurfaceVoxel` alone: case 1 in `rayCast` and its brick took 0.436 to 0.361 ms, and case 2 took
0.361 to 0.318 ms. The deterministic sample images are bit-identical before and after.

Tried without effect, so not worth repeating: `[ForceInline]` on the device functions (NVRTC inlines
them already), moving the per-level arrays of the DDA into scalar registers (their local-memory accesses
hit L1), Slang's `-O3`, binding single-level textures as CUDA arrays instead of mipmapped arrays, and on
the OptiX side the payload in registers, the launch parameters behind a pointer and the traversable
graph flags.

## Finding such cases

1. **Isolate the kernel.** Time kernels with Nsight Systems (`nsys profile --trace=cuda`, then the
   `cuda_gpu_kern_sum` report or the exported SQLite table `CUPTI_ACTIVITY_KIND_KERNEL`; OptiX launches
   appear as "NVIDIA internal"). Compare against a reference build of the same scene, and check whether a
   compute kernel sharing the code shows the same gap: if it does, the cause is in the shared code, not
   in OptiX.
2. **Compare resources.** Extract the PTX from the container and run `ptxas -arch=sm_XX -O3 -v` on it:
   registers, stack frame and spills per entry point.
3. **Read the loop.** `cuobjdump -sass -fun <kernel>` on the assembled cubin. Between the loop label and
   its back branch, every `LDG` / `LD` of a value that does not change in the loop is a candidate for
   case 1, and repeated identical load chains are case 2. Byte selects (`PRMT ... 0x7610`,
   `PRMT ... 0x9910`) around branches mark lowered `&&` conditions.
4. **Check the emitted CUDA** with `slangc -target cuda` (it does not need NVRTC) to see how a construct
   was lowered.

Nsight Compute shows this directly, source line by source line, but needs access to the GPU
performance counters, which by default requires administrator rights on Windows (`ERR_NVGPUCTRPERM`).
