// Gen12 BRC firmware ABI and state semantics, independent of native drivers.
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "../media/hevc_core/huc_brc_dmem.cpp"

int main() {
    using namespace HevcCore::Huc;
    // Offsets reproduced from the media-driver Gen12 structures.
    static_assert(offsetof(BrcInitDmem, FrameWidth_U16) == 50);
    static_assert(offsetof(BrcInitDmem, MinQP_U8) == 58);
    static_assert(offsetof(BrcInitDmem, DevThreshPB0_S8) == 68);
    static_assert(offsetof(BrcInitDmem, InitQPIP_U8) == 105);
    static_assert(offsetof(BrcInitDmem, ACQP_U32) == 144);
    static_assert(offsetof(BrcInitDmem, IsLowDelay_U8) == 173);
    static_assert(offsetof(BrcUpdateDmem, startGAdjFrame_U16) == 72);
    static_assert(offsetof(BrcUpdateDmem, SLB_Data_SizeInBytes) == 82);
    static_assert(offsetof(BrcUpdateDmem, PIC_STATE_StartInBytes) == 84);
    static_assert(offsetof(BrcUpdateDmem, CQP_QPValue_U8) == 111);
    static_assert(offsetof(BrcUpdateDmem, CurrentPass_U8) == 142);
    static_assert(offsetof(BrcUpdateDmem, SliceHeaderSize) == 184);

    BrcInitParams p;
    BrcInitDmem init {};
    assert(buildBrcInitDmem(p, init));
    assert(init.BRCFlag == 1 && init.TargetBitrate_U32 == 4000000);
    assert(init.BufSize_U32 == 8000000 && init.InitBufFull_U32 == 6000000);
    assert(init.FrameRateM_U32 == 30 && init.FrameRateD_U32 == 1);
    assert(init.FrameWidth_U16 == 1920 && init.FrameHeight_U16 == 1080);
    assert(init.GopP_U16 == 15 && init.GopB_U16 == 0 && init.MaxBRCLevel_U8 == 1);
    assert(init.InitQPIP_U8 == 30 && init.InitQPB_U8 == 34);
    assert(init.LumaBitDepth_U8 == 8 && init.ChromaBitDepth_U8 == 8);
    assert(init.IsLowDelay_U8 == 1 && init.CuQpCtrl_U8 == 0);
    assert(init.SlidingWindow_Size_U32 == 30 && init.SLIDINGWINDOW_MaxRateRatio == 120);
    assert(init.UserMaxFrame == 1920 * 1080);
    assert(init.ProfileLevelMaxFramePB_U32 == init.UserMaxFrame);
    p.levelIdc = 120;
    assert(buildBrcInitDmem(p, init) && init.UserMaxFrame == 835584);
    p.bitDepth = 10;
    assert(buildBrcInitDmem(p, init) && init.UserMaxFrame == 1044480);
    p.maximumFrameBytes = 500000;
    assert(buildBrcInitDmem(p, init) && init.UserMaxFrame == 500000);
    p.maximumFrameBytes = 0;
    p.levelIdc = 153;
    p.bitDepth = 8;
    p.frameRateNumerator = 60;
    assert(buildBrcInitDmem(p, init) && init.UserMaxFrame == 1671168);
    const auto savedLimit = init;
    p.levelIdc = 151;
    assert(!buildBrcInitDmem(p, init) && !memcmp(&init, &savedLimit, sizeof(init)));
    p.levelIdc = 153;
    p.frameRateNumerator = 30;
    p.mode = BrcMode::Vbr;
    p.maximumRate = 8000000;
    p.initialFullness = 9000000;
    p.bitDepth = 10;
    p.reset = true;
    assert(buildBrcInitDmem(p, init));
    assert(init.BRCFlag == 2 && init.MaxRate_U32 == 8000000);
    assert(init.InitBufFull_U32 == init.BufSize_U32);
    assert(init.LumaBitDepth_U8 == 10 && init.ChromaBitDepth_U8 == 10);
    assert(init.BRCFunc_U32 == 2);
    p.windowFrames = 150;
    p.windowMaximumRatio = 100;
    assert(buildBrcInitDmem(p, init));
    assert(init.SlidingWindow_Size_U32 == 150 && init.SLIDINGWINDOW_MaxRateRatio == 100);
    const auto savedWindow = init;
    p.windowMaximumRatio = 256; // reject byte-field overflow without truncation
    assert(!buildBrcInitDmem(p, init) && !memcmp(&init, &savedWindow, sizeof(init)));
    p.windowFrames = 0;
    assert(!buildBrcInitDmem(p, init));
    p.windowMaximumRatio = 0;
    p.mode = BrcMode::Acqp;
    p.initialQp = 26;
    p.cuQpControl = true;
    assert(buildBrcInitDmem(p, init));
    assert(init.BRCFlag == 0 && init.InitQPIP_U8 == 26 && init.InitQPB_U8 == 26);
    assert(init.CuQpCtrl_U8 == 3);
    const auto savedInit = init;
    p.width = 65536;
    assert(!buildBrcInitDmem(p, init) && !memcmp(&init, &savedInit, sizeof(init)));
    p.width = 1920;
    p.frameRateDenominator = 0;
    assert(!buildBrcInitDmem(p, init) && !memcmp(&init, &savedInit, sizeof(init)));
    p.frameRateDenominator = 1;
    p.vbvSize = 0;
    assert(!buildBrcInitDmem(p, init));

    BrcUpdateParams u;
    u.batchBytes = 500;
    u.cmd1Offset = 0;
    u.pictureOffset = 124;
    u.cmd2Offset = 288;
    u.frameId = 23;
    u.frameType = BrcFrameType::LowDelay;
    u.referenceFrameIdsL0[0] = u.referenceFrameIdsL1[0] = 22;
    u.referenceDataOffsets[0] = u.referenceDataOffsets[1] = 1024;
    u.sliceHeaderBits = 67;
    u.cuQpControl = true;
    BrcUpdateDmem update {};
    assert(buildBrcUpdateDmem(u, update));
    assert(update.FrameID_U32 == 23 && update.CurrentFrameType_U8 == 1);
    assert(update.Ref_L0_FrameID_U32[0] == 22 && update.Ref_L1_FrameID_U32[0] == 22);
    assert(update.CMD1_StartInBytes == 0 && update.PIC_STATE_StartInBytes == 124);
    assert(update.CMD2_StartInBytes == 288 && update.SLB_Data_SizeInBytes == 500);
    assert(update.PIPE_MODE_SELECT_StartInBytes == 0xffff);
    assert(update.OpMode_U8 == 1 && update.MaxNumPass_U8 == 2 && update.CurrentPass_U8 == 0);
    assert(update.CQP_QPValue_U8 == 26 && update.SliceHeaderSize == 67);
    assert(update.DeltaQPForSadZone0_S8 == -1 && update.DeltaQPForMvZero_S8 == 3);
    assert(update.ReEncodePositiveQPDeltaThr_S8 == 4 && update.ReEncodeNegativeQPDeltaThr_S8 == -5);
    assert(update.SlidingWindow_Enable_U8 == 0);
    u.slidingWindow = true;
    u.pass = 1;
    assert(buildBrcUpdateDmem(u, update) && update.CurrentPass_U8 == 1 &&
        update.SlidingWindow_Enable_U8 == 1);
    const auto savedUpdate = update;
    u.pictureDataOffset = 6 * 1024;
    assert(!buildBrcUpdateDmem(u, update) && !memcmp(&update, &savedUpdate, sizeof(update)));
    u.pictureDataOffset = 1;
    assert(!buildBrcUpdateDmem(u, update));
    u.pictureDataOffset = 0;
    u.referenceDataOffsets[0] = 1025;
    assert(!buildBrcUpdateDmem(u, update));
    u.referenceDataOffsets[0] = 6 * 1024;
    assert(!buildBrcUpdateDmem(u, update));
    u.referenceDataOffsets[0] = 1024;
    u.batchBytes = 491; // cuts off CMD2; never advertise an incomplete SLB
    assert(!buildBrcUpdateDmem(u, update) && !memcmp(&update, &savedUpdate, sizeof(update)));
    u.batchBytes = 500;
    u.pass = 2;
    assert(!buildBrcUpdateDmem(u, update));
    u.pass = 0;
    u.qp = 52;
    assert(!buildBrcUpdateDmem(u, update));
    puts("PASS Gen12 HuC BRC Init/Update DMEM ABI and state");
}
