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

// Opt-in OptiX SDK validation; requires enableOptiX. Existing DeviceDesc ABI is unchanged.
// Diagnostic callbacks may arrive from SDK worker threads. A failed launch/completion
// invalidates its outputs; device faults can require process restart.
NVRHI_C_API FRESULT nvrhiCudaCreateDeviceWithValidation(const DeviceDesc *desc, nvrhi::IDevice **device) noexcept;

inline DeviceHandle createDeviceWithValidation(const DeviceDesc &desc)
{
    nvrhi::IDevice *device = nullptr;
    nvrhiCudaCreateDeviceWithValidation(&desc, &device);
    return TakeOver(device);
}

inline DeviceHandle createDevice(const DeviceDesc &desc)
{
    nvrhi::IDevice *device = nullptr;
    nvrhiCudaCreateDevice(&desc, &device);
    return TakeOver(device);
}
} // namespace nvrhi::cuda
