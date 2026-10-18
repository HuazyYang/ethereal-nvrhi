# Public graphics/CUDA sharing

Link `nvrhi::interop` and `nvrhi::cuda`. Include `nvrhi/graphics-interop.h` and
`nvrhi/cuda.h`; application sharing code does not include D3D, Vulkan or CUDA SDK
headers. `nvrhiCreateGraphicsInteropDevice` selects the configured graphics
backend. The existing CUDA `IInteropDevice` import interface is unchanged.

Create ordinary GPU-only resources with `SharedResourceFlags::Shared`,
`keepInitialState=false`, and nonvirtual allocations. Export them through
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
| D3D11 | Shared KMT 2D textures | D3D11.4 shared timeline fence |
| D3D12 | Shared committed buffers/textures | Shared D3D12 timeline fence |
| Vulkan | Shared dedicated buffers/images | Exported timeline semaphore |

D3D11 buffers and the NT/keyed-mutex export route are rejected by this companion.
The existing lower-level CUDA importer still supports explicit keyed-mutex imports.
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
Its Slang kernel updates shared texture and buffer contents over three round trips;
GPU waits/signals order all cross-API work, and host readback happens only afterward
for verification. It also checks route rejection and retained export lifetimes.
Linux and physical multi-GPU validation remain separate acceptance runs.

Native contracts: [CUDA external resources](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EXTRES__INTEROP.html),
[Vulkan external memory and synchronization](https://docs.vulkan.org/guide/latest/extensions/external.html),
[Direct3D 11 shared fences](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-createfence).

## Verified configurations

Windows on RTX 4050: all three public-path tests pass. Native Direct3D 11,
Direct3D 12 and Vulkan validation are clean, including resource teardown. CUDA
Compute Sanitizer reports zero errors for each graphics API's imported resources.
`--validation` enables the native graphics layer; test logging turns validation
warnings/errors, including teardown leaks, into a failing exit status.
