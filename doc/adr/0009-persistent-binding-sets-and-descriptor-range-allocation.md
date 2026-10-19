# 0009. Persistent binding sets; OffsetAllocator descriptor ranges

- Status: Proposed
- Date: 2026-10-16
- Supersedes: [ADR 0008](0008-binding-frequencies-and-descriptor-allocation.md) (revisions 1 and 2)
- Plan: [`../plans/2026-10-16-persistent-binding-sets.md`](../plans/2026-10-16-persistent-binding-sets.md)

## Context

ADR 0008 revision 2 made every binding set non-persistent: a set held CPU descriptors only, and each command list copied
it into the GPU-visible heap at its first bind. Its plan measured what that costs (`nvrhi_bench_set_binding`, the
Asteroids benchmark, nv_asteroids), on an RTX 4050 laptop GPU and a Radeon 780M integrated GPU:

| Measurement | Result |
| --- | --- |
| First bind of a set in a command list, D3D12 | about 0.3 us (RTX 4050), 0.85 us (Radeon 780M), on top of a bind of a persistent set |
| Asteroids, one set per asteroid (50,000), D3D12 | 10-12 ms CPU per frame before, 30-51 ms after |
| Asteroids, ten sets, and nv_asteroids | within a few percent |

The copy writes into write-combined memory (the shader-visible heap), drained at the next locked instruction. The
vendors say the same in general terms: NVIDIA "keep descriptors around persistently instead of re-allocating or
copying them in each frame"; AMD "avoid copying descriptors"; on Vulkan, Khronos, Arm and NVIDIA recommend cached,
reused descriptor sets with dynamic offsets for per-draw buffers. A per-set flag that kept both models (persistent by
default, copy at bind on request, with a Vulkan descriptor-buffer path) was drafted and dropped as too complex for what
it buys.

So binding sets go back to `ethereal-dev`'s model. Of revision 2, one part is worth keeping on its own: D3D12
`StaticDescriptorHeap` finds a range by a first-fit scan over a `vector<bool>`, linear in the heap size, with no report
of how full the heap is.

`ethereal-dev`'s heap growth (a full heap is replaced by a larger one and the old one copied into it) has known
thread-safety hazards: readers of the heap pointer and start handles take no lock while `Grow` replaces them. They are
older than this ADR and are left to a separate, systemic solution; this ADR does not change growth.

## Decision

### B.1 Binding sets as on `ethereal-dev`

No change to the binding API: no frequencies, no persistence flag, no writable sets, no commit call.

- D3D12: a binding set owns ranges of the shader-visible CBV/SRV/UAV and sampler heaps, written into their CPU
  staging halves at `createBindingSet` and copied to the shader-visible halves; `set*State` points the root tables at
  them. Descriptor tables live in the same heaps.
- Vulkan: every binding set owns a `VkDescriptorSet` from a pool of its own, written at `createBindingSet`; volatile
  constant buffers are dynamic offsets.

### B.2 Range allocation

Every D3D12 descriptor heap allocates ranges with Sebastian Aaltonen's OffsetAllocator (MIT) in O(1). The allocator
has one node per descriptor plus two, the most a heap of that size can use, so it never runs out of nodes and needs no
setting (about 32 bytes of CPU memory per descriptor). Owners keep the range they were given and release it whole; the
index-based `IDescriptorHeap` methods find the range by its base index, and a release of part of a range is an error.
`~Device` reports per heap its size, peak use and largest free block at Info.

### B.3 Heap growth unchanged

`Grow` keeps `ethereal-dev`'s behaviour: when no range fits, the heap is replaced by one of the next power of two in
size (shader-visible heaps up to the D3D12 limits), the old CPU heap is copied into the new CPU and shader-visible
heaps, and the old shader-visible heap lives on only in the command lists that bound it. The live ranges are replayed
into a larger allocator, which keeps every range at its index. The `DeviceDesc` heap sizes and their defaults do not
change, and neither does `c_HeaderVersion`.

### B.4 A fix carried over

A sampler descriptor table is bound with a GPU handle of the sampler heap; `ethereal-dev` bound it with a handle of the
CBV/SRV/UAV heap at the same index.

## Consequences

### Positive

- Binding costs what it costs on `ethereal-dev` on both APIs; no copy at bind.
- Range allocation is O(1), never runs out of nodes, and reports how full each heap is.
- No public API or default changes.

### Negative

- Every live set occupies the shader-visible heaps for its whole life, including the 2048-entry sampler heap.
- `IDescriptorHeap::releaseDescriptors` releases whole ranges only; `ethereal-dev` accepted any subrange.
- The allocator holds about 32 bytes of CPU memory per descriptor, growing with the heap.

### Risks

- The growth hazards of `ethereal-dev` remain until their systemic solution.

## Alternatives considered

- **ADR 0008 revision 2: every set non-persistent.** Measured: up to 3-4x CPU time on D3D12 for sets bound once per
  draw, against the vendors' guidance.
- **ADR 0008 revision 1: binding frequencies on writable sets.** Superseded before revision 2.
- **A `persistent` flag per set, non-persistent sets copied at bind, Vulkan descriptor buffers opt-in.** Drafted on
  2026-10-16 and dropped as too complex.
- **Fixed-size heaps, created once and never grown.** Drafted and implemented on 2026-10-16, then withdrawn: heap
  growth and its hazards belong to the systemic solution, not to this ADR. The commits were never pushed.
- **Revision 2's parking of replaced heaps until the GPU is done with them.** Command lists already hold the heaps they
  bound (`referencedNativeResources`), as on `ethereal-dev`.
- **A node budget for the allocator (`maxDescriptorAllocations`, revision 2).** Saves CPU memory, but the budget has to
  grow by itself or fail scenes with many sets (revision 2's Asteroids `mut`); the exact bound needs no setting.

## References

- ADR 0008 and its plans [`../plans/2026-10-13-binding-frequencies.md`](../plans/2026-10-13-binding-frequencies.md) and
  [`../plans/2026-10-15-immutable-binding-sets.md`](../plans/2026-10-15-immutable-binding-sets.md) (measurements,
  section 5); their commits are under the tags `binding-freq-rev1` and `binding-freq-rev2`.
- NVIDIA, "Advanced API Performance: Descriptors"; AMD, "RDNA Performance Guide"; Khronos, "Descriptor and buffer
  management" sample.
- Sebastian Aaltonen, OffsetAllocator, <https://github.com/sebbbi/OffsetAllocator>.
