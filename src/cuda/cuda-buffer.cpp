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

#include "cuda-backend.h"

#include <nvrhi/common/misc.h>

namespace nvrhi::cuda
{
    Buffer::~Buffer()
    {
        if (!devicePtr && !hostPtr && !externalMemory)
            return;

        ContextScope scope(m_Context);
        if (ownsMemory)
        {
            if (hostPtr)
                g_Cuda.cuMemFreeHost(hostPtr);
            else if (devicePtr)
                g_Cuda.cuMemFree(devicePtr);
        }
        else if (externalMemory && devicePtr)
        {
            // A buffer mapped from external memory is freed with cuMemFree before the memory object is destroyed.
            g_Cuda.cuMemFree(devicePtr);
        }
        if (externalMemory)
            g_Cuda.cuDestroyExternalMemory(externalMemory);
    }

    NativeObject Buffer::getNativeObject(ObjectType objectType) noexcept
    {
        switch (objectType)
        {
        case ObjectTypes::CUDA_DevicePtr:
            return NativeObject(uintptr_t(devicePtr));
        case ObjectTypes::CUDA_ExternalMemory:
            return NativeObject(externalMemory);
        default:
            return nullptr;
        }
    }

    BufferHandle Device::createBuffer(const BufferDesc& desc)
    {
        if (desc.isVirtual)
        {
            m_Context.error("virtual buffers are not supported");
            return nullptr;
        }
        if (desc.sharedResourceFlags != SharedResourceFlags::None)
        {
            m_Context.error("CUDA buffers cannot be shared with other devices; import a graphics buffer with "
                "IInteropDevice::importBuffer instead");
            return nullptr;
        }

        ContextScope scope(m_Context);
        AutoPtr<Buffer> buffer = TakeOver(MAKE_RC_OBJ(Buffer, m_Context));
        buffer->desc = desc;
        const size_t size = size_t(desc.byteSize ? desc.byteSize : 1);

        if (desc.cpuAccess != CpuAccessMode::None)
        {
            // Pinned host memory, also visible to the device: copies into and out of it run on the queues.
            void* host = nullptr;
            if (!NVRHI_CUDA_CHECK(m_Context, cuMemHostAlloc(&host, size, CU_MEMHOSTALLOC_DEVICEMAP | CU_MEMHOSTALLOC_PORTABLE)))
                return nullptr;
            buffer->hostPtr = host;
            if (!NVRHI_CUDA_CHECK(m_Context, cuMemHostGetDevicePointer(&buffer->devicePtr, host, 0)))
                return nullptr;
        }
        else
        {
            if (!NVRHI_CUDA_CHECK(m_Context, cuMemAlloc(&buffer->devicePtr, size)))
                return nullptr;
        }

        return buffer;
    }

    BufferHandle Device::createHandleForNativeBuffer(ObjectType objectType, NativeObject nativeBuffer, const BufferDesc& desc)
    {
        if (objectType != ObjectTypes::CUDA_DevicePtr || !nativeBuffer)
            return nullptr;

        AutoPtr<Buffer> buffer = TakeOver(MAKE_RC_OBJ(Buffer, m_Context));
        buffer->desc = desc;
        buffer->devicePtr = CUdeviceptr(uintptr_t(nativeBuffer));
        buffer->ownsMemory = false;
        return buffer;
    }

    void* Device::mapBuffer(IBuffer* _buffer, CpuAccessMode cpuAccess) noexcept
    {
        (void)cpuAccess;
        Buffer* buffer = checked_cast<Buffer*>(_buffer);
        if (!buffer)
            return nullptr;
        if (!buffer->hostPtr)
        {
            m_Context.error("mapBuffer: the buffer was not created with cpuAccess");
            return nullptr;
        }
        // The memory is coherent pinned memory: reading it after the copies into it have finished (waitForIdle,
        // an event query) returns their results.
        return buffer->hostPtr;
    }
}
