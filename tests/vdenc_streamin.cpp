// CPU check for the Gen12 HEVC VDEnc stream-in records. Field packing follows
// media-driver's CODECHAL_VDENC_HEVC_STREAMIN_STATE_G12.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/hevc_core/vdenc_streamin.cpp"

int main() {
    using namespace HevcCore::VdEnc;

    static_assert(sizeof(StreamInRecord) == 64, "one 64-byte record per 32x32 block");

    StreamInBuffer picture(1920, 1080);
    assert(picture.widthInRecords() == 60);   // ceil(1920, 64) / 32
    assert(picture.heightInRecords() == 34);  // ceil(1080, 64) / 32
    assert(picture.recordCount() == 2040);
    assert(picture.byteSize() == 130560);

    StreamInBuffer padded(100, 50);
    assert(padded.widthInRecords() == 4);   // 128 pixels
    assert(padded.heightInRecords() == 2);  // 64 pixels
    assert(padded.recordCount() == 8);
    assert(padded.byteSize() == 512);

    for (uint32_t word = 0; word < 16; ++word)
        assert(reinterpret_cast<const uint32_t *>(&picture.at(7, 11))[word] == 0);

    StreamInRecord &record = picture.at(7, 11);
    record.roiCtrl = 3;
    record.maxTuSize = 2;
    record.maxCuSize = 1;
    record.numImePredictors = 7;
    record.forceQpDelta = 1;
    record.paletteDisable = 1;
    record.puTypeCtrl = 0x55;
    record.forceMv[0].forceMvX = 0x1234;
    record.forceMv[0].forceMvY = 0xABCD;
    record.dw6.forceRefIdx = 0x1234;
    record.dw6.numMergeCandidateCu8x8 = 1;
    record.dw6.numMergeCandidateCu64x64 = 4;
    record.dw7.segId = 0x5678;
    record.dw7.qpEnable = 9;
    record.dw7.segIdEnable = 1;
    record.dw7.forceRefIdEnable = 1;
    record.dw7.imePredictorSelect = 0x77;
    record.imePredictorMv[3].imePredictorMvX = 0x2222;
    record.dw12.imePredictorRefIdx = 0x3333;
    record.dw13.panicModeLcuThreshold = 0x4444;
    record.dw14.forceQp0 = 11;
    record.dw14.forceQp3 = 44;

    const uint32_t *words = reinterpret_cast<const uint32_t *>(&record);
    assert(words[0] == 0x55607603);
    assert(words[1] == 0xABCD1234);  // force MV 0
    assert(words[6] == 0x40011234);  // force ref idx and merge candidates
    assert(words[7] == 0x77995678);  // segment, QP enable and IME select
    assert(words[11] == 0x00002222); // IME predictor MV 3
    assert(words[12] == 0x00003333);
    assert(words[13] == 0x00004444);
    assert(words[14] == 0x2C00000B); // forced QP 0 and 3
    assert(words[15] == 0);

    // Default intra fill: max TU/CU 3, force-intra control and the quality
    // merge-candidate table (8x8=1, 16x16=2, 32x32=3, 64x64=4).
    StreamInBuffer intra(100, 50);
    StreamInParams intraParams;
    fillStreamIn(intra, intraParams);
    for (uint32_t y = 0; y < intra.heightInRecords(); ++y) {
        for (uint32_t x = 0; x < intra.widthInRecords(); ++x) {
            const StreamInRecord &r = intra.at(x, y);
            assert(r.dw0 == 0x01000F00);
            assert(r.dw6.dw == 0x43210000);
            assert(r.dw7.dw == 0);
        }
    }

    // TargetUsage 7 uses the speed table.
    StreamInParams speed;
    speed.targetUsage = 7;
    fillStreamIn(intra, speed);
    assert(intra.at(0, 0).dw6.dw == 0x22200000);

    // Forced-QP records only carry DW7/DW14.
    StreamInParams forced;
    forced.forceQp = true;
    forced.qpY = 30;
    fillStreamIn(intra, forced);
    assert(intra.at(0, 0).dw0 == 0);
    assert(intra.at(0, 0).dw7.qpEnable == 0xF);
    assert(intra.at(0, 0).dw14.forceQp0 == 30);
    assert(intra.at(0, 0).dw14.forceQp3 == 30);
    assert(intra.at(0, 0).dw6.dw == 0);

    // Four records per 64x64 CTB; the tile base is column major across tiles.
    assert(picture.tileStartRecord(0, 0, 30, 17) == 0);
    assert(picture.tileStartRecord(15, 0, 30, 17) == 1020);
    assert(picture.tileStartByteOffset(15, 0, 30, 17) == 65280);
    assert(picture.tileStartRecord(0, 17, 30, 17) == 2040);
    assert(picture.tileStartByteOffset(0, 17, 30, 17) == 130560);

    puts("PASS Gen12 HEVC VDEnc stream-in records");
    return 0;
}
