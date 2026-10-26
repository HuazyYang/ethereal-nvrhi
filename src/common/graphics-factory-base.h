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

// The part of the graphics factories (ADR 0014) that every backend shares: the adapter list, the factory's message
// callback and the validation layer. Module-private.

#include <nvrhi/factory.h>
#include <nvrhi/core/foundation.h>

#include "backend-factories.h"

#include <string>
#include <vector>

#ifndef NVRHI_FACTORY_WITH_VALIDATION
#define NVRHI_FACTORY_WITH_VALIDATION 0
#endif

namespace nvrhi
{
    // TInterface is the backend factory interface (d3d12::IGraphicsFactory, ...). The derived class lists the
    // interfaces in its QueryInterface table.
    template <typename TInterface>
    class GraphicsFactoryBase : public ObjectImpl<TInterface>
    {
    public:
        explicit GraphicsFactoryBase(IMessageCallback* messageCallback)
            : m_MessageCallback(messageCallback)
        { }

        uint32_t getAdapterCount() noexcept override
        {
            return uint32_t(m_Adapters.size());
        }

        FRESULT getAdapterInfo(uint32_t index, AdapterInfo& outInfo) noexcept override
        {
            if (index >= m_Adapters.size())
                return FE_NOT_FOUND;
            outInfo = m_Adapters[index];
            return FS_OK;
        }

        FRESULT createValidationLayer(IDevice* underlyingDevice, IDevice** ppDevice) noexcept override
        {
            if (!ppDevice)
                return FE_INVALID_ARGS;
            *ppDevice = nullptr;
            if (!underlyingDevice)
                return FE_INVALID_ARGS;
#if NVRHI_FACTORY_WITH_VALIDATION
            return validation::CreateValidationLayer(underlyingDevice, ppDevice);
#else
            warning("NVRHI was built without the validation layer");
            return FE_UNSUPPORTED;
#endif
        }

    protected:
        AutoPtr<IMessageCallback> m_MessageCallback;
        std::vector<AdapterInfo> m_Adapters;

        void message(MessageSeverity severity, const std::string& text) const noexcept
        {
            if (m_MessageCallback)
                m_MessageCallback->message(severity, text.c_str());
        }
        void info(const std::string& text) const noexcept { message(MessageSeverity::Info, text); }
        void warning(const std::string& text) const noexcept { message(MessageSeverity::Warning, text); }
        void error(const std::string& text) const noexcept { message(MessageSeverity::Error, text); }
    };

    // Clears the output and checks the arguments that every creation method takes.
    template <typename TInterface>
    bool BeginCreate(TInterface** ppObject) noexcept
    {
        if (!ppObject)
            return false;
        *ppObject = nullptr;
        return true;
    }
}
