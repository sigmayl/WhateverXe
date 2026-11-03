// CPU check for the Gen12 VDENC HEVC weights/offsets state packet.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_vdenc_hwcmd_g12_X.cpp"
#include "../media/hevc_core/vdenc_weights.cpp"

int main() {
    using namespace HevcCore;
    using namespace HevcCore::VdEnc;

    auto unity = VdEnc::weightsOffsetsState(WeightsParams {});
    assert(unity.size() == 3);
    assert(unity[0] == 0x70880001);
    assert(unity[1] == 0x00010001);  // weight 1, offset 0 twice
    assert(unity[2] == 0x00010001);

    WeightsParams weighted;
    weighted.weightedPred = true;
    weighted.denom = 64;
    weighted.lumaWeights[0][0] = 32;   // 32 + 64 = 96
    weighted.lumaOffsets[0][0] = 3;
    auto cmd = VdEnc::weightsOffsetsState(weighted);
    assert((cmd[1] & 0xFF) == 96);
    assert(((cmd[1] >> 8) & 0xFF) == 3);
    assert(((cmd[1] >> 16) & 0xFF) == 64);   // unset weight + denom
    assert(((cmd[2] >> 16) & 0xFF) == 64);   // backward reference mirrors list 1

    // Clamping keeps weights in the signed byte range.
    WeightsParams clipped = weighted;
    clipped.lumaWeights[0][1] = 200;
    clipped.lumaWeights[0][2] = -200;
    auto cmd2 = VdEnc::weightsOffsetsState(clipped);
    assert(((cmd2[1] >> 16) & 0xFF) == 127);
    assert((cmd2[2] & 0xFF) == 128);  // -128 as a byte

    puts("PASS Gen12 VDENC HEVC weights/offsets state packet");
    return 0;
}
