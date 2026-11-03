// CPU check for the Gen12 HCP PAK insert command. Header flags and inline
// payload rules follow media-driver's AddHcpPakInsertObject.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_hcp_hwcmd_g12_X.cpp"
#include "../media/hevc_core/hcp_pak_insert.cpp"

namespace {
uint32_t field(const HevcCore::Packet &p, uint32_t dw, uint32_t lo, uint32_t hi) {
    const uint32_t width = hi - lo + 1;
    return (p[dw] >> lo) & (width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1);
}
}

int main() {
    using namespace HevcCore;
    using namespace HevcCore::Hcp;

    // Five slice-header bytes, 40 bits: the payload is padded to two DWORDs.
    const uint8_t sliceHeader[] = {0x00, 0x00, 0x01, 0x02, 0x03};
    PakInsertParams slice;
    slice.data = sliceHeader;
    slice.bitSize = 40;
    slice.lastHeader = true;
    slice.endOfSlice = true;
    slice.emulationByteInsert = true;
    slice.skipEmulationByteCount = 3;
    slice.resetBitstreamStartingPos = true;

    auto inserted = pakInsert(slice);
    assert(inserted.header.size() == 2);
    assert(inserted.header[0] == 0x73a20002);  // HCP command 34, length 2
    assert(inserted.header[1] == 0x0000483E);
    assert(field(inserted.header, 1, 1, 1) == 1);   // end of slice
    assert(field(inserted.header, 1, 2, 2) == 1);   // last header
    assert(field(inserted.header, 1, 3, 3) == 1);   // emulation insertion
    assert(field(inserted.header, 1, 4, 7) == 3);   // skip count
    assert(field(inserted.header, 1, 8, 13) == 8);  // bits in last DWORD
    assert(field(inserted.header, 1, 14, 14) == 1); // slice header indicator
    assert(field(inserted.header, 1, 31, 31) == 0); // inline payload
    assert(inserted.payload.size() == 8);
    for (uint32_t i = 0; i < 5; ++i)
        assert(inserted.payload[i] == sliceHeader[i]);
    assert(inserted.payload[5] == 0 && inserted.payload[7] == 0);

    // An unaligned bit count still reports the exact last-DWORD bit count.
    PakInsertParams unaligned = slice;
    unaligned.bitSize = 33;
    auto unalignedCmd = pakInsert(unaligned);
    assert(field(unalignedCmd.header, 1, 8, 13) == 1);
    assert(unalignedCmd.payload.size() == 8);  // ceil(33/8) = 5 bytes, padded

    // End of sequence only.
    PakInsertParams eos;
    eos.lastPicInSeq = true;
    auto eosCmd = pakInsert(eos);
    assert(eosCmd.header[0] == 0x73a20002);
    assert(field(eosCmd.header, 1, 8, 13) == 8);
    assert(eosCmd.payload.size() == 8);
    assert(eosCmd.payload[0] == 0x00 && eosCmd.payload[1] == 0x00
        && eosCmd.payload[2] == 0x01 && eosCmd.payload[3] == 0x48);
    assert(eosCmd.payload[4] == 0x01 && eosCmd.payload[5] == 0x00
        && eosCmd.payload[6] == 0x00 && eosCmd.payload[7] == 0x00);

    // End of sequence and end of bitstream share one payload.
    PakInsertParams both;
    both.lastPicInSeq = true;
    both.lastPicInStream = true;
    auto bothCmd = pakInsert(both);
    assert(bothCmd.header[0] == 0x73a20003);
    assert(field(bothCmd.header, 1, 8, 13) == 16);
    assert(bothCmd.payload.size() == 12);
    assert(bothCmd.payload[8] == 0x4A && bothCmd.payload[9] == 0x01);

    // No payload data is accepted when all bits are in the last DWORD.
    PakInsertParams empty;
    empty.data = nullptr;
    empty.bitSize = 0;
    auto emptyCmd = pakInsert(empty);
    assert(emptyCmd.header[0] == 0x73a20000);
    assert(field(emptyCmd.header, 1, 8, 13) == 32);
    assert(emptyCmd.payload.empty());

    static_assert(kPakInsertMaxPayloadBytes == 16380, "12-bit DWORD_LENGTH payload bound");

    puts("PASS Gen12 HCP HEVC PAK insert command");
    return 0;
}
