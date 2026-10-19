# Immutable binding sets and per-command-list descriptor arenas

- Status: Abandoned
- Approved: 2026-10-15
- Done: 2026-10-15
- Abandoned: 2026-10-16. [ADR 0009](../adr/0009-persistent-binding-sets-and-descriptor-range-allocation.md) superseded
  ADR 0008 after the measurements below. The `binding-freq` branches were rebuilt from their branch points by
  [2026-10-16-persistent-binding-sets.md](2026-10-16-persistent-binding-sets.md); the commits recorded below survive
  under the tag `binding-freq-rev2`.
- Decisions: [ADR 0008](../adr/0008-binding-frequencies-and-descriptor-allocation.md), revision 2
- Replaces: [2026-10-13-binding-frequencies.md](2026-10-13-binding-frequencies.md) (abandoned)

This plan carries out revision 2 of ADR 0008. It rebuilds the `binding-freq` branches from the commits they
branched from, re-applying the parts of revision 1 that revision 2 keeps (ADR 0008, B.8). Sections 1-4 are the
plan for review; sections 5-6 are filled in as the work is done. Where the plan and the code differ, the code is
authoritative and the difference is recorded under the step.

## 1. Context

Revision 2 of ADR 0008 decides:

1. **B.1** nvrhi carries no binding frequency. The binding API is `ethereal-dev`'s, including
   `VolatileConstantBuffer` and `PushConstants` in their `ethereal-dev` form. `c_HeaderVersion` is 29, one
   above `ethereal-dev`'s 28.
2. **B.2, B.3** A binding layout is the backend segment; a binding set is immutable after `createBindingSet`
   (`getDesc`, `getLayout`; no `setItem`, `setItems`, `getGeneration`).
3. **B.4, B.5** Device-owned storage. D3D12: non-shader-visible CPU heaps for sets, view caches and descriptor
   table staging; separate shader-visible heaps for descriptor tables and arena chunks; no mirror between them.
   Vulkan, descriptor sets: one `DescriptorSetAllocator`. Vulkan, descriptor buffers: a CPU descriptor heap and
   one resource and one sampler descriptor buffer. Heaps and buffers that grow are parked until in-flight work
   completes.
4. **B.6** Binding at `set*State`. D3D12 and the Vulkan descriptor-buffer path copy each newly bound set into
   the command list's arena, with a per-command-list bind cache; the Vulkan descriptor-set path binds the set's
   own persistent `VkDescriptorSet`. The sampler heap is kept in budget by a per-command-list cap, the bind
   cache and the guidance to put samplers in one per-frame set; no immutable samplers.
5. **B.7** Validation rules are `ethereal-dev`'s; the new errors are arena exhaustion and the sampler cap.
6. **B.8** Revision 1's history is rewritten: tagged, reset to the branch points, rebuilt.

Repositories and branches:

| Repository | Path in the aggregate | Branch | Revision 1 tip (tagged `binding-freq-rev1`) | Branch point (reset target) |
| --- | --- | --- | --- | --- |
| nvrhi | `ethereal-nvrhi` | `binding-freq` | `e0df03b` | `296422b` (`ethereal-dev`) |
| donut (fork) | `ethereal-donut` | `binding-freq` | `b6c333a` | `9f51e7a` (`ethereal-dev`) |
| samples | `ethereal-samples` | `binding-freq` | `d4c5d5d` | `54f0325` (`main`) |
| aggregate root | `.` | `binding-freq` | `2d7156e` | `e02979c` (`main`) |

donut and samples are checked out detached at their `origin/binding-freq` tips; their local `binding-freq`
branches are created at step 0.

Host. Windows only: D3D11, D3D12 and Vulkan are built and run through the root `build.ps1` from a Visual Studio
developer shell. Revision 1's D3D11 and D3D12 code was never run; nothing in this plan relies on it having been.

## 2. Decisions taken at approval

- **History.** Each `binding-freq` branch is tagged `binding-freq-rev1`, reset to its branch point and rebuilt;
  the rebuilt branches are force-pushed (`--force-with-lease`) and the tags pushed, so revision 1's commits stay
  reachable for the hashes quoted in ADR 0008 B.8 and in the abandoned plan.
- **Re-applied from revision 1:** `049a1c7` (OffsetAllocator), `7658254` (Vulkan pool manager, without its
  transient pools), `56e7d0f` (D3D12 OffsetAllocator ranges, parked heaps), the arena of `9d40474`, and the
  headless Vulkan test device of `3245190`. Nothing else.
- **Header version.** Counted from `ethereal-dev`: 28 to 29, once, in step 1. Every public-header change of this
  plan lands in that commit; the new `DeviceDesc` fields are inert until the step that uses them.
- **Vulkan descriptor buffers** are opt-in and off by default. Step 9 measures them; turning them on by default
  is a follow-up whatever the result.
- **Samplers:** per-command-list cap, bind cache, per-frame sampler set guidance. No immutable samplers, no
  content-keyed sampler deduplication.
- **D3D11** is unchanged: after the reset it is `ethereal-dev`'s code.
- **Windows only.** No Linux build or run is part of this plan.

## 3. Plan

Each step is one or more commits that build on their own. The aggregate is bumped after steps 1, 4, 6 and 10.

### Step 0: Tag, reset, record (all repositories)

1. In each repository, tag the revision 1 tip `binding-freq-rev1` and push the tag.
2. Reset `binding-freq` to the branch point in the table of section 1 (donut and samples: create the local
   branch there). After the reset donut `binding-freq` equals donut `ethereal-dev` and samples `binding-freq`
   equals samples `main`.
3. nvrhi, first commit on the rebuilt branch: ADR 0008 revision 2, `doc/adr/README.md`, this plan,
   `doc/plans/README.md`, and the revision 1 plan as an abandoned record (its status line says why and where its
   commits are).
4. Aggregate, first commit on the rebuilt branch: `.gitmodules` names `binding-freq` for the three submodules,
   and the submodule pointers are the reset tips.
5. Force-push the four branches. This plan goes to Approved in the commit of item 3, before the push.

### Step 1: Re-apply the kept work and the header (nvrhi)

In this order, each a commit that builds:

1. `git cherry-pick 049a1c7`: OffsetAllocator and its upstream tests. No public header.
2. The header commit, every public change of this plan, `c_HeaderVersion` 28 to 29:
   - `include/nvrhi/d3d12.h` `DeviceDesc`: `maxDescriptorAllocations` (16384); `shaderResourceViewHeapSize` and
     `samplerHeapSize` become the CPU heap sizes; new `shaderVisibleShaderResourceViewHeapSize` (16384) and
     `shaderVisibleSamplerHeapSize` (1024, grows to 2048); `dynamicDescriptorChunkSize` (1024),
     `dynamicSamplerChunkSize` (64), `maxDynamicSamplerDescriptors` (256).
   - `include/nvrhi/vulkan.h` `DeviceDesc`: the persistent pool fields of `7658254` (`descriptorPoolSets`,
     `DescriptorPoolSizes descriptorPoolSizes`), not its transient ones; new `descriptorBufferEnabled` (false;
     set when the application enabled `VK_EXT_descriptor_buffer` with its `descriptorBuffer` feature, as
     `bufferDeviceAddressSupported` is), `descriptorBufferResourceCapacity` (16384 descriptors),
     `descriptorBufferSamplerCapacity` (1024), `descriptorBufferChunkSize` (1024),
     `descriptorBufferSamplerChunkSize` (64).
   - `include/nvrhi/nvrhi.h`: `c_HeaderVersion` 29 and nothing else.
   - `tests/abi_lint.py` and `tests/header_hygiene.cmake` pass.
3. `git cherry-pick 7658254`, resolved against the header commit: its `vulkan.h` hunk is already in; the
   transient pools (`acquireTransientPool`, `releaseTransientPool`) are left out; `DescriptorPoolManager` is
   renamed `DescriptorSetAllocator` (ADR 0008 B.4). With this commit the Vulkan descriptor-set path of B.3 and B.6
   is complete.
4. `git cherry-pick 56e7d0f`. It was written on top of revision 1's header and binding code; conflicts in
   `d3d12-resource-bindings.cpp` and `d3d12-device.cpp` are resolved to `ethereal-dev`'s binding code with the
   commit's range and parking changes. Its test file `tests/d3d12-descriptor-heap.cpp` comes with it.
5. The headless Vulkan test device from the tag: `tests/vulkan-test-device.{h,cpp}` and its `tests/CMakeLists.txt`
   entries, with no test using it yet (step 8).

### Step 2: D3D12 arena and copy at `set*State`, on the mirrored heaps (nvrhi)

This step changes the bind path while the CPU heap and the shader-visible heap are still mirrored, so the copy
source (the set's CPU range) already exists and the arena allocates from the shared allocator, as revision 1 did.

1. `TransientDescriptorArena` and `DescriptorChunk` from `9d40474` (through the tag) into
   `src/d3d12/d3d12-backend.h` and `d3d12-commandlist.cpp`: one arena per shader-visible heap type, chunks of
   `max(chunkSize, count)` on `referencedResources`, the arena abandons its chunk when the heap pointer changes.
2. The sampler cap: the sampler arena refuses to hold more than `maxDynamicSamplerDescriptors` per command list.
   Arena exhaustion and the cap are errors through the message callback naming the `DeviceDesc` field; the bind
   is skipped.
3. `setGraphicsBindings` and `setComputeBindings` (`d3d12-resource-bindings.cpp`), used by `setGraphicsState`,
   `setComputeState`, `setMeshletState` and `setRayTracingState` (`d3d12-graphics.cpp`, `d3d12-compute.cpp`,
   `d3d12-meshlets.cpp`, `d3d12-raytracing.cpp`): for the sets selected by `bindingUpdateMask`, one arena range
   per heap type, one `CopyDescriptors` per heap type with one source range per set, root tables at their offsets.
   Volatile root CBVs and root constants as on `ethereal-dev`. Descriptor tables bound from their persistent range.
4. The bind cache: a `CommandList` map from `BindingSet*` to its arena handles, cleared at `open()` and when the
   arena's heap changes.
5. `ShaderTable::bake` (`d3d12-raytracing.cpp`) copies local binding sets into the arena through the bind cache;
   `ShaderTableState` records the heap it was baked against and rebakes when it changes.

### Step 3: D3D12 storage split (nvrhi)

1. `StaticDescriptorHeap` (`d3d12-descriptor-heap.cpp`, `d3d12-backend.h`) splits into a CPU heap (OffsetAllocator,
   grows by copy, replaying live ranges) and a shader-visible heap (OffsetAllocator, grows and parks). The device
   owns CPU CBV/SRV/UAV, CPU sampler, RTV, DSV, shader-visible CBV/SRV/UAV and shader-visible sampler heaps, sized
   from the step 1 fields.
2. `BindingSet::createDescriptors` writes the CPU heap only; `copyToShaderVisibleHeap` is no longer called for
   sets. `~BindingSet` releases its CPU ranges.
3. `DescriptorTable` holds a CPU staging range and a shader-visible range. `writeDescriptorTable` writes the
   staging range and copies the written descriptors; `resizeDescriptorTable` copies from the staging range.
   `getFirstDescriptorIndexInHeap` returns the shader-visible index, as `ResourceDescriptorHeap` users expect.
4. Shader-visible `Grow` (B.5): allocate the new heap, copy every descriptor table's staging range into its range in
   the new heap, park the old heap. Arena chunks are not copied.
5. `getNativeObject` on a binding set returns null for the shader-visible handle types.
6. The heap report at device destruction covers the six heaps and, for the shader-visible sampler heap, the peak
   sampler descriptors held by one command list.

### Step 4: Vulkan descriptor-buffer infrastructure (nvrhi, donut)

1. `vulkan-device.cpp`: when `descriptorBufferEnabled`, load the `VK_EXT_descriptor_buffer` entry points, read
   `VkPhysicalDeviceDescriptorBufferPropertiesEXT`, and refuse the flag (error, path off) without
   `bufferDeviceAddressSupported`.
2. Device storage (`vulkan-backend.h`, new `vulkan-descriptor-buffer.cpp`): the CPU descriptor heap (host pages, an
   OffsetAllocator each, in units of `descriptorBufferOffsetAlignment`), and the resource and sampler descriptor
   buffers (host-visible, device-addressable, `RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT` and
   `SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT` usage, OffsetAllocator, grow and park as on D3D12).
3. `createBindingLayout` and `createBindlessLayout`: `DESCRIPTOR_BUFFER_BIT_EXT` on the set layout; a volatile
   constant buffer item becomes `UNIFORM_BUFFER`; the layout stores `vkGetDescriptorSetLayoutSizeEXT` and each
   binding's `vkGetDescriptorSetLayoutBindingOffsetEXT`.
4. Every pipeline (`vulkan-graphics.cpp`, `vulkan-compute.cpp`, `vulkan-meshlets.cpp`, `vulkan-raytracing.cpp`) is
   created with `VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT` on this path.
5. donut `DeviceManager_VK`: a `DeviceCreationParameters` field, off by default, that enables the extension and its
   feature when the device supports it and sets `descriptorBufferEnabled`; the samples' common command line gains a
   switch for it. The exact names are recorded in section 5.

### Step 5: Vulkan descriptor-buffer binding (nvrhi)

1. `createBindingSet`: a CPU heap segment of the layout's size, descriptors written with `vkGetDescriptorEXT` at
   their binding offsets; volatile constant buffer slots left empty; no `VkDescriptorSet`.
2. `TrackedCommandBuffer` (`vulkan-backend.h`, `vulkan-commandlist.cpp`, `vulkan-queue.cpp`): resource and sampler
   arenas of chunks from the device buffers, released when the instance completes; the sampler cap of step 2.
3. `bindBindingSets` and the four `set*State`: `vkCmdBindDescriptorBuffersEXT` once per command buffer and after a
   grow; per newly bound set an arena range, a `memcpy` of the segment, the volatile constant buffer descriptors
   written from the current version's address, `vkCmdSetDescriptorBufferOffsetsEXT`. Descriptor tables at their
   persistent offsets.
4. Volatile rewrites: a `writeBuffer` to a volatile buffer bound in a set marks that set for a new copy at the next
   draw or dispatch. The bind cache is keyed by the set and the volatile versions in its copy.
5. Descriptor tables: a staging range in the CPU heap and a persistent range in the descriptor buffer;
   `writeDescriptorTable` and `resizeDescriptorTable` as on D3D12; mutable-descriptor-type bindless layouts use the
   largest descriptor size of their types.
6. State tracking unchanged.

### Step 6: Validation layer (nvrhi)

`src/validation` is `ethereal-dev`'s after the reset. This step checks that its rules hold on the new D3D12 bind
path and on both Vulkan paths, and adds nothing else. Arena exhaustion and the sampler cap are reported by the
backends (steps 2 and 5), so they also reach applications built without the validation layer.

### Step 7: Tests (nvrhi)

1. D3D12 (`tests/d3d12-descriptor-heap.cpp`): copy at `set*State` and read back; A, B, A rebinding copies A once
   (arena peak); arena exhaustion and the sampler cap name their knobs; shader-visible `Grow` while a command list
   that bound a descriptor table is in flight, with the table's contents intact in the new heap; the ray-tracing
   shader-table bake if the device supports ray tracing.
2. Vulkan, descriptor sets (`tests/vulkan-test-device.{h,cpp}`): 1000 sets created and destroyed across pools while
   command lists that bound some of them are in flight; no validation messages.
3. Vulkan, descriptor buffers: the D3D12 cases of item 1 on a test device created with the extension, plus a
   volatile constant buffer rewritten between two dispatches with both results read back.
4. Every existing test passes on D3D11, D3D12 and Vulkan.

### Step 8: Documentation (nvrhi)

`doc/ProgrammingGuide.md`, section "Binding Layouts and Sets": immutable sets; expressing frequency with layouts,
volatile constant buffers, push constants and bindless tables; samplers in one per-frame set; the arena and its
`DeviceDesc` knobs and messages; the Vulkan descriptor-buffer switch. `doc/Tutorial.md` where it shows sets.

### Step 9: Measurement (no code)

On the nv_asteroids rig (section 4): D3D12 at the branch point against step 3, for the per-bind copy cost; Vulkan
descriptor sets against descriptor buffers. The result is recorded; descriptor buffers stay off by default.

### Step 10: Close (nvrhi, aggregate)

ADR 0008 to Accepted; this plan to Done with sections 5-6; the aggregate bumps the three submodules.

## 4. Verification

All on the Windows host. Per step, before the commit:

- Full configure and build of the aggregate in Release and Debug (`build.ps1`, all sample, benchmark, GVDB,
  nv_asteroids and unit-test options on); `tests/abi_lint.py` and the header hygiene check pass; `ctest` passes per
  configuration.
- Debug samples under the validation layers with zero new messages: `deferred_shading`, `bindless_rendering`,
  `rt_shadows`, `async_compute`, `meshlets` on D3D11, D3D12 and Vulkan; from step 4 also Vulkan with the
  descriptor-buffer switch. On the discrete adapter, one GPU run at a time.

At the end:

- **Heap report.** Peak shader-visible sampler and CBV/SRV/UAV usage, and peak sampler descriptors per command
  list, for `deferred_shading` and nv_asteroids on D3D12. Expected: the sampler peak per command list well under
  256. A sample that reaches the cap is a donut or sample layout to restructure under the per-frame sampler
  guidance, recorded in section 6.
- **nv_asteroids** (step 9), on the NVIDIA adapter with the rig's protocol (one run at a time, runs spaced, hot runs
  discarded): frame time on D3D12 before and after, and on both Vulkan paths; `-benchmark` for the deterministic
  replay; screenshots compared with the rig's PSNR tool.
- **RenderDoc.** One frame of `deferred_shading` on D3D12: root tables point into arena chunks, a set's tables
  identical in content to its CPU range. On Vulkan descriptor sets: one persistent set per binding set. On Vulkan
  descriptor buffers: two descriptor buffers bound per command buffer.

## 5. Execution record

Work on the Windows host (Visual Studio 2022, Ninja Multi-Config, NVIDIA RTX 4050 laptop GPU next to an AMD
Radeon 780M). Verification common to every step below: the aggregate's existing CMake cache is reconfigured with a
plain `cmake -S . -B build` rather than `build.ps1`, because the `default` preset turns nv_asteroids off; sample
output is `OutputDebugString` only, so it is captured (DBWIN, filtered by the sample's process id) for 12-15 s per
run. The samples enable the debug runtime (D3D12 debug layer, Khronos validation layer) and nvrhi's validation layer
only in Debug builds, so the validation runs below use Debug builds; a Release run shows only that a sample runs and
logs nothing.

| Step | Repository | Commits |
| --- | --- | --- |
| 0 | nvrhi, aggregate | nvrhi `5564af8`; aggregate `ae9f353`; tags `binding-freq-rev1` |
| 1 | nvrhi, aggregate | nvrhi `06e8b57`, `f063c06`, `a8cbc41`, `0f4231c`; aggregate `ba8222f` |
| 2 | nvrhi | `da20943` |
| 3 | nvrhi | `e120916`, and the fix `abcc6da` found at step 4 |
| 4, 5 | nvrhi, donut, aggregate | nvrhi `d3d97f5`; donut `850a803`; aggregate `f2523e3` (steps 2-5) |
| 6 | none | no code |
| 7 | nvrhi | `7b65f1e` |
| 8 | nvrhi | `e46c9b2` |
| 9 | nvrhi | `5d44d6f`, `cbc4160` (fixes the measurement found) |
| 10 | nvrhi, aggregate | the commit that adds this record; the aggregate bump after it |

### Step 0

The four revision 1 tips were tagged `binding-freq-rev1` (annotated) and the tags pushed: nvrhi `e0df03b`, donut
`b6c333a`, samples `d4c5d5d`, aggregate `2d7156e`. The branches were reset to `296422b`, `9f51e7a`, `54f0325` and
`e02979c`; donut and samples got local `binding-freq` branches there, tracking `origin/binding-freq`. nvrhi `5564af8`
adds ADR 0008 revision 2, this plan (Approved), the revision 1 plan as an abandoned record and the two READMEs, and
removes `doc/descriptor-allocation-design.md`, whose survey is the ADR's context (revision 1 removed it the same
way). Aggregate `ae9f353`: `.gitmodules` names `binding-freq` for the three submodules; the donut and samples pins of
`e02979c` were already the reset tips. The four branches were force-pushed with a lease on the revision 1 tips.

### Step 1

- `06e8b57`: `049a1c7` cherry-picked unchanged.
- `f063c06`: the header commit as planned; `c_HeaderVersion` 29.
- `a8cbc41`: `7658254` with its `vulkan.h` hunk dropped (already in `f063c06`), the transient pools removed and the
  class renamed `DescriptorSetAllocator` (`VulkanContext::descriptorSetAllocator`).
- `0f4231c`: `56e7d0f` applied without conflicts (the expected conflicts with revision 1's header did not occur:
  the hunks it touches are the same on `ethereal-dev`). Its test had never run: `tests/d3d12-descriptor-heap.cpp`
  passed `&commandList` to `executeCommandLists`, which releases the `AutoPtr` and hands over a null list (access
  violation). Fixed to `GetAddressOf()`; stdout is unbuffered so a crash keeps the progress lines. Both cases pass
  with the D3D12 debug layer clean; the parked-heap growth test grows the SRV heap from 64 to 1024 descriptors
  under an executing list.
- Deviation: item 5 (the headless Vulkan test device) moves to step 7. With no test using it, the files would not
  be compiled, so nothing about them could be verified here.

Verification: full Release build of the aggregate (two reruns for `z-applocal` "file in use" link flakes, no
compile errors); `ctest` 28 of 28 (the 27 of the branch point plus `nvrhi_d3d12_descriptor_heap`); `deferred_shading`
on D3D11, D3D12 and Vulkan, `bindless_rendering`, `rt_shadows`, `async_compute` and `meshlets` on D3D12 and Vulkan
ran 12 s each in Release with no error or warning lines. Those were Release builds, without the validation layers;
the validation runs for steps 1-4 were made in Debug after step 4 (see there).

### Step 2: `da20943`

As planned, with the arena of `9d40474` (`DescriptorChunk`, the arena per heap type, the sampler cap) and its error
messages reworded. The copy at `set*State` writes straight into the shader-visible heap through the chunk's handles;
the copy source is the set's CPU range. Deviations:

- A binding set in the bind cache is held by the command list's instance whatever its `trackLiveness`: the cache is
  keyed by the set's address, which must not be reused while the list records.
- Ray tracing: a shader table whose pipeline has local bindings is baked per command list even when it is cached
  (`ShaderTableState::bakedRecordingVersion`), because a cache buffer shared by command lists cannot hold records that
  point into one command list's arena. ADR 0008 B.6 was amended in the same commit.

`deferred_shading`, `bindless_rendering`, `rt_shadows`, `rt_reflections` (local bindings), `rt_bindless`,
`async_compute`, `meshlets`, `threaded_rendering`, `variable_shading` ran on D3D12 in Release without messages.

### Step 3: `e120916`

Deviation from items 1, 3 and 4: the shader-visible heaps keep the CPU staging heap at the same indices that they had on
`ethereal-dev`, and binding sets move to two new non-shader-visible heaps (`cpuShaderResourceViewHeap`,
`cpuSamplerHeap`). A UAV clear (`ClearUnorderedAccessView`) needs a CPU and a GPU handle to the same descriptor, and
the native GPU handle queries of textures need a persistent shader-visible descriptor; the staging heap at the same
indices serves both, and `Grow` copying it into the new heap carries descriptor tables over without a per-table replay.
What the ADR wants is unchanged: a binding set takes no shader-visible descriptor. ADR 0008 B.4 / B.5 were rewritten to
match. Also: a sampler descriptor table was bound with an SRV heap handle (pre-existing); it is bound with a handle of
its own heap now. The descriptor-heap test grows the heap through the range API instead of through binding sets, which
no longer touch it.

### Steps 4 and 5: nvrhi `d3d97f5`, donut `850a803`

Landed together: the infrastructure cannot be exercised without the binding path. Deviations, all recorded in ADR 0008
B.4 to B.6 in the same commit:

- One buffer kind instead of one resource and one sampler descriptor buffer: an nvrhi set layout mixes samplers and
  resources, and a set is one segment at one offset of one buffer, so the buffer carries both usages. Both GPUs of this
  host allow it with unlimited ranges (`vulkaninfo`: RTX 4050 descriptors of 4-16 bytes, Radeon 780M of 16-48 bytes).
- A page list instead of a buffer that grows and is parked: the device holds up to `maxDescriptorBufferBindings` pages
  (32 on both GPUs), a command buffer binds them all, and a set's page is its buffer index. Pages never move, so there
  is no CPU shadow, no copy and no parking. The `DeviceDesc` fields became `descriptorBufferPageSize` and
  `descriptorBufferChunkSize` (bytes); the header stays 29, it has not been released.
- The arena state lives in the command list and its chunks on the command buffer's `referencedResources`, as on D3D12,
  rather than in `TrackedCommandBuffer`.
- The donut switch is `DeviceCreationParameters::enableVulkanDescriptorBuffer` plus the environment variable
  `DONUT_VULKAN_DESCRIPTOR_BUFFER=1`, which reaches every sample without a command-line change in each.

Found at this step, in step 2's code (`abcc6da`): with several threads recording, a command list could take an arena
chunk in a shader-visible heap that another thread had just grown, and bind a table of it while the old heap was still
bound. The D3D12 debug layer reported it in `threaded_rendering` (`SET_DESCRIPTOR_TABLE_INVALID`). The chunk now
records its heap and start handles under the heap's lock and holds the heap; a chunk outside the bound heap makes the
command list bind the new heaps and copy every set of the state again.

Verification, Debug builds with the D3D12 debug layer, the Khronos validation layer and nvrhi's validation layer:
`deferred_shading` on D3D11, D3D12, Vulkan and Vulkan with descriptor buffers; `bindless_rendering`, `rt_shadows`,
`rt_bindless`, `async_compute`, `meshlets`, `variable_shading` and `threaded_rendering` on D3D12, Vulkan and Vulkan with
descriptor buffers (`rt_reflections` is D3D12 only by design); no error or warning, after the fix. Full Release build
(only HLSL compiler warnings, pre-existing) and `ctest` 28 of 28.

### Step 6

No code. nvrhi's validation layer was on in every Debug run of steps 4, 5 and 7 and reported nothing.

### Step 7: `7b65f1e`

`nvrhi_test_binding_arena` (GoogleTest) instead of extending `tests/d3d12-descriptor-heap.cpp`: one program runs the
cases on D3D12, Vulkan with descriptor sets and Vulkan with descriptor buffers. 12 cases pass, 6 are skipped (the
D3D12-only cases on the Vulkan backends). The D3D12 test device takes the high-performance adapter. Not done: the
ray-tracing shader-table bake on its own (it needs a ray-tracing pipeline and its shaders); `rt_reflections` covers it in
step 4's runs. Found while writing it: the test device must release nvrhi's device before the Vulkan device.

### Step 8: `e46c9b2`

`doc/ProgrammingGuide.md`, section "Binding Layouts and Sets", with two new subsections. `doc/Tutorial.md` says nothing
this revision changes and is unchanged.

### Step 9: measurement, and the fixes it found (`5d44d6f`, `cbc4160`)

nv_asteroids renders on D3D12 only, so the Vulkan comparison uses the Asteroids benchmark of the samples repository
(`asteroids_nvrhi -benchmark -threads 1`, 50,000 asteroids), which has the three patterns that matter: `tex_mut_pc`
(10 sets and push constants), `tex_mut` (10 sets and a volatile constant buffer written per draw) and `mut` (one set per
asteroid and a volatile constant buffer). "Before" is nvrhi at step 1 (`0f4231c`, binding as on `ethereal-dev`) with
`cbc4160` applied, "after" is the branch; both built in Release, run on the RTX 4050 one at a time, 20 s apart, two
repetitions in opposite orders. Median CPU render ms per frame, repetition 1 / repetition 2:

| Mode | D3D12 before | D3D12 after | Vulkan before | Vulkan after (sets) | Vulkan descriptor buffers |
| --- | --- | --- | --- | --- | --- |
| `tex_mut_pc` | 5.79 / 5.58 | 6.88 / 5.90 | 6.23 / 6.44 | 6.28 / 6.33 | 7.75 / 7.46 |
| `tex_mut` | 5.49 / 5.61 | 5.91 / 9.25 | 12.05 / 10.51 | 11.61 / 10.33 | 17.89 / 12.64 |
| `mut` | 9.90 / 12.15 | 51.16 / 29.75 | 16.48 / 16.10 | 23.31 / 15.97 | 25.42 / 24.29 |

The laptop throttles under sustained load; the spread between the repetitions is that. nv_asteroids `-benchmark`
(replay, fixed step) on D3D12: before 11.29 and 11.35 ms per frame, after 11.27 and 11.26 ms.

Read: the Vulkan descriptor-set path costs what `ethereal-dev` cost. On D3D12 the copy at `set*State` costs a few
percent where sets are reused and three to four times where every draw binds a different set (`mut`). Instrumented,
the copy's own instructions are cheap; the D3D12 shader-visible heap is write-combined memory, and the drain of the
copy's writes at the next locked instruction costs about 0.3 us per copy. The Vulkan descriptor-buffer path costs
1.2-1.5x the descriptor-set path. Descriptor buffers stay off by default.

Two fixes came out of the measurement:

- `5d44d6f`: the first descriptor-buffer run cost 50 ms (`tex_mut`) and 110 ms (`mut`). Instrumented, 0.6 us of the
  0.8 us per copy was the write-combine drain on device-local pages mapped through the PCIe BAR. Pages now live in
  cached host memory; the numbers above are after it.
- `cbc4160`: `mut` failed on D3D12 before and after, because the 50,000 sets needed more OffsetAllocator nodes than
  `maxDescriptorAllocations` (16384), a cap revision 1 introduced and `ethereal-dev` did not have. The node budget now
  doubles with the live ranges.

Heap report, nv_asteroids on D3D12 (graceful exit): shader-visible CBV/SRV/UAV peak 7228 of 16384, sampler peak 576
of 1024, peak sampler descriptors held by one command list 128 of the 256 cap; binding-set CPU heaps 879 descriptors
in 441 ranges and 92 sampler descriptors in 72 ranges.

RenderDoc: one D3D12 frame of `deferred_shading` captured; the final pass renders the lit cube through descriptor
tables copied into the arena (pixel values checked). The RenderDoc tools available here do not show root-table GPU
handles, so the "root tables point into arena chunks" item is checked by the tests and the debug layer only. Vulkan
frames were not captured.

### After closing: a microbenchmark of the first bind (2026-10-16, `d727246`)

`tests/bench-set-binding.cpp` (`nvrhi_bench_set_binding`, ctest runs it with `--quick` only) times `setComputeState` +
`dispatch` for 10,000 distinct sets bound once each, and for two sets alternating 10,000 times (bind-cache hits),
without validation or debug layers. "Before" is the same program built against nvrhi at step 1 (`0f4231c`: D3D12 sets
keep persistent shader-visible copies, no descriptor-buffer path), "after" against the branch; four runs of each,
alternating, 15 repetitions per run. Median ns per bind, range over the four runs:

| Device and path | Distinct, before -> after | Alternating, before -> after | Added by the first bind |
| --- | --- | --- | --- |
| D3D12, RTX 4050 | 182-253 -> 489-570 | 168-245 -> 170-245 | about 0.3 us |
| D3D12, Radeon 780M | 101-170 -> 970-1072 | 92-162 -> 102-222 | about 0.85 us |
| Vulkan descriptor sets, RTX 4050 | 764-995 -> 728-933 | unchanged | none |
| Vulkan descriptor sets, Radeon 780M | 127-209 -> 125-128 | unchanged | none |
| Vulkan descriptor buffers, RTX 4050 | (n/a) -> 379-399 | (n/a) -> 258-272 | about 0.12 us over its alternating case |
| Vulkan descriptor buffers, Radeon 780M | (n/a) -> 256-264 | (n/a) -> 134-138 | about 0.12 us over its alternating case |

A bind-cache hit costs what a bind cost before, so the bookkeeping of revision 2 is negligible; the cost is the copy of a
set a command list has not bound yet. It is not cheaper on the integrated GPU: the D3D12 copy costs about 0.85 us on
the Radeon 780M against 0.3 us on the RTX 4050. In five earlier runs of the "after" program alone, two Radeon 780M D3D12
runs measured about 0.1 us instead, stable within the run; the cause was not found. On the RTX 4050 the descriptor-buffer
path binds distinct sets faster than the descriptor-set path; on the Radeon 780M it is twice as slow.

### Step 10

ADR 0008 to Accepted, with the measured costs in its consequences; this plan to Done. Final verification: full Release
build of the aggregate (link reruns for flakes and once for an executable held open by the RenderDoc capture) and
`ctest` 29 of 29, including `nvrhi_test_binding_arena` in Release.

## 6. Known issues and follow-ups

- **D3D12 per-draw sets.** A scene that binds a different binding set for every draw pays one arena copy per draw:
  about 0.3 us on the RTX 4050 and 0.85 us on the Radeon 780M (`nvrhi_bench_set_binding`), 3-4x CPU time in the
  Asteroids `mut` mode. Options if such scenes matter: per-object data through volatile
  constant buffers, push constants or bindless tables (the guidance now), or keeping a persistent shader-visible copy
  for sets that are bound often, which revision 2 rejected for heap pressure.
- Vulkan descriptor buffers stay opt-in: 1.2-1.5x the descriptor-set path on this machine. Not measured on AMD.
- Immutable samplers in `BindingLayoutDesc` (ADR 0008 alternatives) if the sampler cap proves too tight; nv_asteroids
  peaks at 128 of 256.
- Not done in step 7: a test of the D3D12 shader-table bake on its own (needs a ray-tracing pipeline); not done in
  the verification: a Vulkan RenderDoc capture; the full Debug build of the aggregate (Debug was built for the
  sample targets and the tests).
- The validation layers were not run on the AMD GPU (D3D12 and the descriptor-buffer tests take the high-performance
  adapter).
- The donut-side frequency policy, which passes split which layouts, belongs in `ethereal-donut/doc/RdgDesign.md`
  and is not part of this plan beyond what the heap report shows must change.
- Carried over from the revision 1 plan, section 6, and unchanged here: shrinking a D3D12 descriptor table keeps its
  range and growing one keeps the allocate-copy-release hazard for in-flight lists; after a `Grow` a recording D3D12
  command list keeps the old heap bound until its next `set*State`.
