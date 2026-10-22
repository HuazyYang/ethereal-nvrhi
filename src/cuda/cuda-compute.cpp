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

// A compute pipeline is the launcher of one kernel: its function, block size and parameter layout. The kernel's
// module is loaded once per queue, on first use, so that each queue writes its own SLANG_globalParams.

#include "cuda-backend.h"

#include <nvrhi/common/misc.h>

namespace nvrhi::cuda
{
    ComputePipelineHandle Device::createComputePipeline(const ComputePipelineDesc& desc)
    {
        Shader* shader = desc.CS ? checked_cast<Shader*>(desc.CS.Get()) : nullptr;
        if (!shader || shader->isOptix)
        {
            m_Context.error("createComputePipeline: a CUDA kernel (compute shader) is required");
            return nullptr;
        }

        AutoPtr<ComputePipeline> pipeline = TakeOver(MAKE_RC_OBJ(ComputePipeline, m_Context));
        pipeline->desc = desc;
        pipeline->shader = shader;

        const std::string what = "compute pipeline " + shader->symbolName;
        if (!pipeline->parameters.initialize(m_Context, shader->reflection, what.c_str()))
            return nullptr;

        if (const ShaderEntryPointReflection* entryPoint = shader->entryPoint)
        {
            for (int axis = 0; axis < 3; ++axis)
                pipeline->blockSize[axis] = entryPoint->threadGroupSize[axis] ? entryPoint->threadGroupSize[axis] : 1;
            pipeline->paramsSize = entryPoint->paramsSize;
        }
        else
        {
            m_Context.warning(what + ": no reflection; the kernel is launched with blocks of one thread");
        }

        return pipeline;
    }

    bool ComputePipeline::getKernel(CommandQueue queue, KernelInstance& outKernel)
    {
        if (uint32_t(queue) >= c_NumQueues)
            return false;

        std::lock_guard<std::mutex> lock(m_Mutex);
        KernelInstance& kernel = m_Kernels[uint32_t(queue)];
        if (!kernel.function)
        {
            CUmodule module = shader->library->getModule(queue);
            if (!module)
                return false;

            ContextScope scope(m_Context);
            if (!NVRHI_CUDA_CHECK(m_Context, cuModuleGetFunction(&kernel.function, module, shader->symbolName.c_str())))
            {
                m_Context.error("kernel " + shader->symbolName + " not found in its module");
                kernel = KernelInstance();
                return false;
            }

            if (parameters.globalsSize > 0)
            {
                size_t size = 0;
                if (!NVRHI_CUDA_CHECK(m_Context, cuModuleGetGlobal(&kernel.globals, &size, module, "SLANG_globalParams")))
                {
                    kernel = KernelInstance();
                    return false;
                }
                kernel.globalsSize = size;
                if (size < parameters.globalsSize)
                    m_Context.warning("kernel " + shader->symbolName + ": SLANG_globalParams is smaller than the reflection says");
            }
        }

        outKernel = kernel;
        return true;
    }

    NativeObject ComputePipeline::getNativeObject(ObjectType objectType) noexcept
    {
        if (objectType == ObjectTypes::CUDA_Function)
        {
            KernelInstance kernel;
            if (getKernel(CommandQueue::Graphics, kernel))
                return NativeObject(kernel.function);
        }
        return nullptr;
    }
}
