// The graphics factories of the backend tests: one per backend and process (ADR 0014).
#pragma once

#include <nvrhi/factory.h>

namespace nvrhi_test
{
    // The last factory that graphicsFactory created: every backend factory creates the validation layer.
    inline nvrhi::IGraphicsFactory*& anyGraphicsFactory()
    {
        static nvrhi::IGraphicsFactory* factory = nullptr;
        return factory;
    }

    // The factory of one backend, created on the first call with 'desc':
    //     nvrhi_test::graphicsFactory(nvrhi::d3d12::createGraphicsFactory)->createDevice(desc, &device);
    template <typename TFactory, typename TDesc>
    TFactory* graphicsFactory(nvrhi::AutoPtr<TFactory> (*create)(const TDesc&), const TDesc& desc = TDesc())
    {
        static nvrhi::AutoPtr<TFactory> factory = create(desc);
        if (factory)
            anyGraphicsFactory() = factory;
        return factory;
    }

    inline nvrhi::DeviceHandle createValidationLayer(nvrhi::IDevice* device)
    {
        nvrhi::DeviceHandle validation;
        if (anyGraphicsFactory())
            anyGraphicsFactory()->createValidationLayer(device, &validation);
        return validation;
    }
}
