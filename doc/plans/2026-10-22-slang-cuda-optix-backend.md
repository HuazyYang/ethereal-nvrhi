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
- **Next:** steps 11-15 (the GVDB samples) wait for the user.
