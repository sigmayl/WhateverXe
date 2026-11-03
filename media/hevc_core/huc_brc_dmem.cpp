// SPDX-License-Identifier: MIT
// Gen12 BRC state follows Intel media-driver's SetDmemHuCBrcInitReset,
// SetDmemHuCBrcUpdate and ComputeVDEncInitQP.
#include "huc_brc_dmem.hpp"

extern "C" double pow(double, double) noexcept;
extern "C" double log10(double) noexcept;

namespace HevcCore {
namespace Huc {
namespace {

int32_t clip(int32_t low, int32_t high, int32_t value) {
    return value < low ? low : value > high ? high : value;
}

void initialQps(const BrcInitParams &p, uint8_t &ip, uint8_t &b) {
    const uint32_t frameBytes = (p.width * p.height * 3) >> 1;
    const float x0 = 0, y0 = 1.19f, x1 = 1.75f, y1 = 1.75f;
    const double samplesPerBit = frameBytes * 2. / 3. *
        (float(p.frameRateNumerator) /
            (float(p.frameRateDenominator) * float(p.targetRate)));
    int32_t qip = int32_t(1. / 1.2 * pow(10.,
        (log10(samplesPerBit) - x0) * (y1 - y0) / (x1 - x0) + y0) + 0.5) + 2;
    if (p.gopSize == 1)
        qip += 12;
    else if (p.gopSize < 15)
        qip += (14 - p.gopSize) >> 1;
    qip = clip(p.minimumQp, p.maximumQp, qip) - 1;
    if (qip < 0)
        qip = 1;
    int32_t qb = clip(p.minimumQp, p.maximumQp,
        ((qip + qip) * 563 >> 10) + 1);
    const int32_t adjustment = p.gopSize > 300 ? 8 : 2;
    ip = uint8_t(clip(p.minimumQp, p.maximumQp, qip - adjustment));
    b = uint8_t(clip(p.minimumQp, p.maximumQp, qb - adjustment));
}

template <typename T, unsigned N>
void copy(T (&out)[N], const T (&in)[N]) {
    for (unsigned i = 0; i < N; ++i)
        out[i] = in[i];
}

bool maximumFrameBytes(const BrcInitParams &p, uint32_t &out) {
    // GetMaxMBPS / GetProfileLevelMaxFrameSize from Intel's HEVC base class.
    // Its byte-per-picture cap is independent of the bit-based VBV budget.
    uint32_t samplesPerSecond = 0, bytesPerPicture = 0;
    switch (p.levelIdc) {
        case 30: samplesPerSecond = 552960; bytesPerPicture = 36864; break;
        case 60: samplesPerSecond = 3686400; bytesPerPicture = 122880; break;
        case 63: samplesPerSecond = 7372800; bytesPerPicture = 245760; break;
        case 90: samplesPerSecond = 16588800; bytesPerPicture = 552760; break;
        case 93: samplesPerSecond = 33177600; bytesPerPicture = 983040; break;
        case 120: samplesPerSecond = 66846720; bytesPerPicture = 2228224; break;
        case 123: samplesPerSecond = 133693440; bytesPerPicture = 2228224; break;
        case 150: samplesPerSecond = 267386880; bytesPerPicture = 8912896; break;
        case 153: samplesPerSecond = 534773760; bytesPerPicture = 8912896; break;
        case 156: samplesPerSecond = 1069547520; bytesPerPicture = 8912896; break;
        case 180: samplesPerSecond = 1069547520; bytesPerPicture = 35651584; break;
        case 183: samplesPerSecond = 2139095040; bytesPerPicture = 35651584; break;
        case 186: samplesPerSecond = 4278190080u; bytesPerPicture = 35651584; break;
        default: return false;
    }
    const uint32_t compression = p.levelIdc == 150 || p.levelIdc == 186 ? 6 :
        p.levelIdc > 150 ? 8 : p.levelIdc > 93 ? 4 : 2;
    const double factor = (p.bitDepth == 10 ? 1.875 : 1.5) / compression;
    const double rateLimit = double(samplesPerSecond) * p.frameRateDenominator /
        p.frameRateNumerator * factor;
    uint32_t limit = p.width * p.height;
    if (bytesPerPicture < limit) limit = bytesPerPicture;
    if (rateLimit < limit) limit = uint32_t(rateLimit);
    if (p.maximumFrameBytes && p.maximumFrameBytes < limit) limit = p.maximumFrameBytes;
    if (!limit) return false;
    out = limit;
    return true;
}

}

bool buildBrcInitDmem(const BrcInitParams &p, BrcInitDmem &out) {
    if (!p.width || !p.height || p.width > 65535 || p.height > 65535 ||
        (p.width & 1) || (p.height & 1) || uint64_t(p.width) * p.height * 3 > 0xffffffffu ||
        !p.frameRateNumerator || !p.frameRateDenominator || !p.vbvSize ||
        !p.gopSize || p.gopSize > 4001 || !p.referenceDistance ||
        p.referenceDistance > 16 || p.minimumQp < 10 || p.maximumQp > 51 ||
        p.minimumQp > p.maximumQp || p.initialQp > 51 ||
        (p.bitDepth != 8 && p.bitDepth != 10) || uint8_t(p.mode) > 2 ||
        (p.mode != BrcMode::Acqp && (!p.targetRate || p.maximumRate < p.targetRate)) ||
        (p.windowFrames && (!p.targetRate || p.windowFrames > 256 ||
            p.windowMaximumRatio < 100 || p.windowMaximumRatio > 255)) ||
        (!p.windowFrames && p.windowMaximumRatio))
        return false;
    uint32_t frameBytes;
    if (!maximumFrameBytes(p, frameBytes))
        return false;
    BrcInitDmem d {};
    d.BRCFunc_U32 = p.reset ? 2 : 0;
    d.UserMaxFrame = d.ProfileLevelMaxFramePB_U32 = frameBytes;
    d.InitBufFull_U32 = p.initialFullness < p.vbvSize ? p.initialFullness : p.vbvSize;
    d.BufSize_U32 = p.vbvSize;
    d.TargetBitrate_U32 = p.targetRate;
    d.MaxRate_U32 = p.maximumRate;
    d.FrameRateM_U32 = p.frameRateNumerator;
    d.FrameRateD_U32 = p.frameRateDenominator;
    d.BRCFlag = uint8_t(p.mode);
    d.FrameWidth_U16 = uint16_t(p.width);
    d.FrameHeight_U16 = uint16_t(p.height);
    d.MinQP_U8 = p.minimumQp;
    d.MaxQP_U8 = p.maximumQp;
    d.CuQpCtrl_U8 = p.cuQpControl ? 3 : 0;
    const uint32_t period = ((p.gopSize - 1 + p.referenceDistance - 1) /
        p.referenceDistance) * p.referenceDistance;
    d.GopP_U16 = uint16_t(period / p.referenceDistance);
    d.GopB_U16 = uint16_t(period - d.GopP_U16);
    d.MaxBRCLevel_U8 = d.GopB_U16 ? 2 : 1;
    d.LumaBitDepth_U8 = d.ChromaBitDepth_U8 = p.bitDepth;
    const uint64_t inputBits = uint64_t(p.maximumRate) *
        p.frameRateDenominator / p.frameRateNumerator;
    double ratio = inputBits / (p.vbvSize / 30.);
    ratio = ratio < 0.1 ? 0.1 : ratio > 3.5 ? 3.5 : ratio;
    static const double pbNegative[4] = {0.90, 0.66, 0.46, 0.3};
    static const double pbPositive[4] = {0.3, 0.46, 0.70, 0.90};
    static const double iNegative[4] = {0.80, 0.60, 0.34, 0.2};
    static const double iPositive[4] = {0.2, 0.4, 0.66, 0.90};
    static const double vbrNegative[4] = {0.90, 0.70, 0.50, 0.3};
    static const double vbrPositive[4] = {0.4, 0.5, 0.75, 0.90};
    for (unsigned i = 0; i < 4; ++i) {
        d.DevThreshPB0_S8[i] = int8_t(-50 * pow(pbNegative[i], ratio));
        d.DevThreshPB0_S8[i + 4] = int8_t(50 * pow(pbPositive[i], ratio));
        d.DevThreshI0_S8[i] = int8_t(-50 * pow(iNegative[i], ratio));
        d.DevThreshI0_S8[i + 4] = int8_t(50 * pow(iPositive[i], ratio));
        d.DevThreshVBR0_S8[i] = int8_t(-50 * pow(vbrNegative[i], ratio));
        d.DevThreshVBR0_S8[i + 4] = int8_t(100 * pow(vbrPositive[i], ratio));
    }
    static const int8_t instantP[4] = {40, 60, 80, 120};
    static const int8_t instantB[4] = {35, 60, 80, 120};
    static const int8_t instantI[4] = {40, 60, 90, 115};
    copy(d.InstRateThreshP0_S8, instantP);
    copy(d.InstRateThreshB0_S8, instantB);
    copy(d.InstRateThreshI0_S8, instantI);
    if (p.mode == BrcMode::Acqp)
        d.InitQPIP_U8 = d.InitQPB_U8 = p.initialQp;
    else
        initialQps(p, d.InitQPIP_U8, d.InitQPB_U8);
    d.TopFrmSzThrForAdapt2Pass_U8 = 32;
    d.BotFrmSzThrForAdapt2Pass_U8 = 24;
    static const uint8_t estimated[7] = {4, 8, 12, 16, 20, 24, 28};
    copy(d.EstRateThreshP0_U8, estimated);
    copy(d.EstRateThreshB0_U8, estimated);
    copy(d.EstRateThreshI0_U8, estimated);
    d.TopQPDeltaThrForAdapt2Pass_U8 = 2;
    d.BotQPDeltaThrForAdapt2Pass_U8 = 1;
    const uint32_t fps = p.frameRateNumerator / p.frameRateDenominator;
    d.SlidingWindow_Size_U32 = p.windowFrames ? p.windowFrames : (fps < 60 ? fps : 60);
    d.SLIDINGWINDOW_MaxRateRatio = uint8_t(p.windowFrames ? p.windowMaximumRatio : 120);
    d.LongTermRefEnable_U8 = d.LongTermRefMsdk_U8 = 1;
    d.IsLowDelay_U8 = p.lowDelay;
    out = d;
    return true;
}

bool buildBrcUpdateDmem(const BrcUpdateParams &p, BrcUpdateDmem &out) {
    if (!p.batchBytes || uint32_t(p.cmd1Offset) + 31 * 4 > p.batchBytes ||
        uint32_t(p.pictureOffset) + 41 * 4 > p.batchBytes ||
        uint32_t(p.cmd2Offset) + 51 * 4 > p.batchBytes ||
        (p.cmd1Offset & 3) || (p.pictureOffset & 3) || (p.cmd2Offset & 3) ||
        p.referencesL0 > 3 || p.referencesL1 > 2 ||
        p.referencesL0 + p.referencesL1 > 5 || !p.slices ||
        p.slices > p.maximumSlices || p.qp > 51 || uint8_t(p.frameType) > 4 ||
        !p.maximumPasses || p.maximumPasses > 2 || p.pass >= p.maximumPasses ||
        p.pictureDataOffset % kBrcPictureDataBytes ||
        p.pictureDataOffset >= kBrcPictureDataSlots * kBrcPictureDataBytes)
        return false;
    for (const auto offset : p.referenceDataOffsets)
        if (offset % kBrcPictureDataBytes ||
            offset >= kBrcPictureDataSlots * kBrcPictureDataBytes)
            return false;
    BrcUpdateDmem d {};
    d.TARGETSIZE_U32 = p.targetFullness;
    d.FrameID_U32 = p.frameId;
    copy(d.Ref_L0_FrameID_U32, p.referenceFrameIdsL0);
    copy(d.Ref_L1_FrameID_U32, p.referenceFrameIdsL1);
    static const uint16_t adjustmentFrames[4] = {10, 50, 100, 150};
    copy(d.startGAdjFrame_U16, adjustmentFrames);
    d.TargetSliceSize_U16 = p.targetSliceBytes;
    d.SLB_Data_SizeInBytes = p.batchBytes;
    d.CMD1_StartInBytes = p.cmd1Offset;
    d.PIC_STATE_StartInBytes = p.pictureOffset;
    d.CMD2_StartInBytes = p.cmd2Offset;
    d.PIPE_MODE_SELECT_StartInBytes = 0xffff;
    d.Current_Data_Offset = p.pictureDataOffset;
    copy(d.Ref_Data_Offset, p.referenceDataOffsets);
    d.MaxNumSliceAllowed_U16 = p.maximumSlices;
    d.OpMode_U8 = 1;
    d.CurrentFrameType_U8 = uint8_t(p.frameType);
    d.Num_Ref_L0_U8 = p.referencesL0;
    d.Num_Ref_L1_U8 = p.referencesL1;
    d.Num_Slices = p.slices;
    d.CQP_QPValue_U8 = p.qp;
    d.MaxNumPass_U8 = p.maximumPasses;
    static const uint8_t ratioThresholds[7] = {40, 75, 97, 103, 125, 160, 0};
    static const uint8_t adjustmentMultipliers[5] = {1, 1, 3, 2, 1};
    static const uint8_t adjustmentDivisors[5] = {40, 5, 5, 3, 1};
    static const uint8_t ratioQp[8] = {253, 254, 255, 0, 1, 2, 3, 0};
    copy(d.gRateRatioThreshold_U8, ratioThresholds);
    copy(d.startGAdjMult_U8, adjustmentMultipliers);
    copy(d.startGAdjDiv_U8, adjustmentDivisors);
    copy(d.gRateRatioThresholdQP_U8, ratioQp);
    d.IPAverageCoeff_U8 = 64;
    d.CurrentPass_U8 = p.pass;
    if (p.cuQpControl) {
        d.DeltaQPForSadZone0_S8 = -1;
        d.DeltaQPForSadZone2_S8 = 1;
        d.DeltaQPForSadZone3_S8 = 2;
        d.DeltaQPForMvZero_S8 = 3;
        d.DeltaQPForMvZone0_S8 = -2;
        d.DeltaQPForMvZone2_S8 = 2;
    }
    d.LOG_LCU_Size_U8 = 6;
    d.SlidingWindow_Enable_U8 = p.slidingWindow;
    d.ReEncodePositiveQPDeltaThr_S8 = 4;
    d.ReEncodeNegativeQPDeltaThr_S8 = -5;
    d.SceneChgPrevIntraPctThreshold_U8 = 96;
    d.SceneChgCurIntraPctThreshold_U8 = 192;
    d.SliceHeaderSize = p.sliceHeaderBits;
    out = d;
    return true;
}

}
}
