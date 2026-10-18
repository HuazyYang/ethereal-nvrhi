// SPDX-License-Identifier: MIT
struct Material { uint value; uint3 padding; };
struct RaySettings { float firstX; float stepX; uint2 padding; };
struct Payload { uint value; };
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> output : register(u0);
[shader("raygeneration")]
void rayGeneration(uniform ConstantBuffer<RaySettings> settings)
{
    uint index = DispatchRaysIndex().x;
    RayDesc ray;
    ray.Origin = float3(settings.firstX + settings.stepX * index, 0, -2);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.001;
    ray.TMax = 100;
    Payload payload = { 0 };
    TraceRay(scene, RAY_FLAG_NONE, 255, 0, 1, 0, ray, payload);
    output[index] = payload.value;
}
[shader("miss")]
void rayMiss(inout Payload payload, uniform ConstantBuffer<Material> material)
{
    payload.value = material.value;
}
[shader("closesthit")]
void rayHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes,
            uniform ConstantBuffer<Material> material)
{
    payload.value = material.value;
}
