// CPU check for the Gen12 HCP HEVC slice state packet. Values follow
// media-driver's AddHcpEncodeSliceStateCmd for a VDEnc I slice.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_hcp_hwcmd_g12_X.cpp"
#include "../media/hevc_core/hcp_slice.cpp"

int main() {
    using namespace HevcCore;
    using namespace HevcCore::Hcp;

    auto slice = Hcp::sliceState(SliceParams {});
    assert(slice.size() == 13);
    assert(slice[0] == 0x7394000B);  // HCP command 6, length 11
    assert(slice[1] == 0);
    assert(slice[2] == 0);
    assert(slice[3] == 0x01000686);  // type I, last slice, QP 26, last in tile
    assert(slice[4] == 0x02001800);  // SAO luma/chroma, max merge index 4
    assert(slice[6] == 0x10A00000);  // VDEnc rounding 4/10
    assert(slice[7] == 0x00000604);  // emulation prevention, slice data, header

    // Second slice of a picture: next slice start and not last.
    SliceParams multi;
    multi.widthInCtb = 30;
    multi.ctbAddress = 510;       // 17 CTB rows down
    multi.nextCtbAddress = 540;
    multi.lastSlice = false;
    multi.lastSliceInTile = false;
    auto slice2 = Hcp::sliceState(multi);
    assert((slice2[1] & 0x3FF) == 0);          // start X = 510 % 30
    assert((slice2[1] >> 16 & 0x3FF) == 17);   // start Y = 510 / 30
    assert((slice2[2] & 0x3FF) == 0);          // next X = 540 % 30
    assert((slice2[2] >> 16 & 0x3FF) == 18);   // next Y = 540 / 30
    assert(((slice2[3] >> 2) & 1) == 0);       // not the last slice of the picture

    // Slice header length is a decoder field; the encoder keeps it zero and
    // carries the header bytes through the PAK insert object.
    SliceParams header;
    header.sliceHeaderBytes = 8;
    auto sliceH = Hcp::sliceState(header);
    assert(sliceH[5] == 0);

    // P slice keeps temporal MVP and does not force slice type I.
    SliceParams p;
    p.sliceType = 1;
    p.temporalMvp = true;
    p.cabacInit = true;
    auto sliceP = Hcp::sliceState(p);
    assert((sliceP[3] & 0x3) == 1);
    assert(((sliceP[3] >> 3) & 1) == 0);

    puts("PASS Gen12 HCP HEVC slice state packet");
    return 0;
}
