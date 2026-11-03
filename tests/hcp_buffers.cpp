// CPU check for the Gen12 HCP buffer and indirect-object state packets.
// Word positions follow media-driver's generated
// mhw_vdbox_hcp_hwcmd_g12_X (MIT, Intel Corporation).
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_hcp_hwcmd_g12_X.cpp"
#include "../media/hevc_core/hcp_state.cpp"

int main() {
    using namespace HevcCore;
    using namespace HevcCore::Hcp;

    PipeBufAddrParams buffers;
    buffers.decodedPicture = {0x10000000, 3};
    buffers.deblockingLine = {0x20000000, 1};
    buffers.deblockingTileLine = {0x20001000, 1};
    buffers.deblockingTileColumn = {0x20002000, 1};
    buffers.metadataLine = {0x20003000, 2};
    buffers.metadataTileLine = {0x20004000, 2};
    buffers.metadataTileColumn = {0x20005000, 2};
    buffers.saoLine = {0x20006000, 3};
    buffers.saoTileLine = {0x20007000, 3};
    buffers.saoTileColumn = {0x20008000, 3};
    buffers.currentMvTemporal = {0x20009000, 4};
    for (uint32_t i = 0; i < 8; ++i)
        buffers.references[i] = {0x30000000ull + i * 0x1000, 0};
    buffers.referenceControl = 0x11;
    buffers.originalUncompressed = {0x40000000, 5};
    buffers.streamout = {0x50000000, 6};
    buffers.encodedSliceSize = {0x60000000, 7};
    buffers.lcuIldb = {0x70000000, 8};
    for (uint32_t i = 0; i < 8; ++i)
        buffers.collocatedMv[i] = {0x80000000ull + i * 0x1000, 0};
    buffers.collocatedMvControl = 0x22;
    buffers.frameStatistics = {0x90000000, 9};
    buffers.sseSourceRowstore = {0xA0000000, 10};
    buffers.mvUpperRight = {0xB0000000, 11};
    buffers.intraUpperRight = {0xC0000000, 12};
    buffers.intraLeftRecon = {0xD0000000, 13};

    auto pipeBuf = pipeBufAddrState(buffers);
    assert(pipeBuf.size() == 121);
    assert(pipeBuf[0] == 0x73820077);  // HCP command 2, length 119
    assert(pipeBuf[1] == 0x10000000 && pipeBuf[2] == 0);
    assert(pipeBuf[3] == 3);
    assert(pipeBuf[4] == 0x20000000 && pipeBuf[6] == 1);
    assert(pipeBuf[31] == 0x20009000 && pipeBuf[33] == 4);
    assert(pipeBuf[37] == 0x30000000 && pipeBuf[51] == 0x30007000);
    assert(pipeBuf[53] == 0x11);
    // DW53 is the shared reference attribute word: the same cache policy plus
    // the tiled-resource mode of the reference surfaces (TRMode bits 13:14).
    PipeBufAddrParams tiled = buffers;
    tiled.referenceControl = referenceAttributes(TileMode::YS, 0x11);
    assert(pipeBufAddrState(tiled)[53] == 0x2011);
    tiled.referenceControl = referenceAttributes(TileMode::YF, 0x11);
    assert(pipeBufAddrState(tiled)[53] == 0x4011);
    assert(pipeBuf[54] == 0x40000000 && pipeBuf[56] == 5);
    assert(pipeBuf[57] == 0x50000000 && pipeBuf[59] == 6);
    assert(pipeBuf[60] == 0x60000000 && pipeBuf[62] == 7);
    assert(pipeBuf[63] == 0x70000000 && pipeBuf[65] == 8);
    assert(pipeBuf[66] == 0x80000000 && pipeBuf[80] == 0x80007000);
    assert(pipeBuf[82] == 0x22);
    assert(pipeBuf[98] == 0x90000000 && pipeBuf[100] == 9);
    assert(pipeBuf[101] == 0xA0000000 && pipeBuf[103] == 10);
    assert(pipeBuf[110] == 0xB0000000 && pipeBuf[112] == 11);
    assert(pipeBuf[113] == 0xC0000000 && pipeBuf[115] == 12);
    assert(pipeBuf[116] == 0xD0000000 && pipeBuf[118] == 13);

    // A 64-bit address above 4GB lands in the upper address DWORD.
    PipeBufAddrParams high = buffers;
    high.decodedPicture = {0x0000000212345000ull, 0};
    auto highBuf = pipeBufAddrState(high);
    assert(highBuf[1] == 0x12345000 && highBuf[2] == 2);

    IndObjParams indirect;
    indirect.bitstreamObject = {0x10000000, 1};
    indirect.bitstreamUpperBound = {0x1FFF000, 0};
    indirect.cuObject = {0x20000000, 2};
    indirect.pakBseObject = {0x30000000, 3};
    indirect.pakBseUpperBound = {0x3FFF000, 0};

    auto indObj = indObjBaseAddrState(indirect);
    assert(indObj.size() == 29);
    assert(indObj[0] == 0x7383001b);  // HCP command 3, length 27
    assert(indObj[1] == 0x10000000 && indObj[3] == 1);
    assert(indObj[4] == 0x1FFF000);
    assert(indObj[6] == 0x20000000 && indObj[8] == 2);
    assert(indObj[9] == 0x30000000 && indObj[11] == 3);
    assert(indObj[12] == 0x3FFF000);

    IndObjParams highIndirect = indirect;
    highIndirect.cuObject = {0x0000000112345000ull, 0};
    highIndirect.pakBseObject = {0x0000000212346000ull, 0};
    auto highInd = indObjBaseAddrState(highIndirect);
    assert(highInd[6] == 0x12345000 && highInd[7] == 1);
    assert(highInd[9] == 0x12346000 && highInd[10] == 2);

    puts("PASS Gen12 HCP HEVC buffer state packets");
    return 0;
}
