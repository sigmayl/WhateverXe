// SPDX-License-Identifier: MIT
#include "picture_commands.hpp"

namespace HevcCore {

// MI_FLUSH_DW with the video pipeline cache invalidate bit. It drains the
// media pipeline before the batch ends, so the PAK output is complete when the
// fence signals. iHD places it right after the walker.
Packet flushDw() {
    Packet packet(5);
    // MI opcode 26h, DwordLength 3, no post-sync write.
    packet[0] = 3 | (1u << 7) | (38u << 23);
    return packet;
}

// MI_FORCE_WAKEUP: MI command 1Dh, DwordLength 1. DWORD 1 bit 0..4 force the
// media slices awake, bits 8/9 control the HEVC and MFX power wells and bits
// 24/25 are their masks. iHD sends the HEVC and MFX wells with masks set.
Packet forceWakeup(bool mfxPowerWell, bool hevcPowerWell) {
    Packet packet(2);
    packet[0] = 1 | (29u << 23);
    uint32_t wake = 0;
    if (mfxPowerWell) {
        wake |= 1u << 9;
        wake |= 1u << 25;
    }
    if (hevcPowerWell) {
        wake |= 1u << 8;
        wake |= 1u << 24;
    }
    packet[1] = wake;
    return packet;
}

// MFX_WAIT: one DWORD, command type 3 (parallel video pipe), subtype 1 (single
// DWORD), sub-opcode 0. Bit 8 stalls the VDBOX pipeline until the MFX/VRT
// pipelines reach the wait point. iHD brackets HCP_PIPE_MODE_SELECT with this
// command on Gen11+, before and after.
Packet mfxWait(bool stallVdboxPipeline) {
    Packet packet(1);
    packet[0] = (3u << 29) | (1u << 27) | (stallVdboxPipeline ? 0x100u : 0u);
    return packet;
}

std::vector<Packet> pictureFirstLevel(const PictureParams &params) {
    std::vector<Packet> packets;

    // The picture prolog owns the Gen12 power wells. iHD sends this before the
    // command buffer header, so it must be the first command in the batch.
    packets.push_back(forceWakeup(true, true));

    // iHD initializes both pipes before the HCP pipe mode select. Without the
    // initialization bit the engine accepts the stream but runs no VDEnc work.
    packets.push_back(VdEnc::controlState(true));
    packets.push_back(Hcp::controlState(true));
    // Gen12 AddHcpPipeModeSelectCmd brackets the mode switch with MFX_WAIT
    // internally, including the HEVC encode path.
    packets.push_back(mfxWait(true));
    packets.push_back(Hcp::pipeModeSelect(params.hcpPipeMode));
    packets.push_back(mfxWait(true));
    packets.push_back(Hcp::surfaceState(params.sourceSurface));
    packets.push_back(Hcp::surfaceState(params.reconSurface));
    packets.push_back(Hcp::surfaceState(params.referenceSurface));
    packets.push_back(Hcp::pipeBufAddrState(params.pipeBuf));
    packets.push_back(Hcp::indObjBaseAddrState(params.indObj));

    const auto fqm = Hcp::fqmState(params.qm);
    packets.insert(packets.end(), fqm.begin(), fqm.end());
    const auto qm = Hcp::qmState(params.qm);
    packets.insert(packets.end(), qm.begin(), qm.end());

    packets.push_back(VdEnc::pipeModeSelect(params.vdencPipeMode));
    packets.push_back(VdEnc::srcSurfaceState(params.vdencSource));
    packets.push_back(VdEnc::refSurfaceState(params.vdencReference, 0));
    packets.push_back(VdEnc::dsRefSurfaceState(params.downscaled4x, params.downscaled8x));
    packets.push_back(VdEnc::pipeBufAddrState(params.vdencBuf));

    return packets;
}

std::vector<Packet> pictureSecondLevel(const PictureParams &params) {
    std::vector<Packet> packets;
    packets.push_back(VdEnc::cmd1(params.cmd1));
    packets.push_back(Hcp::picState(params.pic));
    packets.push_back(VdEnc::cmd2(params.cmd2));
    return packets;
}

}
