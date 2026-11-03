// SPDX-License-Identifier: MIT
#include "vdenc_state.hpp"

namespace HevcCore {
namespace VdEnc {
namespace {

// VDENC_PIPE_MODE_SELECT (iHD DWORD 1..3).
constexpr Field StandardSelect {1, 0, 3};
constexpr Field FrameStatisticsStreamOut {1, 5, 5};
constexpr Field PakObjCmdStreamOut {1, 6, 6};
constexpr Field TlbPrefetch {1, 7, 7};
constexpr Field PakThresholdCheck {1, 8, 8};
constexpr Field StreamInEnable {1, 9, 9};
constexpr Field BitDepth {1, 12, 14};
constexpr Field ChromaSubSampling {1, 15, 16};
constexpr Field OutputRangeControl {1, 17, 17};
constexpr Field RandomAccess {1, 18, 18};
constexpr Field PrimaryChannelSelection {1, 21, 22};
constexpr Field FirstSecondaryChannelSelection {1, 23, 24};
constexpr Field TileReplay {1, 25, 25};
constexpr Field Downscaled8XWriteDisable {1, 10, 10};
constexpr Field Downscaled4XWriteDisable {1, 11, 11};
constexpr Field HmeRegionPrefetch {2, 0, 0};
constexpr Field SourcePrefetchOffset {3, 24, 27};
constexpr Field SourceVerticalRequests {3, 16, 19};

// VDENC_Surface_State_Fields, relative to the first DWORD of the block.
constexpr Field CrVCbPixelOffsetVDirection {0, 0, 1};
constexpr Field SurfaceFormatByteSwizzle {0, 2, 2};
constexpr Field ColorSpaceSelection {0, 3, 3};
constexpr Field SurfaceWidth {0, 4, 17};
constexpr Field SurfaceHeight {0, 18, 31};
constexpr Field TileWalk {1, 0, 0};
constexpr Field TiledSurface {1, 1, 1};
constexpr Field SurfacePitch {1, 3, 19};
constexpr Field ChromaDownsampleFilter {1, 20, 22};
constexpr Field SurfaceFormatField {1, 27, 31};
constexpr Field YOffsetForUCb {2, 0, 14};
constexpr Field YOffsetForVCr {3, 0, 15};

// VDENC_REF_SURFACE_STATE (iHD DWORD 1).
constexpr Field SurfaceId {1, 0, 2};

// PIPE_BUF_ADDR slot offsets, three DWORDs per slot.
constexpr uint32_t SlotDsFwdRef0 = 1;
constexpr uint32_t SlotDsFwdRef1 = 4;
constexpr uint32_t SlotDsReserved = 7;
constexpr uint32_t SlotOriginalUncompressed = 10;
constexpr uint32_t SlotStreaminData = 13;
constexpr uint32_t SlotRowStoreScratch = 16;
constexpr uint32_t SlotColocatedMv = 19;
constexpr uint32_t SlotFwdRef0 = 22;
constexpr uint32_t SlotFwdRef1 = 25;
constexpr uint32_t SlotFwdRef2 = 28;
constexpr uint32_t SlotBwdRef0 = 31;
constexpr uint32_t SlotStatisticsStreamout = 34;
constexpr uint32_t SlotDsFwdRef0_4x = 37;
constexpr uint32_t SlotDsFwdRef1_4x = 40;
constexpr uint32_t SlotCuRecordStreamout = 43;
constexpr uint32_t SlotLcuPakObjCmd = 46;
constexpr uint32_t SlotScaledRef8x = 49;
constexpr uint32_t SlotScaledRef4x = 52;
constexpr uint32_t SlotVp9SegMapStreamin = 55;
constexpr uint32_t SlotVp9SegMapStreamout = 58;
constexpr uint32_t DwordWeightsHistogramOffset = 61;
constexpr uint32_t SlotTileRowStore = 62;
constexpr uint32_t SlotCumulativeCuCountStreamout = 65;
constexpr uint32_t SlotPaletteModeStreamout = 68;

void writeSurfaceFields(Packet &packet, uint32_t dw, const SurfaceState &surface,
    uint8_t chromaFilter) {
    packet.set(Field {uint8_t(dw + CrVCbPixelOffsetVDirection.dw), CrVCbPixelOffsetVDirection.lo, CrVCbPixelOffsetVDirection.hi}, surface.vDirection);
    packet.set(Field {uint8_t(dw + SurfaceFormatByteSwizzle.dw), SurfaceFormatByteSwizzle.lo, SurfaceFormatByteSwizzle.hi}, surface.byteSwizzle ? 1 : 0);
    packet.set(Field {uint8_t(dw + ColorSpaceSelection.dw), ColorSpaceSelection.lo, ColorSpaceSelection.hi}, surface.colorSpaceSelect ? 1 : 0);
    packet.set(Field {uint8_t(dw + SurfaceWidth.dw), SurfaceWidth.lo, SurfaceWidth.hi}, surface.width - 1);
    packet.set(Field {uint8_t(dw + SurfaceHeight.dw), SurfaceHeight.lo, SurfaceHeight.hi}, surface.height - 1);
    packet.set(Field {uint8_t(dw + TileWalk.dw), TileWalk.lo, TileWalk.hi}, surface.tileY ? 1 : 0);
    packet.set(Field {uint8_t(dw + TiledSurface.dw), TiledSurface.lo, TiledSurface.hi}, surface.tileY ? 1 : 0);
    packet.set(Field {uint8_t(dw + SurfacePitch.dw), SurfacePitch.lo, SurfacePitch.hi}, surface.pitch - 1);
    packet.set(Field {uint8_t(dw + ChromaDownsampleFilter.dw), ChromaDownsampleFilter.lo, ChromaDownsampleFilter.hi}, chromaFilter);
    packet.set(Field {uint8_t(dw + SurfaceFormatField.dw), SurfaceFormatField.lo, SurfaceFormatField.hi}, surface.format);
    packet.set(Field {uint8_t(dw + YOffsetForUCb.dw), YOffsetForUCb.lo, YOffsetForUCb.hi}, surface.uvRowOffset);
    packet.set(Field {uint8_t(dw + YOffsetForVCr.dw), YOffsetForVCr.lo, YOffsetForVCr.hi}, surface.uvRowOffset);
}

void writeAddressBlock(Packet &packet, uint32_t dw, const AddressBlock &block) {
    packet.setAddress(dw, block.address);
    packet[dw + 2] = block.control;
}

}

// VDENC_CONTROL_STATE: command 0Bh, opcode 1h, pipeline 2h, parallel video
// pipe. DWORD 1 bit 1 requests VDEnc initialization.
Packet controlState(bool initialization) {
    Packet packet(2);
    commandHeader(packet, 0xB, 1, 2, 3);
    packet[1] = initialization ? 2 : 0;
    return packet;
}

Packet pipelineFlush(uint32_t flags) {
    Packet packet(2);
    // MEDIA_COMMAND_OPCODE_EXTENDEDCOMMAND (0Fh) on the media pipeline.
    commandHeader(packet, 0, 0xF, 2, 3);
    packet[1] = flags;
    return packet;
}

Packet pipeModeSelect(const PipeModeSelect &params) {
    Packet packet(6);
    vdCommandHeader(packet, SubopPipeModeSelect);

    // HEVC is HCP base, so the standard select field is zero.
    packet.set(StandardSelect, 0);
    packet.set(FrameStatisticsStreamOut, params.frameStatistics ? 1 : 0);
    packet.set(PakObjCmdStreamOut, params.pakObjCmdStreamOut ? 1 : 0);
    packet.set(TlbPrefetch, 1);
    packet.set(PakThresholdCheck, 0);
    packet.set(StreamInEnable, params.streamIn ? 1 : 0);
    packet.set(BitDepth, params.main10 ? 2 : 0);
    packet.set(ChromaSubSampling, params.chromaType);
    packet.set(OutputRangeControl, 1);
    packet.set(RandomAccess, params.randomAccess ? 1 : 0);
    packet.set(PrimaryChannelSelection, 1);
    packet.set(FirstSecondaryChannelSelection, 2);
    packet.set(TileReplay, params.tileReplay ? 1 : 0);
    packet.set(Downscaled8XWriteDisable, params.ds8xWriteDisable ? 1 : 0);
    packet.set(Downscaled4XWriteDisable, params.ds4xWriteDisable ? 1 : 0);

    // Constructor defaults: HME region prefetch on, top/left prefetch on,
    // horizontal shift 3, 11 vertical and 2 horizontal requests.
    packet[2] = 0x002B030B;
    packet.set(HmeRegionPrefetch, 0);

    // Constructor defaults: source luma/chroma TLB prefetch on, horizontal
    // shift 3, then the HEVC source prefetch tuning.
    packet[3] = 0x00000303;
    packet.set(SourcePrefetchOffset, params.sourcePrefetchOffset);
    packet.set(SourceVerticalRequests, params.sourceVerticalRequests);
    return packet;
}

Packet srcSurfaceState(const SurfaceState &surface) {
    Packet packet(6);
    vdCommandHeader(packet, SubopSrcSurfaceState);
    writeSurfaceFields(packet, 2, surface, surface.chromaDownsampleFilter);
    return packet;
}

Packet refSurfaceState(const SurfaceState &surface, uint32_t surfaceId) {
    Packet packet(6);
    vdCommandHeader(packet, SubopRefSurfaceState);
    packet.set(SurfaceId, surfaceId);
    writeSurfaceFields(packet, 2, surface, 0);
    return packet;
}

Packet dsRefSurfaceState(const SurfaceState &downscaled4x, const SurfaceState &downscaled8x) {
    Packet packet(10);
    vdCommandHeader(packet, SubopDsRefSurfaceState);
    // Gen12 puts the 8x state first and the 4x state second.
    writeSurfaceFields(packet, 2, downscaled8x, 0);
    writeSurfaceFields(packet, 6, downscaled4x, 0);
    return packet;
}

Packet pipeBufAddrState(const PipeBufAddr &buffers) {
    Packet packet(71);
    vdCommandHeader(packet, SubopPipeBufAddrState);
    writeAddressBlock(packet, SlotDsFwdRef0, buffers.dsFwdRef0);
    writeAddressBlock(packet, SlotDsFwdRef1, buffers.dsFwdRef1);
    writeAddressBlock(packet, SlotDsReserved, buffers.dsReserved);
    writeAddressBlock(packet, SlotOriginalUncompressed, buffers.originalUncompressed);
    writeAddressBlock(packet, SlotStreaminData, buffers.streaminData);
    writeAddressBlock(packet, SlotRowStoreScratch, buffers.rowStoreScratch);
    writeAddressBlock(packet, SlotColocatedMv, buffers.colocatedMv);
    writeAddressBlock(packet, SlotFwdRef0, buffers.fwdRef0);
    writeAddressBlock(packet, SlotFwdRef1, buffers.fwdRef1);
    writeAddressBlock(packet, SlotFwdRef2, buffers.fwdRef2);
    writeAddressBlock(packet, SlotBwdRef0, buffers.bwdRef0);
    writeAddressBlock(packet, SlotStatisticsStreamout, buffers.statisticsStreamout);
    writeAddressBlock(packet, SlotDsFwdRef0_4x, buffers.dsFwdRef0_4x);
    writeAddressBlock(packet, SlotDsFwdRef1_4x, buffers.dsFwdRef1_4x);
    writeAddressBlock(packet, SlotCuRecordStreamout, buffers.cuRecordStreamout);
    writeAddressBlock(packet, SlotLcuPakObjCmd, buffers.lcuPakObjCmd);
    writeAddressBlock(packet, SlotScaledRef8x, buffers.scaledRef8x);
    writeAddressBlock(packet, SlotScaledRef4x, buffers.scaledRef4x);
    writeAddressBlock(packet, SlotVp9SegMapStreamin, buffers.vp9SegMapStreamin);
    writeAddressBlock(packet, SlotVp9SegMapStreamout, buffers.vp9SegMapStreamout);
    packet[DwordWeightsHistogramOffset] = buffers.weightsHistogramOffset;
    writeAddressBlock(packet, SlotTileRowStore, buffers.tileRowStore);
    writeAddressBlock(packet, SlotCumulativeCuCountStreamout, buffers.cumulativeCuCountStreamout);
    writeAddressBlock(packet, SlotPaletteModeStreamout, buffers.paletteModeStreamout);
    return packet;
}

}
}
