// SPDX-License-Identifier: MIT
#include "hcp_slice.hpp"

#include <stdlib.h>

#include "hw/mhw_vdbox_hcp_hwcmd_g12_X.h"

namespace HevcCore {
namespace Hcp {

Packet sliceState(const SliceParams &params) {
    mhw_vdbox_hcp_g12_X::HCP_SLICE_STATE_CMD cmd {};

    cmd.DW1.SlicestartctbxOrSliceStartLcuXEncoder = params.ctbAddress % params.widthInCtb;
    cmd.DW1.SlicestartctbyOrSliceStartLcuYEncoder = params.ctbAddress / params.widthInCtb;

    if (params.lastSlice && !params.endAtNextCtb) {
        cmd.DW2.NextslicestartctbxOrNextSliceStartLcuXEncoder = 0;
        cmd.DW2.NextslicestartctbyOrNextSliceStartLcuYEncoder = 0;
    } else {
        cmd.DW2.NextslicestartctbxOrNextSliceStartLcuXEncoder = params.nextCtbAddress % params.widthInCtb;
        cmd.DW2.NextslicestartctbyOrNextSliceStartLcuYEncoder = params.nextCtbAddress / params.widthInCtb;
    }

    const int sliceQp = static_cast<int>(params.qpY) + params.sliceQpDelta;
    cmd.DW3.SliceType = params.sliceType;
    cmd.DW3.Lastsliceofpic = params.lastSlice ? 1 : 0;
    cmd.DW3.SliceqpSignFlag = sliceQp < 0 ? 1 : 0;
    cmd.DW3.DependentSliceFlag = 0;
    cmd.DW3.SliceTemporalMvpEnableFlag = (params.sliceType == 2) ? 0 : (params.temporalMvp ? 1 : 0);
    cmd.DW3.Sliceqp = static_cast<uint32_t>(abs(sliceQp));
    cmd.DW3.SliceCbQpOffset = static_cast<uint32_t>(params.sliceCbQpOffset) & 0x1f;
    cmd.DW3.SliceCrQpOffset = static_cast<uint32_t>(params.sliceCrQpOffset) & 0x1f;
    cmd.DW3.Intrareffetchdisable = params.intraRefFetchDisable ? 1 : 0;
    cmd.DW3.Lastsliceoftile = params.lastSliceInTile ? 1 : 0;
    cmd.DW3.Lastsliceoftilecolumn = params.lastSliceInTileColumn ? 1 : 0;

    cmd.DW4.SliceHeaderDisableDeblockingFilterFlag = params.deblockingFilterDisable ? 1 : 0;
    cmd.DW4.SliceTcOffsetDiv2OrFinalTcOffsetDiv2Encoder = static_cast<uint32_t>(params.tcOffsetDiv2) & 0x7;
    cmd.DW4.SliceBetaOffsetDiv2OrFinalBetaOffsetDiv2Encoder = static_cast<uint32_t>(params.betaOffsetDiv2) & 0xf;
    cmd.DW4.SliceLoopFilterAcrossSlicesEnabledFlag = params.loopFilterAcrossSlices ? 1 : 0;
    cmd.DW4.SliceSaoChromaFlag = params.saoChroma ? 1 : 0;
    cmd.DW4.SliceSaoLumaFlag = params.saoLuma ? 1 : 0;
    cmd.DW4.MvdL1ZeroFlag = 0;
    cmd.DW4.Islowdelay = params.lowDelay ? 1 : 0;
    cmd.DW4.CollocatedFromL0Flag = params.collocatedFromL0 ? 1 : 0;

    const bool weighted = params.weightedPred || params.weightedBipred;
    cmd.DW4.Chromalog2Weightdenom = weighted
        ? params.lumaLog2WeightDenom + params.deltaChromaLog2WeightDenom
        : 0;
    cmd.DW4.LumaLog2WeightDenom = weighted ? params.lumaLog2WeightDenom : 0;
    cmd.DW4.CabacInitFlag = params.cabacInit ? 1 : 0;
    cmd.DW4.Maxmergeidx = params.maxNumMergeCand ? params.maxNumMergeCand - 1 : 0;
    cmd.DW4.Collocatedrefidx = 0;  // I slices and disabled temporal MVP use zero

    // Decoder-only field: the encoder carries the header bytes through the PAK
    // insert object, so iHD leaves this zero for the encode slice state.
    cmd.DW5.Sliceheaderlength = 0;

    cmd.DW6.Roundinter = params.vdencInUse ? 4 : 0;
    cmd.DW6.Roundintra = params.vdencInUse ? 10 : 0;

    // VDEnc inserts CABAC zero words and the tail through the HuC kernel.
    cmd.DW7.Cabaczerowordinsertionenable = params.vdencInUse ? 0 : 1;
    cmd.DW7.Emulationbytesliceinsertenable = 1;
    cmd.DW7.TailInsertionEnable = params.vdencInUse
        ? 0
        : ((params.lastPicInSeq || params.lastPicInStream) && params.lastSlice ? 1 : 0);
    cmd.DW7.SlicedataEnable = 1;
    cmd.DW7.HeaderInsertionEnable = 1;

    cmd.DW8.IndirectPakBseDataStartOffsetWrite = params.headerBytesInserted;

    if (params.transformSkipEnabled) {
        cmd.DW9.TransformskipLambda = params.transformSkipLambda;
        cmd.DW10.TransformskipNumzerocoeffsFactor0 = params.transformSkipZeroCoeffsFactor0;
        cmd.DW10.TransformskipNumnonzerocoeffsFactor0 = params.transformSkipNonZeroCoeffsFactor0;
        cmd.DW10.TransformskipNumzerocoeffsFactor1 = params.transformSkipZeroCoeffsFactor1;
        cmd.DW10.TransformskipNumnonzerocoeffsFactor1 = params.transformSkipNonZeroCoeffsFactor1;
    }

    return packetFrom(cmd);
}

}
}
