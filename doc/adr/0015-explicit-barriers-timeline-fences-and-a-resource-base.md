# 0015. Explicit barriers, timeline fences and a resource base

- Status: Accepted
- Date: 2026-11-05
- Design: aggregate `docs/rdg-design.md`, section 8 (accepted 2026-11-05), decided by aggregate ADR 0003
- Plan: aggregate `docs/plans/2026-11-05-render-graph.md`, step P1.1

## Context

donut's new render graph (aggregate ADR 0003) computes every barrier itself: per subresource and per
plane, with merged read states, split barriers, aliasing barriers, discards and queue-ownership
transfers, placed on the queue that can perform them. It submits its segments with fence waits and
signals between queues, and retires frames on the same fences.

NVRHI's barrier surface is the per-command-list state tracker (`setTextureState`, `setBufferState`,
`commitBarriers`). The tracker works per mip × slice, never tracks planes, never merges read states,
and has no split, aliasing, discard or ownership barriers. The only cross-queue primitive is
`queueWaitForCommandList`; Vulkan adds semaphore waits that are queued onto the next submission. There
is no public fence object, and resources have no common base, so a barrier cannot name "a resource".

## Decision

The RHI is extended in place, under the version policy of ADR 0012 (`c_HeaderVersion` stays 28):

1. **`IResource`**, a new base under `IBuffer`, `ITexture`, `IStagingTexture`,
   `ISamplerFeedbackTexture`, `rt::IAccelStruct` and `rt::IOpacityMicromap`. It has one method,
   `queryHeapBinding(HeapBinding&)`: the heap and byte range a virtual resource is bound to, or
   `FE_NOT_FOUND`. It carries no type tag. A backend reaches the derived interface with
   `QueryInterface(riid, nullptr)`, which reports support without adding a reference (ADR 0006), then
   `checked_cast`.
2. **`SubresourceRegion`**: mips, slices and a Color/Depth/Stencil `PlaneMask`. It is separate from
   `TextureSubresourceSet`, which keeps its 16-byte layout inside `BindingSetItem` and
   `FramebufferAttachment`.
3. **`ResourceBarrier`**, a D3D12-style union of transition, UAV and aliasing barriers, with
   `SplitBegin`, `SplitEnd`, `Discard`, `QueueRelease` and `QueueAcquire` flags, and
   **`ICommandList::barriers(const ResourceBarrier*, uint32_t)`**, which records exactly those barriers
   in order. Pending tracker barriers are committed first. Resources the list tracks follow each
   barrier's `after` state; untracked resources stay untracked. The helpers and the layout assertions
   live in `src/common/resource-barrier.cpp`.
4. **`ICommandList::setExplicitBarrierScope(const char* owner)`**, a validation aid. While a scope is
   open, the validation layer reports every barrier-changing call as an error naming `owner`. The
   backends implement it as an empty call.
5. **`IFence`**, a timeline fence (`getCompletedValue`, `wait`, `signal`), with
   **`IDevice::createFence`, `submit(const SubmitDesc&)`, `queueWaitFence`, `queueSignalFence` and
   `waitFences`**. `submit` carries fence waits and signals in one native submission. CPU waits block
   until "at least" the value: a zero timeout polls, a finite timeout returns `FE_WAIT_TIMEOUT`, and an
   infinite one blocks until the value is reached or the device is lost.
6. **New `Feature` values**: `SplitBarriers`, `AliasingBarriers` and `QueueOwnershipTransfer`. They
   tell a caller which barrier flags have a native effect; the call succeeds either way.
7. Vulkan `bindImageMemory` and `bindBufferMemory` check the `VkResult`.

Per backend:

| Backend | `barriers` | `IFence` and `submit` |
| --- | --- | --- |
| D3D12 legacy | one `ResourceBarrier` call per batch: `BEGIN_ONLY`/`END_ONLY` splits, per-plane subresource indices, `DiscardResource` after a discard transition, aliasing barriers | `ID3D12Fence`; `queue->Wait`, `ExecuteCommandLists`, `queue->Signal` |
| D3D12 enhanced | one `Barrier` call with texture, buffer and global groups; combined read layouts mapped; `AccessBefore` always the real access | as legacy |
| Vulkan | one `vkCmdPipelineBarrier2`; events for splits; aspect masks for planes; real queue-family indices for release/acquire; ends dynamic rendering | timeline `VkSemaphore`; waits and signals in one `vkQueueSubmit2` |
| D3D11 | validation, then nothing; UAV barriers inside relaxed runs map to NVAPI overlap | `ID3D11Fence` from D3D11.4, else `FE_UNSUPPORTED` |
| CUDA | validation, then nothing (one stream executes in order) | a ring of (value, `CUevent`) per fence; a GPU wait must be enqueued after its signal |

The full interface text, the validation rules and the CPU-wait table are in design section 8.

## Consequences

- `ICommandList` (and `d3d12::`, `cuda::ICommandList`), `IDevice` (and `d3d12::`, `vulkan::`,
  `cuda::IDevice`) and every resource interface change their virtual tables. Everything is rebuilt
  together, as ADR 0012 already requires; `verifyHeaderVersion` cannot detect a mismatch.
- `executeCommandLists` and `queueWaitForCommandList` remain, reimplemented on the fence machinery.
- Graph code and other explicit-barrier users never touch the tracker; the tracker stays as the
  fallback and for code that relies on automatic barriers.
- New tests: `nvrhi_barrier_tests` and `nvrhi_fence_tests`, per backend.

## Alternatives

- **A separate `IBarrierCommandList` and a header-version bump.** Rejected in the design review in
  favour of extending `ICommandList` in place.
- **Binary event objects for cross-queue sync.** Rejected: D3D12 has no GPU event, a `VkEvent` cannot
  cross queues, binary events need reset bookkeeping, and timeline values give transitive reduction
  and frame retirement for free.
- **A type tag on `IResource` (`getResourceType`).** Rejected in review in favour of `QueryInterface`.
- **A plane mask on `TextureSubresourceSet`.** Rejected: it would grow `BindingSetItem` from 40 to 48
  bytes and move `FramebufferAttachment` members.
