// Fixture of nvrhi_cuda_tests dispatch: compiled to an NVRHI shader container with a CUDA PTX section. Every binding
// kind the CUDA backend writes into SLANG_globalParams is used once. cuda-reflection.slang compiles the same module
// without writeScale, for every target (a DXIL section holds one compute entry point).

struct Constants
{
    float4 scale;
    uint count;
    uint padding0;
    uint padding1;
    uint padding2;
};

ConstantBuffer<Constants> gConstants : register(b0);
StructuredBuffer<float4> gInput : register(t0);
Texture2D<float4> gTexture : register(t1);
Texture3D<float> gVolumes[2] : register(t2);    // t2, t3
SamplerState gSampler : register(s1);           // pairs with gTexture (t1)
RWStructuredBuffer<float4> gOutput : register(u0);
RWByteAddressBuffer gCounter : register(u1);
RWTexture2D<float4> gSurface : register(u2);

// gOutput[i] = gInput[i] * scale + bias + texel(i) + sampled + volume[1](i)
// gSurface[i % 4, i / 4] = (i, offset, 0, 1); gCounter counts the threads.
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 threadId : SV_DispatchThreadID, uniform float bias, uniform uint offset)
{
    const uint i = threadId.x;
    if (i >= gConstants.count)
        return;

    const float4 texel = gTexture.Load(int3(i % 4, (i / 4) % 4, 0));
    const float4 sampled = gTexture.SampleLevel(gSampler, float2(0.125, 0.125), 0);    // the center of texel (0, 0)
    const float volume = gVolumes[1].Load(int4(i % 2, 0, 0, 0));

    gOutput[i] = gInput[i] * gConstants.scale + bias + texel + sampled + volume;
    gCounter.InterlockedAdd(0, 1);
    if (i < 16)
        gSurface[uint2(i % 4, i / 4)] = float4(float(i), float(offset), 0, 1);
}

#ifndef NO_WRITE_SCALE
// gOutput[index] = scale: each dispatch sees the version of the volatile constant buffer written before it.
[shader("compute")]
[numthreads(1, 1, 1)]
void writeScale(uniform uint index)
{
    gOutput[index] = gConstants.scale;
}
#endif
