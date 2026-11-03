// SPDX-License-Identifier: MIT
// Gen12 HCP quantization matrix and reciprocal matrix commands for HEVC.
// Layouts follow media-driver's HCP_QM_STATE and HCP_FQM_STATE builders.
#ifndef WEX_MEDIA_HEVC_CORE_HCP_QM_HPP
#define WEX_MEDIA_HEVC_CORE_HCP_QM_HPP

#include <vector>

#include "hw_command.hpp"

namespace HevcCore {
namespace Hcp {

// HEVC quantization matrices. Lists are stored in raster order, indexed by
// prediction type (0 intra, 1 inter) and colour component (Y, Cb, Cr). The
// 32x32 list has no chroma entries, matching the HCP_QM_STATE size rules.
struct QmParams {
    uint8_t list4x4[2][3][16] {};
    uint8_t list8x8[2][3][64] {};
    uint8_t list16x16[2][3][64] {};
    uint8_t list32x32[2][64] {};
    uint8_t dc16x16[2][3] {};
    uint8_t dc32x32[2] {};

    // Flat 16 matrices and DC values, the default when the stream disables
    // scaling lists.
    QmParams();
};

// HCP_QM_STATE emits up to 20 commands: 4x4, 8x8, 16x16 and 32x32 for intra
// and inter, each for Y, Cb and Cr, with no 32x32 chroma matrices.
std::vector<Packet> qmState(const QmParams &params);

// HCP_FQM_STATE emits eight commands for the luma reciprocal matrices,
// including the DC reciprocal for 16x16 and 32x32.
std::vector<Packet> fqmState(const QmParams &params);

// 1/q scaling value used by the FQM command: 0xFFFF for q < 2, otherwise
// (4096 * 16) / q.
uint16_t reciprocalScalingValue(uint8_t scaleValue);

}
}

#endif
