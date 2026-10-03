// QueryInterface on real backend objects, without windows: every object answers IObject, each interface it
// implements (ancestors included) and its class ID with the same object, refuses every other public IID and
// class ID, and QueryInterface keeps the reference count balanced. Also through the validation layer.
//
// checked_cast without RTTI (ADR 0006): checked_cast on real objects, and a small submitted workload that
// runs the backends' own checked_casts (QueryInterface checks in Debug builds) and, through the validation
// layer, the wrappers' QueryInterface type tests (CommandListWrapper in executeCommandLists).
#include <nvrhi/nvrhi.h>
#include <nvrhi/core/foundation.h>
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

struct Messages : nvrhi::ObjectImpl<nvrhi::IMessageCallback>
{
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
    NVRHI_END_INTERFACE_TABLE()

    unsigned errors = 0;
    void message(nvrhi::MessageSeverity severity, const char* text) noexcept override
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

// ---- What every object answers: exactly its interfaces and its class ID (ADR 0007) -----------------------
// Explicit interface tables list each interface, ancestors included, and the class ID. The test checks the
// answer for every public IID and for every class ID of the backend (and of the validation layer): the
// object's own are answered with the right pointer and one reference, all others are refused.

static nvrhi::FIID clsid(const char* text)
{
    return nvrhi::details::str_to_guid(text);
}

// Class IDs of the implementation classes (NVRHI_CLASS_CLSID in src/<backend>/<backend>-backend.h). The
// backend headers are internal, so the test keeps its own copy: a changed class ID fails here.
struct ClassIds
{
    const char* device = nullptr;
    const char* commandList = nullptr;
    const char* texture = nullptr;
    const char* stagingTexture = nullptr;
    const char* buffer = nullptr;
    const char* sampler = nullptr;
    const char* eventQuery = nullptr;
    const char* timerQuery = nullptr;
    const char* framebuffer = nullptr;
    const char* heap = nullptr;
    const char* bindingLayout = nullptr;
    const char* bindlessLayout = nullptr;
    const char* bindingSet = nullptr;
    const char* descriptorTable = nullptr;
    const char* descriptorHeap = nullptr;  // d3d12::IDescriptorHeap (StaticDescriptorHeap)

    std::vector<nvrhi::FIID> all() const
    {
        std::vector<nvrhi::FIID> ids;
        for (const char* id : {device, commandList, texture, stagingTexture, buffer, sampler, eventQuery, timerQuery,
                 framebuffer, heap, bindingLayout, bindlessLayout, bindingSet, descriptorTable, descriptorHeap})
            if (id) ids.push_back(clsid(id));
        return ids;
    }
};

#if TEST_D3D11
static ClassIds d3d11ClassIds()
{
    ClassIds c;
    c.device = "b7ce5dbe-5c3f-4caf-8688-33b2818b0944";
    c.commandList = "8be06890-1fd9-4c8f-9f2e-df9b3e26360b";
    c.texture = "0282c219-19ee-4adc-82cd-acd426854be1";
    c.stagingTexture = "ffb8847e-569b-4b5c-b918-5e7c44c50433";
    c.buffer = "6a3cdf58-6bc7-4f79-9865-9b5e8659021c";
    c.sampler = "796446d2-798e-45b7-947e-420c7ca04fd1";
    c.eventQuery = "67e30c28-c4e3-4789-af22-999c7010d120";
    c.timerQuery = "cdfb5d8c-6c78-4927-948f-f481e930caf1";
    c.framebuffer = "a0b3f4d7-d2e0-447f-9bac-2316d7002f5e";
    c.bindingLayout = "8aa2b216-3465-48e1-a3d8-b8e8e275e812";
    c.bindingSet = "fa444fcd-90a5-41c7-948c-201afa6ccb92";
    return c;
}
#endif
#if TEST_D3D12
static ClassIds d3d12ClassIds()
{
    ClassIds c;
    c.device = "a396b171-cf43-4e48-ad04-a9ad0233de55";
    c.commandList = "492c8ee9-1fe6-4ae7-83ef-339279a67f85";
    c.texture = "98601210-9058-416e-af5f-f20b8a5e23b5";
    c.stagingTexture = "d7e41d38-2cff-477a-8614-9d6c857bd6fa";
    c.buffer = "21664363-1c74-4957-9697-04f56fdba1f7";
    c.sampler = "004ddf5d-40d2-4a51-8826-77e859303fa7";
    c.eventQuery = "a7c1bd43-c37f-4240-8e33-2c95a9cdd4fa";
    c.timerQuery = "38e37193-2e0c-41fa-997f-7b67591f3307";
    c.framebuffer = "8392197d-e14e-49cf-924b-8f58723b053e";
    c.heap = "3d3c451e-be2e-48ec-89dc-fa7d8c2e6802";
    c.bindingLayout = "8055d454-f62e-4d7a-943b-cc6924756e66";
    c.bindlessLayout = "4eb4e730-dd1c-4e78-9d98-f8a4f620f67b";
    c.bindingSet = "013a8656-a296-466f-8ed7-bdcbfa0524f7";
    c.descriptorTable = "08810f03-d563-4aee-a118-cb696b15403d";
    c.descriptorHeap = "8ec56a70-7452-4f25-ae57-015eef6b4f91";
    return c;
}
#endif
#if TEST_VULKAN
static ClassIds vulkanClassIds()
{
    ClassIds c;
    c.device = "2bb8f4b1-3443-4dea-b7cd-b2f90490f929";
    c.commandList = "ee5daf89-6f9d-4d31-86ea-3d87a04c101e";
    c.texture = "29661588-ac9b-4579-a87a-ea1a36ebca49";
    c.stagingTexture = "4c5f6166-ef12-4c87-96a0-7b43c285fe20";
    c.buffer = "bb3801b4-15a7-4609-894c-979502c60111";
    c.sampler = "8233239c-be03-4241-8a53-447ce2b60358";
    c.eventQuery = "3d943abe-a8ad-4b7c-b2c5-d27893952429";
    c.timerQuery = "48a7c1f4-5445-431b-a3ce-abf11e8c7f58";
    c.framebuffer = "6d582262-1aac-4cc8-8f06-50f564887f03";
    c.heap = "fa0cd98f-ea03-4280-a5b0-078b83f7ccbc";
    c.bindingLayout = "9141a5a5-91cd-4be3-9096-19766f06c3fe";
    c.bindlessLayout = c.bindingLayout;  // the Vulkan backend implements bindless layouts with BindingLayout
    c.bindingSet = "33d253d4-00df-4c1d-894c-b860835fe09e";
    c.descriptorTable = "25f6a725-2bd9-4793-ac80-04053ef13912";
    return c;
}
#endif

// The validation layer wraps the device and the command lists; everything else is the backend's object.
static ClassIds validationClassIds(ClassIds c)
{
    c.device = "9301ed69-58b0-454d-9d5d-01bb091e8195";       // DeviceWrapper
    c.commandList = "b1cd41fd-ca88-491c-8fe3-f0e9dc5ce85a";  // CommandListWrapper
    return c;
}

// Every public IID: an object must refuse all those it does not implement.
struct NamedIID
{
    nvrhi::FIID iid;
    const char* name;
};
static std::vector<NamedIID> allInterfaces()
{
    std::vector<NamedIID> v{
        {nvrhi::IID_IRHIObject, "IRHIObject"},
        {nvrhi::IID_IHeap, "IHeap"},
        {nvrhi::IID_ITexture, "ITexture"},
        {nvrhi::IID_IStagingTexture, "IStagingTexture"},
        {nvrhi::IID_ISamplerFeedbackTexture, "ISamplerFeedbackTexture"},
        {nvrhi::IID_IInputLayout, "IInputLayout"},
        {nvrhi::IID_IBuffer, "IBuffer"},
        {nvrhi::IID_IShader, "IShader"},
        {nvrhi::IID_IShaderLibrary, "IShaderLibrary"},
        {nvrhi::IID_ISampler, "ISampler"},
        {nvrhi::IID_IFramebuffer, "IFramebuffer"},
        {nvrhi::rt::IID_IOpacityMicromap, "rt::IOpacityMicromap"},
        {nvrhi::rt::IID_IAccelStruct, "rt::IAccelStruct"},
        {nvrhi::IID_IBindingLayout, "IBindingLayout"},
        {nvrhi::IID_IBindingSet, "IBindingSet"},
        {nvrhi::IID_IDescriptorTable, "IDescriptorTable"},
        {nvrhi::IID_IGraphicsPipeline, "IGraphicsPipeline"},
        {nvrhi::IID_IComputePipeline, "IComputePipeline"},
        {nvrhi::IID_IMeshletPipeline, "IMeshletPipeline"},
        {nvrhi::IID_IEventQuery, "IEventQuery"},
        {nvrhi::IID_ITimerQuery, "ITimerQuery"},
        {nvrhi::rt::IID_IShaderTable, "rt::IShaderTable"},
        {nvrhi::rt::IID_IPipeline, "rt::IPipeline"},
        {nvrhi::IID_ICommandListLifetimeTracker, "ICommandListLifetimeTracker"},
        {nvrhi::IID_ICommandList, "ICommandList"},
        {nvrhi::IID_IDevice, "IDevice"},
        {nvrhi::IID_IMessageCallback, "IMessageCallback"},
        {nvrhi::IID_IWeakReferenceSource, "IWeakReferenceSource"},
    };
#if TEST_D3D12
    v.push_back({nvrhi::d3d12::IID_IRootSignature, "d3d12::IRootSignature"});
    v.push_back({nvrhi::d3d12::IID_ICommandList, "d3d12::ICommandList"});
    v.push_back({nvrhi::d3d12::IID_IDevice, "d3d12::IDevice"});
    v.push_back({nvrhi::d3d12::IID_IDescriptorHeap, "d3d12::IDescriptorHeap"});
#endif
#if TEST_VULKAN
    v.push_back({nvrhi::vulkan::IID_IDevice, "vulkan::IDevice"});
#endif
    return v;
}

struct Answer
{
    nvrhi::FIID iid;
    const void* pointer;
};

// Answers exactly `answers` (each interface of the object's chain, with its pointer) and the class ID
// `ownClassId` among `classIds`; identity is the same from every answered interface; the reference count
// stays balanced.
static void checkObject(nvrhi::IObject* object, const std::string& what, std::initializer_list<Answer> answers,
    const char* ownClassId, const ClassIds& classIds)
{
    const nvrhi::FLONG before = refCount(object);
    void* identityPv = nullptr;
    check(object->QueryInterface(nvrhi::IID_IObject, &identityPv) == nvrhi::FS_OK, (what + " IObject").c_str());
    nvrhi::IObject* identity = static_cast<nvrhi::IObject*>(identityPv);
    identity->Release();

    int answered = 0, refused = 0;
    for (const NamedIID& itf : allInterfaces())
    {
        const void* expected = nullptr;
        for (const Answer& a : answers)
            if (a.iid == itf.iid) expected = a.pointer;
        expectQI(object, itf.iid, expected, what + " " + itf.name);
        if (!expected)
        {
            ++refused;
            continue;
        }
        ++answered;
        // From that interface, the identity is the same.
        void* id = nullptr;
        check(static_cast<nvrhi::IObject*>(const_cast<void*>(expected))->QueryInterface(nvrhi::IID_IObject, &id) == nvrhi::FS_OK
            && id == identity, (what + " " + itf.name + ": identity differs").c_str());
        static_cast<nvrhi::IObject*>(id)->Release();
    }
    check(answered == int(answers.size()), (what + ": an expected interface is not a public IID of the test").c_str());

    // The class ID: answered with the class itself (one reference added); a helper base may precede the
    // class's IObject (e.g. vulkan Texture : MemoryResource, ObjectImpl<ITexture>), so the class pointer is at
    // or a little before the identity. Every other class ID is refused.
    bool ownSeen = false;
    for (const nvrhi::FIID& id : classIds.all())
    {
        const bool own = ownClassId && id == clsid(ownClassId);
        if (own && ownSeen) continue;  // listed twice (Vulkan bindless layout)
        void* pv = reinterpret_cast<void*>(1);
        const nvrhi::FRESULT hr = object->QueryInterface(id, &pv);
        if (own)
        {
            ownSeen = true;
            check(hr == nvrhi::FS_OK && pv, (what + ": class ID not answered").c_str());
            const ptrdiff_t delta = reinterpret_cast<char*>(identity) - static_cast<char*>(pv);
            check(delta >= 0 && delta < 256, (what + ": class ID answered with another object").c_str());
            check(refCount(object) == before + 1, (what + ": class ID query did not add one reference").c_str());
            object->Release();
            check(object->QueryInterface(id, nullptr) == nvrhi::FS_OK, (what + ": class ID, null ppv").c_str());
        }
        else
        {
            check(hr == nvrhi::FE_NOINTERFACE && pv == nullptr, (what + ": another class ID not refused").c_str());
            ++refused;
        }
    }
    check(!ownClassId || ownSeen, (what + ": no class ID checked").c_str());
    check(refCount(object) == before, (what + ": reference count not balanced").c_str());
    // AutoPtr::As goes through the same path.
    {
        nvrhi::AutoPtr<nvrhi::IObject> base;
        check(nvrhi::AutoPtr<nvrhi::IObject>(object).As(&base) == nvrhi::FS_OK && base == identity,
            (what + ": AutoPtr::As").c_str());
    }
    check(refCount(object) == before, (what + ": reference count not balanced after AutoPtr::As").c_str());
    std::printf("%s: QueryInterface OK (%d answered + class ID, %d refused, refcount %d)\n", what.c_str(), answered,
        refused, int(before));
}

static void runCasts(nvrhi::IDevice* device, const std::string& p);

static void runObjects(nvrhi::IDevice* device, const void* backendDevice, const nvrhi::FIID* backendDeviceIID,
    const nvrhi::FIID* backendCommandListIID, const ClassIds& ids, const char* path)
{
    std::printf("Device path: %s\n", path);
    const std::string p = std::string(path) + " ";
    using nvrhi::IID_IRHIObject;

    // Device: nvrhi::IDevice, and the backend's IDevice when the object implements it.
    if (backendDevice)
        checkObject(device, p + "device", {{IID_IRHIObject, device}, {nvrhi::IID_IDevice, device}, {*backendDeviceIID, backendDevice}},
            ids.device, ids);
    else
        checkObject(device, p + "device", {{IID_IRHIObject, device}, {nvrhi::IID_IDevice, device}}, ids.device, ids);
    if (backendDevice)
    {
        void* id = nullptr;
        check(static_cast<nvrhi::IObject*>(const_cast<void*>(backendDevice))->QueryInterface(nvrhi::IID_IObject, &id) == nvrhi::FS_OK
            && id == static_cast<nvrhi::IObject*>(device), (p + "backend device identity").c_str());
        static_cast<nvrhi::IObject*>(id)->Release();
    }

    nvrhi::TextureHandle rt;
    device->createTexture(nvrhi::TextureDesc().setWidth(16).setHeight(16)
        .setFormat(nvrhi::Format::RGBA8_UNORM).setIsRenderTarget(true)
        .setInitialState(nvrhi::ResourceStates::RenderTarget).setKeepInitialState(true).setDebugName("qi texture"), &rt);
    check(rt != nullptr, "create texture");
    checkObject(rt, p + "texture", {{IID_IRHIObject, rt.Get()}, {nvrhi::IID_ITexture, rt.Get()}}, ids.texture, ids);

    {
        nvrhi::StagingTextureHandle staging;
        device->createStagingTexture(nvrhi::TextureDesc().setWidth(16).setHeight(16)
            .setFormat(nvrhi::Format::RGBA8_UNORM).setDebugName("qi staging texture"), nvrhi::CpuAccessMode::Read, &staging);
        check(staging != nullptr, "create staging texture");
        checkObject(staging, p + "staging texture", {{IID_IRHIObject, staging.Get()}, {nvrhi::IID_IStagingTexture, staging.Get()}},
            ids.stagingTexture, ids);
    }

    {
        nvrhi::BufferHandle buffer;
        device->createBuffer(nvrhi::BufferDesc().setByteSize(256).setDebugName("qi buffer"), &buffer);
        check(buffer != nullptr, "create buffer");
        checkObject(buffer, p + "buffer", {{IID_IRHIObject, buffer.Get()}, {nvrhi::IID_IBuffer, buffer.Get()}}, ids.buffer, ids);
    }

    {
        nvrhi::SamplerHandle sampler;
        device->createSampler(nvrhi::SamplerDesc(), &sampler);
        check(sampler != nullptr, "create sampler");
        checkObject(sampler, p + "sampler", {{IID_IRHIObject, sampler.Get()}, {nvrhi::IID_ISampler, sampler.Get()}}, ids.sampler, ids);
    }

    {
        nvrhi::EventQueryHandle query;
        device->createEventQuery(&query);
        check(query != nullptr, "create event query");
        checkObject(query, p + "event query", {{IID_IRHIObject, query.Get()}, {nvrhi::IID_IEventQuery, query.Get()}},
            ids.eventQuery, ids);
        nvrhi::TimerQueryHandle timer;
        device->createTimerQuery(&timer);
        check(timer != nullptr, "create timer query");
        checkObject(timer, p + "timer query", {{IID_IRHIObject, timer.Get()}, {nvrhi::IID_ITimerQuery, timer.Get()}},
            ids.timerQuery, ids);
    }

    {
        nvrhi::FramebufferHandle framebuffer;
        device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(rt), &framebuffer);
        check(framebuffer != nullptr, "create framebuffer");
        checkObject(framebuffer, p + "framebuffer", {{IID_IRHIObject, framebuffer.Get()}, {nvrhi::IID_IFramebuffer, framebuffer.Get()}},
            ids.framebuffer, ids);
    }

    if (ids.heap)
    {
        nvrhi::HeapHandle heap;
        device->createHeap(nvrhi::HeapDesc().setCapacity(65536).setType(nvrhi::HeapType::DeviceLocal)
            .setDebugName("qi heap"), &heap);
        check(heap != nullptr, "create heap");
        checkObject(heap, p + "heap", {{IID_IRHIObject, heap.Get()}, {nvrhi::IID_IHeap, heap.Get()}}, ids.heap, ids);
    }

    {
        nvrhi::CommandListHandle commandList;
        device->createCommandList(nvrhi::CommandListParameters(), &commandList);
        check(commandList != nullptr, "create command list");
        nvrhi::ICommandList* c = commandList;
        if (backendCommandListIID)
        {
            // The backend interface is the same object; find its pointer through the query itself and
            // check it upcasts back to the nvrhi interface.
            void* pv = nullptr;
            check(c->QueryInterface(*backendCommandListIID, &pv) == nvrhi::FS_OK, (p + "backend ICommandList").c_str());
            static_cast<nvrhi::IObject*>(pv)->Release();
#if TEST_D3D12
            if (backendCommandListIID == &nvrhi::d3d12::IID_ICommandList)
                check(static_cast<nvrhi::ICommandList*>(static_cast<nvrhi::d3d12::ICommandList*>(pv)) == c,
                    (p + "d3d12::ICommandList upcasts to the same nvrhi::ICommandList").c_str());
#endif
            checkObject(c, p + "command list", {{IID_IRHIObject, c}, {nvrhi::IID_ICommandList, c}, {*backendCommandListIID, pv}},
                ids.commandList, ids);
        }
        else
            checkObject(c, p + "command list", {{IID_IRHIObject, c}, {nvrhi::IID_ICommandList, c}}, ids.commandList, ids);
    }

    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::All;
        layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
        nvrhi::BindingLayoutHandle layout;
        device->createBindingLayout(layoutDesc, &layout);
        check(layout != nullptr, "create binding layout");
        checkObject(layout, p + "binding layout", {{IID_IRHIObject, layout.Get()}, {nvrhi::IID_IBindingLayout, layout.Get()}},
            ids.bindingLayout, ids);
        nvrhi::BufferHandle cb;
        device->createBuffer(nvrhi::BufferDesc().setByteSize(256).setIsConstantBuffer(true)
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true).setDebugName("qi cb"), &cb);
        check(cb != nullptr, "create constant buffer");
        nvrhi::BindingSetDesc setDesc;
        setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, cb));
        nvrhi::BindingSetHandle set;
        device->createBindingSet(setDesc, layout, &set);
        check(set != nullptr, "create binding set");
        checkObject(set, p + "binding set", {{IID_IRHIObject, set.Get()}, {nvrhi::IID_IBindingSet, set.Get()}}, ids.bindingSet, ids);
    }

    // Descriptor table: IDescriptorTable : IBindingSet, where the backend supports bindless layouts.
    if (device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11)
    {
        nvrhi::BindlessLayoutDesc bindless;
        bindless.visibility = nvrhi::ShaderType::All;
        bindless.maxCapacity = 16;
        bindless.layoutType = nvrhi::BindlessLayoutDesc::LayoutType::Immutable;
        bindless.addRegisterSpace(nvrhi::BindingLayoutItem::Texture_SRV(1));
        nvrhi::BindingLayoutHandle layout;
        device->createBindlessLayout(bindless, &layout);
        if (layout)
        {
            nvrhi::DescriptorTableHandle table;
            device->createDescriptorTable(layout, &table);
            check(table != nullptr, "create descriptor table");
            nvrhi::IDescriptorTable* d = table;
            checkObject(d, p + "descriptor table", {{IID_IRHIObject, d}, {nvrhi::IID_IBindingSet, static_cast<nvrhi::IBindingSet*>(d)},
                {nvrhi::IID_IDescriptorTable, d}}, ids.descriptorTable, ids);
            checkObject(layout, p + "bindless layout", {{IID_IRHIObject, layout.Get()}, {nvrhi::IID_IBindingLayout, layout.Get()}},
                ids.bindlessLayout, ids);
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
    nvrhi::TextureHandle texture;
    device->createTexture(nvrhi::TextureDesc().setWidth(16).setHeight(16)
        .setFormat(nvrhi::Format::RGBA8_UNORM).setIsRenderTarget(true)
        .setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true)
        .setDebugName("cast texture"), &texture);
    check(texture != nullptr, "create cast texture");
    nvrhi::BufferHandle buffer;
    device->createBuffer(nvrhi::BufferDesc().setByteSize(256).setIsConstantBuffer(true)
        .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true)
        .setDebugName("cast buffer"), &buffer);
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
    nvrhi::BindingLayoutHandle layout;
    device->createBindingLayout(layoutDesc, &layout);
    check(layout != nullptr, "create cast binding layout");
    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture));
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, buffer));
    nvrhi::BindingSetHandle set;
    device->createBindingSet(setDesc, layout, &set);
    check(set != nullptr, "create cast binding set");

    // Recorded and executed work: the backend casts the command list, the texture and the buffer; the
    // validation layer finds its CommandListWrapper through QueryInterface.
    nvrhi::CommandListHandle commandList;
    device->createCommandList(nvrhi::CommandListParameters(), &commandList);
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
    const nvrhi::FIID* backendCommandListIID, const ClassIds& ids, Messages& messages)
{
    check(device != nullptr, "NVRHI device");
    runObjects(device, backendDevice, backendDeviceIID, backendCommandListIID, ids, "raw");
#if TEST_VALIDATION
    {
        auto validation = nvrhi::validation::createValidationLayer(device);
        // The wrappers implement the nvrhi interfaces only: the backend IIDs and class IDs are refused.
        runObjects(validation, nullptr, backendDeviceIID, nullptr, validationClassIds(ids), "validation");
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
        nvrhi::AutoPtr<Messages> messagesHandle = MAKE_RC_OBJ_PTR(Messages);
        Messages& messages = *messagesHandle;
        checkObject(messagesHandle, "message callback", {{nvrhi::IID_IRHIObject, messagesHandle.Get()},
            {nvrhi::IID_IMessageCallback, messagesHandle.Get()}}, nullptr, ClassIds());
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
            runDevice(device, nullptr, nullptr, nullptr, d3d11ClassIds(), messages);
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
            auto typed = static_cast<nvrhi::d3d12::IDevice*>(device->getNativeObject(nvrhi::ObjectTypes::Nvrhi_D3D12_Device));
            check(typed == device.Get(), "Nvrhi_D3D12_Device native object");
            nvrhi::AutoPtr<nvrhi::d3d12::IDevice> queried;
            nvrhi::IDevice* base = device;
            check(base->QueryInterface(NVRHI_IID_PPV_ARGS(&queried)) == nvrhi::FS_OK && queried == device,
                "NVRHI_IID_PPV_ARGS to d3d12::IDevice");
            check(nvrhi::uuid_of<nvrhi::d3d12::IDevice>() != nvrhi::uuid_of<nvrhi::IDevice>(),
                "d3d12::IDevice and nvrhi::IDevice have distinct IIDs");
            queried = nullptr;
            // The descriptor heaps: COM objects owned by the device; the handles come back through retVal.
            {
                nvrhi::d3d12::IDescriptorHeap* heap = device->getDescriptorHeap(nvrhi::d3d12::DescriptorHeapType::ShaderResourceView);
                check(heap != nullptr, "d3d12 descriptor heap");
                checkObject(heap, "d3d12 descriptor heap", {{nvrhi::IID_IRHIObject, heap},
                    {nvrhi::d3d12::IID_IDescriptorHeap, heap}}, d3d12ClassIds().descriptorHeap, d3d12ClassIds());
                D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
                check(&heap->getCpuHandle(cpu, 0) == &cpu && cpu.ptr == heap->getHeap()->GetCPUDescriptorHandleForHeapStart().ptr,
                    "d3d12 descriptor heap getCpuHandle");
                D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
                check(heap->getGpuHandle(gpu, 0).ptr == heap->getShaderVisibleHeap()->GetGPUDescriptorHandleForHeapStart().ptr,
                    "d3d12 descriptor heap getGpuHandle");
            }
            const nvrhi::FLONG refs = refCount(device);
            runDevice(device, static_cast<nvrhi::d3d12::IDevice*>(device), &nvrhi::d3d12::IID_IDevice,
                &nvrhi::d3d12::IID_ICommandList, d3d12ClassIds(), messages);
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
                auto typed = static_cast<nvrhi::vulkan::IDevice*>(device->getNativeObject(nvrhi::ObjectTypes::Nvrhi_VK_Device));
                check(typed == device.Get(), "Nvrhi_VK_Device native object");
                check(nvrhi::uuid_of<nvrhi::vulkan::IDevice>() != nvrhi::uuid_of<nvrhi::IDevice>(),
                    "vulkan::IDevice and nvrhi::IDevice have distinct IIDs");
                const nvrhi::FLONG refs = refCount(device);
                runDevice(device, static_cast<nvrhi::vulkan::IDevice*>(device), &nvrhi::vulkan::IID_IDevice,
                    nullptr, vulkanClassIds(), messages);
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
