// SPDX-License-Identifier: MIT
#include <nvrhi/cuda.h>
#include <cstdio>
#include <initializer_list>
int main()
{
    using namespace nvrhi;
    cuda::DeviceDesc desc;
    for (auto factory : {cuda::nvrhiCudaCreateDevice, cuda::nvrhiCudaCreateDeviceWithValidation})
    {
        if (factory(&desc, nullptr) != FE_INVALID_ARGS)
            return 1;
        IDevice *out = reinterpret_cast<IDevice *>(1);
        if (factory(nullptr, &out) != FE_INVALID_ARGS || out)
            return 2;
        desc.deviceOrdinal = -1;
        out = reinterpret_cast<IDevice *>(1);
        if (factory(&desc, &out) != FE_INVALID_ARGS || out)
            return 3;
        desc.deviceOrdinal = 0;
    }
    desc.enableOptiX = false;
    IDevice *out = reinterpret_cast<IDevice *>(1);
    if (cuda::nvrhiCudaCreateDeviceWithValidation(&desc, &out) != FE_INVALID_ARGS || out)
        return 4;
#if !TEST_CUDA_OPTIX_SUPPORTED
    desc.enableOptiX = true;
    out = reinterpret_cast<IDevice *>(1);
    if (cuda::nvrhiCudaCreateDeviceWithValidation(&desc, &out) != FE_UNSUPPORTED || out)
        return 5;
#endif
    std::puts("CUDA descriptor errors rejected before driver initialization");
    return 0;
}
