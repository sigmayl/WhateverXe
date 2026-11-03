// CPU check for the Gen12 HEVC picture command order. The order follows
// CodechalVdencHevcStateG12::SendPictureStateCommands.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_hcp_hwcmd_g12_X.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_vdenc_hwcmd_g12_X.cpp"
#include "../media/hevc_core/hcp_state.cpp"
#include "../media/hevc_core/hcp_picture.cpp"
#include "../media/hevc_core/hcp_qm.cpp"
#include "../media/hevc_core/vdenc_state.cpp"
#include "../media/hevc_core/vdenc_picture.cpp"
#include "../media/hevc_core/picture_commands.cpp"

namespace {
uint32_t bits(const HevcCore::Packet &packet, uint32_t lo, uint32_t hi) {
    const uint32_t width = hi - lo + 1;
    return (packet[0] >> lo) & (width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1);
}
bool isHcp(const HevcCore::Packet &packet, uint32_t command) {
    return bits(packet, 23, 26) == 7 && bits(packet, 16, 22) == command;
}
bool isVdenc(const HevcCore::Packet &packet, uint32_t subop) {
    return bits(packet, 23, 26) == 1 && bits(packet, 16, 22) == subop;
}
}

int main() {
    using namespace HevcCore;

    PictureParams params;
    params.referenceSurface.surfaceId = Hcp::SurfaceReference;

    auto first = pictureFirstLevel(params);
    assert(first.size() == 44);

    // The prolog wakes the Gen12 HEVC and MFX power wells before any media
    // engine work; iHD sends this at the head of the picture level.
    assert(bits(first[0], 23, 28) == 29 && bits(first[0], 29, 31) == 0);
    assert((first[0][0] & 0xFFF) == 1);
    assert(first[0][1] == 0x03000300);

    // Pipe initialization precedes the HCP pipe mode select.
    assert(bits(first[1], 23, 26) == 1 && bits(first[1], 16, 22) == 0xB);
    assert(first[1][0] == 0x708B0000 && first[1][1] == 2);
    assert(isHcp(first[2], 0xA));
    assert(first[2][0] == 0x738A0001 && first[2][1] == 1);
    // Gen12 mode switching requires a wait on both sides of PIPE_MODE_SELECT.
    assert(first[3].size() == 1 && first[3][0] == 0x68000100);
    assert(isHcp(first[4], Hcp::CommandPipeModeSelect));
    assert(first[5].size() == 1 && first[5][0] == 0x68000100);
    // The builder is still available for the flows that need it.
    const Packet wait = mfxWait(true);
    assert(wait.size() == 1 && wait[0] == 0x68000100);
    assert(isHcp(first[6], Hcp::CommandSurfaceState));
    assert(isHcp(first[7], Hcp::CommandSurfaceState));
    assert(isHcp(first[8], Hcp::CommandSurfaceState));
    assert(((first[8][1] >> 28) & 0xF) == Hcp::SurfaceReference);
    assert(isHcp(first[9], Hcp::CommandPipeBufAddrState));
    assert(isHcp(first[10], Hcp::CommandIndObjBaseAddrState));

    for (uint32_t i = 11; i < 19; ++i)
        assert(isHcp(first[i], 5));  // HCP_FQM_STATE
    for (uint32_t i = 19; i < 39; ++i)
        assert(isHcp(first[i], Hcp::CommandQmState));

    assert(isVdenc(first[39], VdEnc::SubopPipeModeSelect));
    assert(isVdenc(first[40], VdEnc::SubopSrcSurfaceState));
    assert(isVdenc(first[41], VdEnc::SubopRefSurfaceState));
    assert(isVdenc(first[42], VdEnc::SubopDsRefSurfaceState));
    assert(isVdenc(first[43], VdEnc::SubopPipeBufAddrState));

    auto second = pictureSecondLevel(params);
    assert(second.size() == 3);
    assert(isVdenc(second[0], 10));  // VDENC_CMD1
    assert(isHcp(second[1], Hcp::CommandPicState));
    assert(isVdenc(second[2], 9));   // VDENC_CMD2

    puts("PASS Gen12 HEVC picture command sequence");
    return 0;
}
