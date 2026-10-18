#include <nvrhi/shader-program.h>
#include <nvrhi/cuda.h>
#include <nvrhi/interop.h>
#include <cstdio>
int main()
{
    if (!nvrhi::verifyHeaderVersion()) return 1;
    nvrhi::IShaderProgram* program = reinterpret_cast<nvrhi::IShaderProgram*>(1);
    if (nvrhi::createShaderProgram(nullptr, nullptr, 0, &program) != nvrhi::FE_INVALID_ARGS || program) return 2;
    nvrhi::IDevice* device = reinterpret_cast<nvrhi::IDevice*>(1);
    if (nvrhi::cuda::nvrhiCudaCreateDevice(nullptr, &device) != nvrhi::FE_INVALID_ARGS || device) return 3;
    std::puts("Installed NVRHI/CUDA public C exports and header ABI passed");
    return 0;
}
