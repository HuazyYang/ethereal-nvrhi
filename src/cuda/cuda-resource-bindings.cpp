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

// Binding layouts and sets keep the NVRHI descriptions only: there are no descriptors on CUDA. A kernel's global
// parameters are the fields of its SLANG_globalParams block; the reflection names the HLSL register of each
// (ShaderTool lays the program out for an HLSL target as well), so a binding set item is placed by its register
// class, register space and slot. The command list writes the block when it records a dispatch.

#include "cuda-backend.h"

#include <nvrhi/common/misc.h>

#include <algorithm>

namespace nvrhi::cuda
{
    RegisterClass GetRegisterClass(ResourceType type)
    {
        switch (type)
        {
        case ResourceType::Texture_SRV:
        case ResourceType::TypedBuffer_SRV:
        case ResourceType::StructuredBuffer_SRV:
        case ResourceType::RawBuffer_SRV:
        case ResourceType::RayTracingAccelStruct:
            return RegisterClass::SRV;
        case ResourceType::Texture_UAV:
        case ResourceType::TypedBuffer_UAV:
        case ResourceType::StructuredBuffer_UAV:
        case ResourceType::RawBuffer_UAV:
        case ResourceType::SamplerFeedbackTexture_UAV:
            return RegisterClass::UAV;
        case ResourceType::ConstantBuffer:
        case ResourceType::VolatileConstantBuffer:
            return RegisterClass::CBV;
        case ResourceType::Sampler:
            return RegisterClass::Sampler;
        case ResourceType::PushConstants:
            return RegisterClass::PushConstants;
        default:
            return RegisterClass::None;
        }
    }

    RegisterClass GetRegisterClass(ShaderParameterKind kind)
    {
        switch (kind)
        {
        case ShaderParameterKind::Texture_SRV:
        case ShaderParameterKind::TypedBuffer_SRV:
        case ShaderParameterKind::StructuredBuffer_SRV:
        case ShaderParameterKind::RawBuffer_SRV:
        case ShaderParameterKind::RayTracingAccelStruct:
            return RegisterClass::SRV;
        case ShaderParameterKind::Texture_UAV:
        case ShaderParameterKind::TypedBuffer_UAV:
        case ShaderParameterKind::StructuredBuffer_UAV:
        case ShaderParameterKind::RawBuffer_UAV:
            return RegisterClass::UAV;
        case ShaderParameterKind::ConstantBuffer:
            return RegisterClass::CBV;
        case ShaderParameterKind::Sampler:
            return RegisterClass::Sampler;
        case ShaderParameterKind::PushConstants:
            return RegisterClass::PushConstants;
        default:
            return RegisterClass::None;
        }
    }

    bool ParameterMap::initialize(const Context& context, const ShaderLibraryReflection* reflection, const char* what)
    {
        m_Slots.clear();
        globalsSize = 0;
        this->reflection = reflection;
        if (!reflection)
            return true;

        if (reflection->target != ShaderCodeTarget::CudaPTX && reflection->target != ShaderCodeTarget::OptixPTX &&
            reflection->target != ShaderCodeTarget::OptixIR)
        {
            context.error(std::string(what) + ": the reflection does not describe CUDA code");
            return false;
        }

        globalsSize = reflection->globalParamsSize;
        for (uint32_t i = 0; i < reflection->globalParameterCount && i < reflection->parameters.size(); ++i)
            addParameter(context, i, what);
        return true;
    }

    bool ParameterMap::initializeEntryPoint(const Context& context, const ShaderLibraryReflection* reflection,
        const ShaderEntryPointReflection* entryPoint, const char* what)
    {
        m_Slots.clear();
        globalsSize = 0;
        m_IgnoreSpace = true;
        this->reflection = reflection;
        if (!reflection || !entryPoint)
            return true;

        globalsSize = entryPoint->paramsSize;
        for (uint32_t i = 0; i < entryPoint->parameterCount; ++i)
        {
            const uint32_t index = entryPoint->firstParameter + i;
            if (index < reflection->parameters.size())
                addParameter(context, index, what);
        }
        return true;
    }

    void ParameterMap::addParameter(const Context& context, uint32_t index, const char* what)
    {
        const ShaderParameterReflection& parameter = reflection->parameters[index];
        const RegisterClass registerClass = GetRegisterClass(parameter.kind);
        if (registerClass == RegisterClass::None)
        {
            // Uniform data outside of a constant buffer has no NVRHI binding.
            if (parameter.kind == ShaderParameterKind::Uniform && !parameter.isEntryPointParameter)
                context.warning(std::string(what) + ": global uniform '" + parameter.name.c_str() +
                    "' cannot be bound; put it in a constant buffer or use entry point parameters");
            return;
        }

        ParameterSlot slot;
        slot.parameterIndex = index;
        slot.registerClass = registerClass;
        slot.registerSpace = parameter.registerSpace;
        slot.firstSlot = parameter.registerSlot;
        slot.slotCount = parameter.arraySize == 0 ? 1 : parameter.arraySize;
        slot.offset = parameter.uniformOffset;
        slot.stride = parameter.uniformStride;
        if (slot.stride == 0 && parameter.arraySize > 0 && parameter.arraySize != c_ShaderReflectionUnboundedArray)
            slot.stride = parameter.uniformSize / parameter.arraySize;
        slot.kind = parameter.kind;
        m_Slots.push_back(slot);
    }

    const ParameterSlot* ParameterMap::find(RegisterClass registerClass, uint32_t registerSpace, uint32_t slot) const
    {
        for (const ParameterSlot& candidate : m_Slots)
        {
            if (candidate.registerClass == registerClass && (m_IgnoreSpace || candidate.registerSpace == registerSpace) &&
                slot >= candidate.firstSlot && slot - candidate.firstSlot < candidate.slotCount)
                return &candidate;
        }
        return nullptr;
    }

    BindingLayoutHandle Device::createBindingLayout(const BindingLayoutDesc& desc)
    {
        BindingLayout* layout = MAKE_RC_OBJ(BindingLayout);
        layout->desc = desc;
        return TakeOver(layout);
    }

    BindingSetHandle Device::createBindingSet(const BindingSetDesc& desc, IBindingLayout* layout)
    {
        if (!layout || !layout->getDesc())
        {
            m_Context.error("createBindingSet: a regular binding layout is required");
            return nullptr;
        }

        AutoPtr<BindingSet> set = TakeOver(MAKE_RC_OBJ(BindingSet));
        set->desc = desc;
        set->layout = layout;
        for (const BindingSetItem& item : desc.bindings)
        {
            if (item.resourceHandle)
                set->resources.push_back(item.resourceHandle);
        }
        return set;
    }
}
