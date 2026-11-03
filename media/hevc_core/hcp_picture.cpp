// SPDX-License-Identifier: MIT
#include "hcp_picture.hpp"

#include "hw/mhw_vdbox_hcp_hwcmd_g12_X.h"

namespace HevcCore {
namespace Hcp {

Packet picState(const PicStateParams &params) {
    mhw_vdbox_hcp_g12_X::HCP_PIC_STATE_CMD cmd {};

    cmd.DW1.Framewidthinmincbminus1 = params.widthInMinCbMinus1;
    cmd.DW1.Frameheightinmincbminus1 = params.heightInMinCbMinus1;
    cmd.DW1.PakTransformSkipEnable = params.transformSkip ? 1 : 0;

    cmd.DW2.Mincusize = params.log2MinCodingBlockMinus3;
    cmd.DW2.CtbsizeLcusize = params.log2MaxCodingBlockMinus3;
    cmd.DW2.Maxtusize = params.log2MaxTransformBlockMinus2;
    cmd.DW2.Mintusize = params.log2MinTransformBlockMinus2;
    cmd.DW2.Minpcmsize = 0;
    cmd.DW2.Maxpcmsize = 0;
    cmd.DW2.ChromaSubsampling = params.chromaFormatIdc;

    // Collocated and current picture intra flags are kept at zero for both
    // low and normal frequency paths, as the reference driver does.
    cmd.DW3.Colpicisi = 0;
    cmd.DW3.Curpicisi = 0;

    cmd.DW4.SampleAdaptiveOffsetEnabledFlag = params.saoEnabled ? 1 : 0;
    cmd.DW4.PcmEnabledFlag = 0;
    cmd.DW4.CuQpDeltaEnabledFlag = params.cuQpDeltaEnabled ? 1 : 0;
    cmd.DW4.DiffCuQpDeltaDepthOrNamedAsMaxDqpDepth = params.diffCuQpDeltaDepth;
    cmd.DW4.PcmLoopFilterDisableFlag = params.pcmLoopFilterDisable ? 1 : 0;
    cmd.DW4.ConstrainedIntraPredFlag = params.constrainedIntraPred ? 1 : 0;
    cmd.DW4.Log2ParallelMergeLevelMinus2 = 0;
    cmd.DW4.SignDataHidingFlag = 0;
    cmd.DW4.LoopFilterAcrossTilesEnabledFlag = params.loopFilterAcrossTiles ? 1 : 0;
    cmd.DW4.EntropyCodingSyncEnabledFlag = 0;
    cmd.DW4.TilesEnabledFlag = params.tilesEnabled ? 1 : 0;
    cmd.DW4.WeightedPredFlag = params.weightedPred ? 1 : 0;
    cmd.DW4.WeightedBipredFlag = params.weightedBipred ? 1 : 0;
    cmd.DW4.Fieldpic = 0;
    cmd.DW4.Bottomfield = 0;
    cmd.DW4.TransformSkipEnabledFlag = params.transformSkip ? 1 : 0;
    cmd.DW4.AmpEnabledFlag = params.ampEnabled ? 1 : 0;
    cmd.DW4.TransquantBypassEnableFlag = params.transquantBypass ? 1 : 0;
    cmd.DW4.StrongIntraSmoothingEnableFlag = params.strongIntraSmoothing ? 1 : 0;
    cmd.DW4.CuPacketStructure = 0;

    cmd.DW5.PicCbQpOffset = static_cast<uint32_t>(params.ppsCbQpOffset) & 0x1f;
    cmd.DW5.PicCrQpOffset = static_cast<uint32_t>(params.ppsCrQpOffset) & 0x1f;
    cmd.DW5.MaxTransformHierarchyDepthIntraOrNamedAsTuMaxDepthIntra = params.maxTransformHierarchyDepthIntra;
    cmd.DW5.MaxTransformHierarchyDepthInterOrNamedAsTuMaxDepthInter = params.maxTransformHierarchyDepthInter;
    cmd.DW5.PcmSampleBitDepthChromaMinus1 = params.pcmSampleBitDepthChromaMinus1;
    cmd.DW5.PcmSampleBitDepthLumaMinus1 = params.pcmSampleBitDepthLumaMinus1;
    cmd.DW5.BitDepthChromaMinus8 = params.bitDepthChromaMinus8;
    cmd.DW5.BitDepthLumaMinus8 = params.bitDepthLumaMinus8;

    cmd.DW6.LcuMaxBitsizeAllowed = params.lcuMaxBitsizeAllowed;
    cmd.DW6.Nonfirstpassflag = 0;
    cmd.DW6.LcumaxbitstatusenLcumaxsizereportmask = 0;
    cmd.DW6.FrameszoverstatusenFramebitratemaxreportmask = 0;
    cmd.DW6.FrameszunderstatusenFramebitrateminreportmask = 0;
    cmd.DW6.LoadSlicePointerFlag = 0;

    cmd.DW19.RdoqEnable = params.rdoqEnabled ? 1 : 0;
    cmd.DW19.SseEnable = params.useVdenc ? 1 : 0;
    cmd.DW19.RhodomainRateControlEnable = params.useVdenc ? 1 : 0;
    cmd.DW19.Rhodomainframelevelqp = params.useVdenc ? params.qpY : 0;
    cmd.DW19.FractionalQpAdjustmentEnable = params.useVdenc ? 1 : 0;
    cmd.DW19.FirstSliceSegmentInPicFlag = 1;
    cmd.DW19.Nalunittypeflag = 0;
    cmd.DW19.PartialFrameUpdateMode = params.partialFrameUpdate ? 1 : 0;
    cmd.DW19.TemporalMvPredDisable = (params.temporalMvp && !params.partialFrameUpdate) ? 0 : 1;

    cmd.DW20.Intratucountbasedrdoqdisable = params.rdoqIntraTuDisable ? 1 : 0;

    cmd.DW21.SliceSizeThresholdInBytes = 0;
    cmd.DW22.TargetSliceSizeInBytes = 0;

    cmd.DW34.IntraBoundaryFilteringDisabledFlag = params.intraBoundaryFilteringDisabled ? 1 : 0;
    cmd.DW34.MotionVectorResolutionControlIdc = params.motionVectorResolutionControlIdc;
    cmd.DW34.PpsCurrPicRefEnabledFlag = params.currPicRefEnabled ? 1 : 0;
    cmd.DW34.IbcMotionCompensationBufferReferenceIdc = params.recNotFilteredId;

    cmd.DW35.IbcConfiguration = params.currPicRefEnabled ? 1 : 0;
    cmd.DW35.PaletteModeEnabledFlag = params.paletteEnabled ? 1 : 0;
    cmd.DW35.MonochromePaletteFlag = 0;
    cmd.DW35.PaletteMaxSize = params.paletteEnabled ? 64 : 0;
    cmd.DW35.DeltaPaletteMaxPredictorSize = params.paletteEnabled ? 32 : 0;
    cmd.DW35.LumaBitDepthEntryMinus8 = params.bitDepthLumaMinus8;
    cmd.DW35.ChromaBitDepthEntryMinus8 = params.bitDepthChromaMinus8;

    return packetFrom(cmd);
}

}
}
