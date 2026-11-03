// SPDX-License-Identifier: MIT
// Gen12 VDENC weights/offsets state for HEVC weighted prediction. Field
// mapping follows AddVdencWeightsOffsetsStateCmd.
#ifndef WEX_MEDIA_HEVC_CORE_VDENC_WEIGHTS_HPP
#define WEX_MEDIA_HEVC_CORE_VDENC_WEIGHTS_HPP

#include "hw_command.hpp"

namespace HevcCore {
namespace VdEnc {

struct WeightsParams {
    bool weightedPred {false};
    bool lowDelay {false};
    uint32_t denom {1};             // 1 << luma_log2_weight_denom
    int32_t lumaWeights[2][3] {};   // per list, per reference
    int32_t lumaOffsets[2][3] {};
};

Packet weightsOffsetsState(const WeightsParams &params);

}
}

#endif
