// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <cuda.h>
#include <nvrhi/core/foundation.h>
#include <nvrhi/cuda.h>
#include <nvrhi/interop.h>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif
using namespace nvrhi;
struct Messages : ObjectImpl<IMessageCallback>
{
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
    NVRHI_IMPLEMENTS_INTERFACE(IMessageCallback)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    void message(MessageSeverity, const char *message) noexcept override
    {
        std::fprintf(stderr, "NVRHI: %s\n", message);
    }
};
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #c);                                                \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)
#define OK(c) CHECK(NVRHI_SUCCEEDED(c))
#ifdef _WIN32
struct WinHandle
{
    HANDLE value = nullptr;
    ~WinHandle()
    {
        if (value)
            CloseHandle(value);
    }
    void close()
    {
        if (value)
            CloseHandle(value);
        value = nullptr;
    }
};
static D3D12_RESOURCE_DESC bufferDesc(uint64_t size)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}
static int d3d12(IDevice *cuda, IInteropDevice *interop, IDXGIAdapter1 *adapter)
{
    ComPtr<ID3D12Device> device;
    CHECK(SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));
    constexpr uint32_t count = 259;
    constexpr uint64_t bytes = count * 4;
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))));
    ComPtr<ID3D12CommandAllocator> allocator;
    CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    ComPtr<ID3D12GraphicsCommandList> commands;
    CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&commands))));
    auto rd = bufferDesc(bytes);
    auto info = device->GetResourceAllocationInfo(0, 1, &rd);
    const uint64_t offset = info.Alignment;
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = offset + info.SizeInBytes;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> heap;
    CHECK(SUCCEEDED(device->CreateHeap(&hd, IID_PPV_ARGS(&heap))));
    ComPtr<ID3D12Resource> resource;
    CHECK(SUCCEEDED(device->CreatePlacedResource(heap.Get(), offset, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&resource))));
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;
    CHECK(SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&upload))));
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    CHECK(SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&readback))));
    std::vector<uint32_t> input(count);
    for (uint32_t i = 0; i < count; ++i)
        input[i] = i * 13 + 5;
    void *mapped = nullptr;
    D3D12_RANGE empty{};
    CHECK(SUCCEEDED(upload->Map(0, &empty, &mapped)));
    std::memcpy(mapped, input.data(), bytes);
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12Fence> fence;
    CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))));
    WinHandle memoryHandle, fenceHandle;
    CHECK(SUCCEEDED(device->CreateSharedHandle(heap.Get(), nullptr, GENERIC_ALL, nullptr, &memoryHandle.value)));
    CHECK(SUCCEEDED(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle.value)));
    ExternalMemoryDesc memory;
    memory.handleType = ExternalMemoryHandleType::D3D12Heap;
    memory.handle = reinterpret_cast<intptr_t>(memoryHandle.value);
    memory.adapter = interop->getAdapterIdentity();
    memory.allocationSize = hd.SizeInBytes;
    memory.offset = offset;
    BufferHandle imported;
    OK(interop->importBuffer(memory, BufferDesc().setByteSize(bytes).setCanHaveUAVs(true), &imported));
    // Invalid ranges and mismatched adapters fail before touching native handles.
    IBuffer *rejected = reinterpret_cast<IBuffer *>(1);
    auto bad = memory;
    bad.offset = bad.allocationSize - 3;
    CHECK(interop->importBuffer(bad, BufferDesc().setByteSize(4), &rejected) == FE_INVALID_ARGS);
    CHECK(!rejected);
    bad = memory;
    bad.adapter.uuid[0] ^= 1;
    rejected = reinterpret_cast<IBuffer *>(1);
    CHECK(interop->importBuffer(bad, BufferDesc().setByteSize(4), &rejected) == FE_UNSUPPORTED);
    CHECK(!rejected);
    ExternalFenceDesc fd;
    fd.handleType = ExternalFenceHandleType::D3D12Fence;
    fd.handle = reinterpret_cast<intptr_t>(fenceHandle.value);
    fd.adapter = memory.adapter;
    ExternalFenceHandle externalFence;
    OK(interop->importFence(fd, &externalFence));
    // Imports duplicate borrowed OS handles. Their originals can close immediately.
    memoryHandle.close();
    fenceHandle.close();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commands->ResourceBarrier(1, &barrier);
    commands->CopyBufferRegion(resource.Get(), 0, upload.Get(), 0, bytes);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(1, &barrier);
    CHECK(SUCCEEDED(commands->Close()));
    ID3D12CommandList *lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    CHECK(SUCCEEDED(queue->Signal(fence.Get(), 1)));
    BufferHandle cudaReadback;
    OK(cuda->createBuffer(BufferDesc().setByteSize(bytes).setCpuAccess(CpuAccessMode::Read), &cudaReadback));
    CommandListHandle cudaCommands;
    OK(cuda->createCommandList(CommandListParameters().setQueueType(CommandQueue::Compute), &cudaCommands));
    CHECK(interop->waitFence(cudaCommands, externalFence, 1) == FE_INVALID_ARGS);
    cudaCommands->open();
    OK(interop->waitFence(cudaCommands, externalFence, 1));
    cudaCommands->copyBuffer(cudaReadback, 0, imported, 0, bytes);
    cudaCommands->clearBufferUInt(imported, 0x1234abcd);
    OK(interop->signalFence(cudaCommands, externalFence, 2));
    cudaCommands->close();
    CHECK(cuda->executeCommandList(cudaCommands, CommandQueue::Compute));
    imported = nullptr;
    externalFence = nullptr;
    cudaCommands->open();
    cudaCommands->close();
    auto values = static_cast<uint32_t *>(cuda->mapBuffer(cudaReadback, CpuAccessMode::Read));
    CHECK(values);
    for (uint32_t i = 0; i < count; ++i)
        CHECK(values[i] == input[i]);
    cuda->unmapBuffer(cudaReadback);
    CHECK(SUCCEEDED(queue->Wait(fence.Get(), 2)));
    CHECK(SUCCEEDED(allocator->Reset()));
    CHECK(SUCCEEDED(commands->Reset(allocator.Get(), nullptr)));
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commands->ResourceBarrier(1, &barrier);
    commands->CopyBufferRegion(readback.Get(), 0, resource.Get(), 0, bytes);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(1, &barrier);
    CHECK(SUCCEEDED(commands->Close()));
    queue->ExecuteCommandLists(1, lists);
    CHECK(SUCCEEDED(queue->Signal(fence.Get(), 3)));
    WinHandle event;
    event.value = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    CHECK(event.value);
    CHECK(SUCCEEDED(fence->SetEventOnCompletion(3, event.value)));
    CHECK(WaitForSingleObject(event.value, 10000) == WAIT_OBJECT_0);
    D3D12_RANGE readRange{0, bytes};
    CHECK(SUCCEEDED(readback->Map(0, &readRange, &mapped)));
    values = static_cast<uint32_t *>(mapped);
    for (uint32_t i = 0; i < count; ++i)
        CHECK(values[i] == 0x1234abcd);
    readback->Unmap(0, &empty);
    std::puts("CUDA/D3D12 placed buffer offset, bidirectional fences, handle lifetime and rejection tests passed");
    return 0;
}
static int d3d11(IDevice *cuda, IInteropDevice *interop, IDXGIAdapter1 *adapter)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level;
    CHECK(SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                      &device, &level, &context)));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 8;
    td.Height = 8;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ComPtr<ID3D11Texture2D> texture;
    CHECK(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &texture)));
    ComPtr<IDXGIResource1> dxgi;
    CHECK(SUCCEEDED(texture.As(&dxgi)));
    ComPtr<IDXGIKeyedMutex> keyed;
    CHECK(SUCCEEDED(texture.As(&keyed)));
    WinHandle shared;
    CHECK(SUCCEEDED(dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                             &shared.value)));
    ExternalMemoryDesc memory;
    memory.handleType = ExternalMemoryHandleType::D3D11Resource;
    memory.handle = reinterpret_cast<intptr_t>(shared.value);
    memory.adapter = interop->getAdapterIdentity();
    memory.allocationSize = td.Width * td.Height * 4;
    memory.dedicated = true;
    TextureDesc cd;
    cd.width = td.Width;
    cd.height = td.Height;
    cd.format = Format::R32_UINT;
    cd.isUAV = true;
    TextureHandle imported;
    OK(interop->importTexture(memory, cd, &imported));
    ExternalFenceDesc fd;
    fd.handleType = ExternalFenceHandleType::D3D11KeyedMutex;
    fd.handle = memory.handle;
    fd.adapter = memory.adapter;
    ExternalFenceHandle mutex;
    OK(interop->importFence(fd, &mutex));
    shared.close();
    std::vector<uint32_t> input(td.Width * td.Height), output(input.size());
    for (uint32_t i = 0; i < input.size(); ++i)
    {
        input[i] = i + 100;
        output[i] = i * 7 + 11;
    }
    CHECK(keyed->AcquireSync(0, 10000) == S_OK);
    context->UpdateSubresource(texture.Get(), 0, nullptr, input.data(), td.Width * 4, 0);
    context->Flush();
    CHECK(keyed->ReleaseSync(1) == S_OK);
    StagingTextureHandle staging;
    OK(cuda->createStagingTexture(cd, CpuAccessMode::Read, &staging));
    CommandListHandle commands;
    OK(cuda->createCommandList(CommandListParameters().setQueueType(CommandQueue::Compute), &commands));
    commands->open();
    OK(interop->waitFence(commands, mutex, 1, 10000));
    commands->copyTexture2(staging, TextureSlice(), imported, TextureSlice());
    commands->writeTexture(imported, 0, 0, output.data(), td.Width * 4);
    OK(interop->signalFence(commands, mutex, 2));
    commands->close();
    CHECK(cuda->executeCommandList(commands, CommandQueue::Compute));
    CHECK(keyed->AcquireSync(2, 10000) == S_OK);
    size_t pitch = 0;
    auto pixels = static_cast<uint32_t *>(cuda->mapStagingTexture(staging, TextureSlice(), CpuAccessMode::Read, pitch));
    CHECK(pixels);
    for (uint32_t i = 0; i < input.size(); ++i)
        CHECK(pixels[i] == input[i]);
    cuda->unmapStagingTexture(staging);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    CHECK(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &readback)));
    context->CopyResource(readback.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CHECK(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    for (uint32_t y = 0; y < td.Height; ++y)
        for (uint32_t x = 0; x < td.Width; ++x)
            CHECK(reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(mapped.pData) + y * mapped.RowPitch)[x] ==
                  output[y * td.Width + x]);
    context->Unmap(readback.Get(), 0);
    CHECK(keyed->ReleaseSync(3) == S_OK);
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    CHECK(SUCCEEDED(device.As(&device5)));
    CHECK(SUCCEEDED(context.As(&context4)));
    ComPtr<ID3D11Fence> fence;
    CHECK(SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))));
    WinHandle fenceHandle;
    CHECK(SUCCEEDED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle.value)));
    fd.handleType = ExternalFenceHandleType::D3D11Fence;
    fd.handle = reinterpret_cast<intptr_t>(fenceHandle.value);
    ExternalFenceHandle timeline;
    OK(interop->importFence(fd, &timeline));
    fenceHandle.close();
    CHECK(SUCCEEDED(context4->Signal(fence.Get(), 1)));
    context->Flush();
    commands->open();
    OK(interop->waitFence(commands, timeline, 1));
    OK(interop->signalFence(commands, timeline, 2));
    commands->close();
    CHECK(cuda->executeCommandList(commands, CommandQueue::Compute));
    CHECK(SUCCEEDED(context4->Wait(fence.Get(), 2)));
    CHECK(SUCCEEDED(context4->Signal(fence.Get(), 3)));
    context->Flush();
    WinHandle completed;
    completed.value = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    CHECK(completed.value);
    CHECK(SUCCEEDED(fence->SetEventOnCompletion(3, completed.value)));
    CHECK(WaitForSingleObject(completed.value, 5000) == WAIT_OBJECT_0);
    CHECK(cuda->waitForIdle());
    CHECK(fence->GetCompletedValue() >= 3);
    std::puts("CUDA/D3D11 shared texture, keyed mutex and timeline fence tests passed");
    return 0;
}
#endif
static int peer(IDevice *device, IInteropDevice *interop)
{
    cuda::DeviceDesc desc;
    desc.enableOptiX = false;
    auto second = cuda::createDevice(desc);
    CHECK(second);
    InteropDeviceHandle other;
    OK(cuda::nvrhiCudaCreateInteropDevice(second, &other));
    PeerCapabilities caps;
    OK(other->queryPeerCapabilities(device, caps));
    CHECK(caps.sameDevice && caps.nativeCopy);
    constexpr uint64_t bytes = 1024;
    BufferHandle source, dest, readback;
    OK(device->createBuffer(BufferDesc().setByteSize(bytes), &source));
    OK(second->createBuffer(BufferDesc().setByteSize(bytes), &dest));
    OK(second->createBuffer(BufferDesc().setByteSize(bytes).setCpuAccess(CpuAccessMode::Read), &readback));
    CommandListHandle src, dst;
    OK(device->createCommandList(CommandListParameters().setQueueType(CommandQueue::Compute), &src));
    OK(second->createCommandList(CommandListParameters().setQueueType(CommandQueue::Compute), &dst));
    src->open();
    src->clearBufferUInt(source, 0xabcdef12);
    src->close();
    CHECK(device->executeCommandList(src, CommandQueue::Compute));
    dst->open();
    CHECK(other->copyPeerBuffer(dst, dest, bytes - 1, source, 0, 4, CommandQueue::Compute) == FE_INVALID_ARGS);
    OK(other->copyPeerBuffer(dst, dest, 0, source, 0, bytes, CommandQueue::Compute));
    dst->copyBuffer(readback, 0, dest, 0, bytes);
    dst->close();
    CHECK(second->executeCommandList(dst, CommandQueue::Compute));
    auto values = static_cast<uint32_t *>(second->mapBuffer(readback, CpuAccessMode::Read));
    CHECK(values);
    for (uint32_t i = 0; i < bytes / 4; ++i)
        CHECK(values[i] == 0xabcdef12);
    second->unmapBuffer(readback);
    CHECK(device->waitForIdle());
    int adapterCount = 0;
    CHECK(cuDeviceGetCount(&adapterCount) == CUDA_SUCCESS);
    if (adapterCount < 2)
    {
        std::puts("SKIP: actual multi-adapter peer test requires a second CUDA adapter");
    }
    else
    {
        desc.deviceOrdinal = 1;
        auto remote = cuda::createDevice(desc);
        CHECK(remote);
        OK(interop->queryPeerCapabilities(remote, caps));
        CHECK(!caps.sameDevice);
        if (caps.nativeCopy)
        {
            BufferHandle remoteBuffer, localBuffer, localReadback;
            OK(remote->createBuffer(BufferDesc().setByteSize(bytes), &remoteBuffer));
            OK(device->createBuffer(BufferDesc().setByteSize(bytes), &localBuffer));
            OK(device->createBuffer(BufferDesc().setByteSize(bytes).setCpuAccess(CpuAccessMode::Read), &localReadback));
            CommandListHandle remoteCommands, localCommands;
            OK(remote->createCommandList(CommandListParameters().setQueueType(CommandQueue::Compute), &remoteCommands));
            OK(device->createCommandList(CommandListParameters().setQueueType(CommandQueue::Compute), &localCommands));
            remoteCommands->open();
            remoteCommands->clearBufferUInt(remoteBuffer, 0xfedc3210);
            remoteCommands->close();
            CHECK(remote->executeCommandList(remoteCommands, CommandQueue::Compute));
            localCommands->open();
            OK(interop->copyPeerBuffer(localCommands, localBuffer, 0, remoteBuffer, 0, bytes, CommandQueue::Compute));
            localCommands->copyBuffer(localReadback, 0, localBuffer, 0, bytes);
            localCommands->close();
            CHECK(device->executeCommandList(localCommands, CommandQueue::Compute));
            auto localValues = static_cast<uint32_t *>(device->mapBuffer(localReadback, CpuAccessMode::Read));
            CHECK(localValues);
            for (uint32_t i = 0; i < bytes / 4; ++i)
                CHECK(localValues[i] == 0xfedc3210);
            device->unmapBuffer(localReadback);
            CHECK(remote->waitForIdle());
            std::puts("CUDA multi-adapter native peer transfer passed");
        }
        else
        {
            std::puts("Native peer transfer unsupported between available CUDA adapters");
        }
    }
    std::puts("CUDA peer-copy queue ordering and range validation passed");
    return 0;
}
int main()
{
    auto callback = MAKE_RC_OBJ_PTR(Messages);
    cuda::DeviceDesc d;
    d.errorCB = callback;
    d.enableOptiX = false;
    auto device = cuda::createDevice(d);
    if (!device)
        return 77;
    InteropDeviceHandle interop;
    OK(cuda::nvrhiCudaCreateInteropDevice(device, &interop));
    CHECK(interop->getAdapterIdentity().uuidValid);
#ifdef _WIN32
    CHECK(interop->getAdapterIdentity().luidValid);
    LUID luid{};
    std::memcpy(&luid, interop->getAdapterIdentity().luid, sizeof(luid));
    ComPtr<IDXGIFactory4> factory;
    CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter1> adapter;
    CHECK(SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))));
    CHECK(d3d12(device, interop, adapter.Get()) == 0);
    CHECK(d3d11(device, interop, adapter.Get()) == 0);
#endif
    CHECK(peer(device, interop) == 0);
    return 0;
}
