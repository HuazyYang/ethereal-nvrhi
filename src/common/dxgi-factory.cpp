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

#include "dxgi-factory.h"

#include <dxgidebug.h>

#include <cstring>

namespace nvrhi::dxgi
{
    HMODULE LoadRuntimeLibrary(const string& overrideName, const char* defaultName) noexcept
    {
        const char* name = overrideName.empty() ? defaultName : overrideName.c_str();
        return LoadLibraryA(name);
    }

    std::string ToUtf8(const wchar_t* text)
    {
        const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (length <= 1)
            return std::string();
        std::string result(size_t(length - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
        return result;
    }

    bool Factory::initialize(const string& libraryName, bool debug, std::string& outError)
    {
        m_Module = LoadRuntimeLibrary(libraryName, "dxgi.dll");
        if (!m_Module)
        {
            outError = std::string("cannot load ") + (libraryName.empty() ? "dxgi.dll" : libraryName.c_str());
            return false;
        }

        typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
        const auto createFactory = reinterpret_cast<PFN_CreateDXGIFactory2>(
            reinterpret_cast<void*>(GetProcAddress(m_Module, "CreateDXGIFactory2")));
        if (!createFactory)
        {
            outError = "CreateDXGIFactory2 is not available";
            return false;
        }

        const HRESULT hr = createFactory(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&m_Factory));
        if (FAILED(hr))
        {
            char text[160];
            snprintf(text, sizeof(text), "CreateDXGIFactory2 failed, HRESULT = 0x%08lx%s", hr,
                debug ? " (is the DXGI debug layer installed? Graphics Tools optional feature)" : "");
            outError = text;
            return false;
        }

        AutoPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(m_Factory->QueryInterface(IID_PPV_ARGS(&factory5))))
        {
            BOOL supported = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported))))
                m_TearingSupported = supported != FALSE;
        }

        AutoPtr<IDXGIFactory1> factory1;
        m_Factory->QueryInterface(IID_PPV_ARGS(&factory1));
        for (UINT index = 0; factory1; ++index)
        {
            AutoPtr<IDXGIAdapter1> adapter;
            if (FAILED(factory1->EnumAdapters1(index, &adapter)))
                break;

            DXGI_ADAPTER_DESC1 desc = {};
            if (FAILED(adapter->GetDesc1(&desc)))
                continue;

            AdapterInfo info;
            info.name = string(ToUtf8(desc.Description).c_str());
            info.vendorID = desc.VendorId;
            info.deviceID = desc.DeviceId;
            info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
            static_assert(sizeof(info.luid) == sizeof(desc.AdapterLuid));
            memcpy(info.luid, &desc.AdapterLuid, sizeof(info.luid));
            info.luidValid = true;
            info.isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            info.nativeAdapter = adapter.Get();

            m_Adapters.push_back(std::move(adapter));
            m_AdapterInfos.push_back(std::move(info));
        }

        return true;
    }

    IDXGIAdapter1* Factory::selectAdapter(int32_t adapterIndex, std::string& outError) const
    {
        const uint32_t index = adapterIndex < 0 ? 0u : uint32_t(adapterIndex);
        if (index < m_Adapters.size())
            return m_Adapters[index];

        if (adapterIndex < 0)
            outError = "Cannot find any DXGI adapters in the system.";
        else
            outError = "The specified DXGI adapter " + std::to_string(adapterIndex) + " does not exist.";
        return nullptr;
    }

    void Factory::reportLiveObjects() const
    {
        typedef HRESULT(WINAPI* PFN_DXGIGetDebugInterface1)(UINT, REFIID, void**);
        const auto getDebugInterface = m_Module ? reinterpret_cast<PFN_DXGIGetDebugInterface1>(
            reinterpret_cast<void*>(GetProcAddress(m_Module, "DXGIGetDebugInterface1"))) : nullptr;
        if (!getDebugInterface)
            return;

        AutoPtr<IDXGIDebug> debug;
        if (SUCCEEDED(getDebugInterface(0, IID_PPV_ARGS(&debug))))
        {
            debug->ReportLiveObjects(DXGI_DEBUG_ALL,
                DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_IGNORE_INTERNAL | DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_DETAIL));
        }
    }

    DXGI_FORMAT GetSwapChainFormat(Format format, DXGI_FORMAT (*convertFormat)(Format))
    {
        switch (format)  // NOLINT(clang-diagnostic-switch-enum)
        {
        case Format::SRGBA8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::SBGRA8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return convertFormat(format);
        }
    }

    void GetClientSize(HWND window, uint32_t& outWidth, uint32_t& outHeight)
    {
        RECT rect = {};
        GetClientRect(window, &rect);
        outWidth = uint32_t(rect.right - rect.left);
        outHeight = uint32_t(rect.bottom - rect.top);
    }
}
