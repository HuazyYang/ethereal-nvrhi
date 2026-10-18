// SPDX-License-Identifier: MIT
#include <nvrhi/graphics-interop.h>
namespace nvrhi
{
FRESULT nvrhiCreateGraphicsInteropDevice(IDevice *device, IGraphicsInteropDevice **output) noexcept
{
    if (!output)
        return FE_INVALID_ARGS;
    *output = nullptr;
    if (!device)
        return FE_INVALID_ARGS;
    switch (device->getGraphicsAPI())
    {
#if NVRHI_INTEROP_DX11
    case GraphicsAPI::D3D11:
        return d3d11::nvrhiD3D11CreateGraphicsInteropDevice(device, output);
#endif
#if NVRHI_INTEROP_DX12
    case GraphicsAPI::D3D12:
        return d3d12::nvrhiD3D12CreateGraphicsInteropDevice(device, output);
#endif
#if NVRHI_INTEROP_VULKAN
    case GraphicsAPI::VULKAN:
        return vulkan::nvrhiVulkanCreateGraphicsInteropDevice(device, output);
#endif
    default:
        return FE_UNSUPPORTED;
    }
}
} // namespace nvrhi
