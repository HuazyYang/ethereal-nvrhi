// The D3D12 device's root signature cache when several threads create and release pipelines:
// - shared: in each round all threads create a pipeline with the same new binding layout at once; while the pipelines
//   are alive they must have one root signature (the first one cached wins, the others are spare). The comparison
//   cannot fail wrongly, but it is not always sensitive: the runtime may return one object for equal root signatures
//   (it does when they are created one after the other; when created concurrently they can differ);
// - churn: the threads create and release pipelines of a few layouts with nothing between them, so that lookups meet
//   root signatures whose last reference is being released: three layouts that the threads take in turn, then pairs of
//   threads that share one layout (where the last reference is dropped most often). Each pipeline must have a root
//   signature; a race also shows as a crash or a heap corruption.
// - failed build: layouts that overlap have no root signature; the pipeline fails with an error, nothing is cached.
// The cases are bounded in time as well as in rounds; a watchdog ends the process if one hangs.
//
// usage: nvrhi_root_signature_cache_tests

#include <nvrhi/nvrhi.h>
#include <nvrhi/factory.h>
#include <nvrhi/core/foundation.h>
#include <nvrhi/d3d12.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include "test-factory.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_DEBUG) && defined(_MSC_VER)
#include <crtdbg.h>
#endif

using namespace nvrhi;

namespace
{
    constexpr uint32_t c_Threads = 8;
    constexpr std::chrono::seconds c_CaseTime{ 4 };   // a case stops after this time, when it has run some rounds

    void check(bool condition, const std::string& what)
    {
        if (!condition)
            throw std::runtime_error("check failed: " + what);
    }

    struct Messages : ObjectImpl<IMessageCallback>
    {
        NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Messages)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IMessageCallback)
        NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
        NVRHI_END_INTERFACE_TABLE()

        std::atomic<unsigned> errors{ 0 };

        void message(MessageSeverity severity, const char* text) noexcept override
        {
            if (severity != MessageSeverity::Error && severity != MessageSeverity::Fatal)
                return;
            ++errors;
            std::printf("NVRHI: %s\n", text);
        }
    };

    BindingLayoutHandle createLayout(IDevice* device, uint32_t slot)
    {
        BindingLayoutDesc desc;
        desc.visibility = ShaderType::Compute;
        desc.bindings = { BindingLayoutItem::RawBuffer_SRV(slot) };
        BindingLayoutHandle layout;
        device->createBindingLayout(desc, &layout);
        check(layout != nullptr, "binding layout");
        return layout;
    }

    ComputePipelineHandle createPipeline(IDevice* device, IShader* shader, IBindingLayout* layout)
    {
        ComputePipelineHandle pipeline;
        device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout), &pipeline);
        return pipeline;
    }

    void* rootSignatureOf(IComputePipeline* pipeline)
    {
        if (!pipeline)
            return nullptr;
        return static_cast<ID3D12RootSignature*>(pipeline->getNativeObject(ObjectTypes::D3D12_RootSignature));
    }

    // Runs 'body(thread)' on c_Threads threads that start together.
    template <typename TBody>
    void runTogether(TBody&& body)
    {
        std::atomic<uint32_t> ready{ 0 };
        std::vector<std::thread> threads;
        for (uint32_t t = 0; t < c_Threads; ++t)
        {
            threads.emplace_back([&, t] {
                ready.fetch_add(1);
                while (ready.load() < c_Threads)
                    std::this_thread::yield();
                body(t);
            });
        }
        for (std::thread& thread : threads)
            thread.join();
    }

    void testShared(IDevice* device, IShader* shader)
    {
        constexpr uint32_t c_Rounds = 200;
        const auto end = std::chrono::steady_clock::now() + c_CaseTime;
        uint32_t differing = 0;
        uint32_t round = 0;
        for (; round < c_Rounds && (round < 10 || std::chrono::steady_clock::now() < end); ++round)
        {
            BindingLayoutHandle layout = createLayout(device, round % 8);
            std::vector<ComputePipelineHandle> pipelines(c_Threads);
            runTogether([&](uint32_t t) { pipelines[t] = createPipeline(device, shader, layout); });
            for (uint32_t t = 0; t < c_Threads; ++t)
            {
                check(rootSignatureOf(pipelines[t]) != nullptr, "pipeline and root signature");
                differing += rootSignatureOf(pipelines[t]) != rootSignatureOf(pipelines[0]);
            }
            // A later pipeline finds the cached one, also after the other pipelines are gone (a spare root signature must
            // not take the entry with it).
            differing += rootSignatureOf(createPipeline(device, shader, layout)) != rootSignatureOf(pipelines[0]);
            for (uint32_t t = 1; t < c_Threads; ++t)
                pipelines[t] = nullptr;
            differing += rootSignatureOf(createPipeline(device, shader, layout)) != rootSignatureOf(pipelines[0]);
        }
        check(differing == 0, "shared: " + std::to_string(differing) +
            " pipelines with another root signature than the cached one");
        std::printf("ok: shared, %u rounds of %u threads\n", round, c_Threads);
    }

    // 'pairs': two threads on each of c_Threads / 2 layouts, else the threads take three layouts in turn.
    void testChurn(IDevice* device, IShader* shader, bool pairs)
    {
        constexpr uint32_t c_Rounds = 10000;
        const uint32_t layoutCount = pairs ? c_Threads / 2 : 3;
        std::vector<BindingLayoutHandle> layouts;
        for (uint32_t k = 0; k < layoutCount; ++k)
            layouts.push_back(createLayout(device, k));
        const auto end = std::chrono::steady_clock::now() + c_CaseTime;
        std::atomic<uint32_t> failures{ 0 };
        std::atomic<uint32_t> created{ 0 };
        runTogether([&](uint32_t t) {
            for (uint32_t round = 0; round < c_Rounds && (round < 10 || std::chrono::steady_clock::now() < end); ++round)
            {
                const uint32_t k = pairs ? t % layoutCount : (round + t / 4) % layoutCount;
                ComputePipelineHandle pipeline = createPipeline(device, shader, layouts[k]);
                created.fetch_add(1);
                if (!rootSignatureOf(pipeline))
                    failures.fetch_add(1);
            }
        });
        const char* name = pairs ? "churn, pairs" : "churn";
        check(failures.load() == 0, std::string(name) + ": " + std::to_string(failures.load()) +
            " pipelines without a root signature");
        std::printf("ok: %s, %u pipelines by %u threads\n", name, created.load(), c_Threads);
    }

    void testFailedBuild(IDevice* device, IShader* shader, Messages* messages)
    {
        BindingLayoutHandle first = createLayout(device, 0), second = createLayout(device, 0);
        check(messages->errors.load() == 0, "no NVRHI errors before");
        for (uint32_t attempt = 0; attempt < 2; ++attempt)   // the second one must not find a cached null
        {
            ComputePipelineHandle pipeline;
            const FRESULT result = device->createComputePipeline(ComputePipelineDesc().setComputeShader(shader)
                .addBindingLayout(first).addBindingLayout(second), &pipeline);
            check(result != FS_OK && pipeline == nullptr, "a pipeline with overlapping layouts fails");
        }
        check(messages->errors.load() >= 2, "the failures are reported");
        messages->errors = 0;
        std::printf("ok: failed build (the errors above are expected)\n");
    }
}

int main()
{
#if defined(_DEBUG) && defined(_MSC_VER)
    // A heap assertion is text and an exit code here, not a dialog that waits and takes the focus.
    for (int type : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT })
    {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(25));
        std::printf("FAILED: timed out\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();

    try
    {
        AutoPtr<Messages> messages = MAKE_RC_OBJ_PTR(Messages);
        auto factory = nvrhi_test::graphicsFactory(d3d12::createGraphicsFactory);
        check(factory != nullptr, "d3d12 graphics factory");
        d3d12::DeviceCreationDesc desc;
        desc.featureLevel = D3D_FEATURE_LEVEL_12_0;
        desc.errorCB = messages;
        d3d12::DeviceHandle device;
        factory->createDeviceOnAdapter(desc, &device);
        check(device != nullptr, "d3d12 device");

        ID3DBlob* code = nullptr;
        const char source[] = "[numthreads(1,1,1)] void main() {}";
        check(SUCCEEDED(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code,
            nullptr)), "shader");
        ShaderHandle shader;
        device->createShader(ShaderDesc().setShaderType(ShaderType::Compute).setDebugName("empty"),
            code->GetBufferPointer(), code->GetBufferSize(), &shader);
        code->Release();
        check(shader != nullptr, "shader object");

        testShared(device, shader);
        testChurn(device, shader, false);
        testChurn(device, shader, true);
        testFailedBuild(device, shader, messages);
        check(messages->errors.load() == 0, "no NVRHI errors");
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        std::fflush(stdout);
        std::_Exit(1);
    }
    std::printf("passed\n");
    std::fflush(stdout);
    std::_Exit(0);   // not past the detached watchdog
}
