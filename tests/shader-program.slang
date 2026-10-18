struct Nested { float3 direction; uint tag; };
struct Params { Nested nested; float3 values[3]; row_major float2x3 transform; };
ConstantBuffer<Params> params : register(b2, space1);
StructuredBuffer<float> sources[3] : register(t4, space1);
RWStructuredBuffer<float> output : register(u3, space1);
[shader("compute")][numthreads(1,1,1)]
void layoutMain(uint3 id : SV_DispatchThreadID)
{
    output[id.x] = sources[id.x % 3][id.x] + params.values[id.x % 3].x + params.transform[1][2] + params.nested.direction.z;
}
