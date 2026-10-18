struct Payload { uint value; };
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> output : register(u0);
[shader("raygeneration")]
void rayGeneration() {
    uint3 id = DispatchRaysIndex();
    RayDesc ray;
    ray.Origin = float3(0,0,-2); ray.Direction = float3(0,0,1); ray.TMin = 0.001; ray.TMax = 100;
    Payload payload; payload.value = 1;
    TraceRay(scene, RAY_FLAG_NONE, 255, 0, 1, 0, ray, payload);
    output[id.x] = payload.value;
}
[shader("miss")]
void rayMiss(inout Payload payload) { payload.value = 7; }
[shader("closesthit")]
void rayHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes) { payload.value = 42; }

[shader("intersection")]
void rayIntersection() {
    BuiltInTriangleIntersectionAttributes attributes;
    attributes.barycentrics = float2(0,0);
    // Slang 2026.10.2 exposes the cross-target integer-attribute form for CUDA.
    ReportHitOptix(2.0f, 0, asuint(attributes.barycentrics.x), asuint(attributes.barycentrics.y));
}
