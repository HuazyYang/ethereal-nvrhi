// SPDX-License-Identifier: MIT
Texture2DArray<float4> source : register(t0);
SamplerState pointSampler : register(s0);
RWStructuredBuffer<float4> output : register(u0);
[shader("compute")]
[numthreads(1,1,1)]
void main()
{
    output[0] = source.SampleLevel(pointSampler, float3(0.5,0.5,0), 0);
    output[1] = source.SampleLevel(pointSampler, float3(0.5,0.5,0), 1);
}
