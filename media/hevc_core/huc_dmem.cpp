// SPDX-License-Identifier: MIT
#include "huc_dmem.hpp"

#include <stddef.h>
#include <string.h>

namespace HevcCore {
namespace Huc {
namespace {

template <typename T, size_t N>
void fillUnavailable(T (&offsets)[N]) {
    for (size_t i = 0; i < N; ++i)
        offsets[i] = kOffsetUnavailable;
}

}

PakStitchDmem pakStitchDmem(const PakStitchParams &params) {
    PakStitchDmem dmem;
    memset(&dmem, 0, sizeof(dmem));

    // The kernel reports tile statistics at the offsets supplied here. The
    // other streamout regions are unused on the HEVC VDEnc path.
    fillUnavailable(dmem.tileSizeRecordOffset);
    fillUnavailable(dmem.vdencStatOffset);
    fillUnavailable(dmem.hevcPakStatOffset);
    fillUnavailable(dmem.hevcStreamoutOffset);
    fillUnavailable(dmem.vp9PakStatOffset);
    fillUnavailable(dmem.vp9CounterBufferOffset);

    dmem.pictureWidth = params.pictureWidth;
    dmem.pictureHeight = params.pictureHeight;
    dmem.totalNumberOfPaks = 0;
    dmem.codec = static_cast<uint8_t>(params.codec);
    dmem.maxPass = params.maxPass;
    dmem.currentPass = params.currentPass;
    dmem.minCuSize = params.minCuSize;
    dmem.cabacZeroWordFlag = params.cabacZeroWord ? 1 : 0;
    dmem.bitDepthLuma = params.bitDepthLuma;
    dmem.bitDepthChroma = params.bitDepthChroma;
    dmem.chromaFormatIdc = params.chromaFormatIdc;
    dmem.totalSizeInCommandBuffer = params.totalSizeInCommandBuffer;
    dmem.offsetInCommandBuffer = params.offsetInCommandBuffer;
    dmem.lastTileBsStartInBytes = params.lastTileBsStartInBytes;
    dmem.sliceHeaderSizeInBits = params.sliceHeaderSizeInBits;
    dmem.picStateStartInBytes = params.picStateStartInBytes;
    dmem.stitchEnable = params.stitchEnable ? 1 : 0;
    dmem.stitchCommandOffset = 0;
    dmem.batchBufferEndForStitch = kBatchBufferEnd;

    dmem.tileSizeRecordOffset[0] = params.tileSizeRecordOffset0;
    for (uint32_t pipe = 0; pipe < params.numPipes && pipe < 4; ++pipe) {
        dmem.numTiles[pipe] = params.numTilesPerPipe;
        dmem.tileSizeRecordOffset[pipe + 1] =
            pipe * params.numTilesPerPipe * params.statsSizePerTile + params.tileSizeStatsOffset;
    }

    return dmem;
}

}
}
