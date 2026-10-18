// SPDX-License-Identifier: MIT
#include <nvrhi/nvrhi.h>
#include <cstdio>
int main() {
    nvrhi::TextureDesc desc;
    desc.dimension=nvrhi::TextureDimension::Texture2DArray;
    desc.mipLevels=8;desc.arraySize=12;
    const auto check=[&](const nvrhi::TextureSubresourceSet& requested,bool single,
                         uint32_t expectedMips,uint32_t expectedSlices) {
        auto resolved=requested.resolve(desc,single);
        return resolved.baseMipLevel==requested.baseMipLevel &&
            resolved.baseArraySlice==requested.baseArraySlice &&
            resolved.numMipLevels==expectedMips && resolved.numArraySlices==expectedSlices;
    };
    if(!check({2,nvrhi::TextureSubresourceSet::AllMipLevels,3,nvrhi::TextureSubresourceSet::AllArraySlices},false,6,9) ||
       !check({7,UINT32_MAX-1,11,UINT32_MAX-1},false,1,1) ||
       !check({8,1,12,1},false,0,0) ||
       !check({UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX},false,0,0) ||
       !check({UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX},true,0,0) ||
       !check({2,0,3,0},false,0,0) ||
       !check({2,3,3,4},false,3,4) ||
       !check({2,nvrhi::TextureSubresourceSet::AllMipLevels,3,nvrhi::TextureSubresourceSet::AllArraySlices},true,1,9)) {
        std::fputs("Texture subresource range overflow/clipping regression\n",stderr);return 1;
    }
    desc.dimension=nvrhi::TextureDimension::Texture3D;
    const auto volume=nvrhi::TextureSubresourceSet{2,UINT32_MAX,UINT32_MAX,UINT32_MAX}.resolve(desc,false);
    if(volume.numMipLevels!=6 || volume.baseArraySlice!=0 || volume.numArraySlices!=1)return 1;
    std::puts("PASS texture subresource sentinels, overflow, empty and clipped ranges");
    return 0;
}
