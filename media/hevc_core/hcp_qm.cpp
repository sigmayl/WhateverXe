// SPDX-License-Identifier: MIT
#include "hcp_qm.hpp"

#include <string.h>

#include "hw/mhw_vdbox_hcp_hwcmd_g12_X.h"

namespace HevcCore {
namespace Hcp {
namespace {

using HcpCommand = mhw_vdbox_hcp_g12_X;

constexpr uint8_t kFlatScale = 16;
constexpr uint8_t kFlatDc = 16;

const uint8_t *sourceList(const QmParams &params, uint32_t sizeId, uint32_t pred,
    uint32_t color) {
    switch (sizeId) {
    case 0:
        return params.list4x4[pred][color];
    case 1:
        return params.list8x8[pred][color];
    case 2:
        return params.list16x16[pred][color];
    default:
        return params.list32x32[pred];
    }
}

uint32_t sourceCount(uint32_t sizeId) {
    return sizeId == 0 ? 16 : 64;
}

uint8_t dcValue(const QmParams &params, uint32_t sizeId, uint32_t pred, uint32_t color) {
    if (sizeId == 2)
        return params.dc16x16[pred][color];
    if (sizeId == 3)
        return params.dc32x32[pred];
    return 0;
}

}

QmParams::QmParams() {
    memset(list4x4, kFlatScale, sizeof(list4x4));
    memset(list8x8, kFlatScale, sizeof(list8x8));
    memset(list16x16, kFlatScale, sizeof(list16x16));
    memset(list32x32, kFlatScale, sizeof(list32x32));
    memset(dc16x16, kFlatDc, sizeof(dc16x16));
    memset(dc32x32, kFlatDc, sizeof(dc32x32));
}

uint16_t reciprocalScalingValue(uint8_t scaleValue) {
    if (scaleValue < 2)
        return 0xFFFF;
    return static_cast<uint16_t>((4096 * 16) / scaleValue);
}

std::vector<Packet> qmState(const QmParams &params) {
    std::vector<Packet> packets;
    packets.reserve(20);

    for (uint32_t sizeId = 0; sizeId < 4; ++sizeId) {
        const uint32_t lastColor = sizeId == 3 ? 1 : 3;
        for (uint32_t pred = 0; pred < 2; ++pred) {
            for (uint32_t color = 0; color < lastColor; ++color) {
                HcpCommand::HCP_QM_STATE_CMD cmd {};
                cmd.DW1.Sizeid = sizeId;
                cmd.DW1.PredictionType = pred;
                cmd.DW1.ColorComponent = color;
                cmd.DW1.DcCoefficient = dcValue(params, sizeId, pred, color);

                uint8_t *matrix = reinterpret_cast<uint8_t *>(cmd.Quantizermatrix);
                const uint32_t count = sourceCount(sizeId);
                memcpy(matrix, sourceList(params, sizeId, pred, color), count);
                packets.push_back(packetFrom(cmd));
            }
        }
    }
    return packets;
}

std::vector<Packet> fqmState(const QmParams &params) {
    std::vector<Packet> packets;
    packets.reserve(8);

    for (uint32_t pred = 0; pred < 2; ++pred) {
        for (uint32_t sizeId = 0; sizeId < 4; ++sizeId) {
            HcpCommand::HCP_FQM_STATE_CMD cmd {};
            cmd.DW1.IntraInter = pred;
            cmd.DW1.Sizeid = sizeId;
            cmd.DW1.ColorComponent = 0;
            if (sizeId >= 2)
                cmd.DW1.FqmDcValue1Dc = reciprocalScalingValue(dcValue(params, sizeId, pred, 0));

            uint16_t *matrix = reinterpret_cast<uint16_t *>(cmd.Quantizermatrix);
            const uint8_t *list = sourceList(params, sizeId, pred, 0);
            const uint32_t count = sourceCount(sizeId);
            for (uint32_t i = 0; i < count; ++i)
                matrix[i] = reciprocalScalingValue(list[i]);

            packets.push_back(packetFrom(cmd));
        }
    }
    return packets;
}

}
}
