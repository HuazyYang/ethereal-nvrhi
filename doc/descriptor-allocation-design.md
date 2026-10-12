# Descriptor allocation design: heaps, transient descriptors and dynamic binding sets

- Status: Proposed
- Date: 2026-10-13
- Scope: nvrhi `ethereal-dev` (surveyed at `8d6fa3d`); donut consumers on the `framegraph` branch
- Sources surveyed: slang-rhi at commit `92932ae` (2026-10-12) and Diligent Engine `DiligentCore`
  as checked out next to the aggregate

This document has two halves that depend on each other. Part A is a survey of how slang-rhi uses
Sebastian Aaltonen's OffsetAllocator and how Diligent Engine's `DynamicSuballocationsManager` and
its related allocators manage descriptor heaps and dynamic memory, set against what nvrhi does today.
Part B is the design that follows from it: a coalescing allocator for nvrhi's descriptor heaps, a
rule for shader-visible heap lifetime, a transient descriptor arena per command list, and the one
new public capability that needs the arena, **dynamic binding sets**, which are rewritable binding
sets backed by transient descriptors. The static and mutable update-frequency classes of an earlier
draft are not part of this design: they described what nvrhi already does and changed no behaviour.

## Part A. Survey

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
when something writes descriptors per draw, which is what Part B adds.

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

## Part B. Design

Four pieces, in dependency order. B.1 is a prerequisite for everything that allocates from the
shader-visible heap under load. B.2 and B.3 are the allocation layer. B.4 is the transient arena, and
B.5 is its consumer and the only public API change.

### B.1 Shader-visible heap lifetime

Rule: **a shader-visible heap that any recorded or in-flight command list references is never released
until every such instance has completed.**

Implementation, one of:

- `StaticDescriptorHeap::Grow` parks the old shader-visible heap on the device's lifetime tracker, which
  releases it when the last `CommandListInstance` created before the grow completes. The CPU heap is
  released as today; nothing on the GPU references it.
- The shader-visible heaps are fixed-size, chosen from `d3d12::DeviceDesc` at the tier limits, and
  `Grow` applies to CPU heaps only. Diligent and slang-rhi both take this path.

The first keeps nvrhi's grow-on-demand behaviour and is the recommended first step; the second is the
fallback if parking turns out to interact badly with `commitDescriptorHeaps`, which re-issues
`SetDescriptorHeaps` only when the heap pointer changes. A test that fills the SRV heap past its initial
size while a command list that bound descriptors from it is executing confirms the hazard before the fix
and the fix after.

### B.2 Descriptor range allocation

`StaticDescriptorHeap` replaces the `vector<bool>` scan with an **OffsetAllocator per heap**. Offsets are
descriptor indices, so the unit is one descriptor and `maxAllocs` is a new `d3d12::DeviceDesc` knob
(default 16 K per heap, after slang-rhi). `allocateDescriptors(count)` becomes `allocate(count)`;
`releaseDescriptors(base, count)` needs the node index, so the heap keeps a small side table from base
index to node index, or the backend objects that own ranges (`BindingSet`, `DescriptorTable`,
`Texture`'s view caches) store the `Allocation` instead of the bare index. The second is cleaner and is
the intended form; `IDescriptorHeap`'s public index-based methods stay and look the node up.

`Grow`, when kept under B.1, creates a larger allocator and replays the live ranges into it. The
`storageReport()` numbers replace `m_NumAllocatedDescriptors` and are exposed through the existing
message callback at device destruction, as Diligent logs its peak usage.

### B.3 Vulkan descriptor pool manager

A device-level **pool manager** replaces the one-`VkDescriptorPool`-per-binding-set pattern. It holds
pools created with `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` and sized from
`vulkan::DeviceDesc` (sets per pool and per-type descriptor counts); `createBindingSet` allocates from
the pool with room, creating a pool when none has, and `~BindingSet` frees its set back. The same
manager hands out reset-per-recycle pools for the transient arena in B.4, created without the free bit.

### B.4 Transient descriptor arena

This is the `DynamicSuballocationsManager` role, with slang-rhi's choice of taking chunks from the
general allocator rather than from a reserved region.

**D3D12.** Each `CommandList` owns two arenas, one per shader-visible heap type. An arena holds the
current chunk and a bump offset. A chunk is a descriptor range taken from the heap's allocator (B.2)
and wrapped in a small reference-counted object whose destructor releases the range; the object is pushed
onto the instance's `referencedResources`, so the range returns exactly when nvrhi already knows the GPU
is done with the instance. Chunk sizes come from `d3d12::DeviceDesc::dynamicDescriptorChunkSize`
(default 256) and `dynamicSamplerChunkSize` (default 32). Descriptors are created **directly in the
shader-visible heap** through the CPU handle of the shader-visible range; the CPU mirror is not touched,
and nothing copies that index range except its owner. Chunks from the general allocator rather than a
reserved region, as Diligent does, because a reserved region would have to survive `Grow`'s copy and
move with it, while ordinary ranges survive the copy for free.

**Vulkan.** Each `TrackedCommandBuffer` owns a transient pool list taken from the manager in B.3. An
allocation is one `vkAllocateDescriptorSets`; on `VK_ERROR_OUT_OF_POOL_MEMORY` the arena takes the next
pool. When the command buffer is recycled, every pool it took is `vkResetDescriptorPool`ed and returned.

**D3D11.** No arena; the role does not exist.

### B.5 Dynamic binding sets

The consumer of B.4 and the only public API change.

**The gap.** A binding set is immutable; that is the right trade for bindings that live for many frames
and the wrong one for bindings that change every frame or draw, where every new combination of resources
is a new set: a range allocation under the heap mutex, descriptor creation, a copy into the shader-visible
heap, a pool plus allocate plus update on Vulkan, an entry in donut's `engine::BindingCache`, and a
reference held until the GPU is done. The cases that pay this are the ones that choose resources at run
time: the TAA history ping-pong, SSAO's indexed sets, and any pass whose inputs the FrameGraph pools and
rotates (`ethereal-donut/doc/RdgDesign.md`, sections 3.3 and 6.1). Volatile constant buffers and push
constants carry constants only; descriptor tables exist only for bindless layouts and carry no liveness
or state tracking. Diligent fills this role with `SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC` committed through
`DynamicSuballocationsManager`.

**Constraints.** nvrhi never rewrites shader bytecode, so one `IBindingLayout` is one descriptor set on
Vulkan and one root-parameter group on D3D12, with the set index fixed by the shader through
`registerSpaceIsDescriptorSet`; a layout cannot be split into a persistent and a dynamic part. The ABI
rules in [`com-interface-abi.md`](com-interface-abi.md) forbid adding methods to a released interface, so
`IDevice`, `ICommandList` and `IBindingSet` cannot gain methods. The `framegraph` branch already defines
`ICommandList1`, and a design that needs no command-list method avoids a merge decision.

**B.5.1 Dynamic is a property of the set, not of the layout.** `BindingSetDesc` gains
`bool isDynamic = false`. A set created with it is a **dynamic binding set**. The layout, the root
signature and the Vulkan pipeline layout are unchanged: a dynamic set has exactly the descriptor-table
shape its layout describes, only its storage is transient. Any layout can back both persistent and
dynamic sets, and a pipeline does not know which kind is bound in a slot. This is the whole-layout
granularity the first constraint forces, and it is why no root-parameter regrouping of Diligent's kind is
needed. `isDynamic` lives in the padding after `trackLiveness` (offset 32 of 40), so `sizeof` and the
`offsetof` asserts in `nvrhi.h` hold; `c_HeaderVersion` is bumped regardless.

**B.5.2 The write interface is a derived interface on the set.**

```cpp
NVRHI_IID(IDynamicBindingSet, "<new iid>")
struct IDynamicBindingSet : IBindingSet
{
    NVRHI_DECLARE_UUID_TRAITS(IDynamicBindingSet)

    // Replaces the binding with the same slot, type and arrayElement. Takes effect at the next
    // setGraphicsState / setComputeState / setMeshletState / setRayTracingState on any command list.
    // Returns FE_INVALID_ARGS when the layout has no such binding.
    virtual FRESULT writeItem(const BindingSetItem& item) noexcept = 0;

    // Replaces every binding. The desc must cover the layout exactly, as for createBindingSet.
    virtual FRESULT write(const BindingSetDesc& desc) noexcept = 0;

    // Incremented by every successful write. Command lists use it to decide whether a bound set
    // needs a new version; applications may use it for their own caching.
    [[nodiscard]] virtual uint64_t getGeneration() const noexcept = 0;
};

typedef AutoPtr<IDynamicBindingSet> DynamicBindingSetHandle;
```

`IDevice::createBindingSet` with `desc.isDynamic` returns an object that answers
`IID_IDynamicBindingSet` through `QueryInterface`. `IBindingSet::getDesc()` returns the current
canonical content, valid until the next write. A `nvrhi::utils::CreateDynamicBindingSet` helper does the
create-and-query in one call; it is a plain function, not an interface method. The write lives on the set
and not on `ICommandList` because it changes the set's canonical content, which is command-list
independent; the per-command-list part, the transient copy, is made at bind time where the command list
already is. No released vtable changes.

**B.5.3 Semantics.**

- A dynamic set has **canonical content** from creation and can be bound without ever being written.
  This differs from volatile constant buffers, which must be written in every command list before use.
- A write takes effect at the **next `set*State`** on any command list. A set that is already bound keeps
  drawing with the version it was given; a write between `setGraphicsState` and `draw` does not affect
  that draw. This is Diligent's rule for `SetResource` after `CommitShaderResources`.
- Each `set*State` that binds a dynamic set whose `(set, generation)` differs from what that command
  list bound last time creates a **version**: an immutable transient copy owned by that command-list
  instance, released with it through `referencedResources`.
- The version holds its own references to the resources it points at, so the application may write a new
  resource into the set while an older version is still in flight.
- Writes are **not synchronised** against concurrent recording. Writing a set while another thread records
  a `set*State` that binds it is a race, as for Diligent SRBs. Writing from the recording thread between
  draws is the intended use.
- Volatile constant buffers and push constants in a dynamic set behave as in a persistent set.

**B.5.4 Backend implementation.**

- *D3D12.* A version allocates `layout->descriptorTableSizeSRVetc` and `descriptorTableSizeSamplers` from
  the command list's arenas (B.4) and creates the descriptors there with the code factored out of
  `BindingSet::createDescriptors` into a routine that writes into a caller-supplied handle range. Volatile
  constant buffers stay root CBVs, taken from the version's snapshot as `setComputeBindings` does for a
  persistent set. State tracking runs `setResourceStatesForBindingSet` over the snapshot, so it is
  refactored to take a desc and a layout rather than a `BindingSet*`. `setGraphicsState` and its siblings
  extend the slot comparison that drives `bindingUpdateMask` from "same pointer" to "same pointer and same
  generation".
- *Vulkan.* A version is one `vkAllocateDescriptorSets` from the transient pool list plus one
  `vkUpdateDescriptorSets`, written by the same code path `createBindingSet` uses. Volatile constant
  buffers use dynamic offsets from the version's snapshot, so `bindBindingSets` reads the version's list
  rather than `BindingSet::volatileConstantBuffers`. The rebind condition in `setGraphicsState` includes
  the generation.
- *D3D11.* The command list reads the canonical content at bind time and issues the
  `*SetShaderResources` calls as for a persistent set.

Per-version cost: one descriptor creation per binding plus a bump on D3D12; one set allocation and one
update on Vulkan; one `AddRef` per resource on both.

**B.5.5 Validation layer.**

- `createBindingSet` with `isDynamic` applies the same layout-coverage checks as today and rejects
  bindless layouts.
- `writeItem` is checked against the layout: slot, type and array element must exist, and the resource
  type must match the layout item type, with the volatile and non-volatile constant buffer rule unchanged.
- `write` applies the full coverage check.
- Per command list, the layer counts versions per dynamic set and warns once past
  `c_DynamicBindingSetVersionWarning` (4096), pointing at volatile constant buffers and push constants
  for per-draw constants.
- Arena exhaustion on D3D12 is an error that names the `DeviceDesc` field, after Diligent's
  `GPUDescriptorHeapDynamicSize` message.

**B.5.6 Usage guidance.**

- Keep dynamic sets small; every version pays per binding.
- Prefer persistent sets for samplers. The D3D12 sampler heap is capped at 2048 descriptors for every use
  in the process, and transient sampler versions compete with persistent sets for it.
- Constants that change per draw belong in volatile constant buffers or push constants, not in a dynamic
  set holding a constant buffer binding.
- A pass whose inputs the FrameGraph rotates holds one dynamic set per layout and writes it in `execute`;
  it then has no `BindingCache` traffic at all.

### B.6 Consequences

Positive:

- Per-frame and per-draw resource changes no longer create binding sets, pools or `BindingCache` entries.
  The FrameGraph's pooled and rotated resources, TAA history and SSAO sets become one write per frame.
- No layout, pipeline, root signature or shader change. Existing code is unaffected: `isDynamic` defaults
  to false and no released vtable changes.
- Liveness and state tracking are preserved, which descriptor tables never had.
- Range allocation is O(1) with a storage report; Vulkan pool count drops from one per set to a handful.
- The transient path reuses nvrhi's deferred-release mechanism instead of adding a fence queue.

Negative:

- A second code path for descriptor creation on D3D12 and Vulkan unless `createDescriptors` is factored
  to write into a caller-supplied destination first, which B.5.4 requires.
- The write-then-bind protocol is one more rule, next to write-before-bind for volatile constant buffers.
- Owners of descriptor ranges store an `Allocation` instead of a bare index (B.2).
- On D3D11 the dynamic path exists for portability only.

Risks:

- Heap pressure. Transient chunks live as long as the slowest in-flight command list; many versions or
  many command lists in flight grow the shader-visible heap, which is where B.1 bites. The version-count
  warning and the chunk-size knobs are the mitigation.
- Concurrency misuse. Writing a set that another thread is binding is undefined, and the validation layer
  cannot detect it cheaply.
- Sampler heap exhaustion on D3D12 if dynamic sets carry samplers; mitigated by guidance and the smaller
  sampler chunk.
- B.1 is unconfirmed as a live bug; if the old heap is already kept alive by some path not traced here,
  B.1 reduces to the test.

### B.7 Alternatives considered

- **Frequency enum on the layout (static, mutable, dynamic), the earlier draft.** Static and mutable
  changed no behaviour: unchanged sets are already skipped at draw time and immutable per-object sets are
  already the default. A layout-level dynamic flag also forced every set of that layout to be transient
  and touched every pipeline using it. The flag on the set keeps the capability and drops the ceremony.
- **Per-item dynamic variables inside one layout, as Diligent does.** Needs a second descriptor set per
  layout on Vulkan, which needs shader binding remapping that nvrhi does not do.
- **`ICommandList::writeBindingSet`.** A vtable change, so a new `ICommandList1` or `ICommandList2`
  colliding with the `framegraph` branch, and it would scope content to one command list like a volatile
  buffer, making a set unusable until written in every command list.
- **Descriptor tables for regular layouts.** No liveness or state tracking; in-place writes race with
  in-flight GPU work unless `UPDATE_AFTER_BIND` is used, which has device limits and excludes uniform
  buffers on some implementations.
- **Keep `BindingCache` and pre-warm (RdgDesign 6.1, option D).** Does not remove descriptor churn on
  the first frame after a change and requires every adapter to know its bindings ahead of `execute`.
- **A reserved dynamic region of the shader-visible heap, as Diligent does.** Interacts with `Grow`,
  which copies and re-bases the heap; ordinary ranges survive the copy for free.
- **Diligent's map-based allocator instead of OffsetAllocator.** Exact fit and alignment handling, at
  O(log n) with two ordered-map operations per call and node allocation per free block. Descriptor
  indices need no alignment and the heaps are small, so the simpler O(1) allocator wins; the map-based one
  remains the choice if a byte-granular memory heap with alignment ever needs one.

### B.8 Implementation order and verification

1. **B.1 test, then fix.** Grow the SRV heap while a command list that bound descriptors from it is
   executing; park the old heap on the lifetime tracker; rerun.
2. **B.2.** OffsetAllocator into `src/common` (MIT, with the licence text kept), `StaticDescriptorHeap`
   on top of it, owners store `Allocation`, `maxAllocs` knob, storage report at device destruction.
   Verify with the existing samples under `--debug` and the nvrhi tests; no behaviour change expected.
3. **B.3.** Vulkan pool manager; `createBindingSet` and `~BindingSet` on it. Verify as above.
4. **B.4 and B.5 header.** `BindingSetDesc::isDynamic`, `IDynamicBindingSet`, `c_HeaderVersion` bump,
   `utils::CreateDynamicBindingSet`, the `DeviceDesc` knobs in `d3d12.h` and `vulkan.h`.
5. **B.5 D3D12.** Factor `createDescriptors`; arena, version object, generation-aware slot comparison in
   `setGraphicsState`, `setComputeState`, `setMeshletState`, `setRayTracingState`;
   `setResourceStatesForBindingSet` over a desc.
6. **B.5 Vulkan**, then **D3D11**.
7. **Validation layer** rules from B.5.5.
8. **Tests.** A device test on all three backends that writes a dynamic set between draws and reads back
   the result; a liveness test that releases a resource after writing it out of the set while a version is
   in flight; the growth test from step 1. Next to `tests/qi-device.cpp` if that harness creates real
   devices, otherwise in donut `tests/`.
9. **donut.** Convert the TAA history and SSAO adapters on the `framegraph` branch after its Phase 0
   merge; `BindingCache` is unchanged.
10. **Docs.** `ProgrammingGuide.md`, section "Binding Layouts and Sets": a "Dynamic binding sets"
    paragraph with the write-then-bind rule. This document's status to Accepted, and an ADR recording
    B.5's interface decision once accepted.

Verification across the steps: Release build of the aggregate for D3D11, D3D12 and Vulkan; the tests
above; donut samples under `--debug`; an A/B of one FrameGraph sample comparing binding sets created per
frame, descriptor allocations per frame and frame time before and after; a RenderDoc capture confirming
persistent sets produce identical root signatures and descriptor sets.

## References

- nvrhi: `include/nvrhi/nvrhi.h` (`BindingSetDesc`, `IBindingSet`, `IDescriptorTable`, `GraphicsState`,
  the `static_assert` block on struct layouts); `include/nvrhi/d3d12.h` and `vulkan.h` (`DeviceDesc`);
  `src/d3d12/d3d12-descriptor-heap.cpp` (`StaticDescriptorHeap::Grow`, `allocateDescriptors`,
  `releaseDescriptors`); `src/d3d12/d3d12-resource-bindings.cpp` (`BindingSet::createDescriptors`,
  `CommandList::setComputeBindings`, `resizeDescriptorTable`); `src/d3d12/d3d12-state-tracking.cpp`
  (`setResourceStatesForBindingSet`); `src/d3d12/d3d12-commandlist.cpp` (`commitDescriptorHeaps`);
  `src/d3d12/d3d12-upload.cpp` and `src/d3d12/d3d12-backend.h` (`UploadManager`, `BufferChunk`,
  `CommandListInstance::referencedResources`); `src/vulkan/vulkan-resource-bindings.cpp`
  (`Device::createBindingSet`, `CommandList::bindBindingSets`); `src/vulkan/vulkan-backend.h`
  (`TrackedCommandBuffer`, `BindingSet`); `src/validation/validation-device.cpp`
  (`DeviceWrapper::createBindingSet`); `src/common/bitset-allocator.h`, `src/common/utils.cpp`;
  `include/nvrhi/core/autoptr.h`; `doc/com-interface-abi.md`.
- donut: `doc/RdgDesign.md`, sections 3.3 and 6.1; `src/render/framegraph/FrameGraphAliasing.cpp` on
  the `framegraph` branch.
- Diligent Engine: `Graphics/GraphicsEngineD3D12/include/DescriptorHeap.hpp` and `src/DescriptorHeap.cpp`
  (`GPUDescriptorHeap`, `DynamicSuballocationsManager`, `CPUDescriptorHeap`); `include/D3D12DynamicHeap.hpp`;
  `include/RootParamsManager.hpp`; `src/PipelineResourceSignatureD3D12Impl.cpp` (`CommitRootTables`);
  `src/DeviceContextD3D12Impl.cpp`; `Graphics/GraphicsEngineVulkan/include/DescriptorPoolManager.hpp` and
  `src/DescriptorPoolManager.cpp` (`DescriptorSetAllocator`, `DynamicDescriptorSetAllocator`);
  `include/VulkanDynamicHeap.hpp`; `include/PipelineResourceSignatureVkImpl.hpp`;
  `Graphics/GraphicsAccessories/interface/VariableSizeAllocationsManager.hpp` and
  `VariableSizeGPUAllocationsManager.hpp`; `Graphics/GraphicsEngine/interface/GraphicsTypes.h`
  (`EngineD3D12CreateInfo` heap sizes); `doc/PerformanceGuide.md`.
- slang-rhi `92932ae`: `src/core/offset-allocator.{h,cpp}`, `tests/test-offset-allocator.cpp`,
  `src/heap.{h,cpp}`, `src/vulkan/vk-heap.{h,cpp}`, `src/cuda/cuda-heap.{h,cpp}`, `src/cuda/cuda-buffer.cpp`,
  `src/cuda/cuda-constant-buffer-pool.cpp`, `src/d3d12/d3d12-descriptor-heap.{h,cpp}`,
  `src/d3d12/d3d12-device.cpp`, `src/d3d12/d3d12-command.cpp`, `include/slang-rhi.h` (`IHeap`, `HeapAlloc`).
- OffsetAllocator upstream: Sebastian Aaltonen, MIT licence, 2023.
