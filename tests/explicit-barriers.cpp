// Explicit barriers, timeline fences and the resource base (ADR 0015), per backend:
// - feature queries and IResource::queryHeapBinding;
// - ICommandList::barriers with automatic barriers off: discard, split pairs, combined read states, plane masks,
//   aliasing and UAV barriers, checked by reading the results back and by the API's debug layer;
// - the validation layer's rules for barriers and explicit-barrier scopes (each expected to report one error);
// - IFence and IDevice::submit: the CPU wait table (poll, finite, infinite), CPU and GPU signals, waitFences, and
//   ordering across queues with queue-ownership transfers (Vulkan) and CUDA streams.
//
// The D3D12 debug layer and GPU-based validation, and the Vulkan validation layer with synchronization validation,
// are enabled; the test confirms they are loaded before it trusts a clean result. With NVRHI_WITH_RTXMU the D3D12
// device uses legacy barriers only.
//
// usage: nvrhi_explicit_barrier_tests d3d11 | d3d12 | vulkan | cuda

#include <nvrhi/nvrhi.h>
#include <nvrhi/factory.h>
#include <nvrhi/core/foundation.h>
#if TEST_D3D11
#include <nvrhi/d3d11.h>
#endif
#if TEST_D3D12
#include <nvrhi/d3d12.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#endif
#if TEST_VULKAN
#include <nvrhi/vulkan.h>
#endif
#if TEST_CUDA
#include <nvrhi/cuda.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace nvrhi;

namespace
{
    void check(bool condition, const std::string& what)
    {
        if (!condition)
            throw std::runtime_error("check failed: " + what);
        std::printf("ok: %s\n", what.c_str());
    }

    struct Messages : ObjectImpl<IMessageCallback>
    {
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IMessageCallback)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_END_INTERFACE_TABLE()

        unsigned errors = 0;
        unsigned expectedErrorsSeen = 0;
        bool expecting = false;

        void message(MessageSeverity severity, const char* text) noexcept override
        {
            const bool error = severity == MessageSeverity::Error || severity == MessageSeverity::Fatal;
            if (error && expecting)
            {
                ++expectedErrorsSeen;
                std::printf("  (expected) NVRHI: %s\n", text);
                return;
            }
            if (error)
                ++errors;
            if (severity != MessageSeverity::Info)
                std::printf("NVRHI: %s\n", text);
        }
    };

    // The backend under test: the device, its validation layer, and the native debug-layer error count.
    struct Env
    {
        GraphicsAPI api = GraphicsAPI::D3D12;
        IDevice* device = nullptr;          // the validation layer
        Messages* messages = nullptr;
        bool computeQueue = false;
        bool heaps = false;                 // virtual resources
        bool nativeBarriers = false;        // D3D12 and Vulkan record real barriers
        std::function<unsigned()> nativeErrors = [] { return 0u; };
    };

    // Runs 'body', which must make the validation layer report at least one error.
    void expectError(Env& env, const char* what, const std::function<void()>& body)
    {
        env.messages->expecting = true;
        env.messages->expectedErrorsSeen = 0;
        body();
        env.messages->expecting = false;
        check(env.messages->expectedErrorsSeen > 0, std::string("validation reports: ") + what);
    }

    void checkClean(Env& env, const char* phase)
    {
        const unsigned native = env.nativeErrors();
        check(env.messages->errors == 0 && native == 0, std::string(phase) + ": no NVRHI or debug-layer errors");
    }

    CommandListHandle createCommandList(IDevice* device, CommandQueue queue)
    {
        // D3D11 has only the immediate context; the other backends record deferred lists.
        const bool immediate = device->getGraphicsAPI() == GraphicsAPI::D3D11;
        CommandListHandle commandList;
        device->createCommandList(CommandListParameters().setEnableImmediateExecution(immediate).setQueueType(queue), &commandList);
        check(commandList != nullptr, "command list");
        return commandList;
    }

    void execute(IDevice* device, ICommandList* commandList, CommandQueue queue = CommandQueue::Graphics)
    {
        ICommandList* lists[] = { commandList };
        device->submit(SubmitDesc().setCommandLists(lists, 1).setQueue(queue));
        device->waitForIdle();
        device->runGarbageCollection();
    }

    // The state a clear needs with automatic barriers off (design 6.9).
    ResourceStates colorClearState(GraphicsAPI api) { return api == GraphicsAPI::VULKAN ? ResourceStates::CopyDest : ResourceStates::RenderTarget; }
    ResourceStates depthClearState(GraphicsAPI api) { return api == GraphicsAPI::VULKAN ? ResourceStates::CopyDest : ResourceStates::DepthWrite; }

    // Reads texel (0, 0) of mip 0 of 'slice' of an RGBA8 texture that is in the CopySource state.
    uint32_t readTexel(Env& env, ITexture* texture, ArraySlice slice)
    {
        TextureDesc stagingDesc = texture->getDesc();
        stagingDesc.mipLevels = 1;
        stagingDesc.arraySize = 1;
        stagingDesc.dimension = TextureDimension::Texture2D;
        stagingDesc.isRenderTarget = false;
        stagingDesc.isVirtual = false;
        stagingDesc.debugName = "readback";
        StagingTextureHandle staging;
        env.device->createStagingTexture(stagingDesc, CpuAccessMode::Read, &staging);
        check(staging != nullptr, "staging texture");

        CommandListHandle commandList = createCommandList(env.device, CommandQueue::Graphics);
        commandList->open();
        commandList->beginTrackingTextureState(texture, TextureSubresourceSet(0, 1, slice, 1), ResourceStates::CopySource);
        commandList->copyTexture2(staging, TextureSlice(), texture, TextureSlice().setArraySlice(slice));
        commandList->close();
        execute(env.device, commandList);

        size_t rowPitch = 0;
        const void* data = env.device->mapStagingTexture(staging, TextureSlice(), CpuAccessMode::Read, rowPitch);
        check(data != nullptr, "map staging texture");
        uint32_t texel = 0;
        std::memcpy(&texel, data, sizeof(texel));
        env.device->unmapStagingTexture(staging);
        return texel;
    }

    TextureDesc colorDesc(const char* name, bool isVirtual = false)
    {
        return TextureDesc()
            .setWidth(64).setHeight(64)
            .setMipLevels(2).setArraySize(2)
            .setDimension(TextureDimension::Texture2DArray)
            .setFormat(Format::RGBA8_UNORM)
            .setIsRenderTarget(true)
            .setIsVirtual(isVirtual)
            .setInitialState(ResourceStates::Common)
            .setDebugName(name);
    }

    void testFeatures(Env& env)
    {
        const bool split = env.device->queryFeatureSupport(Feature::SplitBarriers);
        const bool aliasing = env.device->queryFeatureSupport(Feature::AliasingBarriers);
        const bool ownership = env.device->queryFeatureSupport(Feature::QueueOwnershipTransfer);
        std::printf("  features: split %d, aliasing %d, ownership transfer %d\n", split, aliasing, ownership);
        check(split == env.nativeBarriers && aliasing == env.nativeBarriers, "split and aliasing barriers where the API has them");
        check(ownership == (env.api == GraphicsAPI::VULKAN), "queue-ownership transfers on Vulkan only");
    }

    void testHeapBinding(Env& env)
    {
        BufferHandle committed;
        env.device->createBuffer(BufferDesc().setByteSize(4096).setDebugName("committed"), &committed);
        check(committed != nullptr, "committed buffer");
        HeapBinding binding;
        check(committed->queryHeapBinding(binding) == FE_NOT_FOUND, "a committed buffer has no heap binding");

        if (!env.heaps)
            return;

        BufferHandle placed;
        env.device->createBuffer(BufferDesc().setByteSize(65536).setIsVirtual(true).setDebugName("placed"), &placed);
        check(placed->queryHeapBinding(binding) == FE_NOT_FOUND, "an unbound virtual buffer has no heap binding");

        MemoryRequirements requirements;
        env.device->getBufferMemoryRequirements(requirements, placed);
        HeapHandle heap;
        env.device->createHeap(HeapDesc().setCapacity(requirements.size + 65536 * 2).setType(HeapType::DeviceLocal).setDebugName("binding heap"), &heap);
        check(heap != nullptr, "heap");
        const uint64_t offset = 65536;
        check(env.device->bindBufferMemory(placed, heap, offset), "bindBufferMemory");
        check(placed->queryHeapBinding(binding) == FS_OK, "queryHeapBinding after binding");
        check(binding.heap == heap.Get() && binding.offset == offset && binding.size >= 65536, "heap binding: heap, offset and size");
        checkClean(env, "heap binding");
    }

    // Discard, clear, split pair, combined read state, then a readback.
    void testColorBarriers(Env& env)
    {
        TextureHandle texture;
        env.device->createTexture(colorDesc("barrier color"), &texture);
        check(texture != nullptr, "color texture");
        BufferHandle scratch;
        env.device->createBuffer(BufferDesc().setByteSize(256).setDebugName("scratch").setCanHaveUAVs(true), &scratch);

        const ResourceStates clear = colorClearState(env.api);
        CommandListHandle commandList = createCommandList(env.device, CommandQueue::Graphics);
        commandList->open();
        commandList->setEnableAutomaticBarriers(false);

        const ResourceBarrier discard = ResourceBarrier::Transition(texture, ResourceStates::Unknown, clear, SubresourceRegion(), BarrierFlags::Discard);
        check(commandList->barriers(&discard, 1) == FS_OK, "discard transition");
        commandList->clearTextureFloat(texture, AllSubresources, Color(1.f, 0.f, 0.f, 1.f));

        // mip 0: begin a split transition to CopySource; mip 1: a combined read state.
        const ResourceBarrier begin[] = {
            ResourceBarrier::Transition(texture, clear, ResourceStates::CopySource, SubresourceRegion().setMips(0, 1), BarrierFlags::SplitBegin),
            ResourceBarrier::Transition(texture, clear, ResourceStates::ShaderResource | ResourceStates::CopySource, SubresourceRegion().setMips(1, 1)),
        };
        check(commandList->barriers(begin, 2) == FS_OK, "split begin and a combined read state");

        // Unrelated work between the halves.
        const ResourceBarrier scratchState = ResourceBarrier::Transition(scratch, ResourceStates::Unknown, ResourceStates::CopyDest);
        commandList->barriers(&scratchState, 1);
        const uint32_t pattern[4] = { 1, 2, 3, 4 };
        commandList->writeBuffer(scratch, pattern, sizeof(pattern));

        const ResourceBarrier end = ResourceBarrier::Transition(texture, clear, ResourceStates::CopySource, SubresourceRegion().setMips(0, 1), BarrierFlags::SplitEnd);
        check(commandList->barriers(&end, 1) == FS_OK, "split end");
        const ResourceBarrier uavs[] = { ResourceBarrier::Uav(scratch), ResourceBarrier::Uav(nullptr) };
        check(commandList->barriers(uavs, 2) == FS_OK, "UAV barriers");
        commandList->close();
        execute(env.device, commandList);

        check(readTexel(env, texture, 1) == 0xff0000ff, "the cleared colour reads back after the split pair");
        checkClean(env, "color barriers");
    }

    // Plane masks on a depth-stencil texture: depth to a combined read state, stencil to DepthWrite, separately.
    void testPlaneBarriers(Env& env)
    {
        TextureHandle depth;
        env.device->createTexture(TextureDesc()
            .setWidth(64).setHeight(64)
            .setFormat(Format::D24S8)
            .setIsRenderTarget(true)
            .setInitialState(ResourceStates::Common)
            .setDebugName("barrier depth"), &depth);
        check(depth != nullptr, "depth-stencil texture");

        const ResourceStates clear = depthClearState(env.api);
        CommandListHandle commandList = createCommandList(env.device, CommandQueue::Graphics);
        commandList->open();
        commandList->setEnableAutomaticBarriers(false);

        const ResourceBarrier discard = ResourceBarrier::Transition(depth, ResourceStates::Unknown, clear, SubresourceRegion(), BarrierFlags::Discard);
        commandList->barriers(&discard, 1);
        commandList->clearDepthStencilTexture(depth, AllSubresources, true, 1.f, true, 0);

        const ResourceBarrier planes[] = {
            ResourceBarrier::Transition(depth, clear, ResourceStates::DepthRead | ResourceStates::ShaderResource, SubresourceRegion().setPlanes(PlaneMask::Depth)),
            ResourceBarrier::Transition(depth, clear, ResourceStates::DepthWrite, SubresourceRegion().setPlanes(PlaneMask::Stencil)),
        };
        check(commandList->barriers(planes, 2) == FS_OK, "depth and stencil planes transitioned separately");
        const ResourceBarrier back = ResourceBarrier::Transition(depth, ResourceStates::DepthRead | ResourceStates::ShaderResource,
            ResourceStates::DepthWrite, SubresourceRegion().setPlanes(PlaneMask::Depth));
        check(commandList->barriers(&back, 1) == FS_OK, "depth plane back to DepthWrite");
        commandList->close();
        execute(env.device, commandList);
        checkClean(env, "plane barriers");
    }

    // Two placed textures over the same memory: the outgoing one goes to no access, an aliasing barrier, the
    // incoming one starts with a discard.
    void testAliasing(Env& env)
    {
        TextureHandle a, b, c;
        env.device->createTexture(colorDesc("alias a", true), &a);
        env.device->createTexture(colorDesc("alias b", true), &b);
        env.device->createTexture(colorDesc("alias c", true), &c);
        MemoryRequirements requirements;
        env.device->getTextureMemoryRequirements(requirements, a);
        const uint64_t alignment = requirements.alignment ? requirements.alignment : 65536;
        const uint64_t secondOffset = (requirements.size + alignment - 1) / alignment * alignment;

        HeapHandle heap;
        env.device->createHeap(HeapDesc().setCapacity(secondOffset * 2).setType(HeapType::DeviceLocal).setDebugName("alias heap"), &heap);
        check(heap != nullptr, "alias heap");
        check(env.device->bindTextureMemory(a, heap, 0) && env.device->bindTextureMemory(b, heap, 0), "a and b bound at offset 0");
        check(env.device->bindTextureMemory(c, heap, secondOffset), "c bound after them");

        const ResourceStates clear = colorClearState(env.api);
        CommandListHandle commandList = createCommandList(env.device, CommandQueue::Graphics);
        commandList->open();
        commandList->setEnableAutomaticBarriers(false);

        const ResourceBarrier startA = ResourceBarrier::Transition(a, ResourceStates::Unknown, clear, SubresourceRegion(), BarrierFlags::Discard);
        commandList->barriers(&startA, 1);
        commandList->clearTextureFloat(a, AllSubresources, Color(0.f, 0.f, 1.f, 1.f));

        expectError(env, "an aliasing barrier between resources that do not overlap", [&]
        {
            const ResourceBarrier wrong = ResourceBarrier::Aliasing(a, c);
            check(commandList->barriers(&wrong, 1) == FE_INVALID_ARGS, "non-overlapping aliasing barrier refused");
        });

        const ResourceBarrier swap[] = {
            ResourceBarrier::Transition(a, clear, ResourceStates::Unknown),
            ResourceBarrier::Aliasing(a, b),
            ResourceBarrier::Transition(b, ResourceStates::Unknown, clear, SubresourceRegion(), BarrierFlags::Discard),
        };
        check(commandList->barriers(swap, 3) == FS_OK, "outgoing to no access, aliasing barrier, incoming discard");
        commandList->clearTextureFloat(b, AllSubresources, Color(0.f, 1.f, 0.f, 1.f));
        const ResourceBarrier read = ResourceBarrier::Transition(b, clear, ResourceStates::CopySource);
        commandList->barriers(&read, 1);
        commandList->close();
        execute(env.device, commandList);

        check(readTexel(env, b, 0) == 0xff00ff00, "the incoming alias reads back its own clear");
        checkClean(env, "aliasing");
    }

    // The validation layer's rules. None of these barriers reaches the backend.
    void testValidationRules(Env& env)
    {
        // A texture, except on CUDA, which has no render targets: a buffer there.
        TextureHandle texture, hostKeptTexture;
        BufferHandle buffer, hostKeptBuffer;
        IResource* resource = nullptr;
        IResource* hostKept = nullptr;
        if (env.api == GraphicsAPI::CUDA)
        {
            env.device->createBuffer(BufferDesc().setByteSize(256).setDebugName("validation rules"), &buffer);
            env.device->createBuffer(BufferDesc().setByteSize(256).setDebugName("host kept")
                .setKeepInitialState(true).setInitialState(ResourceStates::ShaderResource), &hostKeptBuffer);
            resource = buffer;
            hostKept = hostKeptBuffer;
        }
        else
        {
            env.device->createTexture(colorDesc("validation rules"), &texture);
            env.device->createTexture(colorDesc("host kept").setKeepInitialState(true).setInitialState(ResourceStates::ShaderResource), &hostKeptTexture);
            resource = texture;
            hostKept = hostKeptTexture;
        }
        check(resource != nullptr && hostKept != nullptr, "validation-rule resources");

        CommandListHandle commandList = createCommandList(env.device, CommandQueue::Graphics);
        commandList->open();
        commandList->setEnableAutomaticBarriers(false);

        // The resource really is in CopySource before the tracker is told so.
        const ResourceBarrier initial = ResourceBarrier::Transition(resource, ResourceStates::Unknown, ResourceStates::CopySource);
        check(commandList->barriers(&initial, 1) == FS_OK, "initial transition of an untracked resource");

        expectError(env, "a state call inside an explicit-barrier scope", [&]
        {
            commandList->setExplicitBarrierScope("test pass");
            commandList->commitBarriers();
            commandList->setExplicitBarrierScope(nullptr);
        });

        // D3D11 and CUDA have no tracker to disagree with.
        if (env.nativeBarriers)
        {
            commandList->beginTrackingTextureState(texture, AllSubresources, ResourceStates::CopySource);
            expectError(env, "a 'before' state that disagrees with the tracker", [&]
            {
                const ResourceBarrier wrong = ResourceBarrier::Transition(texture, ResourceStates::RenderTarget, ResourceStates::CopyDest);
                check(commandList->barriers(&wrong, 1) == FE_INVALID_ARGS, "mismatched barrier refused");
            });
        }

        // No D3D12 layout serves UnorderedAccess together with ShaderResource on a texture (design 6.3).
        if (env.api == GraphicsAPI::D3D12)
        {
            expectError(env, "a texture state with UnorderedAccess and ShaderResource", [&]
            {
                const ResourceBarrier wrong = ResourceBarrier::Transition(texture, ResourceStates::CopySource,
                    ResourceStates::UnorderedAccess | ResourceStates::ShaderResource);
                check(commandList->barriers(&wrong, 1) == FE_INVALID_ARGS, "unrepresentable texture state refused");
            });
        }

        expectError(env, "a barrier on a keepInitialState texture", [&]
        {
            const ResourceBarrier wrong = ResourceBarrier::Transition(hostKept, ResourceStates::ShaderResource, ResourceStates::CopySource);
            check(commandList->barriers(&wrong, 1) == FE_INVALID_ARGS, "keepInitialState barrier refused");
        });

        expectError(env, "a SplitEnd without its SplitBegin", [&]
        {
            const ResourceBarrier wrong = ResourceBarrier::Transition(resource, ResourceStates::CopySource, ResourceStates::CopyDest,
                SubresourceRegion(), BarrierFlags::SplitEnd);
            check(commandList->barriers(&wrong, 1) == FE_INVALID_ARGS, "unmatched SplitEnd refused");
        });

        expectError(env, "a malformed barrier", [&]
        {
            const ResourceBarrier wrong = ResourceBarrier::Transition(nullptr, ResourceStates::Common, ResourceStates::CopyDest);
            check(commandList->barriers(&wrong, 1) == FE_INVALID_ARGS, "transition without a resource refused");
        });

        // A barrier the command list tracks moves the tracker.
        const ResourceBarrier tracked = ResourceBarrier::Transition(texture, ResourceStates::CopySource, ResourceStates::CopyDest);
        if (env.nativeBarriers)
        {
            check(commandList->barriers(&tracked, 1) == FS_OK, "barrier on a tracked texture");
            check(commandList->getTextureSubresourceState(texture, 0, 0) == ResourceStates::CopyDest, "the tracker follows the barrier");
        }
        commandList->close();

        // Queue-ownership matching and open splits, where the barriers have no native effect.
        if (!env.nativeBarriers)
        {
            CommandListHandle compute = createCommandList(env.device, env.computeQueue ? CommandQueue::Compute : CommandQueue::Graphics);
            compute->open();
            const ResourceBarrier acquire = ResourceBarrier::Transition(resource, ResourceStates::CopySource, ResourceStates::CopySource)
                .setQueueTransfer(BarrierFlags::QueueAcquire, CommandQueue::Copy);
            compute->barriers(&acquire, 1);
            const ResourceBarrier begin = ResourceBarrier::Transition(resource, ResourceStates::CopySource, ResourceStates::CopyDest,
                SubresourceRegion(), BarrierFlags::SplitBegin);
            compute->barriers(&begin, 1);
            expectError(env, "a split barrier begun and never ended", [&] { compute->close(); });
            expectError(env, "a QueueAcquire without a QueueRelease", [&]
            {
                ICommandList* lists[] = { compute };
                env.device->submit(SubmitDesc().setCommandLists(lists, 1).setQueue(compute->getDesc().queueType));
            });
            env.device->waitForIdle();
        }
        else
        {
            execute(env.device, commandList);
        }

        checkClean(env, "validation rules");
    }

    // The CPU wait table of design 8.3.1, CPU and GPU signals, waitFences.
    void testFences(Env& env)
    {
        FenceHandle fence, other;
        if (env.device->createFence(FenceDesc().setInitialValue(5).setDebugName("test fence"), &fence) == FE_UNSUPPORTED)
        {
            std::printf("SKIP fences: not supported on this device\n");
            return;
        }
        env.device->createFence(FenceDesc().setDebugName("other fence"), &other);
        check(fence != nullptr && other != nullptr, "createFence");
        check(fence->getCompletedValue() == 5, "initial value");
        check(fence->wait(5, 0) == FS_OK, "wait for a reached value returns at once");
        check(fence->wait(6, 0) == FE_WAIT_TIMEOUT, "zero timeout polls");

        const auto start = std::chrono::steady_clock::now();
        check(fence->wait(6, 20'000'000) == FE_WAIT_TIMEOUT, "finite timeout");
        const double waitedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("  finite wait of 20 ms took %.1f ms\n", waitedMs);
        check(waitedMs >= 15.0 && waitedMs < 2000.0, "the finite wait waited about its timeout");

        // D3D11 has no CPU signal: the immediate context signals and flushes, so the value arrives shortly.
        check(fence->signal(7) == FS_OK && fence->wait(7, 1'000'000'000) == FS_OK, "CPU signal");
        expectError(env, "a CPU signal that does not increase the value", [&]
        {
            check(fence->signal(7) == FE_INVALID_ARGS, "non-increasing CPU signal refused");
        });

        check(env.device->queueSignalFence(CommandQueue::Graphics, fence, 9) == FS_OK, "queueSignalFence");
        check(fence->wait(9) == FS_OK, "infinite wait for a GPU signal");

        env.device->submit(SubmitDesc().setQueue(CommandQueue::Graphics).setSignals(std::vector<FenceValue>{ { fence, 11 } }.data(), 1));
        const FenceValue both[] = { { fence, 11 }, { other, 1 } };
        check(env.device->waitFences(both, 2, false, c_InfiniteTimeout) == FS_OK, "waitFences: any");
        check(env.device->waitFences(both, 2, true, 5'000'000) == FE_WAIT_TIMEOUT, "waitFences: all, with one never signaled");
        check(other->signal(1) == FS_OK && env.device->waitFences(both, 2, true, 0) == FS_OK, "waitFences: all, after the CPU signal");

        // A GPU wait for a value the queue signals itself, in one submission order.
        check(env.device->queueSignalFence(CommandQueue::Graphics, other, 2) == FS_OK, "GPU signal");
        check(env.device->queueWaitFence(CommandQueue::Graphics, other, 2) == FS_OK, "queueWaitFence");
        env.device->waitForIdle();
        checkClean(env, "fences");
    }

    // Graphics writes a buffer and signals; the compute queue waits and copies it to a readback buffer. On Vulkan the
    // buffer moves between the queue families with a release and an acquire.
    void testCrossQueue(Env& env)
    {
        FenceHandle fence;
        if (env.device->createFence(FenceDesc().setDebugName("cross-queue fence"), &fence) != FS_OK)
            return;

        const CommandQueue consumerQueue = env.computeQueue ? CommandQueue::Compute : CommandQueue::Graphics;
        const bool transfer = env.computeQueue;

        BufferHandle data, readback;
        env.device->createBuffer(BufferDesc().setByteSize(256).setDebugName("cross-queue data"), &data);
        env.device->createBuffer(BufferDesc().setByteSize(256).setCpuAccess(CpuAccessMode::Read)
            .setInitialState(ResourceStates::CopyDest).setKeepInitialState(true).setDebugName("cross-queue readback"), &readback);
        check(data != nullptr && readback != nullptr, "buffers");

        if (env.api == GraphicsAPI::CUDA)
        {
            // CUDA: a GPU wait submitted before its signal is skipped with an error.
            expectError(env, "a CUDA GPU wait submitted before its signal", [&]
            {
                const FenceValue early{ fence, 100 };
                env.device->submit(SubmitDesc().setQueue(consumerQueue).setWaits(&early, 1));
            });
        }

        uint32_t pattern[64];
        for (uint32_t i = 0; i < 64; ++i)
            pattern[i] = 0x1000 + i;

        CommandListHandle producer = createCommandList(env.device, CommandQueue::Graphics);
        producer->open();
        producer->setEnableAutomaticBarriers(false);
        const ResourceBarrier toCopyDest = ResourceBarrier::Transition(data, ResourceStates::Unknown, ResourceStates::CopyDest);
        producer->barriers(&toCopyDest, 1);
        producer->writeBuffer(data, pattern, sizeof(pattern));
        ResourceBarrier release = ResourceBarrier::Transition(data, ResourceStates::CopyDest, ResourceStates::CopySource);
        if (transfer)
            release.setQueueTransfer(BarrierFlags::QueueRelease, consumerQueue);
        producer->barriers(&release, 1);
        producer->close();

        CommandListHandle consumer = createCommandList(env.device, consumerQueue);
        consumer->open();
        consumer->setEnableAutomaticBarriers(false);
        if (transfer)
        {
            const ResourceBarrier acquire = ResourceBarrier::Transition(data, ResourceStates::CopyDest, ResourceStates::CopySource)
                .setQueueTransfer(BarrierFlags::QueueAcquire, CommandQueue::Graphics);
            consumer->barriers(&acquire, 1);
        }
        consumer->copyBuffer(readback, 0, data, 0, sizeof(pattern));
        consumer->close();

        ICommandList* producerLists[] = { producer };
        const FenceValue produced{ fence, 1 };
        env.device->submit(SubmitDesc().setCommandLists(producerLists, 1).setQueue(CommandQueue::Graphics).setSignals(&produced, 1));

        ICommandList* consumerLists[] = { consumer };
        const FenceValue consumed{ fence, 2 };
        env.device->submit(SubmitDesc().setCommandLists(consumerLists, 1).setQueue(consumerQueue)
            .setWaits(&produced, 1).setSignals(&consumed, 1));

        if (env.api == GraphicsAPI::CUDA)
            env.device->queueSignalFence(CommandQueue::Graphics, fence, 100);   // satisfies nothing; keeps the ring busy

        check(fence->wait(2) == FS_OK, "CPU wait for the consumer's signal");
        const uint32_t* result = static_cast<const uint32_t*>(env.device->mapBuffer(readback, CpuAccessMode::Read));
        check(result != nullptr, "map readback");
        bool equal = std::memcmp(result, pattern, sizeof(pattern)) == 0;
        env.device->unmapBuffer(readback);
        check(equal, transfer ? "the consumer queue reads the producer's data after the fence and the ownership transfer"
            : "the consumer reads the producer's data after the fence");
        env.device->waitForIdle();
        env.device->runGarbageCollection();
        checkClean(env, "cross-queue");
    }

    // A deliberate write-after-write hazard, which synchronization validation must report: the clean runs above
    // then mean that it was watching.
    void testSyncValidationIsLoaded(Env& env)
    {
        BufferHandle buffer;
        env.device->createBuffer(BufferDesc().setByteSize(256).setDebugName("hazard"), &buffer);
        check(buffer != nullptr, "hazard buffer");
        CommandListHandle commandList = createCommandList(env.device, CommandQueue::Graphics);
        commandList->open();
        commandList->setEnableAutomaticBarriers(false);
        const ResourceBarrier state = ResourceBarrier::Transition(buffer, ResourceStates::Unknown, ResourceStates::CopyDest);
        commandList->barriers(&state, 1);
        const uint32_t pattern[4] = { 1, 2, 3, 4 };
        commandList->writeBuffer(buffer, pattern, sizeof(pattern));
        expectError(env, "a write-after-write hazard (synchronization validation)", [&]
        {
            commandList->writeBuffer(buffer, pattern, sizeof(pattern));
        });
        commandList->close();
        execute(env.device, commandList);
        checkClean(env, "sync validation");
    }

    void runAll(Env& env)
    {
        testFeatures(env);
        if (env.api == GraphicsAPI::VULKAN)
            testSyncValidationIsLoaded(env);
        testHeapBinding(env);
        if (env.api != GraphicsAPI::CUDA)
        {
            testColorBarriers(env);
            if (env.nativeBarriers)
            {
                testPlaneBarriers(env);
                testAliasing(env);
            }
        }
        testValidationRules(env);
        testFences(env);
        testCrossQueue(env);
    }

    template <typename TFactory>
    int32_t findNvidiaAdapter(TFactory* factory)
    {
        for (uint32_t index = 0; index < factory->getAdapterCount(); ++index)
        {
            AdapterInfo info;
            if (factory->getAdapterInfo(index, info) == FS_OK && std::string(info.name.c_str()).find("NVIDIA") != std::string::npos)
                return int32_t(index);
        }
        return -1;
    }
}

int main(int argc, char** argv)
{
    // Unbuffered, so that the output shows how far a run that dies got.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try
    {
        check(argc == 2, "specify d3d11, d3d12, vulkan or cuda");
        const std::string backend = argv[1];
        AutoPtr<Messages> messages = MAKE_RC_OBJ_PTR(Messages);
        Env env;
        env.messages = messages;

#if TEST_D3D11
        if (backend == "d3d11")
        {
            d3d11::GraphicsFactoryDesc factoryDesc;
            factoryDesc.messageCallback = messages;
            d3d11::GraphicsFactoryHandle factory = d3d11::createGraphicsFactory(factoryDesc);
            check(factory != nullptr, "d3d11 graphics factory");
            d3d11::DeviceCreationDesc deviceDesc;
            deviceDesc.messageCallback = messages;
            deviceDesc.adapterIndex = findNvidiaAdapter(factory.Get());
            DeviceHandle device, validation;
            check(factory->createDeviceOnAdapter(deviceDesc, &device) == FS_OK, "createDeviceOnAdapter");
            check(factory->createValidationLayer(device, &validation) == FS_OK, "validation layer");
            env.api = GraphicsAPI::D3D11;
            env.device = validation;
            runAll(env);
        }
        else
#endif
#if TEST_D3D12
        if (backend == "d3d12")
        {
            d3d12::GraphicsFactoryDesc factoryDesc;
            factoryDesc.messageCallback = messages;
            factoryDesc.enableDebugRuntime = true;
            factoryDesc.enableGPUValidation = true;
            // The Agility SDK in .\D3D12\, when its exact version is given: it brings enhanced barriers to Windows 10.
            if (const char* sdkVersion = std::getenv("NVRHI_TEST_AGILITY_SDK_VERSION"))
            {
                factoryDesc.sdkVersion = uint32_t(std::strtoul(sdkVersion, nullptr, 10));
                factoryDesc.sdkPath = ".\\D3D12\\";
                std::printf("  Agility SDK %u\n", factoryDesc.sdkVersion);
            }
            d3d12::GraphicsFactoryHandle factory = d3d12::createGraphicsFactory(factoryDesc);
            check(factory != nullptr, "d3d12 graphics factory");
            d3d12::DeviceCreationDesc deviceDesc;
            deviceDesc.errorCB = messages;
            deviceDesc.enableComputeQueue = true;
            deviceDesc.enableCopyQueue = true;
            deviceDesc.adapterIndex = findNvidiaAdapter(factory.Get());
            d3d12::DeviceHandle device;
            check(factory->createDeviceOnAdapter(deviceDesc, &device) == FS_OK, "createDeviceOnAdapter");
            DeviceHandle validation;
            check(factory->createValidationLayer(device, &validation) == FS_OK, "validation layer");

            // The debug layer is loaded when the device answers ID3D12InfoQueue.
            ID3D12Device* native = static_cast<ID3D12Device*>(device->getNativeObject(ObjectTypes::D3D12_Device));
            AutoPtr<ID3D12InfoQueue> infoQueue;
            check(native && SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&infoQueue))), "the D3D12 debug layer is loaded (ID3D12InfoQueue)");
            infoQueue->SetMuteDebugOutput(FALSE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            // Messages as they happen, where the runtime has ID3D12InfoQueue1 (the Agility SDK): a run that dies
            // still shows its last ones. They stay stored for the per-phase count as well.
            AutoPtr<ID3D12InfoQueue1> infoQueue1;
            DWORD callbackCookie = 0;
            if (SUCCEEDED(infoQueue->QueryInterface(IID_PPV_ARGS(&infoQueue1))))
            {
                infoQueue1->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                    LPCSTR description, void*)
                {
                    if (severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                        std::printf("  D3D12 (live, severity %d, id %d): %s\n", int(severity), int(id), description);
                }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &callbackCookie);
            }
            std::printf("  enhanced barriers: %d\n", device->queryFeatureSupport(Feature::EnhancedBarriers));

            env.api = GraphicsAPI::D3D12;
            env.device = validation;
            env.computeQueue = true;
            env.heaps = true;
            env.nativeBarriers = true;
            env.nativeErrors = [&]
            {
                unsigned errors = 0;
                const UINT64 count = infoQueue->GetNumStoredMessages();
                for (UINT64 index = 0; index < count; ++index)
                {
                    SIZE_T size = 0;
                    infoQueue->GetMessage(index, nullptr, &size);
                    std::vector<uint8_t> storage(size);
                    D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                    if (FAILED(infoQueue->GetMessage(index, message, &size)))
                        continue;
                    if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                    {
                        ++errors;
                        std::printf("D3D12: %s\n", message->pDescription);
                    }
                }
                infoQueue->ClearStoredMessages();
                return errors;
            };
            runAll(env);
        }
        else
#endif
#if TEST_VULKAN
        if (backend == "vulkan")
        {
            // Synchronization validation.
#ifdef _WIN32
            _putenv_s("VK_LAYER_VALIDATE_SYNC", "1");
#endif
            vulkan::GraphicsFactoryDesc factoryDesc;
            factoryDesc.messageCallback = messages;
            factoryDesc.enableDebugRuntime = true;
            factoryDesc.enableRayTracingValidation = true;   // creates the debug-utils messenger that reports errors as errors
            factoryDesc.headless = true;
            vulkan::GraphicsFactoryHandle factory = vulkan::createGraphicsFactory(factoryDesc);
            check(factory != nullptr, "vulkan graphics factory");

            bool layerEnabled = false;
            const uint32_t layerCount = factory->getEnabledLayers(nullptr, 0);
            std::vector<const char*> layers(layerCount);
            factory->getEnabledLayers(layers.data(), layerCount);
            for (const char* layer : layers)
                layerEnabled |= std::strcmp(layer, "VK_LAYER_KHRONOS_validation") == 0;
            check(layerEnabled, "the Vulkan validation layer is loaded");

            vulkan::DeviceCreationDesc deviceDesc;
            deviceDesc.errorCB = messages;
            deviceDesc.enableComputeQueue = true;
            deviceDesc.enableCopyQueue = true;
            deviceDesc.adapterIndex = findNvidiaAdapter(factory.Get());
            vulkan::DeviceHandle device;
            check(factory->createDeviceOnAdapter(deviceDesc, &device) == FS_OK, "createDeviceOnAdapter");
            DeviceHandle validation;
            check(factory->createValidationLayer(device, &validation) == FS_OK, "validation layer");

            env.api = GraphicsAPI::VULKAN;
            env.device = validation;
            env.computeQueue = true;
            env.heaps = true;
            env.nativeBarriers = true;
            runAll(env);
            validation = nullptr;
            device->waitForIdle();
        }
        else
#endif
#if TEST_CUDA
        if (backend == "cuda")
        {
            cuda::GraphicsFactoryDesc factoryDesc;
            factoryDesc.messageCallback = messages;
            cuda::GraphicsFactoryHandle factory = cuda::createGraphicsFactory(factoryDesc);
            if (!factory)
            {
                std::printf("SKIP: no CUDA driver\n");
                return 0;
            }
            cuda::DeviceDesc deviceDesc;
            deviceDesc.messageCallback = messages;
            cuda::DeviceHandle device;
            check(factory->createDevice(deviceDesc, &device) == FS_OK, "createDevice");
            DeviceHandle validation;
            check(factory->createValidationLayer(device, &validation) == FS_OK, "validation layer");
            env.api = GraphicsAPI::CUDA;
            env.device = validation;
            env.computeQueue = true;
            runAll(env);
        }
        else
#endif
        {
            check(false, "unknown or disabled backend");
        }

        check(messages->errors == 0, "no unexpected NVRHI errors");
        std::printf("PASS\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
