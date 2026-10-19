# Binding frequencies, writable binding sets and the transient descriptor arena

- Status: Abandoned
- Approved: 2026-10-13
- Done: 2026-10-13
- Abandoned: 2026-10-15. ADR 0008 revision 2 overturned the binding-frequency design this plan carried out.
  The `binding-freq` branches were rebuilt from their branch points by
  [2026-10-15-immutable-binding-sets.md](2026-10-15-immutable-binding-sets.md); the commits recorded below are
  reachable only through the tag `binding-freq-rev1` in each repository.
- Decisions: [ADR 0008](../adr/0008-binding-frequencies-and-descriptor-allocation.md)

This plan carries out ADR 0008. Sections 1-3 are the plan as approved; the results under section 4 and
sections 5-6 were filled in as the work was done. Where the plan and the code differ, the code is
authoritative and the difference is listed under "Deviations".

## 1. Context

ADR 0008 decides four things, in dependency order:

1. **B.1** A shader-visible D3D12 heap that any in-flight command list references is parked on the lifetime
   tracker by `StaticDescriptorHeap::Grow` and released when the last such instance completes.
2. **B.2, B.3** `StaticDescriptorHeap` allocates ranges with an OffsetAllocator per heap; Vulkan binding sets
   come from a device-level descriptor pool manager instead of one pool per set.
3. **B.4** Each D3D12 command list owns a transient descriptor arena per shader-visible heap type, taking
   chunks from the heap's general allocator, with a per-command-list cap on transient samplers; each Vulkan
   command buffer owns a reset-per-recycle transient pool list.
4. **B.5** `BindingLayoutItem` gains `BindingFrequency` (`Static`, `Mutable`, `Dynamic`); `IBindingSet` gains
   `setItem`, `setItems` and `getGeneration`; `ICommandList` gains
   `commitBindingSets(const BindingSetVector&, ResourceStateTransitionMode)`. Mutable items are written on the
   set and committed per command list into the arena; dynamic items (root CBV, root SRV, push constants) are
   written on the set and resolved at the draw. `ResourceType::VolatileConstantBuffer` is retired.

Repositories and branches:

| Repository | Path in the aggregate | Branch |
| --- | --- | --- |
| nvrhi | `ethereal-nvrhi` | `ethereal-dev` |
| donut (fork) | `ethereal-donut` | `ethereal-dev` (the `framegraph` work was merged on 2026-10-12) |
| samples | `ethereal-samples` | `main` |
| aggregate root | `.` | `main` (submodule pointer bumps only) |

Hosts: the Windows host builds and runs D3D11, D3D12 and Vulkan through the root `build.ps1`; the Linux
host builds and runs Vulkan only through the `linux` CMake preset. Steps marked **Windows** cannot be
verified on Linux. The Vulkan validation layer must be available to the Linux runs (see the build notes
in the aggregate README); Debug samples refuse to start without it.

## 2. Decisions taken at approval

- Transient chunks come from the heap's general allocator, not a reserved region. This is reopened only if
  step 1 ends on fixed-size shader-visible heaps.
- `IBindingSet` keeps its IID; `IDevice` and `ICommandList` gain methods in place. No `IBindingSet1` or
  `ICommandList2`. `c_HeaderVersion` is bumped at step 4 and again at step 11 when the deprecated helpers go.
- The old helpers (`BindingLayoutItem::VolatileConstantBuffer`, `BindingSetItem::PushConstants`,
  `ResourceType::VolatileConstantBuffer`) stay as deprecated aliases from step 4 until step 11, so every
  commit in every repository builds and the donut and samples sweep need not land with the header.
- Dynamic items are limited to `ConstantBuffer`, `StructuredBuffer_SRV`, `RawBuffer_SRV` and `PushConstants`.
  Root UAVs are a follow-up.
- `createBindingSet` keeps its signature; mutable and dynamic items become optional in the desc.

## 3. Plan

Each step is one or more commits that build on their own. Steps 1-3 change no public header and no
behaviour visible to applications; they are verified by the existing tests and samples. Steps 4-9 are the
feature. Steps 10-12 are consumers and documentation.

### Step 1: B.1 growth test, then parking fix (nvrhi, **Windows**)

1. A device test that creates a D3D12 device with a small `shaderResourceViewHeapSize`, records a command
   list that binds sets from the heap, executes it, and while it runs creates enough sets to force `Grow`.
   Expected before the fix: a debug-layer error or a GPU fault referencing the released heap. If the test
   cannot provoke the fault, record that under section 6 and keep the fix anyway; the rule in B.1 is cheap.
2. `StaticDescriptorHeap::Grow` (`src/d3d12/d3d12-descriptor-heap.cpp`) hands the old shader-visible heap
   to the device's lifetime tracker; the heap is released when the last instance created before the grow
   completes. `commitDescriptorHeaps` (`src/d3d12/d3d12-commandlist.cpp`) is unchanged.
3. Rerun the test; it passes. If parking interacts badly with `commitDescriptorHeaps`, fall back to
   fixed-size shader-visible heaps and reopen the B.4 chunk source before step 5.

### Step 2: B.2 OffsetAllocator (nvrhi)

1. `src/common/offset-allocator.h` and `.cpp`: Sebastian Aaltonen's OffsetAllocator in namespace
   `nvrhi::details`, asserts mapped to nvrhi's, 16-bit node mode off, the MIT licence text at the top of
   both files and in the third-party notices.
2. `tests/offset-allocator.cpp`: the upstream tests, including the zero-fragmentation case, on gtest.
3. `StaticDescriptorHeap` (`src/d3d12/d3d12-descriptor-heap.cpp`, `src/d3d12/d3d12-backend.h`): one
   allocator per heap replaces the `std::vector<bool>` scan. `allocateDescriptors` and `releaseDescriptors`
   keep their signatures; the heap keeps a side table from base index to node index so that the public
   `IDescriptorHeap` index-based methods still work. `Grow` creates a larger allocator and replays the live
   ranges.
4. Owners store the `Allocation`: `BindingSet`, `DescriptorTable`, `Texture` view caches in
   `d3d12-backend.h`, `d3d12-resource-bindings.cpp`, `d3d12-texture.cpp`. The side table then serves only
   the public index-based methods.
5. `d3d12::DeviceDesc::maxDescriptorAllocations` (default 16384) in `include/nvrhi/d3d12.h`.
6. `~Device` reports each heap's peak usage and largest free block through the message callback at
   `MessageSeverity::Info`.

### Step 3: B.3 Vulkan descriptor pool manager (nvrhi)

1. A `DescriptorPoolManager` in `src/vulkan/vulkan-backend.h` and `src/vulkan/vulkan-resource-bindings.cpp`:
   pools created with the free-descriptor-set bit, sized from `vulkan::DeviceDesc::descriptorPoolSets`
   (default 256) and per-type descriptor counts (defaults after Diligent's `DescriptorPoolSize`); a pool is
   added when none has room.
2. `Device::createBindingSet` allocates from it; `~BindingSet` frees its set back. The per-set
   `VkDescriptorPool` member goes.
3. The manager also hands out transient pools without the free bit, for step 6.

### Step 4: B.5 public header (nvrhi)

All in `include/nvrhi/nvrhi.h` unless noted. The validation layer and every backend compile against the
new header in the same commit; where a backend's real implementation comes in a later step, the method
returns `FE_UNSUPPORTED` and the limitation is listed here.

1. `enum class BindingFrequency : uint8_t { Static, Mutable, Dynamic }`.
2. `BindingLayoutItem::frequency` in the byte that is `unused` today; `setFrequency`; `operator==` compares
   it; the `PushConstants` initializer sets `Dynamic`. The `static_assert` on `sizeof(BindingLayoutItem)`
   stays at 8.
3. `BindingLayoutItem::VolatileConstantBuffer(slot)` becomes `ConstantBuffer(slot).setFrequency(Dynamic)` and
   is marked deprecated. `ResourceType::VolatileConstantBuffer` stays as a deprecated value that
   `createBindingLayout` normalises in every backend. `BindingSetItem::PushConstants` stays, deprecated,
   and is accepted and ignored by `createBindingSet`.
4. `c_MaxVolatileConstantBuffersPerLayout` and `c_MaxVolatileConstantBuffers` renamed to
   `c_MaxDynamicConstantBuffersPerLayout` and `c_MaxDynamicConstantBuffers`, the old names kept as
   deprecated aliases; `c_MaxDynamicShaderResourcesPerLayout = 4`.
5. `IBindingSet::setItem`, `setItems`, `getGeneration`, with the comments from ADR 0008 B.5.4.
   `DescriptorTable` in every backend answers `FE_UNSUPPORTED` and generation 0.
6. `enum class ResourceStateTransitionMode : uint8_t { None, Transition, Verify }` and
   `ICommandList::commitBindingSets`.
7. `d3d12::DeviceDesc`: `dynamicDescriptorChunkSize` (256), `dynamicSamplerChunkSize` (32),
   `maxDynamicSamplerDescriptors` (256). `vulkan::DeviceDesc`: `transientDescriptorPoolSets` (64) and
   per-type counts.
8. `c_HeaderVersion` 28 to 29.
9. `tests/abi_lint.py` and `tests/header_hygiene.cmake` pass; the `static_assert` block on struct layouts
   is unchanged.

Until steps 5-7: `setItem` returns `FE_UNSUPPORTED` on every backend; `commitBindingSets` applies the
transition mode and returns `FE_OK`; dynamic items still bind through the desc exactly as volatile constant
buffers do today, so donut runs unchanged.

### Step 5: B.4 and B.5 on D3D12 (nvrhi, **Windows**)

1. `BindingLayout` (`src/d3d12/d3d12-resource-bindings.cpp`): root-parameter segment per layout in the
   order root constants, root CBVs, root SRVs, static sampler table, mutable sampler table, static SRV/CBV/UAV
   table, mutable table. `descriptorRangesSRVetc`, `bindingLayoutsSRVetc` and the sampler equivalents split
   in two; `rootParametersVolatileCB` becomes `rootParametersDynamic` with the item type.
2. `BindingSet::createDescriptors` factored into a routine that writes a given list of items into a
   caller-supplied CPU handle range; the set uses it for its static ranges and, at generation 0, its mutable
   ranges.
3. `BindingSet::setItem` and `setItems`: validate against the layout, replace the item in the desc, hold the
   new resource, advance the generation. Static slot: `FE_INVALID_ARGS`.
4. Transient arena in `CommandList` (`d3d12-backend.h`, `d3d12-commandlist.cpp`): one per shader-visible
   heap type; chunk object on `referencedResources`; heap-pointer check at chunk acquisition; the sampler
   cap; exhaustion message naming the knob.
5. `CommandList::commitBindingSets`: per-set map (committed generation, version ranges, mode) cleared at
   `open`; version creation through the routine from item 2; state transitions for static and mutable items
   when the mode is `Transition`.
6. `setGraphicsState`, `setComputeState`, `setMeshletState`, `setRayTracingState` (`d3d12-graphics.cpp`,
   `d3d12-compute.cpp`, `d3d12-meshlets.cpp`, `d3d12-raytracing.cpp`) and `setGraphicsBindings` /
   `setComputeBindings`: slot comparison on (pointer, committed version); static tables from the set, mutable
   tables from the version; dynamic items resolved at the draw through the path that applies volatile
   constant buffers today, extended to `SetGraphicsRootShaderResourceView` and the compute equivalent;
   states for dynamic items required at the draw when the remembered mode is `Transition`.
7. `setResourceStatesForBindingSet` (`d3d12-state-tracking.cpp`) over a desc and a layout; the public
   `ICommandList::setResourceStatesForBindingSet(IBindingSet*)` calls it with the set's current content.
8. `d3d12::Device` normalises `VolatileConstantBuffer` layout items.

### Step 6: B.4 and B.5 on Vulkan (nvrhi)

1. `Device::createBindingLayout` (`src/vulkan/vulkan-resource-bindings.cpp`): dynamic `ConstantBuffer` maps
   to `eUniformBufferDynamic` as volatile buffers do today (`vulkan-constants.cpp`), dynamic SRVs to
   `eStorageBufferDynamic`; `VolatileConstantBuffer` normalised.
2. `BindingSet::setItem` and `setItems` as on D3D12.
3. Transient pool list on `TrackedCommandBuffer` (`vulkan-backend.h`, `vulkan-commandlist.cpp`,
   `vulkan-queue.cpp`): pools from the manager of step 3; reset and returned on recycle.
4. `CommandList::commitBindingSets`: version = allocate from the transient list, `VkCopyDescriptorSet` of the
   static and dynamic descriptors from the set's persistent set, `vkUpdateDescriptorSets` for the mutable
   items; per-set map as on D3D12.
5. `bindBindingSets` and the four `set*State` (`vulkan-graphics.cpp`, `vulkan-compute.cpp`,
   `vulkan-meshlets.cpp`, `vulkan-raytracing.cpp`): bind the version; dynamic offsets from the set's current
   dynamic items; a dynamic item whose buffer object differs from the bound set's gets a transient copy at the
   draw, cached per command list by (set, version, dynamic buffer objects).
6. `vulkan-state-tracking.cpp` as for D3D12.
7. The dynamic-descriptor device limits checked in `createBindingLayout`.

### Step 7: B.5 on D3D11 (nvrhi, **Windows**)

1. `BindingSet::setItem` and `setItems` (`src/d3d11/d3d11-resource-bindings.cpp`): replace the item in the
   desc and rebuild the per-stage arrays.
2. `commitBindingSets`: the transition mode only.
3. Bind-time and draw-time reads of the set's current content; a dynamic SRV issues `*SetShaderResources`
   at the draw; dynamic constant buffers go through the `*SetConstantBuffers1` path.

### Step 8: Validation layer (nvrhi)

`src/validation/validation-device.cpp`, `validation-commandlist.cpp`, `validation-backend.h`, by the table
in ADR 0008 B.5.6: layout admissibility and dynamic counts; set completeness; `setItem` type and frequency
checks; the per-command-list map of committed generation and mode; the `set*State` and draw checks; the
root-signature DWORD budget; the version-count warning; `Verify` state checks. The volatile-versus-regular
buffer rule is removed.

### Step 9: Tests (nvrhi, or donut `tests/` if the nvrhi harness has no real devices)

1. Mutable: write and commit between two draws, read back both results.
2. Dynamic: write a dynamic constant buffer and a dynamic SRV between two draws without a commit, once
   changing only the offset and once the buffer object; read back.
3. Transition mode: a resource left in the wrong state is reported by `Verify` and fixed by `Transition`.
4. Liveness: release a resource after writing it out of a set while a version is in flight; no fault.
5. Arena exhaustion: the error names the knob (**Windows**).
6. Step 1's growth test (**Windows**).
7. `setItem` on a static slot and on a descriptor table return the documented codes.

### Step 10: donut and samples sweep (donut, samples)

Every layout that uses `BindingLayoutItem::VolatileConstantBuffer` or `PushConstants` moves to
`ConstantBuffer(slot).setFrequency(Dynamic)` and the existing `PushConstants` initializer; every set drops
`BindingSetItem::PushConstants`. Sets keep giving their constant buffers in the desc, which is the optional
default for a dynamic item and binds as today. `BindingCache` is unchanged.

donut, 20 files: `src/app/ImGuiRenderPass.cpp`; `src/engine/CommonRenderPasses.cpp`,
`MaterialBindingCache.cpp`, `Scene.cpp`; `src/render/BloomPass.cpp`, `DeferredLightingPass.cpp`,
`DepthPass.cpp`, `DLSS.cpp`, `EnvironmentMapPass.cpp`, `ForwardShadingPass.cpp`, `GBufferFillPass.cpp`,
`JointsRenderPass.cpp`, `LightProbeProcessingPass.cpp`, `ManipulatorPass.cpp`, `MipMapGenPass.cpp`,
`PixelReadbackPass.cpp`, `SkyPass.cpp`, `SsaoPass.cpp`, `TemporalAntiAliasingPass.cpp`,
`ToneMappingPasses.cpp`.

samples, 41 files: `src/*` (aftermath, async_compute, bindless_rendering, ddgi_samples, gvdb_samples,
rt_bindless, rt_particles, rt_reflections, rt_shadows, vertex_buffer, vxgi_samples, work_graphs),
`benchmark/Asteroids/nvrhi/renderer.cpp` and `demos/nv_asteroids/src/*` (**Windows**, the benchmark and
demos build only there).

Then the FrameGraph consumers: `TemporalAntiAliasingPass` and `SsaoPass` adapters hold one set per layout,
`setItem` the rotated inputs in `execute`, and `commitBindingSets(state.bindings, Transition)` before
`setGraphicsState`; their `BindingCache` lookups go. Measure with `-stats` (section 4).

### Step 11: Remove the deprecated aliases (nvrhi, then donut and samples)

`BindingLayoutItem::VolatileConstantBuffer`, `BindingSetItem::PushConstants`,
`ResourceType::VolatileConstantBuffer` and the old constant names go; `c_HeaderVersion` 29 to 30; the
normalisation code and its warning go with them. donut and samples build clean against it; the aggregate
bumps all three submodules.

### Step 12: Documentation

`doc/ProgrammingGuide.md`, section "Binding Layouts and Sets": binding frequencies, the write-commit-bind
rule for mutable items, the next-draw rule for dynamic items, the transition mode, the `DeviceDesc` knobs.
ADR 0008 to Accepted; this plan to Done with sections 4-6 filled in.

## 4. Verification

Per step, before the commit:

- nvrhi builds in Debug and Release on both hosts; `tests/abi_lint.py` and the header hygiene check pass;
  the nvrhi tests pass.
- The aggregate builds in Debug and Release; `ctest` passes per config (32 tests per config at the time of
  writing; step 9 adds its own).
- Debug samples run under the validation layer with zero messages: `deferred_shading`, `deferred_shading_fg`,
  `async_compute_fg`, `variable_shading_fg` on Linux with `-vk`; the same plus D3D11 and D3D12 on Windows.

At the end:

- **A/B.** One FrameGraph sample with `-stats` before step 10 and after: binding sets created per frame,
  `BindingCache` misses per frame, descriptor allocations per frame, frame time. The expected result is zero
  per-frame set creation in the TAA and SSAO passes and no frame-time regression.
- **Screenshots.** The `-screenshot` outputs of the deterministic samples are compared with
  `ethereal-samples/tools/ab_compare.py` before and after; bit-exact for every sample listed as deterministic
  in the samples' build notes.
- **RenderDoc** (**Windows**). Capture one frame of `deferred_shading` on D3D12 and Vulkan before step 4 and
  after step 10: Vulkan descriptor sets of static-only layouts identical; D3D12 root signatures differ only by
  the root-parameter split.
- **Heap report.** The peak-usage report at device destruction from step 2, before and after step 10, for
  the SRV and sampler heaps.

### Results (2026-10-13)

Run on 2026-10-13 on the Linux host against the final tips (nvrhi `98096b5`, donut `b6c333a`, samples
`d4c5d5d`):

- nvrhi builds in Debug and Release (full aggregate, 0 errors; the only deprecation warnings left are donut's
  pre-existing `TextureCache` ones); `tests/abi_lint.py` and the header hygiene check pass (both are ctest
  entries).
- `ctest`: 21 tests per configuration, all pass (19 at the start, plus `nvrhi_test_offset_allocator` and
  `nvrhi_test_binding_frequencies`, the latter 23 cases under nvrhi's validation layer and the Khronos layer).
- MinGW compile check of D3D11, D3D12 and the validation layer: 0 errors, one pre-existing `align` attribute
  warning in `d3d12-meshlets.cpp`.
- Debug samples (built with `_DEBUG`, so nvrhi's validation layer is on; the Khronos layer forced), 20 s each
  with `-vk`: `deferred_shading`, `bindless_rendering`, `rt_shadows`, `async_compute`, `meshlets` with zero
  messages; `rt_particles` with only its pre-existing `Undefined-Value-StorageImage-FormatMismatch-ImageView`
  warning (earlier runs of `rt_shadows` and `async_compute` showed the same pre-existing warning). On this
  base no sample takes `-frames`; the `_fg` samples and D3D11/D3D12 are not available.
- Step-level runs: step 3's pool-manager stress program (1000 sets, 8 threads, bindless, dedicated pools) and
  step 10's `VXGISample` run, both clean; per-step builds, tests and sample runs are recorded under each step.
- Not run (section 6): the `-stats` A/B, the screenshot comparison, RenderDoc and the D3D12 heap report.

## 5. Execution record

All work was done on 2026-10-13 on a Linux host (GCC 13, Vulkan, NVIDIA RTX GPU). D3D11 and D3D12 code
was compile-checked with MinGW-w64 (nvrhi standalone, DX11 + DX12 + validation, Microsoft's
DirectX-Headers v1.717.0 for D3D12, mingw-w64's own D3D11/DXGI headers) and has **not been run**; the
Windows items are listed in section 6. Repositories and commits:

| Step | Repository | Commits |
| --- | --- | --- |
| Approval | nvrhi | `61fda40` (ADR 0008 + this plan) |
| 2 (items 1-2) | nvrhi | `049a1c7` |
| 3 | nvrhi | `7658254` |
| 4 | nvrhi | `4d396ed` |
| 7 | nvrhi | `610c2f3` |
| 1, 2 (items 3-6) | nvrhi | `56e7d0f` |
| 8 | nvrhi | `6408f71` |
| 6 | nvrhi | `0c218d9` |
| 5 | nvrhi | `9d40474` |
| 9 (Linux items) | nvrhi | `3245190` |
| 9 (validation cases) | nvrhi | `5d062ce` |
| 10 | donut, samples | `b6c333a`, `d4c5d5d` |
| 11 | nvrhi | `98096b5` |
| 12 | nvrhi | `8462841`, and the commit that adds this record |

**Branches.** The work is on a `binding-freq` branch in all four repositories (nvrhi, donut, samples and the
aggregate), not on the branches of the table in section 1: `ethereal-dev` and `main` were left at the commits
the aggregate `main` pins. The aggregate's `.gitmodules` names `binding-freq` for the three submodules.

**Base.** The aggregate branch `binding-freq` was created from `main` (`e02979c`), which records nvrhi
`ethereal-dev`, donut `ethereal-dev` and samples `main` **before** the RDG/framegraph work; that work lives
on the `framegraph` branches of all four repositories and was not merged into `ethereal-dev` as section 1
says. Consequences: no `_fg` samples, no `-frames`/`-screenshot`/`-stats` options, no FrameGraph consumers,
and 19 ctest tests per configuration at the start instead of 32. The affected items are recorded under the
steps and in section 6.

### Step 1 (D3D12, Windows): `56e7d0f`

`StaticDescriptorHeap::Grow` hands the old shader-visible heap to `Device::parkDescriptorHeap`, which records
each queue's `lastSubmittedInstance`; `Device::releaseCompletedParkedHeaps` (from `runGarbageCollection` and
`waitForIdle`) releases a parked heap once every queue has completed that instance. The CPU heap is released
at once. `commitDescriptorHeaps` is unchanged. The growth test is `runGrowthWhileInFlight` in
`tests/d3d12-descriptor-heap.cpp` (`nvrhi_d3d12_descriptor_heap`). Deviation: because `commitDescriptorHeaps`
already held the heap on the instance's `referencedNativeResources`, the test may not provoke a fault before
the fix (anticipated by item 1.1); the rule is kept. Found in passing: after a `Grow`, a recording command
list keeps the old heap bound until its next `set*State`, so a `writeDescriptorTable` in that window reaches
only the new heap (not a lifetime issue; section 6).

### Step 2: `049a1c7` (items 1-2), `56e7d0f` (items 3-6, Windows)

`src/common/offset-allocator.{h,cpp}` in `nvrhi::details`, in the `nvrhi` library; `tests/offset-allocator.cpp`
(`nvrhi_test_offset_allocator`, the eight upstream cases on gtest); licence in `ThirdPartyLicenses.txt`.
Deviations: no local slang-rhi copy existed, the files came from upstream; `uint*_t` instead of upstream's
typedefs; C++17 positional initialisation instead of designated initializers; `ASSERT` is `assert`.
`StaticDescriptorHeap`: one allocator per heap, node budget `min(maxDescriptorAllocations, size + 2)`;
`DescriptorRange` is `details::Allocation`; `allocateRange`/`releaseRange` (C++ cannot overload on the return
type, so no `allocateDescriptors` overload); the side table holds *every* live range because the `Grow`
replay must enumerate them (deviation from item 3); `Grow` replays gaps and ranges in offset order into a
fresh allocator, which hands out consecutive offsets while nothing has been freed, then frees the gaps.
Owners (`BindingSet`, `DescriptorTable`, `Texture` view caches, `Buffer` clear UAV) store the range;
framebuffer RTV/DSV descriptors stay on the index API. Shrinking a descriptor table keeps its range.
`~Device` reports size, peak, in-use descriptors and ranges and the largest free block per heap at `Info`.

### Step 3: `7658254`

`vulkan::DescriptorPoolManager` owned by the device (`m_Context.descriptorPoolManager`): shared pools with the
free bit (`descriptorPoolSets` = 256, `DescriptorPoolSizes` defaults in `vulkan.h`), a dedicated one-set pool
for bindless layouts, layouts larger than a shared pool and `eMutableEXT` layouts, and transient pools without
the free bit (`transientDescriptorPoolSets` = 64, `transientDescriptorPoolSizes` a quarter of the shared
sizes) handed out by `acquireTransientPool`/`releaseTransientPool`. Descriptor tables use the manager too.
Deviations: `BindingSet::descriptorPool` stays as a non-owning handle (`vkFreeDescriptorSets` and the
`VK_DescriptorPool` native-object query need it); the pool sizes cover only the descriptor types nvrhi
allocates; acceleration-structure pool sizes are omitted unless `VK_KHR_acceleration_structure` is enabled
(a validation error found by a stress program).

### Step 4: `4d396ed`

As planned, plus: `c_BindingSetVersionWarning` (4096); `BindingSetItem::ConstantBuffer` always produces
`ResourceType::ConstantBuffer`; the deprecated helpers carried `[[deprecated]]` (no consumer builds with
warnings as errors); the enum value was deprecated by comment; shared helpers in
`src/common/utils-internal.h` (`IsDynamicConstantBuffer`, `NormalizeBindingLayoutDesc`,
`NormalizeBindingSetDesc`, `CommitBindingSetStates`); `utils::CreateBindingSetAndLayout` derives `Dynamic` from
`BufferDesc::isVolatile`. Deviations: `commitBindingSets` returns `FS_OK`, the only success code; the D3D12
root-CBV path now honours `range.byteOffset` for regular buffers and tracks their state (the plan said it
"already handles both"); Vulkan dynamic offsets are emitted in slot order; D3D11 binds the push-constant
buffer from the layout item; a `PushConstants` layout item left `Static` is normalised to `Dynamic`;
`maxDescriptorAllocations` (item 2.5) was added here so the public headers changed once; the Vulkan knobs of
item 7 had landed in step 3.

### Step 5 (D3D12, Windows): `9d40474`

Root-parameter order as planned. `BindingLayout::tablesSRVetc[2]` / `tablesSamplers[2]`
(`c_StaticTable`, `c_MutableTable`), `rootParametersDynamic`; `BindingSet::writeDescriptors` (static) writes
one table into a caller-supplied handle range; `setItem`/`setItems`; `TransientDescriptorArena` per
shader-visible heap type with `DescriptorChunk` objects on `referencedResources`; `m_CommittedBindingSets`
cleared at `open` and `close`; `setBindings` shared by graphics and compute; `update*Dynamic` at every draw.
`commitBindingSets` returns `FE_OUT_OF_MEMORY` when the arena is exhausted (the error names the knob).
Deviation from ADR B.4: versions are written through the CPU heap and copied with `CopyDescriptorsSimple`,
not created directly in the shader-visible range, because `Grow` copies the CPU heap into the new
shader-visible heap and a shader-visible heap cannot be a copy source; every range stays write-once. Other
deviations: `setItem` matches by register class (exact type is the validation layer's); a committed set is
held by the instance even with `trackLiveness == false`; `ShaderTable::bake` writes both tables and rejects
`Dynamic` items in local layouts.

### Step 6: `0c218d9`

As planned. `BindingSetVersion` objects (one transient `VkDescriptorSet`) on `referencedResources`; versions
keyed by (set, committed mutable generation); dynamic copies keyed by (set, source set, dynamic buffer
list); `m_CommittedBindingSets` and `m_DynamicBindingSetCopies` cleared at `open` and `close`; transient pools
on `TrackedCommandBuffer`, reset on recycle; `update*Bindings` at every draw and dispatch;
`requireBindingSetStates` over desc and layout. `createBindingLayout` refuses a layout over the device's
dynamic-descriptor limits. Deviations: layout items are matched by register class, slot and array element
(exact-type matching would have broken existing sets); `setItem` with a null resource is `FE_INVALID_ARGS`;
for a regular buffer in a Dynamic item the descriptor is written at offset 0 with the range size and
`range.byteOffset` is the dynamic offset; `Transition` re-requires a committed set's states when
`m_BindingStatesDirty` or the set has UAVs; the backend also reports an uncommitted mutable write, an
incomplete set and an unset dynamic item at the bind (builds without the validation layer get a message
instead of undefined behaviour). Two pre-existing bugs fixed: `setMeshletState` compared against the compute
state; the ray-tracing draw-time rebind bound the compute bindings.

### Step 7 (D3D11, Windows): `610c2f3`

`setItem`/`setItems` rebuild the per-stage arrays (`rebuildArrays`, shared with `createBindingSet`);
`commitBindingSets` is the state call; `m_CurrentBindingGenerations` parallels `m_CurrentBindings` and every
`set*State`, draw and dispatch re-issues a bound set whose generation changed. Deviation: on D3D11 a mutable
write also reaches the next draw without a re-bind (there are no versions; the validation layer reports the
misuse); a null resource in `setItem` means "nothing bound".

### Step 8: `6408f71`

The B.5.6 table, with one structural deviation: the validation layer does not wrap `IBindingSet`, so
`setItem` cannot be intercepted (the backends return the documented codes) and a mutable write is told from a
dynamic one by comparing the set's current mutable items with a snapshot taken at `commitBindingSets`
(`CommittedBindingSet` records per command list, cleared at `open`). `Verify` uses the command list's state
queries and skips untracked (`Unknown`) resources, volatile and CPU-accessible buffers and acceleration
structures. No volatile-write tracking existed in the layer before; `m_VolatileBuffersWritten` was added.
Fixed in passing: `setRayTracingState` set `m_MeshletStateSet` and lacked the layout-match check.

### Step 9: `3245190` (Linux items), `5d062ce` (validation cases), `56e7d0f`/`9d40474` (items 5-6, Windows)

`nvrhi_test_binding_frequencies` (gtest) on a headless Vulkan 1.3 device (`tests/vulkan-test-device.{h,cpp}`),
under nvrhi's validation layer and, when `VK_LAYER_PATH` is set, the Khronos layer; compute shaders in
`tests/shaders/` compiled offline with dxc (command in each `.spv.h`). Items 1, 2 (offset, buffer object,
volatile), 3 (`None`/`Transition` through `getBufferState`), 4, 7 and the validation-layer cases
(13 cases in `5d062ce`: every row of the B.5.6 table that a Vulkan device can reach, including the
version-count warning at 4097 commits and `Verify` at the commit and at the dispatch; `setRayTracingState` is not
covered, it needs a ray-tracing pipeline). Items 5 and 6 are in `tests/d3d12-descriptor-heap.cpp` (compile-checked only; D3D12 test
executables cannot link under MinGW for a pre-existing reason).

### Step 10: donut `b6c333a` (19 files), samples `d4c5d5d` (36 files)

The sweep as planned (19 donut files, not 20; 36 samples files, not 41: the others only used
`utils::CreateVolatileConstantBufferDesc`, which is not deprecated). `vxgi_samples/VoxelRenderer.cpp`'s
`BindingSetFactory` skips `PushConstants` and `Dynamic` layout items instead of synthesising a set item.
Found on the GPU: `bindless_rendering` derived its layout with `utils::CreateBindingSetAndLayout` and lost its
push-constant range when the set item went; it now declares its layout explicitly. Deprecation warnings from
these names: 56 before, 0 after. The FrameGraph-consumer paragraph (TAA and SSAO adapters, `-stats`) does
not apply on this base (section 6). The Windows-only files (`benchmark/Asteroids/nvrhi/renderer.cpp`,
`demos/nv_asteroids/src/**`, `work_graphs_d3d12.cpp`, `aftermath.cpp`) were edited textually and not compiled.

### Step 11: `98096b5`

As planned: `ResourceType::VolatileConstantBuffer` (the values after it shift by one; nothing indexes or
serialises by value), `BindingLayoutItem::VolatileConstantBuffer`, `BindingSetItem::PushConstants`,
`c_MaxVolatileConstantBuffers*`, every set-side normalisation and the validation layer's once-per-device
warning are gone; `c_HeaderVersion` is 30; `tools/nvrhi.natvis` updated. `utils::CreateBindingSetAndLayout`
can no longer derive a `PushConstants` layout item (declare the layout explicitly; `utils.h` says so); it
still derives `Dynamic` for a `ConstantBuffer` item whose buffer is volatile. One normalisation remains in
every `createBindingLayout`: a `PushConstants` layout item is stored as `Dynamic` whatever frequency the
desc carries, so hand-built items behave like the initializer's. Stale public-header comments found while
writing the guide were fixed in the same commit (`c_MaxDynamicConstantBuffers` is a sizing figure, nothing
enforces it; the full `FE_INVALID_ARGS` list on `setItem`; `DescriptorPoolSizes`; `Transition` re-applied at
each draw for dynamic items).

### Step 12: `8462841`, and the commit that adds this record

`doc/ProgrammingGuide.md` section "Binding Layouts and Sets" (frequencies and the admissibility table, creating sets, the
write-commit-bind rule, the next-draw rule, transition modes, the validation layer's rules, the `DeviceDesc`
knobs and their messages, usage guidance, a migration note, two worked snippets) and `doc/Tutorial.md`; ADR 0008 to Accepted; this
record.

## 6. Known issues and follow-ups

- **Windows host.** Everything under D3D11 and D3D12 is MinGW compile-checked only. To run there: Debug and
  Release builds; `nvrhi_d3d12_descriptor_heap` (growth while in flight, sampler budget, range API), optionally
  against `56e7d0f^` to see whether the pre-fix fault reproduces; `nvrhi_memory_queries` and `nvrhi_qi_device`
  on d3d11 and d3d12; the samples under the validation layer on D3D11 and D3D12 (`deferred_shading`,
  the ray-tracing samples: the SBT path now writes both table frequencies); RenderDoc per section 4 (root
  signatures differ only by the frequency split; committed sets' tables point into arena chunks and are never
  rewritten); the heap report before and after step 10; the D3D11 checklist of step 7 (dynamic CB and SRV
  between draws, mutable re-bind, incomplete set, grown slot range unbinding, PS-UAV path, `setItem` error
  paths). Watch for debug-layer errors on `SetGraphicsRootShaderResourceView` with typed buffers in Dynamic
  slots and for the 256-sampler transient cap in sampler-heavy scenes.
- **FrameGraph consumers and the `-stats` / screenshot A/B.** Not possible on this base; to be done when the
  binding-frequency branches and the `framegraph` branches are merged: `TemporalAntiAliasingPass` and
  `SsaoPass` adapters hold one set per layout, `setItem` the rotated inputs, `commitBindingSets(state.bindings,
  Transition)`; `-stats` before and after; `ab_compare.py` screenshots.
- Root UAV dynamic items (ADR follow-up).
- `utils::CreateBindingSetAndLayout` cannot express push constants any more (declare the layout); a
  `createBindingSet` overload copying another set's static items remains the remedy if many sets of one
  layout turn out to be a cost.
- `utils::CreateVolatileConstantBufferDesc` keeps its name (not deprecated; ~20 call sites).
- D3D12: after a `Grow`, a recording command list keeps the old shader-visible heap bound until its next
  `set*State`; a `writeDescriptorTable` in that window is visible only in the new heap. Fixed-size heaps would
  remove this; not a lifetime issue.
- D3D12: shrinking a descriptor table keeps its range; growing one keeps the pre-existing
  allocate-copy-release hazard for in-flight lists.
- Vulkan: `createBindingLayout` refuses layouts over `maxDescriptorSetUniformBuffersDynamic` /
  `maxDescriptorSetStorageBuffersDynamic` (device dependent).
- Validation: `Verify` skips acceleration structures (no public state query); `setItem` errors are
  backend-reported only; the version count is approximate at a set's first commit.
- Samples' own shaders: `rt_shadows`, `async_compute`, `rt_particles` declare `Rgba32f` storage images over
  RGBA16F / RGBA8 views (`Undefined-Value-StorageImage-FormatMismatch-ImageView`); pre-existing, unrelated.
- Host notes (Linux): on this base the samples are built without `_DEBUG`, so the Khronos layer must be
  forced with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` (and nvrhi's layer with
  `-DCMAKE_CXX_FLAGS_DEBUG="-g -D_DEBUG"`); donut renders only while its window is focused; its stdout is
  block-buffered when redirected.
