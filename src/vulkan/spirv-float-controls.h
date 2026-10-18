// SPDX-License-Identifier: MIT
#pragma once
#include <nvrhi/nvrhi.h>
#include <cstring>
namespace nvrhi::vulkan::detail {
// Bits 0/1/2 describe 16/32/64-bit arithmetic, independently for RTE and RTZ.
struct SpirvRoundingModes { uint32_t rte = 0, rtz = 0; };
// Bounded scan of execution-mode declarations, not a full SPIR-V validator.
// memcpy permits artifact payloads with no host uint32 alignment guarantee.
inline FRESULT getSpirvRoundingModes(const void* binary, size_t size, SpirvRoundingModes& output) noexcept {
    if (!binary || size < 20 || size % 4) return FE_INVALID_ARGS;
    const auto* bytes = static_cast<const unsigned char*>(binary);
    const auto word = [&](size_t i) { uint32_t value; std::memcpy(&value, bytes + i * 4, 4); return value; };
    if (word(0) != 0x07230203u) return FE_INVALID_ARGS;
    SpirvRoundingModes modes;
    const size_t words = size / 4;
    for (size_t i = 5; i < words;) {
        const uint32_t instruction = word(i), count = instruction >> 16, opcode = instruction & 0xffffu;
        if (!count || count > words - i) return FE_INVALID_ARGS;
        if (opcode == 16) { // OpExecutionMode
            if (count < 3) return FE_INVALID_ARGS;
            const auto mode = word(i + 2);
            if (mode == 4462 || mode == 4463) { // RoundingModeRTE / RoundingModeRTZ
                if (count != 4) return FE_INVALID_ARGS;
                const uint32_t width = word(i + 3);
                uint32_t bit;
                if (width == 16) bit = 1;
                else if (width == 32) bit = 2;
                else if (width == 64) bit = 4;
                else return FE_UNSUPPORTED;
                if (mode == 4462) modes.rte |= bit;
                else modes.rtz |= bit;
            }
        }
        i += count;
    }
    output = modes;
    return FS_OK;
}
inline FRESULT checkSpirvRoundingModes(SpirvRoundingModes required, SpirvRoundingModes supported) noexcept {
    return (required.rte & ~supported.rte) || (required.rtz & ~supported.rtz) ? FE_UNSUPPORTED : FS_OK;
}
} // namespace nvrhi::vulkan::detail
