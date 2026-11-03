// SPDX-License-Identifier: MIT
#include "vdenc_weights.hpp"

#include <algorithm>

#include "hw/mhw_vdbox_vdenc_hwcmd_g12_X.h"

namespace HevcCore {
namespace VdEnc {
namespace {
uint32_t byteValue(int value) {
    return static_cast<uint32_t>(std::max(-128, std::min(127, value))) & 0xFF;
}
}

Packet weightsOffsetsState(const WeightsParams &params) {
    mhw_vdbox_vdenc_g12_X::VDENC_WEIGHTSOFFSETS_STATE_CMD cmd {};

    // Unity weights and zero offsets are the defaults when weighted
    // prediction is off.
    cmd.DW1.WeightsForwardReference0 = 1;
    cmd.DW1.OffsetForwardReference0 = 0;
    cmd.DW1.WeightsForwardReference1 = 1;
    cmd.DW1.OffsetForwardReference1 = 0;
    cmd.DW2.WeightsForwardReference2 = 1;
    cmd.DW2.OffsetForwardReference2 = 0;
    cmd.DW2.HevcVp9WeightsBackwardReference0 = 1;
    cmd.DW2.HevcVp9OffsetBackwardReference0 = 0;

    if (params.weightedPred) {
        const int denom = static_cast<int>(params.denom);
        cmd.DW1.WeightsForwardReference0 = byteValue(params.lumaWeights[0][0] + denom);
        cmd.DW1.OffsetForwardReference0 = static_cast<uint32_t>(params.lumaOffsets[0][0]) & 0xFF;
        cmd.DW1.WeightsForwardReference1 = byteValue(params.lumaWeights[0][1] + denom);
        cmd.DW1.OffsetForwardReference1 = static_cast<uint32_t>(params.lumaOffsets[0][1]) & 0xFF;
        cmd.DW2.WeightsForwardReference2 = byteValue(params.lumaWeights[0][2] + denom);
        cmd.DW2.OffsetForwardReference2 = static_cast<uint32_t>(params.lumaOffsets[0][2]) & 0xFF;

        const unsigned list = params.lowDelay ? 0 : 1;
        cmd.DW2.HevcVp9WeightsBackwardReference0 = params.lowDelay
            ? cmd.DW1.WeightsForwardReference0
            : byteValue(params.lumaWeights[list][0] + denom);
        cmd.DW2.HevcVp9OffsetBackwardReference0 = params.lowDelay
            ? cmd.DW1.OffsetForwardReference0
            : (static_cast<uint32_t>(params.lumaOffsets[list][0]) & 0xFF);
    }

    return packetFrom(cmd);
}

}
}
