// SPDX-License-Identifier: MIT
// Gen12 HEVC parameter sets and IDR slice header. Field order follows the
// header packer in Intel media-driver's encode_hevc_header_packer.cpp.
#ifndef WEX_MEDIA_HEVC_HEADERS_HPP
#define WEX_MEDIA_HEVC_HEADERS_HPP
#include <stdint.h>
#include <vector>
#include "hevc_geometry.hpp"

namespace HevcCore {
struct Config {
    uint32_t width {1920}, height {1080};
    uint32_t fps {30};
    uint32_t qp {26};
    uint32_t level {153}; // general_level_idc: 5.1
    bool main10 {false};
    // Signals cu_qp_delta in the PPS. Its depth subdivides a 64x64 CTU
    // into quantization groups and must agree with HCP_PIC_STATE.
    bool cuQpDelta {false};
    uint32_t diffCuQpDeltaDepth {0};
    // Shared defaults keep the parameter sets and HCP picture state aligned.
    // minCodingBlockLog2 is the smallest luma coding block (8x8 by default);
    // the maximum coding block stays 64x64.
    uint32_t minCodingBlockLog2 {3};
    uint32_t maxTransformLog2 {kMaxTransformLog2};
    uint32_t transformDepth {kTransformHierarchyDepth};
    // sps_temporal_mvp_enabled_flag. This path codes no temporal motion
    // vectors: the slice header signals the flag as zero, so the SPS says zero
    // too and the HCP picture state disables the collocated-MV read.
    bool temporalMvp {false};
    uint32_t visibleWidth {0}, visibleHeight {0};
    bool operator==(const Config &o) const {
        return width == o.width && height == o.height && fps == o.fps &&
            qp == o.qp && level == o.level && main10 == o.main10 &&
            cuQpDelta == o.cuQpDelta && temporalMvp == o.temporalMvp &&
            visibleWidth == o.visibleWidth && visibleHeight == o.visibleHeight &&
            diffCuQpDeltaDepth == o.diffCuQpDeltaDepth &&
            minCodingBlockLog2 == o.minCodingBlockLog2 &&
            maxTransformLog2 == o.maxTransformLog2 &&
            transformDepth == o.transformDepth;
    }
};

enum NalType : uint8_t {
    NalTrailR = 1,
    NalIdrWRADL = 19,
    NalVps = 32,
    NalSps = 33,
    NalPps = 34,
};

struct ParameterSets {
    std::vector<uint8_t> vps, sps, pps, idrHeader;
    std::vector<uint8_t> stream() const {
        std::vector<uint8_t> out;
        const std::vector<uint8_t> *parts[4] = {&vps, &sps, &pps, &idrHeader};
        for (const auto *part : parts)
            out.insert(out.end(), part->begin(), part->end());
        return out;
    }
};

struct SliceConfig {
    uint32_t qp {26};
    bool saoLuma {false};
    bool saoChroma {false};
    // Sets slice_deblocking_filter_disabled_flag. The beta/tc offsets are
    // only present when the filter is enabled.
    bool deblockingFilterDisable {false};
    // The plug-in schedules a B-frame display order. Pictures coded as I
    // carry an empty short-term reference set; an inter picture carries the
    // one reference picture it predicts from.
    bool idr {true};
    bool inter {false};
    // Codes the inter picture as a low-delay B slice: slice_type 0 with list 1
    // holding the same reference as list 0. The HCP header states that a VDEnc
    // HEVC slice is B or I only, and media-driver's low-delay mode converts
    // every "P" picture to this form.
    bool lowDelayB {false};
    uint32_t poc {0};
    // POC of the single L0 reference. It is marked used_by_curr_pic.
    uint32_t refPoc {0};
    // Additional pictures kept in the decoded picture buffer by the inline
    // short-term reference picture set. They are not used by this picture;
    // listing them keeps them available for later B/P pictures.
    uint32_t rpsPocs[4] {};
    uint32_t rpsCount {0};
};

// The caller owns parameter validation: even dimensions, 64x64 CTU limits and
// a profile/level combination that the target media engine accepts. emulate
// selects Annex B emulation prevention bytes; the PAK insert path passes false
// and lets the hardware insert them.
ParameterSets buildParameterSets(const Config &config, bool emulate = true);

// Annex B IDR_W_RADL slice header. It is byte-aligned and ready for the
// HCP_PAK_INSERT_OBJECT payload.
std::vector<uint8_t> buildIdrSliceHeader(const Config &config,
    const SliceConfig &slice, bool emulate = true);

// Annex B slice header for an IDR (slice.idr) or TRAIL_R I picture. Non-IDR
// headers include slice_pic_order_cnt_lsb and an explicit empty short-term
// reference set, so an all-I stream can still carry display order.
std::vector<uint8_t> buildSliceHeader(const Config &config,
    const SliceConfig &slice, bool emulate = true);
}
#endif
