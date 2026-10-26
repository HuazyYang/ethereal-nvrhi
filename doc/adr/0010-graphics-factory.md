# 0010. The graphics factory creates every device and the validation layer

- Status: Superseded by [0014](0014-per-backend-graphics-factories-and-swap-chains.md) (2026-10-26): one factory per backend, each with its own descriptor and export.
- Date: 2026-10-22
- Plan: [`../plans/2026-10-22-slang-cuda-optix-backend.md`](../plans/2026-10-22-slang-cuda-optix-backend.md), step 2

## Context

Each backend exported its own constructor: `nvrhiD3D11CreateDevice`, `nvrhiD3D12CreateDevice` and
`nvrhiVulkanCreateDevice`, with the inline wrappers `d3d11::createDevice`, `d3d12::createDevice` and
`vulkan::createDevice`. The validation layer had `nvrhiCreateValidationLayer` / `validation::createValidationLayer`.

That is four free C exports for one job. Adding the CUDA backend ([ADR 0011](0011-cuda-optix-backend.md)) would make
five. An application that wanted to choose a backend at run time also had to link every backend library, and it had no
way to ask which backends a build contains.

## Decision

`IGraphicsFactory : IObject` (`include/nvrhi/factory.h`) is the one entry point that creates devices. It is created by
the only creation export, `nvrhiCreateGraphicsFactory(const GraphicsFactoryDesc*, IGraphicsFactory**)`, which has an
inline `createGraphicsFactory(desc)` wrapper.

- **One creation method per backend.** `createD3D11Device`, `createD3D12Device`, `createVulkanDevice` and
  `createCudaDevice` each take that backend's existing `DeviceDesc`. There are no overloads (§3.1.5 of
  [`../com-interface-abi.md`](../com-interface-abi.md)), and no `const void*` descriptor that would give up type
  checking. A backend that was not compiled in returns `FE_UNSUPPORTED`. `createValidationLayer(IDevice*, IDevice**)`
  wraps any device.
- **`isBackendAvailable(GraphicsAPI)`** reports the backends that are compiled in. For the CUDA backend it also loads the
  driver library.
- **Descriptors stay where they are.** The backend headers keep their `DeviceDesc` and their native-API interfaces.
  `factory.h` only forward-declares them, so it pulls in no D3D, Vulkan or CUDA header.
- **The per-backend exports are removed.** No backend constructor is exported any more. Each backend has a
  module-private `CreateDevice(const DeviceDesc&, IDevice**)`, declared in `src/common/backend-factories.h`.
- **Build.** In a shared build the factory is part of `nvrhi.dll`. In a static build it is the `nvrhi_factory` library.
  That library links every backend library of the build, followed by `nvrhi` (in link order for single-pass linkers).
  An application links `nvrhi_factory` alone.
- **Factory messages.** The factory reports its own problems (a backend that is not compiled in, a driver that does not
  load) through an optional `IMessageCallback` in `GraphicsFactoryDesc`. Each device still reports through the callback
  in its own descriptor.

## Consequences

- donut's `DeviceManager` owns one factory (`GetGraphicsFactory()`) and creates its device and validation layer with
  it. Applications use the same factory for further devices, for example a CUDA device paired with
  `GetDevice()`.
- Callers outside NVRHI change from `nvrhi::d3d12::createDevice(desc)` to `factory->createD3D12Device(desc, &device)`.
  In this tree that is donut's three device managers, the WaveWorks SDK and NVRHI's own tests. This is an API break.
  Per [ADR 0012](0012-offline-slang-shader-container-and-reflection.md) it does not bump `c_HeaderVersion`.
- The Vulkan dynamic loader setup for shared builds moved with the device constructor, unchanged.

## Alternatives

- **Keep the per-backend exports and add `nvrhiCudaCreateDevice`.** This is the smallest change, but it keeps the
  problems listed above, and the user asked for the factory.
- **One `createDevice(GraphicsAPI, const void* desc)`.** This removes the per-backend methods, but the descriptor would
  no longer be type-checked.
- **A factory per backend** (`ID3D12Factory`, ...). This adds interfaces without adding anything the methods above
  lack.
