#include <nvrhi/shader-program.h>
#include <nvrhi/cuda.h>
#include <nvrhi/graphics-interop.h>
#include <cstdio>
#if TEST_VULKAN_DISPATCH
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#endif
int main()
{
    if (!nvrhi::verifyHeaderVersion()) return 1;
    nvrhi::IShaderProgram* program = reinterpret_cast<nvrhi::IShaderProgram*>(1);
    if (nvrhi::createShaderProgram(nullptr, nullptr, 0, &program) != nvrhi::FE_INVALID_ARGS || program) return 2;
    nvrhi::IGraphicsInteropDevice* interop = reinterpret_cast<nvrhi::IGraphicsInteropDevice*>(1);
    if (nvrhi::nvrhiCreateGraphicsInteropDevice(nullptr, &interop) != nvrhi::FE_INVALID_ARGS || interop) return 3;
#if TEST_CUDA_FACTORY
    nvrhi::IDevice* device = reinterpret_cast<nvrhi::IDevice*>(1);
    if (nvrhi::cuda::nvrhiCudaCreateDevice(nullptr, &device) != nvrhi::FE_INVALID_ARGS || device) return 3;
    device = reinterpret_cast<nvrhi::IDevice*>(1);
    if (nvrhi::cuda::nvrhiCudaCreateDeviceWithValidation(nullptr, &device) != nvrhi::FE_INVALID_ARGS || device) return 4;
#endif
    std::puts("Installed NVRHI/interoperability public C exports and header ABI passed");
    return 0;
}
