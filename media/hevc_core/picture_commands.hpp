// SPDX-License-Identifier: MIT
// Gen12 HEVC VDEnc picture command sequence. The order follows media-driver's
// CodechalVdencHevcStateG12::SendPictureStateCommands: buffer and matrix
// states first, then the VDEnc surface states, with CMD1 / PIC_STATE / CMD2
// placed in the second-level batch buffer used by the HuC kernel.
#ifndef WEX_MEDIA_HEVC_CORE_PICTURE_COMMANDS_HPP
#define WEX_MEDIA_HEVC_CORE_PICTURE_COMMANDS_HPP

#include <vector>

#include "hcp_picture.hpp"
#include "hcp_qm.hpp"
#include "hcp_state.hpp"
#include "vdenc_picture.hpp"
#include "vdenc_state.hpp"

namespace HevcCore {

struct PictureParams {
    Hcp::PipeModeSelect hcpPipeMode;
    Hcp::SurfaceState sourceSurface {Hcp::SurfaceSource, 1920, 1088};
    Hcp::SurfaceState reconSurface {Hcp::SurfaceDecoded, 1920, 1088};
    Hcp::SurfaceState referenceSurface {Hcp::SurfaceReference, 1920, 1088};
    Hcp::PipeBufAddrParams pipeBuf;
    Hcp::IndObjParams indObj;
    Hcp::QmParams qm;

    VdEnc::PipeModeSelect vdencPipeMode;
    VdEnc::SurfaceState vdencSource {1920, 1080, 1920, 1088};
    VdEnc::SurfaceState vdencReference {1920, 1080, 1920, 1088};
    VdEnc::SurfaceState downscaled4x {960, 540, 960, 544};
    VdEnc::SurfaceState downscaled8x {480, 270, 480, 272};
    VdEnc::PipeBufAddr vdencBuf;

    VdEnc::Cmd1Params cmd1;
    Hcp::PicStateParams pic;
    VdEnc::Cmd2Params cmd2;
};

// HCP pipe mode, HCP surface states, HCP buffer and matrix states, then the
// VDEnc pipe mode, surface states and buffer addresses.
std::vector<Packet> pictureFirstLevel(const PictureParams &params);

// VDENC_CMD1, HCP_PIC_STATE and VDENC_CMD2, in that order.
std::vector<Packet> pictureSecondLevel(const PictureParams &params);

// MI_FLUSH_DW that drains the media pipeline. Sent after the walker so the
// PAK output and the slice-size writeback are complete before the batch ends.
Packet flushDw();

// MI_FORCE_WAKEUP. Gen12 keeps the MFX (search) and HEVC (PAK) power wells
// gated until the batch asks for them; without this command the VDEnc starts
// but the PAK never consumes its objects and the pipeline flush never
// completes. iHD sends it at the head of the picture prolog.
Packet forceWakeup(bool mfxPowerWell, bool hevcPowerWell);

// MFX_WAIT handshake. iHD places it before and after HCP_PIPE_MODE_SELECT.
Packet mfxWait(bool stallVdboxPipeline);

}

#endif
