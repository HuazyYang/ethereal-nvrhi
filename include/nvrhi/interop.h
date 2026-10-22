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

// Interoperation between a compute device (CUDA) and the graphics device it was paired with (ADR 0013).
//
// The compute device implements IInteropDevice itself: query it from the device.
//
//     nvrhi::InteropDeviceHandle interop;
//     cudaDevice->QueryInterface(NVRHI_IID_PPV_ARGS(&interop));
//
// Supported graphics APIs: D3D12, and Vulkan with VK_KHR_external_memory_win32 / _fd and
// VK_KHR_external_semaphore_win32 / _fd enabled on the device. D3D11 is not supported (it cannot report the
// allocation size of a resource, which the import needs); every method then returns FE_UNSUPPORTED.

#include <nvrhi/nvrhi.h>

namespace nvrhi
{
    NVRHI_IID(IInteropDevice, "58b4b155-74af-49ba-99d5-15fb6a38b14f")
    struct IInteropDevice : IRHIObject
    {
        NVRHI_DECLARE_UUID_TRAITS(IInteropDevice)

        // The paired graphics device (not AddRef'd), or nullptr if the compute device was created without one.
        virtual nvrhi::IDevice* getGraphicsDevice() noexcept = 0;
        // The compute device (not AddRef'd): this object, seen as its IDevice.
        virtual nvrhi::IDevice* getComputeDevice() noexcept = 0;
        // True if the graphics device is paired and its API and extensions allow sharing.
        virtual bool isInteropSupported() noexcept = 0;

        // Imports a buffer of the graphics device that was created with SharedResourceFlags::Shared. The compute
        // buffer aliases the graphics memory and keeps a reference to graphicsBuffer.
        // Returns FS_OK and a new reference in *ppBuffer, or an FE_* code and nullptr.
        virtual FRESULT importBuffer(IBuffer* graphicsBuffer, IBuffer** ppBuffer) noexcept = 0;

        // Imports a texture of the graphics device that was created with SharedResourceFlags::Shared, with all its
        // mip levels and array slices (1D, 2D, 3D, arrays and cube maps). The compute texture aliases the graphics
        // memory and keeps a reference to graphicsTexture. Formats with 3 components, depth-stencil formats,
        // multisampled, tiled and virtual textures are not supported (FE_UNSUPPORTED). Writing to the texture from
        // the compute device needs TextureDesc::isUAV.
        // Returns FS_OK and a new reference in *ppTexture, or an FE_* code and nullptr.
        virtual FRESULT importTexture(ITexture* graphicsTexture, ITexture** ppTexture) noexcept = 0;

        // Orders work across the devices through shared timeline fences (D3D12 fences, Vulkan timeline semaphores).
        //
        // Compute work executed on computeQueue after this call starts after the graphics work that was executed
        // on graphicsQueue before this call has finished.
        virtual FRESULT computeQueueWaitForGraphicsQueue(CommandQueue computeQueue, CommandQueue graphicsQueue) noexcept = 0;

        // Graphics work executed on graphicsQueue after this call starts after the compute work that was executed
        // on computeQueue before this call has finished.
        virtual FRESULT graphicsQueueWaitForComputeQueue(CommandQueue graphicsQueue, CommandQueue computeQueue) noexcept = 0;
    };

    typedef AutoPtr<IInteropDevice> InteropDeviceHandle;
}
