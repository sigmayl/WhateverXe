// CPU check for the Gen12 HCP HEVC picture state packet. Values follow
// media-driver's AddHcpEncodePicStateCmd for a 1920x1080 VDEnc I picture.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_hcp_hwcmd_g12_X.cpp"
#include "../media/hevc_core/hcp_picture.cpp"

int main() {
    using namespace HevcCore;
    using namespace HevcCore::Hcp;

    auto pic = Hcp::picState(PicStateParams {});
    assert(pic.size() == 41);
    assert(pic[0] == 0x73900027);  // HCP command 5, length 39
    assert(pic[1] == 0x008600EF);  // 239 x 134 minimum coding blocks
    assert(pic[2] == 0x200000CC);  // CTB 64, TU 32, 4:2:0
    assert(pic[4] == 0x04800008);  // SAO, AMP, strong smoothing; no CU QP delta
    assert(pic[19] == 0x81011AC0); // VDEnc SSE, rho-domain QP 26, first slice

    PicStateParams main10;
    main10.bitDepthLumaMinus8 = 2;
    main10.bitDepthChromaMinus8 = 2;
    main10.log2MaxCodingBlockMinus3 = 2;  // 32x32 CTB
    auto pic10 = Hcp::picState(main10);
    assert((pic10[2] >> 2 & 0x3) == 2);
    assert((pic10[2] >> 29 & 0x7) == 1);  // chroma format unchanged
    assert((pic10[5] >> 27 & 0x7) == 2);  // luma bit depth minus 8

    // The coding-block minimum is a two-bit field at DW2 bits 0-1; the VDEnc
    // splits CUs from this value, so a raised minimum has to reach the packet.
    PicStateParams minCu32;
    minCu32.log2MinCodingBlockMinus3 = 2;  // 32x32 minimum
    auto picMinCu = Hcp::picState(minCu32);
    assert((picMinCu[2] & 0x3) == 2);
    assert((picMinCu[2] >> 2 & 0x3) == 3);  // 64x64 CTB unchanged

    PicStateParams palette;
    palette.paletteEnabled = true;
    auto picPalette = Hcp::picState(palette);
    assert((picPalette[35] >> 31) == 1);       // palette enabled
    assert((picPalette[35] & 0x7F) == 64);     // palette max size
    assert((picPalette[35] >> 10 & 0x7F) == 32);  // delta palette max predictor

    PicStateParams rdoq;
    rdoq.rdoqEnabled = true;
    auto picRdoq = Hcp::picState(rdoq);
    assert(((picRdoq[19] >> 25) & 1) == 1);

    // DW19 bit 31 is TemporalMvPredDisable. The reference derives it as
    // !sps_temporal_mvp_enabled_flag, and a partial frame update forces it on.
    PicStateParams temporalMvp;
    temporalMvp.temporalMvp = true;
    auto picTemporal = Hcp::picState(temporalMvp);
    assert(((picTemporal[19] >> 31) & 1) == 0);
    temporalMvp.partialFrameUpdate = true;
    auto picPartial = Hcp::picState(temporalMvp);
    assert(((picPartial[19] >> 31) & 1) == 1);

    puts("PASS Gen12 HCP HEVC picture state packet");
    return 0;
}
