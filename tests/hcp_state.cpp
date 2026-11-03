// CPU check for the Gen12 HCP picture-level packets. Expected words come from
// media-driver's generated mhw_vdbox_hcp_hwcmd_g12_X (MIT, Intel Corporation)
// driven with the same field values in a scratch build.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "../media/hevc_core/hw_command.hpp"
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_hcp_hwcmd_g12_X.cpp"
#include "../media/hevc_core/hcp_state.hpp"
#include "../media/hevc_core/hcp_state.cpp"

namespace {
using HevcCore::Packet;

void expect(const Packet &packet, const std::vector<uint32_t> &words) {
    assert(packet.size() == words.size());
    for (uint32_t i = 0; i < words.size(); ++i) {
        if (packet[i] != words[i]) {
            fprintf(stderr, "DWORD %u: got %08x, want %08x\n", i, packet[i], words[i]);
            assert(false);
        }
    }
}

uint32_t field(const Packet &p, uint32_t dw, uint32_t lo, uint32_t hi) {
    const uint32_t width = hi - lo + 1;
    return (p[dw] >> lo) & (width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1);
}
}

int main() {
    using namespace HevcCore;
    using namespace HevcCore::Hcp;

    auto pipe = Hcp::pipeModeSelect(Hcp::PipeModeSelect {});
    expect(pipe, {0x73800005, 0x00001405, 0, 0, 0, 0, 0});
    // HCP header: engine 7h, media instruction command 0, length 5.
    assert(field(pipe, 0, 0, 11) == 5);
    assert(field(pipe, 0, 16, 22) == CommandPipeModeSelect);
    assert(field(pipe, 0, 23, 26) == 7);
    assert(field(pipe, 0, 27, 28) == 2);
    assert(field(pipe, 0, 29, 31) == 3);
    assert(field(pipe, 1, 0, 0) == 1);   // encode
    assert(field(pipe, 1, 5, 7) == 0);   // HEVC
    assert(field(pipe, 1, 10, 10) == 1); // VDEnc mode
    assert(field(pipe, 1, 2, 2) == 1);   // PAK pipeline stream-out
    assert(field(pipe, 1, 12, 12) == 1); // frame level stream-out

    Hcp::PipeModeSelect rdoq;
    rdoq.rdoq = true;
    rdoq.streamOut = false;
    auto pipe2 = Hcp::pipeModeSelect(rdoq);
    assert(field(pipe2, 1, 11, 11) == 1);
    assert(field(pipe2, 1, 2, 2) == 0 && field(pipe2, 1, 12, 12) == 0);

    // HCP_TILE_CODING for the single-tile picture: one pipe, one tile column,
    // the tile spans the frame in 8x8 minimum coding blocks, and the low
    // stream-out offsets keep the single-tile defaults.
    Hcp::TileCodingParams tile;
    tile.tileWidthInMinCbMinus1 = 79;   // 640 / 8 - 1
    tile.tileHeightInMinCbMinus1 = 45;  // 368 / 8 - 1
    const auto tilePacket = Hcp::tileCodingState(tile);
    assert(tilePacket.size() == 20);
    assert(tilePacket[0] == 0x73950012);            // 18 dwords, command 0x15
    assert(tilePacket[1] == 0x00010001);            // 1 pipe, 1 tile column
    assert(tilePacket[2] == 0xC0000000);            // last tile of row/column
    assert(tilePacket[3] == ((79u << 16) | 45u));   // width/height in min CB
    for (uint32_t i = 4; i < 20; ++i)
        assert(tilePacket[i] == 0);

    Hcp::SurfaceState source;
    source.surfaceId = SurfaceSource;    source.pitch = 1920;
    source.uvRowOffset = 1088;
    expect(Hcp::surfaceState(source), {0x73810003, 0x1000077F, 0x20000440, 0x04400000, 0});
    assert(field(Hcp::surfaceState(source), 2, 27, 31) == 4); // PLANAR4208
    assert(field(Hcp::surfaceState(source), 1, 0, 16) == 1919);
    assert(field(Hcp::surfaceState(source), 1, 28, 31) == 1);
    assert(field(Hcp::surfaceState(source), 3, 0, 15) == 0);       // alpha
    assert(field(Hcp::surfaceState(source), 3, 16, 31) == 1088);   // YOffsetForVCr

    Hcp::SurfaceState recon = source;
    recon.surfaceId = SurfaceDecoded;
    expect(Hcp::surfaceState(recon), {0x73810003, 0x0000077F, 0x20000440, 0x04400000, 0});

    // Compression fields land in DWORD 4.
    Hcp::SurfaceState compressed = source;
    compressed.memoryCompressionEnable = 0x3;
    compressed.compressionType = 0x12;
    compressed.compressionFormat = 0x5;
    assert(Hcp::surfaceState(compressed)[4] == 0x00051203);

    // HCP_REF_IDX_STATE for a P slice: list 0, one entry pointing at
    // reference base address 0 with the picture order count difference.
    Hcp::RefIdxParams refs;
    refs.list = 0;
    refs.numRefs = 1;
    refs.entries[0].frameId = 0;
    refs.entries[0].pocDelta = 2;
    const auto refIdx = Hcp::refIdxState(refs);
    assert(refIdx.size() == 18);
    assert(refIdx[0] == 0x73920010); // header, DwordLength 16
    assert(field(refIdx, 0, 16, 22) == CommandRefIdxState);
    assert(field(refIdx, 1, 0, 0) == 0);  // list 0
    assert(field(refIdx, 1, 1, 4) == 0);  // one reference
    assert(field(refIdx, 2, 0, 7) == 2);  // POC difference
    assert(field(refIdx, 2, 8, 10) == 0); // RefAddr 0
    assert(field(refIdx, 2, 13, 13) == 0);
    // media-driver's encoder codes progressive pictures and writes
    // BottomFieldFlag = 1 into every programmed reference entry.
    assert(field(refIdx, 2, 15, 15) == 1);
    assert(field(refIdx, 2, 14, 14) == 0); // field picture flag stays clear
    for (uint32_t i = 3; i < 18; ++i)
        assert(refIdx[i] == 0);

    // Negative POC differences are stored as an 8-bit two's complement and
    // long-term/field flags travel in the same entry.
    Hcp::RefIdxParams negative;
    negative.list = 0;
    negative.numRefs = 1;
    negative.entries[0].frameId = 3;
    negative.entries[0].pocDelta = -1;
    negative.entries[0].longTerm = true;
    const auto negativePacket = Hcp::refIdxState(negative);
    assert(field(negativePacket, 2, 0, 7) == 0xFF);
    assert(field(negativePacket, 2, 8, 10) == 3);
    assert(field(negativePacket, 2, 13, 13) == 1);
    assert(field(negativePacket, 2, 15, 15) == 1);

    // Two entries use DW2 and DW3.
    Hcp::RefIdxParams two;
    two.numRefs = 2;
    two.entries[0].pocDelta = 1;
    two.entries[1].frameId = 1;
    two.entries[1].pocDelta = 3;
    const auto twoPacket = Hcp::refIdxState(two);
    assert(field(twoPacket, 1, 1, 4) == 1);
    assert(field(twoPacket, 2, 0, 7) == 1);
    assert(field(twoPacket, 3, 0, 7) == 3);
    assert(field(twoPacket, 3, 8, 10) == 1);
    assert(field(twoPacket, 2, 15, 15) == 1);
    assert(field(twoPacket, 3, 15, 15) == 1);

    // HCP_PIPE_BUF_ADDR_STATE reference attributes: cache policy in bits 1:6,
    // tiled-resource mode in bits 13:14 (TileYS = 1, TileYF = 2, TileY and
    // linear = 0). This is Mhw_ConvertToTRMode's mapping.
    assert(referenceAttributes(TileMode::None) == 0);
    assert(referenceAttributes(TileMode::Y) == 0);
    assert(referenceAttributes(TileMode::YS) == 0x2000);
    assert(referenceAttributes(TileMode::YF) == 0x4000);
    assert(referenceAttributes(TileMode::YF, 0x2A) == 0x402A);
    assert(referenceAttributes(TileMode::YS, 0x3C) == 0x203C);

    puts("PASS Gen12 HCP HEVC picture state packets");
    return 0;
}
