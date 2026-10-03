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

#include <nvrhi/core/types.h>

#include <cstdint>
#include <type_traits>

namespace nvrhi
{
    template <typename T> class AutoPtr; // <nvrhi/core/autoptr.h>

    typedef uint32_t ObjectType;

    // ObjectTypes namespace contains identifiers for various object types. 
    // All constants have to be distinct. Implementations of NVRHI may extend the list.
    //
    // The encoding is chosen to minimize potential conflicts between implementations.
    // 0x00aabbcc, where:
    //   aa is GAPI, 1 for D3D11, 2 for D3D12, 3 for VK
    //   bb is layer, 0 for native GAPI objects, 1 for reference NVRHI backend, 2 for user-defined backends
    //   cc is a sequential number

    namespace ObjectTypes
    {
        constexpr ObjectType SharedHandle                           = 0x00000001;

        constexpr ObjectType D3D11_Device                           = 0x00010001;
        constexpr ObjectType D3D11_DeviceContext                    = 0x00010002;
        constexpr ObjectType D3D11_Resource                         = 0x00010003;
        constexpr ObjectType D3D11_Buffer                           = 0x00010004;
        constexpr ObjectType D3D11_RenderTargetView                 = 0x00010005;
        constexpr ObjectType D3D11_DepthStencilView                 = 0x00010006;
        constexpr ObjectType D3D11_ShaderResourceView               = 0x00010007;
        constexpr ObjectType D3D11_UnorderedAccessView              = 0x00010008;

        constexpr ObjectType D3D12_Device                           = 0x00020001;
        constexpr ObjectType D3D12_CommandQueue                     = 0x00020002;
        constexpr ObjectType D3D12_GraphicsCommandList              = 0x00020003;
        constexpr ObjectType D3D12_Resource                         = 0x00020004;
        constexpr ObjectType D3D12_RenderTargetViewDescriptor       = 0x00020005;
        constexpr ObjectType D3D12_DepthStencilViewDescriptor       = 0x00020006;
        constexpr ObjectType D3D12_ShaderResourceViewGpuDescriptor  = 0x00020007;
        constexpr ObjectType D3D12_UnorderedAccessViewGpuDescriptor = 0x00020008;
        constexpr ObjectType D3D12_RootSignature                    = 0x00020009;
        constexpr ObjectType D3D12_PipelineState                    = 0x0002000a;
        constexpr ObjectType D3D12_CommandAllocator                 = 0x0002000b;

        constexpr ObjectType VK_Device                              = 0x00030001;
        constexpr ObjectType VK_PhysicalDevice                      = 0x00030002;
        constexpr ObjectType VK_Instance                            = 0x00030003;
        constexpr ObjectType VK_Queue                               = 0x00030004;
        constexpr ObjectType VK_CommandBuffer                       = 0x00030005;
        constexpr ObjectType VK_DeviceMemory                        = 0x00030006;
        constexpr ObjectType VK_Buffer                              = 0x00030007;
        constexpr ObjectType VK_Image                               = 0x00030008;
        constexpr ObjectType VK_ImageView                           = 0x00030009;
        constexpr ObjectType VK_AccelerationStructureKHR            = 0x0003000a;
        constexpr ObjectType VK_Sampler                             = 0x0003000b;
        constexpr ObjectType VK_ShaderModule                        = 0x0003000c;
        [[deprecated]]
        constexpr ObjectType VK_RenderPass                          = 0x0003000d;
        [[deprecated]]
        constexpr ObjectType VK_Framebuffer                         = 0x0003000e;
        constexpr ObjectType VK_DescriptorPool                      = 0x0003000f;
        constexpr ObjectType VK_DescriptorSetLayout                 = 0x00030010;
        constexpr ObjectType VK_DescriptorSet                       = 0x00030011;
        constexpr ObjectType VK_PipelineLayout                      = 0x00030012;
        constexpr ObjectType VK_Pipeline                            = 0x00030013;
        constexpr ObjectType VK_Micromap                            = 0x00030014;
        constexpr ObjectType VK_ImageCreateInfo                     = 0x00030015;
    };

    // A native API object or interface (ID3D12Device*, VkImage, ...), returned by getNativeObject and friends and
    // passed to the createHandleForNative* methods. A plain pointer, so it crosses the ABI as a scalar: convert it
    // with static_cast<ID3D12Resource*>(object) or static_cast<VkImage>(object). Vulkan non-dispatchable handles
    // are 64-bit values, which fit only into a 64-bit pointer.
    using NativeObject = void*;

    static_assert(sizeof(void*) == 8, "NVRHI requires a 64-bit target (NativeObject holds 64-bit Vulkan handles)");

    struct MemoryRequirements;

    //////////////////////////////////////////////////////////////////////////
    // IRHIObject
    // The base interface of every reference-counted NVRHI object. Objects are
    // nvrhi::IObject implementations (see <nvrhi/core/foundation.h>), created
    // with MAKE_RC_OBJ and held by nvrhi::AutoPtr<T> (the *Handle typedefs).
    // Every public interface answers QueryInterface for its own IID and for
    // the IIDs of the interfaces it derives from (IRHIObject, IObject, ...).
    //////////////////////////////////////////////////////////////////////////

    NVRHI_IID(IRHIObject, "3c7ad626-034c-4f05-83fb-e7e3da19f1a0")
    struct IRHIObject : IObject
    {
        NVRHI_DECLARE_UUID_TRAITS(IRHIObject)

        // Returns a native object or interface, for example ID3D11Device*, or nullptr if the requested interface is unavailable.
        // Does *not* AddRef the returned interface.
        virtual NativeObject getNativeObject(ObjectType objectType) noexcept { (void)objectType; return nullptr; }

        // Optional backing-memory query. Returns false without changing output when unavailable.
        // See doc/memory-queries.md for supported resources and accounting limitations.
        virtual bool queryMemoryRequirements(MemoryRequirements& outRequirements) noexcept { (void)outRequirements; return false; }
    };

    typedef AutoPtr<IRHIObject> RHIObjectHandle;

} // namespace nvrhi