// SPDX-License-Identifier: MIT
// Gen12 HCP picture state for HEVC encoding. The field mapping follows
// media-driver's MhwVdboxHcpInterfaceG12::AddHcpEncodePicStateCmd.
#ifndef WEX_MEDIA_HEVC_CORE_HCP_PICTURE_HPP
#define WEX_MEDIA_HEVC_CORE_HCP_PICTURE_HPP

#include "hw_command.hpp"

namespace HevcCore {
namespace Hcp {

struct PicStateParams {
    uint32_t widthInMinCbMinus1 {239};   // 1920 / 8 - 1
    uint32_t heightInMinCbMinus1 {134};  // 1080 / 8 - 1
    uint32_t log2MinCodingBlockMinus3 {0};
    uint32_t log2MaxCodingBlockMinus3 {3};
    uint32_t log2MinTransformBlockMinus2 {0};
    uint32_t log2MaxTransformBlockMinus2 {3};
    uint32_t chromaFormatIdc {1};
    bool saoEnabled {true};
    // Disabled for the constant-QP path: without ACQP, BRC or VDEnc
    // stream-in there is no per-CU QP data to signal.
    bool cuQpDeltaEnabled {false};
    uint32_t diffCuQpDeltaDepth {};
    bool pcmLoopFilterDisable {false};
    bool constrainedIntraPred {false};
    bool weightedPred {false};
    bool weightedBipred {false};
    bool ampEnabled {true};
    bool transquantBypass {false};
    bool strongIntraSmoothing {true};
    bool tilesEnabled {false};
    bool loopFilterAcrossTiles {false};
    int32_t ppsCbQpOffset {};
    int32_t ppsCrQpOffset {};
    uint32_t maxTransformHierarchyDepthIntra {};
    uint32_t maxTransformHierarchyDepthInter {};
    uint32_t pcmSampleBitDepthChromaMinus1 {};
    uint32_t pcmSampleBitDepthLumaMinus1 {};
    uint32_t bitDepthChromaMinus8 {};
    uint32_t bitDepthLumaMinus8 {};
    // iHD computes this in CalcLCUMaxCodingSize when the plug-in passes zero:
    // 64x64 CTU, 4:2:0, 8-bit -> 81920, masked to the 16-bit command field.
    uint32_t lcuMaxBitsizeAllowed {81920};
    bool rdoqEnabled {false};
    bool rdoqIntraTuDisable {false};
    bool transformSkip {false};
    bool useVdenc {true};
    uint32_t qpY {26};
    bool paletteEnabled {false};
    bool currPicRefEnabled {false};
    uint32_t recNotFilteredId {};
    bool intraBoundaryFilteringDisabled {false};
    uint32_t motionVectorResolutionControlIdc {};
    bool partialFrameUpdate {false};
    bool temporalMvp {false};
};

Packet picState(const PicStateParams &params);

}
}

#endif
