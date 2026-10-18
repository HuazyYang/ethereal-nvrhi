// SPDX-License-Identifier: MIT
#pragma once
#include <nvrhi/nvrhi.h>

namespace nvrhi::cuda
{
// CUDA headers and toolkit types are deliberately not part of this public interface.
struct DeviceDesc
{
    IMessageCallback *errorCB = nullptr;
    int deviceOrdinal = 0;
    bool enableOptiX = true;
};

NVRHI_C_API FRESULT nvrhiCudaCreateDevice(const DeviceDesc *desc, nvrhi::IDevice **device) noexcept;

inline DeviceHandle createDevice(const DeviceDesc &desc)
{
    nvrhi::IDevice *device = nullptr;
    nvrhiCudaCreateDevice(&desc, &device);
    return TakeOver(device);
}
} // namespace nvrhi::cuda
