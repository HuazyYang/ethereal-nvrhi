/*
* Copyright (c) 2026, ethereal contributors. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma once

// The device constructors of the backends, called by the graphics factories (ADR 0014): createDevice wraps the
// caller's native objects with them, and createDeviceOnAdapter calls them once it has created the native objects.
// Module-private: they are not exported. Each one returns FS_OK and a new reference in *ppDevice, or an FE_* code
// and nullptr; ppDevice is not null.

#include <nvrhi/nvrhi.h>

namespace nvrhi::d3d11
{
    struct DeviceDesc;
    FRESULT CreateDevice(const DeviceDesc& desc, nvrhi::IDevice** ppDevice) noexcept;
}

namespace nvrhi::d3d12
{
    struct DeviceDesc;
    struct IDevice;
    FRESULT CreateDevice(const DeviceDesc& desc, IDevice** ppDevice) noexcept;
}

namespace nvrhi::vulkan
{
    struct DeviceDesc;
    struct IDevice;
    // What the factory hands over with a device that it created (vulkan-backend.h); null for a wrapped device.
    struct NativeDeviceOwnership;
    FRESULT CreateDevice(const DeviceDesc& desc, const NativeDeviceOwnership* ownership, IDevice** ppDevice) noexcept;
}

namespace nvrhi::cuda
{
    struct DeviceDesc;
    struct IDevice;
    // Needs the driver: the factory loads it first (LoadDriver in cuda-driver.h).
    FRESULT CreateDevice(const DeviceDesc& desc, IDevice** ppDevice) noexcept;
}

namespace nvrhi::validation
{
    FRESULT CreateValidationLayer(nvrhi::IDevice* underlyingDevice, nvrhi::IDevice** ppDevice) noexcept;
}
