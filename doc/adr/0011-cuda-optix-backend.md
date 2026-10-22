# 0011. A CUDA and OptiX backend: deferred command lists replayed on one stream per queue

- Status: Accepted
- Date: 2026-10-22
- Plan: [`../plans/2026-10-22-slang-cuda-optix-backend.md`](../plans/2026-10-22-slang-cuda-optix-backend.md), steps 3, 5-7

## Context

The GVDB samples (`ethereal-samples/src/gvdb_samples`) ran their compute work on a private COM layer, `gp`. It wraps
the CUDA driver API and OptiX 9, and runs about 140 CUDA C++ kernels and OptiX programs. The work moves that layer
into NVRHI as a backend, in the manner of slang-rhi, and has it run Slang code compiled offline
([ADR 0012](0012-offline-slang-shader-container-and-reflection.md)). The user fixed the following:

- The backend uses the CUDA driver API, not the runtime, and OptiX SDK 9.0 or newer.
- A CUDA stream maps to `ICommandList`, a kernel to `IShader`, and a kernel launcher to `IComputePipeline`. Copies get
  a transient heap. OptiX programs map to `IShader` / `IShaderLibrary`, and the OptiX pipeline to `rt::IPipeline`.
- Command lists execute the way slang-rhi's CUDA backend executes them. In `src/cuda/cuda-command.cpp`,
  `CommandEncoderImpl` records into a `CommandList`, and `CommandQueueImpl::submit` replays it with a
  `CommandExecutor` on the queue's `CUstream`. Each submission records a `CUevent`; `cuEventQuery` retires it.

## Decision

The backend is `src/cuda` (the `nvrhi_cuda` library, under `NVRHI_WITH_CUDA` / `NVRHI_WITH_OPTIX`). Its public header,
`include/nvrhi/cuda.h`, adds `cuda::IDevice`, `cuda::ICommandList`, `cuda::INativeWork` and `cuda::DeviceDesc`;
`IGraphicsFactory::createCudaDevice` creates the device ([ADR 0010](0010-graphics-factory.md)).
`GraphicsAPI::CUDA` is appended.

**Driver.** The driver is loaded at run time: `nvcuda.dll` (or `libcuda.so.1`), then `cuGetProcAddress_v2` for every
function, at the CUDA version of the headers (`src/cuda/cuda-driver.h`). A build with the backend still starts on a
machine without the driver; `isBackendAvailable(CUDA)` is false there. The CUDA toolkit (12.0 or newer) is needed for
its headers only. OptiX is headers only plus `optixInit()`. Its function table is renamed and its stubs are static
(`OPTIX_ENABLE_SDK_MIXING`), so NVRHI never clashes with an application that uses OptiX itself.

**Device.**
- The device uses the device's primary context (`cuDevicePrimaryCtxRetain`), which CUDA libraries share.
- It is selected by matching the adapter of `DeviceDesc::graphicsDevice`: the LUID for D3D11/D3D12, and the device
  UUID or LUID for Vulkan. Without a graphics device it uses `deviceOrdinal`.
- Every call into CUDA runs inside a `ContextScope`, which pushes and pops the context, so any thread may call into the
  device.

**Queues and command lists.**
- **Streams.** Each `CommandQueue` is one non-blocking `CUstream`.
- **Recording.** A command list records plain records into a byte stream. Everything is resolved while it records:
  - upload data is copied into transient memory;
  - the `SLANG_globalParams` block of a dispatch is written from the bound binding sets;
  - push constants become the kernel argument block.
- **Execution.** `executeCommandLists` replays the records on the queue's stream (`cuda-executor.cpp`), records a
  `CUevent`, and returns the submission number.
- **Retirement.** The submission keeps its records, resources and transient memory until the event has signalled.
  `runGarbageCollection` and `queueGetCompletedInstance` retire it.
- **Cross-queue waits.** `queueWaitForCommandList` is `cuStreamWaitEvent`.
- **Ordering.** Work on one stream executes in order, so barriers and resource states are no-ops.
- **One host stall.** An indirect dispatch reads its group counts back before it launches, as slang-rhi does.
- **Native work.** `cuda::ICommandList::enqueueNativeWork(INativeWork*)` hands the `CUstream` to application code
  during replay (CUDA libraries, hand-written launches).

**Transient heap** (`cuda-transient-heap.cpp`):
- There are two kinds of chunk: pinned upload memory, also mapped to the device, and device scratch memory.
- A recording bump-allocates in chunks it takes from the device's pool, and returns them when its submission retires.
- This is the upload manager of the D3D12 and Vulkan backends, with the device holding the pool.

**Kernels.**
- **Libraries and shaders.** An `IShaderLibrary` holds the PTX of a container (or plain PTX). An `IShader` is one entry
  point, with its symbol name and reflection.
- **Modules per queue.** Each queue loads its own `CUmodule` of a library on first use, and with it its own
  `SLANG_globalParams`, so the queues never race on that constant.
- **Compute pipelines.** An `IComputePipeline` is the launcher. It holds the function, the block size from
  `numthreads`, and a map from HLSL registers to offsets in the global block.
- **Binding sets.** Binding layouts and sets keep their descriptions only. A binding set item is placed by register
  class, register space and slot through the reflection.
- **Values written into the block.**
  - Constant buffers: their address (a version in upload memory for volatile buffers).
  - Textures: `CUtexObject`.
  - Writable textures: the `CUsurfObject` of the mip level.
  - Structured, typed and raw buffers: `{address, count}`.
  - Acceleration structures: the traversable handle, written at replay.
- **Samplers.** A texture is sampled with the `Sampler` bound at the same slot (`s<n>` with `t<n>`). CUDA has no
  separate samplers, so texture objects are cached per view and sampler.

**Ray tracing.**
- **Modules and program groups.** `rt::IPipeline` creates one `OptixModule` per library, with the pipeline compile
  options from `rt::PipelineDesc`. The launch parameters are `SLANG_globalParams`, so Slang's global block is the
  launch-parameter block. It also creates one program group per shader or hit group, and sets the stack sizes.
- **Shader table.** `rt::IShaderTable` is written into transient memory when a dispatch is recorded. Each record is the
  program group header followed by the entry point's parameters. A local binding set fills those parameters, matched by
  register class and slot.
- **Acceleration structures.** Builds are recorded and run on the stream, with no host synchronization. OptiX returns
  a traversable handle only when a build is enqueued, so TLAS instances and bindings get the handles at replay
  (`PatchTraversable`).
- **Compaction.** It reads the size the build emitted, after the build's submission has finished.
- **Not supported.** `buildTopLevelAccelStructFromBuffer`: OptiX instances hold handles, not the BLAS addresses of
  `rt::InstanceDesc`. Copying acceleration structures is not supported either.

**Not supported** (`FE_UNSUPPORTED`): graphics and meshlet pipelines, framebuffers, input layouts, heaps and virtual
resources, bindless layouts and descriptor tables, sampler feedback, opacity micromaps, clusters and cooperative
vectors.

## Consequences

- **Tests.** `tests/cuda-tests.cpp` covers:
  - QueryInterface identities;
  - copies of every texture shape;
  - transient memory reused while 160 submissions are in flight;
  - queues and queries;
  - a Slang kernel that uses every binding kind;
  - an OptiX scene with a triangle and an AABB instance.
- **Slang limitation.** Slang has no `ReportHit` for OptiX. An intersection program calls `optixReportIntersection`
  through `__target_switch` (see `tests/shaders/cuda-optix.slang`).
- **Formats.** CUDA arrays have no 3-component or depth-stencil formats, and no swizzle (BGRA reads in memory order).
  Surface writes need the stored element type (`RWTexture2D<float4>` on an RGBA32_FLOAT texture).
- **Indirect dispatch** stalls the host.

## Alternatives

- **Immediate command lists** (the D3D11 model, each call issued to CUDA at once). They are simpler, but command lists
  could not be recorded ahead of submission or on other threads, and the user asked for slang-rhi's model.
- **One module per pipeline instead of per queue.** This still races when one pipeline runs on two queues.
- **The CUDA runtime API.** It would tie NVRHI to a `cudart` version, and its primary-context and stream semantics are
  implicit.
