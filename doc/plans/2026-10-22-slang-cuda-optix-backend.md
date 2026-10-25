# CUDA and OptiX backend for nvrhi with offline Slang reflection

- Status: Approved
- Date: 2026-10-22
- Branch: `slang` in the aggregate, `ethereal-nvrhi`, `ethereal-donut`, `ethereal-donut/thirdparty/shader-tool` and `ethereal-samples`
- Archived at `docs/plans/2026-10-22-slang-cuda-optix-backend.md`. The maintained copy, with the *Step N as built* notes, is in `ethereal-nvrhi/doc/plans/`.

## Context

nvrhi drives only D3D11, D3D12 and Vulkan. The GVDB samples (`ethereal-samples/src/gvdb_samples`) do their compute on a private COM layer called `gp` (`gvdb/GPDevice*.h`):
- a CUDA driver-API device;
- an OptiX 9 device (`GPDeviceOptiX*`);
- nvrhi interop (`GPDeviceNVRHI.*`);
- 127 `.cu` kernels and 12 OptiX programs, compiled to PTX by nvcc, plus Thrust.

This plan:
- adds a `IGraphicsFactory` that becomes the only way to create devices and validation layers;
- moves the `gp` layer into nvrhi as a first-class CUDA + OptiX backend, in the style of slang-rhi;
- feeds that backend with Slang code compiled offline;
- converts the GVDB samples to Slang on the new backend and deletes all CUDA C++ from them.

`gp` is the **reference implementation**. It shows the expected behaviour, and also bugs that this work fixes (see "Reference defects: fixed in this work" below). The `slang-next` branches are **not** a reference and will be removed outside this plan. This plan reuses some of their names (`NVRHI_WITH_CUDA`, `GraphicsAPI::CUDA`, `src/cuda/`), so `slang-next` must never be merged into `slang`.

### Requirements and decisions

| # | Requirement | Decision |
|---|---|---|
| R1 | CUDA driver API, OptiX SDK 9.0 or later | The driver is loaded at runtime: `nvcuda.dll` / `libcuda.so` plus `cuGetProcAddress`. A CUDA-enabled nvrhi therefore still loads on machines without NVIDIA hardware. Building needs the headers from CUDAToolkit 12.0 or later. OptiX is header-only plus `optixInit()`, requires `OPTIX_VERSION >= 90000`, and is found through `OptiX_INSTALL_DIR` / `cmake/FindOptiX.cmake`. |
| R2 | Mapping: CUDA stream to `ICommandList`, kernel to `IShader`, launcher to `IComputePipeline`, a transient heap for copies. OptiX programs to `IShader` / `IShaderLibrary`, pipeline to `rt::IPipeline` | Recording is deferred and replayed, as slang-rhi does in `src/cuda/cuda-command.cpp` (checked against the source):<br>• `CommandEncoderImpl` appends to a `CommandList`.<br>• `CommandQueueImpl::submit` runs a `CommandExecutor` on the queue's `CUstream`.<br>• Completion is tracked with a `CUevent` and a submission ID.<br>• Global parameters go into `SLANG_globalParams`; entry-point parameters go through `CU_LAUNCH_PARAM_BUFFER_POINTER`.<br>Here, each `nvrhi::CommandQueue` owns one non-blocking `CUstream`, and `executeCommandLists` replays the recorded lists on it. |
| R3 | Slang reflection on `IShaderLibrary` / `IShader`, with no `I*Program` interface | ShaderTool compiles `.slang` **offline** into an nvrhi **shader container** that holds the code and the serialized reflection. nvrhi parses it into its own ABI-safe structs. `slang.dll` is not needed at runtime. |
| R4 | One unified `IInteropDevice` that `IDevice` exposes through QueryInterface | The CUDA `Device` implements **both** `cuda::IDevice` and `IInteropDevice`. `IInteropDevice` is implemented by a mixin base, `InteropDevice`, with its own QueryInterface table, and `Device`'s table reaches it with **`NVRHI_IMPLEMENTS_ROUTE_PARENT(InteropDevice)`**. This is the `Multi : ObjectImpl<P1, P2>` pattern in `tests/core/test_qi_route.cpp:131-141`. There is one identity and one reference count. |
| R5 | Do not bump the API version; relax the compatibility restriction | `c_HeaderVersion` stays **28**. Existing vtables **may change**: methods are added to `IShaderLibrary`, `IShader` and `rt::IShaderTable`, `GraphicsAPI::CUDA` is appended, and the per-backend creation exports are removed. `doc/com-interface-abi.md` §3.4 (bump the version on any change; derive `IFoo1`) is amended. §1–§3.3, §4 and §7 still apply in full. |
| R6 | Device creation through a factory | Add `IGraphicsFactory : IObject`, created by one C export, `nvrhiCreateGraphicsFactory`. It creates the D3D11, D3D12, Vulkan and CUDA devices and the validation layer. `nvrhiD3D11CreateDevice`, `nvrhiD3D12CreateDevice`, `nvrhiVulkanCreateDevice` and `nvrhiCreateValidationLayer`, with their inline wrappers, are **removed**. |
| R7 | Development branch `slang` | Branched from `ethereal-dev` in nvrhi and donut, from `main` in the aggregate and the samples, and from shader-tool's current gitlink `afbf273`. nvrhi is currently checked out on `review/slang-next-pr`, which points at the same commit as `ethereal-dev`. |
| R8 | The GVDB samples are the test bed | Every `.cu`, `.cuh`, Thrust and `gp` file is deleted. Kernels and OptiX programs are rewritten in Slang. The library and the samples run on the nvrhi CUDA device and present through `IInteropDevice`. |
| R9 | Interop APIs | **D3D12 and Vulkan only.** D3D11 interop is dropped: D3D11 cannot report a resource's allocation size, so external-memory import of D3D11 resources cannot be correct. `IInteropDevice` returns `FE_UNSUPPORTED` when the paired device is D3D11. There is no keyed-mutex API. The GVDB and SPH samples refuse `-d3d11` with a clear message. |

## Architecture (detailed in the ADRs of step 1)

### Graphics factory (`include/nvrhi/factory.h`, `src/factory/`)

```cpp
namespace d3d11 { struct DeviceDesc; }  namespace d3d12 { struct DeviceDesc; struct IDevice; }
namespace vulkan { struct DeviceDesc; struct IDevice; }  namespace cuda { struct DeviceDesc; struct IDevice; }

struct GraphicsFactoryDesc { IMessageCallback* messageCallback = nullptr; };   // layout static_asserts

NVRHI_IID(IGraphicsFactory, "<fresh GUID>")
struct IGraphicsFactory : IObject
{
    NVRHI_DECLARE_UUID_TRAITS(IGraphicsFactory)
    virtual bool isBackendAvailable(GraphicsAPI api) const noexcept = 0;   // compiled in and loadable
    virtual FRESULT createD3D11Device(const d3d11::DeviceDesc& desc, IDevice** ppDevice) noexcept = 0;
    virtual FRESULT createD3D12Device(const d3d12::DeviceDesc& desc, d3d12::IDevice** ppDevice) noexcept = 0;
    virtual FRESULT createVulkanDevice(const vulkan::DeviceDesc& desc, vulkan::IDevice** ppDevice) noexcept = 0;
    virtual FRESULT createCudaDevice(const cuda::DeviceDesc& desc, cuda::IDevice** ppDevice) noexcept = 0;
    virtual FRESULT createValidationLayer(IDevice* underlyingDevice, IDevice** ppDevice) noexcept = 0;
};
typedef AutoPtr<IGraphicsFactory> GraphicsFactoryHandle;

NVRHI_C_API FRESULT nvrhiCreateGraphicsFactory(const GraphicsFactoryDesc* pDesc, IGraphicsFactory** ppFactory) noexcept;
inline GraphicsFactoryHandle createGraphicsFactory(const GraphicsFactoryDesc& desc) { /* call export, TakeOver */ }
```

- **Methods per backend, not overloads.** Overloads would break R3, and a `const void*` descriptor would lose type safety. A backend that is not compiled in returns `FE_UNSUPPORTED`.
- **Device descriptors stay where they are.** They remain in `d3d11.h`, `d3d12.h`, `vulkan.h` and `cuda.h`. Those headers lose their `*CreateDevice` exports and wrappers.
- **Where the factory is built.** In shared builds, `src/factory/factory.cpp` goes into `nvrhi.dll`. In static builds it is a new `nvrhi_factory` target that links the enabled backend targets. The Vulkan dynamic loader set-up moves into `createVulkanDevice`; it is currently in `nvrhiVulkanCreateDevice`, `src/vulkan/vulkan-device.cpp:36`.
- **Each backend's device factory becomes internal.** For example `nvrhi::d3d12::CreateDeviceInternal`, declared in a private header under `src/`.

### CUDA object mapping (`src/cuda`)

| nvrhi | CUDA / OptiX |
|---|---|
| `cuda::Device : ObjectImpl<cuda::IDevice, InteropDevice>` | Its QueryInterface table lists `cuda::IDevice`, `IDevice`, `IRHIObject`, the class ID, and `ROUTE_PARENT(InteropDevice)`.<br>It uses the primary context (`cuDevicePrimaryCtxRetain`). The device is chosen by LUID/UUID matching against `DeviceDesc::graphicsDevice` or an explicit adapter, never by "ordinal 0".<br>`getGraphicsAPI()` returns `CUDA`. |
| `CommandQueue` (Graphics / Compute / Copy) | One `CUstream` per queue (`CU_STREAM_NON_BLOCKING`) and one `CUevent` per submission instance.<br>`queueWaitForCommandList` uses `cuStreamWaitEvent`. `getNativeQueue(CUDA_Stream)` returns the stream. |
| `ICommandList` | A deferred command buffer that `CommandExecutor` replays at `executeCommandLists`.<br>`cuda::ICommandList::enqueueNativeWork(cuda::INativeWork* work)` keeps a reference to `work` and calls `work->execute(NativeObject stream)` during replay. `INativeWork` is a caller-implemented interface, as §3.3.6 requires for stateful callbacks. |
| `IBuffer` | `cuMemAlloc`; `getGpuVirtualAddress` returns the `CUdeviceptr`. `cpuAccess` buffers use mapped `cuMemHostAlloc`. |
| `ITexture` | `CUarray` / `CUmipmappedArray` created with `SURFACE_LDST`. One surface object per mip. Texture objects are cached per (view, sampler) pair. |
| `IShaderLibrary` / `IShader` (compute) | The PTX module (`cuModuleLoadData`) and the `CUfunction` of an entry point. |
| `IComputePipeline` | The launcher: the `CUfunction`, the thread-group size and parameter layout from reflection, and the `SLANG_globalParams` symbol (`cuModuleGetGlobal`). |
| `IBindingLayout` / `IBindingSet` | Each `(space, slot, type)` maps to a byte offset in the global-params block through the HLSL-to-CUDA cross-map in the reflection.<br>At replay, the block is packed into transient memory and copied to the symbol with `cuMemcpyAsync`. Push constants become entry-point parameters. |
| `IShaderLibrary` / `IShader` (OptiX) | PTX or OptiX-IR, and the entry points (`__raygen__…`). The `OptixModule` is created when the pipeline is created, because its compile options come from `rt::PipelineDesc`. |
| `rt::IPipeline` | Program groups and the `OptixPipeline`, with stack sizes from `optixUtil*`. The Slang global block is the launch-params block. |
| `rt::IShaderTable` | The SBT. Local binding sets are packed into record data, laid out by entry-point reflection. |
| `rt::IAccelStruct` | An `OptixTraversableHandle`. It is built by `ICommandList::build*AccelStruct` on the stream, with no host synchronization, and compacted by `compactBottomLevelAccelStructs`. |
| Event and timer queries | `CUevent`, with `cuEventElapsedTime` for timers. |

**Transient heap (`cuda-transient-heap.cpp`).** It follows `vulkan::UploadManager` (`src/vulkan/vulkan-upload.cpp`) and tags chunks with versions from `src/common/versioning.h`.
- The **upload** heap is pinned, device-mapped chunks (`cuMemHostAlloc(DEVICEMAP)`). It serves `writeBuffer`, `writeTexture`, `setPushConstants`, global-param blocks and uncached shader tables.
- The **scratch** heap is device chunks for accel-build scratch memory and instance arrays.
- A chunk is retired once the queue's completed instance passes the chunk's version.

The reference allocated with `cuMemAllocHost` on every write and ran host-side copies outside stream order; this heap replaces both.

### Shader container and reflection (`include/nvrhi/shader-reflection.h`)

**The container.** nvrhi owns the format in a standalone header with C layout. It has a magic number, a version, one code section per target (PTX, OptiX-IR, DXIL, SPIR-V, DXBC) and a reflection section. ShaderTool vendors a copy of the header, and an aggregate test checks that the two copies are identical.

**The reflection data.** It is a set of `nvrhi::vector` tables that refer to each other by index:
- **Entry points:** name, `ShaderType`, thread-group size, range and size of the entry-point parameters.
- **Global parameters:** name, kind, type, array size, the HLSL/Vulkan `{space, slot}`, and the CUDA/OptiX `{offset, size}`.
- **Type layouts:** kind, size, alignment, stride, and element and field ranges.
- **Fields.**

Each struct has `static_assert`s on its layout (§4.6).

**The API.** Methods are added to the vtables (R5). Each returns a non-owning view (§3.2) and returns `nullptr` when the library was created from plain bytecode:
- `IShaderLibrary::getReflection()` returns `const ShaderLibraryReflection*`.
- `IShader::getReflection()` returns `const ShaderEntryPointReflection*`.

Lookups are C exports, for example `nvrhiFindShaderParameter`. All backends parse containers in `src/common/shader-container.cpp`.

### Interop (`include/nvrhi/interop.h`, `src/cuda/cuda-interop{,-d3d12,-vulkan}.cpp`)

`IInteropDevice : IRHIObject` has these methods:
- `getGraphicsDevice` and `getComputeDevice`. Both return non-owning views.
- `importBuffer(nvrhi::IBuffer* graphicsBuffer, IBuffer** ppCudaBuffer)` and `importTexture` (all mips). They use the existing `getNativeObject(SharedHandle)` export, then `cuImportExternalMemory`.
- `signalGraphicsQueue`, `waitGraphicsQueue`, `signalComputeQueue` and `waitComputeQueue` take a `CommandQueue`. They run over shared timeline fences:
  - D3D12: shared `ID3D12Fence`, imported as `D3D12_FENCE`.
  - Vulkan: exportable timeline semaphores, driven through `vulkan::IDevice::queueSignalSemaphore` / `queueWaitForSemaphore`.

Every method returns `FE_UNSUPPORTED` in either case:
- the CUDA device was created without `graphicsDevice`;
- the graphics device is D3D11.

The validation `DeviceWrapper` (`src/validation/validation-backend.h:322`) also implements `IInteropDevice` when the underlying device does, so that COM identity is kept. It validates the arguments, unwraps them, and forwards the call. It does the same for `cuda::IDevice` and `cuda::ICommandList`.

### Texture interop: 3D textures and full mip chains

Sources checked:
- CUDA Driver API, "External Resource Interoperability": `cuImportExternalMemory`, `cuExternalMemoryGetMappedMipmappedArray`.
- CUDA Programming Guide §4.21, "Graphics interop".
- nvrhi's `src/vulkan/vulkan-texture.cpp`, `vulkan-allocator.cpp`, `src/d3d12/d3d12-texture.cpp` and `src/d3d11/d3d11-texture.cpp`.

**What CUDA requires.**
- `cuExternalMemoryGetMappedMipmappedArray(&mm, extMem, {offset, arrayDesc, numLevels})` maps a **whole mip chain** of any dimension, 3D included.
- `arrayDesc` describes the base level. Its format, dimensions, flags and level count must match the exporting resource exactly; a mismatch is undefined behaviour.
- The Programming Guide's helpers (`getCudaExtentForD3D12Extent`, `getCudaMipmappedArrayFlagsForD3D12Resource`, and the Vulkan equivalents) derive the descriptor as follows:

| Resource | `Width`, `Height`, `Depth` | Flags |
|---|---|---|
| 1D | `w, 0, 0` | |
| 1D array | `w, 0, layers` | `LAYERED` |
| 2D | `w, h, 0` | |
| 2D array | `w, h, layers` | `LAYERED` |
| Cube | `w, h, 6` | `CUBEMAP` |
| Cube array | `w, h, 6·n` | `CUBEMAP` \| `LAYERED` |
| **3D** | `w, h, depth` | none |

- `COLOR_ATTACHMENT` is **mandatory** when the resource is a render target: `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET` on D3D12, the color-attachment usage on Vulkan.
- `SURFACE_LDST` is set when CUDA must write to the array. Here that means when `TextureDesc::isUAV` is set, so the graphics side also declares storage/UAV usage.
- `offset` is 0 and `numLevels` is `desc.mipLevels`. Each level comes from `cuMipmappedArrayGetLevel`: one surface object per level, and one texture object over the whole chain with mip filtering.
- Import needs the **allocation size** and `CUDA_EXTERNAL_MEMORY_DEDICATED`.

**Mapping per API.**

| API | Handle and size | Dedicated? | Design |
|---|---|---|---|
| D3D12 | `ID3D12Device::CreateSharedHandle`. Size from `GetResourceAllocationInfo(0, 1, &desc).SizeInBytes`. | Yes: nvrhi creates shared textures with `CreateCommittedResource` and `D3D12_HEAP_FLAG_SHARED` (`d3d12-texture.cpp`). | External memory plus the mipmapped-array mapping above. Every dimension and mip is supported. |
| Vulkan | `vkGetMemoryWin32HandleKHR` / `vkGetMemoryFdKHR`. Size from `vkGetImageMemoryRequirements`, the same value `allocateTextureMemory` used. | Yes: `allocateTextureMemory` always passes the image as dedicated (`vulkan-allocator.cpp:79`). | Same as D3D12. Requires `VK_KHR_external_memory_win32` / `_fd`. |
| D3D11 | D3D11 has **no** allocation-size query, so `cuImportExternalMemory` cannot be given a correct size. That is why the reference used `w·h·bpp`. The current Programming Guide documents D3D11 only through *graphics interop*. | — | **Not supported (R9).** `importBuffer` and `importTexture` return `FE_UNSUPPORTED`. |

**Format rules.**
- Supported: 1-, 2- and 4-channel 8-, 16- and 32-bit UNORM, SNORM, UINT, SINT and FLOAT. SINT maps to the signed CUDA formats. sRGB uses the `CU_TRSF_SRGB` read flag. BC1–BC7 map to the `CU_AD_FORMAT_BC*` formats when the texture is read-only.
- Rejected with `FE_UNSUPPORTED`: 3-channel formats, depth/stencil formats, MSAA, and tiled or virtual textures.
- Workaround for depth: render depth into an `R32_FLOAT` color target, as `depth-map` already does.

**nvrhi defect to fix on the way.** `vulkan-texture.cpp` chains `VkExternalMemoryImageCreateInfo` only when `sharedResourceFlags == SharedResourceFlags::Shared`, while the allocator exports when `& Shared`. That mismatch is why the GVDB samples use exactly `Shared` on Vulkan. Test with `& Shared` in both places.

**Proof by test, because the documentation only guarantees matching layouts.**
- `interop-texture-{d3d12,vulkan}` covers:
  - shapes: 2D with mips, 2D array, cube, 3D with mips;
  - formats: R8_UNORM, RGBA8_UNORM, R16_FLOAT, R32_FLOAT, RGBA32_FLOAT, R32_UINT, R8_SINT.
- It writes a per-mip, per-slice pattern on the graphics side and reads it in CUDA through surfaces and texture fetches, then does the same in reverse.
- If a combination fails on a driver, `importTexture` refuses that combination with `FE_UNSUPPORTED` and the test records it. There is no silent copy fallback.

### Reference defects: fixed in this work

| Defect (`gvdb/GPDeviceCUDAImpl.cpp` unless noted) | Fix | Step |
|---|---|---|
| The Texture3D case falls through to `NVRHI_ASSERT(0)` (`:639-646`). | The descriptor table above, with 3D and every dimension handled. | 8 |
| Imports map one mip only, and the D3D11 size `w·h·bpp` ignores padding (`:1927-1958`, `numLevels = 1`). | Map the full mip chain using the API's real allocation size. D3D11 interop is removed (R9). | 8 |
| A D3D12 fence is imported as `D3D11_FENCE` (`:2229-2231`). | Import it as `CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE`. | 8 |
| `*_SINT` formats map to unsigned CUDA formats (`:130-169`). | Map them to the `CU_AD_FORMAT_SIGNED_INT*` formats, covered by the format test. | 5 |
| The Win32 handle field is set on the FD path (`:414`, `:609`). | Set `handle.fd` on Linux. On FD import, ownership passes to CUDA, so the FD is not closed afterwards. | 8 |
| Timeline signalling starts at 0 (`GPDeviceNVRHI.cpp:339`, `fetch_add`). | Values start at 1 and increase strictly (`++value`). | 8 |
| Accel builds call `cuStreamSynchronize` (`GPDeviceOptiXImpl.cpp:642-829`). | Builds are stream-ordered. Compaction reads the emitted size through a transient readback after the submission completes, as `compactBottomLevelAccelStructs` does. | 7 |
| The device is hard-coded to ordinal 0 (`:84`). | Match the adapter by LUID, or by Vulkan device UUID, with `cuDeviceGetLuid` / `cuDeviceGetUuid`. | 5 |
| Every write calls `cuMemAllocHost`, and staging copies run on the host outside stream order (`:827-1253`). | Use the transient upload heap; all copies are stream-ordered. | 5 |
| A new `cuCtxCreate` context is created instead of the primary context (`:78-99`). | Use `cuDevicePrimaryCtxRetain`, so other CUDA libraries share the context. | 5 |
| nvrhi Vulkan exports image memory only when `sharedResourceFlags == Shared` (`vulkan-texture.cpp`). | Test with `& Shared`. | 8 |

## Execution scope of the first round

Per the user's instruction of 2026-10-22, the first round implements **steps 0–10 only**: the nvrhi part and its unit tests. It also makes the minimum changes outside nvrhi that those steps need to build:
- the donut DeviceManagers and the waveworks demo, moved onto the factory (step 2);
- the shader-tool container backends and the donut gitlink (step 4);
- `cmake/EtherealShaders.cmake` in the aggregate (step 4).

Then the work **stops**, and the result is reported: the new public headers, test results, and deviations. Steps 11–15 (the GVDB samples) wait for further instructions. The header review from step 1 moves into that report.

## Steps

Each step builds in every repository it touches. The aggregate gitlinks are bumped only when all the repositories build together. Each step ends with an *as built* note in `ethereal-nvrhi/doc/plans/`.

**0. Branches and records.**
- Create `slang` in all five repositories.
- Copy this plan to `docs/plans/` and add a row to the index there.
- Add the maintained copy in `ethereal-nvrhi/doc/plans/`.
- Run `git worktree prune` for the stale `.worktrees/ethereal-slang-next` registrations. This removes registrations only; no branches are deleted.

**1. ADRs, then the public headers for review.**
- nvrhi `doc/adr`:
  - 0010: graphics factory and the removal of the per-backend creation exports.
  - 0011: CUDA/OptiX backend object model and deferred replay.
  - 0012: offline Slang shader container and the reflection on `IShaderLibrary` / `IShader`, plus the relaxed version policy. This ADR amends `doc/com-interface-abi.md` §3.4.
  - 0013: `IInteropDevice` implemented by the CUDA `Device` through `ROUTE_PARENT`.
- ethereal-samples `doc/adr` 0002: GVDB on the nvrhi CUDA backend in Slang.
- Show `factory.h`, `cuda.h`, `interop.h`, `shader-reflection.h` and the `nvrhi.h` edits to the user before implementing them.

**2. Graphics factory.** This step contains no CUDA code.
- **nvrhi:**
  - Add `factory.h` and `src/factory/`, and the `nvrhi_factory` target in static builds.
  - Make each backend's creation internal.
  - Remove the four creation exports and the `createDevice` / `createValidationLayer` wrappers.
  - Add a `qi-device` test of `IGraphicsFactory`.
  - Update `tests/qi-device.cpp:547-697` and `tests/memory-queries.cpp:267-429`.
- **donut:** `src/app/dx11/DeviceManager_DX11.cpp:246`, `dx12/DeviceManager_DX12.cpp:324` and `vulkan/DeviceManager_VK.cpp:1239`. `DeviceManager` owns one factory.
- **samples:** `demos/waveworks/sdk/src/{Local,Wind}_Waves_Simulation.cpp`.
- Verify that the existing samples still run on D3D11, D3D12 and Vulkan.

**3. Public API for CUDA and CMake.**
- `nvrhi.h` additions: `GraphicsAPI::CUDA`, and the reflection methods on `IShaderLibrary` and `IShader`.
- `common/resource.h` additions: `ObjectTypes` `0x0004xxxx` for `CUDA_Context`, `CUDA_Stream`, `CUDA_DevicePtr`, `CUDA_Array`, `CUDA_TexObject`, `CUDA_SurfObject`, `OptiX_DeviceContext`, `OptiX_Pipeline` and `OptiX_Traversable`.
- New headers:
  - `cuda.h`: `cuda::DeviceDesc`, `cuda::IDevice`, `cuda::ICommandList` and `cuda::INativeWork`. It has no creation export; devices are created through `IGraphicsFactory::createCudaDevice`.
  - `interop.h`.
  - `shader-reflection.h`.
- CMake:
  - Options `NVRHI_WITH_CUDA` and `NVRHI_WITH_OPTIX`.
  - `cmake/cuda.cmake`, which uses headers only and does not link `cuda.lib`.
  - The `nvrhi_cuda` target.
  - Header-TU gating in `tests/CMakeLists.txt`.
- Stubs return `FE_UNSUPPORTED`. Existing backends return `nullptr` from `getReflection`.

**4. Shader container and reflection.**
- **nvrhi:**
  - Add `src/common/shader-container.cpp`.
  - D3D11, D3D12 and Vulkan accept containers in `createShaderLibrary` / `createShader`, and each picks its own section.
- **shader-tool, on its `slang` branch:**
  - Add the `SLANG-PTX` and `SLANG-OPTIX` backends, using NVRTC at build time and the OptiX include path.
  - Serialize the reflection from the HLSL and CUDA layouts, cross-mapped by parameter path.
  - Add `--container` and its tests.
- **donut:** bump the shader-tool gitlink.
- **Aggregate:** add `.slang` and the CUDA/OptiX targets to `ethereal_compile_shaders()` in `cmake/EtherealShaders.cmake`.

**5. CUDA core.**
- Files: `src/cuda/cuda-{backend.h, driver-loader, device, buffer, texture, queue, commandlist, executor, transient-heap, queries, formats}.cpp`.
- Device creation through the factory, with adapter matching.
- Copies, writes and clears.
- Readback through `cpuAccess` buffers.
- Queues, events, lifetime tracking and `waitForIdle`.
- `queryFeatureSupport` reports compute, copy and ray-tracing features only. Creating graphics objects returns `FE_UNSUPPORTED`.

**6. CUDA compute.**
- Library and shader creation over PTX containers, and compute pipelines.
- Binding layouts and binding sets resolved against reflection.
- `setComputeState`, `dispatch` and `dispatchIndirect`. Indirect arguments are read back as slang-rhi does.
- `setPushConstants`.
- Texture SRVs paired with samplers: a `Texture*` and a `SamplerState` at the same register index map to one cached `CUtexObject`.
- Bindless arrays through `arrayElement`.
- `enqueueNativeWork`.

**7. OptiX.**
- Files: `src/cuda/cuda-optix-{device, pipeline, shader-table, accel}.cpp`, built under `NVRHI_WITH_OPTIX`.
- `optixInit` is called once.
- Pipelines, shader tables and acceleration structures as described in "CUDA object mapping".
- `dispatchRays` calls `optixLaunch`, with the global block as the launch parameters.

**8. Interop.**
- The `InteropDevice` mixin and `ROUTE_PARENT` in `cuda::Device`.
- D3D12 and Vulkan paths: Win32 handles, and FD handles on Linux. D3D11 returns `FE_UNSUPPORTED`.
- Buffer and texture import, with the full mip-chain mapping from "Texture interop".
- Timeline synchronization.
- Fix the nvrhi Vulkan `== Shared` export check.
- The validation wrapper implements `IInteropDevice`.

**9. nvrhi tests.**

Test fixtures are compiled with ShaderTool when `TARGET ShaderTool` exists; otherwise the tests are skipped.

| Test | What it covers |
|---|---|
| `qi-device` | The new `cuda` backend. `IInteropDevice` and `IObject` return the same identity on the raw device and through the validation layer. |
| `cuda-copy` | Copies. |
| `cuda-transient` | Chunks are reused while submissions are still in flight. |
| `cuda-dispatch` | Binding sets, push constants, sampling and surfaces. |
| `cuda-optix` | Triangle and AABB scenes. |
| `shader-reflection` | One container loaded on D3D12, Vulkan and CUDA. |
| `interop-{d3d12, vulkan}` | Buffer import, then a round trip with timeline synchronization. |
| `interop-texture-{d3d12, vulkan}` | The shape and format matrix from "Texture interop", in both directions. |
| `interop-d3d11` | Every `IInteropDevice` method returns `FE_UNSUPPORTED`. |

`nvrhi_abi_lint`, header hygiene and the header TUs must pass.

**10. Version record.**
- Confirm that `c_HeaderVersion` is still 28.
- The amendment to `com-interface-abi.md` §3.4 is in place.
- Keep the CMake package `ExactVersion` (`CMakeLists.txt:457`) unless ADR 0012 decides otherwise.

**11. GVDB library to Slang** (ethereal-samples, `slang`).
- First, capture reference screenshots of every sample from the current CUDA build. Keep them outside the repository.
- Add `gvdb/shaders/`:
  - `GVDB.slang`: `VDBInfo`, the traversal code and 5 kernels.
  - `GVDBDDA.slang`.
  - `GVDBOperators.slang`: 21 kernels.
  - `GVDBParticles.slang`: 35 kernels.
  - `GVDBSort.slang`: a 64-bit radix sort and a min/max reduction, replacing Thrust.
- Map the CUDA constructs to Slang:
  - `VDBInfo`'s texture and surface handles become global `Texture3D` / `RWTexture3D` arrays bound per element.
  - Node pointers become addresses.
  - `__constant__` variables become global parameters.
- Move `GVDB.h` and the `*Ops` classes from `gp::` to nvrhi interfaces. `getKernel` becomes a cached `IComputePipeline`.
- Re-enable `render-to-file` and `render-kernel` first. Other samples stay excluded in `samples/CMakeLists.txt` until their own step.

**12. CUDA renderer and graphics samples.**
- In `sample-utils`:
  - Add `GVDBRaycast.slang` and update `VolRenderer`.
  - `ScreenPresenter` uses `IInteropDevice` (`importTexture` plus timeline sync); the keyed-mutex path is removed.
  - `GVDBApp` rejects `-d3d11` with a clear message.
  - Update `HeadlessContext`.
  - `GVDBApp` creates the CUDA device through donut's factory with `graphicsDevice` set.
- Re-enable `3d-print`, `depth-map`, `resample`, `spray-deposit` and `point-fusion`.

**13. OptiX samples.**
- `OptixPrograms.slang`, and `OptixRenderer` on `rt::IPipeline`, `rt::IShaderTable` and `rt::IAccelStruct`.
- Re-enable `point-cloud`, `interactive-optix` and `fluid-surface`.

**14. SPH fluid pool.**
- Add `SPHFluidKernels.slang`.
- Import the shared vertex buffers through `IInteropDevice::importBuffer`.
- Remove the D3D11 host-copy fallback, the keyed-mutex paths (`main.cpp:172-200`) and `areRenderBuffersShared()`. `-d3d11` is rejected.

**15. Remove the CUDA C++.**
- Delete:
  - `gvdb/GPDevice*`;
  - every `.cu` and `.cuh` file;
  - `gvdb_add_ptx`;
  - the `ptx` VFS mount;
  - the `CUDA::cudart_static` link.
- In the root `CMakeLists.txt`, drop `CUDA` from the project languages and remove `ETHEREAL_WITH_OPTIX`. The root now forwards `NVRHI_WITH_OPTIX` instead.
- Update `gvdb_samples/README.md`, then bump the gitlinks.

## Rules check

- **ABI §2 (exports).**
  - Only `nvrhiCreateGraphicsFactory` and the reflection lookup functions are new exports. All are `NVRHI_C_API … noexcept`, return `FRESULT` and take pointer arguments.
  - The removed exports leave no C++ symbols behind.
- **ABI §3.1–3.3 (interface shape).**
  - Every new interface uses single inheritance from `IObject` / `IRHIObject` and gets a fresh IID.
  - Every method is `noexcept`. There are no overloads, which is why the factory has one creation method per backend.
  - No struct is passed or returned by value. Objects are returned as `FRESULT` + `IFoo**`, and views as raw pointers documented as "does not AddRef".
  - Callbacks are caller-implemented interfaces (`INativeWork`).
- **ABI §3.4.** Relaxed by ADR 0012, as the user decided. This is the only rule that changes.
- **ABI §4 and §7.** Descriptor and reflection structs have no base class, use fixed-type enums and nvrhi containers, and carry layout asserts. Public headers include no `std` headers.
- **ABI §6.4.**
  - `InteropDevice` is a mixin inside one object, the `ROUTE_PARENT` case, not aggregation, so no aggregation spans modules.
  - `checked_cast` to implementation classes happens only within `nvrhi`.
- **Function design.** No `AutoPtr` parameters: functions take raw `T*` and AddRef internally. `AutoPtr` is used only as a return value and for data members.
- **Repository conventions.**
  - Plans and ADRs follow the `doc/plans` and `doc/adr` READMEs, use repo-relative paths only, and live in the repository where the work happens.
  - New directories are `snake_case`; sample directories stay `kebab-case`.
  - Commits go only on the `slang` branches, and only when the user asks for them.

## Verification

- **Build.** Debug and Release, from PowerShell with VsDevCmd: `.\build.ps1 -CMakeArgs '-DETHEREAL_BUILD_GVDB=ON','-DNVRHI_WITH_CUDA=ON','-DNVRHI_WITH_OPTIX=ON','-DNVRHI_BUILD_TESTS=ON'`.
- **nvrhi tests.** Run `ctest --preset release`, one GPU test at a time.
- **After step 2.** Every existing donut-based sample starts on D3D11, D3D12 and Vulkan, with and without `-debug` (the validation layer through the factory).
- **GVDB samples.**
  - Run every sample with `--screenshot <png> --frames N` on D3D12 and Vulkan, plus the headless samples. Check that `-d3d11` is refused cleanly.
  - Compare each image with the references from step 11, allowing a small tolerance for differences between Slang/NVRTC and nvcc fast-math. Read each PNG back.
- **Debug builds.** Run under the nvrhi validation layer and the D3D12 / Vulkan debug layers. There must be no errors, and the reference counts must balance.
- **`sph_fluid_pool`.** Runs unattended on D3D12 and Vulkan with zero-copy shared vertex buffers.
- **Cleanup.** `grep` finds no `.cu` or `.cuh` file, no nvcc call and no `gp::` reference in `ethereal-samples`. It also finds no `nvrhi*CreateDevice` or `nvrhiCreateValidationLayer` call anywhere in the tree.

## Execution record (round 1: steps 0-10)

Round 1 ran on 2026-10-22 and is committed on the `slang` branch of all five repositories.

*Step 0 as built.*
- `slang` was created in all five repositories:
  - from `ethereal-dev` in nvrhi and donut, by then checked out on `ethereal-dev` rather than `review/slang-next-pr`;
  - from `main` in the aggregate and the samples;
  - from shader-tool `main` at `afbf273`.
- `git worktree prune` removed the stale `.worktrees/ethereal-slang-next` registrations of nvrhi, donut, samples and shader-tool. No branch was deleted.
- The approved plan is in the aggregate's `docs/plans` and here. Both plan indexes have a row for it.

*Step 1 as built.*
- ADRs [0010](../adr/0010-graphics-factory.md)-[0013](../adr/0013-interop-device.md) are here, and ethereal-samples ADR 0002 (Proposed, for steps 11-15) is in that repository.
- **Deviation:** the user asked for round 1 to run to the end and report, so the header review moves into the round's report. It is not a stop before step 2.

*Step 2 as built.*
- **nvrhi:**
  - `include/nvrhi/factory.h` and `src/factory/factory.cpp`.
  - `nvrhi_factory` in static builds, linking every backend library and then `nvrhi`.
  - The per-backend constructors are module-private, in `src/common/backend-factories.h`.
  - The four creation exports and their wrappers are removed.
  - The tests get the factory through `tests/test-factory.h`. `qi-device` checks the factory's QueryInterface and that the new IIDs are refused.
- **donut:** `DeviceManager::GetGraphicsFactory()`, plus the three device managers; `donut_app` links `nvrhi_factory`.
- **samples:** the WaveWorks SDK.
- **Deviation:** `isBackendAvailable` is not `const`, because for CUDA it loads the driver.

*Step 3 as built.* The public API is as planned, with these deviations:
- The container format is its own C header, `include/nvrhi/shader-container.h`, next to `shader-reflection.h`, so that ShaderTool can vendor it.
- `IShaderLibrary` gained only `getReflection()`, because the entry points are in the reflection. `IShader` gained `getReflection()` and `getLibraryReflection()`.
- `IInteropDevice` orders the queues with two calls, `computeQueueWaitForGraphicsQueue` and `graphicsQueueWaitForComputeQueue`, instead of four signal / wait methods. Each call pairs the signal with the wait.
- `cuda::INativeWork` derives from `IRHIObject`, like `IMessageCallback`.
- The new headers include no CUDA header, so the header TUs need no gating.

*Step 4 as built.*
- **nvrhi:** `src/common/shader-container.cpp`, used by every backend.
- **shader-tool:** `ShaderTool container --container-targets ...` and `shadertool_add_shader_containers()`, written by a subagent; its tests pass.
- **aggregate:**
  - `ethereal_compile_shader_containers()`;
  - `ETHEREAL_WITH_CUDA`, which forwards `NVRHI_WITH_CUDA`, `NVRHI_WITH_OPTIX` and `NVRHI_SHADERTOOL_TARGET`;
  - the test `shader_container_header_copy`.
- **Deviations:**
  - A DXIL or DXBC section holds one non-ray-tracing entry point, so the reflection fixture (`tests/shaders/cuda-reflection.slang`) is the dispatch module without its second kernel.
  - The donut gitlink of shader-tool points at the shader-tool `slang` commit with the container mode.

*Steps 5-7 as built.* `src/cuda`, as in ADR 0011. Additions to the plan:
- Each queue loads its own module of a library, so the queues never race on `SLANG_globalParams`.
- Acceleration-structure handles are written at replay (`PatchTraversable`).
- Slang has no `ReportHit` for OptiX. The fixture's intersection program calls `optixReportIntersection` through `__target_switch`.

*Step 8 as built.* Interop is as in ADR 0013. **Deviation found by the tests:**
- The NVIDIA Vulkan driver pads the layers of mipmapped arrays and cube maps beyond what a layered CUDA array expects. From the second layer on, CUDA read the wrong memory.
- A probe over ten shapes showed that D3D12 maps all of them, and that Vulkan maps everything except layered textures with more than one mip level.
- The Vulkan import refuses those with `FE_UNSUPPORTED`.

The validation layer answers `IInteropDevice` through the `InteropDeviceWrapper` mixin.

*Step 9 as built.*
- **Deviation:** one executable, `tests/cuda-tests.cpp` (`nvrhi_cuda_tests <test>`), instead of one per area. It has 13 ctest entries: `cuda_qi`, `cuda_copy`, `cuda_transient`, `cuda_queues`, `cuda_dispatch`, `cuda_optix`, `cuda_reflection`, `cuda_interop-d3d11`, `cuda_interop-d3d12`, `cuda_interop-vulkan`, plus the existing tests.
- The fixtures are compiled from `tests/shaders` by ShaderTool: the target named by `NVRHI_SHADERTOOL_TARGET`, or `NVRHI_SHADERTOOL_EXECUTABLE`.
- `abi_lint.py` scans the new headers.

*Step 10 as built.*
- `c_HeaderVersion` is 28; its comment points to the policy.
- `doc/com-interface-abi.md` §3.4 is amended.
- The CMake package keeps `ExactVersion`.

## Verification results (round 1)

- **Standalone nvrhi, Debug** (`build-slang`, vcpkg `x64-windows-static-md`, CUDA and OptiX on): 25 of 26 tests pass. The failure is `memory_queries_d3d12`, a Win10-runtime rejection of the AS resource flag. The `build-factory` binary from before this work fails the same way.
- **Aggregate, Release** (`ETHEREAL_WITH_CUDA=ON`):
  - The whole tree builds, after reruns for the known vcpkg app-local "file in use" link flakes.
  - All 38 aggregate tests pass, including `memory_queries_d3d12` (the aggregate uses the Agility SDK), every `cuda_*` test and `shader_container_header_copy`.
- **Samples:**
  - `depth-map` (still on `gp`) renders the same picture on D3D11, D3D12 and Vulkan, with and without `-debug`.
  - `basic_triangle` starts on all three APIs with `-debug`.
  - `waveworks_ocean --simulation-dev=gpu` runs on all three APIs.
- **Shared build** (`NVRHI_BUILD_SHARED=ON`, Release, CUDA and OptiX on):
  - `nvrhi.dll` exports 31 symbols, all named `nvrhi*`, among them `nvrhiCreateGraphicsFactory` and the reflection lookups.
  - 25 of 26 tests pass; `memory_queries_d3d12` fails as in the static build.
  - **Pre-existing break fixed:** this build did not compile on `ethereal-dev` either. Vulkan-Hpp 1.4.363 (vcpkg) no longer defaults the device proc address of the four-argument `DispatchLoaderDynamic::init`, so `vulkan-device.cpp` now calls `init(instance, vkGetInstanceProcAddr)` and then `init(vk::Device(device))`.

## Execution record (round 2: steps 11-15)

Round 2 ran on 2026-10-22 and 2026-10-23, after the user asked to commit round 1 (including shader-tool) and to carry on until the plan is fulfilled. It is not committed yet.

*Step 11 as built.*
- Reference screenshots of every sample were captured from the round-1 build (CUDA C++ on `gp`) and kept outside the repository: 8 frames on D3D12 and Vulkan, plus 120 frames for the animated samples.
- **The compute layer** is `gvdb/GVDBCompute.h`:
  - `IComputeContext`: the CUDA device, the one open command list GVDB work is recorded into, `flush()` / `finish()`, `readBuffer()`, the kernels of the shader containers of a file system, and `releaseAfterUse()` for resources that recorded work reaches through addresses or handles.
  - `IComputeKernel`: a compute pipeline whose arguments are the entry point's uniform parameters. `gvdb::launchKernel(commandList, kernel, groups, args...)` places them at the reflected offsets and checks their sizes.
- **Slang modules:** `gvdb/shaders/GVDB.slangh` (VDBInfo, traversal, atlas access), `GVDBOperators.slang`, `GVDBParticles.slang` (22 particle and 4 topology kernels; the reference's 12 unused kernels are not ported) and `GVDBPrimitives.slang` (recursive prefix sum, an LSD radix sort of 64-bit keys in 8-bit passes with a ballot-based stable rank, a two-stage min/max, a buffer-to-atlas block copy; they replace Thrust). The DDA lives with the raycaster (step 12). `gvdb_tests` checks the primitives and a volume.
- **Deviations from the planned mapping**, all because the kernels use pointers: CUDA-only Slang code has no D3D register layout, so a CUDA-only module cannot bind global resources.
  - `VDBInfo` keeps the channels' texture and surface handles (plus `atlas_rcp`, for normalized sampling) instead of global `Texture3D` / `RWTexture3D` arrays. **New nvrhi API:** `cuda::IDevice::getTextureHandle` / `getSurfaceHandle` (test `cuda_handles`). Slang turns a handle into a `Sampler3D` / `RWTexture3D` with `Handle()`; a `bit_cast` to a handle type crashes the Slang 2026.10.2 compiler.
  - `__constant__` variables become kernel arguments, passed by value, not global parameters. A kernel is an `IComputeKernel` launched through push constants, not a pipeline with binding sets.
  - `GVDBVolume` checks the host `VDBInfo` against the reflection of the kernels.
- **nvrhi fix:** `cuLaunchKernel` takes the kernel parameters at their exact size; a size padded to the struct alignment gave `CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES`. ShaderTool records the exact end and the backend passes `min(paramsSize, end of the last parameter)` (test `cuda_handles`, unpadded arguments).
- **ShaderTool, CUDA-only modules:**
  - discovery checks the module for the first requested target;
  - the HLSL cross-map is built in a quiet session and is best-effort;
  - resource-typed PTX entry-point parameters are uniform data (handles);
  - a global that needs a register is an error without a D3D layout.
- **Slang defect worked around:** `WaveMatch` lowers to `__match_all_sync` on CUDA (2026.10.2); the radix sort matches digits with a 9-bit ballot instead.

*Step 12 as built.*
- `sample-utils/kernels/GVDBRaycast.slang(h)`: the brick functions are types implementing `IBrickFunc`, `rayCast()` is generic over them, and the `SCN_*` settings travel in a `RayContext`. `ScnInfo` is passed by pointer, uploaded per volume instance.
- Structs used in arrays carry the padding of the reference's `__align__(16)` explicitly (`ScnRay` 64 bytes, `ScanObj` 48).
- `ScreenPresenter` imports its textures and orders the queues with `computeQueueWaitForGraphicsQueue` / `graphicsQueueWaitForComputeQueue`; the keyed-mutex path is gone.
- `GVDBApp` creates the CUDA device through Donut's factory with `graphicsDevice` set and gets `IInteropDevice` by QueryInterface. It refuses D3D11 with a warning: Donut's `log::error` breaks into the debugger.
- Converted: `render-to-file`, `render-kernel`, `3d-print`, `depth-map`, `resample`, `spray-deposit`, `point-fusion`.
- **Aggregate:** `ethereal_compile_shader_containers()` gets `BASE_DIRECTORY`, so that containers keep their subdirectory.

*Step 13 as built.*
- `kernels/optix/OptixPrograms.slang` (plus `OptixConstants.h`, shared with the host) and `OptixRenderer` on `rt::IPipeline`, `rt::IShaderTable` and `rt::IAccelStruct` of the CUDA device.
- **Launch parameters** are a `[[vk::push_constant]]` block, the only kind of global a CUDA-only module can have besides plain data. Two changes make that work:
  - ShaderTool accepts push-constant blocks in CUDA-only modules;
  - **nvrhi** copies push constants to device memory before a launch; they were read from host-mapped upload memory by every thread. The `cuda_optix` test now takes its miss colour from a push-constant block.
- **The TLAS handle** is known only once its build is replayed, so `OptixRenderer` flushes the context after a TLAS build before it reads the handle with `getNativeObject(OptiX_TraversableHandle)`.
- **Shader records carry no data:** a hit finds its scene instance by `InstanceID()` in a device table. The hit groups are refilled on every render, so shading and material changes apply at once.
- **Slang and OptiX:**
  - Slang passes payloads by value, one payload register per 32-bit word (the backend's comment said "by pointer"; fixed). The intersection programs read the ray kind and depth, the payload's first two words, with `optixGetPayload_0/1`.
  - `optixReportIntersection`, the object-to-world transforms of vectors and normals and `optixIsTriangleHit` are called through `__target_switch` / `__intrinsic_asm`. Custom attributes are read through an attribute struct.
- `fluid-surface`'s simulation is `FluidSystemKernels.slang`, with the parameter block and the buffer tables as by-value kernel arguments; helpers are `[ForceInline]`, since a call copies the blocks.
- **Defect found and fixed (also in the CUDA raycaster):** the DDA of `rayCast()` runs on at `p == res`, the cell on a node's far face, so that the level test climbs up, and read the child list for it: past the node's list, into another node's children or past the end of the pool. Found by compute-sanitizer after fluid-surface crashed at frame 35; the reference has the same code. That cell is now treated as empty.
- **Reference defect not carried over:** `gp` created every channel's texture object for linear filtering, so its `tex3D<uchar4>` colour reads returned float bit patterns. fluid-surface shows its particle colours now; the reference showed grey with green and blue patches.

*Step 14 as built.*
- `kernels/SPHFluidKernels.slang`; `ISPHParticles` and `ISPHSolver` run on a compute context. The parameter block and the buffer tables are kernel arguments.
- Position, colour and velocity are vertex buffers of the graphics device imported with `IInteropDevice::importBuffer`; `configure()` fails when the import does. The D3D11 host-copy fallback, `areRenderBuffersShared()`, `updateRenderBuffers()` and the keyed-mutex paths are gone; `GVDBApp`'s frame orders the queues.

*Step 15 as built.*
- The `gp` sources, every `.cu` and `.cuh` file, `cuda_math.cuh` and `OptixLaunchParams.h` went to the recycle bin; so did the aggregate's `cmake/FindOptiX.cmake`. `gvdb_add_ptx` and the `ptx` mount had gone with steps 11-12.
- `sph_fluid_pool` is re-enabled and the temporary sample filter is removed.
- **Aggregate root:** no CUDA language; `ETHEREAL_WITH_OPTIX` is removed (nvrhi's `NVRHI_WITH_OPTIX` finds the SDK itself); `ETHEREAL_BUILD_GVDB` stops with an error without `ETHEREAL_WITH_CUDA`. README updated.
- **Fast math:** ShaderTool takes `-fp-mode precise|fast|default`, a per-target Slang setting (`fast` is NVRTC's `--use_fast_math`). Every GVDB container is compiled with it, as the reference's nvcc builds were (`--use_fast_math`). It brought the surface and SPH step times back to the reference's and depth-map from 67 to 79 dB.
- **Pre-existing sample defect fixed:** Donut renders only while the window has the focus, so a screenshot run whose window did not get it (started from a script) stopped after its first frame and spun. `GVDBApp::ShouldRenderUnfocused()` returns true for screenshot runs. Before: about 1 run in 16 stalled; after: none in 30.
- **Raycast speed:** the GVDB raycasts (compute kernels and OptiX intersection programs) were 25-48% slower than the nvcc build. NVRTC is not the cause: both builds use the same CUDA 12.9 compiler (the same NVVM build and `ptxas`). The difference is the input: the CUDA that Slang emits, against the hand-written CUDA of the reference. Slang lowers `&&` loop conditions to branches over `bool` temporaries, and the `VDBInfo` reads inside them (the loop bounds `res`, `dim`) stayed in the marching loops; the reference had them hoisted. Sampling with normalized coordinates (Slang's `Sample`) adds a read of the atlas scale and a multiply per sample, which the reference's `tex3D` with voxel coordinates did not have. The DDA looked each cell's child up twice (`isBitOn()`, then `getChild()`), as the reference's source does; nvcc merged part of that, but the Slang build repeated the whole chain of reads. (Not checked directly: compiling Slang's CUDA with nvcc should give the same slow code.) `GVDBRaycast.slangh` now reads them once: `ChannelTexels` holds a channel's handle and scale, `insideRange()` tests the bounds, `rayCast` caches the level's `res` / `dim` and looks the child up once. Same results (the deterministic captures are bit-identical). Two smaller changes, each within the noise: OptiX pipelines use `OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING` (an `rt::IAccelStruct` scene is one TLAS over geometry), and `atlasSample()` uses `Sample` (`tex3D`) instead of `SampleLevel(…, 0)`. Ruled out on the way: stack sizes, forced inlining, the payload in registers, the launch parameters behind a pointer, single-level textures bound as arrays. The two cases and how to find them are written up in `doc/cuda-slang-performance.md`.
- `gvdb_samples/README.md` and samples ADR 0002 (now Accepted) describe the result. The gitlinks are bumped when the round is committed.

## Verification results (round 2)

- **Tests:** the 11 `cuda_*` tests and `gvdb_primitives` / `gvdb_volume` pass (Release aggregate).
- **Screenshots, 8 frames,** against the round-1 references (PSNR, D3D12 / Vulkan):

  | Sample | dB | Note |
  |---|---|---|
  | render-to-file / render-kernel | 64.5 / 62.3 | console |
  | resample | 87.6 / 87.6 | |
  | depth-map | 78.6 / 78.6 | |
  | sph_fluid_pool | 78.0 / 78.6 | |
  | spray-deposit | 54.5 / 54.9 | |
  | interactive-optix | 54.5 / 54.6 | progressive sampling |
  | 3d-print | 33.7 / 33.8 | the reference's inset follows the mouse; the main view is the same |
  | fluid-surface | 32.9 / 32.7 | particle colours (the reference's colour defect) |
  | point-cloud | 25.0 / 25.0 | sampling noise: two new runs differ by the same 25 dB |
  | point-fusion | 21.3 / 21.5 | the scan jitter is nondeterministic in both |

- **120 frames** (D3D12): fluid-surface 29.8, point-cloud 37.3, point-fusion 29.0, sph_fluid_pool 34.3 dB; the same shapes. Step times against the reference: GVDB surface 18 ms (20.4), SPH step 29.0 ms mean (28.0). fluid-surface's "fluid" time also covers the previous frame's OptiX render, which its timer waits for (see the raycast speed below).
- **Raycast speed against the pre-conversion build** (Nsight Systems, median kernel time, D3D12):

  | Kernel | Pre-conversion | Before the raycast fix | Now |
  |---|---|---|---|
  | fluid-surface, OptiX launch (60 frames) | 10.56 ms | 13.9 ms | 10.40 ms |
  | interactive-optix trilinear, OptiX launch | 4.21 ms | 5.71 ms | 4.33 ms |
  | interactive-optix volume, OptiX launch | 5.59 ms | 7.22 ms | 5.82 ms |
  | 3d-print, `gvdbRaySurfaceVoxel` | 0.294 ms | 0.436 ms | 0.301 ms |

  fluid-surface's own timers after 120 frames: fluid 35.3 ms (34.7 before the conversion), surface 16.9 ms (18.7). The other kernels of the frame (SPH, GVDB surface building, AS builds) are within 6%.
- **Other shadings** (interactive-optix trilinear / empty skip / level set; fluid-surface trilinear / volume) render without errors; the reference has no captures of them.
- **ShaderTool's own container tests** (standalone build with `ptx`, `optix`, `dxil` and `spirv`): 493 checks pass, including a new CUDA-only fixture (`tests/container/cudaonly.slang`, built twice): a push-constant block without a D3D register, a plain-data global, pointer and descriptor-handle entry parameters, the exact `paramsSize`, and `-fp-mode` (IEEE `div.rn` / `sqrt.rn` by default, `approx.ftz` with `fast`; the test fails when the two builds are swapped).
- **Debug layers** (`--debug`, D3D12 and Vulkan, the OptiX samples and sph_fluid_pool): no errors; only the known Vulkan warnings about the swap-chain image usage of the screenshot readback.
- **compute-sanitizer memcheck:** fluid-surface, 40 frames, 0 errors (after the DDA fix).
- **Cleanup greps:** no `.cu` / `.cuh` file, no `gp::` reference, no nvcc call in `ethereal-samples`; no `nvrhi*CreateDevice` or `nvrhiCreateValidationLayer` call in the tree.

## Known issues and follow-ups

- **Not supported on the CUDA backend:**
  - `buildTopLevelAccelStructFromBuffer` and `copyRaytracingAccelerationStructure`;
  - bindless layouts and descriptor tables;
  - texture views of a partial mip range (a single level, or the whole chain, is fine);
  - swizzles for BGRA formats;
  - 3-component and depth formats in CUDA arrays.
- **Vulkan interop:** layered textures with mip chains are refused.
- **ShaderTool containers** cannot yet describe:
  - registers of resources nested in parameter blocks;
  - `$Globals` offsets on PTX;
  - a "no binding" marker;
  - matrix layouts.

  These are format additions for a minor version.
- **`slang-next`:** the branches still exist, for later removal.
- **CUDA-only Slang modules** can bind globals only as plain data or a push-constant block. An explicit register (or an attribute naming one) for resources would let them use binding sets.
