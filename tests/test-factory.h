// The graphics factory of the backend tests: one per process (ADR 0010).
#pragma once

#include <nvrhi/factory.h>

namespace nvrhi_test
{
    inline nvrhi::IGraphicsFactory* graphicsFactory()
    {
        static nvrhi::GraphicsFactoryHandle factory = nvrhi::createGraphicsFactory(nvrhi::GraphicsFactoryDesc());
        return factory;
    }

    inline nvrhi::DeviceHandle createValidationLayer(nvrhi::IDevice* device)
    {
        nvrhi::DeviceHandle validation;
        graphicsFactory()->createValidationLayer(device, &validation);
        return validation;
    }
}
