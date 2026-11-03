// SPDX-License-Identifier: MIT
#include "hcp_state.hpp"

#include <assert.h>

#include "hw/mhw_vdbox_hcp_hwcmd_g12_X.h"

namespace HevcCore {
namespace Hcp {
namespace {

using HcpCommand = mhw_vdbox_hcp_g12_X;

void writeAddress(HcpCommand::SPLITBASEADDRESS64BYTEALIGNED_CMD &block, uint64_t address) {
    assert((address & 0x3F) == 0);
    block.DW0_1.Value[0] = static_cast<uint32_t>(address & 0xFFFFFFC0u);
    block.DW0_1.Value[1] = static_cast<uint32_t>(address >> 32);
}

void writeAddress(HcpCommand::SPLITBASEADDRESS4KBYTEALIGNED_CMD &block, uint64_t address) {
    assert((address & 0xFFF) == 0);
    block.DW0_1.Value[0] = static_cast<uint32_t>(address & 0xFFFFF000u);
    block.DW0_1.Value[1] = static_cast<uint32_t>(address >> 32);
}

void writeAttributes(HcpCommand::MEMORYADDRESSATTRIBUTES_CMD &attributes, uint32_t control) {
    attributes.DW0.Value = control;
}

// HCP_PIPE_MODE_SELECT (iHD DWORD 1 and 6).
constexpr Field CodecSelect {1, 0, 0};
constexpr Field DeblockerStreamOut {1, 1, 1};
constexpr Field PakPipelineStreamOut {1, 2, 2};
constexpr Field CodecStandardSelect {1, 5, 7};
constexpr Field AdvancedRateControl {1, 9, 9};
constexpr Field VdencMode {1, 10, 10};
constexpr Field RdoqEnabled {1, 11, 11};
constexpr Field PakFrameLevelStreamOut {1, 12, 12};
constexpr Field MultiEngineMode {1, 13, 14};
constexpr Field PipeWorkingMode {1, 15, 16};
constexpr Field TileBasedEngine {1, 17, 17};
constexpr Field PrefetchDisable {1, 18, 18};
constexpr Field PhaseIndicator {6, 0, 1};
constexpr Field SeparateTileProgramming {6, 2, 2};
constexpr Field FrameReconstructionDisable {6, 3, 3};

// HCP_SURFACE_STATE (iHD DWORD 1..4).
constexpr Field SurfacePitchMinus1 {1, 0, 16};
constexpr Field SurfaceIdField {1, 28, 31};
constexpr Field YOffsetForUCbInPixel {2, 0, 14};
constexpr Field SurfaceFormat {2, 27, 31};
constexpr Field DefaultAlphaValue {3, 0, 15};
constexpr Field YOffsetForVCr {3, 16, 31};
constexpr Field MemoryCompressionEnable {4, 0, 7};
constexpr Field CompressionType {4, 8, 15};
constexpr Field CompressionFormat {4, 16, 20};

}

// VD_CONTROL_STATE with the HCP media instruction command (0Ah) and the codec
// engine opcode (7h). DWORD 1 bit 0 initializes the pipeline.
Packet controlState(bool initialization) {
    Packet packet(3);
    commandHeader(packet, 0xA, 7, 2, 3);
    packet[1] = initialization ? 1 : 0;
    return packet;
}

Packet pipeModeSelect(const PipeModeSelect &params) {    Packet packet(7);
    hcpCommandHeader(packet, CommandPipeModeSelect);

    // HEVC is the HCP base standard, so the standard select field stays 0.
    packet.set(CodecSelect, 1);
    packet.set(DeblockerStreamOut, params.deblockerStreamOut ? 1 : 0);
    packet.set(PakPipelineStreamOut, params.streamOut ? 1 : 0);
    packet.set(CodecStandardSelect, 0);
    packet.set(AdvancedRateControl, params.advancedRateControl ? 1 : 0);
    packet.set(VdencMode, 1);
    packet.set(RdoqEnabled, params.rdoq ? 1 : 0);
    packet.set(PakFrameLevelStreamOut, params.streamOut ? 1 : 0);
    packet.set(MultiEngineMode, params.multiEngineMode);
    packet.set(PipeWorkingMode, params.pipeWorkingMode);
    packet.set(TileBasedEngine, params.tileBasedEngine ? 1 : 0);
    packet.set(PrefetchDisable, params.prefetchDisable ? 1 : 0);
    packet.set(PhaseIndicator, params.phaseIndicator);
    packet.set(SeparateTileProgramming, params.separateTileProgramming ? 1 : 0);
    packet.set(FrameReconstructionDisable, params.frameReconstructionDisable ? 1 : 0);
    return packet;
}

Packet surfaceState(const SurfaceState &surface) {
    Packet packet(5);
    hcpCommandHeader(packet, CommandSurfaceState);
    packet.set(SurfacePitchMinus1, surface.pitch - 1);
    packet.set(SurfaceIdField, surface.surfaceId);
    packet.set(YOffsetForUCbInPixel, surface.uvRowOffset);
    packet.set(SurfaceFormat, surface.format);
    packet.set(DefaultAlphaValue, surface.defaultAlpha);
    // Both chroma planes share the row offset. The builder used to leave the
    // VCr field clear, which pointed the PAK's Cr reads at the luma plane.
    packet.set(YOffsetForVCr, surface.uvRowOffset);
    packet.set(MemoryCompressionEnable, surface.memoryCompressionEnable);
    packet.set(CompressionType, surface.compressionType);
    packet.set(CompressionFormat, surface.compressionFormat);
    return packet;
}

uint32_t referenceAttributes(TileMode tiling, uint32_t cachePolicy) {
    const uint32_t mode = tiling == TileMode::YS ? 1u
        : tiling == TileMode::YF ? 2u : 0u;
    return cachePolicy | (mode << 13);
}

Packet pipeBufAddrState(const PipeBufAddrParams &params) {
    HcpCommand::HCP_PIPE_BUF_ADDR_STATE_CMD cmd {};

    writeAddress(cmd.DecodedPicture, params.decodedPicture.address);
    writeAttributes(cmd.DecodedPictureMemoryAddressAttributes, params.decodedPicture.control);

    writeAddress(cmd.DeblockingFilterLineBuffer, params.deblockingLine.address);
    writeAttributes(cmd.DeblockingFilterLineBufferMemoryAddressAttributes, params.deblockingLine.control);

    writeAddress(cmd.DeblockingFilterTileLineBuffer, params.deblockingTileLine.address);
    writeAttributes(cmd.DeblockingFilterTileLineBufferMemoryAddressAttributes, params.deblockingTileLine.control);

    writeAddress(cmd.DeblockingFilterTileColumnBuffer, params.deblockingTileColumn.address);
    writeAttributes(cmd.DeblockingFilterTileColumnBufferMemoryAddressAttributes, params.deblockingTileColumn.control);

    writeAddress(cmd.MetadataLineBuffer, params.metadataLine.address);
    writeAttributes(cmd.MetadataLineBufferMemoryAddressAttributes, params.metadataLine.control);

    writeAddress(cmd.MetadataTileLineBuffer, params.metadataTileLine.address);
    writeAttributes(cmd.MetadataTileLineBufferMemoryAddressAttributes, params.metadataTileLine.control);

    writeAddress(cmd.MetadataTileColumnBuffer, params.metadataTileColumn.address);
    writeAttributes(cmd.MetadataTileColumnBufferMemoryAddressAttributes, params.metadataTileColumn.control);

    writeAddress(cmd.SaoLineBuffer, params.saoLine.address);
    writeAttributes(cmd.SaoLineBufferMemoryAddressAttributes, params.saoLine.control);

    writeAddress(cmd.SaoTileLineBuffer, params.saoTileLine.address);
    writeAttributes(cmd.SaoTileLineBufferMemoryAddressAttributes, params.saoTileLine.control);

    writeAddress(cmd.SaoTileColumnBuffer, params.saoTileColumn.address);
    writeAttributes(cmd.SaoTileColumnBufferMemoryAddressAttributes, params.saoTileColumn.control);

    writeAddress(cmd.CurrentMotionVectorTemporalBuffer, params.currentMvTemporal.address);
    writeAttributes(cmd.CurrentMotionVectorTemporalBufferMemoryAddressAttributes, params.currentMvTemporal.control);

    for (uint32_t i = 0; i < 8; ++i)
        writeAddress(cmd.ReferencePictureBaseAddressRefaddr07[i], params.references[i].address);
    writeAttributes(cmd.ReferencePictureBaseAddressMemoryAddressAttributes, params.referenceControl);

    writeAddress(cmd.OriginalUncompressedPictureSource, params.originalUncompressed.address);
    writeAttributes(cmd.OriginalUncompressedPictureSourceMemoryAddressAttributes, params.originalUncompressed.control);

    writeAddress(cmd.StreamoutDataDestination, params.streamout.address);
    writeAttributes(cmd.StreamoutDataDestinationMemoryAddressAttributes, params.streamout.control);

    writeAddress(cmd.DecodedPictureStatusErrorBufferBaseAddressOrEncodedSliceSizeStreamoutBaseAddress,
        params.encodedSliceSize.address);
    writeAttributes(cmd.DecodedPictureStatusErrorBufferBaseAddressMemoryAddressAttributes,
        params.encodedSliceSize.control);

    writeAddress(cmd.LcuIldbStreamoutBuffer, params.lcuIldb.address);
    writeAttributes(cmd.LcuIldbStreamoutBufferMemoryAddressAttributes, params.lcuIldb.control);

    for (uint32_t i = 0; i < 8; ++i)
        writeAddress(cmd.CollocatedMotionVectorTemporalBuffer07[i], params.collocatedMv[i].address);
    writeAttributes(cmd.CollocatedMotionVectorTemporalBuffer07MemoryAddressAttributes,
        params.collocatedMvControl);

    writeAddress(cmd.FrameStatisticsStreamoutDataDestinationBufferBaseAddress, params.frameStatistics.address);
    writeAttributes(cmd.FrameStatisticsStreamoutDataDestinationBufferAttributesReadWrite,
        params.frameStatistics.control);

    writeAddress(cmd.SseSourcePixelRowstoreBufferBaseAddress, params.sseSourceRowstore.address);
    writeAttributes(cmd.SseSourcePixelRowstoreBufferAttributesReadWrite, params.sseSourceRowstore.control);

    writeAddress(cmd.MotionVectorUpperRightColumnStoreBufferBaseAddress, params.mvUpperRight.address);
    writeAttributes(cmd.MotionVectorUpperRightColumnStoreBufferAttributesReadWrite, params.mvUpperRight.control);

    writeAddress(cmd.IntraPredictionUpperRightColumnStoreBufferBaseAddress, params.intraUpperRight.address);
    writeAttributes(cmd.IntraPredictionUpperRightColumnStoreBufferAttributesReadWrite,
        params.intraUpperRight.control);

    writeAddress(cmd.IntraPredictionLeftReconColumnStoreBufferBaseAddress, params.intraLeftRecon.address);
    writeAttributes(cmd.IntraPredictionLeftReconColumnStoreBufferAttributesReadWrite,
        params.intraLeftRecon.control);

    return packetFrom(cmd);
}

Packet refIdxState(const RefIdxParams &params) {    Packet packet(18);
    hcpCommandHeader(packet, CommandRefIdxState);
    packet[1] = (params.list & 1u)
        | ((params.numRefs ? params.numRefs - 1 : 0) << 1);
    for (uint32_t i = 0; i < params.numRefs && i < 16; ++i) {
        const RefListEntry &entry = params.entries[i];
        // HCP_REF_LIST_ENTRY: POC delta in bits 0-7, frame store id in bits
        // 8-10, long-term flag in bit 13, field flags above it. media-driver's
        // encoder always codes progressive pictures and writes
        // BottomFieldFlag = 1 for them (mhw_vdbox_hcp_generic.h inverts the
        // picture's RefBottomFieldFlag), so the entry must carry bit 15.
        const uint32_t delta = static_cast<uint32_t>(entry.pocDelta) & 0xFFu;
        packet[2 + i] = delta
            | ((entry.frameId & 0x7u) << 8)
            | (1u << 15)
            | (entry.longTerm ? (1u << 13) : 0u);
    }
    return packet;
}

Packet indObjBaseAddrState(const IndObjParams &params) {
    HcpCommand::HCP_IND_OBJ_BASE_ADDR_STATE_CMD cmd {};

    writeAddress(cmd.HcpIndirectBitstreamObjectBaseAddress, params.bitstreamObject.address);
    writeAttributes(cmd.HcpIndirectBitstreamObjectMemoryAddressAttributes,
        params.bitstreamObject.control);
    writeAddress(cmd.HcpIndirectBitstreamObjectAccessUpperBound, params.bitstreamUpperBound.address);

    assert((params.cuObject.address & 0x3F) == 0);
    cmd.DW6_7.Value[0] = static_cast<uint32_t>(params.cuObject.address & 0xFFFFFFC0u);
    cmd.DW6_7.Value[1] = static_cast<uint32_t>(params.cuObject.address >> 32);
    writeAttributes(cmd.HcpIndirectCuObjectObjectMemoryAddressAttributes, params.cuObject.control);

    assert((params.pakBseObject.address & 0x3F) == 0);
    cmd.DW9_10.Value[0] = static_cast<uint32_t>(params.pakBseObject.address & 0xFFFFFFC0u);
    cmd.DW9_10.Value[1] = static_cast<uint32_t>(params.pakBseObject.address >> 32);
    writeAttributes(cmd.HcpPakBseObjectAddressMemoryAddressAttributes, params.pakBseObject.control);
    writeAddress(cmd.HcpPakBseObjectAccessUpperBound, params.pakBseUpperBound.address);

    return packetFrom(cmd);
}

Packet tileCodingState(const TileCodingParams &params) {
    HcpCommand::HCP_TILE_CODING_CMD cmd {};
    cmd.DW1.NumberOfActiveBePipes = params.activeBePipes;
    cmd.DW1.NumOfTileColumnsInAFrame = params.tileColumnsInFrame;
    cmd.DW1.TileRowStoreSelect = params.tileRowStoreSelect;
    cmd.DW1.TileColumnStoreSelect = params.tileColumnStoreSelect;
    cmd.DW2.TileColumnPosition = params.tileStartLcuX;
    cmd.DW2.TileRowPosition = params.tileStartLcuY;
    cmd.DW2.Islasttileofrow = params.lastTileOfRow ? 1 : 0;
    cmd.DW2.Islasttileofcolumn = params.lastTileOfColumn ? 1 : 0;
    cmd.DW3.Tileheightinmincbminus1 = params.tileHeightInMinCbMinus1;
    cmd.DW3.Tilewidthinmincbminus1 = params.tileWidthInMinCbMinus1;
    cmd.DW4.BitstreamByteOffsetEnable = params.bitstreamByteOffsetEnable ? 1 : 0;
    cmd.DW4.BitstreamByteOffset = params.bitstreamByteOffset;
    cmd.DW5.PakFrameStatisticsOffset = params.pakFrameStatisticsOffset;
    cmd.DW6.CuLevelStreamoutOffset = params.cuLevelStreamoutOffset;
    cmd.DW7.SliceSizeStreamoutOffset = params.sliceSizeStreamoutOffset;
    cmd.DW8.CuRecordOffset = params.cuRecordOffset;
    cmd.DW9.SseRowstoreOffset = params.sseRowstoreOffset;
    cmd.DW10.SaoRowstoreOffset = params.saoRowstoreOffset;
    cmd.DW11.TileSizeStreamoutOffset = params.tileSizeStreamoutOffset;
    return packetFrom(cmd);
}

}
}
