// CPU check for the shim's Gen12 HEVC VDEnc/HCP picture sequence. It verifies
// the packet order, the surface and buffer addresses, and the batch terminator.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../media/va_shim/hevc_commands.cpp"
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/command_buffer.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_hcp_hwcmd_g12_X.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_vdenc_hwcmd_g12_X.cpp"
#include "../media/hevc_core/hcp_state.cpp"
#include "../media/hevc_core/hcp_picture.cpp"
#include "../media/hevc_core/hcp_qm.cpp"
#include "../media/hevc_core/hcp_slice.cpp"
#include "../media/hevc_core/hcp_pak_insert.cpp"
#include "../media/hevc_core/vdenc_state.cpp"
#include "../media/hevc_core/vdenc_picture.cpp"
#include "../media/hevc_core/vdenc_walker.cpp"
#include "../media/hevc_core/vdenc_weights.cpp"
#include "../media/hevc_core/picture_commands.cpp"
#include "../media/hevc_core/huc_commands.cpp"
#include "../media/hevc_core/huc_brc_const.cpp"
#include "../media/hevc_core/huc_brc_dmem.cpp"

namespace {

using namespace VaShim;

uint32_t bits(uint32_t word, uint32_t lo, uint32_t hi) {
    const uint32_t width = hi - lo + 1;
    return (word >> lo) & (width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1);
}

bool isHcp(uint32_t word, uint32_t command) {
    return bits(word, 23, 26) == 7 && bits(word, 16, 22) == command;
}

bool isVdenc(uint32_t word, uint32_t subop) {
    return bits(word, 23, 26) == 1 && bits(word, 16, 22) == subop;
}
bool isVdPipelineFlush(uint32_t word) {
    return bits(word, 23, 26) == 0xF && bits(word, 16, 22) == 0;
}

bool contains(const uint32_t *words, uint32_t count, uint32_t value) {
    for (uint32_t i = 0; i < count; ++i) {
        if (words[i] == value)
            return true;
    }
    return false;
}

// HCP and VDEnc media commands count their length as DWORDs minus two.
uint32_t packetLength(uint32_t header) {
    return (header & 0xFFF) + 2;
}

uint32_t findLastHcp(const uint32_t *words, uint32_t count, uint32_t command) {
    for (uint32_t i = count; i > 0; --i) {
        if (isHcp(words[i - 1], command))
            return i - 1;
    }
    return count;
}

uint32_t findFirst(const uint32_t *words, uint32_t count, uint32_t value) {    for (uint32_t i = 0; i < count; ++i) {
        if (words[i] == value)
            return i;
    }
    return count;
}

uint32_t findLastVdenc(const uint32_t *words, uint32_t count, uint32_t subop) {    for (uint32_t i = count; i > 0; --i) {
        if (isVdenc(words[i - 1], subop))
            return i - 1;
    }
    return count;
}

HevcSurface surface(uint32_t address, uint32_t pitch, uint32_t height) {
    HevcSurface result;
    result.address = address;
    result.uvAddress = address + pitch * height;
    result.pitch = pitch;
    result.uvRowOffset = height;
    result.width = 1920;
    result.height = height;
    result.format = 0x34323066; // '420f'
    result.valid = true;
    return result;
}

}

int main() {
    HevcPictureDesc desc;
    desc.width = 1920;
    desc.height = 1080;
    desc.qpY = 26;
    desc.frameNumber = 1;
    desc.codingType = 0; // I
    desc.source = surface(0x12345000, 1920, 1080);
    desc.recon = surface(0x12360000, 1920, 1088);
    desc.reference = desc.recon;
    desc.buffers.bitstream = {0x12400000, 1u << 20, true};
    desc.buffers.pakBse = {0x12500000, 1u << 19, true};
    desc.buffers.cuRecord = {0x12580000, 1u << 19, true};
    desc.buffers.streamIn = {0x12600000, 1u << 18, true};
    desc.buffers.rowStore = {0x12640000, 1u << 18, true};
    desc.buffers.encodedSliceSize = {0x12680000, 1u << 16, true};
    desc.buffers.statistics = {0x12690000, 1u << 18, true};
    desc.buffers.tileRecord = {0x12700000, 1u << 18, true};
    desc.buffers.aggregatedStats = {0x12740000, 1u << 18, true};
    desc.buffers.brcHistory = {0x12780000, 1u << 18, true};
    desc.buffers.brcData = {0x127c0000, 1u << 18, true};
    desc.buffers.scaled4x = {0x12800000, 1u << 18, true};
    desc.buffers.scaled8x = {0x12840000, 1u << 18, true};
    desc.buffers.hcpStreamout = {0x12900000, 1u << 20, true};
    desc.buffers.sseRowstore = {0x12a00000, 1u << 18, true};
    desc.buffers.hcpScratch = {0x13000000,
        kHcpScratchRegionBytes * kHcpScratchRegions, true};

    // The chroma row offset must describe the graphics-address spacing, not
    // the CPU mapping: the surface manager pads the GPU allocation further
    // (a 368-line picture maps chroma at row 368 on the CPU and row 384 in
    // the GPU allocation). Reading the CPU value makes the chroma plane start
    // 16 rows of padding early, which the hardware encodes as black chroma.
    assert(HevcCore::uvRowOffsetFromAddresses(
        0x40020000u, 0x4005c000u, 640) == 384);
    assert(HevcCore::uvRowOffsetFromAddresses(
        0x40020000u, 0x40020000u + 368u * 640u, 640) == 368);
    assert(HevcCore::uvRowOffsetFromAddresses(0x40020000u, 0x40020100u, 640) == 0);
    assert(HevcCore::uvRowOffsetFromAddresses(0x40020000u, 0x40020000u, 640) == 0);
    assert(HevcCore::uvRowOffsetFromAddresses(0x40020000u, 0x4005c000u, 0) == 0);
    // Both chroma fields carry the same row offset for a 4:2:0 picture.
    desc.source.uvRowOffset = 384;
    const auto surfacePacket = HevcCore::Hcp::surfaceState(
        hcpSurface(0, desc.source, false));
    assert(bits(surfacePacket[2], 0, 14) == 384);   // YOffsetForUCb
    assert(bits(surfacePacket[3], 16, 31) == 384);  // YOffsetForVCr

    // Gen12's encode builders use P010 for the source, P010VARIANT for
    // reconstruction and NV12 for both downscaled reconstructions. The raw
    // and reconstructed VDEnc formats come from the Gen12 mapper, whose
    // values differ from the generated header's older format descriptions.
    {
        HevcPictureDesc ten = desc;
        ten.source.format = 0x78343230; // CoreVideo 'x420'
        ten.source.pitch = 3840;
        ten.recon.format = 0x78343230;
        ten.recon.pitch = 3840;
        ten.reference = ten.recon;
        assert(pictureIsTenBit(ten));
        const auto tenParams = pictureParams(ten);
        assert(tenParams.sourceSurface.format == 13);
        assert(tenParams.reconSurface.format == 3);
        assert(tenParams.referenceSurface.format == 3);
        assert(tenParams.vdencSource.format == 13);
        assert(tenParams.vdencReference.format == 3);
        assert(tenParams.downscaled4x.format == 4);
        assert(tenParams.downscaled8x.format == 4);
        assert(tenParams.downscaled4x.pitch == 512);
        assert(tenParams.downscaled8x.pitch == 256);
        const auto tenSource = HevcCore::VdEnc::srcSurfaceState(tenParams.vdencSource);
        const auto tenRecon = HevcCore::VdEnc::refSurfaceState(tenParams.vdencReference, 0);
        assert(bits(tenSource[3], 27, 31) == 13);
        assert(bits(tenRecon[3], 27, 31) == 3);
        assert(bits(tenSource[3], 3, 19) == 3839);
        assert(tenParams.vdencPipeMode.main10);
        assert(tenParams.pic.bitDepthLumaMinus8 == 2);
        assert(tenParams.pic.bitDepthChromaMinus8 == 2);
        const auto tenPic = HevcCore::Hcp::picState(tenParams.pic);
        assert(bits(tenPic[5], 27, 29) == 2);   // luma bit depth minus 8
        assert(bits(tenPic[5], 24, 26) == 2);   // chroma bit depth minus 8
        // The 8-bit descriptor from the same builder stays on PLANAR4208.
        assert(!pictureIsTenBit(desc));
        const auto eightParams = pictureParams(desc);
        assert(eightParams.sourceSurface.format == HevcCore::Hcp::FormatPlanar420_8);
        assert(!eightParams.vdencPipeMode.main10);
        assert(eightParams.pic.bitDepthLumaMinus8 == 0);
        ten.source.format = 0x78663230; // CoreVideo 'xf20'
        assert(pictureIsTenBit(ten));
        ten.source.format = 0x50303130; // IOSurface 'P010'
        assert(pictureIsTenBit(ten));
        ten.source.format = 0x78343232; // 10-bit 4:2:2 is outside this path
        assert(!pictureIsTenBit(ten));
    }

    // iHD's tracked NV12 reconstruction surfaces at 1920x1080. Both are
    // Y-tiled outputs, including for the first intra picture.
    const auto ds4 = hevcScaledSurface(1920, 1080, false);    const auto ds8 = hevcScaledSurface(1920, 1080, true);
    assert(ds4.width == 480 && ds4.height == 320 && ds4.pitch == 512);
    assert(ds8.width == 240 && ds8.height == 320 && ds8.pitch == 256);
    assert(ds4.uvRowOffset == 320 && ds8.uvRowOffset == 320);
    assert(hevcScaledSurfaceBytes(ds4) == 245760);
    assert(hevcScaledSurfaceBytes(ds8) == 122880);
    const auto small = hevcScaledSurface(640, 360, false);
    assert(small.width == 160 && small.height == 128 && small.pitch == 256);
    const auto mapping = pictureParams(desc);
    assert(mapping.vdencBuf.scaledRef4x.address == desc.buffers.scaled4x.address);
    assert(mapping.vdencBuf.scaledRef8x.address == desc.buffers.scaled8x.address);
    const auto dsPacket = HevcCore::VdEnc::dsRefSurfaceState(
        mapping.downscaled4x, mapping.downscaled8x);
    assert(bits(dsPacket[2], 4, 17) == 239); // 8x first
    assert(bits(dsPacket[6], 4, 17) == 479); // 4x second

    // media-driver gives the PAK frame statistics record its own resource, so
    // the plug-in's statistics surface carries only the VDEnc stream-out.
    assert(mapping.vdencBuf.statisticsStreamout.address ==
        desc.buffers.statistics.address);
    assert(mapping.pipeBuf.frameStatistics.address != 0);
    assert(mapping.pipeBuf.frameStatistics.address !=
        desc.buffers.statistics.address);

    const uint32_t dwords = hevcPictureCommandDwords(desc);
    assert(dwords > 40);
    assert((dwords & 1) == 0);

    uint32_t buffer[8192] {};
    assert(dwords <= sizeof(buffer) / sizeof(buffer[0]));
    const uint32_t written = buildHevcPictureCommands(desc, buffer, dwords);
    assert(written == dwords);
    assert(buffer[written - 1] == 0x05000000);

    // Picture level commands first, in the media-driver order. Each media
    // packet occupies the DwordLength + 1 words its header declares.
    uint32_t at = 0;
    // The prolog starts with MI_FORCE_WAKEUP (MI command 1Dh, length 1) so the
    // HEVC and MFX power wells are on before the VDEnc and PAK run.
    assert(bits(buffer[at], 23, 28) == 29);
    assert(bits(buffer[at], 29, 31) == 0);
    assert((buffer[at] & 0xFFF) == 1);
    assert(buffer[at + 1] == 0x03000300u);
    at += 2;

    // HCP_PIC_STATE DWORD 6 carries the per-LCU bit budget. iHD replaces a
    // plug-in zero with CalcLCUMaxCodingSize() = 81920 for a 64x64 CTU,
    // 4:2:0, 8-bit picture; the command field keeps the low 16 bits.
    {
        const auto second = HevcCore::pictureSecondLevel(pictureParams(desc));
        bool checked = false;
        for (const auto &packet : second) {
            if (!isHcp(packet[0], 0x10))
                continue;
            // Preserve unsplit 32x32 TUs, while allowing 16x16 CUs to split
            // through 8x8 into sixteen 4x4 TUs. Depth one breaks that tree.
            assert(bits(packet[2], 6, 7) == 3); // maximum TU 32x32
            assert(bits(packet[5], 10, 12) == 2); // intra transform depth
            assert(bits(packet[5], 13, 15) == 2); // inter transform depth
            assert(packet[6] == 0x4000u);
            checked = true;
        }
        assert(checked);
    }

    // Diagnostic switches used for the per-row flow-control A/B: the default
    // last-slice next position is media-driver's (0,0); the variant ends the
    // slice one CTB past the frame. The picture state can also carry partial
    // frame update mode.
    {
        assert(sliceParams(desc).nextCtbAddress == 0);
        HevcPictureDesc sliceEnd = desc;
        sliceEnd.sliceEndAtFrameEnd = true;
        const auto endParams = sliceParams(sliceEnd);
        assert(endParams.widthInCtb == 30 && endParams.nextCtbAddress == 30u * 17u);
        const auto endPacket = HevcCore::Hcp::sliceState(endParams);
        // One CTB past the 30x17 CTB frame is raster position 510 = (0, 17).
        assert(bits(endPacket[2], 0, 9) == 0);     // next slice X
        assert(bits(endPacket[2], 16, 25) == 17);  // next slice Y
        assert(bits(endPacket[3], 2, 2) == 1);     // last slice of the picture

        HevcPictureDesc partial = desc;
        partial.partialFrameUpdate = true;
        bool sawPartial = false;
        for (const auto &packet : HevcCore::pictureSecondLevel(
                pictureParams(partial))) {
            if (!isHcp(packet[0], 0x10))
                continue;
            assert(bits(packet[19], 30, 30) == 1); // PartialFrameUpdateMode
            sawPartial = true;
        }
        assert(sawPartial);

        // This path has no collocated-MV buffer and writes zero for temporal
        // MVP in the slice header, so the SPS and the picture state keep it
        // off by default. Enabling the flag clears the picture-state disable.
        auto temporalDisable = [](const HevcPictureDesc &which) {
            bool value = true;
            for (const auto &packet : HevcCore::pictureSecondLevel(
                    pictureParams(which))) {
                if (isHcp(packet[0], 0x10))
                    value = bits(packet[19], 31, 31) == 1;
            }
            return value;
        };
        assert(!desc.temporalMvpEnabled);
        assert(temporalDisable(desc));
        HevcPictureDesc withTemporalMvp = desc;
        withTemporalMvp.temporalMvpEnabled = true;
        assert(!temporalDisable(withTemporalMvp));
    }
    assert(isVdenc(buffer[at], 0xB)); // VDENC_CONTROL_STATE
    at += packetLength(buffer[at]);
    assert(isHcp(buffer[at], 0xA)); // VD_CONTROL_STATE
    at += packetLength(buffer[at]);
    // Gen12 AddHcpPipeModeSelectCmd waits before and after the mode switch.
    assert(buffer[at++] == 0x68000100);
    assert(isHcp(buffer[at], 0)); // HCP_PIPE_MODE_SELECT
    at += packetLength(buffer[at]);
    assert(buffer[at++] == 0x68000100);
    assert(isHcp(buffer[at], 1)); // HCP_SURFACE_STATE source
    assert(bits(buffer[at + 2], 27, 31) == 4); // NV12, not packed YUY2
    at += packetLength(buffer[at]);
    assert(isHcp(buffer[at], 1)); // HCP_SURFACE_STATE recon
    assert(bits(buffer[at + 2], 27, 31) == 4); // NV12, not packed YUY2
    at += packetLength(buffer[at]);
    assert(isHcp(buffer[at], 1)); // HCP_SURFACE_STATE reference
    assert(bits(buffer[at + 2], 27, 31) == 4); // NV12, not packed YUY2
    at += packetLength(buffer[at]);
    assert(isHcp(buffer[at], 2)); // HCP_PIPE_BUF_ADDR_STATE
    at += packetLength(buffer[at]);
    assert(isHcp(buffer[at], 3)); // HCP_IND_OBJ_BASE_ADDR_STATE
    at += packetLength(buffer[at]);
    assert(isHcp(buffer[at], 5)); // HCP_FQM_STATE
    at += packetLength(buffer[at]);

    // Slice level follows media-driver's SendHwSliceEncodeCommand order: the
    // PAK slice state and its inserts are queued first, then the VDEnc
    // weights/offsets state, then the walker that starts the VDEnc walk. The
    // walker has to be last, because it is the command that makes the VDEnc
    // produce the PAK objects the slice state is waiting for.
    const uint32_t slice = findLastHcp(buffer, written, 20);
    const uint32_t weights = findLastVdenc(buffer, written, 8);
    const uint32_t walker = findLastVdenc(buffer, written, 7);
    assert(slice < weights);
    assert(weights < walker);
    assert(walker < written - 1);
    // HCP_TILE_CODING describes the single tile and precedes the slice
    // commands. media-driver sends it per tile; without it the PAK falls back
    // to the command defaults, which describe a one-minimum-CB tile.
    const uint32_t tilePacket = findLastHcp(buffer, written, 0x15);
    assert(tilePacket < slice);
    assert(bits(buffer[tilePacket + 1], 0, 7) == 1);    // one active BE pipe
    assert(bits(buffer[tilePacket + 1], 16, 31) == 1);  // one tile column
    assert(bits(buffer[tilePacket + 2], 30, 31) == 3);  // last tile row/column
    assert(bits(buffer[tilePacket + 3], 0, 10) == 134);   // 1080 / 8 - 1
    assert(bits(buffer[tilePacket + 3], 16, 26) == 239);  // 1920 / 8 - 1
    // The single slice is the last slice of the picture, of its tile and of
    // the tile column; the PAK waits for the column flag before completing.
    assert((buffer[slice + 3] & 0x03000000) == 0x03000000);
    // Unity weights and zero offsets: weighted prediction is off for this
    // single-pass I picture, but the command is still sent.
    assert(buffer[weights + 1] == 0x00010001);
    assert(buffer[weights + 2] == 0x00010001);
    // The walker must run to the end of the frame. The next-slice position is
    // the frame size in CTB units: 1920x1080 is 30x17 CTBs.
    assert((buffer[walker + 2] & 0x3FF) == 17);          // next slice Y
    assert(((buffer[walker + 2] >> 16) & 0x3FF) == 30);  // next slice X

    // Tail, in media-driver's order: the per-slice flush waits for the VDEnc,
    // MFX and the parser and flushes the VDEnc pipeline; the picture-level
    // flush waits for and flushes the HEVC (PAK) pipeline; a cache-invalidating
    // MI_FLUSH_DW precedes and follows the picture flush. The per-slice word
    // must not set the HEVC command flush (0x10000), which would drop the PAK
    // state between slices.
    auto miLength = [](uint32_t header) { return (header & 0x3F) + 2; };
    uint32_t tail = walker + packetLength(buffer[walker]);
    assert(isVdPipelineFlush(buffer[tail]) && buffer[tail + 1] == 0x0002001A);
    tail += packetLength(buffer[tail]);
    assert(buffer[tail] == 0x13000083); // MI_FLUSH_DW, cache invalidate
    tail += miLength(buffer[tail]);
    assert(isVdPipelineFlush(buffer[tail]) && buffer[tail + 1] == 0x00010011);
    tail += packetLength(buffer[tail]);
    assert(buffer[tail] == 0x13000083); // MI_FLUSH_DW, cache invalidate
    tail += miLength(buffer[tail]);
    if (tail < written - 1)
        assert(buffer[tail] == 0); // MI_NOOP keeps the batch end DWORD aligned
    assert(buffer[written - 1] == 0x05000000);

    // Every bound address is referenced by the sequence.
    assert(contains(buffer, written, 0x12345000)); // source plane
    assert(contains(buffer, written, 0x12360000)); // recon plane
    assert(contains(buffer, written, 0x12400000)); // bitstream
    assert(contains(buffer, written, 0x12500000)); // PAK BSE
    assert(contains(buffer, written, 0x12580000)); // CU record
    assert(contains(buffer, written, 0x12600000)); // stream-in
    assert(contains(buffer, written, 0x12640000)); // row store

    // HCP_PIPE_BUF_ADDR_STATE must not leave a line, tile, column or
    // statistics slot null: the PAK reads and writes those buffers even when
    // the deblocking and SAO filters are off, and a null base address stalls
    // it. Slot dword offsets follow the Gen12 command layout: two DWORDs of
    // base address, then one DWORD of memory attributes.
    const uint32_t pipeBuf = findLastHcp(buffer, written, 2);
    assert(buffer[pipeBuf] == 0x73820077);
    assert(buffer[pipeBuf + 57] == desc.buffers.hcpStreamout.address);
    assert(buffer[pipeBuf + 57] != desc.buffers.bitstream.address);
    assert(buffer[pipeBuf + 101] == desc.buffers.sseRowstore.address);
    assert(buffer[pipeBuf + 101] != desc.buffers.rowStore.address);
    const uint32_t addressSlot[] = {
        1,                                    // decoded picture
        4, 7, 10,                             // deblocking line, tile line, tile column
        13, 16, 19,                           // metadata line, tile line, tile column
        22, 25, 28,                           // SAO line, tile line, tile column
        31,                                   // current motion vector temporal
        54, 57, 60, 63,                       // source, streamout, slice size, ILDB
        66,                                   // collocated motion vector
        98, 101,                              // frame statistics, SSE row store
        110, 113, 116,                        // upper-right / left-recon column stores
    };
    for (uint32_t offset : addressSlot) {
        assert(offset < 121);
        assert((buffer[pipeBuf + offset] & 0xFFFFFFC0u) != 0);
    }

    // A smaller destination is rejected instead of overwriting memory.
    assert(buildHevcPictureCommands(desc, buffer, dwords - 1) == 0);

    // With the picture state in its own buffer the ring batch starts that
    // buffer as a second-level batch and carries no inline picture state.
    desc.secondLevelBatch = true;
    desc.buffers.secondLevel = {0x126c0000, 1u << 16, true};
    const uint32_t batched = hevcPictureCommandDwords(desc);
    assert(batched < dwords);
    assert(batched <= sizeof(buffer) / sizeof(buffer[0]));
    assert(buildHevcPictureCommands(desc, buffer, batched) == batched);
    const uint32_t start = findFirst(buffer, batched, 0x18C00101);
    assert(start != batched);
    assert(buffer[start + 1] == 0x126c0000);
    desc.secondLevelBatch = false;

    // A slice header adds its PAK insert packet before the VDEnc commands.
    const uint8_t header[8] = {0, 0, 0, 1, 0x26, 0x01, 0xaf, 0x09};
    desc.sliceHeader = header;
    desc.sliceHeaderBits = sizeof(header) * 8;
    const uint32_t withHeader = hevcPictureCommandDwords(desc);
    assert(withHeader > dwords);
    assert(withHeader <= sizeof(buffer) / sizeof(buffer[0]));
    assert(buildHevcPictureCommands(desc, buffer, withHeader) == withHeader);
    const uint32_t insert = findLastHcp(buffer, withHeader, 34);
    const uint32_t headerWalker = findLastVdenc(buffer, withHeader, 7);
    assert(insert < headerWalker);
    assert(buffer[insert + 2] == 0x01000000);
    assert((buffer[insert + 1] & 0x8) != 0);
    assert(bits(buffer[insert + 1], 4, 7) == 6);
    assert(buffer[withHeader - 1] == 0x05000000);

    // Parameter sets are inserted before the slice header. The first PAK
    // insert carries them and does not mark the last header; the slice insert
    // does.
    const uint8_t parameterSets[12] = {0x40, 0x01, 0x0c, 0x01, 0xff, 0xff,
        0x01, 0x60, 0x00, 0x00, 0x03, 0x00};
    desc.parameterSets = parameterSets;
    desc.parameterSetBytes = sizeof(parameterSets);
    const uint32_t withSets = hevcPictureCommandDwords(desc);
    assert(withSets > withHeader);
    assert(withSets <= sizeof(buffer) / sizeof(buffer[0]));
    assert(buildHevcPictureCommands(desc, buffer, withSets) == withSets);
    uint32_t inserts = 0;
    uint32_t firstInsert = 0;
    for (uint32_t i = 0; i + 1 < withSets; ++i) {
        if (!isHcp(buffer[i], 34))
            continue;
        if (inserts == 0)
            firstInsert = i;
        ++inserts;
    }
    assert(inserts == 2);
    assert((buffer[firstInsert + 1] & 0x4) == 0); // parameter sets first
    assert((buffer[firstInsert + 1] & 0x8) == 0); // already escaped per NAL
    assert(buffer[firstInsert + 2] == 0x010c0140);
    assert(contains(buffer, withSets, 0x00030000));

    // Intel Gen12 BRC needs the slice group in the same SLB as the picture
    // group, with byte offsets to both PIO and hardware QP fields.
    HevcBrcBatchLayout brcLayout;
    uint32_t brcBatch[1024] {};
    HevcPictureDesc brcPicture = desc;
    brcPicture.rateControlEnabled = true;
    brcPicture.cuQpDeltaEnabled = true;
    const uint32_t brcWords = buildHevcBrcBatchCommands(brcPicture, brcBatch,
        1024, brcLayout);
    // Gen12 HCP_PIC_STATE is 41 DWORDs, between 31-DWORD CMD1 and CMD2.
    assert(brcWords == 168 && brcLayout.update.batchBytes == 672);
    assert(brcBatch[0] == 0x68000100);
    assert(brcBatch[9] == 0x05000000);
    assert(brcLayout.update.cmd1Offset == 40);
    assert(brcLayout.update.pictureOffset == 164);
    assert(brcLayout.update.cmd2Offset == 328);
    const uint32_t adaptiveCmd2 = brcLayout.update.cmd2Offset / 4;
    assert((brcBatch[adaptiveCmd2 + 13] & 0xffff0000u) == 0xf0120000u);
    assert((brcBatch[adaptiveCmd2 + 14] & 0xffffu) == 0x21dbu);
    assert((brcBatch[adaptiveCmd2 + 16] & 0xf0000000u) == 0);
    assert((brcBatch[adaptiveCmd2 + 16] & 0x00010000u) != 0);
    assert(brcBatch[133] == 0x05000000);
    assert(brcLayout.slice.startOffset == 536);
    assert(brcLayout.slice.sliceStateOffset == 536);
    assert(brcLayout.slice.sliceHeaderOffset == 608);
    assert(brcLayout.slice.vdencWeightsOffset == 624);
    assert(brcLayout.slice.commandsBytes == 136);
    assert(isHcp(brcBatch[brcLayout.slice.sliceStateOffset / 4], 20));
    assert(isHcp(brcBatch[brcLayout.slice.sliceHeaderOffset / 4], 34));
    assert(brcBatch[156] == 0x70880001);
    assert(brcBatch[159] == 0x05000000);
    for (unsigned i = 160; i < brcWords; ++i)
        assert(brcBatch[i] == 0);
    uint32_t updatedQp = 0xcdcdcdcd;
    assert(validateHevcBrcBatchOutput(brcPicture, brcLayout, brcBatch,
        brcWords, updatedQp) && updatedQp == 26);
    uint32_t firmwareBatch[1024];
    memcpy(firmwareBatch, brcBatch, sizeof(firmwareBatch));
    const uint32_t picQpAt = brcLayout.update.pictureOffset / 4 + 19;
    const uint32_t vdencQpAt = brcLayout.update.cmd2Offset / 4 + 27;
    const uint32_t sliceQpAt = brcLayout.slice.startOffset / 4 + 3;
    firmwareBatch[picQpAt] = (firmwareBatch[picQpAt] & ~(63u << 8)) | (34u << 8);
    firmwareBatch[vdencQpAt] = (firmwareBatch[vdencQpAt] & ~(63u << 8)) | (34u << 8);
    firmwareBatch[sliceQpAt] = (firmwareBatch[sliceQpAt] & ~(63u << 6)) | (34u << 6);
    assert(validateHevcBrcBatchOutput(brcPicture, brcLayout, firmwareBatch,
        brcWords, updatedQp) && updatedQp == 34);
    // CU-QP syntax permits a different slice/header base QP. VDEnc's frame
    // QP must still match the HCP rho-domain QP.
    firmwareBatch[vdencQpAt] ^= 1u << 8;
    assert(!validateHevcBrcBatchOutput(brcPicture, brcLayout, firmwareBatch,
        brcWords, updatedQp) && updatedQp == 34);
    firmwareBatch[vdencQpAt] ^= 1u << 8;
    firmwareBatch[sliceQpAt] = (firmwareBatch[sliceQpAt] & ~(63u << 6)) | (26u << 6);
    assert(validateHevcBrcBatchOutput(brcPicture, brcLayout, firmwareBatch,
        brcWords, updatedQp) && updatedQp == 34);
    const auto noCuQp = brcPicture;
    brcPicture.cuQpDeltaEnabled = false;
    assert(!validateHevcBrcBatchOutput(brcPicture, brcLayout, firmwareBatch,
        brcWords, updatedQp) && updatedQp == 34);
    brcPicture = noCuQp;
    const auto pictureStateAt = brcLayout.update.pictureOffset / 4 + 4;
    firmwareBatch[pictureStateAt] ^= 1u << 6;
    assert(!validateHevcBrcBatchOutput(brcPicture, brcLayout, firmwareBatch,
        brcWords, updatedQp) && updatedQp == 34);
    firmwareBatch[pictureStateAt] ^= 1u << 6;
    auto fineGroups = brcPicture;
    fineGroups.diffCuQpDeltaDepth = 3;
    HevcBrcBatchLayout fineLayout;
    uint32_t fineBatch[1024] {};
    const auto fineWords = buildHevcBrcBatchCommands(fineGroups, fineBatch,
        1024, fineLayout);
    assert(fineWords &&
        ((fineBatch[fineLayout.update.pictureOffset / 4 + 4] >> 6) & 3) == 3);
    fineGroups.diffCuQpDeltaDepth = 4;
    assert(!buildHevcBrcBatchCommands(fineGroups, fineBatch, 1024, fineLayout));
    // HuC's Main10 HCP statistics QP includes QpBdOffsetY; VDEnc still uses
    // the unshifted QpY. Keep the exact offset and the 0..51 VDEnc bound.
    auto tenBitBrc = brcPicture;
    tenBitBrc.source.format = tenBitBrc.recon.format = 0x78343230;
    HevcBrcBatchLayout tenBitLayout;
    uint32_t tenBitBatch[1024] {};
    const auto tenBitWords = buildHevcBrcBatchCommands(tenBitBrc, tenBitBatch,
        1024, tenBitLayout);
    const auto tenPic = tenBitLayout.update.pictureOffset / 4 + 19;
    const auto tenVdenc = tenBitLayout.update.cmd2Offset / 4 + 27;
    tenBitBatch[tenPic] = (tenBitBatch[tenPic] & ~(63u << 8)) | (41u << 8);
    tenBitBatch[tenVdenc] = (tenBitBatch[tenVdenc] & ~(255u << 8)) | (29u << 8);
    assert(validateHevcBrcBatchOutput(tenBitBrc, tenBitLayout, tenBitBatch,
        tenBitWords, updatedQp) && updatedQp == 29);
    tenBitBatch[tenPic] ^= 1u << 8;
    assert(!validateHevcBrcBatchOutput(tenBitBrc, tenBitLayout, tenBitBatch,
        tenBitWords, updatedQp) && updatedQp == 29);
    tenBitBatch[tenPic] = (tenBitBatch[tenPic] & ~(63u << 8)) | (63u << 8);
    tenBitBatch[tenVdenc] = (tenBitBatch[tenVdenc] & ~(255u << 8)) | (51u << 8);
    assert(validateHevcBrcBatchOutput(tenBitBrc, tenBitLayout, tenBitBatch,
        tenBitWords, updatedQp) && updatedQp == 51);
    tenBitBatch[tenVdenc] |= 1u << 14;
    assert(!validateHevcBrcBatchOutput(tenBitBrc, tenBitLayout, tenBitBatch,
        tenBitWords, updatedQp) && updatedQp == 51);
    updatedQp = 34;
    firmwareBatch[brcLayout.slice.sliceHeaderOffset / 4] |= 0xfffu;
    assert(!validateHevcBrcBatchOutput(brcPicture, brcLayout, firmwareBatch,
        brcWords, updatedQp) && updatedQp == 34);
    assert(!validateHevcBrcBatchOutput(brcPicture, brcLayout, brcBatch,
        brcWords - 1, updatedQp) && updatedQp == 34);
    HevcCore::Huc::BrcUpdateDmem brcDmem {};
    assert(HevcCore::Huc::buildBrcUpdateDmem(brcLayout.update, brcDmem));
    assert(brcDmem.CMD2_StartInBytes == 328);
    HevcCore::Huc::BrcConstantParams constParams {
        brcPicture.inter, brcWords * 4, 1, &brcLayout.slice};
    HevcCore::Huc::BrcConstantData constData {};
    assert(HevcCore::Huc::buildBrcConstantData(constParams, constData));
    assert(constData.Slice[0].SliceHeaderPIO_StartInBytes == 608);
    uint32_t insufficient[1024];
    memset(insufficient, 0xcd, sizeof(insufficient));
    HevcBrcBatchLayout unchanged = brcLayout;
    assert(!buildHevcBrcBatchCommands(brcPicture, insufficient, brcWords - 1,
        unchanged));
    assert(insufficient[0] == 0xcdcdcdcd);
    assert(memcmp(&unchanged, &brcLayout, sizeof(unchanged)) == 0);
    // A BRC picture must execute both groups from HuC's output SLB. Inline
    // original slice state/PAK headers would encode a different QP/header.
    brcPicture.resolvedAddresses = true;
    brcPicture.secondLevelBatch = true;
    brcPicture.brcBatchOutput = true;
    brcPicture.brcPictureOffset = brcLayout.update.cmd1Offset;
    brcPicture.brcSliceOffset = brcLayout.slice.startOffset;
    brcPicture.brcBatchBytes = brcLayout.update.batchBytes;
    brcPicture.brcPakInfoAddress = 0x14000000;
    brcPicture.buffers.secondLevel = {0x14100000, 65536, true};
    assert(pictureParams(brcPicture).vdencPipeMode.frameStatistics);
    const auto brcPictureWords = buildHevcPictureCommands(brcPicture, buffer, 8192);
    assert(brcPictureWords);
    assert(contains(buffer, brcPictureWords, 0x14100000 + brcLayout.update.cmd1Offset));
    assert(contains(buffer, brcPictureWords, 0x14100000 + brcLayout.slice.startOffset));
    assert(findLastHcp(buffer, brcPictureWords, 20) == brcPictureWords);
    assert(findLastHcp(buffer, brcPictureWords, 34) == brcPictureWords);
    assert(contains(buffer, brcPictureWords, brcPicture.brcPakInfoAddress));
    brcPicture.brcSliceOffset = 65536;
    assert(!buildHevcPictureCommands(brcPicture, insufficient, 1024));
    assert(insufficient[0] == 0xcdcdcdcd);
    brcPicture.sliceHeader = nullptr;
    assert(!buildHevcBrcBatchCommands(brcPicture, insufficient, 1024, unchanged));

    desc.parameterSets = nullptr;
    desc.parameterSetBytes = 0;
    desc.sliceHeader = nullptr;
    desc.sliceHeaderBits = 0;

    // HuC PAK integration moves the second-level commands into their own
    // buffer, starts it with MI_BATCH_BUFFER_START and appends the HuC state
    // packets before the batch terminator.
    desc.huc.enabled = true;
    desc.huc.dmemAddress = 0x12710000;
    desc.huc.dmemLength = 192;
    desc.buffers.secondLevel = {0x12700000, 64u << 10, true};
    desc.huc.regions[0] = {0x12690000, 0, false, true};
    desc.huc.regions[7] = {0x12700000, 0, false, true};
    desc.huc.regions[15] = {0x12720000, 0, false, true};

    const uint32_t hucDwords = hevcPictureCommandDwords(desc);
    assert(hucDwords <= sizeof(buffer) / sizeof(buffer[0]));
    const uint32_t hucWritten = buildHevcPictureCommands(desc, buffer, hucDwords);
    assert(hucWritten == hucDwords);
    assert(buffer[hucWritten - 1] == 0x05000000);
    // MI_BATCH_BUFFER_START with second-level and PPGTT bits set.
    assert(contains(buffer, hucWritten, 0x18C00101));
    assert(contains(buffer, hucWritten, 0x12700000)); // second-level batch
    assert(contains(buffer, hucWritten, 0x75810003)); // HUC_IMEM_STATE
    assert(contains(buffer, hucWritten, 0x75800001)); // HUC_PIPE_MODE_SELECT
    assert(contains(buffer, hucWritten, 0x75820004)); // HUC_DMEM_STATE
    assert(contains(buffer, hucWritten, 0x7584002F)); // HUC_VIRTUAL_ADDR_STATE
    assert(contains(buffer, hucWritten, 0x75A10000)); // HUC_START
    assert(contains(buffer, hucWritten, 0x12710000)); // DMEM source

    const uint32_t secondDwords = hevcSecondLevelCommandDwords(desc);
    assert(secondDwords > 4);
    uint32_t secondBuffer[1024] {};
    assert(secondDwords <= sizeof(secondBuffer) / sizeof(secondBuffer[0]));
    assert(buildHevcSecondLevelCommands(desc, secondBuffer, secondDwords) ==
        secondDwords);
    assert(secondBuffer[secondDwords - 1] == 0x05000000);

    // With resolved graphics addresses the PAK upper bounds are base + size;
    // a handle cannot be extended that way.
    HevcPictureDesc resolved = desc;
    resolved.resolvedAddresses = true;
    const uint32_t resolvedDwords = hevcPictureCommandDwords(resolved);
    uint32_t resolvedBuffer[8192] {};
    assert(buildHevcPictureCommands(resolved, resolvedBuffer, resolvedDwords) ==
        resolvedDwords);
    const uint32_t resolvedPipe = findLastHcp(resolvedBuffer, resolvedDwords, 2);
    const uint32_t scratchSlots[] = {4, 7, 10, 13, 16, 19, 22, 25, 28,
        31, 63, 110, 113, 116, 66};
    constexpr uint32_t hcpScratchRegions = 15;
    for (uint32_t i = 0; i < hcpScratchRegions; ++i) {
        const auto address = resolvedBuffer[resolvedPipe + scratchSlots[i]];
        assert(address == resolved.buffers.hcpScratch.address + i * kHcpScratchRegionBytes);
        assert(address + kHcpScratchRegionBytes <= resolved.buffers.hcpScratch.address +
            resolved.buffers.hcpScratch.size);
        assert(address != resolved.buffers.rowStore.address);
        assert(address != resolved.buffers.cuRecord.address);
        for (uint32_t j = 0; j < i; ++j)
            assert(address != resolvedBuffer[resolvedPipe + scratchSlots[j]]);
    }
    // The gen12 VDEnc binds two scratch buffers of its own: the tile row store
    // (slot 62) and the cumulative CU count streamout surface (slot 65). The
    // reference allocates both for every picture and the VDEnc writes them
    // while walking the picture.
    const uint32_t resolvedVdenc = findLastVdenc(resolvedBuffer, resolvedDwords, 4);
    assert(resolvedBuffer[resolvedVdenc + 62] ==
        resolved.buffers.hcpScratch.address + 15 * kHcpScratchRegionBytes);
    assert(resolvedBuffer[resolvedVdenc + 65] ==
        resolved.buffers.hcpScratch.address + 16 * kHcpScratchRegionBytes);
    uint32_t indObj = 0;
    while (indObj + 1 < resolvedDwords && !isHcp(resolvedBuffer[indObj], 3))
        indObj += resolvedBuffer[indObj] == 0x68000100
            ? 1 : packetLength(resolvedBuffer[indObj]);
    assert(isHcp(resolvedBuffer[indObj], 3));
    // Encoder mapping: the indirect-CU field names the CU/MV object surface,
    // the PAK BSE field names the bitstream buffer and its upper bound. The
    // PAK object commands arrive from the VDEnc, not through this field.
    assert(resolvedBuffer[indObj + 1] == 0);           // bitstream object unused
    // CU records follow the LCU PAK object block inside the object surface.
    // media-driver sizes that block for the smallest allowed coding unit:
    // ALIGN(ceil(1920/16) * ceil(1080/16) * 8 dwords * 4, 4096) = 0x40000.
    assert(resolvedBuffer[indObj + 6] == 0x12500000 + 0x40000);
    assert(resolvedBuffer[indObj + 9] == 0x12400000);  // PAK BSE = bitstream
    assert(resolvedBuffer[indObj + 12] == 0x12500000); // bitstream end

    // Switch: the gen12 softlet HEVC VDEnc packet leaves m_mvOffset at zero,
    // so the PAK's indirect CU object starts on the same buffer base as the
    // VDEnc LCU PAK object command stream. This keeps the hand-off addresses
    // pairable when the VDEnc streams its objects out to memory.
    HevcPictureDesc cuObjBase = resolved;
    cuObjBase.cuObjectAtBase = true;
    const uint32_t cuObjBaseDwords = hevcPictureCommandDwords(cuObjBase);
    uint32_t cuObjBaseBuffer[8192] {};
    assert(buildHevcPictureCommands(cuObjBase, cuObjBaseBuffer,
        cuObjBaseDwords) == cuObjBaseDwords);
    uint32_t baseIndObj = 0;
    while (baseIndObj + 1 < cuObjBaseDwords &&
        !isHcp(cuObjBaseBuffer[baseIndObj], 3))
        baseIndObj += cuObjBaseBuffer[baseIndObj] == 0x68000100
            ? 1 : packetLength(cuObjBaseBuffer[baseIndObj]);
    assert(isHcp(cuObjBaseBuffer[baseIndObj], 3));
    assert(cuObjBaseBuffer[baseIndObj + 6] == 0x12500000);

    // The per-slice flush now always carries media-driver's HEVC flush bit and
    // the picture-level flush follows it directly; the tail check below pins
    // that order. No separate switch is needed.

    // A completed engine writes a per-submission tag only after both pipeline
    // drains; resetting the GPU cannot manufacture this post-sync write.
    HevcPictureDesc tagged = desc;
    tagged.huc.enabled = false;
    tagged.traceCheckpoints = false;
    tagged.completionAddress = 0x126cffc0;
    tagged.completionTag = 42;
    const uint32_t taggedWords = buildHevcPictureCommands(tagged, buffer, 8192);
    uint32_t completion = 0;
    for (uint32_t i = 0; i + 4 < taggedWords; ++i) {
        if (buffer[i] == 0x13004083) {
            completion = i;
            assert(buffer[i + 1] == tagged.completionAddress);
            assert(buffer[i + 2] == 0);
            assert(buffer[i + 3] == 42 && buffer[i + 4] == 0);
        }
    }
    assert(completion > findLastVdenc(buffer, taggedWords, 7));
    assert(buffer[completion - 13] == 0x13000083);
    assert(buffer[completion - 8] == 0x12080002);
    assert(buffer[completion - 7] == 0x28a0);
    assert(buffer[completion - 6] == tagged.completionAddress + 8);
    assert(buffer[completion - 4] == 0x12080002);
    assert(buffer[completion - 3] == 0x28bc);
    assert(buffer[taggedWords - 1] == 0x05000000);

    // Inter picture: one L0 reference. The reference surface address goes into
    // HCP_PIPE_BUF_ADDR_STATE and the VDEnc reference state, and the PAK gets
    // an HCP_REF_IDX_STATE before the slice state.
    HevcPictureDesc inter = desc;
    inter.codingType = 1;
    inter.inter = true;
    inter.poc = 2;
    inter.refPoc = 0;
    inter.reference = surface(0x12370000, 1920, 1088);
    // Two-slot downscale pool: this picture writes slot 1 while it reads the
    // reference picture's downscale from slot 0, mirroring media-driver's
    // per-picture Get4x/8xDsReconSurface(scaledIdx) pool index.
    inter.scaledSlot = 1;
    inter.referenceScaledSlot = 0;
    inter.referenceScaledValid = true;
    const auto interMapping = pictureParams(inter);
    assert(!interMapping.cmd1.intra);
    assert(!interMapping.cmd2.intra);
    // The command field is the active count minus one.
    assert(interMapping.cmd2.numRefIdxL0 == 0);
    assert(interMapping.cmd2.refL0[0] == 0);
    assert(interMapping.pipeBuf.references[0].address == 0x12370000);
    // Current POC 2, reference POC 0: the VDEnc POC slots read 2.
    const auto interCmd2 = HevcCore::VdEnc::cmd2(interMapping.cmd2);
    // VDENC_CMD2.DW2.PictureType is the hardware PICTURE_TYPE: 0 = I, 1 = P.
    // These pictures code P slices, so the value is P; 2/3 are the B values the
    // reference driver uses for B slices.
    assert(bits(interCmd2[2], 20, 21) == 1);
    // The P picture has one L0 reference and no L1 list. The L1 mapping byte
    // must stay invalid (0x7); a zero there names frame 0 and tells the VDEnc
    // that list 1 exists on a slice that cannot express it.
    assert(bits(interCmd2[11], 24, 26) == 0x7);
    assert(bits(interCmd2[11], 0, 2) == interMapping.cmd2.refIdxMapping[0]);
    assert(interCmd2[8] == 0x54555555);
    assert(bits(interCmd2[3], 0, 7) == 2);
    assert(bits(interCmd2[3], 8, 15) == 2);
    assert(bits(interCmd2[3], 16, 23) == 2);
    assert(bits(interCmd2[4], 0, 7) == 2);
    assert(interMapping.vdencReference.width == 1920);
    assert(interMapping.vdencReference.height == 1088);

    // Diagnostic variant: the P slice syntax and reference list stay, while the
    // VDEnc runs in its intra-only picture mode (PICTURE_TYPE I). The slice
    // state must remain a P slice or the experiment would change two things.
    HevcPictureDesc intraOnly = inter;
    intraOnly.interIntraOnly = true;
    const auto intraOnlyMapping = pictureParams(intraOnly);
    assert(intraOnlyMapping.cmd1.intra && intraOnlyMapping.cmd2.intra);
    const auto intraOnlyCmd2 = HevcCore::VdEnc::cmd2(intraOnlyMapping.cmd2);
    assert(bits(intraOnlyCmd2[2], 20, 21) == 0);
    assert(sliceParams(intraOnly).sliceType == 1);

    // Low-delay B: the form media-driver's m_lowDelay gives every inter
    // picture. The HCP slice type is B, the VDEnc picture type is 3, the merge
    // tables switch to the low-delay shape, and the slice carries a second
    // reference list that repeats list 0.
    HevcPictureDesc lowDelayB = inter;
    lowDelayB.lowDelay = true;
    const auto ldbMapping = pictureParams(lowDelayB);
    assert(ldbMapping.cmd2.lowDelayB);
    assert(sliceParams(lowDelayB).sliceType == 0);
    const auto ldbCmd2 = HevcCore::VdEnc::cmd2(ldbMapping.cmd2);
    assert(bits(ldbCmd2[2], 20, 21) == 3);
    assert(ldbCmd2[8] == 0);                    // low-delay merge table
    assert(bits(ldbCmd2[9], 0, 15) == 0);       // low-delay cost table
    assert(bits(ldbCmd2[11], 24, 31) == 0x80);  // no L1 slot, marker set
    assert(bits(ldbCmd2[11], 0, 2) == ldbMapping.cmd2.refIdxMapping[0]);
    uint32_t ldbBuffer[8192] {};
    const uint32_t ldbWritten = buildHevcPictureCommands(lowDelayB, ldbBuffer, 8192);
    assert(ldbWritten > 0);
    uint32_t refIdxCommands = 0;
    uint32_t list0 = ldbWritten, list1 = ldbWritten;
    for (uint32_t i = 0; i < ldbWritten; ++i) {
        if (!isHcp(ldbBuffer[i], 18))
            continue;
        ++refIdxCommands;
        (bits(ldbBuffer[i + 1], 0, 0) == 0 ? list0 : list1) = i;
    }
    assert(refIdxCommands == 2);
    assert(list0 < list1);
    assert(list1 < findLastHcp(ldbBuffer, ldbWritten, 20));
    // Both lists name the same frame store and the same POC distance.
    assert(bits(ldbBuffer[list0 + 2], 0, 15) == bits(ldbBuffer[list1 + 2], 0, 15));
    const uint32_t interWritten = buildHevcPictureCommands(inter, buffer, 8192);
    assert(interWritten > 0);
    // Rebuild the intra picture into its own buffer: the inter assembly above
    // reused `buffer`, and both slice states are compared below.
    uint32_t intraBuffer[8192] {};
    const uint32_t intraWritten = buildHevcPictureCommands(desc, intraBuffer, 8192);
    assert(intraWritten > 0);
    const uint32_t intraSlice = findLastHcp(intraBuffer, intraWritten, 20);
    const uint32_t intraPipe = findLastVdenc(intraBuffer, intraWritten, 4);
    assert(intraPipe != intraWritten);
    const uint32_t refIdx = findLastHcp(buffer, interWritten, 18);
    const uint32_t interSlice = findLastHcp(buffer, interWritten, 20);
    assert(refIdx < interSlice);
    assert(bits(buffer[refIdx], 0, 11) == 16);     // DwordLength
    assert(bits(buffer[refIdx], 16, 22) == 18);    // HCP_REF_IDX_STATE
    assert(bits(buffer[refIdx + 1], 0, 0) == 0);   // list 0
    assert(bits(buffer[refIdx + 1], 1, 4) == 0);   // one reference
    assert(bits(buffer[refIdx + 2], 0, 7) == 2);   // POC difference
    assert(bits(buffer[refIdx + 2], 8, 10) == 0);  // RefAddr 0
    assert(bits(buffer[interSlice + 3], 0, 1) == 1); // P slice
    // HCP_SLICE_STATE.DW4.Islowdelay stays clear: the low-delay value did not
    // change the inter stall on the target (notes.md).
    assert(bits(buffer[interSlice + 4], 14, 14) == 0);
    assert(bits(intraBuffer[intraSlice + 4], 14, 14) == 0);
    assert(contains(buffer, interWritten, 0x12370000)); // reference plane

    // VDENC_PIPE_BUF_ADDR_STATE: the VDEnc reads the reference's 8x/4x
    // downscaled recon and its temporal MV buffer from fixed slots. A null
    // temporal MV slot makes the VDEnc read address 0 while it searches the
    // inter frame, so the slot must always carry the HCP collocated-MV region.
    const uint32_t vdencPipe = findLastVdenc(buffer, interWritten, 4);
    assert(vdencPipe != interWritten);
    const uint32_t temporalMv = inter.buffers.hcpScratch.address +
        (inter.resolvedAddresses ? 14 * kHcpScratchRegionBytes : 0);
    assert(buffer[vdencPipe + 19] == temporalMv);
    assert(buffer[vdencPipe + 20] == 0);
    // Downscale pool addressing: slot 0 is the reference picture's recon,
    // slot 1 is this picture's own output. The strides come from the tracked
    // 1920x1080 NV12 surfaces (4x 245760 bytes, 8x 122880 bytes).
    const uint32_t ds4xStride = hevcScaledSurfaceBytes(
        hevcScaledSurface(inter.width, inter.height, false));
    const uint32_t ds8xStride = hevcScaledSurfaceBytes(
        hevcScaledSurface(inter.width, inter.height, true));
    assert(ds4xStride == 245760 && ds8xStride == 122880);
    assert(buffer[vdencPipe + 1] == inter.buffers.scaled8x.address);
    assert(buffer[vdencPipe + 37] == inter.buffers.scaled4x.address);
    assert(buffer[vdencPipe + 49] == inter.buffers.scaled8x.address + ds8xStride);
    assert(buffer[vdencPipe + 52] == inter.buffers.scaled4x.address + ds4xStride);
    assert(buffer[vdencPipe + 22] == inter.reference.address);
    // VDENC_PIPE_MODE_SELECT keeps both write-disabled bits clear: the VDEnc
    // must write this picture's downscale for the next reference to read.
    const uint32_t vdencMode = findLastVdenc(buffer, interWritten, 0);
    assert(vdencMode != interWritten);
    assert(bits(buffer[vdencMode + 1], 10, 10) == 0);
    assert(bits(buffer[vdencMode + 1], 11, 11) == 0);
    // VDENC_PIPE_MODE_SELECT.RandomAccess keeps the captured value; the
    // low-delay value did not change the inter stall (notes.md).
    assert(bits(buffer[vdencMode + 1], 18, 18) == 1);
    const uint32_t intraMode = findLastVdenc(intraBuffer, intraWritten, 0);
    assert(intraMode != intraWritten);
    assert(bits(intraBuffer[intraMode + 1], 18, 18) == 1);
    // An intra picture has no reference list, so no downscaled reference
    // surface is supplied, while the temporal MV buffer is still programmed.
    assert(intraBuffer[intraPipe + 19] == desc.buffers.hcpScratch.address);
    assert(intraBuffer[intraPipe + 1] == 0);

    // HuC readiness uses the executing engine's relative MMIO addresses and
    // a tail range disjoint from HCP probes and the completion marker.
    HevcPictureDesc readiness = desc;
    readiness.huc.enabled = false;
    readiness.resolvedAddresses = true;
    readiness.completionAddress = readiness.buffers.statistics.address +
        readiness.buffers.statistics.size - 64;
    readiness.completionTag = 9;
    auto statusReads = [](const std::vector<HevcCore::Packet> &packets) {
        unsigned count = 0;
        for (const auto &packet : packets)
            if (packet.size() == 4 && packet[0] == 0x12080002 &&
                (packet[1] == 0x2000 || packet[1] == 0x23b0))
                ++count;
        return count;
    };
    assert(statusReads(assemble(readiness)) == 0);
    readiness.probeHucStatus = true;
    const auto readyPackets = assemble(readiness);
    assert(statusReads(readyPackets) == 2);
    unsigned index = 0;
    for (const auto &packet : readyPackets) {
        if (packet.size() == 4 && packet[0] == 0x12080002 &&
            (packet[1] == 0x2000 || packet[1] == 0x23b0)) {
            assert(packet[1] == (index ? 0x23b0 : 0x2000));
            assert(packet[2] == readiness.buffers.statistics.address +
                readiness.buffers.statistics.size - 84 + index * 4);
            assert(packet[2] + 4 <= readiness.completionAddress);
            ++index;
        }
    }
    assert((readyPackets.back()[0] & (1u << 14)) != 0); // completion follows reads
    readiness.probeHucKernel = 8;
    const auto loadPackets = assemble(readiness);
    assert(statusReads(loadPackets) == 3);
    bool kernelFound = false;
    for (const auto &packet : loadPackets) {
        if (packet[0] == 0x75810003) {
            assert(packet[4] == 8);
            kernelFound = true;
        }
        assert(packet[0] != 0x75A10000);
    }
    assert(kernelFound);
    bool authenticationFound = false;
    for (const auto &packet : loadPackets)
        if (packet.size() == 4 && packet[0] == 0x12000002 && packet[1] == 0xc1dc) {
            assert(packet[2] == readiness.completionAddress - 8);
            authenticationFound = true;
        }
    assert(authenticationFound);
    readiness.resolvedAddresses = false;
    assert(statusReads(assemble(readiness)) == 0);
    readiness.resolvedAddresses = true;
    readiness.buffers.statistics.size = 64;
    assert(statusReads(assemble(readiness)) == 0);

    puts("PASS Gen12 HEVC VDEnc/HCP picture sequence");
    return 0;
}
