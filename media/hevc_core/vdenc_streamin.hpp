// SPDX-License-Identifier: MIT
// Gen12 HEVC VDEnc stream-in records. Each 64-byte record describes one 32x32
// block for the VDEnc kernel: ROI, force intra/skip, QP, merge candidates and
// IME predictors. Layout follows media-driver's
// CODECHAL_VDENC_HEVC_STREAMIN_STATE_G12.
#ifndef WEX_MEDIA_HEVC_CORE_VDENC_STREAMIN_HPP
#define WEX_MEDIA_HEVC_CORE_VDENC_STREAMIN_HPP

#include <stdint.h>
#include <vector>

namespace HevcCore {
namespace VdEnc {

struct StreamInRecord {
    union {
        struct {
            uint32_t roiCtrl : 8;
            uint32_t maxTuSize : 2;
            uint32_t maxCuSize : 2;
            uint32_t numImePredictors : 4;
            uint32_t reserved0 : 5;
            uint32_t forceQpDelta : 1;
            uint32_t paletteDisable : 1;
            uint32_t reserved1 : 1;
            uint32_t puTypeCtrl : 8;
        };
        uint32_t dw0;
    };

    union {
        struct {
            uint32_t forceMvX : 16;
            uint32_t forceMvY : 16;
        };
        uint32_t dw;
    } forceMv[4];

    uint32_t dw5;

    union {
        struct {
            uint32_t forceRefIdx : 16;         // 4 bits per 16x16 block
            uint32_t numMergeCandidateCu8x8 : 4;
            uint32_t numMergeCandidateCu16x16 : 4;
            uint32_t numMergeCandidateCu32x32 : 4;
            uint32_t numMergeCandidateCu64x64 : 4;
        };
        uint32_t dw;
    } dw6;

    union {
        struct {
            uint32_t segId : 16;               // 4 bits per 16x16 block
            uint32_t qpEnable : 4;
            uint32_t segIdEnable : 1;
            uint32_t reserved : 2;
            uint32_t forceRefIdEnable : 1;
            uint32_t imePredictorSelect : 8;
        };
        uint32_t dw;
    } dw7;

    union {
        struct {
            uint32_t imePredictorMvX : 16;
            uint32_t imePredictorMvY : 16;
        };
        uint32_t dw;
    } imePredictorMv[4];

    union {
        struct {
            uint32_t imePredictorRefIdx : 16;  // 4 bits per 16x16 block
            uint32_t reserved : 16;
        };
        uint32_t dw;
    } dw12;

    union {
        struct {
            uint32_t panicModeLcuThreshold : 16;
            uint32_t reserved : 16;
        };
        uint32_t dw;
    } dw13;

    union {
        struct {
            uint32_t forceQp0 : 8;
            uint32_t forceQp1 : 8;
            uint32_t forceQp2 : 8;
            uint32_t forceQp3 : 8;
        };
        uint32_t dw;
    } dw14;

    uint32_t dw15;
};

static_assert(sizeof(StreamInRecord) == 64, "stream-in record must be 64 bytes");

// Per-picture controls written into every 32x32 record. The merge-candidate
// counts follow media-driver's TargetUsage tables (1/4 quality, 7 speed).
struct StreamInParams {
    bool intra {true};
    uint32_t targetUsage {4};
    uint32_t numImePredictors {};
    bool forceQp {false};
    uint32_t qpY {26};
};

// Stream-in data is one record per 32x32 block, padded to the CTB grid.
class StreamInBuffer {
public:
    StreamInBuffer(uint32_t width, uint32_t height);

    uint32_t widthInRecords() const { return widthInRecords_; }
    uint32_t heightInRecords() const { return heightInRecords_; }
    uint32_t recordCount() const { return static_cast<uint32_t>(records_.size()); }
    uint32_t byteSize() const { return recordCount() * 64; }
    const uint8_t *bytes() const {
        return reinterpret_cast<const uint8_t *>(records_.data());
    }

    StreamInRecord &at(uint32_t recordX, uint32_t recordY);
    const StreamInRecord &at(uint32_t recordX, uint32_t recordY) const;

    // Record index where a tile's stream-in data starts. The VDEnc tile layout
    // is four records per 64x64 CTB and column major across tiles, matching
    // media-driver's TileStreaminOffset calculation.
    uint32_t tileStartRecord(uint32_t tileStartLcuX, uint32_t tileStartLcuY,
        uint32_t widthInLcu, uint32_t tileHeightInLcu) const;
    uint32_t tileStartByteOffset(uint32_t tileStartLcuX, uint32_t tileStartLcuY,
        uint32_t widthInLcu, uint32_t tileHeightInLcu) const;

private:
    uint32_t widthInRecords_ {};
    uint32_t heightInRecords_ {};
    std::vector<StreamInRecord> records_;
};

void fillStreamIn(StreamInBuffer &buffer, const StreamInParams &params);

}
}

#endif
