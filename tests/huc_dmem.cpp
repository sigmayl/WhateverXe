// CPU check for the Gen12 HuC PAK-stitch DMEM layout. Sizes, offsets and
// sentinel values follow media-driver's HucPakStitchDmemVdencG12.
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/hevc_core/huc_dmem.cpp"

int main() {
    using namespace HevcCore::Huc;

    static_assert(sizeof(PakStitchDmem) == 192, "DMEM buffer is 192 bytes");
    static_assert(offsetof(PakStitchDmem, lastTileBsStartInBytes) == 120);
    static_assert(offsetof(PakStitchDmem, totalSizeInCommandBuffer) == 128);
    static_assert(offsetof(PakStitchDmem, numSlices) == 138);
    static_assert(offsetof(PakStitchDmem, numTiles) == 146);
    static_assert(offsetof(PakStitchDmem, codec) == 156);
    static_assert(offsetof(PakStitchDmem, stitchCommandOffset) == 168);
    static_assert(offsetof(PakStitchDmem, batchBufferEndForStitch) == 172);
    static_assert(offsetof(PakStitchDmem, reserved) == 176);

    PakStitchParams params;
    params.pictureWidth = 1920;
    params.pictureHeight = 1080;
    params.numPipes = 2;
    params.numTilesPerPipe = 2;
    params.tileSizeRecordOffset0 = 0x100;
    params.tileSizeStatsOffset = 0x200;
    params.statsSizePerTile = 0x40;
    params.lastTileBsStartInBytes = 0x300;
    params.sliceHeaderSizeInBits = 0x40;
    params.totalSizeInCommandBuffer = 512;
    params.offsetInCommandBuffer = 72;
    params.picStateStartInBytes = 0x80;
    params.minCuSize = 3;
    params.bitDepthLuma = 8;
    params.bitDepthChroma = 8;
    params.chromaFormatIdc = 1;

    auto dmem = pakStitchDmem(params);

    assert(dmem.pictureWidth == 1920 && dmem.pictureHeight == 1080);
    assert(dmem.totalNumberOfPaks == 0);
    assert(dmem.codec == 2);  // HEVC VDEnc
    assert(dmem.maxPass == 1 && dmem.currentPass == 1);
    assert(dmem.minCuSize == 3);
    assert(dmem.cabacZeroWordFlag == 0);
    assert(dmem.bitDepthLuma == 8 && dmem.bitDepthChroma == 8);
    assert(dmem.chromaFormatIdc == 1);
    assert(dmem.totalSizeInCommandBuffer == 512);
    assert(dmem.offsetInCommandBuffer == 72);
    assert(dmem.lastTileBsStartInBytes == 0x300);
    assert(dmem.sliceHeaderSizeInBits == 0x40);
    assert(dmem.picStateStartInBytes == 0x80);
    assert(dmem.stitchEnable == 1);
    assert(dmem.stitchCommandOffset == 0);
    assert(dmem.batchBufferEndForStitch == kBatchBufferEnd);

    assert(dmem.tileSizeRecordOffset[0] == 0x100);
    assert(dmem.tileSizeRecordOffset[1] == 0x200);
    assert(dmem.tileSizeRecordOffset[2] == 0x280);
    assert(dmem.tileSizeRecordOffset[3] == kOffsetUnavailable);
    assert(dmem.tileSizeRecordOffset[4] == kOffsetUnavailable);
    assert(dmem.numTiles[0] == 2 && dmem.numTiles[1] == 2);
    assert(dmem.numTiles[2] == 0 && dmem.numTiles[3] == 0);
    assert(dmem.numSlices[0] == 0 && dmem.numSlices[3] == 0);

    // Unused streamout regions stay unavailable.
    assert(dmem.vdencStatOffset[0] == kOffsetUnavailable);
    assert(dmem.hevcPakStatOffset[4] == kOffsetUnavailable);
    assert(dmem.hevcStreamoutOffset[0] == kOffsetUnavailable);
    assert(dmem.vp9PakStatOffset[0] == kOffsetUnavailable);
    assert(dmem.vp9CounterBufferOffset[0] == kOffsetUnavailable);

    PakStitchParams single;
    single.numPipes = 1;
    single.numTilesPerPipe = 4;
    single.tileSizeStatsOffset = 0x80;
    single.statsSizePerTile = 0x20;
    auto singleDmem = pakStitchDmem(single);
    assert(singleDmem.tileSizeRecordOffset[0] == 0);
    assert(singleDmem.tileSizeRecordOffset[1] == 0x80);
    assert(singleDmem.tileSizeRecordOffset[2] == kOffsetUnavailable);
    assert(singleDmem.numTiles[0] == 4);

    puts("PASS Gen12 HuC HEVC PAK stitch DMEM layout");
    return 0;
}
