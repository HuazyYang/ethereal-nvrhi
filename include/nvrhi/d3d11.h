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

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>

namespace nvrhi::ObjectTypes
{
    constexpr ObjectType Nvrhi_D3D11_Device = 0x00010101;
};

namespace nvrhi::d3d11
{
    struct DeviceDesc
    {
        IMessageCallback* messageCallback = nullptr; // the device keeps a reference to it
        ID3D11DeviceContext* context = nullptr;
        bool aftermathEnabled = false;
    };

    // Creates a D3D11 device. Returns FS_OK and a new reference in *ppDevice, or an FE_* code and nullptr.
    NVRHI_C_API FRESULT nvrhiD3D11CreateDevice(const DeviceDesc* pDesc, nvrhi::IDevice** ppDevice) noexcept;

    NVRHI_C_API DXGI_FORMAT nvrhiD3D11ConvertFormat(nvrhi::Format format) noexcept;

    inline DeviceHandle createDevice(const DeviceDesc& desc)
    {
        nvrhi::IDevice* device = nullptr;
        nvrhiD3D11CreateDevice(&desc, &device);
        return TakeOver(device);
    }

    inline DXGI_FORMAT convertFormat(nvrhi::Format format) { return nvrhiD3D11ConvertFormat(format); }
}
