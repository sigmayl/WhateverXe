// SPDX-License-Identifier: MIT
// Gen12 VDENC picture-level HEVC commands built on the vendored MHW command
// definitions. The value sequences are ported from media-driver's
// mhw_vdbox_vdenc_g12_X::AddVdencCmd1Cmd / AddVdencCmd2Cmd.
#ifndef WEX_MEDIA_HEVC_CORE_VDENC_PICTURE_HPP
#define WEX_MEDIA_HEVC_CORE_VDENC_PICTURE_HPP

#include "hw_command.hpp"

namespace HevcCore {
namespace VdEnc {

// VDENC_CMD1 carries the HEVC mode-cost and lambda tables. P pictures reuse
// the shared baseline values.
struct Cmd1Params {
    bool intra {true};
    bool visualQuality {false};
    uint32_t qpY {26};
    int32_t sliceQpDelta {};
    bool roi {false};
};

// VDENC_CMD2 carries the HEVC picture parameters used by the VDEnc engine.
struct Cmd2Params {
    uint32_t frameWidth {1920};
    uint32_t frameHeight {1080};
    bool intra {true};
    // VDENC_CMD2.DW2.PICTURE_TYPE. The hardware definition documents 0 = I and
    // 1 = P; the reference driver also uses 2 and 3 for regular and low-delay
    // B pictures. This path codes I and P slices, so inter pictures carry P.
    uint32_t pictureType {1};
    bool lowDelayB {false};
    bool temporalMvp {false};
    bool transformSkip {false};
    bool tilesEnabled {false};
    bool tileReplay {false};
    bool streamIn {false};
    bool pakOnlyMultipass {false};
    bool visualQuality {false};
    bool useDefaultQpDeltas {false};
    bool rounding {false};
    uint32_t targetUsage {4};        // 1 quality, 4 normal, 7 speed
    uint32_t qpY {26};
    int32_t sliceQpDelta {};
    uint32_t numRefIdxL0 {0};
    uint32_t numRefIdxL1 {0};
    // Picture order counts the VDEnc uses to describe the reference list.
    // Fields carry currPoc - refPoc, clipped to -16..16.
    uint32_t currPoc {0};
    uint32_t refPocL0[3] {};
    uint32_t refPocL1[3] {};
    uint32_t brcMinQp {10};
    uint32_t brcMaxQp {51};
    uint32_t roundInter {4};
    uint32_t roundIntra {10};
    // Reference frame indices for L0/L1; 0xFF marks an unused entry.
    uint8_t refL0[3] {0xFF, 0xFF, 0xFF};
    uint8_t refL1[3] {0xFF, 0xFF, 0xFF};
    // REF_IDX_MAPPING table indexed by frame index.
    uint8_t refIdxMapping[8] {7, 7, 7, 7, 7, 7, 7, 7};
};

Packet cmd1(const Cmd1Params &params);
Packet cmd2(const Cmd2Params &params);

}
}

#endif
