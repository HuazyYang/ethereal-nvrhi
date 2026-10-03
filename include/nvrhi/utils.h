/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
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

#include <nvrhi/nvrhi.h>

namespace nvrhi::utils
{
    // C exports behind the helpers below (see <nvrhi/common/export.h>): aggregates go in and out through pointers,
    // interfaces through IXxx**.

    NVRHI_C_API void nvrhiUtilsCreateAddBlendState(BlendFactor srcBlend, BlendFactor dstBlend,
        BlendState::RenderTarget* pResult) noexcept;
    NVRHI_C_API void nvrhiUtilsCreateStaticConstantBufferDesc(uint32_t byteSize, const char* debugName,
        BufferDesc* pResult) noexcept;
    NVRHI_C_API void nvrhiUtilsCreateVolatileConstantBufferDesc(uint32_t byteSize, const char* debugName,
        uint32_t maxVersions, BufferDesc* pResult) noexcept;
    // *inoutLayout and *inoutSet: an existing object, which is used as is (no reference is added or released),
    // or nullptr, in which case the object is created and returned with a new reference.
    NVRHI_C_API bool nvrhiUtilsCreateBindingSetAndLayout(IDevice* device, ShaderType visibility,
        uint32_t registerSpace, const BindingSetDesc* bindingSetDesc, IBindingLayout** inoutLayout,
        IBindingSet** inoutSet, bool registerSpaceIsDescriptorSet) noexcept;
    NVRHI_C_API void nvrhiUtilsClearColorAttachment(ICommandList* commandList, IFramebuffer* framebuffer,
        uint32_t attachmentIndex, const Color* color) noexcept;
    NVRHI_C_API void nvrhiUtilsClearDepthStencilAttachment(ICommandList* commandList, IFramebuffer* framebuffer,
        float depth, uint32_t stencil) noexcept;
    NVRHI_C_API void nvrhiUtilsBuildBottomLevelAccelStruct(ICommandList* commandList, rt::IAccelStruct* as,
        const rt::AccelStructDesc* desc) noexcept;
    NVRHI_C_API void nvrhiUtilsTextureUavBarrier(ICommandList* commandList, ITexture* texture) noexcept;
    NVRHI_C_API void nvrhiUtilsBufferUavBarrier(ICommandList* commandList, IBuffer* buffer) noexcept;
    NVRHI_C_API Format nvrhiUtilsChooseFormat(IDevice* device, FormatSupport requiredFeatures,
        const Format* requestedFormats, size_t requestedFormatCount) noexcept;
    NVRHI_C_API const char* nvrhiUtilsGraphicsAPIToString(GraphicsAPI api) noexcept;
    NVRHI_C_API const char* nvrhiUtilsTextureDimensionToString(TextureDimension dimension) noexcept;
    NVRHI_C_API const char* nvrhiUtilsShaderStageToString(ShaderType stage) noexcept;
    NVRHI_C_API const char* nvrhiUtilsResourceTypeToString(ResourceType type) noexcept;
    NVRHI_C_API const char* nvrhiUtilsFormatToString(Format format) noexcept;
    NVRHI_C_API const char* nvrhiUtilsCommandQueueToString(CommandQueue queue) noexcept;

    inline BlendState::RenderTarget CreateAddBlendState(
        BlendFactor srcBlend,
        BlendFactor dstBlend)
    {
        BlendState::RenderTarget result;
        nvrhiUtilsCreateAddBlendState(srcBlend, dstBlend, &result);
        return result;
    }

    inline BufferDesc CreateStaticConstantBufferDesc(
        uint32_t byteSize,
        const char* debugName)
    {
        BufferDesc result;
        nvrhiUtilsCreateStaticConstantBufferDesc(byteSize, debugName, &result);
        return result;
    }

    inline BufferDesc CreateVolatileConstantBufferDesc(
        uint32_t byteSize,
        const char* debugName,
        uint32_t maxVersions)
    {
        BufferDesc result;
        nvrhiUtilsCreateVolatileConstantBufferDesc(byteSize, debugName, maxVersions, &result);
        return result;
    }

    // Creates bindingLayout (from the items of bindingSetDesc) if it is null, then bindingSet if it is null.
    inline bool CreateBindingSetAndLayout(
        IDevice* device,
        nvrhi::ShaderType visibility,
        uint32_t registerSpace,
        const BindingSetDesc& bindingSetDesc,
        BindingLayoutHandle& bindingLayout,
        BindingSetHandle& bindingSet,
        bool registerSpaceIsDescriptorSet = false)
    {
        IBindingLayout* layout = bindingLayout.Get();
        IBindingSet* set = bindingSet.Get();
        const bool result = nvrhiUtilsCreateBindingSetAndLayout(device, visibility, registerSpace, &bindingSetDesc,
            &layout, &set, registerSpaceIsDescriptorSet);
        if (!bindingLayout)
            bindingLayout = TakeOver(layout);
        if (!bindingSet)
            bindingSet = TakeOver(set);
        return result;
    }

    inline void ClearColorAttachment(
        ICommandList* commandList,
        IFramebuffer* framebuffer,
        uint32_t attachmentIndex,
        Color color
    )
    {
        nvrhiUtilsClearColorAttachment(commandList, framebuffer, attachmentIndex, &color);
    }

    inline void ClearDepthStencilAttachment(
        ICommandList* commandList,
        IFramebuffer* framebuffer,
        float depth,
        uint32_t stencil
    )
    {
        nvrhiUtilsClearDepthStencilAttachment(commandList, framebuffer, depth, stencil);
    }

    inline void BuildBottomLevelAccelStruct(
        ICommandList* commandList,
        rt::IAccelStruct* as,
        const rt::AccelStructDesc& desc
    )
    {
        nvrhiUtilsBuildBottomLevelAccelStruct(commandList, as, &desc);
    }

    // Places a UAV barrier on the provided texture.
    // Useful when doing multiple consecutive dispatch calls with the same resources but different constants.
    // Ignored if there was a call to setEnableUavBarriersForTexrure(..., false) on this texture.
    inline void TextureUavBarrier(
        ICommandList* commandList,
        ITexture* texture)
    {
        nvrhiUtilsTextureUavBarrier(commandList, texture);
    }

    // Places a UAV barrier on the provided buffer.
    // Useful when doing multiple consecutive dispatch calls with the same resources but different constants.
    // Ignored if there was a call to setEnableUavBarriersForBuffer(..., false) on this buffer.
    inline void BufferUavBarrier(
        ICommandList* commandList,
        IBuffer* buffer)
    {
        nvrhiUtilsBufferUavBarrier(commandList, buffer);
    }

    // Selects a format from the supplied list that supports all the required features on the given device.
    // The formats are tested in the same order they're provided, and the first matching one is returned.
    // If no formats are matching, Format::UNKNOWN is returned.
    inline Format ChooseFormat(
        IDevice* device,
        nvrhi::FormatSupport requiredFeatures,
        const nvrhi::Format* requestedFormats,
        size_t requestedFormatCount)
    {
        return nvrhiUtilsChooseFormat(device, requiredFeatures, requestedFormats, requestedFormatCount);
    }

    inline const char* GraphicsAPIToString(GraphicsAPI api) { return nvrhiUtilsGraphicsAPIToString(api); }
    inline const char* TextureDimensionToString(TextureDimension dimension) { return nvrhiUtilsTextureDimensionToString(dimension); }
    inline const char* ShaderStageToString(ShaderType stage) { return nvrhiUtilsShaderStageToString(stage); }
    inline const char* ResourceTypeToString(ResourceType type) { return nvrhiUtilsResourceTypeToString(type); }
    inline const char* FormatToString(Format format) { return nvrhiUtilsFormatToString(format); }
    inline const char* CommandQueueToString(CommandQueue queue) { return nvrhiUtilsCommandQueueToString(queue); }

    // Automatic begin/end marker for command list
    class ScopedMarker
    {
    public:
        ICommandList* m_commandList;
        ScopedMarker(ICommandList* commandList, const char* markerName) : m_commandList(commandList)
        {
            m_commandList->beginMarker(markerName);
        }

        ScopedMarker(CommandListHandle* commandList, const char* markerName) :
            ScopedMarker(commandList->Get(), markerName)
        {}

        ~ScopedMarker()
        {
            m_commandList->endMarker();
        }
    };

}
