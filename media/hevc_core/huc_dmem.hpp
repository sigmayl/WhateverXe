// SPDX-License-Identifier: MIT
// Gen12 HuC DMEM layout used by the HEVC VDEnc PAK-integration kernel.
// Field order and sentinel values follow media-driver's
// HucPakStitchDmemVdencG12.
#ifndef WEX_MEDIA_HEVC_CORE_HUC_DMEM_HPP
#define WEX_MEDIA_HEVC_CORE_HUC_DMEM_HPP

#include <stdint.h>

namespace HevcCore {
namespace Huc {

// Offset entry the kernel treats as unavailable.
constexpr uint32_t kOffsetUnavailable = 0xFFFFFFFF;
constexpr uint32_t kBatchBufferEnd = 0x05000000;

#pragma pack(push, 1)
struct PakStitchDmem {
    uint32_t tileSizeRecordOffset[5];
    uint32_t vdencStatOffset[5];
    uint32_t hevcPakStatOffset[5];
    uint32_t hevcStreamoutOffset[5];
    uint32_t vp9PakStatOffset[5];
    uint32_t vp9CounterBufferOffset[5];
    uint32_t lastTileBsStartInBytes;
    uint32_t sliceHeaderSizeInBits;
    uint16_t totalSizeInCommandBuffer;
    uint16_t offsetInCommandBuffer;
    uint16_t pictureWidth;
    uint16_t pictureHeight;
    uint16_t totalNumberOfPaks;
    uint16_t numSlices[4];
    uint16_t numTiles[4];
    uint16_t picStateStartInBytes;
    uint8_t codec;            // 1 HEVC DP, 2 HEVC VDEnc, 3 VP9 VDEnc
    uint8_t maxPass;
    uint8_t currentPass;
    uint8_t minCuSize;        // 3: 8x8, 4: 16x16
    uint8_t cabacZeroWordFlag;
    uint8_t bitDepthLuma;
    uint8_t bitDepthChroma;
    uint8_t chromaFormatIdc;
    uint8_t currentFrameBrcLevel;
    uint8_t brcUnderflowEnable;
    uint8_t stitchEnable;
    uint8_t reserved1;
    uint16_t stitchCommandOffset;
    uint16_t reserved2;
    uint32_t batchBufferEndForStitch;
    uint8_t reserved[16];
};
#pragma pack(pop)

static_assert(sizeof(PakStitchDmem) == 192, "HuC PAK-stitch DMEM must be 192 bytes");

enum class Codec : uint8_t {
    HevcDp = 1,
    HevcVdEnc = 2,
    Vp9VdEnc = 3,
};

struct PakStitchParams {
    uint16_t pictureWidth {};
    uint16_t pictureHeight {};
    uint16_t numPipes {1};
    uint16_t numTilesPerPipe {};
    uint32_t tileSizeRecordOffset0 {};
    uint32_t tileSizeStatsOffset {};
    uint32_t statsSizePerTile {};
    uint32_t lastTileBsStartInBytes {};
    uint32_t sliceHeaderSizeInBits {};
    uint16_t totalSizeInCommandBuffer {};
    uint16_t offsetInCommandBuffer {};
    uint16_t picStateStartInBytes {};
    uint8_t minCuSize {3};
    uint8_t bitDepthLuma {8};
    uint8_t bitDepthChroma {8};
    uint8_t chromaFormatIdc {1};
    uint8_t maxPass {1};
    uint8_t currentPass {1};
    Codec codec {Codec::HevcVdEnc};
    bool cabacZeroWord {false};
    bool stitchEnable {true};
};

PakStitchDmem pakStitchDmem(const PakStitchParams &params);

}
}

#endif
