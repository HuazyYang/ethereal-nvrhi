// SPDX-License-Identifier: MIT
#include "../src/vulkan/spirv-float-controls.h"
#include <array>
#include <cstdio>
#include <vector>
using namespace nvrhi;
namespace d = nvrhi::vulkan::detail;
int main() {
    const std::array<uint32_t, 5> header{0x07230203u,0x00010500u,0,10,0};
    auto module = [&] { return std::vector<uint32_t>(header.begin(), header.end()); };
    auto scan = [](const auto& words,d::SpirvRoundingModes& modes) {
        return d::getSpirvRoundingModes(words.data(),words.size()*4,modes);
    };
    d::SpirvRoundingModes modes{7,7};
    auto words = module();
    words.insert(words.end(),{(2u<<16)|17u,1,(6u<<16)|16u,1,17,8,8,8});
    if (scan(words,modes)!=FS_OK || modes.rte || modes.rtz) return 1;
    words.insert(words.end(),{(4u<<16)|16u,1,4462,32,(4u<<16)|16u,2,4463,16,(4u<<16)|16u,2,4462,64});
    if (scan(words,modes)!=FS_OK || modes.rte!=6 || modes.rtz!=1) return 2;
    if (d::checkSpirvRoundingModes(modes,{6,1})!=FS_OK ||
        d::checkSpirvRoundingModes(modes,{2,1})!=FE_UNSUPPORTED ||
        d::checkSpirvRoundingModes(modes,{6,0})!=FE_UNSUPPORTED ||
        d::checkSpirvRoundingModes({},{})!=FS_OK) return 3;
    // Artifact payload need not be naturally aligned in host storage.
    std::vector<unsigned char> unaligned(words.size()*4+1);
    std::memcpy(unaligned.data()+1,words.data(),words.size()*4);
    modes={};
    if(d::getSpirvRoundingModes(unaligned.data()+1,unaligned.size()-1,modes)!=FS_OK || modes.rte!=6) return 4;
    auto rejects = [&](std::vector<uint32_t> bad) {
        d::SpirvRoundingModes unchanged{5,3};
        return scan(bad,unchanged)==FE_INVALID_ARGS && unchanged.rte==5 && unchanged.rtz==3;
    };
    auto bad=module();bad.push_back(0);if(!rejects(bad))return 5;
    bad=module();bad.push_back((4u<<16)|16u);if(!rejects(bad))return 6;
    bad=module();bad.insert(bad.end(),{(3u<<16)|16u,1,4462});if(!rejects(bad))return 7;
    bad=module();bad[0]=0;if(!rejects(bad))return 8;
    if(d::getSpirvRoundingModes(nullptr,20,modes)!=FE_INVALID_ARGS ||
        d::getSpirvRoundingModes(words.data(),19,modes)!=FE_INVALID_ARGS ||
        d::getSpirvRoundingModes(words.data(),words.size()*4-1,modes)!=FE_INVALID_ARGS)return 9;
    bad=module();bad.insert(bad.end(),{(4u<<16)|16u,1,4462,8});
    if(scan(bad,modes)!=FE_UNSUPPORTED)return 10;
    std::puts("SPIR-V RTE/RTZ width requirements, unsupported modes, bounded and unaligned scan PASS");
    return 0;
}
