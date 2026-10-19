# 0008. Binding frequencies above nvrhi; immutable binding sets and per-command-list descriptor arenas

- Status: Superseded by [0009](0009-persistent-binding-sets-and-descriptor-range-allocation.md)
- Date: 2026-10-13
- Revised: 2026-10-15. Revision 1, accepted 2026-10-13 and implemented on the `binding-freq` branches
  (header 30), put an update frequency on every binding layout item and made `IBindingSet` writable.
  Revision 2 overturns that part; the table below lists what changed, and revision 1's design is
  recorded under alternatives. The `binding-freq` branches are rebuilt from their branch points, and
  revision 1's commits survive only under the tag `binding-freq-rev1` (B.8).
- Plan: [`../plans/2026-10-15-immutable-binding-sets.md`](../plans/2026-10-15-immutable-binding-sets.md)
  implements revision 2; [`../plans/2026-10-13-binding-frequencies.md`](../plans/2026-10-13-binding-frequencies.md)
  implemented revision 1.
- Scope: nvrhi `ethereal-dev` (surveyed at `8d6fa3d`) and the `binding-freq` branches of nvrhi, donut
  and samples; donut consumers on the `framegraph` branch
- Sources surveyed: slang-rhi at commit `92932ae` (2026-10-12) and Diligent Engine `DiligentCore`
  as checked out next to the aggregate (`074060cfd`, 2026-10-04, for the Vulkan comparison)

## Revision 2 at a glance

| Topic | Revision 1 | Revision 2 |
| --- | --- | --- |
| Who classifies binding frequency | nvrhi: a `BindingFrequency` on each `BindingLayoutItem` | donut and the application, with several binding layouts and binding sets per pipeline, as on `ethereal-dev` |
| `BindingLayoutItem` | Frequency stored in the `unused` byte | `ethereal-dev`'s struct, unchanged |
| `VolatileConstantBuffer`, `PushConstants` | Retired into `ConstantBuffer` + `Dynamic`; aliases removed | Kept in their `ethereal-dev` form; `c_MaxVolatileConstantBuffers*` keep their names |
| `IBindingSet` | Writable: `setItem`, `setItems`, `getGeneration` | Immutable after creation; `ethereal-dev`'s interface |
| `commitBindingSets`, `ResourceStateTransitionMode`, versions, dynamic items | Added | Removed |
| `IBindingLayout` | A frequency-split root segment | The backend segment: D3D12 root parameters and descriptor table ranges; a Vulkan descriptor set layout, with its size and binding offsets on the descriptor-buffer path (B.2) |
| Where a set's descriptors live | D3D12: CPU heap plus a persistent shader-visible mirror; Vulkan: its own pool and `VkDescriptorSet` | D3D12: a segment of a device-owned CPU heap only. Vulkan: one persistent `VkDescriptorSet` from the device's `DescriptorSetAllocator`; with `VK_EXT_descriptor_buffer`, a segment of a device-owned CPU heap, as on D3D12 (B.3, B.4) |
| Command list | Binds persistent sets; copies mutable versions into the arena at commit | D3D12 and the Vulkan descriptor-buffer path: `set*State` copies every newly bound set into the command list's GPU-visible arena and binds the copy. Vulkan otherwise: binds the set's `VkDescriptorSet` directly, as on `ethereal-dev` (B.6) |
| Bindless `IDescriptorTable` | Unchanged | Unchanged on D3D12 and on Vulkan without descriptor buffers; on the descriptor-buffer path a persistent range of the device's descriptor buffer |
| Allocation layer (OffsetAllocator, parked heaps, Vulkan pool manager, arena chunks) | Introduced | Kept, with the roles in B.4 to B.6 |

## Context

This record has two halves that depend on each other. The context is a survey of how slang-rhi uses
Sebastian Aaltonen's OffsetAllocator and how Diligent Engine's `DynamicSuballocationsManager` and
its related allocators manage descriptor heaps and dynamic memory, set against what nvrhi does today.
The decision is the design that follows from it: nvrhi does not classify how often a binding changes;
donut and the application do, with the binding layouts and sets nvrhi already has. Inside nvrhi a binding
layout is the backend segment the shader sees and a binding set is immutable. On D3D12 a set is a group of
CPU descriptors in a device-owned CPU heap, and each command list owns a GPU-visible descriptor arena into
which `set*State` copies the sets it binds. On Vulkan a set is one persistent descriptor set from a
device-level allocator, as Diligent keeps its static and mutable resources; devices with
`VK_EXT_descriptor_buffer` take the D3D12 pattern instead. The allocation layer that supports this is the
survey's result: a coalescing allocator for descriptor ranges, a rule for shader-visible heap lifetime, and a
transient arena per command list with chunks taken from the heap's general allocator. How the work is
carried out is in the plan.

### A.1 The allocators

| Allocator | Where | Algorithm | Cost | Frees |
| --- | --- | --- | --- | --- |
| OffsetAllocator | slang-rhi `src/core/offset-allocator.{h,cpp}` | Two-level power-of-two bins (32 top bins, 8 leaf bins each) over a node list; neighbours merged on free | O(1) allocate and free | Immediate; the caller defers |
| `VariableSizeAllocationsManager` | Diligent `Graphics/GraphicsAccessories/interface` | Best fit: `std::map` of free blocks by offset plus `std::multimap` by size; merge on free; alignment aware | O(log n) | Immediate |
| `VariableSizeGPUAllocationsManager` | same | The above plus a `std::deque` of stale frees tagged with a fence value | O(log n) | Deferred by fence; `ReleaseStaleAllocations(lastCompletedFence)` |
| `DynamicSuballocationsManager` | Diligent `GraphicsEngineD3D12/include/DescriptorHeap.hpp` | Bump pointer inside chunks taken from the dynamic region of the shader-visible heap | O(1), lock-free | Whole chunks, once per frame |
| `StaticDescriptorHeap` | nvrhi `src/d3d12/d3d12-descriptor-heap.cpp` | First-fit scan over `std::vector<bool>` from a cursor, under a mutex; `releaseDescriptors` moves the cursor back to the lowest freed index; grows by power of two when the scan fails | O(heap size) worst case | Immediate; owners defer through `referencedResources` |
| `BitSetAllocator` | nvrhi `src/common/bitset-allocator.h` | Round-robin scan for one index | O(capacity) | Immediate |

**OffsetAllocator as vendored by slang-rhi.** The copy is Aaltonen's code with the namespace changed
to `rhi::` and the asserts changed to `SLANG_RHI_ASSERT`; the 16-bit node mode is off. The tests in
`tests/test-offset-allocator.cpp` are the upstream tests, including the zero-fragmentation case. What
matters when porting it:

- It knows nothing about bytes. Offsets are in whatever unit the caller chooses; slang-rhi divides
  byte sizes by the page's alignment so a 256 MiB page fits 32-bit offsets.
- The node table and free-node list are allocated at construction, sized by `maxAllocs`. A node is
  28 bytes (two `uint32_t`, four `uint32_t` node indices, one `bool`, padded). The default
  `maxAllocs` of 128 K costs about 3.5 MiB plus a 512 KiB free list.
- `Allocation` is `{offset, metadata}`; `metadata` is the node index and must be handed back to
  `free`. slang-rhi carries it in the public `HeapAlloc::nodeIndex`.
- `storageReport()` gives total free space and the largest free region; `storageReportFull()` gives a
  per-bin histogram. Both are cheap.

**Diligent's descriptor allocators.** `DescriptorHeapAllocationManager` wraps one
`ID3D12DescriptorHeap` or a sub-range of one, with a `VariableSizeAllocationsManager` and a mutex.
`CPUDescriptorHeap` is a pool of such managers, each a separate non-shader-visible heap, adding one
when all are full. `GPUDescriptorHeap` is **one** shader-visible heap split into two sub-ranges with
one manager each: the first for static and mutable SRB descriptors, the second for dynamic chunks. It
is sized at device creation and never grows.

`DynamicSuballocationsManager` is per device context and per heap type. `Allocate(count)` bumps inside
the current chunk; when the chunk is exhausted it asks the GPU heap's dynamic manager for a chunk of
`max(chunkSize, count)`, the only point that takes a mutex. `Free` is a no-op.
`ReleaseAllocations(queueMask)` at `FinishFrame` hands every chunk to `GPUDescriptorHeap::Free`, which
pushes a stale-allocation object onto the device's release queue; the chunk returns to the dynamic
manager when the fence passes. The consumer is
`PipelineResourceSignatureD3D12Impl::CommitRootTables`: for each heap type with dynamic root tables it
allocates exactly the dynamic descriptor count, copies the SRB's CPU-side cache in with one
`CopyDescriptorsSimple`, and sets the root tables. Per draw there is no lock and no allocator call
unless a chunk boundary is crossed.

On Vulkan the same split exists: `DescriptorSetAllocator` for static and mutable sets, a pool-of-pools
with `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` so sets are freed individually through the
release queue; `DynamicDescriptorSetAllocator` per context, which takes pools from the global manager,
allocates one set per commit, and at frame end disposes every pool through the release queue, where
`vkResetDescriptorPool` runs once the fence passes.

**slang-rhi's two uses of OffsetAllocator.**

- `Heap::Page` in `src/heap.{h,cpp}` is general GPU memory sub-allocation behind the public `IHeap`.
  A heap is a list of pages; each page is one real allocation (a `VkBuffer` with device memory on
  Vulkan, a `cuMemAlloc` on CUDA) and one OffsetAllocator counting in units of the page's alignment.
  Allocate rounds the size to the alignment, picks a page size (256 MiB for requests up to 64 MiB,
  otherwise the size rounded up to a 256 MiB multiple), scans for a page with that size and alignment
  that accepts `allocate(size / alignment)`, and creates a page if none does, retrying once after
  `removeEmptyPages()` on out-of-memory. Free goes through `retire`, which the backends call only when
  the GPU is done: Vulkan compares the queue's last finished and last submitted IDs and otherwise queues
  a pending free; CUDA retires immediately when the page was never used on a stream, the queue is idle,
  or no cross-stream events are pending, and adds a page cache with per-stream ownership. Only CUDA and
  Vulkan implement `createHeap`; on CUDA every buffer and the constant-buffer pool are sub-allocations
  from two device-wide heaps.
- `src/d3d12/d3d12-descriptor-heap.{h,cpp}` manages descriptor indices. `CPUDescriptorHeap` is a page
  list of non-shader-visible heaps with one allocator per page. `GPUDescriptorHeap` is one
  shader-visible heap with one allocator and a mutex; the destructor asserts everything was returned.
  `GPUDescriptorArena`, one per command buffer and heap type, takes chunks from the GPU heap and
  bump-allocates inside them; `reset()` returns every chunk when the command buffer is recycled. This is
  the `DynamicSuballocationsManager` role with the allocator touched only at chunk granularity.

**nvrhi today.** `StaticDescriptorHeap` is one object per heap type (RTV, DSV, SRV/CBV/UAV, sampler).
It owns a CPU heap and, for the two shader-visible types, a shader-visible heap of the same size that
mirrors it one to one; `copyToShaderVisibleHeap` copies a range after the owner has written it into the
CPU heap. Allocation is the `vector<bool>` scan. When the scan fails, `Grow` creates both heaps at the
next power of two (capped at the D3D12 tier limits for shader-visible heaps), copies the old contents,
and lets the `AutoPtr` assignment release the old heaps. Binding sets allocate their ranges at creation
and release them in their destructor; `CommandListInstance::referencedResources` holds the sets alive
until the instance completes, which is nvrhi's deferred-free mechanism for every GPU object.
`resizeDescriptorTable` is allocate-new, copy, release-old. On Vulkan every binding set creates its own
`VkDescriptorPool` with `maxSets = 1`. `UploadManager` (D3D12 and Vulkan) is nvrhi's dynamic memory
heap: a list of `BufferChunk`s, a bump pointer in the current chunk, and a version stamp per chunk set
to the command-list instance that last wrote it; a chunk is reusable once that instance has completed.

### A.2 The four roles, side by side

| Role | Diligent | slang-rhi | nvrhi |
| --- | --- | --- | --- |
| Persistent shader-visible descriptors (SRBs, binding sets) | First region of one fixed heap, map-based allocator, fence-deferred free | One fixed heap per type, OffsetAllocator, free deferred by the caller | One heap per type mirroring a CPU heap, `vector<bool>` scan, grows by copy, free deferred by holding the set |
| Transient per-draw descriptors | Second region of the same heap; per-context chunk and bump; chunks released per frame via fence | Per-command-buffer arena of chunks from the same heap; reset on recycle | None; volatile CBs are root CBVs, push constants are root constants |
| CPU-only descriptors (RTV, DSV, staging) | Page list of separate heaps | Page list of separate heaps | One heap, grown by copy; safe because nvrhi stores indices and recomputes handles at use |
| Buffer memory | Per-context linear pages from a global dynamic manager, released per frame | `Heap`: 256 MiB pages per alignment class, OffsetAllocator per page, submission-ID deferred free | `UploadManager`: chunk list, bump pointer, whole-chunk reuse by instance version |

The libraries agree on the split. The differences are in the data structure for the persistent role and
in whether a transient role exists at all.

### A.3 What the mechanisms buy

**Coalescing variable-size allocation.** Binding sets have different sizes and bindless tables resize by
allocate-copy-release. A first-fit scan over a bit vector rescans from the lowest freed index after every
release, holds the heap mutex while it does, and shows fragmentation only when `Grow` fires.
OffsetAllocator and Diligent's manager merge neighbours on free and report free size and largest block.
OffsetAllocator's bin rounding wastes at most 1/8 of a block for sizes off a bin boundary; Diligent's
manager wastes nothing but pays two ordered-map operations.

**Fixed-size shader-visible heaps.** Neither Diligent nor slang-rhi grows one. GPU handles in recorded
command lists, and the heap set by `SetDescriptorHeaps`, point into the old heap. nvrhi's `Grow`
replaces the shader-visible heap and releases the old one in the same call. Whether an in-flight
command list can still be executing against the old heap at that moment was not traced in this survey
and must be confirmed with a test. The two remedies are a size chosen up front (the tier limits are
1,000,000 CBV/SRV/UAV descriptors and 2048 samplers) or parking the old heap on the lifetime tracker
until every in-flight instance has completed.

**Chunk plus bump plus bulk free.** One mutex acquisition per chunk, none per draw; frees happen in bulk
when the GPU is known to be done; the only waste is the unused tail of the last chunk per context per
frame. Chunk size is the one knob: Diligent 256 and 32, slang-rhi 128 and 4. This is only worth having
when descriptors are written per bind, which is what the decision adds: every set a command list binds is
copied into its arena.

**Fence-keyed deferred free.** Diligent's stale queue and slang-rhi's pending-free list defer by fence or
submission ID with no object per allocation. nvrhi defers by holding an `AutoPtr` to the owning object on
the command-list instance, which is more general and heavier per object but needs no new machinery. A
transient chunk fits this model as a small reference-counted object whose destructor releases the range.

**Unit scaling and per-alignment pages.** slang-rhi keeps OffsetAllocator's offsets 32-bit by counting in
alignment units and keeps a page list per alignment class. The cost is the node table, because it sizes
`maxAllocs` to the unit count of the page:

| Page | Alignment | Units and nodes | Node table | Free list |
| --- | --- | --- | --- | --- |
| 256 MiB CUDA | 128 B | 2,097,152 | 56 MiB | 8 MiB |
| 256 MiB Vulkan | 256 B | 1,048,576 | 28 MiB | 4 MiB |
| 1,000,000 descriptors | 1 | 16,384 allocations max | 448 KiB | 64 KiB |

For descriptor heaps the metadata is negligible. For memory pages it is a quarter of the page in CPU
memory; a port caps `maxAllocs` at the number of allocations it will hold, not at the unit count.

**Default sizes for reference.**

| Library | CPU heaps CBV/SRV/UAV, sampler, RTV, DSV | GPU persistent | GPU transient | Chunk |
| --- | --- | --- | --- | --- |
| Diligent | 8192, 2048, 1024, 1024 per page | 16384 CBV/SRV/UAV, 1024 sampler | 8192, 1024 | 256, 32 |
| slang-rhi | 16384, 4096, 4096, 4096 per page | 1,000,000 and 2048 | arena from the same heap | 128, 4 |
| nvrhi (`d3d12::DeviceDesc`) | 16384 SRV, 1024 sampler, 1024 RTV, 1024 DSV, growing | same heaps | none | none |

### A.4 Two places where nothing changes

- **The framegraph branch does not need OffsetAllocator.** `src/render/framegraph/FrameGraphAliasing.cpp`
  on donut's `framegraph` branch assigns heap offsets at compile time from lifetime intervals and binds
  placed textures with `bindTextureMemory`. That is offline interval packing over one heap per compile,
  not a run-time allocator. OffsetAllocator would only matter there for a run-time pool of transient
  heaps, which the branch does not have.
- **`UploadManager` already has the Diligent shape.** Pages, bump pointer, release when the instance that
  wrote the page completes. The whole-chunk granularity matches Diligent's dynamic heap.

## Decision

Responsibilities, top to bottom:

| Layer | Owns | Does not own |
| --- | --- | --- |
| donut and the application | How often each binding changes; grouping bindings into layouts by frequency; the choice of volatile constant buffers, push constants and bindless tables; reusing sets (`engine::BindingCache`) | Descriptors |
| `IBindingLayout` | The backend segment: on D3D12 a contiguous run of root parameters and the ranges of its descriptor tables; on Vulkan a `VkDescriptorSetLayout`, with its size and binding offsets on the descriptor-buffer path (B.2) | Frequency |
| `IBindingSet` | Immutable descriptors and references to the resources they describe (B.3). D3D12 and the Vulkan descriptor-buffer path: CPU descriptors in a segment of a device-owned CPU heap. Vulkan otherwise: one persistent `VkDescriptorSet` from the device's `DescriptorSetAllocator` | Shader-visible descriptors on D3D12; a descriptor pool of its own |
| `ICommandList` | D3D12 and the Vulkan descriptor-buffer path: a GPU-visible descriptor arena; at `set*State` it copies every newly bound set into the arena and binds the copy. Vulkan otherwise: binds the set's `VkDescriptorSet` (B.6) | The sets' descriptors |
| Device | The CPU heaps shared by all sets, the shader-visible heaps or descriptor buffers, the Vulkan `DescriptorSetAllocator` (B.4) | |

### B.1 Frequency is not an nvrhi concept

nvrhi carries no update frequency. `BindingLayoutItem`, `BindingLayoutDesc`, `BindingSetDesc`,
`IBindingSet` and `IDescriptorTable` are `ethereal-dev`'s. `ResourceType::VolatileConstantBuffer`,
`ResourceType::PushConstants`, their `BindingLayoutItem` and `BindingSetItem` helpers,
`c_MaxVolatileConstantBuffersPerLayout` and `c_MaxVolatileConstantBuffers` are kept in their `ethereal-dev`
form, with their `ethereal-dev` rules. `c_HeaderVersion` is counted from `ethereal-dev`: 28 there, 29 here,
for the new `DeviceDesc` fields of B.4 and B.6. Revision 1's 29 and 30 leave the branch with its history (B.8).

The application expresses frequency with three tools nvrhi already has:

- **Set index.** A pipeline takes up to `c_MaxBindingLayouts` layouts, each on its own register space or
  descriptor set. A pass splits its bindings into per-frame, per-view, per-pass, per-material and per-object
  layouts and rebinds only the slots that change; `set*State` copies or rebinds only those (B.6).
- **Root arguments for per-draw constants.** `VolatileConstantBuffer` items (root CBVs on D3D12, dynamic
  uniform buffers on Vulkan) and `PushConstants`, with today's rules.
- **Bindless descriptor tables** for large arrays and for resources indexed per draw.

Samplers follow the same rule with one firm recommendation: the samplers of a frame or a pass go in one
sampler-only layout bound once per command list, such as donut's four shared `CommonRenderPasses` samplers,
so per-material and per-object sets carry no samplers. B.6 says why.

Sets that change per frame or per pass become separate immutable sets made once or cached by desc: the TAA
history ping-pong is two sets created once, and a pass whose inputs the FrameGraph pools and rotates looks
its set up in `BindingCache`. Creating a set is cheaper than on `ethereal-dev` (B.3), which is what makes this
practical. The policy, where each donut pass draws its layout boundaries, belongs to donut
(`ethereal-donut/doc/RdgDesign.md`, sections 3.3 and 6.1), not to this record.

### B.2 Binding layout: the backend segment

- *D3D12.* A layout is a contiguous segment of the pipeline's root signature, in this order: root constants
  for a `PushConstants` item, one root CBV per `VolatileConstantBuffer` item, one descriptor table for the
  CBV/SRV/UAV items, one descriptor table for the sampler items. Each table carries its descriptor ranges
  (register, space, offset in the table). The layouts of a pipeline are concatenated, as on `ethereal-dev`.
  The layout also records each table's size and each item's offset in it: that is the shape of a set's CPU
  segment and of its copy in the arena, which are identical, so one copy moves a whole table.
- *Vulkan, descriptor sets.* A layout is one `VkDescriptorSetLayout`, unchanged from `ethereal-dev`:
  `UNIFORM_BUFFER_DYNAMIC` for volatile constant buffers and a push constant range.
- *Vulkan, descriptor buffers.* The same `VkDescriptorSetLayout`, created with
  `VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`. Such a layout cannot hold `*_DYNAMIC`
  descriptors, so a volatile constant buffer item becomes a plain `UNIFORM_BUFFER` binding; the shader is
  unaffected. The layout records `vkGetDescriptorSetLayoutSizeEXT` and each binding's
  `vkGetDescriptorSetLayoutBindingOffsetEXT`: the shape of a set's CPU segment and of its copy in the arena,
  identical as on D3D12.
- *D3D11.* Unchanged.
- *Bindless layouts.* Unchanged, apart from the descriptor-buffer flag on that path.

### B.3 Binding set: immutable after creation

`IBindingSet` is `ethereal-dev`'s interface: `getDesc()` and `getLayout()`. There is no `setItem`, no
`setItems` and no `getGeneration`; a set does not change after `createBindingSet` returns.

- *D3D12.* `createBindingSet` allocates one range per table in the device's non-shader-visible CBV/SRV/UAV
  and sampler heaps (B.4) and creates the descriptors there. A set has no shader-visible range, and
  `copyToShaderVisibleHeap` is no longer called for sets. A volatile constant buffer item has no descriptor;
  it is a root CBV resolved at the draw, as today.
- *Vulkan, descriptor sets.* `createBindingSet` allocates one `VkDescriptorSet` from the device's
  `DescriptorSetAllocator` (B.4) and writes it once with `vkUpdateDescriptorSets`, as `ethereal-dev` writes the
  set it allocates from a pool of its own. A set creates no `VkDescriptorPool`. The set is never written again.
  `~BindingSet` returns it with `vkFreeDescriptorSets`; liveness makes that safe, because a command list that
  bound the set holds it through `referencedResources` until its instance completes. A volatile constant
  buffer item is a dynamic uniform buffer whose offset is given at the draw, as today.
- *Vulkan, descriptor buffers.* `createBindingSet` allocates one segment of the layout's size, aligned to
  `descriptorBufferOffsetAlignment`, in the device's CPU descriptor heap (B.4), and writes each descriptor at
  its binding offset with `vkGetDescriptorEXT`. A volatile constant buffer item's slot is left empty; the
  command list writes it at the bind (B.6). The set has no `VkDescriptorSet`.
- *D3D11.* Unchanged.

Liveness is unchanged on every path: a set holds references to its resources, and a command list holds the set
through `referencedResources` when `trackLiveness` is set; with it off, the application keeps the set alive
while the GPU uses it, as today. On D3D12 and on the descriptor-buffer path, a set's CPU segment is read only
on the CPU timeline, by `CopyDescriptors` or a `memcpy` while a command list records, so it could return to the
device the moment the set is destroyed with no deferred free. On those two paths `getNativeObject` on a
binding set has no `VkDescriptorSet` or shader-visible GPU handle to return; on the descriptor-set path it
still returns the `VkDescriptorSet`.

### B.4 Device-owned descriptor storage

**D3D12.** Binding sets leave the shader-visible heaps. The device owns:

| Heap | Shader-visible | Holds | Allocator |
| --- | --- | --- | --- |
| CPU CBV/SRV/UAV (`cpuShaderResourceViewHeap`) | no | Binding sets | OffsetAllocator; grows by copy as today, replaying live ranges |
| CPU sampler (`cpuSamplerHeap`) | no | Binding sets | as above |
| RTV, DSV | no | As today | as above |
| Shader-visible CBV/SRV/UAV (`shaderResourceViewHeap`) | yes, with a CPU staging heap at the same indices | Descriptor tables, the command lists' arena chunks (B.6), and the descriptors a UAV clear or a native GPU handle query needs | OffsetAllocator; grows, the old heap parked (B.5) |
| Shader-visible sampler (`samplerHeap`) | yes, with a CPU staging heap at the same indices | Sampler descriptor tables and sampler arena chunks; capped at 2048 | as above |

The two kinds are sized independently through `d3d12::DeviceDesc`: `shaderResourceViewHeapSize` and
`samplerHeapSize` for the binding-set heaps, `shaderVisibleShaderResourceViewHeapSize` and
`shaderVisibleSamplerHeapSize` for the shader-visible ones. A shader-visible heap keeps the CPU staging heap it
had on `ethereal-dev`, at the same indices: whatever must be shader-visible for longer than one command list, a
descriptor table, a UAV clear's descriptor, a texture's native GPU handle, is written into the staging range and
copied in, because a `ClearUnorderedAccessView` needs a CPU handle as well as a GPU one and `CopyDescriptors` cannot
read a shader-visible heap. Only arena chunks are written into the shader-visible heap directly, by the copy at
`set*State`; their staging range is unused.

**Vulkan, descriptor sets.** The device owns one `DescriptorSetAllocator`: revision 1's pool manager (B.8), a
list of pools created with `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` and sized from
`vulkan::DeviceDesc` (sets per pool, descriptors per type per pool). It allocates from the first pool with
room and creates a pool when none has; sets are freed individually. Binding sets and bindless descriptor
tables allocate from it. This is Diligent's `DescriptorSetAllocator` for static and mutable resources.

**Vulkan, descriptor buffers.** The device owns:

| Storage | Holds | Allocator |
| --- | --- | --- |
| CPU descriptor heap (`CpuDescriptorHeap`) | Binding sets' segments | Host memory pages of 1 MiB with an OffsetAllocator each, in units of `descriptorBufferOffsetAlignment` |
| Descriptor buffer pages (`DescriptorBufferHeap`) | Descriptor tables, and the command lists' arena chunks (B.6) | A page list: host-visible, device-addressable buffers of `vulkan::DeviceDesc::descriptorBufferPageSize` bytes with both `RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT` and `SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT` usage, an OffsetAllocator each; a page is added when none has room, up to `maxDescriptorBufferBindings` pages |

One buffer kind holds samplers and resources together because an nvrhi set layout mixes them: a set's descriptors are
one segment at one offset of one buffer, and a buffer bound for a set with samplers must carry the sampler usage. Both
usages on one buffer are allowed within `descriptorBufferAddressSpaceSize`, and a page is capped at
`maxSamplerDescriptorBufferRange` and `maxResourceDescriptorBufferRange`. A command buffer binds the whole page list with
one `vkCmdBindDescriptorBuffersEXT`, page i at buffer index i, and rebinds it only when a page has been added; that
plays the part of `SetDescriptorHeaps`. Pages never move, grow or shrink, so there is no CPU shadow and nothing to
copy or park: a range stays valid until it is released, and a page lives as long as the device and as long as any
command buffer that bound it. Descriptor tables are written in place into their range. On D3D12 a page list is not
possible, because one shader-visible heap per type is bound at a time.

**Choosing the Vulkan path.** The path is a device-wide choice made at device creation: `vulkan::DeviceDesc`
gains an opt-in flag (named in the plan), and the descriptor-buffer path is taken when the flag is set and the
device exposes `VK_EXT_descriptor_buffer` with its `descriptorBuffer` feature. Every layout and pipeline of that
device then uses it, since a pipeline created with `VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT` cannot bind
descriptor sets and the two cannot be mixed in one pipeline. The application-facing API is the same on both
paths.

Range allocation everywhere is revision 1's: an OffsetAllocator per heap, buffer or page, the owner of a range
storing the `Allocation`, and the storage report at device destruction.

### B.5 Shader-visible heap lifetime

Rule: **a shader-visible heap or descriptor buffer that any recorded or in-flight command list references is
never released until every such instance has completed.** `StaticDescriptorHeap::Grow` parks the old
shader-visible heap on the device's lifetime tracker, which releases it when the last `CommandListInstance`
created before the grow completes; CPU heaps are released at once, since nothing on the GPU references them.
`Grow` copies the staging heap into the new shader-visible heap, which carries every descriptor table and
every UAV clear descriptor over; arena chunks are carried over as garbage and never read there, because each
arena abandons its current chunk when the heap changes and chunks already recorded stay valid in the parked heap.
The parking is implemented on `binding-freq` and stays. After this revision only descriptor tables and arena chunks
live in the shader-visible heaps and descriptor buffers, so binding sets no longer drive their growth. Neither Vulkan
path has a heap that moves: descriptor pools and descriptor buffer pages stay where they are, and a page list
replaces growth (B.4).

### B.6 Binding at `set*State`

**D3D12.**

- Each `CommandList` owns two arenas, one per shader-visible heap type. An arena takes chunks of
  `max(chunkSize, count)` from the device's shader-visible heap (B.4), bumps inside the current chunk, and
  pushes each chunk onto the instance's `referencedResources`, so the chunk returns when the GPU is done.
  This is revision 1's arena unchanged.
- "Owned by the command list" means the command list owns chunks of the device's shader-visible heap, not
  an `ID3D12DescriptorHeap` of its own. D3D12 binds one CBV/SRV/UAV heap and one sampler heap at a time, and
  bindless tables, including `ResourceDescriptorHeap` indexing, must be in the bound heap; a heap per command
  list would make descriptor tables unusable in the same pass.
- `set*State` uses the existing `bindingUpdateMask` (a slot whose set pointer changed) to find the sets to
  copy. For those it sums the table sizes per heap type, allocates one contiguous arena range per heap type,
  issues one `ID3D12Device::CopyDescriptors` per heap type with one source range per set (its CPU segment)
  and one destination range, and sets each layout's root tables to their offsets in the destination.
  Volatile root CBVs and root constants are applied as on `ethereal-dev`. An `IDescriptorTable` in the
  binding array is bound from its persistent shader-visible range, with no copy.
- **Per-command-list bind cache.** A small map from set to its arena handles, cleared at `open()` and when
  the arena moves to a new heap after `Grow`. Binding a set that this command list has already copied, in an
  A, B, A pattern or across interleaved draws of one material, reuses the copy. An arena range is written
  once and never rewritten, so reuse is safe and the D3D12 rule for non-volatile descriptors holds.
- Chunk sizes stay `d3d12::DeviceDesc::dynamicDescriptorChunkSize` and `dynamicSamplerChunkSize`; their
  defaults rise from 256 and 32 to 1024 and 64 because every bound set now passes through the arena, to be
  tuned by measurement. Exhaustion is an error that names the knob.
- **The sampler heap.** The shader-visible sampler heap holds at most 2048 descriptors for the whole device,
  and every sampler table a command list binds is now copied into it. Three measures, and only these, keep it
  in budget:
  1. **A per-command-list cap.** `maxDynamicSamplerDescriptors` (default 256) limits the sampler descriptors
     one command list may hold in its arena. Going past it is an error that names the knob; it is never a
     silent heap growth that starves descriptor tables.
  2. **The bind cache.** A set's sampler table is copied once per command list however often the set is
     rebound.
  3. **Guidance (B.1).** Samplers live in one sampler-only layout per frame or pass, bound once per command
     list, so a command list copies a handful of sampler descriptors rather than one table per material.

  nvrhi gains no immutable samplers and does not deduplicate sampler tables by content; both are recorded
  under alternatives. Neither Vulkan path has a sampler heap of fixed size: the descriptor-set path has pools, and
  on the descriptor-buffer path samplers share the pages with resources, so the cap is D3D12's only.
- **Ray tracing.** `ShaderTable::bake` already runs per command list through `ShaderTableState`. It copies each
  record's local binding set into the arena, through the same bind cache, and writes the arena handles into
  the record. A shader table is rebaked when the arena moves to a new heap, as it is today on a version change.
  Because the records then point into one command list's arena, a table whose pipeline has local bindings is
  baked once per command list even when it is cached: its cache buffer, shared by every command list, could not
  hold records for two arenas at once.

**Vulkan, descriptor sets.**

- `bindBindingSets` binds each newly bound set's own `VkDescriptorSet` with `vkCmdBindDescriptorSets`, with
  dynamic offsets for volatile constant buffers, exactly as on `ethereal-dev`. There is no copy, no arena and
  no bind cache. Bindless descriptor tables are bound the same way.

**Vulkan, descriptor buffers: the D3D12 pattern.**

- Each command list owns one arena: chunks of `vulkan::DeviceDesc::descriptorBufferChunkSize` bytes from the device's
  pages, each pushed onto the command buffer's `referencedResources` and released when it completes, as on D3D12.
- `bindBindingSets` first copies every set it binds: an arena range of the layout's size, a `memcpy` of the set's CPU
  segment into the mapped page. Then it binds the page list if this command buffer has not bound the current one
  (the first bind, or a page was added), and then calls `vkCmdSetDescriptorBufferOffsetsEXT` once per contiguous run
  of sets, with each set's page as its buffer index. Rebinding descriptor buffers is expensive on some
  implementations, so it happens once per command buffer in the common case. An `IDescriptorTable` is bound at its
  persistent range, with no copy.
- **Volatile constant buffers.** With no dynamic offsets on this path, the copy carries the volatile buffer's
  descriptor: at the bind the command list writes it with `vkGetDescriptorEXT` from the address of the
  buffer's current version. A later `writeBuffer` to that buffer before the next draw makes a new copy of the
  set at that draw, one small `memcpy` and one `vkGetDescriptorEXT` per volatile item, where the
  descriptor-set path changes a dynamic offset. Push constants are unchanged.
- The same bind cache as on D3D12, keyed by the set and the volatile versions written into its copy.
- nvrhi's Vulkan backend has no local ray tracing bindings, so there is nothing to bake on either path.

**D3D11.** Unchanged; sets are bound directly.

**State tracking.** As on `ethereal-dev`: when automatic barriers are enabled, `set*State` requires the states
of the resources of each newly bound set through `setResourceStatesForBindingSet`.

### B.7 Validation layer

The rules are `ethereal-dev`'s: layout match per slot at `set*State`, the volatile-versus-regular buffer
rule, volatile buffers written before the draw, push constants set before the draw. Every frequency check
from revision 1 is removed. What is new is the arena exhaustion error and the per-command-list sampler cap
error, each naming its `DeviceDesc` field, on D3D12 and on the descriptor-buffer path, and the storage report
at device destruction.

### B.8 What happens to the revision 1 work

The history is rewritten, not reverted. Each `binding-freq` branch is tagged `binding-freq-rev1` at its
revision 1 tip, then reset to the commit it branched from: nvrhi `ethereal-dev` (`296422b`), donut
`ethereal-dev` (`9f51e7a`), samples `main` (`54f0325`) and the aggregate `main` (`e02979c`). Revision 2 is built
on top as new commits, re-applying the parts of revision 1 it keeps, and the branches are force-pushed. The
hashes below are reachable through the tag; the plan does the exact split.

| nvrhi commit | Content | Fate |
| --- | --- | --- |
| `049a1c7` | OffsetAllocator and its tests | Re-applied |
| `7658254` | Vulkan device-level descriptor pool manager for binding sets | Re-applied without its transient pools, renamed `DescriptorSetAllocator`; binding sets and descriptor tables allocate from it |
| `56e7d0f` | D3D12 OffsetAllocator ranges, parked shader-visible heaps | Re-applied; the mirror between CPU and shader-visible heaps is removed on top |
| `9d40474` | D3D12: versions, dynamic items, transient arena | The arena is re-applied with B.6's role; versions, generations and draw-time dynamic items are dropped |
| `3245190` | Binding frequencies on a real Vulkan device | The headless Vulkan test device is re-applied; the tests are replaced by tests of B.6 on both Vulkan paths and on D3D12 |
| `4d396ed`, `98096b5` | Frequencies, writable `IBindingSet`, `commitBindingSets`; volatile and push constant aliases removed | Dropped; the binding API stays `ethereal-dev`'s |
| `610c2f3` | D3D11 writable sets | Dropped |
| `6408f71`, `5d062ce` | Validation frequency checks and their tests | Dropped |
| `0c218d9` | Vulkan: writable sets, versions in transient pools, dynamic items | Dropped; the descriptor-set path is `ethereal-dev`'s binding with the pool manager underneath |
| `8462841` | Programming Guide and Tutorial | Dropped; rewritten for this revision |
| `61fda40`, `7f7a781`, `e0df03b` | ADR 0008 revision 1, its plan and their records | Replaced by this revision and its plan; the revision 1 plan is kept as an abandoned record |

The donut and samples sweep commits (`b6c333a`, `d4c5d5d`) and the aggregate's two `binding-freq` commits are
dropped. The Vulkan descriptor-buffer path is new code with no counterpart in revision 1.

## Consequences

### Positive

- nvrhi's binding API is `ethereal-dev`'s again; existing layouts and sets compile unchanged, and nvrhi holds
  no policy about frequency.
- Sets are immutable: no generations, no commit step, no write-versus-record race.
- Creating a set is cheaper. On D3D12 and on the descriptor-buffer path it writes CPU descriptors only, with no
  shader-visible allocation and no copy; on the Vulkan descriptor-set path it is one allocation from shared
  pools and one update, with no pool per set. `BindingCache` misses and FrameGraph-rotated inputs cost less.
- The D3D12 shader-visible heaps and the Vulkan descriptor buffers hold bindless tables and in-flight arena
  chunks only. Thousands of cached sets no longer occupy shader-visible space, and none of them sit
  permanently in the 2048-entry sampler heap.
- On Vulkan without descriptor buffers, binding costs what it costs on `ethereal-dev`: no per-bind copy. This
  is the model Diligent uses for static and mutable resources.

### Negative

- A per-bind cost on D3D12 and on the descriptor-buffer path where `ethereal-dev` had none: one copy per newly
  bound set. The bind cache bounds this to once per set per command list, plus a recopy per volatile buffer
  rewrite on the descriptor-buffer path. Measured on an RTX 4050 laptop (plan, step 9): the copy itself is cheap,
  but its writes into the shader-visible heap are write-combined, and their drain at the next locked instruction
  costs about 0.3 us per first bind of a set on the RTX 4050 and 0.85 us on a Radeon 780M integrated GPU
  (`nvrhi_bench_set_binding`); a bind-cache hit costs what a bind cost on `ethereal-dev`. A scene that binds a different set for every draw pays that per draw: the Asteroids
  benchmark with one set per asteroid (50,000 draws, `mut`) went from 10-12 ms to 30-51 ms of CPU time per frame
  on D3D12, while its ten-set modes and nv_asteroids stayed within a few percent. Per-object data belongs in
  volatile constant buffers, push constants or bindless tables, not in per-object sets (B.1).
- Arena traffic grows from revision 1's mutable versions to every bound set; chunk sizes and the initial
  shader-visible heap size need retuning, and each new command list copies its sets afresh.
- Two Vulkan paths to keep and test, chosen per device. Their behaviour must match; the test suite runs on both.
- On the Vulkan descriptor-set path every live set still holds a descriptor set, so the number of cached sets
  drives the pool count, though as a handful of pools rather than one per set.
- Ray tracing shader tables copy their local sets per command list at bake on D3D12.
- Native object queries for a set's GPU handle stop working on D3D12 and on the descriptor-buffer path.
- Most of revision 1's implementation is discarded (B.8).
- Frequency design moves to donut, which now has to choose layout boundaries per pass.

### Risks

- The descriptor-buffer path depends on the driver: descriptor sizes, rebinding cost, the memory its pages can
  live in and the sampler address space differ by vendor. Measured (plan, step 9) it costs 1.2-1.5x the
  descriptor-set path in the Asteroids benchmark once its pages are in cached host memory (5x with device-local
  pages), so it stays opt-in.
- The sampler heap. Sampler tables are now copied per bind into the 2048-entry D3D12 heap, shared by every
  command list in flight. The decision in B.6 is a per-command-list cap, the bind cache and the guidance to keep
  samplers in one per-frame set; a scene that still exhausts the cap has to restructure its sampler layouts,
  because nvrhi offers no immutable samplers to fall back on. The heap report at device destruction and the
  cap's error message are how such a scene is found.
- Heap or descriptor-buffer growth while arena chunks are in flight; B.5's parking covers it and its test stays.

### Set against Diligent

- **D3D12.** Diligent copies an SRB's CPU descriptor cache into the GPU heap at `CommitShaderResources` for
  dynamic variables and keeps static and mutable variables in persistent shader-visible ranges per SRB. This
  revision takes the copy path for every set and keeps no persistent shader-visible range per set: more copies
  at bind time, in exchange for cheap sets and a small persistent heap.
- **Vulkan.** At `074060cfd`, Diligent keeps its static and mutable resources in one persistent
  `VkDescriptorSet` per SRB from the device's `DescriptorSetAllocator`, written with `vkUpdateDescriptorSets`
  when a variable is set and bound directly. Its dynamic variables live in a second descriptor set, which needs
  SPIR-V remapping and is allocated afresh from a per-context `DynamicDescriptorSetAllocator` and written at
  every `CommitShaderResources`. Buffers whose offset changes get only a dynamic offset at the draw. The
  descriptor-set path here is Diligent's static and mutable model, written once at creation because nvrhi sets
  are immutable; Diligent's per-commit dynamic set has no counterpart, since a per-change binding is a separate
  nvrhi set and per-draw constants use dynamic offsets, as Diligent's do. Diligent has no descriptor-buffer path.
- **API.** The frequency Diligent declares per variable is declared here by the layout boundaries the
  application draws; Diligent's `CommitShaderResources` transition mode has no counterpart beyond nvrhi's
  automatic barriers.

## Alternatives considered

- **Revision 1: a frequency on every binding layout item, carried by one writable `IBindingSet`, committed by
  `commitBindingSets` with a resource state transition mode.** Accepted 2026-10-13 and implemented on
  `binding-freq`; overturned in revision 2. It put a policy into nvrhi that layouts already express, added
  two application rules (commit after a mutable write, set dynamic items before the draw), generations, a
  commit map per command list, versions with their own resource references, and a sweep of every layout in
  donut and the samples. Its own earlier drafts, a dynamic flag on the set and a separate
  `IDynamicBindingSet` with `setDynamicBinding`, were rejected inside revision 1 for the same reasons it
  was later rejected as a whole.
- **Keep `ethereal-dev`'s persistent shader-visible range per set on D3D12.** No per-bind copy, but every live
  set holds shader-visible space, sampler sets hold the 2048 heap, creating a set copies to the shader-visible
  heap, and growth is driven by the number of cached sets.
- **A separate shader-visible `ID3D12DescriptorHeap` per command list.** Breaks bindless tables and
  `ResourceDescriptorHeap` indexing in the same pass, because one heap per type is bound at a time.
- **Vulkan: host-side descriptor records and a template update into a transient set at every bind (the first
  draft of revision 2).** The D3D12 pattern in core Vulkan, but every bind allocates and writes a set:
  Diligent's dynamic path applied to every set, which Diligent itself limits to dynamic variables. Replaced by
  persistent sets.
- **Vulkan: a staging `VkDescriptorSet` per set and `vkCopyDescriptorSet` at bind.** The staging set is already
  bindable, so the copy costs time and buys nothing.
- **Vulkan: `VK_EXT_descriptor_buffer` as the only path.** Not available on every device nvrhi targets; it is
  the opt-in path.
- **Vulkan: descriptor buffers and descriptor sets mixed per pipeline.** Not allowed by the extension.
- **No bind cache.** Simpler, but an A, B, A pattern copies A twice per command list.
- **Immutable samplers in `BindingLayoutDesc`** (D3D12 static samplers in the root signature, Vulkan
  `pImmutableSamplers`, embedded samplers on the descriptor-buffer path). Diligent's main answer to the sampler
  heap: such samplers take no heap space. Not taken: it is a new API item that `ethereal-dev` does not have,
  and the per-frame sampler set gives most of the benefit with the API unchanged. It remains the next step if
  the cap proves too tight.
- **Deduplicating sampler tables by content per command list.** Sampler descriptors are values, so two sets
  with the same samplers could share one arena copy. Not taken: it adds a content hash to every bind for a case
  the per-frame sampler set already removes.
- **A reserved dynamic region of the shader-visible heap, as Diligent does.** As in revision 1: off the table
  while the heap grows; it returns with fixed-size heaps.
- **Diligent's map-based allocator instead of OffsetAllocator.** As in revision 1: descriptor indices need no
  alignment and the heaps are small, so the O(1) allocator wins.

## Implementation

Revision 1 was implemented on nvrhi `binding-freq` (header 28 to 30), donut and samples `binding-freq`,
following [`../plans/2026-10-13-binding-frequencies.md`](../plans/2026-10-13-binding-frequencies.md); its
commits are kept under the tag `binding-freq-rev1`. Revision 2 is carried out by
[`../plans/2026-10-15-immutable-binding-sets.md`](../plans/2026-10-15-immutable-binding-sets.md), which rebuilds
the `binding-freq` branches from their branch points (B.8); done 2026-10-15 on the Windows host, with the
measurements, the deviations and what remains in the plan's execution record.

## References

- nvrhi `ethereal-dev`: `include/nvrhi/nvrhi.h` (`BindingLayoutItem`, `BindingLayoutDesc`, `BindingSetDesc`,
  `IBindingSet`, `IDescriptorTable`, `GraphicsState`); `include/nvrhi/d3d12.h` and `vulkan.h` (`DeviceDesc`);
  `src/d3d12/d3d12-descriptor-heap.cpp` (`StaticDescriptorHeap::Grow`, `copyToShaderVisibleHeap`);
  `src/d3d12/d3d12-resource-bindings.cpp` (`BindingSet::createDescriptors`, `CommandList::setComputeBindings`,
  `setGraphicsBindings`, `bindingUpdateMask`, `writeDescriptorTable`, `resizeDescriptorTable`);
  `src/d3d12/d3d12-raytracing.cpp` (`ShaderTable::bake`); `src/d3d12/d3d12-state-tracking.cpp`
  (`setResourceStatesForBindingSet`); `src/d3d12/d3d12-commandlist.cpp` (`commitDescriptorHeaps`);
  `src/d3d12/d3d12-backend.h` (`CommandListInstance::referencedResources`);
  `src/vulkan/vulkan-resource-bindings.cpp` (`Device::createBindingSet`, `CommandList::bindBindingSets`);
  `src/vulkan/vulkan-backend.h` (`TrackedCommandBuffer`, `BindingSet`); `src/validation/validation-device.cpp`,
  `src/validation/validation-commandlist.cpp`; `doc/com-interface-abi.md`.
- nvrhi `binding-freq`: the commits in B.8; `src/common/offset-allocator.{h,cpp}`.
- donut: `doc/RdgDesign.md`, sections 3.3 and 6.1; `src/render/framegraph/FrameGraphAliasing.cpp` on
  the `framegraph` branch.
- Diligent Engine, D3D12: `Graphics/GraphicsEngineD3D12/include/DescriptorHeap.hpp` and `src/DescriptorHeap.cpp`
  (`GPUDescriptorHeap`, `DynamicSuballocationsManager`, `CPUDescriptorHeap`); `include/RootParamsManager.hpp`;
  `src/PipelineResourceSignatureD3D12Impl.cpp` (`CommitRootTables`); `src/DeviceContextD3D12Impl.cpp`
  (`CommitShaderResources`).
- Diligent Engine `074060cfd`, Vulkan: `Graphics/GraphicsEngineVulkan/include/ShaderResourceCacheVk.hpp` and
  `src/ShaderResourceCacheVk.cpp` (the SRB cache, immediate writes of static and mutable variables);
  `include/DescriptorPoolManager.hpp` and `src/DescriptorPoolManager.cpp` (`DescriptorSetAllocator`,
  `DynamicDescriptorSetAllocator`); `src/DeviceContextVkImpl.cpp` (`CommitShaderResources`, the dynamic set
  allocation, `CommitDescriptorSets` and its dynamic offsets); `src/PipelineResourceSignatureVkImpl.cpp`
  (`CommitDynamicResources`).
- Diligent Engine, shared: `Graphics/GraphicsAccessories/interface/VariableSizeAllocationsManager.hpp`;
  `doc/PerformanceGuide.md`.
- slang-rhi `92932ae`: `src/core/offset-allocator.{h,cpp}`, `tests/test-offset-allocator.cpp`,
  `src/heap.{h,cpp}`, `src/d3d12/d3d12-descriptor-heap.{h,cpp}`, `src/d3d12/d3d12-command.cpp`.
- OffsetAllocator upstream: Sebastian Aaltonen, MIT licence, 2023.
- Vulkan: `VK_EXT_descriptor_buffer` (`vkGetDescriptorSetLayoutSizeEXT`,
  `vkGetDescriptorSetLayoutBindingOffsetEXT`, `vkGetDescriptorEXT`, `vkCmdBindDescriptorBuffersEXT`,
  `vkCmdSetDescriptorBufferOffsetsEXT`, `VkPhysicalDeviceDescriptorBufferPropertiesEXT`);
  `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`.
- Related: [ADR 0002](0002-irhiobject-root-interface-and-iids.md) (IIDs on every interface),
  [`../com-interface-abi.md`](../com-interface-abi.md) section 3.4 (evolving an interface), and the plan
  [`../plans/2026-10-13-binding-frequencies.md`](../plans/2026-10-13-binding-frequencies.md).
