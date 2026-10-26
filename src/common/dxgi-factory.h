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

// The DXGI part of the D3D11 and D3D12 graphics factories (ADR 0014): dxgi.dll loaded at run time, the DXGI factory,
// the adapters, and helpers for the DXGI swap chains. Module-private; compiled into both D3D backends, as
// dxgi-format.cpp is.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <nvrhi/factory.h>
#include <nvrhi/swapchain.h>
#include <nvrhi/core/autoptr.h>

#include <Windows.h>
#include <dxgi1_6.h>

#include <string>
#include <vector>

namespace nvrhi::dxgi
{
    // Loads a library for a factory. The libraries stay loaded until the process ends: devices and swap chains that
    // the application keeps after releasing the factory still run code from them. Returns nullptr on failure.
    HMODULE LoadRuntimeLibrary(const string& overrideName, const char* defaultName) noexcept;

    std::string ToUtf8(const wchar_t* text);

    class Factory
    {
    public:
        // Loads dxgi.dll (or libraryName), creates the DXGI factory (a debug one if requested) and enumerates the
        // adapters. Returns false and an error message on failure.
        bool initialize(const string& libraryName, bool debug, std::string& outError);

        [[nodiscard]] IDXGIFactory2* get() const { return m_Factory; }
        [[nodiscard]] const std::vector<AdapterInfo>& getAdapterInfos() const { return m_AdapterInfos; }
        [[nodiscard]] bool isTearingSupported() const { return m_TearingSupported; }

        // DeviceCreationDesc::adapterIndex: -1 selects adapter 0. Returns nullptr and an error message if the
        // adapter does not exist.
        IDXGIAdapter1* selectAdapter(int32_t adapterIndex, std::string& outError) const;

        // IDXGIDebug::ReportLiveObjects, if the DXGI debug layer is installed.
        void reportLiveObjects() const;

    private:
        HMODULE m_Module = nullptr;
        AutoPtr<IDXGIFactory2> m_Factory;
        std::vector<AutoPtr<IDXGIAdapter1>> m_Adapters;
        std::vector<AdapterInfo> m_AdapterInfos;
        bool m_TearingSupported = false;
    };

    // The DXGI format of a swap chain for an NVRHI format: DXGI does not create sRGB swap chains, but the back
    // buffers are interpreted as sRGB when the textures use the sRGB format.
    DXGI_FORMAT GetSwapChainFormat(Format format, DXGI_FORMAT (*convertFormat)(Format));

    // The size of the window's client area.
    void GetClientSize(HWND window, uint32_t& outWidth, uint32_t& outHeight);
}
