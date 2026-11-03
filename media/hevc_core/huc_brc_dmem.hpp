// SPDX-License-Identifier: MIT
/*
* Copyright (c) 2017-2019, Intel Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/
//!
//! \file     codechal_vdenc_hevc_g12.h
//! \brief    HEVC VDEnc encoder for GEN12 platform.
//!

#ifndef WEX_MEDIA_HEVC_CORE_HUC_BRC_DMEM_HPP
#define WEX_MEDIA_HEVC_CORE_HUC_BRC_DMEM_HPP
#include <stdint.h>
#include <stddef.h>
namespace HevcCore {
namespace Huc {
// Gen12 SetDmemHuCBrcUpdate uses six separate 256-DWORD picture-data slots.
constexpr uint32_t kBrcPictureDataBytes = 1024;
constexpr uint32_t kBrcPictureDataSlots = 6;
struct BrcInitDmem
{
    uint32_t    BRCFunc_U32;                  // 0: Init; 2: Reset, bit7 0: frame-based; 1: tile-based
    uint32_t    UserMaxFrame;                 // ProfileLevelMaxFrame_U32
    uint32_t    InitBufFull_U32;
    uint32_t    BufSize_U32;
    uint32_t    TargetBitrate_U32;
    uint32_t    MaxRate_U32;
    uint32_t    MinRate_U32;
    uint32_t    FrameRateM_U32;
    uint32_t    FrameRateD_U32;
    uint32_t    LumaLog2WeightDenom_U32;
    uint32_t    ChromaLog2WeightDenom_U32;
    uint8_t     BRCFlag : 7;         // ACQP/ICQ=0, CBR=1, VBR=2, VCM=3, LOWDELAY=4
    uint8_t     SSCFlag : 1;         // SSC: 0x80
    uint8_t     Reserved;
    uint16_t    GopP_U16;
    uint16_t    GopB_U16;
    uint16_t    FrameWidth_U16;
    uint16_t    FrameHeight_U16;
    uint16_t    GopB1_U16;
    uint16_t    GopB2_U16;
    uint8_t     MinQP_U8;
    uint8_t     MaxQP_U8;
    uint8_t     MaxBRCLevel_U8;
    uint8_t     LumaBitDepth_U8;
    uint8_t     ChromaBitDepth_U8;
    uint8_t     CuQpCtrl_U8;        // 0=No CUQP; 1=CUQP for I-frame; 2=CUQP for P/B-frame

    uint8_t     RSVD0[4];
    int8_t      DevThreshPB0_S8[8];
    int8_t      DevThreshVBR0_S8[8];
    int8_t      DevThreshI0_S8[8];
    int8_t      InstRateThreshP0_S8[4];
    int8_t      InstRateThreshB0_S8[4];
    int8_t      InstRateThreshI0_S8[4];
    uint8_t     LowDelayMode_U8;
    uint8_t     InitQPIP_U8;
    uint8_t     InitQPB_U8;                    // In CQP mode, InitQPB_U8= InitQPIP_U8
    uint8_t     QPDeltaThrForAdapt2Pass_U8;
    uint8_t     TopFrmSzThrForAdapt2Pass_U8;
    uint8_t     BotFrmSzThrForAdapt2Pass_U8;
    uint8_t     QPSelectForFirstPass_U8;
    uint8_t     MBHeaderCompensation_U8;
    uint8_t     OverShootCarryFlag_U8;
    uint8_t     OverShootSkipFramePct_U8;
    uint8_t     EstRateThreshP0_U8[7];
    uint8_t     EstRateThreshB0_U8[7];

    uint8_t     EstRateThreshI0_U8[7];
    uint8_t     QPP_U8;
    uint8_t     StreamInSurfaceEnable_U8;           // 0-disabled, 1-enabled
    uint8_t     StreamInROIEnable_U8;               // 0-disabled, 1-enabled
    uint8_t     TimingBudget_Enable_U8;             // 0-disabled, 1-enabled
    uint8_t     TopQPDeltaThrForAdapt2Pass_U8;      // 2
    uint8_t     BotQPDeltaThrForAdapt2Pass_U8;      // 1
    uint8_t     RESERVED;
    uint8_t     NetworkTraceEnable_U8;                  // 0-disabled, 1-enabled
    uint8_t     LowDelaySceneChangeXFrameSizeEnable_U8; // 0-disabled, 1-enabled
    uint32_t    ACQP_U32;                               // 1
    uint32_t    SlidingWindow_Size_U32;                 // 30

    uint8_t     SLIDINGWINDOW_MaxRateRatio;
    uint8_t     LookaheadDepth_U8;
    int8_t      CbQPOffset;
    int8_t      CrQPOffset;

    uint32_t    ProfileLevelMaxFramePB_U32;

    // tile-based BRC
    uint16_t    SlideWindowRC;                          // Reserved now
    uint16_t    MaxLogCUSize;

    uint16_t    FrameWidthInLCU;
    uint16_t    FrameHeightInLCU;

    uint8_t     BRCPyramidEnable_U8;
    uint8_t     LongTermRefEnable_U8;
    uint16_t    LongTermRefInterval_U16;
    uint8_t     LongTermRefMsdk_U8;
    uint8_t     IsLowDelay_U8;
    uint16_t    RSVD3;
    uint32_t    RSVD1[4];                               // 64 bytes aligned
};
static_assert(sizeof(BrcInitDmem) == 192, "Gen12 BRC DMEM ABI");

struct BrcUpdateDmem
{
    uint32_t    TARGETSIZE_U32;
    uint32_t    FrameID_U32;                    // frame number
    uint32_t    Ref_L0_FrameID_U32[8];
    uint32_t    Ref_L1_FrameID_U32[8];
    uint16_t    startGAdjFrame_U16[4];          // 10, 50, 100, 150
    uint16_t    TargetSliceSize_U16;
    uint16_t    SLB_Data_SizeInBytes;
    uint16_t    PIC_STATE_StartInBytes;         // PIC_STATE starts in byte. 0xFFFF means not available in SLB
    uint16_t    CMD2_StartInBytes;
    uint16_t    CMD1_StartInBytes;
    uint16_t    PIPE_MODE_SELECT_StartInBytes;  // PIPE Mode select starts in byte. 0xFFFF means not available in SLB
    uint16_t    Current_Data_Offset;            // Data block offset of current picture from beginning of the data buffer (region 9)
    uint16_t    Ref_Data_Offset[5];             // Data block offset of ref pictures from beginning of the data buffer (region 9)
    uint16_t    MaxNumSliceAllowed_U16;
    uint8_t     OpMode_U8;                      // 1: frame-based BRC (including ACQP), 2: Weighted prediction, Weighted prediction should not be enabled in first pass.
                                                // Same as other common flags, this is a bit operation. Each bit is zero for disabling and 1 for enabling. i.e. 01: BRC, 10: WP - never used; 11: BRC+WP, 4: tile-based BRC (frame level), 8: tile-based BRC (tile level)
    uint8_t     CurrentFrameType_U8;
    uint8_t     Num_Ref_L0_U8;
    uint8_t     Num_Ref_L1_U8;
    uint8_t     Num_Slices;
    uint8_t     CQP_QPValue_U8;                 // CQP QP value (needed for ICQ and ACQP)
    uint8_t     CQP_FracQP_U8;
    uint8_t     MaxNumPass_U8;                  // max number of BRC passes (SAO second pass is not included.)
    uint8_t     gRateRatioThreshold_U8[7];
    uint8_t     startGAdjMult_U8[5];
    uint8_t     startGAdjDiv_U8[5];
    uint8_t     gRateRatioThresholdQP_U8[8];
    uint8_t     SceneChgPrevIntraPctThreshold_U8;
    uint8_t     SceneChgCurIntraPctThreshold_U8;
    uint8_t     IPAverageCoeff_U8;
    uint8_t     CurrentPass_U8;
    int8_t      DeltaQPForMvZero_S8;
    int8_t      DeltaQPForMvZone0_S8;
    int8_t      DeltaQPForMvZone1_S8;
    int8_t      DeltaQPForMvZone2_S8;
    int8_t      DeltaQPForSadZone0_S8;
    int8_t      DeltaQPForSadZone1_S8;
    int8_t      DeltaQPForSadZone2_S8;
    int8_t      DeltaQPForSadZone3_S8;
    int8_t      DeltaQPForROI0_S8;
    int8_t      DeltaQPForROI1_S8;
    int8_t      DeltaQPForROI2_S8;
    int8_t      DeltaQPForROI3_S8;
    int8_t      LumaLog2WeightDenom_S8;     // default: 6
    int8_t      ChromaLog2WeightDenom_S8;   // default: 6
    uint8_t     DisabledFeature_U8;
    uint8_t     SlidingWindow_Enable_U8;    // 0-disabled, 1-enabled
    uint8_t     LOG_LCU_Size_U8;            // 6
    uint16_t    NetworkTraceEntry_U16;              // default: 0
    uint16_t    LowDelaySceneChangeXFrameSize_U16;  // default: 0
    int8_t      ReEncodePositiveQPDeltaThr_S8;      // default: 4
    int8_t      ReEncodeNegativeQPDeltaThr_S8;      // default: -10

    // tile-based BRC
    uint8_t     MaxNumTileHuCCallMinus1;            // maximal tile row
    uint8_t     TileHucCallIndex;
    uint8_t     TileHuCCallPassIndex;               // Start from 1
    uint8_t     TileHuCCallPassMax;                 // Reserved now
    uint16_t    TileSizeInLCU;

    uint32_t    TxSizeInBitsPerFrame;

    uint8_t     StartTileIdx;
    uint8_t     EndTileIdx;

    uint16_t    NumFrameSkipped;
    uint32_t    SkipFrameSize;

    uint32_t    SliceHeaderSize;
    uint8_t     IsLongTermRef;

    uint8_t    EnableMotionAdaptive;
    uint8_t    EnableLookAhead;
    uint8_t    UPD_CQMEnabled_U8; // 0 indicates CQM is disabled for current frame; otherwise CQM is enabled
};
static_assert(sizeof(BrcUpdateDmem) == 192, "Gen12 BRC DMEM ABI");

enum class BrcMode : uint8_t { Acqp = 0, Cbr = 1, Vbr = 2 };
enum class BrcFrameType : uint8_t { Intra = 0, LowDelay = 1, B = 2, B1 = 3, B2 = 4 };

struct BrcInitParams {
    BrcMode mode {BrcMode::Cbr};
    uint32_t width {1920}, height {1080};
    uint32_t targetRate {4000000}, maximumRate {4000000}; // bits/second
    uint32_t vbvSize {8000000}, initialFullness {6000000}; // bits
    uint32_t frameRateNumerator {30}, frameRateDenominator {1};
    uint32_t windowFrames {0}, windowMaximumRatio {0}; // optional percentage
    // Intel GetProfileLevelMaxFrameSize supplies bytes here, whereas VBV and
    // target fullness use bits. Zero selects the profile/level limit.
    uint32_t maximumFrameBytes {0}, levelIdc {153};
    uint32_t gopSize {16}, referenceDistance {1};
    uint8_t bitDepth {8}, minimumQp {10}, maximumQp {51}, initialQp {26};
    bool cuQpControl {false}, lowDelay {true}, reset {false};
};

struct BrcUpdateParams {
    uint32_t targetFullness {6000000}, frameId {0};
    BrcFrameType frameType {BrcFrameType::Intra};
    uint16_t batchBytes {0}, cmd1Offset {0}, pictureOffset {0}, cmd2Offset {0};
    uint16_t pictureDataOffset {0}, referenceDataOffsets[5] {};
    uint32_t referenceFrameIdsL0[8] {}, referenceFrameIdsL1[8] {};
    uint16_t targetSliceBytes {0}, maximumSlices {200};
    uint8_t referencesL0 {1}, referencesL1 {1}, slices {1};
    uint8_t qp {26}, pass {0}, maximumPasses {2};
    uint32_t sliceHeaderBits {0};
    bool cuQpControl {false};
    bool slidingWindow {false};
};

// The packet ABI stays independent of native driver objects and resources.
// Invalid geometry/rates/command ranges leave the caller's output unchanged.
bool buildBrcInitDmem(const BrcInitParams &params, BrcInitDmem &out);
bool buildBrcUpdateDmem(const BrcUpdateParams &params, BrcUpdateDmem &out);

}
}
#endif
