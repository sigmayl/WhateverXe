// SPDX-License-Identifier: MIT
/*
* Copyright (c) 2017-2021, Intel Corporation
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
// Intel media-driver f55f1c8ea764453553d1cc5add705a32cdca712d.
// Gen12 HuC BRC constant-data wire format; offsets are relative to the SLB.
#ifndef WEX_MEDIA_HEVC_CORE_HUC_BRC_CONST_HPP
#define WEX_MEDIA_HEVC_CORE_HUC_BRC_CONST_HPP
#include <stdint.h>
#include <stddef.h>
namespace HevcCore {
namespace Huc {
constexpr uint32_t kBrcMaximumSlices = 70;
struct BrcConstantData
{
    uint16_t    SADQPLambdaI[52];
    uint16_t    SADQPLambdaP[52];
    uint16_t    RDQPLambdaI[52];
    uint16_t    RDQPLambdaP[52];
    uint16_t    SLCSZ_THRDELTAI_U16[52];
    uint16_t    SLCSZ_THRDELTAP_U16[52];
    uint8_t     DistThreshldI[9];
    uint8_t     DistThreshldP[9];
    uint8_t     DistThreshldB[9];
    int8_t      DistQPAdjTabI[81];
    int8_t      DistQPAdjTabP[81];
    int8_t      DistQPAdjTabB[81];
    int8_t      FrmSzAdjTabI_S8[72];
    int8_t      FrmSzAdjTabP_S8[72];
    int8_t      FrmSzAdjTabB_S8[72];
    uint8_t     FrmSzMaxTabI[9];
    uint8_t     FrmSzMaxTabP[9];
    uint8_t     FrmSzMaxTabB[9];
    uint8_t     FrmSzMinTabI[9];
    uint8_t     FrmSzMinTabP[9];
    uint8_t     FrmSzMinTabB[9];
    int8_t      QPAdjTabI[45];
    int8_t      QPAdjTabP[45];
    int8_t      QPAdjTabB[45];
    struct
    {
        uint8_t     I_INTRA_64X64DC;    // added later since I frame needs to be setup differently
        uint8_t     I_INTRA_32x32;
        uint8_t     I_INTRA_16x16;
        uint8_t     I_INTRA_8x8;
        uint8_t     I_INTRA_SADMPM;
        uint8_t     I_INTRA_RDEMPM;
        uint8_t     I_INTRA_NxN;
        uint8_t     INTRA_64X64DC;
        uint8_t     INTRA_32x32;
        uint8_t     INTRA_16x16;
        uint8_t     INTRA_8x8;
        uint8_t     INTRA_SADMPM;
        uint8_t     INTRA_RDEMPM;
        uint8_t     INTRA_NxN;
        uint8_t     INTER_32x32;
        uint8_t     INTER_32x16;
        uint8_t     INTER_16x16;
        uint8_t     INTER_16x8;
        uint8_t     INTER_8x8;
        uint8_t     REF_ID;
        uint8_t     MERGE_64X64;
        uint8_t     MERGE_32X32;
        uint8_t     MERGE_16x16;
        uint8_t     MERGE_8x8;
        uint8_t     SKIP_64X64;
        uint8_t     SKIP_32X32;
        uint8_t     SKIP_16x16;
        uint8_t     SKIP_8x8;
    } ModeCosts[52];
    struct
    {
        // Unit in Bytes
        uint16_t    SizeOfCMDs;
        uint16_t    HcpWeightOffsetL0_StartInBytes;         // HCP_WEIGHTOFFSET_L0 starts in bytes from beginning of the SLB. 0xFFFF means unavailable in SLB
        uint16_t    HcpWeightOffsetL1_StartInBytes;         // HCP_WEIGHTOFFSET_L1 starts in bytes from beginning of the SLB. 0xFFFF means unavailable in SLB
        uint16_t    SliceState_StartInBytes;
        uint16_t    SliceHeaderPIO_StartInBytes;
        uint16_t    VdencWeightOffset_StartInBytes;
        // Unit in Bits
        uint16_t    SliceHeader_SizeInBits;
        uint16_t    WeightTable_StartInBits;                // number of bits from beginning of slice header for weight table first bit, 0xffff means not awailable
        uint16_t    WeightTable_EndInBits;                  // number of bits from beginning of slice header for weight table last bit, 0xffff means not awailable
    } Slice[kBrcMaximumSlices];
    uint8_t     PenaltyForIntraNonDC32x32PredMode[52];
    uint32_t   UPD_TR_TargetSize_U32; //TR_BRC
    uint32_t   UPD_LA_TargetFulness_U32; //LOOK_AHEAD
    uint8_t    UPD_deltaQP;
    uint8_t    UPD_TCBRC_SCENARIO_U8;  // Unite scenario is 1, other is 0
    uint8_t    UPD_ROM_CURRENT_U8;     // ROM average of current frame
    uint8_t    UPD_ROM_ZERO_U8;        // ROM zero percentage (255 is 100%)
};

static_assert(sizeof(BrcConstantData) == 4080, "Gen12 BRC constant-data ABI");
static_assert(offsetof(BrcConstantData, ModeCosts) == 1299, "BRC mode-cost offset");
static_assert(offsetof(BrcConstantData, Slice) == 2756, "BRC slice-layout offset");
static_assert(offsetof(BrcConstantData, PenaltyForIntraNonDC32x32PredMode) == 4016,
    "BRC penalty-table offset");

struct BrcSliceLayout {
    uint32_t startOffset {}, commandsBytes {};
    uint32_t sliceStateOffset {}, sliceHeaderOffset {}, vdencWeightsOffset {};
    uint32_t sliceHeaderBits {};
};
struct BrcConstantParams {
    bool inter {false};
    uint32_t batchBytes {}, sliceCount {1};
    const BrcSliceLayout *slices {};
};

// Populates Intel's default lambdas/mode costs and the admitted slice layouts.
// Invalid offsets leave the caller's output unchanged. Weighted prediction,
// tile replay and low-delay BRC tolerance are outside this builder's scope.
bool buildBrcConstantData(const BrcConstantParams &params, BrcConstantData &out);
}
}
#endif
