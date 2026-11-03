// SPDX-License-Identifier: MIT
// Gen12 HCP slice state for HEVC encoding. Field mapping follows
// MhwVdboxHcpInterfaceG12::AddHcpEncodeSliceStateCmd.
#ifndef WEX_MEDIA_HEVC_CORE_HCP_SLICE_HPP
#define WEX_MEDIA_HEVC_CORE_HCP_SLICE_HPP

#include "hw_command.hpp"

namespace HevcCore {
namespace Hcp {

struct SliceParams {
    uint32_t widthInCtb {30};        // ceil(picture width / CTB size)
    uint32_t ctbAddress {};
    uint32_t numLcusInSlice {};
    uint32_t nextCtbAddress {};      // first CTB of the following slice
    bool lastSlice {true};
    // Diagnostic: program nextCtbAddress even for the last slice, instead of
    // the (0,0) pair media-driver writes for it.
    bool endAtNextCtb {false};
    uint32_t sliceType {2};          // 0 B, 1 P, 2 I
    bool lastSliceInTile {true};
    bool lastSliceInTileColumn {false};
    int32_t sliceQpDelta {};
    uint32_t qpY {26};
    int32_t sliceCbQpOffset {};
    int32_t sliceCrQpOffset {};
    bool temporalMvp {false};
    bool intraRefFetchDisable {false};
    bool deblockingFilterDisable {false};
    int32_t tcOffsetDiv2 {};
    int32_t betaOffsetDiv2 {};
    bool loopFilterAcrossSlices {false};
    bool saoLuma {true};
    bool saoChroma {true};
    bool lowDelay {false};
    bool collocatedFromL0 {false};
    bool weightedPred {false};
    bool weightedBipred {false};
    uint32_t lumaLog2WeightDenom {};
    uint32_t deltaChromaLog2WeightDenom {};
    bool cabacInit {};
    uint32_t maxNumMergeCand {5};
    bool vdencInUse {true};
    // Byte offset from the slice start to the slice data, i.e. the packed
    // slice header length. The PAK uses it to place the slice body.
    uint32_t sliceHeaderBytes {};
    bool lastPicInSeq {false};
    bool lastPicInStream {false};
    uint32_t headerBytesInserted {};
    bool transformSkipEnabled {false};
    uint32_t transformSkipLambda {};
    uint32_t transformSkipZeroCoeffsFactor0 {};
    uint32_t transformSkipNonZeroCoeffsFactor0 {};
    uint32_t transformSkipZeroCoeffsFactor1 {};
    uint32_t transformSkipNonZeroCoeffsFactor1 {};
};

Packet sliceState(const SliceParams &params);

}
}

#endif
