// CPU check for the shim-side HEVC encode context: creation, parameter
// plumbing and Schedule payload parsing.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../media/va_shim/hevc_context.cpp"
#include "../media/va_shim/hevc_commands.cpp"
#include "../media/va_shim/hevc_token.cpp"
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
#include "../media/hevc_core/huc_dmem.cpp"
#include "../media/hevc_core/huc_brc_dmem.cpp"
#include "../media/hevc_core/huc_brc_const.cpp"
#include "../media/hevc_core/hevc_headers.cpp"
#include "../media/hevc_core/vdenc_streamin.cpp"

// The context's diagnostic path prints the IOSurface CPU mapping; the test
// has no real surface allocator.
extern "C" void *IOSurfaceGetBaseAddress(void *) {
    static char page[1 << 20];
    return page;
}

// The linear-resource path needs client VM. The test never enables the
// experimental allocation path, so failing the allocation is sufficient.
extern "C" unsigned int mach_task_self_ = 0;
extern "C" int vm_allocate(unsigned int, unsigned long *, unsigned long, int) {
    return -1;
}
extern "C" int vm_deallocate(unsigned int, unsigned long, unsigned long) {
    return 0;
}

namespace {

using namespace VaShim;

char gLastLine[256];
char gBrcLines[4096];
unsigned gBrcLineBytes;
bool gMockMain10;
bool gMockReconEightBit;
bool gMockShortPitch;
bool gNativeVbr;
bool gFirmwareVbr;
unsigned gBrcFailureStage;
bool gCbrRequest;
bool gQualityRequest;
bool gNativeOversizedPicture;
bool gNativeUnfittablePicture;
bool gHucAuthenticated {true};

void capture(const char *line) {
    unsigned long length = 0;
    while (line[length] && length + 1 < sizeof(gLastLine))
        ++length;
    memcpy(gLastLine, line, length);
    gLastLine[length] = 0;
    if (strstr(line, "BRC") && length + gBrcLineBytes + 2 < sizeof(gBrcLines)) {
        memcpy(gBrcLines + gBrcLineBytes, line, length);
        gBrcLineBytes += length;
        gBrcLines[gBrcLineBytes++] = '\n';
        gBrcLines[gBrcLineBytes] = 0;
    }
}

ContextDesc descriptor(uint32_t id, uint32_t width, uint32_t height) {
    ContextDesc desc {};
    desc.type = kHevcEncodeContextType;
    desc.subtype = kHevcEncodeContextSubtype;
    desc.id = id;
    desc.width = width;
    desc.height = height;
    return desc;
}

ContextParamEntry entry(uint32_t parameter, void *value) {
    ContextParamEntry result {};
    result.type = kHevcEncodeContextType;
    result.subtype = kHevcEncodeContextSubtype;
    result.id = 1001;
    result.parameter = parameter;
    result.value = value;
    return result;
}

struct MockSubmission {
    HevcEncodeContext *encoder {};
    uint8_t descriptor[128] {};
    uint32_t batch[8192] {};
    uint32_t token[HevcToken::kTotalDwords] {};
    unsigned requests {}, resolves {}, pictures {}, hucSubmits {}, fence {};
    unsigned sliceType {3}, referenceLists {};
    bool complete {true};
};

void exerciseHeaderCache() {
    HevcEncodeContext encoder {};
    HevcPictureDesc picture;
    picture.width = 640;
    picture.height = 360;
    auto headers = [&]() {
        assert(preparePictureHeader(&encoder, picture));
        return std::vector<uint8_t>(picture.parameterSets,
            picture.parameterSets + picture.parameterSetBytes);
    };
    const auto original = headers();
    const void *cached = encoder.parameterSets;
    assert(headers() == original && encoder.parameterSets == cached);

    // A syntax change at the same size and QP must not reuse an old SPS/PPS.
    // Depth one cannot describe 16x16 -> 8x8 -> 4x4 transform trees.
    picture.transformDepth = 1;
    const auto shallow = headers();
    assert(shallow != original);
    picture.transformDepth = 2;
    assert(headers() == original);
    picture.maxTransformLog2 = 4;
    assert(headers() != original);
    picture.maxTransformLog2 = 5;
    assert(headers() == original);
    picture.cuQpDeltaEnabled = true;
    const auto withDeltas = headers();
    assert(withDeltas != original);
    picture.diffCuQpDeltaDepth = 3;
    assert(headers() != withDeltas);
    picture.diffCuQpDeltaDepth = 0;
    assert(headers() == withDeltas);
    picture.cuQpDeltaEnabled = false;
    assert(headers() == original);
    picture.source.format = 0x78343230;
    assert(headers() != original && encoder.headerConfig.main10);
    picture.source.format = 0x4e563132;
    assert(headers() == original && !encoder.headerConfig.main10);

    // Display order belongs to each slice header; it does not change the SPS.
    picture.nalType = 1;
    picture.poc = 2;
    cached = encoder.parameterSets;
    assert(headers() == original && encoder.parameterSets == cached);
    // A HuC output slice QP and its packed header must describe the same
    // decoder state, independently of the frame-level QP used for CU deltas.
    for (bool main10 : {false, true}) {
        picture.source.format = main10 ? 0x78343230 : 0x4e563132;
        picture.cuQpDeltaEnabled = true;
        picture.inter = picture.lowDelay = true;
        picture.codingType = 2;
        picture.refPoc = 1;
        headers();
        uint32_t batch[1024] {};
        HevcBrcBatchLayout layout;
        const uint32_t words = buildHevcBrcBatchCommands(picture, batch, 1024, layout);
        assert(words && brcSliceHeaderMatches(&encoder, picture, layout, batch, words));
        const uint32_t state = layout.slice.sliceStateOffset / 4 + 3;
        batch[state] ^= 1u << 6;
        assert(!brcSliceHeaderMatches(&encoder, picture, layout, batch, words));
        batch[state] ^= 1u << 6;
        batch[layout.slice.sliceHeaderOffset / 4 + 2] ^= 1u << 24;
        assert(!brcSliceHeaderMatches(&encoder, picture, layout, batch, words));
        assert(!brcSliceHeaderMatches(&encoder, picture, layout, batch, 1));
    }
    free(encoder.parameterSets);
    free(encoder.sliceHeader);
}

bool gRateUpdates = false;
bool gMockBrcReencode = false;

void exerciseBrcPictureData() {
    using namespace HevcCore::Huc;
    HevcEncodeContext encoder {};
    HevcPictureDesc picture;
    BrcUpdateParams update;
    update.referencesL0 = update.referencesL1 = 0;
    assert(prepareBrcPictureData(&encoder, picture, update));
    assert(update.pictureDataOffset == 0);
    picture.inter = true;
    picture.poc = 1;
    picture.refPoc = 0;
    update.referencesL0 = update.referencesL1 = 1;
    const auto initial = update;
    // Selecting a slot does not publish a picture that has not completed.
    assert(!prepareBrcPictureData(&encoder, picture, update));
    assert(!memcmp(&initial, &update, sizeof(update)));
    assert(rememberBrcPictureData(&encoder, 0, 0));
    for (uint32_t frame = 1; frame <= 320; ++frame) {
        picture.poc = frame;
        picture.refPoc = frame - 1;
        // Retain an old picture across many slot wraps as well as the three
        // recent pictures. A simple frame-number ring would overwrite it.
        picture.rpsCount = 1;
        picture.rpsPocs[0] = 0;
        for (uint32_t distance = 1; distance <= 3 && frame > distance; ++distance)
            picture.rpsPocs[picture.rpsCount++] = frame - distance;
        const auto before = encoder;
        assert(prepareBrcPictureData(&encoder, picture, update));
        assert(!memcmp(before.brcPictureData, encoder.brcPictureData,
            sizeof(encoder.brcPictureData)));
        assert(update.pictureDataOffset < kBrcPictureDataSlots * kBrcPictureDataBytes);
        assert(update.pictureDataOffset != update.referenceDataOffsets[0]);
        assert(update.referenceDataOffsets[0] == update.referenceDataOffsets[1]);
        assert(encoder.brcPictureData[update.referenceDataOffsets[0] /
            kBrcPictureDataBytes].poc == frame - 1);
        for (uint32_t r = 0; r < picture.rpsCount; ++r)
            assert(!encoder.brcPictureData[update.pictureDataOffset /
                kBrcPictureDataBytes].valid || encoder.brcPictureData[
                update.pictureDataOffset / kBrcPictureDataBytes].poc != picture.rpsPocs[r]);
        const auto firstPass = update;
        assert(prepareBrcPictureData(&encoder, picture, update));
        assert(!memcmp(&firstPass, &update, sizeof(update)));
        assert(rememberBrcPictureData(&encoder, frame, update.pictureDataOffset));
        assert(encoder.brcPictureData[0].valid && encoder.brcPictureData[0].poc == 0);
    }
    const auto completed = encoder;
    assert(!rememberBrcPictureData(&encoder, 321, 6 * 1024));
    assert(!rememberBrcPictureData(&encoder, 321, 1));
    assert(!memcmp(completed.brcPictureData, encoder.brcPictureData,
        sizeof(encoder.brcPictureData)));
    resetReferenceHistory(&encoder);
    assert(!prepareBrcPictureData(&encoder, picture, update));
    picture = {};
    update = {};
    update.referencesL0 = update.referencesL1 = 0;
    assert(prepareBrcPictureData(&encoder, picture, update) && update.pictureDataOffset == 0);
}

void exerciseSubmission(bool main10 = false, bool brc = false, bool authenticated = true,
    bool update = false, bool reencode = false, bool classification = false,
    bool nativeBudget = false, bool lowNativeQp = false, unsigned firmwareVbr = 0,
    unsigned failureStage = 0, bool quality = false) {
    update = brc; // Software rate requests automatically run the complete BRC path.
    gMockMain10 = main10;
    gRateUpdates = update;
    gMockBrcReencode = reencode;
    gNativeVbr = nativeBudget;
    gFirmwareVbr = firmwareVbr;
    gBrcFailureStage = failureStage;
    gQualityRequest = quality;
    gNativeUnfittablePicture = false;
    gCbrRequest = update && !nativeBudget && !firmwareVbr && !quality;
    gBrcLineBytes = 0;
    gBrcLines[0] = 0;
    gHucAuthenticated = authenticated;
    auto desc = descriptor(1001, 640, 384);
    auto *encoder = static_cast<HevcEncodeContext *>(createHevcEncodeContext(nullptr, &desc));
    assert(encoder);
    MockSubmission gpu;
    gpu.encoder = encoder;
    encoder->videoContext = encoder->submitContext = &gpu;
    encoder->surfaceManager = encoder;
    encoder->scratchTried = 1;
    auto &b = encoder->scratch;
    HevcBuffer *buffers[] = {&b.bitstream, &b.pakBse, &b.cuRecord, &b.streamIn,
        &b.rowStore, &b.encodedSliceSize, &b.statistics, &b.secondLevel, &b.dmem,
        &b.aggregatedStats, &b.brcHistory, &b.brcData, &b.tileRecord, &b.scaled4x,
        &b.scaled8x, &b.hcpStreamout, &b.sseRowstore, &b.hcpScratch};
    static_assert(sizeof(buffers) / sizeof(buffers[0]) == kScratchCount, "mock resource layout");
    std::vector<std::vector<uint8_t>> memory(kScratchCount);
    for (uint32_t i = 0; i < kScratchCount; ++i) {
        memory[i].resize(i == kScratchHcp ? kHcpScratchRegionBytes * kHcpScratchRegions
                                         : kStreamInSize);
        *buffers[i] = {0x100000 + i * 0x1000, uint32_t(memory[i].size()), true, memory[i].data()};
        encoder->linear[i].base = memory[i].data();
        encoder->linear[i].size = memory[i].size();
    }
    HevcSubmitOps ops;
    ops.registerSurface = [](void *, void *) -> int32_t { return 0; };
    ops.surfaceAddress = [](void *surface, void *, uint32_t plane,
        uint32_t *address, uint32_t *format) -> int32_t {
        *address = uintptr_t(surface) + plane * 0x1000;
        *format = 0; return 1;
    };
    ops.surfaceWidth = [](void *) -> unsigned long { return 640; };
    ops.surfaceHeight = [](void *) -> unsigned long { return 360; };
    ops.surfaceFormat = [](void *surface) -> uint32_t {
        if (gMockReconEightBit && surface == reinterpret_cast<void *>(0x4000))
            return 0x4e563132;
        return gMockMain10 ? 0x78343230 : 0x4e563132;
    };
    ops.surfacePlanePitch = [](void *, unsigned long) -> unsigned long {
        return gMockMain10 && !gMockShortPitch ? 1280 : 640;
    };
    ops.surfacePlaneBase = [](void *, unsigned long plane) -> void * {
        return reinterpret_cast<void *>(0x80000000ul +
            plane * (gMockMain10 ? 1280ul : 640ul) * 384);
    };
    ops.debugMarker = nullptr; // No functional or diagnostic markers installed.
    ops.hucAuthenticated = []() -> int32_t { return gHucAuthenticated ? 1 : 0; };
    ops.request = [](void *context, uint32_t words, uint32_t aux, void **out) -> int32_t {
        auto &m = *static_cast<MockSubmission *>(context);
        ++m.requests;
        assert(words <= 8192 && aux == HevcToken::kTotalDwords);
        memset(m.batch, 0, sizeof(m.batch));
        write64(m.descriptor, kVideoCommandStart, uintptr_t(m.batch));
        write64(m.descriptor, kVideoCommandBase, uintptr_t(m.batch));
        write64(m.descriptor, kVideoSidebandStart, uintptr_t(m.token));
        *out = m.descriptor; return StatusSuccess;
    };
    ops.submit = [](void *context, uint32_t *fence) -> int32_t {
        auto &m = *static_cast<MockSubmission *>(context);
        if (gBrcFailureStage == 4 && m.batch[0] != kPlaceholderBatchEnd &&
            m.batch[2] != 0x75810003)
            return StatusNotFound;
        *fence = ++m.fence; return StatusSuccess;
    };
    ops.wait = [](void *context, uint32_t) -> int32_t {
        auto &m = *static_cast<MockSubmission *>(context);
        if (m.batch[0] == kPlaceholderBatchEnd) {
            ++m.resolves;
            uint32_t *data = m.token + 2;
            assert(data[0] == kPictureResourceCount);
            for (uint32_t i = 0; i < data[0]; ++i) {
                const auto at = HevcToken::kFirstRecord + i * HevcToken::kRecordStride;
                data[at + HevcToken::kRecordAddressLow] = 0x20000000 + i * 0x1000000;
                data[at + HevcToken::kRecordSize] = 0; // keep known CPU allocation size
            }
        } else if (m.batch[2] == 0x75810003) {
            ++m.hucSubmits;
            const auto &dmem = m.encoder->scratch.dmem;
            if (m.batch[6] == HevcCore::Huc::kHevcBrcInitKernel) {
                const auto *input = static_cast<const HevcCore::Huc::BrcInitDmem *>(dmem.cpuAddress);
                assert(input->FrameWidth_U16 == 640 && input->FrameHeight_U16 == 360);
                assert(input->TargetBitrate_U32 == (gQualityRequest ? 0u : gNativeVbr ? 400000u : 4000000u) &&
                    input->BRCFlag == (gNativeVbr || gQualityRequest ? 0 : (gCbrRequest ? 1 : 2)));
                assert(input->FrameRateM_U32 == 30 && input->FrameRateD_U32 == 1);
            } else {
                assert(m.batch[6] == HevcCore::Huc::kHevcBrcUpdateKernel && gRateUpdates);
                const auto *input = static_cast<const HevcCore::Huc::BrcUpdateDmem *>(dmem.cpuAddress);
                assert(input->CurrentPass_U8 <= 1 && input->FrameID_U32 == m.encoder->brcFrameId);
                assert(input->MaxNumPass_U8 == (gNativeVbr || gQualityRequest ? 1 : 2));
                assert(input->TARGETSIZE_U32 == (gNativeVbr ? 22500u : 6000000u) &&
                    (gNativeVbr ? input->CQP_QPValue_U8 >= 10 : input->CQP_QPValue_U8 == (gQualityRequest ? 20 : 26)));
                const auto &source = m.encoder->scratch.aggregatedStats;
                const auto &output = m.encoder->scratch.secondLevel;
                const auto *constants = reinterpret_cast<const HevcCore::Huc::BrcConstantData *>(
                    static_cast<const uint8_t *>(source.cpuAddress) + kBrcConstantsOffset);
                assert(constants->Slice[0].SliceState_StartInBytes == 536);
                if (input->CurrentPass_U8)
                    assert(constants->Slice[0].SliceHeader_SizeInBits < input->SliceHeaderSize);
                assert(input->CMD1_StartInBytes == 40 && input->PIC_STATE_StartInBytes == 164);
                memcpy(output.cpuAddress, source.cpuAddress, input->SLB_Data_SizeInBytes);
                // First-pass BRC may change the frame QP while retaining the
                // slice/header QP. CU-QP syntax carries the difference.
                auto *words = static_cast<uint32_t *>(output.cpuAddress);
                assert(m.encoder->headerConfig.diffCuQpDeltaDepth == 3);
                assert(((words[input->PIC_STATE_StartInBytes / 4 + 4] >> 6) & 3) == 3);
                const uint32_t picQp = input->PIC_STATE_StartInBytes / 4 + 19;
                const uint32_t vdencQp = input->CMD2_StartInBytes / 4 + 27;
                // Native Main10 HuC output uses the 12-point HCP statistics
                // offset while leaving VDEnc's frame QpY unshifted.
                const uint32_t qp = gFirmwareVbr && gNativeUnfittablePicture ? 51u :
                    gNativeVbr ? input->CQP_QPValue_U8 : 25u;
                words[picQp] = (words[picQp] & ~(63u << 8)) |
                    ((qp + (gMockMain10 ? 12 : 0)) << 8);
                words[vdencQp] = (words[vdencQp] & ~(255u << 8)) | (qp << 8);
                if (gBrcFailureStage == 2)
                    words[input->CMD2_StartInBytes / 4] = 0;
                // A no-repack second update need not write any SLB output.
                if (input->CurrentPass_U8 && !gMockBrcReencode)
                    memset(output.cpuAddress, 0, input->SLB_Data_SizeInBytes);
            }
            write32(dmem.cpuAddress, kBrcReadbackOffset, 64);
            write32(dmem.cpuAddress, kBrcReadbackOffset + 4, 0x6040);
            write32(dmem.cpuAddress, kBrcReadbackOffset + 8, 1);
            write32(dmem.cpuAddress, kBrcReadbackOffset + 12, gHucAuthenticated ? 1 : 0);
            const bool secondUpdate = m.batch[6] == HevcCore::Huc::kHevcBrcUpdateKernel &&
                static_cast<const HevcCore::Huc::BrcUpdateDmem *>(dmem.cpuAddress)->CurrentPass_U8 == 1;
            write32(dmem.cpuAddress, kBrcReadbackOffset + 16,
                secondUpdate && gMockBrcReencode ? 0x80000001u : 1u);
            const bool failCompletion = (gBrcFailureStage == 1 &&
                m.batch[6] == HevcCore::Huc::kHevcBrcInitKernel) ||
                (gBrcFailureStage == 3 && secondUpdate);
            if (gHucAuthenticated && !failCompletion)
                write32(dmem.cpuAddress, kBrcReadbackOffset + 24, m.encoder->brcSubmissionCount);
        } else {
            ++m.pictures;
            m.sliceType = 3;
            m.referenceLists = 0;
            for (unsigned i = 0; i + 12 < 8192; ++i) {
                if (m.batch[i] == 0x7394000B)
                    m.sliceType = m.batch[i + 3] & 3;
                if (m.batch[i] == 0x73920010)
                    ++m.referenceLists;
            }
            if (gRateUpdates) {
                const uint32_t base = 0x20000000 + 9 * 0x1000000;
                bool pictureGroup = false, sliceGroup = false;
                for (unsigned i = 0; i + 2 < 8192; ++i) {
                    if (m.batch[i] != 0x18c00101) continue;
                    pictureGroup |= m.batch[i + 1] == base + m.encoder->brcLayout.update.cmd1Offset;
                    sliceGroup |= m.batch[i + 1] == base + m.encoder->brcLayout.slice.startOffset;
                }
                assert(pictureGroup && sliceGroup);
            }
            if (m.complete) {
                auto &stats = m.encoder->scratch.statistics;
                uint32_t tag = m.encoder->sequenceCount;
                for (unsigned i = 0; i + 4 < 8192; ++i)
                    if (m.batch[i] == 0x13004083) tag = m.batch[i + 3];
                write32(stats.cpuAddress, stats.size - 64, tag);
                const auto *input = static_cast<const HevcCore::Huc::BrcUpdateDmem *>(m.encoder->scratch.dmem.cpuAddress);
                const auto *batchOutput = static_cast<const uint32_t *>(m.encoder->scratch.secondLevel.cpuAddress);
                const uint32_t outputQp = batchOutput ? (batchOutput[m.encoder->brcLayout.update.cmd2Offset / 4 + 27] >> 8) & 255 : 0;
                const uint32_t bytes = gNativeUnfittablePicture ? 5000 :
                    gFirmwareVbr && gNativeOversizedPicture ? (outputQp <= 25 ? 4000 : 1000) :
                    gNativeOversizedPicture ? (input->CQP_QPValue_U8 == 20 ? 2000 : 1000) : 1234;
                write32(stats.cpuAddress, stats.size - 64 + 8, bytes);
            }
        }
        return StatusSuccess; // a reset can also release the fence
    };
    setHevcSubmitOps(ops);
    uint8_t payload[968] {};
    write64(payload, 16, 0x2000);
    write64(payload, 912, 0x4000);
    if (classification || nativeBudget)
        write32(payload, 828, 19);
    CommandNode node {};
    node.type = kHevcEncodeContextType;
    node.subtype = kHevcEncodeContextSubtype;
    node.payload = payload;
    void *ref = nullptr;
    if (brc) {
        HevcPlugin::RateControlInput input;
        input.mode = 1;
        input.constantBitRate = gCbrRequest;
        input.targetRate = 4000000; input.maximumRate = input.vbvSize = 8000000;
        input.initialFullness = 6000000; input.gopSize = firmwareVbr ? 1 : 16; input.qp = 20;
        if (quality) { input.mode = 0; input.targetRate = input.maximumRate = 0; }
        if (lowNativeQp)
            input.qp = 0;
        input.frameRateNumerator = 30; input.frameRateDenominator = 1;
        if (firmwareVbr) {
            input.limitCount = 1; input.limitFrames[0] = input.windowFrames = 2;
            input.limitBits[0] = 30000; input.windowMaximumRatio = 200;
        }
        if (nativeBudget) {
            input.targetRate = 400000; input.maximumRate = 450000;
            input.vbvSize = 30000; input.initialFullness = 22500;
            input.windowFrames = input.limitFrames[0] = 2;
            input.windowMaximumRatio = 112;
            input.limitCount = 1; input.limitBits[0] = 30000;
        }
        assert(hevcSetRateControlInput(encoder, &input) == StatusSuccess);
        if (!authenticated) {
            assert(hevcSchedule(encoder, &node, &ref) == StatusNotFound);
            assert(!ref && !encoder->brcInitialized && !gpu.hucSubmits && !gpu.pictures);
            // A bitrate request cannot silently fall back to CQP. Software
            // can explicitly switch to a quality-only request without HuC.
            input.mode = 0; input.constantBitRate = false; input.qp = 26;
            assert(hevcSetRateControlInput(encoder, &input) == StatusSuccess);
            gRateUpdates = false;
        }
    }
    if (main10) {
        // Main10 is available without markers, but surface validation remains.
        gMockReconEightBit = true;
        assert(hevcSchedule(encoder, &node, &ref) == StatusNotFound);
        assert(gpu.resolves == 0 && gpu.pictures == 0 && !ref);
        gMockReconEightBit = false;
        gMockShortPitch = true;
        assert(hevcSchedule(encoder, &node, &ref) == StatusNotFound);
        assert(gpu.resolves == 0 && gpu.pictures == 0 && !ref);
        gMockShortPitch = false;
    }
    if (failureStage) {
        assert(brc && update && authenticated);
        assert(hevcSchedule(encoder, &node, &ref) == StatusNotFound);
        assert(!ref && !read32(payload, 936) && !encoder->reconCacheCount);
        assert(encoder->submissionFailed && encoder->brcSubmissionCount);
        const unsigned requests = gpu.requests, huc = gpu.hucSubmits, pictures = gpu.pictures;
        assert(huc == (failureStage == 1 ? 1u : failureStage == 3 ? 3u : 2u));
        assert(pictures == (failureStage == 3 ? 1u : 0u));
        assert(hevcSchedule(encoder, &node, &ref) == StatusNotFound);
        gRateUpdates = false;
        assert(hevcSchedule(encoder, &node, &ref) == StatusNotFound);
        assert(gpu.requests == requests && gpu.hucSubmits == huc && gpu.pictures == pictures);
        destroyHevcEncodeContexts();
        setHevcSubmitOps({});
        gBrcFailureStage = 0;
        gCbrRequest = gMockMain10 = false;
        return;
    }
    // One Schedule must resolve AND encode its first picture before returning.
    const int32_t firstStatus = hevcSchedule(encoder, &node, &ref);
    if (firstStatus != StatusSuccess)
        fprintf(stderr, "Schedule failed: main10=%d brc=%d update=%d HuC=%u: %s\n",
            main10, brc, update, gpu.hucSubmits, gBrcLines);
    assert(firstStatus == StatusSuccess);
    const unsigned repeat = update && authenticated && reencode ? 1 : 0;
    const unsigned updatesPerPicture = update ? (nativeBudget || quality ? 1 : 2) : 0;
    assert(gpu.resolves == 1 && gpu.pictures == 1 + repeat);
    assert(gpu.hucSubmits == (brc && authenticated ? 1u + updatesPerPicture : 0u));
    assert(encoder->brcInitialized == (brc && authenticated));
    assert(ref && static_cast<uint32_t *>(ref)[1] ==
        2u + (brc && authenticated ? 1u + updatesPerPicture : 0u) + repeat);
    assert(encoder->sequenceCount == 2); // HuC work is not an encoded picture
    assert(read32(payload, 936) == 1234);
    assert(encoder->headerConfig.main10 == main10);
    if (lowNativeQp)
        assert(nativeBudget && encoder->headerConfig.qp == 10);
    assert(encoder->inputSurface.pitch == (main10 ? 1280 : 640));
    assert(encoder->inputSurface.uvRowOffset == 384);
    assert(hevcSchedule(encoder, &node, &ref) == StatusSuccess);
    assert(gpu.resolves == 1 && gpu.pictures == 2 * (1 + repeat));
    assert(gpu.hucSubmits == (brc && authenticated ? 1u + 2u * updatesPerPicture : 0u));
    if (update && authenticated)
        assert(encoder->brcFrameId == 3);
    // Changing only reconstruction still invalidates the cached addresses.
    write64(payload, 912, 0x6000);
    assert(hevcSchedule(encoder, &node, &ref) == StatusSuccess);
    assert(gpu.resolves == 2 && gpu.pictures == 3 * (1 + repeat));
    write64(payload, 16, 0x8000);
    assert(hevcSchedule(encoder, &node, &ref) == StatusSuccess);
    assert(gpu.resolves == 3 && gpu.pictures == 4 * (1 + repeat));
    if (classification) {
        assert(!brc && findRecon(encoder, 0x6000));
        // Reconstruction slot zero recurs inside a GOP. An admitted L0
        // reconstruction from a completed picture must still produce an
        // inter slice and both low-delay reference lists.
        write32(payload, 896, 0);
        write32(payload, 828, 1);
        write32(payload, 900, 5);
        write64(payload, 912, 0xa000);
        write64(payload, 40, 0x6000);
        write32(payload, 28, 0);
        assert(hevcSchedule(encoder, &node, &ref) == StatusSuccess);
        assert(gpu.sliceType == 0 && gpu.referenceLists == 2);
        assert(encoder->referenceHandle == 0x6000 && findRecon(encoder, 0xa000));

        // A native non-IRAP intra picture has no L0 surface, regardless of
        // its reconstruction index. It must not inherit an old reference.
        write32(payload, 896, 1);
        write32(payload, 900, 6);
        write64(payload, 40, 0);
        assert(hevcSchedule(encoder, &node, &ref) == StatusSuccess);
        assert(gpu.sliceType == 2 && !gpu.referenceLists && !encoder->referenceHandle);

        // IRAP refuses inter prediction even when the payload names a
        // previously completed L0 surface, and discards the old history.
        write32(payload, 828, 19);
        write32(payload, 900, 0);
        write64(payload, 40, 0xa000);
        write32(payload, 28, 6);
        write64(payload, 912, 0xc000);
        assert(hevcSchedule(encoder, &node, &ref) == StatusSuccess);
        assert(gpu.sliceType == 2 && !gpu.referenceLists && !encoder->referenceHandle);
        assert(!findRecon(encoder, 0xa000) && encoder->reconCacheCount == 1);

        // A surface that has never completed is not an admitted reference.
        write32(payload, 828, 1);
        write32(payload, 900, 1);
        write64(payload, 40, 0xd000);
        assert(hevcSchedule(encoder, &node, &ref) == StatusSuccess);
        assert(gpu.sliceType == 2 && !gpu.referenceLists && !encoder->referenceHandle);

        // Software can request intra pictures by omitting L0.
        write64(payload, 40, 0);
        write32(payload, 28, 1);
        write32(payload, 900, 2);
        write64(payload, 912, 0xe000);
        assert(hevcSchedule(encoder, &node, &ref) == StatusSuccess);
        assert(gpu.sliceType == 2 && !gpu.referenceLists && !encoder->referenceHandle);
    }
    if (nativeBudget && !lowNativeQp) {
        assert(brc && update && authenticated && !reencode && !classification);
        gNativeOversizedPicture = true;
        const unsigned attempts = gpu.pictures, huc = gpu.hucSubmits;
        write32(payload, 828, 1); write32(payload, 900, 1);
        write64(payload, 912, 0xa000); write64(payload, 40, 0x6000);
        assert(hevcSchedule(encoder, &node, &ref) == StatusSuccess);
        assert(read32(payload, 936) == 2000 && encoder->headerConfig.qp == 20);
        assert(encoder->vbrBudget.availableBits() == 14000);
        write32(payload, 900, 2); write64(payload, 912, 0xc000);
        write64(payload, 40, 0xa000); write32(payload, 28, 1);
        assert(hevcSchedule(encoder, &node, &ref) == StatusSuccess);
        assert(read32(payload, 936) == 1000 && encoder->headerConfig.qp == 21);
        assert(gpu.pictures == attempts + 3 && gpu.hucSubmits == huc + 3);
        assert(encoder->brcFrameId == 7 && encoder->referenceHandle == 0xa000);
        assert(encoder->vbrBudget.availableBits() == 22000);
        gNativeOversizedPicture = false;
        // A completed frame that cannot fit at QP51 is rejected before
        // publication; the session cannot submit or spend credit afterward.
        encoder->rateInput.qp = 51;
        gNativeUnfittablePicture = true;
    }
    if (firmwareVbr) {
        assert(encoder->vbrBudget.availableBits() > 0);
        const unsigned attempts = gpu.pictures, huc = gpu.hucSubmits;
        gNativeOversizedPicture = true;
        assert(hevcSchedule(encoder, &node, &ref) == StatusSuccess);
        assert(read32(payload, 936) == 1000 && encoder->headerConfig.qp == 26);
        assert(gpu.pictures == attempts + 2 && gpu.hucSubmits == huc + 2);
        assert(encoder->brcFrameId == 6);
        gNativeOversizedPicture = false;
        gNativeUnfittablePicture = true;
    }
    // Missing completion and an oversized QP51 result both refuse publication.
    const unsigned completedPictures = gpu.pictures;
    const uint32_t remainingBudget = encoder->vbrBudget.availableBits();
    gpu.complete = (nativeBudget && !lowNativeQp) || firmwareVbr;
    ref = nullptr;
    assert(hevcSchedule(encoder, &node, &ref) == StatusNotFound);
    assert(!ref && encoder->submissionFailed);
    assert(encoder->vbrBudget.availableBits() == remainingBudget);
    assert(hevcSchedule(encoder, &node, &ref) == StatusNotFound);
    assert(gpu.pictures == completedPictures + 1);
    destroyHevcEncodeContexts();
    setHevcSubmitOps({});
    gMockMain10 = false;
    gRateUpdates = false;
    gMockBrcReencode = false;
}

}

int main() {
    setHevcLog(capture);
    exerciseHeaderCache();
    exerciseBrcPictureData();

    // A planar surface can have padding in both pitch and height. Whole-
    // surface BytesPerRow and visible dimensions must not replace its planes.
    HevcSubmitOps geometry;
    geometry.registerSurface = [](void *, void *) -> int32_t { return 0; };
    geometry.surfaceAddress = [](void *, void *, uint32_t plane,
        uint32_t *address, uint32_t *format) -> int32_t {
        *address = 20 + plane; *format = 0; return 1;
    };
    geometry.surfaceWidth = [](void *) -> unsigned long { return 1920; };
    geometry.surfaceHeight = [](void *) -> unsigned long { return 1080; };
    geometry.surfacePitch = [](void *) -> unsigned long { return 2904; };
    geometry.surfacePlanePitch = [](void *, unsigned long) -> unsigned long { return 2048; };
    geometry.surfacePlaneBase = [](void *, unsigned long plane) -> void * {
        return reinterpret_cast<void *>(0x10000000ul + plane * 2048ul * 1088);
    };
    setHevcSubmitOps(geometry);
    HevcEncodeContext surfaceContext {};
    surfaceContext.surfaceManager = &surfaceContext;
    HevcSurface bound;
    assert(bindPictureSurface(&surfaceContext, &surfaceContext, "test", bound));
    assert(bound.pitch == 2048 && bound.uvRowOffset == 1088 && bound.height == 1080);
    geometry.surfacePlaneBase = nullptr;
    setHevcSubmitOps(geometry);
    assert(!bindPictureSurface(&surfaceContext, &surfaceContext, "test", bound));
    assert(!bound.valid);
    setHevcSubmitOps({});

    ContextDesc desc = descriptor(1001, 1600, 608);
    void *context = createHevcEncodeContext(nullptr, &desc);
    assert(context);
    assert(isHevcEncodeContext(context));
    assert(findHevcEncodeContext(1001) == context);
    assert(!isHevcEncodeContext(nullptr));

    uint32_t configuration[36] {};
    configuration[0] = 2; // first word of the encoder configuration block
    ContextParamEntry set1 = entry(1, configuration);
    set1.context = context;
    assert(hevcSetContextParam(context, &set1) == StatusSuccess);

    uint32_t pair[2] = {7, 11};
    ContextParamEntry set5 = entry(5, pair);
    set5.context = context;
    assert(hevcSetContextParam(context, &set5) == StatusSuccess);

    uint32_t size = 0;
    ContextParamEntry get2 = entry(2, &size);
    get2.context = context;
    assert(hevcGetContextParam(context, &get2) == StatusSuccess);
    assert(size == 64);

    uint32_t other = 0;
    ContextParamEntry unknown = entry(9, &other);
    unknown.context = context;
    assert(hevcSetContextParam(context, &unknown) == StatusInvalidParameter);

    // A payload with the fields the encoder fills for a picture.
    uint8_t payload[968] {};
    const uint32_t reconstructionIndex = 1;
    const uint32_t width = 1600;
    const uint32_t height = 608;
    memcpy(payload + 896, &reconstructionIndex, 4);
    memcpy(payload + 900, &width, 4);
    memcpy(payload + 904, &height, 4);
    const uintptr_t input = 0x11110000;
    const uintptr_t recon = 0x22220000;
    memcpy(payload + 16, &input, sizeof(input));
    memcpy(payload + 912, &recon, sizeof(recon));

    CommandNode node {};
    node.type = kHevcEncodeContextType;
    node.subtype = kHevcEncodeContextSubtype;
    node.id = 1001;
    node.context = context;
    node.payload = payload;

    // Without the exact driver's submission entry points, Schedule fails
    // after parsing the payload.
    assert(hevcSchedule(context, &node, nullptr) == StatusNotFound);

    auto *encoder = static_cast<HevcEncodeContext *>(context);
    uint32_t completion = 7;
    assert(checkPictureCompletion(encoder, StatusSuccess, &completion, 7));
    assert(!encoder->submissionFailed);
    // A stale tag and a fence released after reset must not pass as a frame.
    assert(!checkPictureCompletion(encoder, StatusSuccess, &completion, 8));
    assert(encoder->submissionFailed);
    const auto schedules = encoder->scheduleCount;
    assert(hevcSchedule(context, &node, nullptr) == StatusNotFound);
    assert(encoder->scheduleCount == schedules);
    encoder->submissionFailed = false;
    assert(!checkPictureCompletion(encoder, StatusNotFound, &completion, 7));
    encoder->submissionFailed = false;
    completion = 0;
    assert(!checkPictureCompletion(encoder, StatusSuccess, &completion, 7));
    encoder->submissionFailed = false;
    assert(strstr(gLastLine, "hevc") != nullptr);

    // Native output ABI: caller owns the pointer slot, encoder owns the
    // render/media fence record. Byte ranges are 64-bit and must not touch
    // the adjacent pool identity or statistics pointer.
    memset(payload + 920, 0xa5, 32);
    void *outputRef = nullptr;
    assert(publishPictureResult(encoder, payload, &outputRef, 42934, 1u << 20, 17));
    assert(read32(payload, 920) == 0xa5a5a5a5);
    assert(read32(payload, 924) == 0xa5a5a5a5);
    assert(read32(payload, 928) == 0 && read32(payload, 932) == 0);
    assert(read32(payload, 936) == 42934 && read32(payload, 940) == 0);
    assert(read32(payload, 944) == 0xa5a5a5a5);
    auto *firstRef = static_cast<uint32_t *>(outputRef);
    assert(firstRef && firstRef[0] == 0 && firstRef[1] == 17);
    assert(publishPictureResult(encoder, payload, &outputRef, 2000, 4096, 18));
    assert(outputRef != firstRef && firstRef[1] == 17);
    assert(static_cast<uint32_t *>(outputRef)[1] == 18);
    assert(!publishPictureResult(encoder, payload, &outputRef, 4097, 4096, 19));
    assert(!publishPictureResult(encoder, payload, &outputRef, 0, 4096, 19));
    assert(read32(payload, 936) == 2000);

    // An IRAP picture starts a new coded video sequence. The decoder empties
    // its picture buffer, so the POCs remembered for the keep list and the
    // surface-to-POC map from the previous sequence must not reach the new
    // slice: listing them makes the next slice name pictures the decoder can
    // no longer find.
    {
        HevcEncodeContext fresh {};
        fresh.recentPoc[0] = 0;
        fresh.recentPoc[1] = 2;
        fresh.recentPocCount = 2;
        fresh.recentPocNext = 2;
        fresh.reconCacheCount = 3;
        fresh.reconCacheNext = 3;
        resetReferenceHistory(&fresh);
        assert(fresh.recentPocCount == 0 && fresh.recentPocNext == 0);
        assert(fresh.reconCacheCount == 0 && fresh.reconCacheNext == 0);
        HevcPictureDesc picture {};
        picture.poc = 4;
        picture.inter = true;
        picture.refPoc = 2;
        fillReferenceSet(&fresh, picture);
        assert(picture.rpsCount == 1);
        assert(picture.rpsPocs[0] == 2);
    }

    // The surface-to-POC cache holds eight entries and then evicts round-robin.
    // The ninth distinct surface must still be remembered together with the
    // downscale slot it wrote: an entry recorded without its slot is invalid,
    // and a later inter picture that references that surface silently falls
    // back to an I picture.
    {
        HevcEncodeContext fresh {};
        for (uint32_t i = 0; i < 9; ++i)
            rememberRecon(&fresh, 0x100 + i, i, 0x40000000u + i * 0x2000u,
                i % 2);
        assert(!findRecon(&fresh, 0x100)); // first entry was evicted
        const auto *ninth = findRecon(&fresh, 0x108);
        assert(ninth);
        assert(ninth->poc == 8);
        assert(ninth->address == 0x40000000u + 8 * 0x2000u);
        assert(ninth->scaledSlot == 0);
        // A reused surface updates the existing entry in place.
        rememberRecon(&fresh, 0x108, 12, 0x50000000u, 1);
        const auto *updated = findRecon(&fresh, 0x108);
        assert(updated && updated->poc == 12 && updated->scaledSlot == 1);
        assert(updated->address == 0x50000000u);
    }

    node.payload = nullptr;
    assert(hevcSchedule(context, &node, nullptr) == StatusInvalidParameter);

    // A context for another codec is not ours.
    ContextDesc avc = descriptor(1001, 640, 480);
    avc.subtype = 0x400;
    assert(!createHevcEncodeContext(nullptr, &avc));

    {
        HevcEncodeContext resolved {};
        HevcPictureDesc picture;
        picture.source.pitch = picture.recon.pitch = 3808;
        picture.source.uvRowOffset = picture.recon.uvRowOffset = 2144;
        resolved.resolvedAddress[0] = 0x100000;
        resolved.resolvedAddress[1] = 0x1000000;
        resolved.resolvedAddress[19] = 0x100000 + 3840 * 2144;
        resolved.resolvedAddress[20] = 0x1000000 + 3840 * 2144;
        resolved.resolvedSize[0] = resolved.resolvedSize[1] = 3840;
        resolved.resolvedSize[kReferenceResourceIndex] = 3840;
        applyResolvedAddresses(&resolved, picture);
        assert(picture.source.pitch == 3840 && picture.recon.pitch == 3840);
        assert(picture.reference.pitch == 3840);
        assert(picture.source.uvRowOffset == 2144 && picture.recon.uvRowOffset == 2144);
    }

    destroyHevcEncodeContexts();
    assert(!isHevcEncodeContext(context));
    assert(!findHevcEncodeContext(1001));

    exerciseSubmission();
    exerciseSubmission(true);
    exerciseSubmission(false, true);
    exerciseSubmission(false, true, false);
    exerciseSubmission(false, true, true, true);
    exerciseSubmission(true, true, true, true);
    exerciseSubmission(false, true, false, true);
    exerciseSubmission(false, true, true, true, true);
    exerciseSubmission(true, true, true, true, true);
    exerciseSubmission(false, false, true, false, false, true);
    exerciseSubmission(true, false, true, false, false, true);

    exerciseSubmission(false, true, true, true, false, false, true);
    exerciseSubmission(true, true, true, true, false, false, true);
    exerciseSubmission(false, true, true, true, false, false, true, true);
    // Native GOP1 with inter enabled, and native GOP16 with inter disabled,
    // must both use HuC VBR and preserve the same completed-output budgets.
    for (unsigned mode = 1; mode <= 2; ++mode) {
        exerciseSubmission(false, true, true, true, false, false, false, false, mode);
        exerciseSubmission(true, true, true, true, false, false, false, false, mode);
    }
    for (unsigned failure = 1; failure <= 4; ++failure) {
        exerciseSubmission(false, true, true, true, false, false, false, false, 0, failure);
        exerciseSubmission(true, true, true, true, false, false, false, false, 0, failure);
    }
    exerciseSubmission(false, true, true, true, false, false, false, false, 0, 0, true);
    exerciseSubmission(true, true, true, true, false, false, false, false, 0, 0, true);
    puts("PASS HEVC encode context including native VBR window retries");
    return 0;
}
