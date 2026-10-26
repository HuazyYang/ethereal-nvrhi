# 0014. Per-backend graphics factories create native devices and swap chains

- Status: Accepted
- Date: 2026-10-26
- Supersedes: [0010](0010-graphics-factory.md)
- Plan: [`../plans/2026-10-26-graphics-factory-swap-chain.md`](../plans/2026-10-26-graphics-factory-swap-chain.md)

## Context

The graphics factory of [ADR 0010](0010-graphics-factory.md) only wraps native devices that the caller created.
donut's `DeviceManager_DX11/DX12/VK.cpp` (about 2500 lines) does everything else:
- creates the DXGI factory and the VkInstance, with its debug callbacks;
- selects the adapter or physical device;
- creates the native device and its queues;
- runs the swap chain and the frame pacing between the command queue and presentation.

An application that does not use donut has to repeat all of it. It also has to link `d3d11.lib`, `d3d12.lib` and
`dxgi.lib`, and export `D3D12SDKVersion`/`D3D12SDKPath` from its executable to load the Agility SDK.

The user asked for this work to move into NVRHI, with these choices:
- one factory per backend;
- creation descriptors that hold no native objects;
- a swap-chain interface in the `nvrhi` namespace with an implementation per backend;
- `beginFrame` as the per-frame entry point;
- no adapter interface and no window-system enum;
- the Agility SDK loaded through `ID3D12SDKConfiguration1::CreateDeviceFactory` with no executable exports.

## Decision

**Factories.** `nvrhi::IGraphicsFactory` (`include/nvrhi/factory.h`) is the common base. It has:
- `getGraphicsAPI`;
- adapter enumeration (`getAdapterCount`, `getAdapterInfo` into a plain `AdapterInfo`);
- `getNativeObject` (DXGI factory, `ID3D12DeviceFactory`, VkInstance, `vkGetInstanceProcAddr`);
- `createValidationLayer`.

Each backend header declares, in its own namespace:
- `GraphicsFactoryDesc`, holding only what that backend reads:
  - D3D: debug runtime, GPU validation, Agility `sdkVersion`/`sdkPath`, DLL name overrides;
  - Vulkan: library name, instance extensions and layers, ignored validation locations, headless;
  - CUDA: driver library.
- `IGraphicsFactory : nvrhi::IGraphicsFactory`, with:
  - `createDevice(const DeviceDesc&, IDevice**)`: the former `IGraphicsFactory::create<Backend>Device`. It wraps
    caller-made native objects; `DeviceDesc` is unchanged.
  - `createDeviceOnAdapter(const DeviceCreationDesc&, IDevice**)` (D3D11, D3D12, Vulkan). It selects the adapter and
    creates the native device and queues inside, then builds a `DeviceDesc` and runs the same backend path as
    `createDevice`. The NVRHI device owns these native objects. The two methods have different names because ABI R3
    forbids overloads.
  - `createSwapChain(const SwapChainDesc&, nvrhi::IDevice*, <backend>::IGraphicsSwapChain**)`.
  - backend extras: `reportLiveObjects` (D3D); the enabled instance extensions and layers (Vulkan).
- A creation export, `nvrhi<Backend>CreateGraphicsFactory(const GraphicsFactoryDesc*, IGraphicsFactory**)`, with
  an inline `createGraphicsFactory(desc)` wrapper. It returns `FE_UNSUPPORTED` when the backend's runtime does not
  load. This replaces `isBackendAvailable`.

`DeviceCreationDesc` holds no native objects: no `ID3D11Device*`, context, `ID3D12CommandQueue*`, `VkInstance`,
`VkPhysicalDevice`, `VkDevice` or `VkQueue`. It holds:
- the adapter index;
- what to create: queues, feature level, device extensions, multiview requirements, the
  `physicalDeviceFeatures2Extensions` chain, the `vulkan::IDeviceCreateCallback` that edits `VkDeviceCreateInfo`,
  and the surface the device must be able to present to;
- the non-native fields of `DeviceDesc`, repeated (ABI §4.1 forbids a base struct).

`cuda::DeviceDesc` already holds no native objects, so CUDA has only `createDevice`. Its `driverLibraryName` moves
to `cuda::GraphicsFactoryDesc`.

Adapter selection is DeviceManager's: DXGI index (-1 means adapter 0); Vulkan's `pickPhysicalDevice` rules, which
prefer a discrete GPU.

**Swap chains.** `nvrhi::IGraphicsSwapChain : IRHIObject` (`include/nvrhi/swapchain.h`) has:
- `getDesc`, which reports the actual size and format;
- the back buffers and the current index;
- `beginFrame`:
  - D3D11: nothing.
  - D3D12: waits on the CPU for the current back buffer's frame-fence sync point.
  - Vulkan: acquires the next image and queues the semaphore wait on the graphics queue.
  - CUDA: waits for the in-flight stream work of the reused frame slot.
- `present`, `resize`, `setVsyncEnabled`.

`FE_OUT_OF_DATE` (-10) asks for a resize. `SwapChainDesc::window` is the native handle (`HWND`; an X11 `Window` with
`display` elsewhere); there is no window-system enum. `d3d11/d3d12/vulkan/cuda::IGraphicsSwapChain` derive from it
and add typed accessors. A CUDA swap chain has no window: it rotates CUDA-texture back buffers and paces frames.

**Run-time loading.**
- D3D factories `LoadLibrary` dxgi/d3d11/d3d12, and NVRHI stops linking their import libraries.
- With `sdkVersion` set, the D3D12 factory creates an `ID3D12DeviceFactory` from `sdkPath` (relative to the
  executable). That device factory creates the devices and configures the debug layer.
- The Vulkan factory loads the Vulkan library and owns the Vulkan-Hpp default dispatcher, whose storage moves into
  NVRHI.
- New object types:
  - `DXGI_Factory`, `DXGI_Adapter`, `DXGI_SwapChain`;
  - `D3D12_DeviceFactory`, `D3D12_Fence`;
  - `VK_SurfaceKHR`, `VK_SwapchainKHR`, `VK_GetInstanceProcAddr`.

## Consequences

- donut's DeviceManager keeps the window, GLFW, the render passes and the frame loop. It creates its backend factory,
  device and swap chain through these interfaces and holds no native device, queue or instance.
- Every caller of the ADR 0010 factory changes:
  - `nvrhi::createGraphicsFactory` → `<backend>::createGraphicsFactory`;
  - `create<Backend>Device` → `createDevice`.
  Affected callers in this tree: donut, the WaveWorks SDK, the GVDB samples and the NVRHI tests. This is an API
  break. Per [ADR 0012](0012-offline-slang-shader-container-and-reflection.md) it does not bump `c_HeaderVersion`.
- Applications no longer export `D3D12SDKVersion`. The Agility SDK version and path are factory settings. They need
  `ID3D12SDKConfiguration1`, which a probe confirmed on Windows 10 19045 with SDK 1.619.6 (2026-10-26).
- An application links only the backends it uses: each export lives in its backend library, and `nvrhi_factory`
  goes away.
- One Vulkan device per process, as before: the dispatcher is global.

## Alternatives

- **A creation section in each `DeviceDesc`** (native pointers null → create them). Rejected by the user: the native
  objects must be created inside the creation function, not passed in.
- **One `DeviceCreationDesc` with a union of the backend descriptors and one `createDeviceOnAdapter`.** Rejected by
  the user in favour of per-backend factories.
- **An `IGraphicsAdapter` interface.** Rejected: the adapter selection of DeviceManager is reused as it is.
- **Keep the exe exports as a fallback for the Agility SDK.** Rejected by the user; the device factory works on this
  machine.
