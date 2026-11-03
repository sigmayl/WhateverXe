// SPDX-License-Identifier: MIT
#include "vdenc_walker.hpp"

#include "hw/mhw_vdbox_vdenc_hwcmd_g12_X.h"

namespace HevcCore {
namespace VdEnc {

Packet walkerState(const WalkerParams &params) {
    mhw_vdbox_vdenc_g12_X::VDENC_WALKER_STATE_CMD cmd {};

    cmd.DW1.MbLcuStartXPosition = params.startXCtb;
    cmd.DW1.MbLcuStartYPosition = params.startYCtb;
    cmd.DW2.NextsliceMbLcuStartXPosition = params.nextXCtb;
    cmd.DW2.NextsliceMbStartYPosition = params.nextYCtb;

    cmd.DW3.Log2WeightDenomLuma = params.log2WeightDenom;
    cmd.DW3.HevcLog2WeightDemonLuma = params.log2WeightDenom;
    cmd.DW3.NumParEngine = params.numParEngine;
    cmd.DW5.TileWidth = params.widthInPix - 1;
    cmd.DW5.TileHeight = params.heightInPix - 1;

    // Reserved upper fields are cleared the same way as the reference driver.
    cmd.DW11.Value &= 0xfffc00ff;
    cmd.DW12.Value = (cmd.DW12.Value & 0xfcffffff) | 0x01000000;
    cmd.DW12.PaletteModeEnable = params.paletteEnabled ? 1 : 0;
    cmd.DW12.IbcControl = params.ibcEnabled ? 1 : 0;

    return packetFrom(cmd);
}

}
}
