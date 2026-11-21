// Fixture of nvrhi_cuda_tests queue_kernels (ADR 0016): gOutput is bound through SLANG_globalParams. Two streams that
// shared one module would race on it; with a module per queue slot each launch writes its own buffer.

RWStructuredBuffer<uint> gOutput : register(u0);

// gOutput[i] = value, after 'iterations' steps of busy work, so that launches on two streams overlap.
[shader("compute")]
[numthreads(64, 1, 1)]
void fill(uint3 threadId : SV_DispatchThreadID, uniform uint value, uniform uint count, uniform uint iterations)
{
    const uint i = threadId.x;
    if (i >= count)
        return;
    uint x = value;
    for (uint k = 0; k < iterations; ++k)
        x = x * 1664525u + 1013904223u;
    // The loop's result decides the value only if it reaches 0, which the test computes as well.
    gOutput[i] = x == 0u ? 0u : value;
}

// The same work without global parameters: the buffer is a device pointer among the kernel parameters, so a launch
// writes no SLANG_globalParams. The overlap of two such launches on two queues is reported beside that of fill.
[shader("compute")]
[numthreads(64, 1, 1)]
void fillDirect(uint3 threadId : SV_DispatchThreadID, uniform uint* output, uniform uint value,
    uniform uint count, uniform uint iterations)
{
    const uint i = threadId.x;
    if (i >= count)
        return;
    uint x = value;
    for (uint k = 0; k < iterations; ++k)
        x = x * 1664525u + 1013904223u;
    output[i] = x == 0u ? 0u : value;
}
