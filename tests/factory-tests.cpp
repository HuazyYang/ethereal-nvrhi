// The graphics factories and swap chains (ADR 0014): per backend, the factory enumerates the adapters, creates a
// device with createDeviceOnAdapter (the native objects created inside), wraps it in the validation layer, and runs
// frames on a swap chain of a hidden window (CUDA: a windowless swap chain), with a resize in between.
//
// usage: nvrhi_factory_tests d3d11 | d3d12 | vulkan | cuda

#include <nvrhi/nvrhi.h>
#include <nvrhi/factory.h>
#include <nvrhi/swapchain.h>
#include <nvrhi/core/foundation.h>
#if TEST_D3D11
#include <nvrhi/d3d11.h>
#endif
#if TEST_D3D12
#include <nvrhi/d3d12.h>
#endif
#if TEST_VULKAN
#include <nvrhi/vulkan.h>
#endif
#if TEST_CUDA
#include <nvrhi/cuda.h>
#endif

#include <cstdio>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    void check(bool condition, const char* what)
    {
        if (!condition)
            throw std::runtime_error(std::string("check failed: ") + what);
        std::printf("ok: %s\n", what);
    }

    struct Messages : nvrhi::ObjectImpl<nvrhi::IMessageCallback>
    {
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IMessageCallback)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_END_INTERFACE_TABLE()

        unsigned errors = 0;
        void message(nvrhi::MessageSeverity severity, const char* text) noexcept override
        {
            if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal)
                ++errors;
            if (severity != nvrhi::MessageSeverity::Info)
                std::printf("NVRHI: %s\n", text);
        }
    };

#ifdef _WIN32
    // A hidden window of 'width' x 'height' client pixels.
    HWND createWindow(int width, int height)
    {
        WNDCLASSA windowClass = {};
        windowClass.lpfnWndProc = DefWindowProcA;
        windowClass.hInstance = GetModuleHandleA(nullptr);
        windowClass.lpszClassName = "nvrhi_factory_tests";
        RegisterClassA(&windowClass);
        RECT rect = { 0, 0, width, height };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        return CreateWindowA(windowClass.lpszClassName, "nvrhi_factory_tests", WS_OVERLAPPEDWINDOW, 0, 0,
            rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);
    }

    void resizeWindow(HWND window, int width, int height)
    {
        RECT rect = { 0, 0, width, height };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
    }
#endif

    void checkAdapters(nvrhi::IGraphicsFactory* factory, nvrhi::GraphicsAPI api)
    {
        check(factory->getGraphicsAPI() == api, "factory API");
        const uint32_t count = factory->getAdapterCount();
        check(count > 0, "at least one adapter");
        for (uint32_t index = 0; index < count; ++index)
        {
            nvrhi::AdapterInfo info;
            check(factory->getAdapterInfo(index, info) == nvrhi::FS_OK, "getAdapterInfo");
            std::printf("  adapter %u: %s (vendor 0x%04x, %llu MB%s%s)\n", index, info.name.c_str(), info.vendorID,
                (unsigned long long)(info.dedicatedVideoMemory >> 20), info.isDiscrete ? ", discrete" : "",
                info.isSoftware ? ", software" : "");
            check(info.nativeAdapter != nullptr || api == nvrhi::GraphicsAPI::CUDA, "native adapter");
        }
        nvrhi::AdapterInfo info;
        check(factory->getAdapterInfo(count, info) == nvrhi::FE_NOT_FOUND, "getAdapterInfo past the end");
    }

    // Clears the current back buffer (when the backend can) and presents, 'frames' times.
    void runFrames(nvrhi::IDevice* device, nvrhi::IGraphicsSwapChain* swapChain, int frames, bool clear)
    {
        nvrhi::CommandListHandle commandList;
        device->createCommandList(nvrhi::CommandListParameters(), &commandList);
        check(commandList != nullptr, "command list");

        for (int frame = 0; frame < frames; ++frame)
        {
            const nvrhi::FRESULT begin = swapChain->beginFrame();
            check(begin == nvrhi::FS_OK, "beginFrame");
            nvrhi::ITexture* backBuffer = swapChain->getBackBuffer(swapChain->getCurrentBackBufferIndex());
            check(backBuffer != nullptr, "current back buffer");
            check(backBuffer->getDesc().width == swapChain->getDesc().width, "back buffer width");

            commandList->open();
            if (clear)
                commandList->clearTextureFloat(backBuffer, nvrhi::AllSubresources, nvrhi::Color(0.1f * float(frame % 10), 0.2f, 0.3f, 1.f));
            commandList->close();
            device->executeCommandList(commandList);

            const nvrhi::FRESULT present = swapChain->present();
            check(present == nvrhi::FS_OK || present == nvrhi::FE_OUT_OF_DATE, "present");
            device->runGarbageCollection();
        }
        device->waitForIdle();
    }

    void runSwapChain(nvrhi::IDevice* device, nvrhi::IGraphicsSwapChain* swapChain, void* window)
    {
        check(swapChain != nullptr, "swap chain");
        check(swapChain->getBackBufferCount() > 0, "back buffers");
        std::printf("  swap chain %ux%u, %u buffers, format %d\n", swapChain->getDesc().width, swapChain->getDesc().height,
            swapChain->getBackBufferCount(), int(swapChain->getDesc().format));
        runFrames(device, swapChain, 10, true);

#ifdef _WIN32
        resizeWindow(static_cast<HWND>(window), 320, 200);
#else
        (void)window;
#endif
        check(swapChain->resize(0, 0) == nvrhi::FS_OK, "resize to the window");
        check(swapChain->getDesc().width == 320 && swapChain->getDesc().height == 200, "resized to 320x200");
        runFrames(device, swapChain, 10, true);
    }
}

int main(int argc, char** argv)
{
    try
    {
        check(argc == 2, "specify d3d11, d3d12, vulkan or cuda");
        const std::string backend = argv[1];
        nvrhi::AutoPtr<Messages> messages = MAKE_RC_OBJ_PTR(Messages);

        nvrhi::SwapChainDesc swapChainDesc;
        swapChainDesc.format = nvrhi::Format::SRGBA8_UNORM;
        swapChainDesc.bufferCount = 3;
#ifdef _WIN32
        HWND window = nullptr;
        if (backend != "cuda")
        {
            window = createWindow(640, 360);
            check(window != nullptr, "hidden window");
            swapChainDesc.window = window;
        }
#else
        void* window = nullptr;
#endif

#if TEST_D3D11
        if (backend == "d3d11")
        {
            nvrhi::d3d11::GraphicsFactoryDesc factoryDesc;
            factoryDesc.messageCallback = messages;
            nvrhi::d3d11::GraphicsFactoryHandle factory = nvrhi::d3d11::createGraphicsFactory(factoryDesc);
            check(factory != nullptr, "d3d11 graphics factory");
            checkAdapters(factory, nvrhi::GraphicsAPI::D3D11);
            check(factory->getNativeObject(nvrhi::ObjectTypes::DXGI_Factory) != nullptr, "DXGI factory");

            nvrhi::d3d11::DeviceCreationDesc deviceDesc;
            deviceDesc.messageCallback = messages;
            nvrhi::DeviceHandle device;
            check(factory->createDeviceOnAdapter(deviceDesc, &device) == nvrhi::FS_OK && device, "createDeviceOnAdapter");
            nvrhi::DeviceHandle validation;
            check(factory->createValidationLayer(device, &validation) == nvrhi::FS_OK && validation, "validation layer");

            nvrhi::d3d11::GraphicsSwapChainHandle swapChain;
            check(factory->createSwapChain(swapChainDesc, validation, &swapChain) == nvrhi::FS_OK, "createSwapChain");
            check(swapChain->getDXGISwapChain() != nullptr, "IDXGISwapChain");
            runSwapChain(validation, swapChain, window);
        }
        else
#endif
#if TEST_D3D12
        if (backend == "d3d12")
        {
            nvrhi::d3d12::GraphicsFactoryDesc factoryDesc;
            factoryDesc.messageCallback = messages;
            factoryDesc.enableDebugRuntime = true;
#ifdef NVRHI_TEST_D3D12_SDK_VERSION
            factoryDesc.sdkVersion = NVRHI_TEST_D3D12_SDK_VERSION;
            factoryDesc.sdkPath = ".\\D3D12\\";
#endif
            nvrhi::d3d12::GraphicsFactoryHandle factory = nvrhi::d3d12::createGraphicsFactory(factoryDesc);
            check(factory != nullptr, "d3d12 graphics factory");
            checkAdapters(factory, nvrhi::GraphicsAPI::D3D12);
            check((factory->getNativeObject(nvrhi::ObjectTypes::D3D12_DeviceFactory) != nullptr) == (factoryDesc.sdkVersion != 0),
                "Agility SDK device factory");

            nvrhi::d3d12::DeviceCreationDesc deviceDesc;
            deviceDesc.errorCB = messages;
            deviceDesc.enableComputeQueue = true;
            deviceDesc.enableCopyQueue = true;
            nvrhi::d3d12::DeviceHandle device;
            check(factory->createDeviceOnAdapter(deviceDesc, &device) == nvrhi::FS_OK && device, "createDeviceOnAdapter");
            check(device->getNativeQueue(nvrhi::ObjectTypes::D3D12_CommandQueue, nvrhi::CommandQueue::Compute) != nullptr, "compute queue");
            check(device->getNativeQueue(nvrhi::ObjectTypes::D3D12_CommandQueue, nvrhi::CommandQueue::Copy) != nullptr, "copy queue");
#ifdef NVRHI_TEST_D3D12_SDK_VERSION
            check(GetModuleHandleA("D3D12Core.dll") != nullptr, "D3D12Core.dll of the Agility SDK is loaded");
#endif
            nvrhi::DeviceHandle validation;
            check(factory->createValidationLayer(device, &validation) == nvrhi::FS_OK && validation, "validation layer");

            nvrhi::d3d12::GraphicsSwapChainHandle swapChain;
            check(factory->createSwapChain(swapChainDesc, validation, &swapChain) == nvrhi::FS_OK, "createSwapChain");
            check(swapChain->getDXGISwapChain() != nullptr, "IDXGISwapChain3");
            check(swapChain->getNativeObject(nvrhi::ObjectTypes::D3D12_Fence) != nullptr, "frame fence");
            runSwapChain(validation, swapChain, window);
        }
        else
#endif
#if TEST_VULKAN
        if (backend == "vulkan")
        {
            nvrhi::vulkan::GraphicsFactoryDesc factoryDesc;
            factoryDesc.messageCallback = messages;
            nvrhi::vulkan::GraphicsFactoryHandle factory = nvrhi::vulkan::createGraphicsFactory(factoryDesc);
            check(factory != nullptr, "vulkan graphics factory");
            checkAdapters(factory, nvrhi::GraphicsAPI::VULKAN);
            check(factory->getNativeObject(nvrhi::ObjectTypes::VK_Instance) != nullptr, "VkInstance");
            check(factory->getEnabledInstanceExtensions(nullptr, 0) > 0, "enabled instance extensions");

            nvrhi::vulkan::DeviceCreationDesc deviceDesc;
            deviceDesc.errorCB = messages;
            deviceDesc.surface.window = swapChainDesc.window;
            deviceDesc.surface.format = swapChainDesc.format;
            deviceDesc.surface.bufferCount = swapChainDesc.bufferCount;
            nvrhi::vulkan::DeviceHandle device;
            check(factory->createDeviceOnAdapter(deviceDesc, &device) == nvrhi::FS_OK && device, "createDeviceOnAdapter");
            check(device->getEnabledExtensions(nullptr, 0) > 0, "enabled device extensions");
            nvrhi::DeviceHandle validation;
            check(factory->createValidationLayer(device, &validation) == nvrhi::FS_OK && validation, "validation layer");

            {
                nvrhi::vulkan::GraphicsSwapChainHandle swapChain;
                check(factory->createSwapChain(swapChainDesc, validation, &swapChain) == nvrhi::FS_OK, "createSwapChain");
                check(swapChain->getVkSwapchain() != VK_NULL_HANDLE, "VkSwapchainKHR");
                check(swapChain->getDesc().format == nvrhi::Format::SBGRA8_UNORM, "RGBA8 swapped for BGRA8");
                runSwapChain(validation, swapChain, window);
            }
            // The device owns its VkDevice and keeps the factory (and the VkInstance) alive.
            factory = nullptr;
            validation = nullptr;
            device->waitForIdle();
        }
        else
#endif
#if TEST_CUDA
        if (backend == "cuda")
        {
            nvrhi::cuda::GraphicsFactoryDesc factoryDesc;
            factoryDesc.messageCallback = messages;
            nvrhi::cuda::GraphicsFactoryHandle factory = nvrhi::cuda::createGraphicsFactory(factoryDesc);
            if (!factory)
            {
                std::printf("SKIP: no CUDA driver\n");
                return 0;
            }
            checkAdapters(factory, nvrhi::GraphicsAPI::CUDA);

            nvrhi::cuda::DeviceDesc deviceDesc;
            deviceDesc.messageCallback = messages;
            nvrhi::cuda::DeviceHandle device;
            check(factory->createDevice(deviceDesc, &device) == nvrhi::FS_OK && device, "createDevice");

            nvrhi::SwapChainDesc cudaDesc;
            cudaDesc.width = 256;
            cudaDesc.height = 128;
            cudaDesc.format = nvrhi::Format::RGBA8_UNORM;
            cudaDesc.bufferCount = 2;
            nvrhi::cuda::GraphicsSwapChainHandle swapChain;
            check(factory->createSwapChain(cudaDesc, device, &swapChain) == nvrhi::FS_OK, "createSwapChain (windowless)");
            check(swapChain->getFrameQuery(0) == nullptr, "no frame presented yet");
            runFrames(device, swapChain, 6, false);
            check(swapChain->getFrameQuery(0) != nullptr && device->pollEventQuery(swapChain->getFrameQuery(0)), "frame query done");
            check(swapChain->resize(64, 32) == nvrhi::FS_OK && swapChain->getBackBuffer(0)->getDesc().width == 64, "resize");
            runFrames(device, swapChain, 3, false);
        }
        else
#endif
        {
            check(false, "unknown or disabled backend");
        }

        check(messages->errors == 0, "no NVRHI errors");
#ifdef _WIN32
        if (window)
            DestroyWindow(window);
#endif
        std::printf("PASS\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
