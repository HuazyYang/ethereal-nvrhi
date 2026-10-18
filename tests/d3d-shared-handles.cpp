// SPDX-License-Identifier: MIT
#include <nvrhi/core/foundation.h>
#if TEST_D3D11
#include <nvrhi/d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#endif
#if TEST_D3D12
#include <nvrhi/d3d12.h>
#endif
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace nvrhi;
using Microsoft::WRL::ComPtr;
namespace
{
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
struct Messages : ObjectImpl<IMessageCallback>
{
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    unsigned errors = 0;
    void message(MessageSeverity severity, const char* text) noexcept override
    {
        if (severity >= MessageSeverity::Error) ++errors;
        std::fprintf(stderr, "NVRHI: %s\n", text);
    }
};
void ownedHandleValid(HANDLE handle)
{
    DWORD flags = 0;
    require(handle && GetHandleInformation(handle, &flags), "owned NT handle must be valid");
}
void ownedHandleClosed(HANDLE handle)
{
    DWORD flags = 0;
    SetLastError(ERROR_SUCCESS);
    require(!GetHandleInformation(handle, &flags) && GetLastError() == ERROR_INVALID_HANDLE,
        "resource destructor must close its owned NT handle");
}
TextureDesc textureDesc(SharedResourceFlags flags)
{
    TextureDesc td;
    td.width = 16; td.height = 8; td.format = Format::RGBA8_UNORM;
    td.isUAV = true; td.sharedResourceFlags = flags;
    td.initialState = ResourceStates::Common;
    return td;
}
#if TEST_D3D11
int testD3D11()
{
    ComPtr<ID3D11Device> native;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &native, nullptr, &context))) return 77;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    if (FAILED(native.As(&device5)) || FAILED(context.As(&context4))) return 77;
    auto messages = MAKE_RC_OBJ_PTR(Messages);
    d3d11::DeviceDesc desc; desc.context = context.Get(); desc.messageCallback = messages;
    auto device = d3d11::createDevice(desc);
    require(bool(device), "NVRHI D3D11 device");
    TextureHandle texture;
    require(NVRHI_SUCCEEDED(device->createTexture(textureDesc(SharedResourceFlags::Shared_NTHandle), &texture)),
        "create D3D11 NT texture");
    auto handle = static_cast<HANDLE>(texture->getNativeObject(ObjectTypes::SharedHandle));
    ownedHandleValid(handle);
    ComPtr<ID3D11Texture2D> opened;
    require(SUCCEEDED(device5->OpenSharedResource1(handle, IID_PPV_ARGS(&opened))), "open D3D11 NT resource");
    texture = nullptr;
    ownedHandleClosed(handle); // Opened resource is deliberately still alive.
    opened.Reset();

    require(NVRHI_SUCCEEDED(device->createTexture(textureDesc(SharedResourceFlags::Shared), &texture)),
        "create D3D11 KMT texture");
    auto* resource = static_cast<ID3D11Resource*>(texture->getNativeObject(ObjectTypes::D3D11_Resource));
    ComPtr<IDXGIResource> dxgi;
    require(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&dxgi))), "query native KMT resource");
    HANDLE nativeHandle = nullptr;
    require(SUCCEEDED(dxgi->GetSharedHandle(&nativeHandle)), "query KMT handle");
    require(static_cast<HANDLE>(texture->getNativeObject(ObjectTypes::SharedHandle)) == nativeHandle,
        "SharedHandle must expose the KMT handle, not the resource pointer");
    texture = nullptr;
    // KMT handles are borrowed and are never passed to CloseHandle. The native
    // resource remains alive through dxgi, so opening after wrapper destruction is valid.
    require(SUCCEEDED(native->OpenSharedResource(nativeHandle, IID_PPV_ARGS(&opened))), "KMT native owner lifetime");
    opened.Reset(); dxgi.Reset();

    ComPtr<ID3D11Fence> gate;
    require(SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate))), "query test gate");
    ComPtr<ID3D11Device> signalingDevice;
    ComPtr<ID3D11DeviceContext> signalingContext;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &signalingDevice, nullptr, &signalingContext)), "independent signaling device");
    ComPtr<ID3D11Device5> signalingDevice5;
    ComPtr<ID3D11DeviceContext4> signalingContext4;
    require(SUCCEEDED(signalingDevice.As(&signalingDevice5)) && SUCCEEDED(signalingContext.As(&signalingContext4)),
        "independent fence interfaces");
    HANDLE gateHandle = nullptr;
    require(SUCCEEDED(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &gateHandle)), "export query gate");
    ComPtr<ID3D11Fence> signalingGate;
    const auto openResult = signalingDevice5->OpenSharedFence(gateHandle, IID_PPV_ARGS(&signalingGate));
    CloseHandle(gateHandle);
    require(SUCCEEDED(openResult), "open independent query gate");
    EventQueryHandle query;
    require(NVRHI_SUCCEEDED(device->createEventQuery(&query)), "query creation");
    require(SUCCEEDED(context4->Wait(gate.Get(), 1)), "hold GPU query pending");
    device->setEventQuery(query, CommandQueue::Graphics);
    context->Flush();
    const bool pendingReportedComplete = device->pollEventQuery(query);
    // Release the GPU before checking the regression, including on a failing build.
    require(SUCCEEDED(signalingContext4->Signal(signalingGate.Get(), 1)), "release query gate");
    signalingContext->Flush();
    device->waitEventQuery(query);
    require(!pendingReportedComplete, "S_FALSE must not report a pending GPU query complete");
    require(device->pollEventQuery(query), "completed query must report complete");
    require(!messages->errors, "D3D11 error callback");
    std::puts("PASS D3D11 NT handle ownership, borrowed KMT lifetime, pending GPU query");
    return 0;
}
#endif
#if TEST_D3D12
int testD3D12()
{
    ComPtr<ID3D12Device> native;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&native)))) return 77;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{};
    require(SUCCEEDED(native->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))), "D3D12 graphics queue");
    auto messages = MAKE_RC_OBJ_PTR(Messages);
    d3d12::DeviceDesc desc; desc.pDevice = native.Get(); desc.pGraphicsCommandQueue = queue.Get(); desc.errorCB = messages;
    auto device = d3d12::createDevice(desc);
    require(bool(device), "NVRHI D3D12 device");
    TextureHandle texture;
    require(NVRHI_SUCCEEDED(device->createTexture(textureDesc(SharedResourceFlags::Shared), &texture)), "shared D3D12 texture");
    auto handle = static_cast<HANDLE>(texture->getNativeObject(ObjectTypes::SharedHandle));
    ownedHandleValid(handle);
    ComPtr<ID3D12Resource> opened;
    require(SUCCEEDED(native->OpenSharedHandle(handle, IID_PPV_ARGS(&opened))), "open D3D12 shared texture");
    texture = nullptr;
    ownedHandleClosed(handle);
    opened.Reset();
    BufferDesc bd; bd.byteSize = 1024; bd.canHaveRawViews = true;
    bd.sharedResourceFlags = SharedResourceFlags::Shared;
    BufferHandle buffer;
    require(NVRHI_SUCCEEDED(device->createBuffer(bd, &buffer)), "shared D3D12 buffer");
    handle = static_cast<HANDLE>(buffer->getNativeObject(ObjectTypes::SharedHandle));
    ownedHandleValid(handle);
    require(SUCCEEDED(native->OpenSharedHandle(handle, IID_PPV_ARGS(&opened))), "open D3D12 shared buffer");
    buffer = nullptr;
    ownedHandleClosed(handle);
    require(!messages->errors, "D3D12 error callback");
    std::puts("PASS D3D12 shared buffer and texture NT handle ownership");
    return 0;
}
#endif
}
int main(int argc, char** argv)
{
    try
    {
#if TEST_D3D11
        if (argc == 2 && !std::strcmp(argv[1], "d3d11")) return testD3D11();
#endif
#if TEST_D3D12
        if (argc == 2 && !std::strcmp(argv[1], "d3d12")) return testD3D12();
#endif
        return 1;
    }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
