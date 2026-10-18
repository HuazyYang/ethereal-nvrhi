struct Params { uint count; float scale; uint2 padding; };
ConstantBuffer<Params> params : register(b2);
StructuredBuffer<float> source : register(t3);
RWStructuredBuffer<float> destination : register(u4);
[shader("compute")]
[numthreads(64,1,1)]
void main(uint3 tid : SV_DispatchThreadID) { if(tid.x < params.count) destination[tid.x] = source[tid.x]*params.scale; }
