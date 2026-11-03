// SPDX-License-Identifier: BSD-3-Clause
// Assembles the Gen12 HEVC VDEnc/HCP picture sequence for one frame. The
// packet layouts and field meanings come from media/hevc_core; this file only
// binds the shim's surfaces and buffers and lays the packets out in the order
// media-driver uses for a single-slice VDEnc picture.
#include "hevc_commands.hpp"
#include "hevc_buffer_sizes.hpp"

#include "../hevc_core/hcp_pak_insert.hpp"
#include "../hevc_core/hcp_slice.hpp"
#include "../hevc_core/huc_commands.hpp"
#include "../hevc_core/picture_commands.hpp"
#include "../hevc_core/vdenc_weights.hpp"
#include "../hevc_core/vdenc_walker.hpp"

namespace VaShim {
namespace {

using namespace HevcCore;

constexpr uint32_t kMiBatchBufferEnd = 0x05000000;
// MI_BATCH_BUFFER_START, DwordLength 1: header plus low/high address. Bit 22
// marks the target as a second-level batch, and bit 8 selects the PPGTT
// address space the media engine uses. Without both bits the engine treats the
// sub-batch as a nested GGTT buffer and never runs the VDEnc picture commands.
constexpr uint32_t kMiBatchBufferStart = 0x18C00101;

uint32_t surfaceFormat(const HevcSurface &surface) {
    // P010 surfaces use the VDEnc 10-bit format; the plug-in's 8-bit NV12
    // surfaces use planar 4:2:0.
    if (surface.format == 0x78343230 || surface.format == 0x78663230 ||
        surface.format == 0x50303130)
        return VdEnc::FormatP010;
    return VdEnc::FormatPlanar420_8;
}

// CoreVideo negotiates x420/xf20 for Main10; IOSurface fourccs are in
// character order, unlike libva's little-endian VA_FOURCC('P','0','1','0').
bool tenBitSurface(const HevcSurface &surface) {
    return surface.format == 0x78343230 || surface.format == 0x78663230 ||
        surface.format == 0x50303130;
}

bool validQpDepth(const HevcPictureDesc &desc) {
    return desc.minCodingBlockLog2 >= 3 && desc.minCodingBlockLog2 <= 6 &&
        desc.diffCuQpDeltaDepth <= 6 - desc.minCodingBlockLog2;
}

Hcp::SurfaceState hcpSurface(uint32_t surfaceId, const HevcSurface &surface,
    bool tenBit) {
    Hcp::SurfaceState state;
    state.surfaceId = surfaceId;
    state.pitch = surface.pitch;
    state.uvRowOffset = surface.uvRowOffset;
    // AddHcpEncodeSurfaceStateCmd distinguishes raw P010 from the hardware's
    // split MSB/LSB reconstructed format. The decode builder is different.
    state.format = !tenBit ? Hcp::FormatPlanar420_8 :
        surfaceId == Hcp::SurfaceSource ? Hcp::FormatP010 : Hcp::FormatP010Variant;
    state.defaultAlpha = 0;
    state.memoryCompressionEnable = 0;
    state.compressionType = 0;
    state.compressionFormat = 0;
    return state;
}

VdEnc::SurfaceState vdencSurface(const HevcSurface &surface,
    bool tenBit = false, bool reconstruction = false) {
    VdEnc::SurfaceState state;
    state.width = surface.width;
    state.height = surface.height;
    state.pitch = surface.pitch;
    state.uvRowOffset = surface.uvRowOffset;
    // Gen12's raw/recon mappers select P010/P010VARIANT. Its tracked 4x/8x
    // reconstructions remain NV12 even when the full picture is Main10.
    state.format = (tenBit || surfaceFormat(surface) == VdEnc::FormatP010)
        ? (reconstruction ? VdEnc::FormatP010Variant : VdEnc::FormatP010)
        : VdEnc::FormatPlanar420_8;
    state.tileY = true;
    state.byteSwizzle = false;
    state.colorSpaceSelect = false;
    state.chromaDownsampleFilter = 7;
    state.vDirection = 0;
    return state;
}

HevcCore::Hcp::AddressBlock block(uint64_t address, uint32_t control = 0) {
    HevcCore::Hcp::AddressBlock result;
    result.address = address;
    result.control = control;
    return result;
}

HevcCore::VdEnc::AddressBlock vdencBlock(uint64_t address, uint32_t control = 0) {
    HevcCore::VdEnc::AddressBlock result;
    result.address = address;
    result.control = control;
    return result;
}

uint64_t endAddress(const HevcPictureDesc &desc, const HevcBuffer &buffer) {
    // With resolved graphics addresses the upper bound is the end of the
    // allocation and the PAK/bitstream writers clamp their output to it. The
    // batch is always built from resolved addresses; without them the whole
    // picture is a placeholder, so the bound value is irrelevant.
    (void)desc;
    return static_cast<uint64_t>(buffer.address) + buffer.size;
}

uint32_t sliceType(uint32_t codingType, bool lowDelayB) {
    // AVD coding types are 0 I, 1 P, 2 B; HCP slice types are 2 I, 1 P, 0 B.
    if (codingType == 0)
        return 2;
    // The gen12 HCP slice state documents that a VDEnc HEVC slice is B or I
    // only, and media-driver's low-delay mode codes every inter picture as a B
    // slice whose list 1 repeats the single list 0 reference. A plain P slice
    // here leaves the PAK waiting for an object stream it never consumes.
    if (lowDelayB)
        return 0;
    return 1;
}

// media-driver's m_lowDelay: the inter picture is coded as a low-delay B
// slice. Both lists name the same reference picture.
bool codesLowDelayB(const HevcPictureDesc &desc) {
    return desc.inter && desc.lowDelay;
}

PictureParams pictureParams(const HevcPictureDesc &desc) {
    const uint32_t type = sliceType(desc.codingType, codesLowDelayB(desc));
    const bool intra = type == 2;
    // Diagnostic: keep the P slice syntax but run the VDEnc in its intra-only
    // picture mode, so every coding unit it hands the PAK is intra. Separates
    // "the PAK cannot code inter CUs with residual" from "the PAK cannot code a
    // P slice at all".
    const bool vdencIntra = intra || desc.interIntraOnly;
    // The picture's bit depth comes from the source surface the plug-in hands
    // over; every surface state below has to describe the same depth.
    const bool tenBit = tenBitSurface(desc.source);

    PictureParams params;
    params.hcpPipeMode.streamOut = true;
    params.hcpPipeMode.advancedRateControl = desc.rateControlEnabled;
    params.hcpPipeMode.frameReconstructionDisable = false;
    params.hcpPipeMode.pipeWorkingMode = 0;
    params.hcpPipeMode.phaseIndicator = 0;

    params.sourceSurface = hcpSurface(Hcp::SurfaceSource, desc.source, tenBit);
    params.reconSurface = hcpSurface(Hcp::SurfaceDecoded, desc.recon, tenBit);
    // media-driver's SetHcpRefSurfaceParams describes the reconstruction
    // surface here, not the picture being predicted from: the reference
    // address itself comes from the pipe buffer state. The two differ when a
    // reference picture was allocated with another pitch.
    params.referenceSurface = hcpSurface(Hcp::SurfaceReference, desc.recon, tenBit);

    const HevcSurface &reference = desc.reference.valid ? desc.reference : desc.recon;
    params.pipeBuf.decodedPicture = block(desc.recon.address);
    params.pipeBuf.originalUncompressed = block(desc.source.address);
    // HCP PAK statistics and the compressed bitstream have independent
    // writers. Sharing their base destroys the output NAL units.
    params.pipeBuf.streamout = block(desc.buffers.hcpStreamout.address);
    params.pipeBuf.encodedSliceSize = block(desc.buffers.encodedSliceSize.address);
    params.pipeBuf.references[0] = block(reference.address);
    // DW53 carries the cache policy for every reference address plus the
    // tiling mode of the first reference surface. The plug-in's surfaces are
    // Y-tiled, which maps to TRMODE_NONE (Mhw_ConvertToTRMode), and the default
    // codec cache policy leaves the MOCS index clear.
    params.pipeBuf.referenceControl =
        HevcCore::Hcp::referenceAttributes(HevcCore::Hcp::TileMode::Y);

    // HCP and VDEnc run concurrently. Each HCP line/column store needs an
    // independent backing range; sharing an intra row store or temporal MV
    // buffer with another writer corrupts state once a picture spans rows.
    auto hcpScratch = [&](uint32_t region) {
        return block(static_cast<uint64_t>(desc.buffers.hcpScratch.address) +
            (desc.resolvedAddresses ? region * kHcpScratchRegionBytes : 0));
    };
    params.pipeBuf.deblockingLine = hcpScratch(0);
    params.pipeBuf.deblockingTileLine = hcpScratch(1);
    params.pipeBuf.deblockingTileColumn = hcpScratch(2);
    params.pipeBuf.metadataLine = hcpScratch(3);
    params.pipeBuf.metadataTileLine = hcpScratch(4);
    params.pipeBuf.metadataTileColumn = hcpScratch(5);
    params.pipeBuf.saoLine = hcpScratch(6);
    params.pipeBuf.saoTileLine = hcpScratch(7);
    params.pipeBuf.saoTileColumn = hcpScratch(8);
    params.pipeBuf.currentMvTemporal = hcpScratch(9);
    params.pipeBuf.lcuIldb = hcpScratch(10);
    params.pipeBuf.collocatedMv[0] = hcpScratch(14);
    // The PAK writes its per-tile frame record here; media-driver gives that
    // record its own resource (m_resPakStatsBuffer) instead of sharing the
    // VDEnc statistics surface the plug-in provides (m_resVdencStatsBuffer).
    params.pipeBuf.frameStatistics = hcpScratch(17);
    params.pipeBuf.sseSourceRowstore = block(desc.buffers.sseRowstore.address);
    params.pipeBuf.mvUpperRight = hcpScratch(11);
    params.pipeBuf.intraUpperRight = hcpScratch(12);
    params.pipeBuf.intraLeftRecon = hcpScratch(13);

    // media-driver's encoder mapping: HCP_IND_OBJ_BASE_ADDR_STATE names the
    // CU/MV object surface in the indirect-CU field and the bitstream in the
    // PAK BSE field. The PAK object commands themselves reach the PAK from the
    // VDEnc (slot 46), so the CU field must not alias that area.
    params.indObj.bitstreamObject = block(0);
    params.indObj.bitstreamUpperBound = block(0);
    // media-driver keeps both in one allocation: the LCU PAK object commands
    // start at the surface base (the VDENC slot 46 buffer) and the CU records
    // follow after the object block. The block is sized for the smallest
    // coding unit the HEVC standard allows, not for the 64x64 CTU:
    // m_mvOffset = ALIGN(ceil(w/16) * ceil(h/16) * GetHcpPakObjSize() * 4,
    // 4096), so the CU-record base must sit past every object the VDEnc can
    // write, not just past one 64-byte object per CTU.
    const uint64_t cuObjectOffset = hevcBufferSizes(desc.width, desc.height).cuObjectOffset;
    params.indObj.cuObject = block(desc.buffers.pakBse.address + cuObjectOffset);
    if (desc.cuObjectAlias)
        params.indObj.cuObject = block(desc.buffers.cuRecord.address);
    if (desc.cuObjectAtBase)
        params.indObj.cuObject = block(desc.buffers.pakBse.address);
    params.indObj.pakBseObject = block(desc.buffers.bitstream.address);
    params.indObj.pakBseUpperBound =
        block(endAddress(desc, desc.buffers.bitstream));

    params.vdencPipeMode.main10 = tenBit;
    // Stream-in carries lookahead/ROI records and is only valid when the
    // picture was prepared for it. This path is single-pass CQP, so the VDEnc
    // must not wait for stream-in data that was never produced.
    params.vdencPipeMode.streamIn = false;
    params.vdencPipeMode.frameStatistics = desc.rateControlEnabled;
    // Single pass: the VDEnc normally hands PAK objects to the PAK over the
    // internal path. media-driver's gen12 encoder also sets the stream-out bit
    // for the first pass, so the picture object commands are visible in the
    // object buffer; the diagnostic keeps the two paths selectable.
    params.vdencPipeMode.pakObjCmdStreamOut = desc.pakObjectStreamOut;
    // media-driver: VDENC_PIPE_MODE_SELECT.RandomAccess = !m_lowDelay. The
    // low-delay profile clears it; the random-access profile sets it.
    params.vdencPipeMode.randomAccess = !desc.lowDelay;
    params.vdencPipeMode.chromaType = 1;
    // media-driver never disables the downscale write. The picture writes its
    // own 4x/8x downscale into its pool slot while it reads the reference
    // picture's downscale from the slot that picture wrote, so the two never
    // alias.
    params.vdencPipeMode.ds8xWriteDisable = false;
    params.vdencPipeMode.ds4xWriteDisable = false;

    params.vdencSource = vdencSurface(desc.source, tenBit);
    // Same as the HCP surface state: the VDEnc reference surface state
    // describes the reconstruction surface, and the reference picture's
    // address comes from the pipe buffer slots.
    params.vdencReference = vdencSurface(desc.recon, tenBit, true);
    params.downscaled4x = vdencSurface(
        hevcScaledSurface(desc.width, desc.height, false));
    params.downscaled8x = vdencSurface(
        hevcScaledSurface(desc.width, desc.height, true));

    params.vdencBuf.originalUncompressed = vdencBlock(desc.source.address);
    params.vdencBuf.streaminData = vdencBlock(desc.buffers.streamIn.address);
    params.vdencBuf.rowStoreScratch = vdencBlock(desc.buffers.rowStore.address);
    // media-driver programs the reference's temporal MV buffer for the VDEnc on
    // every HEVC picture, even when temporal prediction is off. The same buffer
    // backs the HCP collocated-MV slot, so the two engines agree on its layout.
    params.vdencBuf.colocatedMv = vdencBlock(hcpScratch(14).address);
    params.vdencBuf.cuRecordStreamout = vdencBlock(desc.buffers.cuRecord.address);
    params.vdencBuf.lcuPakObjCmd = vdencBlock(desc.buffers.pakBse.address);
    // media-driver allocates and binds both gen12-only VDEnc buffers for every
    // picture: the tile row store (ceil(width/32) * 2 cache lines) and the
    // cumulative CU count surface (one DWORD per LCU). The VDEnc writes them
    // while it walks the picture, so a clear address is not a valid "unused"
    // value.
    params.vdencBuf.tileRowStore = vdencBlock(hcpScratch(15).address);
    params.vdencBuf.cumulativeCuCountStreamout = vdencBlock(hcpScratch(16).address);
    params.vdencBuf.fwdRef0 = vdencBlock(reference.address);
    params.vdencBuf.statisticsStreamout = vdencBlock(desc.buffers.statistics.address);
    // One 4x/8x downscale recon surface per tracked picture, addressed by slot
    // inside the pool. media-driver does the same with a per-picture pool index
    // (Get4x/8xDsReconSurface(scaledIdx)): the VDEnc writes this picture's
    // downscale into its own slot and reads the reference's from the slot the
    // reference picture wrote.
    const uint32_t scaled4xStride = hevcScaledSurfaceBytes(
        hevcScaledSurface(desc.width, desc.height, false));
    const uint32_t scaled8xStride = hevcScaledSurfaceBytes(
        hevcScaledSurface(desc.width, desc.height, true));
    const uint64_t write4x = desc.buffers.scaled4x.address +
        static_cast<uint64_t>(desc.scaledSlot) * scaled4xStride;
    const uint64_t write8x = desc.buffers.scaled8x.address +
        static_cast<uint64_t>(desc.scaledSlot) * scaled8xStride;
    params.vdencBuf.scaledRef4x = vdencBlock(write4x);
    params.vdencBuf.scaledRef8x = vdencBlock(write8x);
    if (!intra && desc.referenceScaledValid) {
        const uint64_t read4x = desc.buffers.scaled4x.address +
            static_cast<uint64_t>(desc.referenceScaledSlot) * scaled4xStride;
        const uint64_t read8x = desc.buffers.scaled8x.address +
            static_cast<uint64_t>(desc.referenceScaledSlot) * scaled8xStride;
        params.vdencBuf.dsFwdRef0 = vdencBlock(read8x);
        params.vdencBuf.dsFwdRef0_4x = vdencBlock(read4x);
    }

    params.cmd1.intra = vdencIntra;
    params.cmd1.qpY = desc.qpY;
    params.cmd1.sliceQpDelta = 0;

    params.pic.widthInMinCbMinus1 = (desc.width + 7) / 8 - 1;
    params.pic.heightInMinCbMinus1 = (desc.height + 7) / 8 - 1;
    // The SPS and this field describe the same coding-block range; the VDEnc
    // and the PAK must split CUs identically or the CU records go out of sync.
    params.pic.log2MinCodingBlockMinus3 = desc.minCodingBlockLog2 - 3;
    params.pic.chromaFormatIdc = 1;
    params.pic.saoEnabled = false;
    // Constant QP: the VDEnc has no ACQP/BRC/stream-in input. The PPS and this
    // field still have to agree with what the PAK codes.
    params.pic.cuQpDeltaEnabled = desc.cuQpDeltaEnabled;
    params.pic.diffCuQpDeltaDepth = desc.diffCuQpDeltaDepth;
    params.pic.log2MaxTransformBlockMinus2 = desc.maxTransformLog2 - 2;
    params.pic.maxTransformHierarchyDepthIntra = desc.transformDepth;
    params.pic.maxTransformHierarchyDepthInter = desc.transformDepth;
    params.pic.ampEnabled = true;
    params.pic.strongIntraSmoothing = true;
    params.pic.useVdenc = true;
    params.pic.partialFrameUpdate = desc.partialFrameUpdate;
    // media-driver: HCP_PIC_STATE.DW19.TemporalMvPredDisable =
    // !sps_temporal_mvp_enable_flag (and forced to 1 under a partial frame
    // update). The SPS builder writes that flag as one, so the picture state
    // must advertise temporal MV prediction; keeping it disabled diverges from
    // the reference on exactly the pictures that can use it.
    params.pic.temporalMvp = desc.temporalMvpEnabled;
    params.pic.lcuMaxBitsizeAllowed = desc.lcuMaxBitsizeAllowed;
    params.pic.qpY = desc.qpY;
    // media-driver writes bit_depth_luma_minus8 / bit_depth_chroma_minus8
    // from the sequence parameters; the HCP picture state and the SPS have to
    // agree or the PAK reads the source at the wrong sample width.
    params.pic.bitDepthLumaMinus8 = tenBit ? 2 : 0;
    params.pic.bitDepthChromaMinus8 = tenBit ? 2 : 0;

    params.cmd2.frameWidth = desc.width;
    params.cmd2.frameHeight = desc.height;
    params.cmd2.intra = vdencIntra;
    // Gen12 BRC's CU-QP DMEM zones require the matching VDEnc adaptive-QP
    // controls in the input SLB (media-driver bUseDefaultQpDeltas). Merely
    // enabling delta syntax in the PPS/HCP picture does not enable them.
    params.cmd2.useDefaultQpDeltas = desc.rateControlEnabled && desc.cuQpDeltaEnabled;
    // PictureType comes straight from media-driver's ternary
    // (CodingType == I_TYPE ? 0 : (bIsLowDelayB ? 3 : 2)). A low-delay B codes
    // as type 3 and its slice header carries list 1, which is what the two
    // values have to agree on.
    params.cmd2.lowDelayB = codesLowDelayB(desc);
    // Every inter picture on this path is a low-delay B, so the type above
    // wins; the field still carries the plain P value for the shaped builder.
    params.cmd2.pictureType = intra ? 0 : 1;
    params.cmd2.qpY = desc.qpY;
    params.cmd2.pakOnlyMultipass = desc.brcPakOnlyPass;
    params.cmd2.targetUsage = 4;
    params.cmd2.streamIn = false;
    // media-driver's gen12 packet drives this from sps_temporal_mvp_enabled_flag
    // for non-I pictures. The slice header this path writes leaves temporal MVP
    // clear, so the VDEnc keeps the matching value; the diagnostic raises it to
    // the reference value.
    params.cmd2.temporalMvp = desc.interTemporalMvp;
    // VDENC_CMD2 carries the active reference count minus one. This path
    // codes one L0 reference for a P slice and none for an I slice.
    params.cmd2.numRefIdxL0 = 0;
    params.cmd2.numRefIdxL1 = 0;
    params.cmd2.refL0[0] = intra ? 0xFF : 0;
    params.cmd2.refIdxMapping[0] = intra ? 7 : 0;
    // The VDEnc reference slots carry the picture order count difference.
    params.cmd2.currPoc = desc.poc;
    params.cmd2.refPocL0[0] = desc.refPoc;
    params.cmd2.refPocL1[0] = desc.refPoc;

    return params;
}

Hcp::SliceParams sliceParams(const HevcPictureDesc &desc) {
    Hcp::SliceParams params;
    params.widthInCtb = (desc.width + 63) / 64;
    params.ctbAddress = 0;
    // media-driver programs (0,0) for the last slice's next-slice position.
    // The diagnostic variant ends the slice one CTB past the frame.
    params.nextCtbAddress = desc.sliceEndAtFrameEnd
        ? params.widthInCtb * ((desc.height + 63) / 64) : 0;
    params.endAtNextCtb = desc.sliceEndAtFrameEnd;
    params.lastSlice = true;
    params.sliceType = sliceType(desc.codingType, codesLowDelayB(desc));
    params.lastSliceInTile = true;
    // The picture is one tile and one slice, so this slice is also the last
    // slice of the single tile column.
    params.lastSliceInTileColumn = true;
    params.qpY = desc.qpY;
    params.maxNumMergeCand = 5;
    params.vdencInUse = true;
    params.intraRefFetchDisable = desc.brcPakOnlyPass;
    // media-driver passes m_lowDelay into the slice state's isLowDelay bit.
    params.lowDelay = desc.lowDelay;
    params.sliceHeaderBytes = (desc.sliceHeaderBits + 7) / 8;
    // The PAK needs the SAO and deblocking line buffers to run those filters.
    // This path does not provide them yet, so the picture is coded without
    // either filter and the slice header matches.
    params.saoLuma = false;
    params.saoChroma = false;
    params.deblockingFilterDisable = true;
    params.loopFilterAcrossSlices = false;
    return params;
}

// The picture is one tile and one pipe. media-driver emits HCP_TILE_CODING
// before the slice commands of every tile; the command supplies the tile
// geometry in minimum coding blocks and the per-tile stream-out offsets.
Hcp::TileCodingParams tileCodingParams(const HevcPictureDesc &desc) {
    Hcp::TileCodingParams params;
    params.activeBePipes = 1;
    params.tileColumnsInFrame = 1;
    params.tileRowStoreSelect = 0;
    params.tileColumnStoreSelect = 0;
    params.tileStartLcuX = 0;
    params.tileStartLcuY = 0;
    params.lastTileOfColumn = true;
    params.lastTileOfRow = true;
    params.tileWidthInMinCbMinus1 = (desc.width + 7) / 8 - 1;
    params.tileHeightInMinCbMinus1 = (desc.height + 7) / 8 - 1;
    // Tile replay and the stream-out offsets stay at the single-tile,
    // single-pipe defaults; the buffers they address are bound at offset 0.
    params.bitstreamByteOffsetEnable = false;
    params.pakFrameStatisticsOffset = 0;
    params.cuLevelStreamoutOffset = 0;
    params.sliceSizeStreamoutOffset = 0;
    params.cuRecordOffset = 0;
    params.sseRowstoreOffset = 0;
    params.saoRowstoreOffset = 0;
    params.tileSizeStreamoutOffset = 0;
    return params;
}

VdEnc::WalkerParams walkerParams(const HevcPictureDesc &desc) {
    VdEnc::WalkerParams params;
    params.widthInPix = desc.width;
    params.heightInPix = desc.height;
    params.numParEngine = 0;
    // The walker runs until the next-slice position. iHD derives it from the
    // slice's LCU count; for the single full-frame slice that is the position
    // one past the last CTB, i.e. the frame size in CTB units. Leaving both
    // fields at zero makes the VDEnc walk zero CTBs, so the PAK never receives
    // an object stream and the HEVC pipeline never signals completion.
    const uint32_t widthInCtb = (desc.width + 63) / 64;
    const uint32_t heightInCtb = (desc.height + 63) / 64;
    const uint32_t lcusInSlice = widthInCtb * heightInCtb;
    params.startXCtb = 0;
    params.startYCtb = 0;
    params.nextXCtb = lcusInSlice / heightInCtb;
    params.nextYCtb = lcusInSlice / widthInCtb;
    if (desc.oneCtb) {
        params.nextXCtb = 1;
        params.nextYCtb = 1;
    }
    return params;
}

// Diagnostic probe: copy the same pixels out of the reference and the source
// through the engine's own path, one picture after the reference was written.
// Equal values mean the previous reconstruction is visible to this submission;
// different values mean the reference surface is not what the earlier picture
// left in it.
void appendReferenceProbe(std::vector<Packet> &packets, const HevcPictureDesc &desc) {
    if (!desc.inter || !desc.traceCheckpoints || !desc.resolvedAddresses ||
        desc.buffers.statistics.size < 512)
        return;
    // Luma row offsets, then chroma row offsets inside the interleaved plane.
    constexpr uint32_t lumaRows[] = {0, 90, 300, 600};
    constexpr uint32_t chromaRows[] = {0, 200};
    const uint32_t base = desc.buffers.statistics.address +
        desc.buffers.statistics.size - 256;
    uint32_t sample = 0;
    auto probe = [&](uint32_t offset) {
        for (uint32_t which = 0; which < 2; ++which) {
            Packet copy(5);
            copy[0] = 0x17000003; // MI_COPY_MEM_MEM, source/destination PPGTT
            copy[1] = (base + (sample * 2 + which) * 4) & ~3u;
            copy[2] = 0;
            copy[3] = (which ? desc.source.address : desc.reference.address) + offset;
            copy[4] = 0;
            packets.push_back(copy);
        }
        ++sample;
    };
    for (uint32_t r = 0; r < 4; ++r)
        probe(lumaRows[r] * 1920);
    for (uint32_t r = 0; r < 2; ++r)
        probe((1088 + chromaRows[r]) * 1920);
}

void appendInsert(std::vector<Packet> &packets, const uint8_t *data,
    uint32_t bitSize, bool lastHeader, bool emulate) {
    if (!data || bitSize == 0)
        return;
    Hcp::PakInsertParams insert;
    insert.data = data;
    insert.bitSize = bitSize;
    insert.lastHeader = lastHeader;
    insert.endOfSlice = false;
    insert.emulationByteInsert = emulate;
    // The four-byte Annex B start code and two-byte NAL header are framing,
    // not RBSP. Escaping them turns 00 00 00 01 into 00 00 03 00 01.
    insert.skipEmulationByteCount = emulate ? 6 : 0;
    // Normal access-unit headers leave the bitstream position alone; the
    // hardware advances it after each insert. Only dynamic-slice control
    // resets the starting position.
    insert.resetBitstreamStartingPos = false;
    const Hcp::PakInsertCommand command = Hcp::pakInsert(insert);
    packets.push_back(command.header);
    // The payload follows the header in the command buffer, word aligned.
    const uint32_t dwords = static_cast<uint32_t>(command.payload.size() / 4);
    if (dwords == 0)
        return;
    // The payload is appended as a word packet so the assembler keeps one
    // representation for the whole batch.
    Packet payload(dwords);
    for (uint32_t i = 0; i < dwords; ++i) {
        payload[i] = static_cast<uint32_t>(command.payload[i * 4])
            | (static_cast<uint32_t>(command.payload[i * 4 + 1]) << 8)
            | (static_cast<uint32_t>(command.payload[i * 4 + 2]) << 16)
            | (static_cast<uint32_t>(command.payload[i * 4 + 3]) << 24);
    }
    packets.push_back(payload);
}

void appendPakInsert(std::vector<Packet> &packets, const HevcPictureDesc &desc) {
    // The first PAK insert carries the parameter sets; the second carries the
    // slice header and marks it as the last header of the access unit.
    // Parameter sets already contain emulation prevention, independently for
    // each NAL. Hardware emulation across the combined units corrupts their
    // start codes. The single slice NAL uses hardware emulation after framing.
    appendInsert(packets, desc.parameterSets, desc.parameterSetBytes * 8, false, false);
    appendInsert(packets, desc.sliceHeader, desc.sliceHeaderBits, true, true);
}

Packet batchBufferStart(uint32_t address) {
    Packet packet(3);
    packet[0] = kMiBatchBufferStart;
    packet[1] = address & ~3u;
    packet[2] = 0;
    return packet;
}

std::vector<Packet> hucPackets(const HevcPictureDesc &desc) {
    Huc::Params params;
    params.kernelDescriptor = desc.huc.kernelDescriptor;
    params.dmemAddress = desc.huc.dmemAddress;
    params.dmemLength = desc.huc.dmemLength;
    params.dmemOffset = desc.huc.dmemOffset;
    for (uint32_t i = 0; i < Huc::kRegionCount; ++i) {
        params.regions[i].address = desc.huc.regions[i].address;
        params.regions[i].offset = desc.huc.regions[i].offset;
        params.regions[i].writable = desc.huc.regions[i].writable;
        params.regions[i].used = desc.huc.regions[i].used;
    }
    return Huc::pakIntegrate(params);
}

// Diagnostic checkpoint: a 64-bit immediate store into the statistics buffer.
// MI_STORE_DATA_IMM with a fixed header, address low/high, and the marker in
// the first payload DWORD.
Packet checkpoint(const HevcPictureDesc &desc, uint32_t marker) {
    Packet packet(5);
    packet[0] = 0x10000003;
    // Each group stores to its own dword so the buffer keeps the whole trace
    // even when a later group never runs. The VDEnc streams its frame
    // statistics over the front of this allocation, so the trace lives in the
    // last 512 bytes, next to the completion tag and the MMIO samples.
    const uint32_t trace = desc.buffers.statistics.size >= 512
        ? desc.buffers.statistics.size - 512 : 0;
    packet[1] = (desc.buffers.statistics.address + trace + marker * 4) & ~3u;
    packet[2] = 0;
    packet[3] = marker;
    packet[4] = 0;
    return packet;
}

// MI_STORE_REGISTER_MEM for a register inside the media MMIO window.
// DW0 asks the hardware to add the executing engine's CS MMIO base, so the
// register is named by its offset inside that window. The offset goes into the
// 21-bit RegisterAddress field unchanged: the reads that verified this path
// return the frame byte count and the image status for a completed picture
// (hevc_context's "frame bytes"/"image status" log lines).
Packet mmioRead(uint32_t relativeRegister, uint64_t destination,
    bool relative = true) {
    Packet packet(4);
    packet[0] = relative ? 0x12080002 : 0x12000002;
    packet[1] = relativeRegister & (relative ? 0x3FFFu : 0x7ffffcu);
    packet[2] = static_cast<uint32_t>(destination & ~3u);
    packet[3] = static_cast<uint32_t>(destination >> 32);
    return packet;
}

// Temporary phase-4 reconstruction probe; retained only in the private capture.
void appendGpuSamples(std::vector<Packet> &packets,
    const HevcPictureDesc &desc, bool after) {
    if (!desc.traceCheckpoints || !desc.resolvedAddresses || desc.poc != 0 ||
        desc.buffers.statistics.size < 4096)
        return;
    const uint32_t rows[] = {0, 1, 16, 31, 32, 63, 64, 90, 127, 128,
        192, 384, 540, 768, 1024, 1072, 1079};
    const uint32_t cols[] = {0, 32, 64, 128, 480, 960, 1440, 1916};
    const uint32_t count = 17 * 8 * 2;
    for (uint32_t group = after ? 2 : 0; group < (after ? 3u : 2u); ++group) {
        const auto &surface = group == 0 ? desc.source : desc.recon;
        for (uint32_t tiled = 0; tiled < 2; ++tiled) {
            for (uint32_t r = 0; r < 17; ++r) {
                for (uint32_t c = 0; c < 8; ++c) {
                    const uint32_t x = cols[c] < surface.width - 3
                        ? cols[c] : (surface.width - 4) & ~3u;
                    const uint32_t y = rows[r] < surface.height
                        ? rows[r] : surface.height - 1;
                    const uint32_t offset = tiled
                        ? (y / 32) * surface.pitch * 32 + (x / 128) * 4096
                            + ((x % 128) / 16) * 512 + (y % 32) * 16 + x % 16
                        : y * surface.pitch + x;
                    const uint32_t sample = group * count + tiled * 17 * 8 + r * 8 + c;
                    Packet copy(5);
                    copy[0] = 0x17000003; // MI_COPY_MEM_MEM, source/destination PPGTT
                    copy[1] = desc.buffers.statistics.address + desc.buffers.statistics.size
                        - 4096 + sample * 4;
                    copy[2] = 0;
                    copy[3] = surface.address + offset;
                    copy[4] = 0;
                    packets.push_back(copy);
                }
            }
        }
    }
}

std::vector<Packet> assemble(const HevcPictureDesc &desc) {
    const PictureParams params = pictureParams(desc);

    std::vector<Packet> packets;
    auto addCheckpoint = [&](uint32_t marker) {
        if (desc.traceCheckpoints)
            packets.push_back(checkpoint(desc, marker));
    };
    appendGpuSamples(packets, desc, false);
    appendReferenceProbe(packets, desc);
    auto first = pictureFirstLevel(params);
    if (desc.rateControlEnabled)
        first[0][0] = 0x0e800000; // Gen12's two-DWORD MI_FORCE_WAKEUP
    packets.insert(packets.end(), first.begin(), first.end());
    if (desc.pictureFlush)
        packets.push_back(flushDw());
    addCheckpoint(1);
    if (desc.brcLastImageStatusAddress) {
        Packet restore(4);
        restore[0] = 0x14880002; // MI_LOAD_REGISTER_MEM, CS-relative, PPGTT
        restore[1] = 0x28bc;
        restore[2] = desc.brcLastImageStatusAddress;
        packets.push_back(restore);
    }
    if (desc.brcBatchOutput) {
        packets.push_back(batchBufferStart(desc.buffers.secondLevel.address +
            desc.brcPictureOffset));
    } else if (desc.huc.enabled || desc.secondLevelBatch) {
        // Picture state lives in its own buffer: the HuC kernel reads it from
        // there, and for the non-HuC path media-driver starts the same buffer
        // as a second-level batch.
        packets.push_back(batchBufferStart(desc.buffers.secondLevel.address));
    } else {
        const auto second = pictureSecondLevel(params);
        packets.insert(packets.end(), second.begin(), second.end());
    }
    addCheckpoint(2);

    // Slice level, in media-driver's SendHwSliceEncodeCommand order: the
    // reference list, the PAK slice state and the NAL/slice-header inserts are
    // queued first, then the VDEnc weights/offsets state and the walker that
    // starts the VDEnc. The walker must be last: it is the command that makes
    // the VDEnc produce the PAK objects the slice state is waiting for.
    // media-driver emits HCP_TILE_CODING once per tile before the slice
    // commands. Without it the PAK falls back to the command defaults, which
    // describe a one-minimum-CB tile instead of the whole frame.
    packets.push_back(Hcp::tileCodingState(tileCodingParams(desc)));
    if (desc.inter) {
        // One L0 entry pointing at reference base address 0. The PAK uses the
        // POC difference to build the reference picture list.
        Hcp::RefIdxParams refs;
        refs.list = 0;
        refs.numRefs = 1;
        refs.entries[0].frameId = 0;
        // The command field is 8 bits; wrap to the shortest signed distance
        // so a reference across the POC LSB boundary keeps its sign.
        int32_t delta = static_cast<int32_t>(desc.poc)
            - static_cast<int32_t>(desc.refPoc);
        delta = ((delta + 128) & 0xFF) - 128;
        refs.entries[0].pocDelta = delta;
        packets.push_back(Hcp::refIdxState(refs));
        // A low-delay B slice carries both lists and media-driver sends one
        // HCP_REF_IDX_STATE per list. List 1 repeats the single list 0 entry,
        // which is what m_lowDelay means here: both lists name the previous
        // picture.
        if (codesLowDelayB(desc)) {
            refs.list = 1;
            packets.push_back(Hcp::refIdxState(refs));
        }
    }
    if (desc.brcBatchOutput) {
        packets.push_back(batchBufferStart(desc.buffers.secondLevel.address +
            desc.brcSliceOffset));
    } else {
        packets.push_back(Hcp::sliceState(sliceParams(desc)));
        addCheckpoint(3);
        if (!desc.skipPak)
            appendPakInsert(packets, desc);
        addCheckpoint(4);

        VdEnc::WeightsParams weights;
        weights.weightedPred = false;
        packets.push_back(VdEnc::weightsOffsetsState(weights));
    }
    addCheckpoint(5);

    if (!desc.skipWalker)
        packets.push_back(VdEnc::walkerState(walkerParams(desc)));
    addCheckpoint(6);

    // media-driver's completion order (codechal_vdenc_hevc.cpp:2493-2534):
    // the per-slice flush waits for the VDEnc, MFX and the command parser and
    // flushes the VDEnc pipeline (0x2001A); the picture-level flush then waits
    // for and flushes the PAK (0x10011); a cache-invalidating MI_FLUSH_DW sits
    // before and after the picture flush. The per-slice word must not carry the
    // HEVC command flush (0x10000): the reference flushes only the VDEnc there,
    // and flushing the PAK between slices drops the state its coding units
    // depend on.
    packets.push_back(VdEnc::pipelineFlush(0x0002001A));
    addCheckpoint(7);
    if (desc.traceCheckpoints) {
        // Sample the PAK's own progress before the cache-invalidating flush can
        // block. The registers come from the gen12 HCP MMIO map
        // (mhw_vdbox_hcp_impl.h: *_REG_OFFSET_INIT): frame byte count, image
        // status control and mask, slice count, HCP command counter, CABAC
        // status, SE bit count and the VDEnc mode timer. A stalled HEVC
        // pipeline then still reports which stage it reached instead of
        // leaving the completion tag untouched. The samples go to the last 128
        // bytes of the statistics buffer: the VDEnc streams its per-CTU
        // statistics into the front of that buffer, which overwrites a probe
        // placed there. MI_STORE_REGISTER_MEM takes the register offset masked
        // to the engine window (IsRelativeMMIO: 0x3FFF) and relative to the
        // media MMIO base the executing engine adds back.
        constexpr uint32_t kPakStatusRegisters[] = {
            0x28A0, // HCP_ENC_BIT_STREAM_BYTE_COUNT_FRAME
            0x28BC, // HCP_ENC_IMAGE_STATUS_CTRL
            0x28C8, // HCP_ENC_SLICE_COUNT
            0x2820, // HCP_COMMAND_COUNTER
            0x2804, // HCP_CABAC_STATUS
            0x28A8, // HCP_ENC_BIT_STREAM_SE_BIT_COUNT_FRAME
            0x28C0, // HCP_ENC_QP_STATUS_COUNT: counts coded QPs, i.e. progress
            0x28A4, // HCP_ENC_BIT_STREAM_BYTE_COUNT_FRAME_NO_HEADER
            0x28DC, // HCP_ENC_VDENC_MODE_TIMER
            0x34,   // VCS0 ring head: a small ring offset, proves the read path
            0x38,   // VCS0 ring start: the submission's ring GPU address
        };
        constexpr uint32_t kPakStatusCount =
            sizeof(kPakStatusRegisters) / sizeof(kPakStatusRegisters[0]);
        const uint64_t probe = desc.buffers.statistics.address +
            desc.buffers.statistics.size - 128;
        for (uint32_t i = 0; i < kPakStatusCount; ++i) {
            packets.push_back(mmioRead(kPakStatusRegisters[i],
                probe + i * 4));
        }
    }
    addCheckpoint(8);
    // media-driver invalidates the media pipeline cache before waiting for the
    // PAK and again after it; both flushes carry the same invalidate bit.
    packets.push_back(flushDw());
    packets.push_back(VdEnc::pipelineFlush(0x00010011));
    addCheckpoint(9);
    packets.push_back(flushDw());
    addCheckpoint(10);
    if (desc.brcBatchOutput) {
        packets.push_back(mmioRead(0x28A0, desc.brcPakInfoAddress));
        Packet pass(5);
        pass[0] = 0x10000002; // one DWORD plus a NOOP
        pass[1] = desc.brcPakInfoAddress + 4;
        pass[3] = desc.brcPass;
        packets.push_back(pass);
    }
    appendGpuSamples(packets, desc, true);
    if (desc.huc.enabled) {
        const auto huc = hucPackets(desc);
        packets.insert(packets.end(), huc.begin(), huc.end());
        addCheckpoint(11);
    }
    if (desc.probeHucStatus && desc.resolvedAddresses &&
        desc.buffers.statistics.valid && desc.buffers.statistics.address &&
        desc.buffers.statistics.size >= 128) {
        // The existing HCP probes occupy the first 44 bytes of this tail;
        // the completion marker starts at +64. Keep these samples between.
        const uint64_t sample = uint64_t(desc.buffers.statistics.address) +
            desc.buffers.statistics.size - 128 + 44;
        packets.push_back(mmioRead(Huc::kGucStatusRegister, sample - 16, false));
        packets.push_back(mmioRead(Huc::kGucWopcmSizeRegister, sample - 12, false));
        packets.push_back(mmioRead(Huc::kGucWopcmOffsetRegister, sample - 8, false));
        packets.push_back(mmioRead(Huc::kBiosWopcmRegister, sample - 4, false));
        if (desc.probeHucKernel < 256) {
            const auto load = Huc::kernelLoad(desc.probeHucKernel);
            for (uint32_t i = 0; i < load.size(); ++i) {
                packets.push_back(load[i]);
                if (i == 1) // after IMEM's wait, before pipe mode select
                    packets.push_back(mmioRead(Huc::kStatus2Register, sample + 8));
            }
        }
        packets.push_back(mmioRead(Huc::kStatusRegister, sample));
        packets.push_back(mmioRead(Huc::kStatus2Register, sample + 4));
        packets.push_back(mmioRead(Huc::kKernelLoadInfoRegister, sample + 12, false));
    }
    if (desc.completionAddress && desc.completionTag) {
        // Read the current VDBOX's HCP status after the drains. The slice-size
        // streamout and VDEnc statistics are not the frame byte count.
        for (uint32_t i = 0; i < 2; ++i) {
            Packet status(4);
            status[0] = 0x12080002; // SRM, PPGTT, relative CS MMIO
            status[1] = i == 0 ? 0x28A0 : 0x28BC; // byte count / image status
            status[2] = desc.completionAddress + 8 + i * 4;
            packets.push_back(status);
        }
        Packet complete = flushDw();
        complete[0] |= 1u << 14; // MI_FLUSH_DW: write immediate, PPGTT
        complete[1] = desc.completionAddress;
        complete[3] = desc.completionTag;
        packets.push_back(complete);
    }
    return packets;
}

bool validBrcBatchOutput(const HevcPictureDesc &desc) {
    if (!desc.brcBatchOutput)
        return true;
    if (!desc.resolvedAddresses || !desc.secondLevelBatch ||
        !desc.rateControlEnabled || desc.huc.enabled || desc.skipPak ||
        !desc.buffers.secondLevel.valid || !desc.buffers.secondLevel.address ||
        (desc.buffers.secondLevel.address & 3) || !desc.brcBatchBytes ||
        desc.brcBatchBytes >= 0xffff || (desc.brcBatchBytes & 3) ||
        desc.brcBatchBytes > desc.buffers.secondLevel.size ||
        uint64_t(desc.buffers.secondLevel.address) + desc.brcBatchBytes > uint64_t(1) << 32 ||
        (desc.brcPictureOffset & 3) || (desc.brcSliceOffset & 3) ||
        desc.brcPictureOffset < 40 || desc.brcSliceOffset >= desc.brcBatchBytes ||
        !desc.brcPakInfoAddress || (desc.brcPakInfoAddress & 3) ||
        desc.brcPakInfoAddress > 0xfffffff8 || desc.brcPass > 1 ||
        (desc.brcLastImageStatusAddress &&
            ((desc.brcLastImageStatusAddress & 3) ||
                uint64_t(desc.brcLastImageStatusAddress) < desc.buffers.statistics.address ||
                uint64_t(desc.brcLastImageStatusAddress) + 4 >
                    uint64_t(desc.buffers.statistics.address) + desc.buffers.statistics.size)) ||
        !desc.buffers.statistics.valid || !desc.buffers.statistics.address)
        return false;
    const auto second = pictureSecondLevel(pictureParams(desc));
    uint32_t pictureBytes = 4; // MI_BATCH_BUFFER_END
    for (const auto &packet : second)
        pictureBytes += packet.size() * 4;
    // Check full groups before serializing anything. The slice contains its
    // state, a PAK-insert header/payload, weights, a batch end and HuC padding.
    const uint32_t minimumSliceBytes = Hcp::sliceState(sliceParams(desc)).size() * 4 +
        8 + 4 + VdEnc::weightsOffsetsState(VdEnc::WeightsParams {}).size() * 4 + 4 + 32;
    return uint64_t(desc.brcPictureOffset) + pictureBytes <= desc.brcSliceOffset &&
        uint64_t(desc.brcSliceOffset) + minimumSliceBytes <= desc.brcBatchBytes;
}

}

// The picture's bit depth is the bit depth of the source surface the plug-in
// hands over: P010 fourcc means 10-bit, NV12 means 8-bit.
bool pictureIsTenBit(const HevcPictureDesc &desc) {
    return tenBitSurface(desc.source);
}

HevcSurface hevcScaledSurface(uint32_t width, uint32_t height, bool eightX) {
    auto align = [](uint32_t value, uint32_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    };
    HevcSurface surface;
    surface.width = align(width / 4, 16);
    // iHD reserves two tile-aligned fields for the 4x reconstruction.
    surface.height = align((align(height / 4, 16) + 1) / 2, 32) * 2;
    if (eightX) {
        surface.width /= 2;
        surface.height = align(surface.height / 2, 32) * 2;
    }
    surface.pitch = align(surface.width, 128);
    surface.uvRowOffset = surface.height;
    surface.format = 0x34323076; // NV12 video range
    return surface;
}

uint32_t hevcScaledSurfaceBytes(const HevcSurface &surface) {
    const uint32_t chromaRows = (surface.height / 2 + 31) & ~31u;
    return surface.pitch * (surface.height + chromaRows);
}

uint32_t hevcPictureCommandDwords(const HevcPictureDesc &desc) {
    if (!validQpDepth(desc) || !validBrcBatchOutput(desc))
        return 0;
    const std::vector<Packet> packets = assemble(desc);
    uint32_t dwords = 0;
    for (const Packet &packet : packets)
        dwords += packet.size();
    // A NOOP keeps MI_BATCH_BUFFER_END at an even DWORD offset.
    if ((dwords & 1) == 0)
        ++dwords;
    return dwords + 1;
}

uint32_t buildHevcPictureCommands(const HevcPictureDesc &desc,
    uint32_t *destination, uint32_t capacityDwords) {
    if (!destination)
        return 0;
    const uint32_t needed = hevcPictureCommandDwords(desc);
    if (!needed || needed > capacityDwords)
        return 0;

    const std::vector<Packet> packets = assemble(desc);
    uint32_t offset = 0;
    for (const Packet &packet : packets) {
        for (uint32_t i = 0; i < packet.size(); ++i)
            destination[offset++] = packet[i];
    }
    if ((offset & 1) == 0)
        destination[offset++] = 0; // MI_NOOP before the batch end
    destination[offset++] = kMiBatchBufferEnd;
    return offset;
}

uint32_t hevcSecondLevelCommandDwords(const HevcPictureDesc &desc) {
    const PictureParams params = pictureParams(desc);
    const std::vector<Packet> packets = pictureSecondLevel(params);
    uint32_t dwords = 0;
    for (const Packet &packet : packets)
        dwords += packet.size();
    if ((dwords & 1) == 0)
        ++dwords;
    return dwords + 1;
}

uint32_t buildHevcSecondLevelCommands(const HevcPictureDesc &desc,
    uint32_t *destination, uint32_t capacityDwords) {
    if (!destination)
        return 0;
    if (hevcSecondLevelCommandDwords(desc) > capacityDwords)
        return 0;

    const PictureParams params = pictureParams(desc);
    const std::vector<Packet> packets = pictureSecondLevel(params);
    uint32_t offset = 0;
    for (const Packet &packet : packets) {
        for (uint32_t i = 0; i < packet.size(); ++i)
            destination[offset++] = packet[i];
    }
    if ((offset & 1) == 0)
        destination[offset++] = 0;
    destination[offset++] = kMiBatchBufferEnd;
    return offset;
}

uint32_t buildHevcBrcBatchCommands(const HevcPictureDesc &desc,
    uint32_t *destination, uint32_t capacityDwords, HevcBrcBatchLayout &layout) {
    if (!destination || !validQpDepth(desc) || !desc.sliceHeader || !desc.sliceHeaderBits ||
        desc.sliceHeaderBits >= 0xffff || desc.parameterSetBytes > 8191 ||
        (desc.parameterSetBytes && !desc.parameterSets) || desc.qpY > 51)
        return 0;
    auto params = pictureParams(desc);
    params.hcpPipeMode.advancedRateControl = true;
    std::vector<Packet> packets;
    uint32_t dwords = 0;
    auto appendPacket = [&](const Packet &packet) {
        packets.push_back(packet);
        dwords += packet.size();
    };
    auto endGroup = [&]() {
        Packet end(1);
        end[0] = kMiBatchBufferEnd;
        appendPacket(end);
    };
    appendPacket(mfxWait(true));
    appendPacket(Hcp::pipeModeSelect(params.hcpPipeMode));
    appendPacket(mfxWait(true));
    endGroup();

    HevcBrcBatchLayout result;
    result.update.cmd1Offset = uint16_t(dwords * 4);
    const auto second = pictureSecondLevel(params);
    appendPacket(second[0]);
    result.update.pictureOffset = uint16_t(dwords * 4);
    appendPacket(second[1]);
    result.update.cmd2Offset = uint16_t(dwords * 4);
    appendPacket(second[2]);
    endGroup();

    result.slice.startOffset = dwords * 4;
    result.slice.sliceStateOffset = dwords * 4;
    appendPacket(Hcp::sliceState(sliceParams(desc)));
    // The parameter-set insert precedes the slice PIO only on an IRAP.
    std::vector<Packet> headers;
    appendInsert(headers, desc.parameterSets, desc.parameterSetBytes * 8,
        false, false);
    for (const auto &packet : headers)
        appendPacket(packet);
    result.slice.sliceHeaderOffset = dwords * 4;
    headers.clear();
    appendInsert(headers, desc.sliceHeader, desc.sliceHeaderBits, true, true);
    for (const auto &packet : headers)
        appendPacket(packet);
    result.slice.vdencWeightsOffset = dwords * 4;
    VdEnc::WeightsParams weights;
    weights.lowDelay = desc.lowDelay;
    appendPacket(VdEnc::weightsOffsetsState(weights));
    endGroup();
    // codec_def_encode_hevc.h: ENCODE_VDENC_HEVC_PADDING_DW_SIZE = 8.
    appendPacket(Packet(8));
    result.slice.commandsBytes = dwords * 4 - result.slice.startOffset;
    result.slice.sliceHeaderBits = desc.sliceHeaderBits;
    if (dwords > capacityDwords || dwords * 4 >= 0xffff)
        return 0;
    result.update.batchBytes = uint16_t(dwords * 4);
    result.update.frameId = desc.frameNumber;
    result.update.frameType = desc.inter ? Huc::BrcFrameType::LowDelay
        : Huc::BrcFrameType::Intra;
    result.update.referencesL0 = desc.inter ? 1 : 0;
    result.update.referencesL1 = desc.inter ? 1 : 0;
    result.update.qp = uint8_t(desc.qpY);
    result.update.sliceHeaderBits = desc.sliceHeaderBits;
    result.update.cuQpControl = desc.cuQpDeltaEnabled;
    uint32_t offset = 0;
    for (const auto &packet : packets)
        for (uint32_t i = 0; i < packet.size(); ++i)
            destination[offset++] = packet[i];
    layout = result;
    return offset;
}

bool validateHevcBrcBatchOutput(const HevcPictureDesc &desc,
    const HevcBrcBatchLayout &layout, const uint32_t *commands,
    uint32_t capacityDwords, uint32_t &qp) {
    const auto &update = layout.update;
    const auto &slice = layout.slice;
    HevcCore::Huc::BrcUpdateDmem validated {};
    if (!commands || !update.batchBytes || (update.batchBytes & 3) ||
        update.batchBytes / 4 > capacityDwords ||
        !HevcCore::Huc::buildBrcUpdateDmem(update, validated))
        return false;
    const uint32_t cmd1 = update.cmd1Offset / 4;
    const uint32_t pic = update.pictureOffset / 4;
    const uint32_t cmd2 = update.cmd2Offset / 4;
    const uint32_t first = slice.startOffset / 4;
    const uint32_t end = update.batchBytes / 4;
    const auto expectedSlice = HevcCore::Hcp::sliceState(sliceParams(desc));
    if ((slice.startOffset & 3) || first > end || end - first < expectedSlice.size() ||
        uint64_t(slice.startOffset) + slice.commandsBytes > update.batchBytes ||
        update.cmd1Offset + 31 * 4 != update.pictureOffset ||
        update.pictureOffset + 41 * 4 != update.cmd2Offset ||
        update.cmd2Offset + 52 * 4 != slice.startOffset)
        return false;
    const auto params = pictureParams(desc);
    const auto expectedPic = HevcCore::Hcp::picState(params.pic);
    if (commands[cmd1] != HevcCore::VdEnc::cmd1(params.cmd1)[0] ||
        commands[pic] != expectedPic[0] || commands[pic + 1] != expectedPic[1] ||
        commands[pic + 5] != expectedPic[5] ||
        ((commands[pic + 4] ^ expectedPic[4]) & (7u << 5)) ||
        commands[cmd2] != HevcCore::VdEnc::cmd2(params.cmd2)[0] ||
        commands[cmd2 + 51] != kMiBatchBufferEnd ||
        commands[first] != expectedSlice[0] ||
        (commands[first + 3] & 3) != sliceType(desc.codingType, codesLowDelayB(desc)))
        return false;
    const uint32_t pictureQp = (commands[pic + 19] >> 8) & 63;
    const uint32_t vdencQp = (commands[cmd2 + 27] >> 8) & 255;
    const uint32_t sliceQp = (commands[first + 3] >> 6) & 63;
    // HuC writes the HCP statistics QP with HEVC's bit-depth offset, while
    // VDEnc and the slice header retain QpY. Intel's HEVC status reporting
    // subtracts 12 from accumulated Main10 QP before returning QpY.
    const uint32_t qpOffset = tenBitSurface(desc.source) ? 12 : 0;
    if (vdencQp > 51 || pictureQp != vdencQp + qpOffset || sliceQp > 51 ||
        (!desc.cuQpDeltaEnabled && vdencQp != sliceQp) ||
        (commands[first + 3] & (1u << 3)))
        return false;
    uint32_t at = first + expectedSlice.size();
    bool header = false, weights = false, finished = false;
    while (at < end) {
        const uint32_t word = commands[at];
        if (word == kMiBatchBufferEnd) {
            finished = true;
            break;
        }
        if (!word) { ++at; continue; }
        const uint32_t length = (word & 0xfff) + 2;
        if (length > end - at)
            return false;
        if ((word & ~0xfffu) == 0x73a20000 && !weights) {
            if (length < 3)
                return false;
            header = true;
        } else if (word == 0x70880001 && header && !weights) {
            weights = true;
        } else {
            return false;
        }
        at += length;
    }
    if (!finished || !header || !weights)
        return false;
    qp = vdencQp;
    return true;
}

}
