# Persistent binding sets, OffsetAllocator descriptor ranges

- Status: Approved
- Approved: 2026-10-16
- Revised: 2026-10-16, before any push. The first approved version also made the D3D12 descriptor heaps fixed-size;
  that was withdrawn the same day: `ethereal-dev`'s heap growth stays as it is, and its thread-safety hazards are left
  to a separate, systemic solution.
- Decisions: [ADR 0009](../adr/0009-persistent-binding-sets-and-descriptor-range-allocation.md), which supersedes
  [ADR 0008](../adr/0008-binding-frequencies-and-descriptor-allocation.md)
- Replaces: [2026-10-15-immutable-binding-sets.md](2026-10-15-immutable-binding-sets.md) (abandoned)

Sections 1-4 are the plan as approved; sections 5-6 are filled in as the work is done. Where the plan and the code
differ, the code is authoritative and the difference is recorded under the step.

## 1. Context

ADR 0009 returns binding sets to `ethereal-dev`'s model and takes OffsetAllocator for D3D12 descriptor ranges, with
heap growth unchanged. Parts of revision 2 are re-applied:

| From revision 2 (tag `binding-freq-rev2`) | Used here |
| --- | --- |
| `06e8b57` OffsetAllocator with its upstream tests | as is |
| `0f4231c`, `cbc4160` ranges in `StaticDescriptorHeap`, range owners, replay on growth, usage report | without the parking and the node budget |
| `e120916` | only the sampler-table bind fix |

Repositories and branches. The `binding-freq` branches are tagged and rebuilt from their branch points:

| Repository | Branch | Revision 2 tip (tag `binding-freq-rev2`) | Reset target |
| --- | --- | --- | --- |
| nvrhi `ethereal-nvrhi` | `binding-freq` | `0a93e0a` | `296422b` (`ethereal-dev`) |
| donut `ethereal-donut` | `binding-freq` | `850a803` | `9f51e7a` (`ethereal-dev`) |
| samples `ethereal-samples` | `binding-freq` | `54f0325` | `54f0325` (`main`, unchanged) |
| aggregate | `binding-freq` | `7a2b4b6` | `e02979c` (`main`) |

Host: Windows only. Donut needs no change: revision 2's only donut commit was the descriptor-buffer switch.

## 2. Decisions

| # | Question | Decided |
| --- | --- | --- |
| R1 | Vulkan descriptor pools | One pool per set, as `ethereal-dev` (no change to the Vulkan backend) |
| R2 | D3D12 heap growth | As `ethereal-dev`: grow to the next power of two and copy; the live ranges are replayed into the larger allocator. No parking of replaced heaps |
| R3 | Heap sizes and defaults | Unchanged |
| R4 | Allocator nodes | Heap size + 2, recomputed on growth; no `maxDescriptorAllocations` |
| R5 | Header version | Unchanged (28): no public header change |
| R6 | ADR | ADR 0009; ADR 0008 set to "Superseded by 0009" |
| R7 | Fix kept from revision 2 | Sampler descriptor tables bound with a sampler-heap handle |
| R8 | Verification | Builds only in this plan; tests, sample and demo runs, benchmarks and profiling are run separately, later |

## 3. Plan

### Step 0: Tag, reset, record (all repositories)

1. Tag the revision 2 tips `binding-freq-rev2` and push the tags.
2. Reset nvrhi, donut and the aggregate `binding-freq` to the targets above.
3. nvrhi, first commit: ADR 0009; ADR 0008 as at `0a93e0a` with its status set to "Superseded by 0009"; the revision 2
   plan marked abandoned, pointing at the tag; the revision 1 plan as at `0a93e0a`; this plan; both READMEs.
4. Aggregate: `.gitmodules` names `binding-freq` for the three submodules; the nvrhi pin.
5. Force-push nvrhi, donut and the aggregate with a lease on the revision 2 tips, when the user asks for it.

### Step 1: OffsetAllocator (nvrhi)

Cherry-pick `06e8b57`.

### Step 2: Ranges in the descriptor heaps (nvrhi)

1. `StaticDescriptorHeap`: one OffsetAllocator per heap with heap size + 2 nodes, a side table of live ranges by base
   index, `allocateRange` / `releaseRange` for owners, the index-based `IDescriptorHeap` methods on top.
2. `Grow` as on `ethereal-dev` (next power of two, clamped for shader-visible heaps, copy of the CPU heap into both new
   heaps), plus the replay of the live ranges into the larger allocator.
3. Owners keep their `DescriptorRange`: binding sets, descriptor tables (released whole; a shrink keeps the range),
   texture views, buffer and texture clear UAVs (from `0f4231c`).
4. `~Device` reports each heap at Info.

### Step 3: Sampler descriptor tables (nvrhi)

`setGraphicsBindings` and `setComputeBindings` bind a descriptor table with a GPU handle of its own heap.

### Step 4: Documentation (nvrhi)

`doc/ProgrammingGuide.md`: the DX12 descriptor heaps, their growth and the usage report.

### Step 5: Close

The aggregate bump; this plan's sections 5 and 6. ADR 0009 stays Proposed and this plan Approved until the
verification of section 4 has run.

## 4. Verification

In this plan: the Release and Debug builds of the aggregate (nvrhi, donut, the samples and demos, the tests) with no
new warning.

Later, separately (R8):

- `ctest`, including `nvrhi_test_offset_allocator`; revision 2's descriptor-heap test (range API, growth while command
  lists are in flight) adapted to this branch, and a sampler descriptor table case.
- Debug sample runs under the D3D12 debug layer, the Khronos layer and nvrhi's validation layer, with no message:
  `deferred_shading` on D3D11, D3D12 and Vulkan; `bindless_rendering`, `rt_shadows`, `rt_bindless`, `async_compute`,
  `meshlets`, `variable_shading`, `threaded_rendering` on D3D12 and Vulkan; `rt_reflections` on D3D12.
- Asteroids benchmark: D3D12 and Vulkan at revision 2's "before" numbers, D3D12 `mut` at 10-12 ms and running.
- nv_asteroids `-benchmark` on D3D12 and the heap report at exit.

## 5. Execution record

Carried out 2026-10-16. Commits are in nvrhi unless named otherwise.

- **Step 0.** The tag `binding-freq-rev2` was pushed in all four repositories; nvrhi, donut and the aggregate were
  reset to their targets. `c384f3a` records the docs; the aggregate commit has `.gitmodules` and the nvrhi pin.
  Step 0.5 waits for the user, so `origin/binding-freq` still holds revision 2.
- **Step 1.** `dca6ecc`, the cherry-pick of `06e8b57`.
- **Step 2.** `4adce2b`. *Deviation:* `tests/d3d12-descriptor-heap.cpp` and its CMake entry, part of `0f4231c`, were
  left out because tests are deferred (R8).
- **Step 3.** `a194dd2`.
- **Step 4.** `e0165fd`.
- **Step 5.** This record; the aggregate bump.

The first approved version (fixed-size heaps) was implemented and then withdrawn before any push; this branch was
rebuilt from `296422b` for the revised plan.

Section 4, as run: the Release and Debug builds of the aggregate pass with no warning in the D3D12 backend or
OffsetAllocator. Each needed one rerun for vcpkg z-applocal "file in use" link failures, which are not code errors.
Nothing else in section 4 was run.

## 6. Known issues and follow-ups

- Every live set occupies the shader-visible heaps for its whole life, including the 2048-entry sampler heap.
- `ethereal-dev`'s heap-growth hazards remain: readers of the heap pointer and start handles take no lock while `Grow`
  replaces them. A systemic solution is planned separately.
- Open: push the rebuilt branches (step 0.5); the deferred verification of section 4.
