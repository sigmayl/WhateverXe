// SPDX-License-Identifier: MIT
// Gen12 VDENC walker state for HEVC. Field mapping follows
// mhw_vdbox_vdenc_g12_X::AddVdencWalkerStateCmd.
#ifndef WEX_MEDIA_HEVC_CORE_VDENC_WALKER_HPP
#define WEX_MEDIA_HEVC_CORE_VDENC_WALKER_HPP

#include "hw_command.hpp"

namespace HevcCore {
namespace VdEnc {

struct WalkerParams {
    uint32_t startXCtb {};        // slice start CTB X
    uint32_t startYCtb {};        // slice start CTB Y
    uint32_t nextXCtb {};         // next slice start CTB X
    uint32_t nextYCtb {};         // next slice start CTB Y
    uint32_t widthInPix {1920};   // coded picture width
    uint32_t heightInPix {1088};  // coded picture height
    uint32_t log2WeightDenom {};  // luma weight denominator, 0 without weighted pred
    uint32_t numParEngine {};
    bool firstSuperSlice {false};
    bool ibcEnabled {false};
    bool paletteEnabled {false};
};

Packet walkerState(const WalkerParams &params);

}
}

#endif
