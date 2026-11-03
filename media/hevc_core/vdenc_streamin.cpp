// SPDX-License-Identifier: MIT
#include "vdenc_streamin.hpp"

#include <assert.h>

namespace HevcCore {
namespace VdEnc {
namespace {

uint32_t alignCeil(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

}

StreamInBuffer::StreamInBuffer(uint32_t width, uint32_t height)
    : widthInRecords_(alignCeil(width, 64) / 32)
    , heightInRecords_(alignCeil(height, 64) / 32)
    , records_(static_cast<size_t>(widthInRecords_) * heightInRecords_) {}

StreamInRecord &StreamInBuffer::at(uint32_t recordX, uint32_t recordY) {
    assert(recordX < widthInRecords_ && recordY < heightInRecords_);
    return records_[static_cast<size_t>(recordY) * widthInRecords_ + recordX];
}

const StreamInRecord &StreamInBuffer::at(uint32_t recordX, uint32_t recordY) const {
    assert(recordX < widthInRecords_ && recordY < heightInRecords_);
    return records_[static_cast<size_t>(recordY) * widthInRecords_ + recordX];
}

uint32_t StreamInBuffer::tileStartRecord(uint32_t tileStartLcuX, uint32_t tileStartLcuY,
    uint32_t widthInLcu, uint32_t tileHeightInLcu) const {
    // Four 32x32 records per 64x64 CTB.
    return 4 * (tileStartLcuY * widthInLcu + tileStartLcuX * tileHeightInLcu);
}

uint32_t StreamInBuffer::tileStartByteOffset(uint32_t tileStartLcuX, uint32_t tileStartLcuY,
    uint32_t widthInLcu, uint32_t tileHeightInLcu) const {
    return tileStartRecord(tileStartLcuX, tileStartLcuY, widthInLcu, tileHeightInLcu) * 64;
}

void fillStreamIn(StreamInBuffer &buffer, const StreamInParams &params) {
    uint32_t merge8 = 1;
    uint32_t merge16 = 2;
    uint32_t merge32 = 3;
    uint32_t merge64 = 4;
    if (params.targetUsage == 7) {
        merge8 = 0;
        merge16 = 2;
        merge32 = 2;
        merge64 = 2;
    } else if (params.targetUsage != 1 && params.targetUsage != 4) {
        merge8 = 0;
        merge16 = 1;
        merge32 = 2;
        merge64 = 3;
    }

    for (uint32_t y = 0; y < buffer.heightInRecords(); ++y) {
        for (uint32_t x = 0; x < buffer.widthInRecords(); ++x) {
            StreamInRecord &record = buffer.at(x, y);
            record = StreamInRecord {};
            if (params.forceQp) {
                // Per-32x32 force QP: the kernel applies the four values to
                // the four 16x16 sub-blocks of the record.
                record.dw7.qpEnable = 0xF;
                record.dw14.forceQp0 = params.qpY & 0xFF;
                record.dw14.forceQp1 = params.qpY & 0xFF;
                record.dw14.forceQp2 = params.qpY & 0xFF;
                record.dw14.forceQp3 = params.qpY & 0xFF;
            } else {
                record.maxTuSize = 3;
                record.maxCuSize = 3;
                record.numImePredictors = params.numImePredictors & 0xF;
                record.puTypeCtrl = params.intra ? 1 : 0;
                record.dw6.numMergeCandidateCu8x8 = merge8;
                record.dw6.numMergeCandidateCu16x16 = merge16;
                record.dw6.numMergeCandidateCu32x32 = merge32;
                record.dw6.numMergeCandidateCu64x64 = merge64;
            }
        }
    }
}

}
}
