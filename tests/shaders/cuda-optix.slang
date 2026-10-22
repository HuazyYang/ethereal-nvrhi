// Fixture of nvrhi_cuda_tests optix: compiled to an NVRHI shader container with an OptiX PTX section.
//
// Rays go from z = -1 towards +z through a width x height grid over [-1, 1]^2. The scene has a triangle instance
// (left) and an AABB instance (right): triangle hits return the tint of their record, box hits return red, misses
// return blue. The box hit group has no attributes of its own (BuiltInTriangleIntersectionAttributes, unused).

struct Payload
{
    float4 color;
};

struct HitConstants
{
    float4 tint;
};

struct BoxAttributes
{
    float distance;
};

RaytracingAccelerationStructure gScene : register(t0);
RWStructuredBuffer<float4> gOutput : register(u0);

[shader("raygeneration")]
void rayGeneration()
{
    const uint2 index = DispatchRaysIndex().xy;
    const uint2 size = DispatchRaysDimensions().xy;
    const float2 uv = (float2(index) + 0.5) / float2(size) * 2.0 - 1.0;

    RayDesc ray;
    ray.Origin = float3(uv, -1.0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.0;
    ray.TMax = 100.0;

    Payload payload;
    payload.color = float4(0, 0, 0, 0);
    TraceRay(gScene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);
    gOutput[index.y * size.x + index.x] = payload.color;
}

[shader("miss")]
void miss(inout Payload payload)
{
    payload.color = float4(0, 0, 1, 1);
}

[shader("closesthit")]
void triangleHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes,
    uniform ConstantBuffer<HitConstants> constants)
{
    payload.color = constants.tint;
}

// Slang has no ReportHit for OptiX: the CUDA target calls optixReportIntersection directly.
void reportBoxHit(float distance)
{
    __target_switch
    {
    case cuda:
        __intrinsic_asm "optixReportIntersection($0, 0)";
    default:
        BoxAttributes attributes;
        attributes.distance = distance;
        ReportHit(distance, 0, attributes);
    }
}

[shader("intersection")]
void boxIntersection()
{
    reportBoxHit(1.0);
}

[shader("closesthit")]
void boxHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.color = float4(1, 0, 0, 1);
}
