# 0013. `IInteropDevice`: the CUDA device shares memory and queue order with its graphics device

- Status: Accepted
- Date: 2026-10-22
- Plan: [`../plans/2026-10-22-slang-cuda-optix-backend.md`](../plans/2026-10-22-slang-cuda-optix-backend.md), step 8

## Context

The GVDB samples compute on CUDA and present with D3D11, D3D12 or Vulkan. Their `gp` layer imported graphics resources
into CUDA and synchronized the two devices with fences and D3D11 keyed mutexes. The user asked for:
- one interop interface, `IInteropDevice`;
- that interface aggregated into `IDevice` and reached with QueryInterface, with the device implementing both
  `IDevice` and `IInteropDevice`, routed through a parent class.

They also asked that 3D textures and complete mip chains be imported, which `gp` never did. Investigation found:
- `cuExternalMemoryGetMappedMipmappedArray` maps a whole mip chain of any shape. Its array descriptor must match the
  graphics resource: extent, format, level count, LAYERED / CUBEMAP, and COLOR_ATTACHMENT for render targets.
- The import needs the allocation size, and the DEDICATED flag for committed or dedicated memory.
- **D3D11 has no way to report the allocation size of a resource.** That is why `gp` used `w·h·bpp`, which is wrong.
  The user decided to **drop D3D11 interop**.

## Decision

**The interface** (`include/nvrhi/interop.h`):

```cpp
struct IInteropDevice : IRHIObject
{
    nvrhi::IDevice* getGraphicsDevice();   // not AddRef'd; null if unpaired
    nvrhi::IDevice* getComputeDevice();    // this object, as IDevice
    bool isInteropSupported();
    FRESULT importBuffer(IBuffer* graphicsBuffer, IBuffer** ppBuffer);
    FRESULT importTexture(ITexture* graphicsTexture, ITexture** ppTexture);   // every mip and slice
    FRESULT computeQueueWaitForGraphicsQueue(CommandQueue computeQueue, CommandQueue graphicsQueue);
    FRESULT graphicsQueueWaitForComputeQueue(CommandQueue graphicsQueue, CommandQueue computeQueue);
};
```

**Pairing.**
- `cuda::DeviceDesc::graphicsDevice` pairs a CUDA device with a graphics device. The CUDA device on the same adapter
  is selected ([ADR 0011](0011-cuda-optix-backend.md)).
- `cuda::Device` derives from `ObjectImpl<cuda::IDevice, InteropDevice>`. `InteropDevice` is a mixin that implements
  `IInteropDevice` with its own QueryInterface table.
- The device's table reaches the mixin with **`NVRHI_IMPLEMENTS_ROUTE_PARENT(InteropDevice)`**, the
  `Multi : ObjectImpl<P1, P2>` pattern of `tests/core/test_qi_route.cpp`. `IRHIObject`, which is a base through both
  interfaces, is answered through `IDevice`. The result is one identity and one reference count.
- The validation layer's `DeviceWrapper` keeps that identity the same way. Its `InteropDeviceWrapper` mixin answers
  `IInteropDevice` only when the wrapped device does, checks the arguments, and forwards the calls.

**API-specific parts** (`src/cuda/cuda-interop-d3d12.cpp`, `cuda-interop-vulkan.cpp`):

| | D3D12 | Vulkan | D3D11 |
|---|---|---|---|
| Memory | the NT handle NVRHI keeps for shared resources, imported as `D3D12_RESOURCE`, DEDICATED, size from `GetResourceAllocationInfo` | the `OPAQUE_WIN32` handle (`OPAQUE_FD`, duplicated, on Linux), DEDICATED, size from `vkGetImage/BufferMemoryRequirements` | `FE_UNSUPPORTED` |
| Graphics signals | `ID3D12CommandQueue::Signal` on a shared fence | an empty `vkQueueSubmit` that signals an exportable timeline semaphore | `FE_UNSUPPORTED` |
| Graphics waits | `ID3D12CommandQueue::Wait` | `vulkan::IDevice::queueWaitForSemaphore`, so the next NVRHI submission waits | `FE_UNSUPPORTED` |
| CUDA side | `cuSignal/WaitExternalSemaphoresAsync` on the queue's stream; values start at 1 and increase | same | none |

**Textures.** The array descriptor is derived from the `TextureDesc` (`GetArrayDescriptor`):
- `SURFACE_LDST` for UAVs, `COLOR_ATTACHMENT` for render targets;
- `LAYERED` for arrays, `CUBEMAP` for cube maps;
- `Depth` is the depth of 3D textures, or the layer count.

It is refused (`FE_UNSUPPORTED`) for 3-component, depth-stencil, multisampled, tiled and virtual textures.

**Vulkan exception.** On the NVIDIA Vulkan driver, the layers of a **mipmapped array or cube texture** are padded
beyond the layout of a layered CUDA array: from the second layer on, CUDA reads the wrong memory. Single-level arrays
and cube maps, and 2D and 3D textures with mip chains, map exactly. The Vulkan import refuses layered textures with
more than one mip level. The rule is the plan's: refuse what does not map, never fall back to a silent copy.

## Consequences

- **What the tests cover.** `tests/cuda-tests.cpp` (`interop-d3d12`, `interop-vulkan`) writes a pattern into every mip
  and slice on one device and reads it back on the other, in both directions. The shapes are:
  - 2D with mips, 2D arrays, cube maps, cube maps with mips;
  - 3D with mips;
  - large 2D arrays and 2D textures, in eight formats.

  On D3D12 every shape maps. On Vulkan every shape maps except the refused ones. `interop-d3d11` checks that a D3D11
  pairing selects the adapter and refuses interop.
- **NVRHI fix.** NVRHI's Vulkan backend exported shared memory only when `sharedResourceFlags` was exactly `Shared`. It
  now tests the bit.
- **D3D11 applications** need another way to share (a host copy), or a D3D12 / Vulkan device.
- **Synchronization is explicit.** Nothing orders the devices implicitly; the samples call the two wait methods around
  each hand-over.

## Alternatives

- **An aggregated member reached with `NVRHI_IMPLEMENTS_ROUTE_MEMBER`.** Rejected by the user: the device itself
  implements the interface.
- **D3D11 through CUDA graphics interop** (`cuGraphicsD3D11RegisterResource` plus map/unmap). It handles 3D and mips
  without a size. Rejected by the user in favour of dropping D3D11 interop.
- **Keyed mutexes.** Not needed once D3D11 is out; timeline fences order both remaining APIs.
- **Mapping Vulkan arrays layer by layer at inferred offsets.** Vulkan does not report the layer stride of an optimally
  tiled image, and one CUDA array per layer would not be one texture object for `Texture2DArray`.
