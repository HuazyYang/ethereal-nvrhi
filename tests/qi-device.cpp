// QueryInterface on real backend objects, without windows: every object answers IObject, IRHIObject and
// each interface of its chain with the same object, refuses unrelated IIDs, and QueryInterface keeps the
// reference count balanced. Also through the validation layer.
//
// checked_cast without RTTI (ADR 0006): checked_cast on real objects, and a small submitted workload that
// runs the backends' own checked_casts (QueryInterface checks in Debug builds) and, through the validation
// layer, the wrappers' QueryInterface type tests (CommandListWrapper in executeCommandLists).
#include <nvrhi/nvrhi.h>
#include <nvrhi/validation.h>
#include <nvrhi/common/misc.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#if TEST_D3D12
#include <nvrhi/d3d12.h>
#endif
#if TEST_D3D11
#include <nvrhi/d3d11.h>
#endif
#if TEST_D3D11 || TEST_D3D12
#include <wrl/client.h>
#endif
#if TEST_VULKAN
#define VK_NO_PROTOTYPES
#include <nvrhi/vulkan.h>
#if !TEST_SHARED
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

static void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct Messages : nvrhi::IMessageCallback
{
    unsigned errors = 0;
    void message(nvrhi::MessageSeverity severity, const char* text) override
    {
        if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal) ++errors;
        std::printf("NVRHI: %s\n", text);
    }
};

static nvrhi::FLONG refCount(nvrhi::IObject* object)
{
    object->AddRef();
    return object->Release();
}

// Queries riid, checks the result against `expected` (null: must be refused) and releases it.
static void expectQI(nvrhi::IObject* object, nvrhi::FREFIID riid, const void* expected, const std::string& what)
{
    void* pv = reinterpret_cast<void*>(1);
    const nvrhi::FRESULT hr = object->QueryInterface(riid, &pv);
    if (expected)
    {
        check(hr == nvrhi::FS_OK, (what + ": QueryInterface failed").c_str());
        check(pv == expected, (what + ": QueryInterface returned another pointer").c_str());
        static_cast<nvrhi::IObject*>(pv)->Release();
    }
    else
    {
        check(hr == nvrhi::FE_NOINTERFACE, (what + ": unrelated IID not refused").c_str());
        check(pv == nullptr, (what + ": refused query left a pointer").c_str());
    }
}

// The IIDs every object answers (IObject, IRHIObject) plus the given chain, with an identity check and
// a balanced reference count.
struct Expect
{
    nvrhi::FIID iid;
    const void* pointer;  // null: must be refused
    const char* name;
};

static void checkObject(nvrhi::IRHIObject* object, const char* what, std::initializer_list<Expect> expects)
{
    const nvrhi::FLONG before = refCount(object);
    const std::string prefix = std::string(what) + " ";
    expectQI(object, nvrhi::IID_IObject, static_cast<nvrhi::IObject*>(object), prefix + "IObject");
    expectQI(object, nvrhi::IID_IRHIObject, object, prefix + "IRHIObject");
    for (const Expect& e : expects)
        expectQI(object, e.iid, e.pointer, prefix + e.name);
    // Identity is the same from every interface the object answers.
    for (const Expect& e : expects)
    {
        if (!e.pointer) continue;
        void* id = nullptr;
        check(static_cast<nvrhi::IObject*>(const_cast<void*>(e.pointer))->QueryInterface(nvrhi::IID_IObject, &id) == nvrhi::FS_OK,
            (prefix + e.name + ": identity query").c_str());
        check(id == static_cast<nvrhi::IObject*>(object), (prefix + e.name + ": identity differs").c_str());
        static_cast<nvrhi::IObject*>(id)->Release();
    }
    check(refCount(object) == before, (prefix + "reference count not balanced").c_str());
    // AutoPtr::As goes through the same path.
    {
        nvrhi::AutoPtr<nvrhi::IRHIObject> base;
        check(nvrhi::AutoPtr<nvrhi::IRHIObject>(object).As(&base) == nvrhi::FS_OK && base == object,
            (prefix + "AutoPtr::As").c_str());
    }
    check(refCount(object) == before, (prefix + "reference count not balanced after AutoPtr::As").c_str());
    std::printf("%s: QueryInterface OK (%d expectations, refcount %d)\n", what, int(expects.size()) + 2, int(before));
}

static void runCasts(nvrhi::IDevice* device, const std::string& p);

static void runObjects(nvrhi::IDevice* device, const void* backendDevice, const nvrhi::FIID* backendDeviceIID,
    const nvrhi::FIID* backendCommandListIID, const char* path)
{
    std::printf("Device path: %s\n", path);
    const std::string p = std::string(path) + " ";

    // Device: nvrhi::IDevice, and the backend's IDevice when the object implements it.
    {
        std::vector<Expect> e{
            {nvrhi::IID_IDevice, device, "IDevice"},
            {nvrhi::IID_ITexture, nullptr, "ITexture (unrelated)"},
            {nvrhi::IID_ICommandList, nullptr, "ICommandList (unrelated)"},
        };
        if (backendDeviceIID)
            e.push_back({*backendDeviceIID, backendDevice, backendDevice ? "backend IDevice" : "backend IDevice (not implemented)"});
        const nvrhi::FLONG before = refCount(device);
        for (const Expect& x : e)
            expectQI(device, x.iid, x.pointer, p + "device " + x.name);
        expectQI(device, nvrhi::IID_IObject, static_cast<nvrhi::IObject*>(device), p + "device IObject");
        expectQI(device, nvrhi::IID_IRHIObject, static_cast<nvrhi::IRHIObject*>(device), p + "device IRHIObject");
        check(refCount(device) == before, (p + "device reference count not balanced").c_str());
        if (backendDevice)
        {
            void* id = nullptr;
            check(static_cast<nvrhi::IObject*>(const_cast<void*>(backendDevice))->QueryInterface(nvrhi::IID_IObject, &id) == nvrhi::FS_OK
                && id == static_cast<nvrhi::IObject*>(device), (p + "backend device identity").c_str());
            static_cast<nvrhi::IObject*>(id)->Release();
        }
        std::printf("%sdevice: QueryInterface OK (refcount %d)\n", p.c_str(), int(before));
    }

    {
        auto texture = device->createTexture(nvrhi::TextureDesc().setWidth(16).setHeight(16)
            .setFormat(nvrhi::Format::RGBA8_UNORM).setDebugName("qi texture"));
        check(texture != nullptr, "create texture");
        nvrhi::ITexture* t = texture;
        checkObject(t, (p + "texture").c_str(), {
            {nvrhi::IID_ITexture, t, "ITexture"},
            {nvrhi::IID_IBuffer, nullptr, "IBuffer (unrelated)"},
            {nvrhi::IID_IDevice, nullptr, "IDevice (unrelated)"}});
    }

    {
        auto buffer = device->createBuffer(nvrhi::BufferDesc().setByteSize(256).setDebugName("qi buffer"));
        check(buffer != nullptr, "create buffer");
        nvrhi::IBuffer* b = buffer;
        checkObject(b, (p + "buffer").c_str(), {
            {nvrhi::IID_IBuffer, b, "IBuffer"},
            {nvrhi::IID_ITexture, nullptr, "ITexture (unrelated)"}});
    }

    {
        auto commandList = device->createCommandList();
        check(commandList != nullptr, "create command list");
        nvrhi::ICommandList* c = commandList;
        const void* backendCommandList = nullptr;
        if (backendCommandListIID)
        {
            // The backend interface is the same object; find its pointer through the query itself and
            // check it upcasts back to the nvrhi interface.
            void* pv = nullptr;
            check(c->QueryInterface(*backendCommandListIID, &pv) == nvrhi::FS_OK, (p + "backend ICommandList").c_str());
            backendCommandList = pv;
            static_cast<nvrhi::IObject*>(pv)->Release();
#if TEST_D3D12
            if (backendCommandListIID == &nvrhi::d3d12::IID_ICommandList)
                check(static_cast<nvrhi::ICommandList*>(static_cast<nvrhi::d3d12::ICommandList*>(pv)) == c,
                    (p + "d3d12::ICommandList upcasts to the same nvrhi::ICommandList").c_str());
#endif
        }
        if (backendCommandList)
            checkObject(c, (p + "command list").c_str(), {
                {nvrhi::IID_ICommandList, c, "ICommandList"},
                {*backendCommandListIID, backendCommandList, "backend ICommandList"},
                {nvrhi::IID_IDevice, nullptr, "IDevice (unrelated)"}});
        else
            checkObject(c, (p + "command list").c_str(), {
                {nvrhi::IID_ICommandList, c, "ICommandList"},
                {nvrhi::IID_IDevice, nullptr, "IDevice (unrelated)"}});
    }

    // Descriptor table: IDescriptorTable : IBindingSet, where the backend supports bindless layouts.
    if (device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11)
    {
        nvrhi::BindlessLayoutDesc bindless;
        bindless.visibility = nvrhi::ShaderType::All;
        bindless.maxCapacity = 16;
        bindless.layoutType = nvrhi::BindlessLayoutDesc::LayoutType::Immutable;
        bindless.addRegisterSpace(nvrhi::BindingLayoutItem::Texture_SRV(1));
        auto layout = device->createBindlessLayout(bindless);
        if (layout)
        {
            auto table = device->createDescriptorTable(layout);
            check(table != nullptr, "create descriptor table");
            nvrhi::IDescriptorTable* d = table;
            checkObject(d, (p + "descriptor table").c_str(), {
                {nvrhi::IID_IDescriptorTable, d, "IDescriptorTable"},
                {nvrhi::IID_IBindingSet, static_cast<nvrhi::IBindingSet*>(d), "IBindingSet"},
                {nvrhi::IID_IBindingLayout, nullptr, "IBindingLayout (unrelated)"}});
            checkObject(layout, (p + "bindless layout").c_str(), {
                {nvrhi::IID_IBindingLayout, static_cast<nvrhi::IBindingLayout*>(layout), "IBindingLayout"},
                {nvrhi::IID_IBindingSet, nullptr, "IBindingSet (unrelated)"}});
        }
        else
        {
            std::printf("%sbindless layout: unsupported, skipped\n", p.c_str());
        }
    }

    runCasts(device, p);
}

static void runCasts(nvrhi::IDevice* device, const std::string& p)
{
    auto texture = device->createTexture(nvrhi::TextureDesc().setWidth(16).setHeight(16)
        .setFormat(nvrhi::Format::RGBA8_UNORM).setIsRenderTarget(true)
        .setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true)
        .setDebugName("cast texture"));
    check(texture != nullptr, "create cast texture");
    auto buffer = device->createBuffer(nvrhi::BufferDesc().setByteSize(256).setIsConstantBuffer(true)
        .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true)
        .setDebugName("cast buffer"));
    check(buffer != nullptr, "create cast buffer");

    // checked_cast between public interfaces of real objects. In Debug builds it asks QueryInterface.
    {
        nvrhi::IRHIObject* object = texture;
        const nvrhi::FLONG before = refCount(object);
        check(nvrhi::checked_cast<nvrhi::ITexture*>(object) == texture.Get(), (p + "checked_cast to ITexture").c_str());
        check(nvrhi::details::QICastMatches(object, texture.Get()), (p + "texture answers ITexture").c_str());
        check(!nvrhi::details::QICastMatches(object, static_cast<nvrhi::IBuffer*>(object)),
            (p + "texture does not answer IBuffer").c_str());
        nvrhi::IRHIObject* bufferObject = buffer;
        check(nvrhi::checked_cast<nvrhi::IBuffer*>(bufferObject) == buffer.Get(), (p + "checked_cast to IBuffer").c_str());
        check(refCount(object) == before, (p + "checked_cast reference count not balanced").c_str());
    }

    // Binding set: the validation layer checked_casts BindingSetItem::resourceHandle to ITexture / IBuffer,
    // the backend casts the interfaces to its classes.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    auto layout = device->createBindingLayout(layoutDesc);
    check(layout != nullptr, "create cast binding layout");
    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture));
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, buffer));
    auto set = device->createBindingSet(setDesc, layout);
    check(set != nullptr, "create cast binding set");

    // Recorded and executed work: the backend casts the command list, the texture and the buffer; the
    // validation layer finds its CommandListWrapper through QueryInterface.
    auto commandList = device->createCommandList();
    check(commandList != nullptr, "create cast command list");
    uint8_t data[256] = {};
    commandList->open();
    commandList->writeBuffer(buffer, data, sizeof(data));
    commandList->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0.f));
    commandList->close();
    device->executeCommandList(commandList);
    device->waitForIdle();
    device->runGarbageCollection();
    std::printf("%scasts: checked_cast and executed work OK\n", p.c_str());
}

static void runDevice(nvrhi::IDevice* device, const void* backendDevice, const nvrhi::FIID* backendDeviceIID,
    const nvrhi::FIID* backendCommandListIID, Messages& messages)
{
    check(device != nullptr, "NVRHI device");
    runObjects(device, backendDevice, backendDeviceIID, backendCommandListIID, "raw");
#if TEST_VALIDATION
    {
        auto validation = nvrhi::validation::createValidationLayer(device);
        // The wrapper implements nvrhi::IDevice only: the backend IID is refused.
        runObjects(validation, nullptr, backendDeviceIID, nullptr, "validation");
    }
#endif
    device->waitForIdle();
    device->runGarbageCollection();
    check(messages.errors == 0, "no validation/backend errors");
}

int main(int argc, char** argv)
{
    try
    {
        check(argc == 2, "specify d3d11, d3d12 or vulkan");
        const std::string backend = argv[1];
        Messages messages;
#if TEST_D3D11
        if (backend == "d3d11")
        {
            Microsoft::WRL::ComPtr<ID3D11Device> native;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &native, nullptr, &context)), "D3D11 WARP device");
            nvrhi::d3d11::DeviceDesc desc;
            desc.context = context.Get();
            desc.messageCallback = &messages;
            nvrhi::DeviceHandle device = nvrhi::d3d11::createDevice(desc);
            const nvrhi::FLONG refs = refCount(device);
            runDevice(device, nullptr, nullptr, nullptr, messages);
            check(refCount(device) == refs, "d3d11 device reference count balanced");
        }
        else
#endif
#if TEST_D3D12
        if (backend == "d3d12")
        {
            Microsoft::WRL::ComPtr<ID3D12Device> native;
            check(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&native))), "D3D12 device");
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            check(SUCCEEDED(native->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))), "D3D12 queue");
            nvrhi::d3d12::DeviceDesc desc;
            desc.pDevice = native.Get();
            desc.pGraphicsCommandQueue = queue.Get();
            desc.errorCB = &messages;
            nvrhi::d3d12::DeviceHandle device = nvrhi::d3d12::createDevice(desc);
            check(device != nullptr, "d3d12 device");
            // The backend interface, through the native-object path and through the typed query.
            nvrhi::d3d12::IDevice* typed = device->getNativeObject(nvrhi::ObjectTypes::Nvrhi_D3D12_Device);
            check(typed == device.Get(), "Nvrhi_D3D12_Device native object");
            nvrhi::AutoPtr<nvrhi::d3d12::IDevice> queried;
            nvrhi::IDevice* base = device;
            check(base->QueryInterface(NVRHI_IID_PPV_ARGS(&queried)) == nvrhi::FS_OK && queried == device,
                "NVRHI_IID_PPV_ARGS to d3d12::IDevice");
            check(nvrhi::uuid_of<nvrhi::d3d12::IDevice>() != nvrhi::uuid_of<nvrhi::IDevice>(),
                "d3d12::IDevice and nvrhi::IDevice have distinct IIDs");
            queried = nullptr;
            const nvrhi::FLONG refs = refCount(device);
            runDevice(device, static_cast<nvrhi::d3d12::IDevice*>(device), &nvrhi::d3d12::IID_IDevice,
                &nvrhi::d3d12::IID_ICommandList, messages);
            check(refCount(device) == refs, "d3d12 device reference count balanced");
        }
        else
#endif
#if TEST_VULKAN
        if (backend == "vulkan")
        {
#ifdef _WIN32
            auto loader = LoadLibraryA("vulkan-1.dll");
            check(loader != nullptr, "Vulkan loader");
#define LOAD_VK(name) auto name = reinterpret_cast<PFN_##name>(GetProcAddress(loader, #name)); check(name != nullptr, #name)
#else
            auto loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
            check(loader != nullptr, "Vulkan loader");
#define LOAD_VK(name) auto name = reinterpret_cast<PFN_##name>(dlsym(loader, #name)); check(name != nullptr, #name)
#endif
            LOAD_VK(vkGetInstanceProcAddr);
            LOAD_VK(vkCreateInstance);
            LOAD_VK(vkEnumeratePhysicalDevices);
            LOAD_VK(vkGetPhysicalDeviceQueueFamilyProperties);
            LOAD_VK(vkCreateDevice);
            LOAD_VK(vkGetDeviceQueue);
            LOAD_VK(vkDestroyDevice);
            LOAD_VK(vkDestroyInstance);
#undef LOAD_VK
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.apiVersion = VK_API_VERSION_1_3;  // runCasts records barriers: NVRHI uses synchronization2
            VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            instanceInfo.pApplicationInfo = &app;
            VkInstance instance;
            check(vkCreateInstance(&instanceInfo, nullptr, &instance) == VK_SUCCESS, "Vulkan instance");
            uint32_t count = 0;
            check(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count, "Vulkan physical devices");
            std::vector<VkPhysicalDevice> physical(count);
            check(vkEnumeratePhysicalDevices(instance, &count, physical.data()) == VK_SUCCESS, "Vulkan enumeration");
            vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, families.data());
            uint32_t family = 0;
            while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
            check(family < count, "Vulkan graphics queue family");
            float priority = 1.f;
            VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;
            VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            features13.synchronization2 = VK_TRUE;
            VkPhysicalDeviceVulkan12Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            features.pNext = &features13;
            features.timelineSemaphore = VK_TRUE;
            features.descriptorIndexing = VK_TRUE;
            features.runtimeDescriptorArray = VK_TRUE;
            features.descriptorBindingPartiallyBound = VK_TRUE;
            features.descriptorBindingVariableDescriptorCount = VK_TRUE;
            VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            deviceInfo.pNext = &features;
            deviceInfo.queueCreateInfoCount = 1;
            deviceInfo.pQueueCreateInfos = &queueInfo;
            VkDevice native;
            check(vkCreateDevice(physical[0], &deviceInfo, nullptr, &native) == VK_SUCCESS, "Vulkan device");
            nvrhi::vulkan::DeviceDesc desc{};
            desc.instance = instance;
            desc.physicalDevice = physical[0];
            desc.device = native;
            desc.graphicsQueueIndex = int(family);
            vkGetDeviceQueue(native, family, 0, &desc.graphicsQueue);
            desc.errorCB = &messages;
#if !TEST_SHARED
            VULKAN_HPP_DEFAULT_DISPATCHER.init(instance, vkGetInstanceProcAddr, native);
#endif
            {
                nvrhi::vulkan::DeviceHandle device = nvrhi::vulkan::createDevice(desc);
                check(device != nullptr, "vulkan device");
                nvrhi::vulkan::IDevice* typed = device->getNativeObject(nvrhi::ObjectTypes::Nvrhi_VK_Device);
                check(typed == device.Get(), "Nvrhi_VK_Device native object");
                check(nvrhi::uuid_of<nvrhi::vulkan::IDevice>() != nvrhi::uuid_of<nvrhi::IDevice>(),
                    "vulkan::IDevice and nvrhi::IDevice have distinct IIDs");
                const nvrhi::FLONG refs = refCount(device);
                runDevice(device, static_cast<nvrhi::vulkan::IDevice*>(device), &nvrhi::vulkan::IID_IDevice,
                    nullptr, messages);
                check(refCount(device) == refs, "vulkan device reference count balanced");
            }
            vkDestroyDevice(native, nullptr);
            vkDestroyInstance(instance, nullptr);
#ifdef _WIN32
            FreeLibrary(loader);
#else
            dlclose(loader);
#endif
        }
        else
#endif
            throw std::runtime_error("backend not compiled");
        std::printf("PASS: %s QueryInterface, native/validation paths\n", backend.c_str());
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
