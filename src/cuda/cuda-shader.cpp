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

// Shader libraries hold the code of an NVRHI shader container (ADR 0012): the CUDA kernels (PTX) and the OptiX
// programs (PTX or OptiX-IR), each with its reflection. A shader is one entry point of a library: a CUDA kernel,
// which a compute pipeline launches, or an OptiX program, which a ray tracing pipeline links.

#include "cuda-backend.h"

#include <nvrhi/common/misc.h>

#include <cstring>

namespace nvrhi::cuda
{
    namespace
    {
        bool IsRayTracingStage(ShaderType type)
        {
            return (type & ShaderType::AllRayTracing) != 0;
        }

        bool ReadPart(const void* binary, size_t binarySize, std::initializer_list<ShaderCodeTarget> targets,
            ShaderLibrary::Part& part, std::string& error)
        {
            bool present = false;
            for (ShaderCodeTarget target : targets)
                present = present || nvrhiGetShaderContainerCode(binary, binarySize, target, nullptr, nullptr);
            if (!present)
                return true;

            ShaderBinary shaderBinary;
            if (!OpenShaderBinary(binary, binarySize, targets, shaderBinary, error))
                return false;

            const char* code = static_cast<const char*>(shaderBinary.code);
            part.target = shaderBinary.target;
            part.code.assign(code, code + shaderBinary.codeSize);
            if (part.target != ShaderCodeTarget::OptixIR && (part.code.empty() || part.code.back() != '\0'))
                part.code.push_back('\0');
            part.reflection = std::move(shaderBinary.reflection);
            return true;
        }
    }

    bool ShaderLibrary::initialize(const void* binary, size_t binarySize, std::string& error)
    {
        if (!binary || binarySize == 0)
        {
            error = "empty shader binary";
            return false;
        }

        if (!nvrhiIsShaderContainer(binary, binarySize))
        {
            // Plain PTX: compute code without reflection. Only push constants (the kernel arguments) can be bound.
            compute.target = ShaderCodeTarget::CudaPTX;
            const char* code = static_cast<const char*>(binary);
            compute.code.assign(code, code + binarySize);
            if (compute.code.back() != '\0')
                compute.code.push_back('\0');
            return true;
        }

        if (!ReadPart(binary, binarySize, { ShaderCodeTarget::CudaPTX }, compute, error) ||
            !ReadPart(binary, binarySize, { ShaderCodeTarget::OptixIR, ShaderCodeTarget::OptixPTX }, optix, error))
            return false;

        if (compute.empty() && optix.empty())
        {
            error = "the NVRHI shader container has no CUDA or OptiX code";
            return false;
        }
        return true;
    }

    ShaderLibrary::~ShaderLibrary()
    {
        ContextScope scope(m_Context);
        for (CUmodule module : m_Modules)
        {
            if (module)
                g_Cuda.cuModuleUnload(module);
        }
    }

    CUmodule ShaderLibrary::getModule(CommandQueue queue)
    {
        if (compute.empty() || uint32_t(queue) >= c_NumQueues)
            return nullptr;

        std::lock_guard<std::mutex> lock(m_Mutex);
        CUmodule& module = m_Modules[uint32_t(queue)];
        if (module)
            return module;

        char log[4096] = {};
        CUjit_option options[] = { CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES };
        void* values[] = { log, reinterpret_cast<void*>(uintptr_t(sizeof(log))) };

        ContextScope scope(m_Context);
        const CUresult result = g_Cuda.cuModuleLoadDataEx(&module, compute.code.data(), 2, options, values);
        if (result != CUDA_SUCCESS)
        {
            m_Context.error("cuModuleLoadDataEx failed: " + ResultToString(result) + (log[0] ? std::string("\n") + log : std::string()));
            module = nullptr;
        }
        return module;
    }

    void ShaderLibrary::getBytecode(const void** ppBytecode, size_t* pSize) const noexcept
    {
        const Part& part = compute.empty() ? optix : compute;
        if (ppBytecode) *ppBytecode = part.code.data();
        if (pSize) *pSize = part.code.size();
    }

    ShaderHandle ShaderLibrary::getShader(const char* entryName, ShaderType shaderType, const ShaderDesc* desc)
    {
        if (!entryName)
            return nullptr;

        const bool isOptix = IsRayTracingStage(shaderType);
        const Part& part = isOptix ? optix : compute;
        if (part.empty())
        {
            m_Context.error(std::string("shader library has no ") + (isOptix ? "OptiX" : "CUDA") + " code for " + entryName);
            return nullptr;
        }

        AutoPtr<Shader> shader = TakeOver(MAKE_RC_OBJ(Shader));
        if (desc)
            shader->desc = *desc;
        shader->desc.shaderType = shaderType;
        shader->desc.entryName = entryName;
        shader->library = this;
        shader->isOptix = isOptix;
        shader->reflection = part.reflection.Get();
        shader->entryPoint = FindEntryPoint(shader->reflection, entryName, shaderType);

        if (shader->reflection && !shader->entryPoint)
        {
            m_Context.error(std::string("entry point ") + entryName + " not found in the shader library");
            return nullptr;
        }
        shader->symbolName = shader->entryPoint ? shader->entryPoint->symbolName.c_str() : entryName;
        return shader;
    }

    ShaderLibraryHandle Device::createShaderLibrary(const void* binary, size_t binarySize)
    {
        AutoPtr<ShaderLibrary> library = TakeOver(MAKE_RC_OBJ(ShaderLibrary, m_Context));
        std::string error;
        if (!library->initialize(binary, binarySize, error))
        {
            m_Context.error("createShaderLibrary: " + error);
            return nullptr;
        }
        return library;
    }

    ShaderHandle Device::createShader(const ShaderDesc& desc, const void* binary, size_t binarySize)
    {
        // A shader of its own: a library with one entry point taken from it.
        ShaderLibraryHandle library = createShaderLibrary(binary, binarySize);
        if (!library)
            return nullptr;
        return checked_cast<ShaderLibrary*>(library.Get())->getShader(desc.entryName.c_str(), desc.shaderType, &desc);
    }
}
