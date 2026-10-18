Texture2D<float4> pointTexture : register(t0);
Texture2D<float4> linearTexture : register(t1);
SamplerState pointSampler : register(s0);
SamplerState linearSampler : register(s1);
RWStructuredBuffer<float4> output : register(u0);
[shader("compute")]
[numthreads(1,1,1)]
void textureSampling()
{
    output[0] = pointTexture.SampleLevel(pointSampler,float2(0.5f,0.5f),0);
    output[1] = linearTexture.SampleLevel(linearSampler,float2(0.5f,0.5f),0);
}
