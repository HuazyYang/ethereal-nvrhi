// SPDX-License-Identifier: MIT
#include "cuda-backend.h"
#include <nvrhi/interop.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace nvrhi::cuda
{
namespace
{
template <class Pointer> Pointer backendObject(IObject *object)
{
    Pointer pointer = nullptr;
    if (!object || NVRHI_FAILED(object->QueryInterface(NVRHI_IID_PPV_ARGS(&pointer))))
        return nullptr;
    pointer->Release();
    return pointer; // The caller keeps its input object alive; recording retains it below.
}
FRESULT result(CUresult code)
{
    if (code == CUDA_SUCCESS)
        return FS_OK;
    if (code == CUDA_ERROR_OUT_OF_MEMORY)
        return FE_OUT_OF_MEMORY;
    if (code == CUDA_ERROR_NOT_SUPPORTED || code == CUDA_ERROR_PEER_ACCESS_UNSUPPORTED)
        return FE_UNSUPPORTED;
    if (code == CUDA_ERROR_INVALID_VALUE || code == CUDA_ERROR_INVALID_HANDLE)
        return FE_INVALID_ARGS;
    return FE_GENERIC_ERROR;
}
AdapterIdentity identity(CUdevice device)
{
    AdapterIdentity id;
    CUuuid uuid{};
    id.uuidValid = cuDeviceGetUuid(&uuid, device) == CUDA_SUCCESS;
    if (id.uuidValid)
        std::memcpy(id.uuid, uuid.bytes, 16);
#ifdef _WIN32
    unsigned nodeMask = 0;
    id.luidValid = cuDeviceGetLuid(reinterpret_cast<char *>(id.luid), &nodeMask, device) == CUDA_SUCCESS;
    id.nodeMask = nodeMask;
#endif
    return id;
}
bool sameAdapter(const AdapterIdentity &a, const AdapterIdentity &b)
{
    // If both identities expose a UUID, it is authoritative. LUID matching also
    // checks the node mask so linked-adapter resources cannot silently cross nodes.
    if (a.uuidValid && b.uuidValid)
        return std::memcmp(a.uuid, b.uuid, 16) == 0;
    return a.luidValid && b.luidValid && std::memcmp(a.luid, b.luid, 8) == 0 && a.nodeMask && b.nodeMask &&
           (a.nodeMask & b.nodeMask) != 0;
}
struct DuplicatedHandle
{
    intptr_t value = -1;
    bool fd = false, owned = false;
    ~DuplicatedHandle()
    {
        if (!owned)
            return;
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(value));
#else
        close(int(value));
#endif
    }
    FRESULT duplicate(intptr_t input, bool fileDescriptor, bool kmt = false)
    {
        fd = fileDescriptor;
        if (input == -1 || (!fileDescriptor && !input))
            return FE_INVALID_ARGS;
#ifdef _WIN32
        if (fileDescriptor)
            return FE_UNSUPPORTED;
        if (kmt)
        {
            value = input;
            return FS_OK;
        }
        HANDLE copy = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(input), GetCurrentProcess(), &copy, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            return FE_INVALID_ARGS;
        value = reinterpret_cast<intptr_t>(copy);
#else
        if (!fileDescriptor)
            return FE_UNSUPPORTED;
        value = dup(int(input));
        if (value < 0)
            return FE_INVALID_ARGS;
#endif
        owned = true;
        return FS_OK;
    }
    void transferred()
    {
        if (fd)
            owned = false;
    }
};
bool memoryType(ExternalMemoryHandleType type, CUexternalMemoryHandleType &native, bool &fd, bool &kmt)
{
    fd = false;
    kmt = false;
    switch (type)
    {
    case ExternalMemoryHandleType::OpaqueFd:
        fd = true;
        native = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
        break;
    case ExternalMemoryHandleType::OpaqueWin32:
        native = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
        break;
    case ExternalMemoryHandleType::OpaqueWin32Kmt:
        kmt = true;
        native = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT;
        break;
    case ExternalMemoryHandleType::D3D11Resource:
        native = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_RESOURCE;
        break;
    case ExternalMemoryHandleType::D3D11ResourceKmt:
        kmt = true;
        native = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_RESOURCE_KMT;
        break;
    case ExternalMemoryHandleType::D3D12Heap:
        native = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP;
        break;
    case ExternalMemoryHandleType::D3D12Resource:
        native = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
        break;
    default:
        return false;
    }
    return true;
}
bool fenceType(ExternalFenceHandleType type, CUexternalSemaphoreHandleType &native, bool &fd, bool &kmt)
{
    fd = false;
    kmt = false;
    switch (type)
    {
    case ExternalFenceHandleType::BinaryFd:
        fd = true;
        native = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;
        break;
    case ExternalFenceHandleType::BinaryWin32:
        native = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32;
        break;
    case ExternalFenceHandleType::TimelineFd:
        fd = true;
        native = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_FD;
        break;
    case ExternalFenceHandleType::TimelineWin32:
        native = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_WIN32;
        break;
    case ExternalFenceHandleType::D3D11Fence:
        native = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE;
        break;
    case ExternalFenceHandleType::D3D12Fence:
        native = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE;
        break;
    case ExternalFenceHandleType::D3D11KeyedMutex:
        native = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_KEYED_MUTEX;
        break;
    case ExternalFenceHandleType::D3D11KeyedMutexKmt:
        kmt = true;
        native = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_KEYED_MUTEX_KMT;
        break;
    default:
        return false;
    }
    return true;
}
struct ImportedMemory
{
    ContextPtr context;
    RHIObjectHandle source;
    CUexternalMemory handle = nullptr;
    CUdeviceptr buffer = 0;
    CUmipmappedArray array = nullptr;
    ~ImportedMemory()
    {
        Scope scope(context->handle);
        if (buffer)
            cuMemFree(buffer);
        if (array)
            cuMipmappedArrayDestroy(array);
        if (handle)
            cuDestroyExternalMemory(handle);
    }
};
NVRHI_CLASS_CLSID(ImportedFence, "5490c031-09e1-40ad-a1ab-701827580031")
class ImportedFence final : public ObjectImpl<IExternalFence>
{
  public:
    CUDA_INTERFACES(ImportedFence, IExternalFence)
    ContextPtr context;
    ExternalFenceDesc desc;
    RHIObjectHandle source;
    CUexternalSemaphore semaphore = nullptr;
    ImportedFence(ContextPtr c, const ExternalFenceDesc &d) : context(c), desc(d), source(d.sourceOwner)
    {
    }
    ~ImportedFence()
    {
        Scope scope(context->handle);
        if (semaphore)
            cuDestroyExternalSemaphore(semaphore);
    }
    const ExternalFenceDesc &getDesc() const noexcept override
    {
        return desc;
    }
    bool binary() const
    {
        return desc.handleType == ExternalFenceHandleType::BinaryFd ||
               desc.handleType == ExternalFenceHandleType::BinaryWin32;
    }
    bool keyed() const
    {
        return desc.handleType == ExternalFenceHandleType::D3D11KeyedMutex ||
               desc.handleType == ExternalFenceHandleType::D3D11KeyedMutexKmt;
    }
};
class InteropDevice final : public ObjectImpl<IInteropDevice>
{
    AutoPtr<Device> m_device;
    ContextPtr m_context;
    AdapterIdentity m_identity;
    FRESULT importMemory(const ExternalMemoryDesc &d, std::shared_ptr<ImportedMemory> &imported)
    {
        if (!d.allocationSize || d.offset >= d.allocationSize)
            return FE_INVALID_ARGS;
        if (!sameAdapter(m_identity, d.adapter))
            return FE_UNSUPPORTED;
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC native{};
        bool fd = false, kmt = false;
        if (!memoryType(d.handleType, native.type, fd, kmt))
            return FE_UNSUPPORTED;
        if (kmt && !d.sourceOwner)
            return FE_INVALID_ARGS;
        DuplicatedHandle handle;
        const auto status = handle.duplicate(d.handle, fd, kmt);
        if (NVRHI_FAILED(status))
            return status;
        if (fd)
            native.handle.fd = int(handle.value);
        else
            native.handle.win32.handle = reinterpret_cast<void *>(handle.value);
        native.size = d.allocationSize;
        if (d.dedicated || d.handleType == ExternalMemoryHandleType::D3D11Resource ||
            d.handleType == ExternalMemoryHandleType::D3D11ResourceKmt ||
            d.handleType == ExternalMemoryHandleType::D3D12Resource)
            native.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
        auto allocation = std::make_shared<ImportedMemory>();
        allocation->context = m_context;
        allocation->source = d.sourceOwner;
        const auto code = cuImportExternalMemory(&allocation->handle, &native);
        if (code != CUDA_SUCCESS)
            return result(code);
        handle.transferred();
        imported = std::move(allocation);
        return FS_OK;
    }
    CommandList *list(ICommandList *command) const
    {
        auto *value = backendObject<CommandList *>(command);
        return value && value->context == m_context && value->opened && value->valid ? value : nullptr;
    }

  public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(InteropDevice)
    NVRHI_IMPLEMENTS_INTERFACE(IInteropDevice)
    NVRHI_IMPLEMENTS_INTERFACE(IRHIObject)
    NVRHI_END_INTERFACE_TABLE()
    explicit InteropDevice(Device *device)
        : m_device(device), m_context(device->context), m_identity(identity(device->context->device))
    {
    }
    const AdapterIdentity &getAdapterIdentity() const noexcept override
    {
        return m_identity;
    }
    FRESULT importBuffer(const ExternalMemoryDesc &external, const BufferDesc &desc, IBuffer **output) noexcept override
    {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        if (!desc.byteSize || external.offset > external.allocationSize ||
            desc.byteSize > external.allocationSize - external.offset)
            return FE_INVALID_ARGS;
        if (desc.isVirtual || desc.cpuAccess != CpuAccessMode::None)
            return FE_UNSUPPORTED;
        try
        {
            Scope scope(m_context->handle);
            std::shared_ptr<ImportedMemory> allocation;
            auto status = importMemory(external, allocation);
            if (NVRHI_FAILED(status))
                return status;
            CUDA_EXTERNAL_MEMORY_BUFFER_DESC mapping{};
            mapping.offset = external.offset;
            mapping.size = desc.byteSize;
            auto code = cuExternalMemoryGetMappedBuffer(&allocation->buffer, allocation->handle, &mapping);
            if (code != CUDA_SUCCESS)
                return result(code);
            BufferDesc copy = desc;
            copy.sharedResourceFlags = SharedResourceFlags::None;
            auto buffer = MAKE_RC_OBJ_PTR(Buffer, m_context, copy);
            buffer->memory = std::make_shared<Allocation>();
            buffer->memory->context = m_context;
            buffer->memory->ptr = allocation->buffer;
            buffer->memory->owned = false;
            buffer->memory->externalOwner = allocation;
            *output = buffer.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc &)
        {
            return FE_OUT_OF_MEMORY;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }
    FRESULT importTexture(const ExternalMemoryDesc &external, const TextureDesc &desc,
                          ITexture **output) noexcept override
    {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        if (!desc.width || !desc.height || !desc.depth || !desc.arraySize || !desc.mipLevels)
            return FE_INVALID_ARGS;
        if (desc.sampleCount != 1 || desc.isVirtual)
            return FE_UNSUPPORTED;
        try
        {
            Scope scope(m_context->handle);
            std::shared_ptr<ImportedMemory> allocation;
            auto status = importMemory(external, allocation);
            if (NVRHI_FAILED(status))
                return status;
            CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mapping{};
            mapping.offset = external.offset;
            mapping.numLevels = desc.mipLevels;
            auto &a = mapping.arrayDesc;
            if (!arrayFormat(desc.format, a.Format, a.NumChannels))
                return FE_UNSUPPORTED;
            a.Width = desc.width;
            a.Height = desc.height;
            a.Depth = 0;
            switch (desc.dimension)
            {
            case TextureDimension::Texture1D:
                a.Height = 0;
                break;
            case TextureDimension::Texture2D:
                break;
            case TextureDimension::Texture3D:
                a.Depth = desc.depth;
                break;
            case TextureDimension::Texture1DArray:
                a.Height = 0;
                a.Depth = desc.arraySize;
                a.Flags |= CUDA_ARRAY3D_LAYERED;
                break;
            case TextureDimension::Texture2DArray:
                a.Depth = desc.arraySize;
                a.Flags |= CUDA_ARRAY3D_LAYERED;
                break;
            case TextureDimension::TextureCube:
            case TextureDimension::TextureCubeArray:
                a.Depth = desc.arraySize;
                a.Flags |= CUDA_ARRAY3D_CUBEMAP;
                if (a.Depth > 6)
                    a.Flags |= CUDA_ARRAY3D_LAYERED;
                break;
            default:
                return FE_UNSUPPORTED;
            }
            if (desc.isUAV)
                a.Flags |= CUDA_ARRAY3D_SURFACE_LDST;
            if (desc.isRenderTarget)
                a.Flags |= CUDA_ARRAY3D_COLOR_ATTACHMENT;
            auto code = cuExternalMemoryGetMappedMipmappedArray(&allocation->array, allocation->handle, &mapping);
            if (code != CUDA_SUCCESS)
                return result(code);
            TextureDesc copy = desc;
            copy.sharedResourceFlags = SharedResourceFlags::None;
            auto texture = MAKE_RC_OBJ_PTR(Texture, m_context, copy);
            texture->mipArray = allocation->array;
            texture->owned = false;
            texture->externalOwner = allocation;
            *output = texture.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc &)
        {
            return FE_OUT_OF_MEMORY;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }
    FRESULT importFence(const ExternalFenceDesc &desc, IExternalFence **output) noexcept override
    {
        if (!output)
            return FE_INVALID_ARGS;
        *output = nullptr;
        if (!sameAdapter(m_identity, desc.adapter))
            return FE_UNSUPPORTED;
        try
        {
            CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC native{};
            bool fd = false, kmt = false;
            if (!fenceType(desc.handleType, native.type, fd, kmt))
                return FE_UNSUPPORTED;
            if (kmt && !desc.sourceOwner)
                return FE_INVALID_ARGS;
            DuplicatedHandle handle;
            auto status = handle.duplicate(desc.handle, fd, kmt);
            if (NVRHI_FAILED(status))
                return status;
            if (fd)
                native.handle.fd = int(handle.value);
            else
                native.handle.win32.handle = reinterpret_cast<void *>(handle.value);
            Scope scope(m_context->handle);
            auto fence = MAKE_RC_OBJ_PTR(ImportedFence, m_context, desc);
            auto code = cuImportExternalSemaphore(&fence->semaphore, &native);
            if (code != CUDA_SUCCESS)
                return result(code);
            handle.transferred();
            *output = fence.Detach();
            return FS_OK;
        }
        catch (const std::bad_alloc &)
        {
            return FE_OUT_OF_MEMORY;
        }
        catch (...)
        {
            return FE_GENERIC_ERROR;
        }
    }
    FRESULT waitFence(ICommandList *command, IExternalFence *external, uint64_t value,
                      uint32_t timeoutMs) noexcept override
    {
        auto commands = list(command);
        auto fence = backendObject<ImportedFence *>(external);
        if (!commands || !fence || fence->context != m_context || (fence->binary() && value))
            return FE_INVALID_ARGS;
        try
        {
            AutoPtr<ImportedFence> retained(fence);
            commands->record([retained, value, timeoutMs](Replay &replay) {
                CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS params{};
                if (retained->keyed())
                {
                    params.params.keyedMutex.key = value;
                    params.params.keyedMutex.timeoutMs = timeoutMs;
                }
                else if (!retained->binary())
                    params.params.fence.value = value;
                return replay.context->check(
                    cuWaitExternalSemaphoresAsync(&retained->semaphore, &params, 1, replay.stream),
                    "cuWaitExternalSemaphoresAsync");
            });
            return FS_OK;
        }
        catch (...)
        {
            return FE_OUT_OF_MEMORY;
        }
    }
    FRESULT signalFence(ICommandList *command, IExternalFence *external, uint64_t value) noexcept override
    {
        auto commands = list(command);
        auto fence = backendObject<ImportedFence *>(external);
        if (!commands || !fence || fence->context != m_context || (fence->binary() && value))
            return FE_INVALID_ARGS;
        try
        {
            AutoPtr<ImportedFence> retained(fence);
            commands->record([retained, value](Replay &replay) {
                CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS params{};
                if (retained->keyed())
                    params.params.keyedMutex.key = value;
                else if (!retained->binary())
                    params.params.fence.value = value;
                return replay.context->check(
                    cuSignalExternalSemaphoresAsync(&retained->semaphore, &params, 1, replay.stream),
                    "cuSignalExternalSemaphoresAsync");
            });
            return FS_OK;
        }
        catch (...)
        {
            return FE_OUT_OF_MEMORY;
        }
    }
    FRESULT queryPeerCapabilities(IDevice *peer, PeerCapabilities &output) const noexcept override
    {
        output = {};
        auto device = backendObject<Device *>(peer);
        if (!device)
            return FE_UNSUPPORTED;
        output.sameDevice = device->context->device == m_context->device;
        if (output.sameDevice)
        {
            output.nativeCopy = true;
            return FS_OK;
        }
        int access = 0;
        auto code = cuDeviceCanAccessPeer(&access, m_context->device, device->context->device);
        if (code != CUDA_SUCCESS)
            return result(code);
        output.nativeCopy = access != 0;
        int atomics = 0;
        if (access && cuDeviceGetP2PAttribute(&atomics, CU_DEVICE_P2P_ATTRIBUTE_NATIVE_ATOMIC_SUPPORTED,
                                              m_context->device, device->context->device) == CUDA_SUCCESS)
            output.nativeAtomics = atomics != 0;
        return FS_OK;
    }
    FRESULT copyPeerBuffer(ICommandList *command, IBuffer *destination, uint64_t destinationOffset, IBuffer *source,
                           uint64_t sourceOffset, uint64_t bytes, CommandQueue sourceQueue) noexcept override
    {
        auto commands = list(command);
        auto dst = backendObject<Buffer *>(destination);
        auto src = backendObject<Buffer *>(source);
        if (!commands || !dst || !src || dst->context != m_context || unsigned(sourceQueue) >= 3 || !bytes ||
            destinationOffset > dst->desc.byteSize || bytes > dst->desc.byteSize - destinationOffset ||
            sourceOffset > src->desc.byteSize || bytes > src->desc.byteSize - sourceOffset)
            return FE_INVALID_ARGS;
        try
        {
            if (src->context->handle != m_context->handle)
            {
                int access = 0;
                auto code = cuDeviceCanAccessPeer(&access, m_context->device, src->context->device);
                if (code != CUDA_SUCCESS)
                    return result(code);
                if (!access)
                    return FE_UNSUPPORTED;
                Scope scope(m_context->handle);
                code = cuCtxEnablePeerAccess(src->context->handle, 0);
                if (code != CUDA_SUCCESS && code != CUDA_ERROR_PEER_ACCESS_ALREADY_ENABLED)
                    return result(code);
            }
            AutoPtr<Buffer> d(dst), s(src);
            commands->record([d, s, destinationOffset, sourceOffset, bytes, sourceQueue](Replay &replay) {
                CUevent ready = nullptr;
                {
                    Scope sourceScope(s->context->handle);
                    if (!replay.context->check(cuEventCreate(&ready, CU_EVENT_DISABLE_TIMING), "cuEventCreate"))
                        return false;
                    if (!replay.context->check(cuEventRecord(ready, s->context->streams[unsigned(sourceQueue)]),
                                               "cuEventRecord"))
                    {
                        cuEventDestroy(ready);
                        return false;
                    }
                }
                const auto wait = cuStreamWaitEvent(replay.stream, ready, 0);
                {
                    Scope sourceScope(s->context->handle);
                    cuEventDestroy(ready);
                }
                if (!replay.context->check(wait, "cuStreamWaitEvent"))
                    return false;
                return replay.context->check(cuMemcpyPeerAsync(d->memory->ptr + destinationOffset, d->context->handle,
                                                               s->memory->ptr + sourceOffset, s->context->handle,
                                                               size_t(bytes), replay.stream),
                                             "cuMemcpyPeerAsync");
            });
            return FS_OK;
        }
        catch (...)
        {
            return FE_OUT_OF_MEMORY;
        }
    }
};
} // namespace
FRESULT nvrhiCudaCreateInteropDevice(IDevice *device, IInteropDevice **output) noexcept
{
    if (!output)
        return FE_INVALID_ARGS;
    *output = nullptr;
    auto backend = backendObject<Device *>(device);
    if (!backend)
        return FE_UNSUPPORTED;
    try
    {
        *output = MAKE_RC_OBJ(InteropDevice, backend);
        return FS_OK;
    }
    catch (const std::bad_alloc &)
    {
        return FE_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return FE_GENERIC_ERROR;
    }
}
} // namespace nvrhi::cuda
