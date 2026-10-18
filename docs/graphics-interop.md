# Public graphics/CUDA sharing

Link `nvrhi::interop` and `nvrhi::cuda`. Include `nvrhi/graphics-interop.h` and
`nvrhi/cuda.h`; application sharing code does not include D3D, Vulkan or CUDA SDK
headers. `nvrhiCreateGraphicsInteropDevice` selects the configured graphics
backend. The existing CUDA `IInteropDevice` import interface is unchanged.

Create ordinary GPU-only textures with the `textureSharingFlags` returned by
`queryRoute`, buffers with `SharedResourceFlags::Shared`, and all shared resources
with `keepInitialState=false` and nonvirtual allocations. Export them through
`exportBuffer` or `exportTexture`, then pass `IExternalMemory::getDesc()` and the
resource description to the CUDA importer. The export object owns any OS handle
and retains its device/resource. The importer duplicates owned OS handles and
retains the export object through `sourceOwner`. KMT handles are borrowed and
never closed; their resource owner remains retained.

`queryRoute(GraphicsAPI::CUDA, importer->getAdapterIdentity(), capabilities)`
checks the adapter and enabled native route. Unsupported peer APIs, mismatched
adapters, missing Vulkan extensions and unsupported resources fail explicitly;
there is no host-copy fallback. Capability flags describe resource classes, while
export validates the particular resource and allocation.

| Graphics source | Memory | Synchronization |
| --- | --- | --- |
| D3D11 | Unkeyed NT shared 2D textures | D3D11.4 shared timeline fence |
| D3D12 | Shared committed buffers/textures | Shared D3D12 timeline fence |
| Vulkan | Shared dedicated buffers/images | Exported timeline semaphore |

D3D11 uses `SharedResourceFlags::Shared_NTHandleUnkeyed`, which creates a resource
with `D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE`. Its export
owns a separate duplicated NT handle. Existing `Shared_NTHandle` retains its keyed
mutex behavior and must not be combined with the new flag. D3D11 buffers, keyed
mutexes and legacy KMT exports are rejected by this companion. The lower-level
CUDA importer still accepts native KMT and keyed-mutex descriptors for callers
that manage those routes. KMT external arrays faulted on valid 65x33 textures on
the tested CUDA 12.9/RTX 4050 setup; padding the size or changing array flags did not
resolve it. The public route therefore selects NT handles instead of advertising
KMT texture support. No resource dimensions are rounded or padded as a workaround.

Adding the route's creation flags changes its structure ABI: header version is 30
and `IGraphicsInteropDevice` has a new interface ID. Rebuild callers and use
`verifyHeaderVersion` when loading separately built NVRHI libraries.

Virtual/placed, tiled, multisampled, depth/stencil and compressed allocations are
not exported here. Existing native-descriptor imports remain available for callers
that already manage placed heap offsets themselves.

For Vulkan device creation, enable `VK_KHR_external_memory_win32` and
`VK_KHR_external_semaphore_win32` on Windows, or their `_fd` counterparts on Linux.
The existing Vulkan backend already requires timeline semaphore support. The
exporter checks the enabled extensions and native external timeline capability.

## Ordering

Use a shared fence from `createFence`, import its descriptor on CUDA, and use
monotonically increasing values. Serialize queue submissions and interop calls to
each queue, as with existing NVRHI queue APIs.

1. Record graphics writes, then `releaseResource` as the final use of each shared
   resource. Submit that command list and call graphics `signalFence(value)`.
2. Record CUDA `waitFence(value)`, dispatch the Slang kernel using the imported
   ordinary NVRHI resources, then CUDA `signalFence(value+1)`; submit.
3. Call graphics `waitFence(value+1)`, record `acquireResource` with the intended
   NVRHI state, then record graphics reads or presentation work and submit.

The graphics calls enqueue GPU operations immediately and retain fence objects
through completion. Vulkan release/acquire transfers whole-resource ownership
through `VK_QUEUE_FAMILY_EXTERNAL` with all image subresources in `GENERAL`.
D3D12 transfers use `COMMON`. Automatic initial-state restoration must remain off
so closing a released command list cannot transition externally owned memory.
Before the first graphics use, initialize state tracking normally; reacquisition
restores tracking itself. The same resource must not be accessed on both sides
without the corresponding ownership/fence sequence.

`ethereal-samples/src/slang_compute/InteropTests.cpp` is an SDK-free usage example.
Its Slang kernel updates 65x33 shared textures and buffer contents over three round trips;
GPU waits/signals order all cross-API work, and host readback happens only afterward
for verification. It also checks route rejection and retained export lifetimes.
Linux and physical multi-GPU validation remain separate acceptance runs.

Native contracts: [CUDA external resources](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EXTRES__INTEROP.html),
[Vulkan external memory and synchronization](https://docs.vulkan.org/guide/latest/extensions/external.html),
[Direct3D 11 resource sharing flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_resource_misc_flag),
[Direct3D 11 shared fences](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-createfence).

## Verified configurations

Windows on RTX 4050/CUDA 12.9: all three public 65x33 tests pass. Native Direct3D 11,
Direct3D 12 and Vulkan validation are clean, including resource teardown. CUDA
Compute Sanitizer reports zero errors for each graphics API's imported resources.
`--validation` enables the native graphics layer; test logging turns validation
warnings/errors, including teardown leaks, into a failing exit status.

## Installed static Vulkan consumers

The static Vulkan backend keeps the application-owned Vulkan-Hpp dynamic dispatcher
contract. An application linking the installed `nvrhi_vk` target (including through
`nvrhi::interop`) must link `Vulkan::Headers` and define dispatcher storage in exactly
one translation unit:

```cpp
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
```

Initialize that dispatcher for the application's Vulkan instance and device before
using the static backend. The shared NVRHI library supplies and initializes its own
dispatcher. See `tests/installed-consumer` for the static and shared package link
checks; neither configuration imports the Slang compiler into the runtime package.
