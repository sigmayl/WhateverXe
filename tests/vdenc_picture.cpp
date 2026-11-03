// CPU check for the Gen12 VDENC HEVC picture commands (CMD1/CMD2). Expected
// values follow media-driver's AddVdencCmd1Cmd / AddVdencCmd2Cmd for an
// 1920x1080 I picture, QP 26, target usage 4.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_vdenc_hwcmd_g12_X.cpp"
#include "../media/hevc_core/vdenc_picture.cpp"

namespace {
uint32_t field(const HevcCore::Packet &p, uint32_t dw, uint32_t lo, uint32_t hi) {
    const uint32_t width = hi - lo + 1;
    return (p[dw] >> lo) & (width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1);
}
}

int main() {
    using namespace HevcCore;
    using namespace HevcCore::VdEnc;

    Cmd1Params c1;
    auto cmd1 = VdEnc::cmd1(c1);
    assert(cmd1.size() == 31);
    assert(cmd1[1] == 0x05030200);
    assert(cmd1[2] == 0x0B090806);
    assert(cmd1[3] == 0x1C140C04);
    assert(cmd1[5] == 0x5C544C44);
    assert(cmd1[10] == 0x23131F0F);
    assert(cmd1[11] == 0x00002313);
    assert(cmd1[12] == 0x3E5C445C);
    assert(cmd1[13] == 0x1E040044);
    assert(cmd1[16] == 0x00070000);
    assert(cmd1[17] == 0x0D0E1007);
    assert(cmd1[18] == 0x00001E32);
    assert(cmd1[19] == 0x00140400);
    assert(cmd1[22] == 0x10101010 && cmd1[29] == 0x10101010);
    assert(cmd1[30] == 0x00101010);

    Cmd2Params c2;
    auto cmd2 = VdEnc::cmd2(c2);
    assert(cmd2.size() == 51);
    assert(cmd2[1] == 0x0437077F);  // 1919 | (1079 << 16)
    assert(cmd2[2] == 0x0005AFF3);  // I picture, temporal MVP and transform skip off
    assert(cmd2[3] == 0 && cmd2[4] == 0);
    assert(cmd2[5] == 0x00C0AC00);
    assert(cmd2[7] == 0x00062003);
    assert(cmd2[8] == 0x54555555);
    assert(cmd2[9] == 0x43845555);
    assert(cmd2[12] == 0xCE4014A0);
    assert(cmd2[15] == 0x4E201F40);
    assert(cmd2[16] == 0x0F00330A);  // MinQp 10, MaxQp 51
    assert(cmd2[17] == 0x00002710);
    assert(cmd2[19] == 0x180000C0);
    assert(cmd2[22] == 0x1F001102);
    assert(cmd2[23] == 0xAAAA1F00);
    assert(cmd2[26] == 0x0010003D);  // QP 26 lambda, picture type I
    assert(cmd2[27] == 0x00001A00);  // QpPrimeYAc 26
    assert(cmd2[34] == 0x21000000);

    // P picture with one reference at POC 0 and current POC 2. The reference
    // slots carry currPoc - refPoc; unused slots read POC 0, matching the
    // reference driver's invalid-frame-index handling.
    Cmd2Params p2;
    p2.intra = false;
    p2.numRefIdxL0 = 0;
    p2.currPoc = 2;
    p2.refPocL0[0] = 0;
    p2.refPocL1[0] = 0;
    const auto pcmd = VdEnc::cmd2(p2);
    // PICTURE_TYPE: 0 = I, 1 = P. A P slice must carry P; 2/3 are the B
    // values used for regular and low-delay B slices.
    assert(field(pcmd, 2, 20, 21) == 1);
    assert(field(pcmd, 3, 0, 7) == 2);   // POC0 in L0
    assert(field(pcmd, 3, 8, 15) == 2);  // POC0 in L1
    assert(field(pcmd, 3, 16, 23) == 2); // POC1 in L0
    assert(field(pcmd, 3, 24, 31) == 2); // POC1 in L1
    assert(field(pcmd, 4, 0, 7) == 2);   // POC2 in L0
    assert(field(pcmd, 4, 8, 15) == 2);  // POC2 in L1
    assert(field(pcmd, 5, 24, 27) == 0); // one L0 reference
    assert(field(pcmd, 5, 10, 11) == 3); // sub-pel mode
    assert(field(pcmd, 2, 24, 26) == 0); // no long-term references

    // A negative difference is stored as an 8-bit two's complement.
    Cmd2Params back;
    back.intra = false;
    back.currPoc = 1;
    back.refPocL0[0] = 3;
    back.refPocL1[0] = 3;
    const auto bcmd = VdEnc::cmd2(back);
    assert(field(bcmd, 3, 0, 7) == 0xFE); // 1 - 3
    assert(cmd2[35] == 0x00000700);
    // DW11 holds four 3-bit reference indices (FwdRef0..2, BwdRef0) and the
    // mapping-enable flag at bit 31. Every slot starts marked invalid (7);
    // media-driver fills the table with 7 before naming the slots that hold a
    // reference, so an unused list-1 slot never reads as frame 0.
    assert(cmd2[11] == 0x87070707);

    // Header: parallel video pipe, opcode 1, length 49 for CMD2.
    assert(field(cmd2, 0, 0, 11) == 49);
    assert(field(cmd2, 0, 23, 26) == 1);
    assert(field(cmd2, 0, 29, 31) == 3);

    // A P picture selects PICTURE_TYPE_P and keeps the reference POC words.
    Cmd2Params p;
    p.intra = false;
    p.numRefIdxL0 = 0;
    p.refL0[0] = 0;
    p.refIdxMapping[0] = 2;
    auto cmd2p = VdEnc::cmd2(p);
    assert(field(cmd2p, 2, 20, 21) == 1);
    // Default POCs are all zero, so every reference difference is zero.
    // DW4 keeps its non-POC upper half from the baseline word.
    assert(cmd2p[3] == 0 && (cmd2p[4] & 0xFFFF) == 0);
    assert(cmd2p[11] == 0x87070702);  // L0 slot names frame 2, L1 stays invalid
    assert(cmd2p[26] == 0x00100042);  // P picture, QP 26

    // Speed mode changes the tuned words.
    Cmd2Params speed;
    speed.targetUsage = 7;
    auto cmd2s = VdEnc::cmd2(speed);
    assert(cmd2s[12] == 0x89800DC0);
    assert(cmd2s[34] == 0x20000000);

    puts("PASS Gen12 VDENC HEVC CMD1/CMD2 picture commands");
    return 0;
}
