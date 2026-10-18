RWStructuredBuffer<uint> output : register(u0);
[shader("compute")]
[numthreads(4,1,1)]
void entryParameters(uint3 tid : SV_DispatchThreadID, uniform uint factor, uniform uint64_t bias)
{
    output[tid.x] = factor + uint(bias >> 32) + tid.x;
}
