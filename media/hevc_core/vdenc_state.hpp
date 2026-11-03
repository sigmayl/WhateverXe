// SPDX-License-Identifier: MIT
// Gen12 VDENC picture-level state commands for HEVC encoding. Field layouts
// follow media-driver's mhw_vdbox_vdenc_hwcmd_g12_X (DWORD numbers are noted
// next to each block), so packets can be diffed against the reference driver.
#ifndef WEX_MEDIA_HEVC_CORE_VDENC_STATE_HPP
#define WEX_MEDIA_HEVC_CORE_VDENC_STATE_HPP

#include "hw_command.hpp"

namespace HevcCore {
namespace VdEnc {

// SUBOPB values for VDENC_* pipe commands.
enum Subop : uint32_t {
    SubopPipeModeSelect = 0,
    SubopSrcSurfaceState = 1,
    SubopRefSurfaceState = 2,
    SubopDsRefSurfaceState = 3,
    SubopPipeBufAddrState = 4,
    SubopWalkerState = 7,
};

// Gen12 MosFormatToVdencSurfaceRawFormat / ReconFormat values, programmed in
// Dwords25.DW1.SurfaceFormat by mhw_vdbox_vdenc_g12_X.h. The generated command
// header's older P010 enum descriptions use different values.
enum SurfaceFormat : uint32_t {
    FormatP010Variant = 3,
    FormatPlanar420_8 = 4,
    FormatP010 = 13,
};

// Common geometry for the source, reconstructed and downscaled surfaces.
struct SurfaceState {
    uint32_t width {};        // actual picture width in pixels
    uint32_t height {};       // actual picture height in pixels
    uint32_t pitch {};        // surface pitch in bytes
    uint32_t uvRowOffset {};  // chroma plane start, in rows
    uint32_t format {FormatPlanar420_8};
    bool tileY {true};
    bool byteSwizzle {false};
    bool colorSpaceSelect {false};
    // Only programmed on the source surface, matching the reference driver.
    uint8_t chromaDownsampleFilter {7};
    uint8_t vDirection {};
};

// VDENC_PIPE_MODE_SELECT. Defaults mirror the iHD command constructor; the
// HEVC picture path overrides the codec-specific fields.
struct PipeModeSelect {
    bool main10 {false};
    bool streamIn {false};
    bool frameStatistics {false};
    // The VDEnc hands PAK objects to the PAK through the internal path for a
    // single-pass picture; media-driver only streams them out to memory for
    // pass 0 of a multi-pass encode, and clears the bit otherwise.
    bool pakObjCmdStreamOut {false};
    bool randomAccess {true};
    bool tileReplay {false};
    // Leave the existing 8x/4x downscaled recon contents in place instead of
    // overwriting them with this picture. Inter pictures read the reference's
    // downscaled planes from the same buffers.
    bool ds8xWriteDisable {false};
    bool ds4xWriteDisable {false};
    uint32_t chromaType {1};          // 0: 4:0:0, 1: 4:2:0, 2: 4:2:2, 3: 4:4:4
    uint8_t sourcePrefetchOffset {4};
    uint8_t sourceVerticalRequests {1};
};

// One 64B aligned buffer reference: address plus the surface control DWORD.
struct AddressBlock {
    uint64_t address {};
    uint32_t control {};
};

// VDENC_PIPE_BUF_ADDR_STATE buffer slots, in command DWORD order.
struct PipeBufAddr {
    AddressBlock dsFwdRef0 {};
    AddressBlock dsFwdRef1 {};
    AddressBlock dsReserved {};
    AddressBlock originalUncompressed {};
    AddressBlock streaminData {};
    AddressBlock rowStoreScratch {};
    AddressBlock colocatedMv {};
    AddressBlock fwdRef0 {};
    AddressBlock fwdRef1 {};
    AddressBlock fwdRef2 {};
    AddressBlock bwdRef0 {};
    AddressBlock statisticsStreamout {};
    AddressBlock dsFwdRef0_4x {};
    AddressBlock dsFwdRef1_4x {};
    AddressBlock cuRecordStreamout {};
    AddressBlock lcuPakObjCmd {};
    AddressBlock scaledRef8x {};
    AddressBlock scaledRef4x {};
    AddressBlock vp9SegMapStreamin {};
    AddressBlock vp9SegMapStreamout {};
    uint32_t weightsHistogramOffset {};
    AddressBlock tileRowStore {};
    AddressBlock cumulativeCuCountStreamout {};
    AddressBlock paletteModeStreamout {};
};

Packet pipeModeSelect(const PipeModeSelect &params);
// VDENC_CONTROL_STATE (command 0Bh, opcode 1h). Sent once at the start of the
// picture with the initialization bit set; the VDEnc pipe ignores the picture
// commands until it arrives.
Packet controlState(bool initialization = true);
// VD_PIPELINE_FLUSH. DWORD 1 selects the pipelines to wait for and flush:
// bit 0 HEVC done, 1 VDENC done, 3 MFX done, 4 parser done, 16 HEVC flush,
// 17 VDENC flush. iHD waits for VDENC after the walker and for HEVC at the
// end of the slice batch; without them the pipe can stall.
Packet pipelineFlush(uint32_t flags);
Packet srcSurfaceState(const SurfaceState &surface);
Packet refSurfaceState(const SurfaceState &surface, uint32_t surfaceId);
Packet dsRefSurfaceState(const SurfaceState &downscaled4x, const SurfaceState &downscaled8x);
Packet pipeBufAddrState(const PipeBufAddr &buffers);

}
}

#endif
