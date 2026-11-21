# 0016. Command-queue objects and CUDA queue pools

- Status: Accepted
- Date: 2026-11-21
- Amends: [0011](0011-cuda-optix-backend.md) (one stream per queue), [0015](0015-explicit-barriers-timeline-fences-and-a-resource-base.md) (`submit`)
- Plan: aggregate `docs/plans/2026-11-21-cuda-queue-pools.md`

## Context

The CUDA backend gives each `CommandQueue` type one non-blocking `CUstream` (ADR 0011). That is three
streams in all.
- A command list is replayed at submit onto the stream of its `queueType`.
- Each queue loads its own `CUmodule`, so `SLANG_globalParams` never race between streams.
- Cross-stream order comes from `cuEvent`s: the `IFence` event ring and `submit`'s waits and signals
  (ADR 0015).

A CUDA device can run many streams at once. One stream per type, though, serialises every pair of
same-type workloads, even independent ones such as two async compute chains of the render graph. The
public API names a queue only by its type, so an application cannot address a second stream.

## Decision

1. **`nvrhi::ICommandQueue`** (an `IRHIObject`) is one queue of the device: a `CUstream`, an
   `ID3D12CommandQueue` or a `VkQueue`.
   - It reports its type, its index in its type's pool, and its last submitted and completed instances.
   - `IDevice::getCommandQueueCount(type)` and `getCommandQueue(type, index)` enumerate them. Index 0 is
     the type's primary queue.
   - D3D11, D3D12 and Vulkan have one queue per type.
2. **CUDA queue pools.** `cuda::DeviceDesc::queueCounts[type]` takes 1 to 8 streams per type, and
   defaults to 1. With more than one, the queues of a type run concurrently.
3. **Binding at `open()`.** Every recording is bound to one queue of its type.
   - `CommandListParameters::commandQueue` pins it.
   - Otherwise `open()` picks a free queue: fewest recordings bound and not yet submitted, then fewest
     submissions in flight, then lowest index.
   - `ICommandList::getCommandQueue()` reports the binding.
   - CUDA records against the bound stream's module. Modules and kernels are loaded per stream, lazily.
4. **Submission.** A submission runs its lists on their bound queues.
   - If those are several queues of one type, it forks: each queue waits, then runs its lists.
   - It joins on the first list's queue, which records the signals.
   - `SubmitDesc::commandQueue` names the queue of a submission without lists.
   - Instances of a queue with index i > 0 carry i in bits 56-59, so `queueWaitForCommandList` finds
     the queue. Primary-queue instances are unchanged.
5. **Type-named calls act on the primary queue:** `queueWaitForCommandList` (on its waiting side),
   `queueWaitFence`, `queueSignalFence`, `getNativeQueue`, `setEventQuery`, the interop waits and the
   swap chain.

## Consequences

- With every count at 1, behaviour, instance numbers and the native streams are those of ADR 0011.
- With pools, same-type lists that are not pinned to one queue are ordered only by fences. The render
  graph pins its segments to lanes and waits across lanes with fences.
- The modules are loaded per stream: memory grows with the pool sizes, on first use.
- Every backend implements `ICommandQueue` (one per type outside CUDA). The validation layer wraps the
  objects and checks pinned and submitted queue types.

## Alternatives considered

- **Pool indices without an object** (`CommandListParameters::queueIndex`, `ICommandList::getQueueIndex`).
  Rejected for the object, which carries the native stream and the instance counters.
- **Explicit acquisition** of a queue by the application. Rejected for binding at `open()`, which needs
  no pool management in callers.
- **One module per pipeline, shared by streams.** Rejected as in ADR 0011: concurrent launches race on
  `SLANG_globalParams`.
- **`cuStreamWaitValue64` timelines.** These are not needed: the event ring orders any pair of streams.
  The WDDM deadlock of a wait enqueued before its write (aggregate plan 2026-11-05, P1.1) still rules
  them out.

## References

- ADR 0011, ADR 0015; aggregate `docs/rdg-design.md` 6.8, 8.3, 9.2.
- Plan: aggregate `docs/plans/2026-11-21-cuda-queue-pools.md`.
