// CPU check for the Gen12 VDENC HEVC walker state packet. Values follow
// media-driver's AddVdencWalkerStateCmd for a single-pipe, untiled slice.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_vdenc_hwcmd_g12_X.cpp"
#include "../media/hevc_core/vdenc_walker.cpp"

int main() {
    using namespace HevcCore;
    using namespace HevcCore::VdEnc;

    auto walker = VdEnc::walkerState(WalkerParams {});
    assert(walker.size() == 27);
    assert(walker[0] == 0x70870019);  // subop 7, length 25
    assert(walker[1] == 0 && walker[2] == 0);
    assert(walker[5] == 0x043F077F);  // tile 1920x1088
    assert(walker[11] == 0);
    assert(walker[12] == 0x01000000); // IBC/palette disabled, reserved bit set

    WalkerParams multi;
    multi.startXCtb = 3;
    multi.startYCtb = 7;
    multi.nextXCtb = 4;
    multi.nextYCtb = 8;
    multi.log2WeightDenom = 5;
    auto walker2 = VdEnc::walkerState(multi);
    assert((walker2[1] & 0x1FF) == 7);          // start Y
    assert((walker2[1] >> 16 & 0x1FF) == 3);    // start X
    assert((walker2[2] & 0x3FF) == 8);          // next Y
    assert((walker2[2] >> 16 & 0x3FF) == 4);    // next X
    assert((walker2[3] & 0x7) == 5);            // weight denominator

    puts("PASS Gen12 VDENC HEVC walker state packet");
    return 0;
}
