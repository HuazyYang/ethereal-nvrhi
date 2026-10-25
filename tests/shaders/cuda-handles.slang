// Fixture of the "handles" test: Slang's DescriptorHandle<T> on CUDA. The handles are the CUDA texture and surface
// objects (cuda::IDevice::getTextureHandle / getSurfaceHandle), passed as a kernel argument and in a buffer; surface
// handles of two element types are made from raw 64-bit values, as a sparse volume keeps one per channel.

struct Handles
{
    Sampler3D<float>.Handle linearVolume;   // the volume with linear filtering
    uint64_t rawSurfaces[2];                // an R32_FLOAT and an RGBA8_UINT volume, as RWTexture3D handles
};

[shader("compute")]
[numthreads(4, 4, 4)]
void main(uniform Handles* handles, uniform Sampler3D<float>.Handle pointVolume, uniform float* output,
          uint3 id : SV_DispatchThreadID)
{
    const uint index = (id.z * 4 + id.y) * 4 + id.x;

    // Point sampling at a texel centre returns the texel; linear sampling halfway to the next texel along x averages
    // the two (clamped at the last texel).
    const float point = pointVolume.SampleLevel((float3(id) + 0.5) / 4.0, 0);
    const float linear = handles->linearVolume.SampleLevel((float3(id) + float3(1.0, 0.5, 0.5)) / 4.0, 0);

    RWTexture3D<float>.Handle floats = RWTexture3D<float>.Handle(handles->rawSurfaces[0]);
    RWTexture3D<uint8_t4>.Handle bytes = RWTexture3D<uint8_t4>.Handle(handles->rawSurfaces[1]);
    floats[id] = point * 2.0;
    bytes[id] = uint8_t4(uint8_t(index), uint8_t(id.x), uint8_t(id.y), uint8_t(id.z));

    output[index * 2 + 0] = point;
    output[index * 2 + 1] = linear;
}

// 12 bytes of kernel arguments: cuLaunchKernel takes them unpadded.
[shader("compute")]
[numthreads(1, 1, 1)]
void unpaddedArguments(uniform uint* output, uniform uint value)
{
    output[0] = value;
}
