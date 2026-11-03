// SPDX-License-Identifier: BSD-3-Clause
// Shim-side HEVC encode context. The class layout is private to the shim; the
// driver only touches it through the virtual methods at offsets 24, 32 and 40,
// which are the SetContextParam, GetContextParam and Schedule entry points the
// shipped TGL driver dispatches to.
#include "hevc_context.hpp"
#include "hevc_plugin.hpp"

#include <stdint.h>
#include <string.h>

#include "hevc_commands.hpp"
#include "hevc_token.hpp"
#include "hevc_buffer_sizes.hpp"
#include "../hevc_core/hevc_headers.hpp"
#include "../hevc_core/huc_commands.hpp"
#include "../hevc_core/huc_dmem.hpp"
#include "../hevc_core/huc_brc_dmem.hpp"
#include "../hevc_core/brc_window.hpp"
#include "../hevc_core/vdenc_streamin.hpp"

extern "C" void *calloc(unsigned long count, unsigned long size) noexcept;
extern "C" void free(void *pointer) noexcept;
extern "C" char *getenv(const char *name) noexcept;
// Only used to release the renderer's data-buffer lock when its submit path
// returns before it reaches the unlock.
extern "C" int pthread_mutex_unlock(void *mutex);
// Client VM backing store for driver-owned linear resources. The shipped
// driver passes a vm_allocate range to IOAccelResourceCreate, and the kernel
// maps that range into the media address space.
extern "C" unsigned int mach_task_self_;
extern "C" int vm_allocate(unsigned int task, unsigned long *address,
    unsigned long size, int flags);
extern "C" int vm_deallocate(unsigned int task, unsigned long address,
    unsigned long size);

namespace VaShim {
namespace {

// The encoder plugin keeps at most eight encode contexts per driver. Leave a
// little room for a session that recreates them.
constexpr uint32_t kMaxContexts = 8;
constexpr uint32_t kMaxLoggedSchedules = 8;

// Picture payload offsets used by the encoder plugin. They come from the
// payload builder in AppleGVAHEVCEncoder (CVPixelBufferGetIOSurface at +16 and
// +912, picture fields around +828..+904).
constexpr uint32_t kPayloadInputSurface = 16;
constexpr uint32_t kPayloadNalType = 828;
constexpr uint32_t kPayloadTemporalId = 840;
constexpr uint32_t kPayloadSliceCount = 844;
constexpr uint32_t kPayloadReconstructionIndex = 896;
constexpr uint32_t kPayloadPoc = 900;
constexpr uint32_t kPayloadField904 = 904;
constexpr uint32_t kPayloadReconSurface = 912;
constexpr uint32_t kPayloadExtra = 944;
constexpr uint32_t kPayloadExtra2 = 952;
// Reference picture the plug-in expects this picture to predict from. The
// payload carries the surface pointer and its POC in two parallel fields for
// list 0 and list 1; the surface pointer moves with each picture.
constexpr uint32_t kPayloadRefL0Surface = 0x28;
constexpr uint32_t kPayloadRefL0Poc = 0x1C;
constexpr uint32_t kPayloadRefL1Surface = 0x1A8;
constexpr uint32_t kPayloadRefL1Poc = 0x19C;
// Bitstream buffer identity. The plug-in's payload debug dump names these
// fields poolId, bufferId, readOffset and writeOffset.
constexpr uint32_t kPayloadPoolId = 920;
constexpr uint32_t kPayloadBufferId = 924;

// TGL driver layout. The surface manager is a 0x1010-byte object at driver +
// 0xA150; its IOAccelShared reference is attached from driver + 0x38 and the
// attached flag sits at driver + 0xA178. The two buffer pools start at
// driver + 0x80 (pool 0) and driver + 0x1090 (pool 5); each pool holds up to
// 128 0x20-byte plane entries whose client-shared pointer is at +0x10.
constexpr uint32_t kSurfaceManagerOffset = 0xA150;
constexpr uint32_t kSurfaceManagerAttached = 0xA178;
constexpr uint32_t kDriverSharedObject = 0x38;
constexpr uint32_t kPool0Offset = 0x80;
constexpr uint32_t kPool5Offset = 0x1090;
constexpr uint32_t kPoolBufferId = 4;
constexpr uint32_t kPoolBufferSize = 8;
constexpr uint32_t kPoolEntryShared = 0x20;
// Accelerator id passed to IOSurfaceBindAccel by the shipped encode paths.
constexpr uint32_t kDriverAcceleratorId = 0xA86C;
// The client-shared page stores the 32-bit resource handle at +0x100 for both
// surface-manager planes and driver buffer resources. The kernel resolves the
// handle; it is not a byte address.
constexpr uint32_t kClientSharedAddress = 0x100;

// Internal scratch buffers. Each picture buffer is a separate IOSurface so the
// surface manager assigns it its own resource handle; the driver's address
// values are handles, not byte addresses inside one allocation.
constexpr uint32_t kScratchPitch = 16u * 1024; // 4096 BGRA pixels per row
// The 4x/8x downscale recon pool keeps one slot per tracked picture: a
// picture writes its own downscale into one slot while reading the reference's
// from the slot that picture wrote.
constexpr uint32_t kScaledPoolSlots = 2;
constexpr uint32_t kBitstreamSize = 4u * 1024 * 1024;
constexpr uint32_t kPakBseSize = 2u * 1024 * 1024;
constexpr uint32_t kCuRecordSize = 1u * 1024 * 1024;
constexpr uint32_t kStreamInSize = 512u * 1024;
constexpr uint32_t kRowStoreSize = 512u * 1024;
constexpr uint32_t kSliceSizeSize = 64u * 1024;
constexpr uint32_t kStatisticsSize = 256u * 1024;
// HuC PAK-integration resources.
constexpr uint32_t kSecondLevelSize = 64u * 1024;
constexpr uint32_t kDmemSize = 64u * 1024;
constexpr uint32_t kBrcReadbackOffset = 4096;
constexpr uint32_t kBrcConstantsOffset = kSecondLevelSize;
constexpr uint32_t kBrcPakInfoOffset = kBrcConstantsOffset + 4096;
constexpr uint32_t kAggregatedStatsSize = 256u * 1024;
constexpr uint32_t kBrcHistorySize = 256u * 1024;
constexpr uint32_t kBrcDataSize = 256u * 1024;
constexpr uint32_t kTileRecordSize = 256u * 1024;

enum ScratchIndex : uint32_t {
    kScratchBitstream = 0,
    kScratchPakBse,
    kScratchCuRecord,
    kScratchStreamIn,
    kScratchRowStore,
    kScratchSliceSize,
    kScratchStatistics,
    kScratchSecondLevel,
    kScratchDmem,
    kScratchAggregatedStats,
    kScratchBrcHistory,
    kScratchBrcData,
    kScratchTileRecord,
    kScratchScaled4x,
    kScratchScaled8x,
    kScratchHcpStreamout,
    kScratchSseRowstore,
    kScratchHcp,
    kScratchCount,
};
// The picture resolves the source, the reconstruction, the reference this
// picture predicts from and every scratch buffer. media-driver adds the
// reference surfaces to the command buffer as resources for each picture, so
// the kernel maps them for that submission; the reference occupies the last
// slot and is programmed with the current reconstruction for an I picture,
// which has no reference of its own.
constexpr uint32_t kPictureResourceCount = kScratchCount + 5;
constexpr uint32_t kReferenceResourceIndex = kPictureResourceCount - 1;
static_assert(kPictureResourceCount <= HevcToken::kMaxResources, "resolve table capacity");

// IOAccelVideoContext fields used by the renderer's submission path.
constexpr uint32_t kVideoCommandStart = 112; // descriptor + 112
constexpr uint32_t kVideoSidebandStart = 120;
constexpr uint32_t kVideoCommandBase = 16;
constexpr uint32_t kVideoMutex = 216;
constexpr uint32_t kPlaceholderBatchEnd = 0x05000000;
// The resolve probe submits only MI_BATCH_BUFFER_END; the kernel writes the
// resolved addresses into the sideband token and into the batch area.
constexpr uint32_t kResolveCommandDwords = 1024;
// Dead space after the picture batch for the token anchors. The kernel writes
// up to 122 dwords past the anchor.
constexpr uint32_t kResolveDeadDwords = 160;

// A driver-owned linear resource. Unlike an IOSurface-backed resource, the
// client-shared page carries a real graphics address, so commands and HuC
// virtual-address regions can name it directly.
struct LinearResource {
    void *resource {};
    void *base {};
    uint32_t address {};
    uint32_t size {};
};

struct HevcEncodeContext {
    void **vtable;          // +0
    void *driver;           // +8
    void *videoContext;     // +16, renderer IOAccelVideoContext
    void *submitContext;    // picture submissions (engine override probe)
    uint32_t id;            // +24
    uint32_t width;         // +28
    uint32_t height;        // +32
    uint32_t setParam1;     // +36
    uint32_t setParam5[2];  // +40
    uint32_t getParam2;     // +48
    uint32_t scheduleCount; // +52
    uint32_t logged;        // +56
    uint32_t loggedConfig;  // +60
    uint32_t format;        // +64, surface pixel format
    uint32_t poolInitialized; // +68
    void *surfaceManager;   // +72
    uint32_t surfacesLogged; // +80
    HevcSurface inputSurface;
    HevcSurface reconSurface;
    // The plug-in's recon surface object for the current picture. The cache
    // below is keyed by this pointer, which is what the payload returns as a
    // reference; HevcSurface.address is the driver resource handle instead.
    void *payloadReconSurface;
    // Recon surfaces already resolved to graphics addresses, keyed by the
    // surface handle. A P picture looks its reference up here; an entry is
    // refreshed whenever the plug-in reuses the surface for a newer picture.
    struct ReconCacheEntry {
        uint32_t handle;
        uint32_t poc;
        uint32_t address;
        // Downscale pool slot this picture wrote its 4x/8x recon into.
        uint32_t scaledSlot;
        bool valid;
    };
    ReconCacheEntry reconCache[8];
    uint32_t reconCacheCount;
    uint32_t reconCacheNext;
    // Surface handle and resolved graphics address of the reference this
    // picture predicts from. The handle is what the resolve token lists; the
    // address returned with the picture is used when it matches.
    uint32_t referenceHandle;
    uint32_t resolvedReferenceHandle;
    // Downscale pool slot the picture being submitted writes into. Advances
    // after every completed picture so the next one reads this slot and writes
    // the other.
    uint32_t scaledSlot;
    // Source graphics address of the last completed picture. The diagnostic
    // marker uses it as an inter picture's reference.
    uint32_t lastPictureSource;
    // POCs of the most recently completed pictures in decode order. Every
    // slice lists them in its reference picture set so they stay in the DPB
    // until the plug-in stops referencing them.
    uint32_t recentPoc[3];
    uint32_t recentPocCount;
    uint32_t recentPocNext;
    void *scratchOwners[kScratchCount]; // retaining CVPixelBuffers, held for the context lifetime
    void *scratchSurfaces[kScratchCount]; // IOSurfaces for CPU writes
    LinearResource linear[kScratchCount];
    HevcBuffers scratch;
    // Cached VPS/SPS/PPS for the current coding configuration, and the per-picture
    // slice header handed to the PAK insert.
    void *parameterSets;
    uint32_t parameterSetBytes;
    HevcCore::Config headerConfig;
    void *sliceHeader;
    uint32_t sliceHeaderBytes;
    uint32_t sliceHeaderCapacity;
    uint32_t sliceHeaderEmulatedBits;
    uint32_t scratchTried;
    uint32_t sequenceCount;
    uint32_t realSubmits;
    uint32_t lastFence;
    uint32_t completionRecords[32][2];
    uint32_t nextCompletionRecord;
    bool submissionFailed;
    HevcPlugin::RateControlInput rateInput;
    bool rateInputValid;
    bool brcInitialized;
    uint32_t brcSubmissionCount;
    uint32_t brcLastKernelStatus;
    HevcCore::Huc::BrcInitDmem brcConfiguration;
    uint32_t brcFrameId;
    uint32_t brcOutputQp;
    bool brcActive;
    HevcCore::FrameBudget vbrBudget;
    HevcBrcBatchLayout brcLayout;
    struct BrcPictureData {
        uint32_t poc, age;
        bool valid;
    } brcPictureData[HevcCore::Huc::kBrcPictureDataSlots];
    // Graphics addresses returned by the kernel resolve token, in the order
    // the probe listed them: source, recon, then the scratch buffers.
    uint64_t resolvedAddress[kPictureResourceCount];
    uint32_t resolvedSize[kPictureResourceCount];
    uint32_t resolvedCount;
    uint32_t resolvedSourceHandle;
    uint32_t resolveCount;
    // Resource handles in the same order as resolvedAddress. The batch carries
    // the resolved graphics addresses while the kernel resolve token takes the
    // handles, so both are needed once the addresses are applied.
    uint32_t pictureHandle[kPictureResourceCount];
};

// The renderer keeps its three IOAccelVideoContext objects at driver+80,
// driver+88 and driver+96. The shipped context initializer maps AVD context
// ids onto them: 1001 uses +88, 1003 uses +96 and 1002 uses +80. Using the
// wrong proxy reaches a kernel context without the media sideband dispatcher,
// which rejects the 0x8D VDENC token.
constexpr uint32_t kVideoContextOffset = 80;

uint32_t videoContextOffsetForId(uint32_t id) {
    if (id == 1001)
        return kVideoContextOffset + 8;
    if (id == 1003)
        return kVideoContextOffset + 16;
    return kVideoContextOffset;
}

char gHevcLine[320];
HevcLogFn gLog;
HevcSubmitOps gOps;
void (*gCompletionSignal)();
HevcEncodeContext *gContexts[kMaxContexts];

void append(char *&out, const char *text) {
    while (*text)
        *out++ = *text++;
}

void appendDec(char *&out, uint64_t value) {
    char buffer[24];
    int count = 0;
    do {
        buffer[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        *out++ = buffer[--count];
}

void appendHex(char *&out, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    *out++ = '0';
    *out++ = 'x';
    char buffer[16];
    int count = 0;
    do {
        buffer[count++] = digits[value & 0xF];
        value >>= 4;
    } while (value);
    while (count)
        *out++ = buffer[--count];
}

void emit(const char *line, char *end) {
    *end = 0;
    if (gLog)
        gLog(line);
}

void logMessage(const char *message) {
    if (!gLog)
        return;
    char *out = gHevcLine;
    append(out, "hevc ");
    append(out, message);
    emit(gHevcLine, out);
}

void logValue(const char *label, uint64_t value) {
    if (!gLog)
        return;
    char *out = gHevcLine;
    append(out, "hevc ");
    append(out, label);
    append(out, "=");
    appendDec(out, value);
    emit(gHevcLine, out);
}

void logPointer(const char *label, const void *value) {
    if (!gLog)
        return;
    char *out = gHevcLine;
    append(out, "hevc ");
    append(out, label);
    append(out, "=");
    appendHex(out, reinterpret_cast<uintptr_t>(value));
    emit(gHevcLine, out);
}

uint32_t read32(const void *base, uint32_t offset) {
    const auto *bytes = static_cast<const uint8_t *>(base);
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<uint32_t>(bytes[offset + i]) << (8 * i);
    return value;
}

void *readPointer(const void *base, uint32_t offset) {
    const auto *bytes = static_cast<const uint8_t *>(base);
    uintptr_t value = 0;
    for (unsigned i = 0; i < sizeof(uintptr_t); ++i)
        value |= static_cast<uintptr_t>(bytes[offset + i]) << (8 * i);
    return reinterpret_cast<void *>(value);
}

void write32(void *base, uint32_t offset, uint32_t value) {
    auto *bytes = static_cast<uint8_t *>(base);
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

void write64(void *base, uint32_t offset, uint64_t value) {
    auto *bytes = static_cast<uint8_t *>(base);
    for (unsigned i = 0; i < 8; ++i)
        bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

void logConfig(const void *value) {
    if (!gLog || !value)
        return;
    for (unsigned base = 0; base < 12; base += 6) {
        char *out = gHevcLine;
        append(out, "hevc cfg");
        appendDec(out, base);
        append(out, "=");
        for (unsigned i = 0; i < 6; ++i) {
            if (i)
                *out++ = ',';
            appendDec(out, read32(value, (base + i) * 4));
        }
        emit(gHevcLine, out);
    }
}

int32_t destroyThunk(void *context);

int32_t unsupported(void *, void *, void *) {
    return StatusInvalidParameter;
}

// The driver's surface manager accessor (0x3E45 in TGL VA 16.0.32) attaches
// the IOAccelShared reference before the first surface is registered. The
// attached flag lives in the driver object, so attaching twice is harmless.
void attachSurfaceManager(HevcEncodeContext *encoder) {
    if (!encoder->surfaceManager)
        return;
    auto *driver = static_cast<char *>(encoder->driver);
    if (!driver || driver[kSurfaceManagerAttached])
        return;
    if (!gOps.attachShared)
        return;
    void *shared = *reinterpret_cast<void **>(driver + kDriverSharedObject);
    if (!shared) {
        logMessage("surface manager shared object missing");
        return;
    }
    gOps.attachShared(shared, encoder->surfaceManager);
    logMessage("surface manager shared attached");
}

void logSurface(const char *label, const HevcSurface &surface) {
    if (!gLog)
        return;
    char *out = gHevcLine;
    append(out, "hevc ");
    append(out, label);
    append(out, " addr=");
    appendHex(out, surface.address);
    append(out, " uv=");
    appendHex(out, surface.uvAddress);
    append(out, " pitch=");
    appendDec(out, surface.pitch);
    append(out, " size=");
    appendDec(out, surface.width);
    *out++ = 'x';
    appendDec(out, surface.height);
    append(out, " format=");
    appendHex(out, surface.format);
    append(out, " uvRow=");
    appendDec(out, surface.uvRowOffset);
    emit(gHevcLine, out);
}

void dumpSurfaceGeometry(const char *label, void *surface, uintptr_t luma,
    uint32_t rowBytes, uint32_t format);

// Registers an IOSurface with the renderer's surface manager and reads the
// GPU addresses of its luma and chroma planes. The manager keys entries by
// IOSurface ID, so repeated calls for the same surface are cheap.
bool bindPictureSurface(HevcEncodeContext *encoder, void *surface,
    const char *label, HevcSurface &result) {
    if (!surface || !encoder->surfaceManager || !gOps.registerSurface ||
        !gOps.surfaceAddress)
        return false;
    const int32_t status = gOps.registerSurface(encoder->surfaceManager, surface);
    if (status != 0) {
        logValue("surface register status", static_cast<uint32_t>(status));
        return false;
    }
    uint32_t address = 0;
    uint32_t value = 0;
    if (!gOps.surfaceAddress(surface, encoder->surfaceManager, 0, &address, &value)) {
        logMessage("surface plane 0 address unavailable");
        return false;
    }
    uint32_t chroma = 0;
    uint32_t chromaValue = 0;
    gOps.surfaceAddress(surface, encoder->surfaceManager, 1, &chroma, &chromaValue);
    if (gOps.bindSurface)
        gOps.bindSurface(encoder->surfaceManager, surface);
    // The shipped AVC path binds the surface to the accelerator after the
    // surface manager registration. Do the same so the kernel maps the
    // surface before a batch references it.
    if (gOps.surfaceBindAccel && encoder->driver) {
        gOps.surfaceBindAccel(surface,
            read32(encoder->driver, kDriverAcceleratorId), 0);
    }

    result.address = address;
    result.uvAddress = chroma;
    // `value` is the driver's surface-format enum; the command builders need
    // the IOSurface pixel format to distinguish P010 from NV12.
    result.format = gOps.surfaceFormat
        ? gOps.surfaceFormat(surface) : value;
    const uint32_t iosurfacePitch = gOps.surfacePitch
        ? static_cast<uint32_t>(gOps.surfacePitch(surface)) : 0;
    result.width = gOps.surfaceWidth
        ? static_cast<uint32_t>(gOps.surfaceWidth(surface)) : encoder->width;
    result.height = gOps.surfaceHeight
        ? static_cast<uint32_t>(gOps.surfaceHeight(surface)) : encoder->height;
    // The whole-surface BytesPerRow value is not the luma pitch of a planar
    // IOSurface. Plane padding also makes the chroma start differ from the
    // visible height (for example, 1080 lines stored in 1088 rows).
    result.pitch = gOps.surfacePlanePitch
        ? static_cast<uint32_t>(gOps.surfacePlanePitch(surface, 0)) : 0;
    const uintptr_t luma = gOps.surfacePlaneBase
        ? reinterpret_cast<uintptr_t>(gOps.surfacePlaneBase(surface, 0)) : 0;
    const uintptr_t uv = gOps.surfacePlaneBase
        ? reinterpret_cast<uintptr_t>(gOps.surfacePlaneBase(surface, 1)) : 0;
    if (!result.pitch || !luma) {
        result.valid = false;
        logMessage("surface plane geometry unavailable");
        return false;
    }
    result.uvRowOffset = HevcCore::uvRowOffsetFromAddresses(luma, uv,
        result.pitch);
    if (!result.uvRowOffset) {
        result.valid = false;
        logMessage("surface plane geometry unavailable");
        return false;
    }
    result.valid = address != 0;
    logSurface(label, result);
    if (iosurfacePitch && gLog) {
        char *out = gHevcLine;
        append(out, "hevc ");
        append(out, label);
        append(out, " iosurface pitch=");
        appendDec(out, iosurfacePitch);
        emit(gHevcLine, out);
    }
    dumpSurfaceGeometry(label, surface, luma, iosurfacePitch,
        result.format);
    return result.valid;
}

// Resolves a buffer created through the driver's CreateBuffer slot. Pool 0 and
// pool 5 each keep one buffer identity plus one client-shared page per plane.
bool resolvePoolBuffer(HevcEncodeContext *encoder, uint32_t poolId,
    uint32_t bufferId, HevcBuffer &result) {
    if (!encoder->driver)
        return false;
    const uint32_t offset = poolId == 5 ? kPool5Offset : kPool0Offset;
    const char *pool = static_cast<char *>(encoder->driver) + offset;
    if (read32(pool, kPoolBufferId) != bufferId)
        return false;
    void *shared = readPointer(pool, kPoolEntryShared);
    if (!shared)
        return false;
    result.address = read32(shared, kClientSharedAddress);
    result.size = read32(pool, kPoolBufferSize);
    result.cpuAddress = readPointer(pool, 0x10);
    result.valid = result.address != 0;
    return result.valid;
}

// The encoder XPC service runs with a sanitized environment, so launchd
// variables never reach it. Optional diagnostic markers next to the installed
// shim remain available; normal capability selection follows software requests.
constexpr const char *kAllocMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_ALLOC";
constexpr const char *kBuildMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_BUILD";
constexpr const char *kDumpMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_DUMP";
constexpr const char *kHucMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_HUC";
constexpr const char *kHucStatusMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_HUC_STATUS";
constexpr const char *kHucImemMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_HUC_IMEM";
// Diagnostic: keep the picture state commands inline in the ring batch
// instead of starting them as a second-level batch.
constexpr const char *kInlineMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_INLINE";
constexpr const char *kSliceEndMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_SLICEEND";
constexpr const char *kPartialFrameMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_PFU";
// Submits a sentinel-only batch once the addresses are known. It isolates the
// engine execution path from the HCP/VDEnc command contents.
constexpr const char *kPingMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_PING";
// Interleaves stores that record which packet group the engine reached.
constexpr const char *kCheckpointsMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_CHECKPOINTS";
// Fills the write-side scratch buffers with a poison pattern before submit, so
// the post dump shows whether the engine wrote anything into them.
constexpr const char *kPoisonMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_POISON";
// Diagnostic: drops the slice state and PAK inserts, so the VDEnc walker runs
// without a PAK consumer and its output can be observed in isolation.
constexpr const char *kNoPakMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_NOPAK";
// Diagnostic: makes the VDEnc stream its LCU PAK object commands out to the
// object buffer instead of the internal VDEnc-to-PAK path.
// Diagnostic: drop the VDEnc walker so the PAK slice commands are the only
// work in the batch.
constexpr const char *kNoWalkerMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_NOWALK";
// Diagnostic: makes the VDEnc stream its LCU PAK object commands out to the
// object buffer instead of the internal VDEnc-to-PAK path.
constexpr const char *kObjOutMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_OBJOUT";
// Diagnostic: point the PAK's indirect CU object surface at the buffer the
// VDEnc is given for CU-record streamout, so a record written there is read by
// the PAK instead of the object block inside pakBse.
constexpr const char *kCuRecMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_CUREC";
// Diagnostic: print the raw VDEnc PAK-object stream so the per-CU hand-off
// words can be diffed between a completing and a stalled picture.
constexpr const char *kObjRawMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_OBJRAW";
// Diagnostic: put the PAK's indirect CU object at the same base as the VDEnc
// LCU PAK object command stream. The softlet HEVC VDEnc packet leaves
// m_mvOffset at zero, so both engines use the buffer base.
constexpr const char *kCuObjBaseMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_CUOBJ0";
// Diagnostic: code an inter picture as a P slice whose coding units are all
// intra (VDEnc intra-only picture mode). Separates a PAK failure on inter CUs
// from a PAK failure on the P slice path itself.
constexpr const char *kInterIntraMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_P_INTRA";
// Diagnostic: raise VDENC_CMD2.DW2.TemporalMvpEnableFlag for inter pictures.
constexpr const char *kInterTemporalMvpMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_P_TMVP";
// Diagnostic: clear HCP_PIC_STATE.DW6.LcuMaxBitsizeAllowed for inter pictures,
// i.e. code them with no per-LCU bit bound the way media-driver does for CQP.
constexpr const char *kNoLcuMaxMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_NOLCUMAX";
// Diagnostic: clear cu_qp_delta for inter pictures (PPS and HCP picture state).
constexpr const char *kPNoCuQpMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_P_NOCUQP";
// Diagnostic: signal cu_qp_delta in the PPS and the HCP picture state. The
// Gen12 HCP is documented to require it in VDEnc mode.
constexpr const char *kCuQpMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_CUQP";
// Diagnostic: declare a 16x16 maximum luma transform block instead of 32x32.
constexpr const char *kTu16Marker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_TU16";
// Diagnostic: declare an 8x8 maximum luma transform block.
constexpr const char *kTu8Marker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_TU8";
// Diagnostic: change the picture QP. A coarse QP removes most coefficients
// from the inter coding units, which separates "any inter residual stalls" from
// "a large coefficient payload stalls".
constexpr const char *kQpHighMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_QP51";
constexpr const char *kQpMidMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_QP40";
constexpr const char *kQpLowMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_QP10";
// Diagnostic: zero the shared HCP/VDEnc scratch (line buffers, temporal MV,
// tile row store, cumulative CU count) before the picture, instead of leaving
// whatever the previous picture stored there. A real driver allocates those
// buffers per picture, so an element read before it is written must see zero.
constexpr const char *kZeroMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_ZERO";
// Diagnostic: point an inter picture's reference at its own source surface.
// Keeps the P syntax but removes the previous recon surface from the read
// path, which separates a reference-surface problem from a PAK syntax one.
constexpr const char *kSelfRefMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_P_SELF";
// Diagnostic: predict an inter picture from the previous picture's source
// surface instead of its reconstruction. Both are valid, mapped surfaces of
// the same geometry, so the result separates a reconstruction problem (layout
// or contents) from the inter coding path itself.
constexpr const char *kPrevSourceMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_P_PREVSRC";
// Diagnostic: dump the raw picture payload and the two extra blocks it points
// at. Used to map fields Apple's plug-in supplies for inter pictures.
constexpr const char *kPayloadDumpMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_PAYLOAD";
// Diagnostic: keep the inter picture stream but drop the reference 4x/8x
// downscale binding, so the VDEnc searches the full-resolution reference only.
constexpr const char *kNoDsRefMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_NO_DSREF";
// Diagnostic: insert a video-pipeline flush between the picture-level and the
// slice-level command groups. media-driver submits the two levels as separate
// command buffers, so the picture-level submission ends at a batch boundary;
// this reproduces that boundary inside one batch.
constexpr const char *kPictureFlushMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_PICFLUSH";
// Diagnostic: walk one CTB instead of the whole picture, so the VDEnc's PAK
// object stream stays tiny. Separates a volume-dependent handoff stall from a
// stall that happens before the first object.
constexpr const char *kOneCtbMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_ONE_CTB";
// Diagnostic: copy the current source pixels into the reference surface before
// the inter picture is submitted. The picture then predicts perfectly while
// the reference keeps its own address, which separates "the engines cannot read
// this surface" from "inter coding with motion and residual stalls".
constexpr const char *kCopySourceMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_P_COPYSRC";
// Diagnostic: read the reference surface once through the CPU mapping before
// submitting the inter picture. Only the lock/unlock pair differs from the
// plain inter path, which separates surface preparation from contents.
constexpr const char *kRefTouchMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_P_REFTOUCH";
// Diagnostic: fill the reference surface with neutral 8-bit gray before the
// inter picture. The picture then has maximum residual over a surface written
// through the CPU mapping, which separates "contents close to the source avoid
// the stall" from "a CPU write to the reference avoids the stall".
constexpr const char *kRefGrayMarker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_P_REFGRAY";
// Diagnostic engine overrides: resolve always uses the context's own proxy,
// while picture submissions can be sent to another video context.
constexpr const char *kVc80Marker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_VC80";
constexpr const char *kVc96Marker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_VC96";

bool experimentalEnabled(const char *variable, const char *marker) {
    if (getenv(variable))
        return true;
    return gOps.debugMarker && gOps.debugMarker(marker) != 0;
}

// Reports the IOSurface geometry a picture buffer really has. The surface
// state in the command stream must repeat these values; a mismatch shows up as
// a sheared or tiled-looking picture.
void dumpSurfaceGeometry(const char *label, void *surface, uintptr_t luma,
    uint32_t rowBytes, uint32_t format) {
    if (!gLog || !experimentalEnabled("WEX_HEVC_DUMP", kDumpMarker))
        return;
    char *out = gHevcLine;
    append(out, "hevc ");
    append(out, label);
    append(out, " planes=");
    appendDec(out, gOps.surfacePlaneCount ? gOps.surfacePlaneCount(surface) : 0);
    append(out, " format=");
    appendHex(out, format);
    append(out, " rowBytes=");
    appendDec(out, rowBytes);
    emit(gHevcLine, out);
    for (uint32_t plane = 0; plane < 2; ++plane) {
        const uintptr_t base = gOps.surfacePlaneBase
            ? reinterpret_cast<uintptr_t>(gOps.surfacePlaneBase(surface, plane))
            : 0;
        out = gHevcLine;
        append(out, "hevc plane ");
        appendDec(out, plane);
        append(out, " size=");
        appendDec(out, gOps.surfacePlaneWidth
            ? gOps.surfacePlaneWidth(surface, plane) : 0);
        *out++ = 'x';
        appendDec(out, gOps.surfacePlaneHeight
            ? gOps.surfacePlaneHeight(surface, plane) : 0);
        append(out, " pitch=");
        appendDec(out, gOps.surfacePlanePitch
            ? gOps.surfacePlanePitch(surface, plane) : 0);
        append(out, " offset=");
        appendDec(out, base > luma ? base - luma : 0);
        emit(gHevcLine, out);
    }
}

// Diagnostic dump of the newest surface-manager entry and its client-shared
// page. It answers what the driver's 32-bit "address" field at +0x100 really
// is and whether a 64-bit address sits next to it.
void dumpSurfaceEntry(HevcEncodeContext *encoder, const char *label) {
    if (!encoder->surfaceManager ||
        !experimentalEnabled("WEX_HEVC_DUMP", kDumpMarker) || !gLog)
        return;
    void *entry = *reinterpret_cast<void **>(encoder->surfaceManager);
    if (!entry) {
        logMessage("dump: surface manager empty");
        return;
    }
    unsigned guard = 0;
    while (readPointer(entry, 0x60) && guard++ < 256)
        entry = readPointer(entry, 0x60);
    char *out = gHevcLine;
    append(out, "dump entry ");
    append(out, label);
    append(out, " id=");
    appendHex(out, read32(entry, 0));
    append(out, " flag=");
    appendHex(out, read32(entry, 4));
    append(out, " f14=");
    appendHex(out, read32(entry, 0x14));
    append(out, " res0=");
    appendHex(out, reinterpret_cast<uintptr_t>(readPointer(entry, 0x18)));
    append(out, " cs0=");
    appendHex(out, reinterpret_cast<uintptr_t>(readPointer(entry, 0x30)));
    append(out, " priv0=");
    appendHex(out, reinterpret_cast<uintptr_t>(readPointer(entry, 0x48)));
    emit(gHevcLine, out);

    const char *shared = static_cast<const char *>(readPointer(entry, 0x30));
    if (shared) {
        for (unsigned base = 0; base < 0x140; base += 32) {
            out = gHevcLine;
            append(out, "dump cs+");
            appendHex(out, base);
            for (unsigned i = 0; i < 8; ++i) {
                *out++ = ' ';
                appendHex(out, read32(shared, base + i * 4));
            }
            emit(gHevcLine, out);
        }
    }
    const char *priv = static_cast<const char *>(readPointer(entry, 0x48));
    if (priv) {
        out = gHevcLine;
        append(out, "dump bindinfo");
        for (unsigned i = 0; i < 12; ++i) {
            *out++ = ' ';
            appendHex(out, read32(priv, i * 4));
        }
        emit(gHevcLine, out);
    }
    void *resource = readPointer(entry, 0x18);
    if (resource && gOps.clientSharedPrivate) {
        const char *page = static_cast<const char *>(
            gOps.clientSharedPrivate(resource, 0));
        if (page) {
            for (unsigned base = 0; base < 0x80; base += 32) {
                out = gHevcLine;
                append(out, "dump priv+");
                appendHex(out, base);
                for (unsigned i = 0; i < 8; ++i) {
                    *out++ = ' ';
                    appendHex(out, read32(page, base + i * 4));
                }
                emit(gHevcLine, out);
            }
        }
    }
}

// The renderer's sideband buffer carries the bind records the kernel parses
// before executing the batch. Dump it and the data-buffer entries so the
// address fields can be compared with the surface manager values.
void dumpSideband(HevcEncodeContext *encoder) {
    if (!encoder->videoContext ||
        !experimentalEnabled("WEX_HEVC_DUMP", kDumpMarker) || !gLog)
        return;
    const char *vc = static_cast<const char *>(encoder->videoContext);
    const char *base = static_cast<const char *>(readPointer(vc, 200));
    const char *write = static_cast<const char *>(readPointer(vc, 168));
    if (!base) {
        logMessage("dump sideband missing");
        return;
    }
    const unsigned length = write > base ? static_cast<unsigned>(write - base) : 0;
    for (unsigned off = 0; off < length && off < 0x120; off += 32) {
        char *out = gHevcLine;
        append(out, "dump sb+");
        appendHex(out, off);
        for (unsigned i = 0; i < 8; ++i) {
            *out++ = ' ';
            appendHex(out, read32(base, off + i * 4));
        }
        emit(gHevcLine, out);
    }
    for (unsigned i = 0; i < 4; ++i) {
        const char *entry = vc + 88 + i * 64;
        if (!readPointer(entry, 0))
            break;
        char *out = gHevcLine;
        append(out, "dump db");
        appendDec(out, i);
        append(out, " base=");
        appendHex(out, reinterpret_cast<uintptr_t>(readPointer(entry, 0)));
        append(out, " addr=");
        appendHex(out, read32(entry, 48));
        append(out, " res=");
        appendHex(out, reinterpret_cast<uintptr_t>(readPointer(entry, 32)));
        emit(gHevcLine, out);
    }
}

}  // namespace

// Looks through the renderer's data buffers for media command headers emitted
// by the stock AVC path. The address dwords around HCP_PIPE_BUF_ADDR_STATE
// show whether this driver's commands carry resource handles or resolved
// graphics addresses. This stays outside the anonymous namespace: the
// forwarding thunks in va_shim_darwin.cpp call it through the header, and an
// internal definition leaves that reference unresolved in the dylib.
void hevcDumpScheduleResult(void *driver) {
    if (!driver || !experimentalEnabled("WEX_HEVC_DUMP", kDumpMarker) || !gLog)
        return;
    const uint32_t offsets[3] = {kVideoContextOffset, kVideoContextOffset + 8,
        kVideoContextOffset + 16};
    for (unsigned v = 0; v < 3; ++v) {
        void *vc = readPointer(driver, offsets[v]);
        if (!vc)
            continue;
        for (unsigned entryIndex = 0; entryIndex < 3; ++entryIndex) {
            const char *entry = static_cast<const char *>(vc) + 88 + entryIndex * 64;
            const char *base = static_cast<const char *>(readPointer(entry, 0));
            const char *cursor = static_cast<const char *>(readPointer(entry, 16));
            if (!base || !cursor || cursor <= base)
                break;
            uint32_t dwords = static_cast<uint32_t>(cursor - base) / 4;
            if (dwords > 8192)
                dwords = 8192;
            unsigned matches = 0;
            for (uint32_t i = 0; i + 1 < dwords && matches < 3; ++i) {
                const uint32_t word = read32(base, i * 4);
                if (word != 0x73820077 && word != 0x73810003 &&
                    word != 0x73800005)
                    continue;
                char *out = gHevcLine;
                append(out, "avc cmd vc");
                appendDec(out, v);
                append(out, " db");
                appendDec(out, entryIndex);
                append(out, " +");
                appendDec(out, i * 4);
                for (unsigned j = 0; j < 8 && i + j < dwords; ++j) {
                    *out++ = ' ';
                    appendHex(out, read32(base, (i + j) * 4));
                }
                emit(gHevcLine, out);
                ++matches;
            }
        }
    }
}

namespace {

// Allocates one driver-owned linear resource. The descriptor is the 3776-byte
// IOAccelResourceCreate argument the shipped driver fills in sub_9628: client
// VM address in both backing fields, size in the length fields, and the 0x80
// flag that marks the resource as client-memory backed. The resulting
// client-shared page publishes the resource handle at +0x100, which is the
// form media commands use and the kernel bind table resolves.
bool createLinearBuffer(HevcEncodeContext *encoder, const char *label,
    uint32_t size, uint32_t index, HevcBuffer &out) {
    if (!gOps.resourceCreate || !gOps.resourceClientShared || !encoder->driver)
        return false;
    void *shared = readPointer(encoder->driver, kDriverSharedObject);
    if (!shared)
        return false;

    const uint32_t bytes = (size + 0xFFFu) & ~0xFFFu;
    unsigned long base = 0;
    if (vm_allocate(mach_task_self_, &base, bytes, 1) != 0) {
        logMessage("linear vm_allocate failed");
        return false;
    }

    uint8_t descriptor[3776] {};
    write64(descriptor, 0, 0x80);
    write32(descriptor, 8, bytes);
    write32(descriptor, 10, 0x00010001);
    write64(descriptor, 24, bytes);
    write32(descriptor, 32, 0x01000101);
    write64(descriptor, 64, base);
    write64(descriptor, 72, base);
    write64(descriptor, 88, bytes);

    void *resource = gOps.resourceCreate(shared, descriptor, sizeof(descriptor));
    if (!resource) {
        vm_deallocate(mach_task_self_, base, bytes);
        logMessage("linear resource create failed");
        return false;
    }
    void *privateOut = nullptr;
    const char *client = static_cast<const char *>(
        gOps.resourceClientShared(resource, &privateOut));
    const uint32_t address = client ? read32(client, kClientSharedAddress) : 0;
    // The client-shared page publishes the renderer's resource handle, the
    // same form the surface manager returns for IOSurface planes. Media
    // commands name resources by that handle and the kernel's bind table
    // resolves it before the batch runs.
    if (address == 0) {
        if (gOps.resourceRelease)
            gOps.resourceRelease(resource);
        vm_deallocate(mach_task_self_, base, bytes);
        logMessage("linear resource has no handle");
        return false;
    }

    encoder->linear[index].resource = resource;
    encoder->linear[index].base = reinterpret_cast<void *>(base);
    encoder->linear[index].address = address;
    encoder->linear[index].size = size;
    out.address = address;
    out.size = size;
    out.valid = true;
    out.cpuAddress = reinterpret_cast<void *>(base);
    logValue(label, address);
    return true;
}

bool createScratchBuffer(HevcEncodeContext *encoder, const char *label,
    uint32_t size, uint32_t ownerIndex, HevcBuffer &out) {
    if (!gOps.createSurface || !gOps.registerSurface || !gOps.surfaceAddress) {
        logMessage("scratch surface allocation unavailable");
        return false;
    }
    const uint32_t rows = (size + kScratchPitch - 1) / kScratchPitch;
    void *owner = nullptr;
    void *surface = gOps.createSurface(kScratchPitch, rows, kScratchPitch,
        0x42475241, &owner);
    if (!surface) {
        logMessage("scratch surface create failed");
        return false;
    }
    const int32_t status = gOps.registerSurface(encoder->surfaceManager, surface);
    if (status != 0) {
        logValue("scratch register status", static_cast<uint32_t>(status));
        return false;
    }
    uint32_t address = 0;
    uint32_t value = 0;
    if (!gOps.surfaceAddress(surface, encoder->surfaceManager, 0, &address, &value) ||
        !address) {
        logMessage("scratch surface address unavailable");
        return false;
    }
    // Bind the scratch surface to the accelerator and mark it in use, as the
    // shipped encode path does for every surface a batch references.
    if (gOps.bindSurface)
        gOps.bindSurface(encoder->surfaceManager, surface);
    if (gOps.surfaceBindAccel && encoder->driver) {
        gOps.surfaceBindAccel(surface,
            read32(encoder->driver, kDriverAcceleratorId), 0);
    }
    encoder->scratchOwners[ownerIndex] = owner ? owner : surface;
    encoder->scratchSurfaces[ownerIndex] = surface;
    out.address = address;
    out.size = size;
    out.valid = true;
    out.cpuAddress = gOps.surfaceBase ? gOps.surfaceBase(surface) : nullptr;
    logValue(label, address);
    dumpSurfaceEntry(encoder, label);
    return true;
}

// The media engine reads internal buffers through their graphics addresses.
// Try the driver's linear-resource form first; the IOSurface form remains as
// a fallback for a driver build that does not expose IOAccelResourceCreate.
bool createBuffer(HevcEncodeContext *encoder, const char *label, uint32_t size,
    uint32_t index, HevcBuffer &out) {
    if (createLinearBuffer(encoder, label, size, index, out))
        return true;
    logMessage("linear buffer unavailable; using surface scratch");
    return createScratchBuffer(encoder, label, size, index, out);
}

void allocateScratch(HevcEncodeContext *encoder) {
    if (encoder->scratchTried)
        return;
    encoder->scratchTried = 1;
    const auto sizes = hevcBufferSizes(encoder->width, encoder->height);
    if (!sizes.valid) {
        logMessage("HEVC scratch size overflow");
        return;
    }
    createBuffer(encoder, "bitstream gpu address", kBitstreamSize,
        kScratchBitstream, encoder->scratch.bitstream);
    createBuffer(encoder, "pakbse gpu address",
        sizes.pakBseBytes > kPakBseSize ? sizes.pakBseBytes : kPakBseSize,
        kScratchPakBse, encoder->scratch.pakBse);
    createBuffer(encoder, "curecord gpu address",
        sizes.cuRecordBytes > kCuRecordSize ? sizes.cuRecordBytes : kCuRecordSize,
        kScratchCuRecord, encoder->scratch.cuRecord);
    createBuffer(encoder, "streamin gpu address",
        sizes.streamInBytes > kStreamInSize ? sizes.streamInBytes : kStreamInSize,
        kScratchStreamIn, encoder->scratch.streamIn);
    createBuffer(encoder, "rowstore gpu address", kRowStoreSize,
        kScratchRowStore, encoder->scratch.rowStore);
    createBuffer(encoder, "slicesize gpu address", kSliceSizeSize,
        kScratchSliceSize, encoder->scratch.encodedSliceSize);
    createBuffer(encoder, "statistics gpu address", kStatisticsSize,
        kScratchStatistics, encoder->scratch.statistics);
    createBuffer(encoder, "secondlevel gpu address", kSecondLevelSize,
        kScratchSecondLevel, encoder->scratch.secondLevel);
    createBuffer(encoder, "huc dmem gpu address", kDmemSize,
        kScratchDmem, encoder->scratch.dmem);
    createBuffer(encoder, "huc stats gpu address", kAggregatedStatsSize,
        kScratchAggregatedStats, encoder->scratch.aggregatedStats);
    createBuffer(encoder, "huc brc history gpu address", kBrcHistorySize,
        kScratchBrcHistory, encoder->scratch.brcHistory);
    createBuffer(encoder, "huc brc data gpu address", kBrcDataSize,
        kScratchBrcData, encoder->scratch.brcData);
    createBuffer(encoder, "huc tile record gpu address", kTileRecordSize,
        kScratchTileRecord, encoder->scratch.tileRecord);
    createBuffer(encoder, "scaled4x gpu address",
        kScaledPoolSlots * hevcScaledSurfaceBytes(
            hevcScaledSurface(encoder->width, encoder->height, false)),
        kScratchScaled4x, encoder->scratch.scaled4x);
    createBuffer(encoder, "scaled8x gpu address",
        kScaledPoolSlots * hevcScaledSurfaceBytes(
            hevcScaledSurface(encoder->width, encoder->height, true)),
        kScratchScaled8x, encoder->scratch.scaled8x);
    createBuffer(encoder, "hcp streamout gpu address", kBitstreamSize,
        kScratchHcpStreamout, encoder->scratch.hcpStreamout);
    createBuffer(encoder, "sse rowstore gpu address", kRowStoreSize,
        kScratchSseRowstore, encoder->scratch.sseRowstore);
    createBuffer(encoder, "hcp scratch gpu address", kHcpScratchRegionBytes * kHcpScratchRegions,
        kScratchHcp, encoder->scratch.hcpScratch);
}

// The plug-in leaves the pool and buffer ids zero in the picture payload, so
// fall back to the first populated driver pool. The create-buffer call before
// Schedule registers the bitstream buffer there.
bool resolveAnyPoolBuffer(HevcEncodeContext *encoder, HevcBuffer &result) {
    if (!encoder->driver)
        return false;
    const uint32_t offsets[2] = {kPool0Offset, kPool5Offset};
    for (unsigned i = 0; i < 2; ++i) {
        const char *pool = static_cast<char *>(encoder->driver) + offsets[i];
        if (!read32(pool, kPoolBufferId))
            continue;
        void *shared = readPointer(pool, kPoolEntryShared);
        if (!shared)
            continue;
        const uint32_t address = read32(shared, kClientSharedAddress);
        if (!address)
            continue;
        result.address = address;
        result.size = read32(pool, kPoolBufferSize);
        result.cpuAddress = readPointer(pool, 0x10);
        result.valid = true;
        return true;
    }
    return false;
}

void fillBuffers(HevcEncodeContext *encoder, const void *payload,
    HevcBuffers &buffers) {
    const uint32_t poolId = read32(payload, kPayloadPoolId);
    const uint32_t bufferId = read32(payload, kPayloadBufferId);
    if (!resolvePoolBuffer(encoder, poolId, bufferId, buffers.bitstream) &&
        !resolveAnyPoolBuffer(encoder, buffers.bitstream))
        buffers.bitstream = encoder->scratch.bitstream;
    buffers.pakBse = encoder->scratch.pakBse;
    buffers.cuRecord = encoder->scratch.cuRecord;
    buffers.streamIn = encoder->scratch.streamIn;
    buffers.rowStore = encoder->scratch.rowStore;
    buffers.encodedSliceSize = encoder->scratch.encodedSliceSize;
    buffers.statistics = encoder->scratch.statistics;
    buffers.secondLevel = encoder->scratch.secondLevel;
    buffers.dmem = encoder->scratch.dmem;
    buffers.aggregatedStats = encoder->scratch.aggregatedStats;
    buffers.brcHistory = encoder->scratch.brcHistory;
    buffers.brcData = encoder->scratch.brcData;
    buffers.tileRecord = encoder->scratch.tileRecord;
    buffers.scaled4x = encoder->scratch.scaled4x;
    buffers.scaled8x = encoder->scratch.scaled8x;
    buffers.hcpStreamout = encoder->scratch.hcpStreamout;
    buffers.sseRowstore = encoder->scratch.sseRowstore;
    buffers.hcpScratch = encoder->scratch.hcpScratch;
}

// Replaces the renderer's resource handles with the graphics addresses the
// kernel returned for the resolve token. The probe lists the resources in
// exactly this order.
// The resolve token lists resource handles; the submitted batch needs the
// graphics addresses a previous probe returned. Keep the handles aside before
// applyResolvedAddresses replaces the address fields.
void capturePictureHandles(HevcEncodeContext *encoder,
    const HevcPictureDesc &picture) {
    encoder->pictureHandle[0] = picture.source.address;
    encoder->pictureHandle[1] = picture.recon.address;
    encoder->pictureHandle[2] = picture.buffers.bitstream.address;
    encoder->pictureHandle[3] = picture.buffers.pakBse.address;
    encoder->pictureHandle[4] = picture.buffers.cuRecord.address;
    encoder->pictureHandle[5] = picture.buffers.streamIn.address;
    encoder->pictureHandle[6] = picture.buffers.rowStore.address;
    encoder->pictureHandle[7] = picture.buffers.encodedSliceSize.address;
    encoder->pictureHandle[8] = picture.buffers.statistics.address;
    encoder->pictureHandle[9] = picture.buffers.secondLevel.address;
    encoder->pictureHandle[10] = picture.buffers.dmem.address;
    encoder->pictureHandle[11] = picture.buffers.aggregatedStats.address;
    encoder->pictureHandle[12] = picture.buffers.brcHistory.address;
    encoder->pictureHandle[13] = picture.buffers.brcData.address;
    encoder->pictureHandle[14] = picture.buffers.tileRecord.address;
    encoder->pictureHandle[15] = picture.buffers.scaled4x.address;
    encoder->pictureHandle[16] = picture.buffers.scaled8x.address;
    encoder->pictureHandle[17] = picture.buffers.hcpStreamout.address;
    encoder->pictureHandle[18] = picture.buffers.sseRowstore.address;
    encoder->pictureHandle[19] = picture.source.uvAddress;
    encoder->pictureHandle[20] = picture.recon.uvAddress;
    encoder->pictureHandle[21] = picture.buffers.hcpScratch.address;
    // The reference slot carries the surface this picture predicts from. An
    // I picture has none, so the reconstruction handle stands in for it; the
    // resolved address is then already the reconstruction address.
    encoder->pictureHandle[kReferenceResourceIndex] =
        encoder->referenceHandle ? encoder->referenceHandle
                                 : encoder->pictureHandle[1];
}

// Recon surface cache. The plug-in hands back a previously encoded picture's
// recon surface as this picture's reference; the graphics address and POC
// were recorded when that picture completed. A surface reused for a newer
// picture is updated in place.
const HevcEncodeContext::ReconCacheEntry *findRecon(
    const HevcEncodeContext *encoder, uint32_t handle) {
    if (!handle)
        return nullptr;
    for (uint32_t i = 0; i < encoder->reconCacheCount; ++i) {
        const auto &entry = encoder->reconCache[i];
        if (entry.valid && entry.handle == handle)
            return &entry;
    }
    return nullptr;
}

void rememberRecon(HevcEncodeContext *encoder, uint32_t handle,
    uint32_t poc, uint32_t address, uint32_t scaledSlot) {
    if (!handle || !address)
        return;
    for (uint32_t i = 0; i < encoder->reconCacheCount; ++i) {
        auto &entry = encoder->reconCache[i];
        if (entry.valid && entry.handle == handle) {
            entry.poc = poc;
            entry.address = address;
            entry.scaledSlot = scaledSlot;
            return;
        }
    }
    if (encoder->reconCacheCount < 8) {
        auto &entry = encoder->reconCache[encoder->reconCacheCount++];
        entry = {handle, poc, address, scaledSlot, true};
        return;
    }
    auto &entry = encoder->reconCache[encoder->reconCacheNext++ % 8];
    entry = {handle, poc, address, scaledSlot, true};
}

void rememberPoc(HevcEncodeContext *encoder, uint32_t poc) {
    if (encoder->recentPocCount < 3) {
        encoder->recentPoc[encoder->recentPocCount++] = poc;
        return;
    }
    encoder->recentPoc[encoder->recentPocNext++ % 3] = poc;
}

// An IRAP picture starts a new coded video sequence: the decoder empties the
// decoded picture buffer, so the POCs remembered for the keep list and the
// surface-to-POC map from the previous sequence no longer describe anything.
// Carrying them over makes the next slice list pictures the DPB cannot contain.
void resetReferenceHistory(HevcEncodeContext *encoder) {
    encoder->recentPocCount = 0;
    encoder->recentPocNext = 0;
    encoder->reconCacheCount = 0;
    encoder->reconCacheNext = 0;
    memset(encoder->brcPictureData, 0, sizeof(encoder->brcPictureData));
}

// Follow Intel's six 1024-byte picture-data slots. A current picture must not
// overwrite the feedback of any picture retained in its RPS. Selection is
// read-only so a second BRC pass reuses the same slot, and a failed submission
// cannot become a reference. Rate/quality reinitialization retains completed
// picture data; an IRAP clears it together with the decoded-picture history.
bool prepareBrcPictureData(const HevcEncodeContext *encoder,
    const HevcPictureDesc &picture, HevcCore::Huc::BrcUpdateParams &update) {
    using namespace HevcCore::Huc;
    if (picture.rpsCount > 4 || update.referencesL0 > 1 || update.referencesL1 > 1)
        return false;
    uint32_t reference = kBrcPictureDataSlots;
    for (uint32_t i = 0; i < kBrcPictureDataSlots; ++i)
        if (encoder->brcPictureData[i].valid &&
            encoder->brcPictureData[i].poc == picture.refPoc)
            reference = i;
    if (picture.inter && reference == kBrcPictureDataSlots)
        return false;
    uint32_t selected = kBrcPictureDataSlots, oldest = 0;
    for (uint32_t i = 0; i < kBrcPictureDataSlots; ++i) {
        const auto &slot = encoder->brcPictureData[i];
        bool retained = picture.inter && i == reference;
        for (uint32_t r = 0; slot.valid && r < picture.rpsCount; ++r)
            retained = retained || slot.poc == picture.rpsPocs[r];
        if (retained) continue;
        if (!slot.valid) { selected = i; break; }
        if (selected == kBrcPictureDataSlots || slot.age > oldest) {
            selected = i;
            oldest = slot.age;
        }
    }
    if (selected == kBrcPictureDataSlots)
        return false;
    update.pictureDataOffset = uint16_t(selected * kBrcPictureDataBytes);
    if (picture.inter) {
        if (update.referencesL0)
            update.referenceDataOffsets[0] = uint16_t(reference * kBrcPictureDataBytes);
        if (update.referencesL1)
            update.referenceDataOffsets[update.referencesL0] =
                uint16_t(reference * kBrcPictureDataBytes);
    }
    return true;
}

bool rememberBrcPictureData(HevcEncodeContext *encoder, uint32_t poc,
    uint32_t offset) {
    using namespace HevcCore::Huc;
    if (offset % kBrcPictureDataBytes || offset >= kBrcPictureDataSlots * kBrcPictureDataBytes)
        return false;
    for (auto &slot : encoder->brcPictureData)
        if (slot.valid && slot.age != 0xffffffffu) ++slot.age;
    encoder->brcPictureData[offset / kBrcPictureDataBytes] = {poc, 0, true};
    return true;
}

// Fills the slice's keep list: the recently completed pictures plus the
// reference this picture predicts from. The header marks only refPoc as used.
void fillReferenceSet(const HevcEncodeContext *encoder,
    HevcPictureDesc &picture) {
    auto add = [&](uint32_t poc) {
        if (poc == picture.poc || picture.rpsCount >= 4)
            return;
        for (uint32_t i = 0; i < picture.rpsCount; ++i)
            if (picture.rpsPocs[i] == poc)
                return;
        picture.rpsPocs[picture.rpsCount++] = poc;
    };
    for (uint32_t i = 0; i < encoder->recentPocCount; ++i)
        add(encoder->recentPoc[i]);
    if (picture.inter)
        add(picture.refPoc);
}

void applyResolvedAddresses(HevcEncodeContext *encoder, HevcPictureDesc &picture) {
    auto address = [encoder](uint32_t index) -> uint32_t {
        return encoder->resolvedAddress[index] & 0xFFFFFFFFu;
    };
    auto size = [encoder](uint32_t index, uint32_t fallback) -> uint32_t {
        return encoder->resolvedSize[index] ? encoder->resolvedSize[index]
                                            : fallback;
    };
    picture.source.address = address(0);
    picture.recon.address = address(1);
    // For an IOSurface the resolve record carries the GPU pitch, whereas
    // IOSurfaceGetBytesPerRowOfPlane describes its CPU mapping. They differ
    // for widths such as 3808: the tiled allocation uses a 3840-byte pitch.
    // Use that pitch before deriving chroma rows from the graphics addresses.
    picture.source.pitch = size(0, picture.source.pitch);
    picture.recon.pitch = size(1, picture.recon.pitch);
    picture.reference.pitch = size(kReferenceResourceIndex, picture.recon.pitch);
    // An inter picture already carries the reference address from the recon
    // surface cache; the current recon address is not its reference.
    if (!picture.inter)
        picture.reference.address = address(1);
    picture.buffers.bitstream.address = address(2);
    picture.buffers.bitstream.size = size(2, picture.buffers.bitstream.size);
    picture.buffers.pakBse.address = address(3);
    picture.buffers.cuRecord.address = address(4);
    picture.buffers.streamIn.address = address(5);
    picture.buffers.rowStore.address = address(6);
    picture.buffers.encodedSliceSize.address = address(7);
    picture.buffers.statistics.address = address(8);
    picture.buffers.secondLevel.address = address(9);
    picture.buffers.dmem.address = address(10);
    picture.buffers.aggregatedStats.address = address(11);
    picture.buffers.brcHistory.address = address(12);
    picture.buffers.brcData.address = address(13);
    picture.buffers.tileRecord.address = address(14);
    picture.buffers.scaled4x.address = address(15);
    picture.buffers.scaled8x.address = address(16);
    picture.buffers.hcpStreamout.address = address(17);
    picture.buffers.sseRowstore.address = address(18);
    picture.source.uvAddress = address(19);
    picture.recon.uvAddress = address(20);
    // The HCP and VDEnc surface states address chroma with a row offset from
    // the luma base, so the offset has to match the graphics allocation. The
    // CPU mapping of an IOSurface pads the planes differently (a 368-line
    // picture maps chroma at row 368 while the GPU allocation puts it at row
    // 384), and the hardware then codes 16 rows of padding as black chroma.
    const uint32_t sourceUvRow = HevcCore::uvRowOffsetFromAddresses(
        picture.source.address, picture.source.uvAddress,
        picture.source.pitch);
    if (sourceUvRow)
        picture.source.uvRowOffset = sourceUvRow;
    const uint32_t reconUvRow = HevcCore::uvRowOffsetFromAddresses(
        picture.recon.address, picture.recon.uvAddress, picture.recon.pitch);
    if (reconUvRow)
        picture.recon.uvRowOffset = reconUvRow;
    if (gLog) {
        char *out = gHevcLine;
        append(out, "hevc chroma row offset source=");
        appendDec(out, picture.source.uvRowOffset);
        append(out, " recon=");
        appendDec(out, picture.recon.uvRowOffset);
        emit(gHevcLine, out);
    }
    picture.buffers.hcpScratch.address = address(21);
    // An inter picture reads the reference that was resolved for this
    // submission. The recon cache address from the picture that produced the
    // reference is the fallback for a picture whose reference was not in the
    // token (an unchanged source handle skips the resolve probe).
    if (picture.inter && encoder->referenceHandle &&
        encoder->resolvedReferenceHandle == encoder->referenceHandle &&
        address(kReferenceResourceIndex) != 0)
        picture.reference.address = address(kReferenceResourceIndex);
    if (gLog) {
        char *out = gHevcLine;
        append(out, "hevc resolved addresses source=");
        appendHex(out, picture.source.address);
        append(out, " recon=");
        appendHex(out, picture.recon.address);
        append(out, " bitstream=");
        appendHex(out, picture.buffers.bitstream.address);
        append(out, " dmem=");
        appendHex(out, picture.buffers.dmem.address);
        if (picture.inter) {
            append(out, " reference=");
            appendHex(out, picture.reference.address);
        }
        emit(gHevcLine, out);
    }
}

// Writes a scratch buffer through its CPU mapping. The HuC DMEM block and the
// second-level batch are the only buffers the shim has to fill itself.
void *scratchBase(HevcEncodeContext *encoder, uint32_t index) {
    if (index >= kScratchCount)
        return nullptr;
    if (encoder->linear[index].base)
        return encoder->linear[index].base;
    void *surface = encoder->scratchSurfaces[index];
    if (!surface || !gOps.surfaceBase)
        return nullptr;
    return gOps.surfaceBase(surface);
}

// Prints the first words of a scratch buffer after a submission. VDEnc writes
// statistics and row-store data even when the bitstream stays empty, so this
// separates "engine did not run" from "bitstream pointer or slice-size layout
// is wrong".
void dumpBufferWords(const char *label, const void *base, uint32_t words,
    uint32_t wordOffset = 0) {
    if (!base || !gLog)
        return;
    char *out = gHevcLine;
    append(out, "post ");
    append(out, label);
    if (words > 16)
        words = 16;
    for (uint32_t i = 0; i < words; ++i) {
        *out++ = ' ';
        appendHex(out, read32(base, (wordOffset + i) * 4));
    }
    emit(gHevcLine, out);
}

void dumpScratchWords(HevcEncodeContext *encoder, const char *label,
    uint32_t index, uint32_t words) {
    dumpBufferWords(label, scratchBase(encoder, index), words);
}

// Reports how much of a stream-out buffer the engine filled. The VDEnc writes
// one CU record per coding unit and the PAK writes the bitstream; a buffer that
// stays untouched after a failed submission shows which engine stopped.
void dumpEngineBuffer(HevcEncodeContext *encoder, const char *label,
    uint32_t index, uint64_t limit) {
    const auto *base = static_cast<const uint32_t *>(scratchBase(encoder, index));
    if (!base || !gLog)
        return;
    const uint32_t words = static_cast<uint32_t>(
        limit < 0x40000u ? limit : 0x40000u);
    uint32_t sum = 0;
    uint32_t nonzero = 0;
    uint32_t last = 0;
    // With POISON the untouched value is 0xCDCDCDCD, so a dword that differs
    // from it is memory the engine wrote. Both counts are reported: nonzero
    // shows the content, written/lastWritten show the engine's extent.
    uint32_t written = 0;
    uint32_t lastWritten = 0;
    for (uint32_t i = 0; i < words; ++i) {
        const uint32_t value = read32(base, i * 4);
        sum = (sum * 33u) ^ value;
        nonzero += value != 0 ? 1 : 0;
        last = value != 0 ? i : last;
        written += value != 0xCDCDCDCDu ? 1 : 0;
        lastWritten = value != 0xCDCDCDCDu ? i : lastWritten;
    }
    char *out = gHevcLine;
    append(out, "engine ");
    append(out, label);
    append(out, " words=");
    appendDec(out, words);
    append(out, " nonzero=");
    appendDec(out, nonzero);
    append(out, " last=");
    appendDec(out, last);
    append(out, " written=");
    appendDec(out, written);
    append(out, " lastWritten=");
    appendDec(out, lastWritten);
    append(out, " hash=");
    appendHex(out, sum);
    for (uint32_t i = 0; i < 8; ++i) {
        *out++ = ' ';
        appendHex(out, read32(base, i * 4));
    }
    emit(gHevcLine, out);
}

// Reports how far a stream-out buffer the shim does not own (the plug-in's
// bitstream pool buffer) was written: the PAK fills it from the start, so the
// last non-zero byte is the point the PAK stopped.
void dumpWrittenTail(const char *label, const void *base, uint32_t size,
    uint32_t limit) {
    const auto *bytes = static_cast<const uint8_t *>(base);
    if (!bytes || !gLog)
        return;
    const uint32_t count = size < limit ? size : limit;
    uint32_t last = 0;
    for (uint32_t i = 0; i < count; ++i)
        last = bytes[i] ? i : last;
    char *out = gHevcLine;
    append(out, "tail ");
    append(out, label);
    append(out, " last=");
    appendDec(out, last);
    append(out, " of=");
    appendDec(out, count);
    const uint32_t begin = last > 12 ? last - 12 : 0;
    for (uint32_t i = begin; i <= last && i < count; ++i) {
        *out++ = ' ';
        appendHex(out, bytes[i]);
    }
    emit(gHevcLine, out);
}

// Prints the extent and the first and last object rows of the VDEnc PAK
// object stream. The buffer keeps everything the engine wrote even when the
// submission never completed, so a stalled hand-over still shows where the
// VDEnc stopped and what the first objects look like. POISON fills the buffer
// with 0xCD, so any other byte marks memory the engine touched.
void dumpPakObjectStream(const void *memory, uint32_t bytes);

// Dumps the PAK object block of the picture currently being submitted. The
// plug-in can supply its own pool buffer, so the picture's resolved allocation
// is the one the VDEnc wrote; the shim scratch is only the fallback.
void dumpPicturePakObjectStream(const HevcPictureDesc &picture) {
    if (picture.buffers.pakBse.cpuAddress && picture.buffers.pakBse.size)
        dumpPakObjectStream(picture.buffers.pakBse.cpuAddress,
            picture.buffers.pakBse.size);
}

void dumpPakObjectStream(const void *memory, uint32_t bytes) {
    if (!gLog || !memory)
        return;
    const auto *base = static_cast<const uint8_t *>(memory);
    uint32_t lastWritten = 0;
    for (uint32_t i = 0; i < bytes; ++i)
        if (base[i] != 0xCD)
            lastWritten = i;
    logValue("pakbse last written", lastWritten);
    // 64-byte rows, each one HCP PAK-object command. Print the first rows with
    // the fields that decide how far the PAK walks (CU count and the last-CTB
    // flags), then the rows around the furthest written byte.
    const uint32_t rows = lastWritten / 64 + 1;
    const uint32_t headRows = rows < 24 ? rows : 24;
    for (uint32_t row = 0; row < headRows; ++row) {
        const uint32_t dw0 = read32(base, row * 64);
        const uint32_t dw1 = read32(base, row * 64 + 4);
        char *out = gHevcLine;
        append(out, "obj ");
        appendDec(out, row);
        append(out, " dw0=");
        appendHex(out, dw0);
        append(out, " dw1=");
        appendHex(out, dw1);
        append(out, " cu=");
        appendDec(out, (dw1 >> 20) & 0x3F);
        append(out, " lastctb=");
        appendDec(out, (dw1 >> 31) & 1);
        append(out, " lasttile=");
        appendDec(out, (dw1 >> 30) & 1);
        for (uint32_t i = 2; i < 6; ++i) {
            *out++ = ' ';
            appendHex(out, read32(base, row * 64 + i * 4));
        }
        emit(gHevcLine, out);
    }
    const uint32_t headSkip = headRows * 64;
    const uint32_t tailRow = lastWritten > headSkip + 512
        ? (lastWritten - 512) / 64 : 0;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const uint32_t begin = pass == 0 ? 0 : tailRow;
        const uint32_t end = pass == 0 ? 8 : tailRow + 8;
        for (uint32_t row = begin; row < end; ++row) {
            if (row < headRows)
                continue;
            char *out = gHevcLine;
            append(out, "obj ");
            appendDec(out, row * 64);
            for (uint32_t i = 0; i < 16; ++i) {
                *out++ = ' ';
                appendHex(out, read32(base, row * 64 + i * 4));
            }
            emit(gHevcLine, out);
        }
    }
    if (experimentalEnabled("WEX_HEVC_OBJRAW", kObjRawMarker)) {
        // Flat rows for offline analysis. The stream is a sequence of
        // (register offset, value) pairs, so a row boundary can split a pair.
        // The raw dump has to reach the byte where the VDEnc stopped so the
        // half-written CU record can be identified. 32 KiB covers the first
        // two LCU objects of every clip used here.
        constexpr uint32_t kRawDumpBytes = 32u * 1024u;
        const uint32_t rawBytes = lastWritten + 64 < kRawDumpBytes
            ? lastWritten + 64 : kRawDumpBytes;
        for (uint32_t row = 0; row < rawBytes; row += 64) {
            char *out = gHevcLine;
            append(out, "raw ");
            appendDec(out, row);
            for (uint32_t i = 0; i < 16; ++i) {
                *out++ = ' ';
                appendHex(out, read32(base, row + i * 4));
            }
            emit(gHevcLine, out);
        }
    }
}

// Prints how far an engine wrote into a poisoned scratch buffer. POISON fills
// the buffer with 0xCD, so the last byte that differs bounds the engine's
// progress; this works after a failed submission as well.
void reportBufferExtentAt(const char *label, const void *memory,
    uint32_t bytes) {
    if (!gLog || !memory)
        return;
    const auto *base = static_cast<const uint8_t *>(memory);
    uint32_t lastWritten = 0;
    for (uint32_t i = 0; i < bytes; ++i)
        if (base[i] != 0xCD)
            lastWritten = i;
    char *out = gHevcLine;
    append(out, "extent ");
    append(out, label);
    *out++ = ' ';
    appendDec(out, lastWritten);
    emit(gHevcLine, out);
}

bool checkPictureCompletion(HevcEncodeContext *encoder, int32_t waitStatus,
    const void *completion, uint32_t expected) {
    if (waitStatus == StatusSuccess && completion && expected &&
        read32(completion, 0) == expected)
        return true;
    encoder->submissionFailed = true;
    logMessage("picture did not complete; context submissions stopped");
    return false;
}

bool publishPictureResult(HevcEncodeContext *encoder, void *payload,
    void *refCon, uint32_t bytes, uint32_t capacity, uint32_t fence) {
    if (!payload || !bytes || bytes > capacity)
        return false;
    // Native HEVC Schedule returns a byte range, not an asynchronous status
    // code, at these two 64-bit payload fields.
    write64(payload, 928, 0);
    write64(payload, 936, bytes);
    if (refCon) {
        auto *record = encoder->completionRecords[encoder->nextCompletionRecord++ % 32];
        record[0] = 0;
        record[1] = fence;
        *static_cast<void **>(refCon) = record;
    }
    return true;
}

// The sandbox permits the existing trace channel but cannot create a capture
// file. A bounded first-picture dump lets the encoded stream be decoded by a
// separate verifier. Only completed pictures are eligible.
void dumpEncodedPicture(const HevcPictureDesc &picture) {
    if (!gLog || !picture.buffers.statistics.cpuAddress ||
        !picture.buffers.bitstream.cpuAddress)
        return;
    const uint32_t bytes = read32(picture.buffers.statistics.cpuAddress,
        picture.buffers.statistics.size - 64 + 8);
    if (!bytes || bytes > picture.buffers.bitstream.size || bytes > 65536)
        return;
    logValue("bitstream bytes", bytes);
    const auto *data = static_cast<const uint8_t *>(picture.buffers.bitstream.cpuAddress);
    constexpr char hex[] = "0123456789abcdef";
    for (uint32_t offset = 0; offset < bytes; offset += 32) {
        char *out = gHevcLine;
        append(out, "hevc data ");
        appendDec(out, offset);
        *out++ = ' ';
        for (uint32_t i = offset; i < bytes && i < offset + 32; ++i) {
            *out++ = hex[data[i] >> 4];
            *out++ = hex[data[i] & 15];
        }
        emit(gHevcLine, out);
    }
}

// Reads luma samples through the IOSurface's CPU mapping. Comparing the
// source against the reconstruction separates "the engine read the wrong
// pixels" from "the bitstream does not describe what the engine reconstructed".
void dumpSurfaceSamples(const char *label, void *surface) {
    if (!gLog || !surface || !gOps.surfacePlaneBase || !gOps.surfacePlanePitch)
        return;
    uint32_t lockSeed = 0;
    const bool locked = gOps.surfaceLock &&
        gOps.surfaceLock(surface, 1 /* kIOSurfaceLockReadOnly */, &lockSeed) == 0;
    const auto *base = static_cast<const uint8_t *>(
        gOps.surfacePlaneBase(surface, 0));
    const uint32_t pitch = static_cast<uint32_t>(
        gOps.surfacePlanePitch(surface, 0));
    const uint32_t height = static_cast<uint32_t>(gOps.surfacePlaneHeight
        ? gOps.surfacePlaneHeight(surface, 0) : 0);
    const uint32_t width = static_cast<uint32_t>(gOps.surfacePlaneWidth
        ? gOps.surfacePlaneWidth(surface, 0) : 0);
    if (!base || !pitch || !height || !width)
        return;
    const uint32_t rows[4] = {0, 90, 540, height - 1};
    const uint32_t cols[5] = {0, 480, 960, 1440, width - 1};
    char *out = gHevcLine;
    append(out, "post ");
    append(out, label);
    append(out, " luma");
    for (uint32_t r = 0; r < 4; ++r) {
        for (uint32_t c = 0; c < 5; ++c) {
            *out++ = ' ';
            appendDec(out, base[static_cast<size_t>(rows[r]) * pitch + cols[c]]);
        }
    }
    emit(gHevcLine, out);
    if (locked && gOps.surfaceUnlock)
        gOps.surfaceUnlock(surface, 1, &lockSeed);
}

// FNV-1a over every fourth luma sample: a cheap way to tell a written surface
// from one that still holds allocation garbage.
uint32_t lumaChecksum(void *surface) {
    uint32_t hash = 2166136261u;
    if (!surface || !gOps.surfacePlaneBase || !gOps.surfacePlanePitch)
        return 0;
    uint32_t lockSeed = 0;
    const bool locked = gOps.surfaceLock &&
        gOps.surfaceLock(surface, 1 /* read only */, &lockSeed) == 0;
    const auto *base = static_cast<const uint8_t *>(
        gOps.surfacePlaneBase(surface, 0));
    const uint32_t pitch = static_cast<uint32_t>(
        gOps.surfacePlanePitch(surface, 0));
    const uint32_t height = static_cast<uint32_t>(gOps.surfacePlaneHeight
        ? gOps.surfacePlaneHeight(surface, 0) : 0);
    const uint32_t width = static_cast<uint32_t>(gOps.surfacePlaneWidth
        ? gOps.surfacePlaneWidth(surface, 0) : 0);
    if (base && pitch && height && width) {
        for (uint32_t y = 0; y < height; y += 4) {
            const uint8_t *row = base + static_cast<size_t>(y) * pitch;
            for (uint32_t x = 0; x < width; x += 4)
                hash = (hash ^ row[x]) * 16777619u;
        }
    }
    if (locked && gOps.surfaceUnlock)
        gOps.surfaceUnlock(surface, 1, &lockSeed);
    return hash;
}

// Copies luma and chroma of one surface into another through the CPU mapping.
// Both surfaces belong to the same picture geometry, so their plane pitches
// agree; the smaller of the two row lengths bounds the copy regardless.
bool copySurfacePlanes(void *src, void *dst) {
    if (!src || !dst || !gOps.surfacePlaneBase || !gOps.surfacePlanePitch)
        return false;
    bool copied = false;
    for (uint32_t plane = 0; plane < 2; ++plane) {
        uint32_t srcSeed = 0;
        uint32_t dstSeed = 0;
        const bool srcLocked = gOps.surfaceLock &&
            gOps.surfaceLock(src, 1, &srcSeed) == 0;
        const bool dstLocked = gOps.surfaceLock &&
            gOps.surfaceLock(dst, 0, &dstSeed) == 0;
        auto *dstBase = static_cast<uint8_t *>(gOps.surfacePlaneBase(dst, plane));
        const auto *srcBase = static_cast<const uint8_t *>(
            gOps.surfacePlaneBase(src, plane));
        const uint32_t dstPitch = static_cast<uint32_t>(
            gOps.surfacePlanePitch(dst, plane));
        const uint32_t srcPitch = static_cast<uint32_t>(
            gOps.surfacePlanePitch(src, plane));
        const uint32_t height = static_cast<uint32_t>(gOps.surfacePlaneHeight
            ? gOps.surfacePlaneHeight(dst, plane) : 0);
        const uint32_t width = static_cast<uint32_t>(gOps.surfacePlaneWidth
            ? gOps.surfacePlaneWidth(dst, plane) : 0);
        if (dstBase && srcBase && dstPitch && srcPitch && height && width) {
            const uint32_t bytes = width < srcPitch ? width : srcPitch;
            for (uint32_t y = 0; y < height; ++y)
                memcpy(dstBase + static_cast<size_t>(y) * dstPitch,
                    srcBase + static_cast<size_t>(y) * srcPitch, bytes);
            copied = true;
        }
        if (dstLocked && gOps.surfaceUnlock)
            gOps.surfaceUnlock(dst, 0, &dstSeed);
        if (srcLocked && gOps.surfaceUnlock)
            gOps.surfaceUnlock(src, 1, &srcSeed);
    }
    return copied;
}

// Fills luma and chroma of one surface with an 8-bit constant through the CPU
// mapping.
bool fillSurfacePlanes(void *surface, uint8_t value) {
    if (!surface || !gOps.surfacePlaneBase || !gOps.surfacePlanePitch)
        return false;
    bool filled = false;
    for (uint32_t plane = 0; plane < 2; ++plane) {
        uint32_t seed = 0;
        const bool locked = gOps.surfaceLock &&
            gOps.surfaceLock(surface, 0, &seed) == 0;
        auto *base = static_cast<uint8_t *>(
            gOps.surfacePlaneBase(surface, plane));
        const uint32_t pitch = static_cast<uint32_t>(
            gOps.surfacePlanePitch(surface, plane));
        const uint32_t height = static_cast<uint32_t>(gOps.surfacePlaneHeight
            ? gOps.surfacePlaneHeight(surface, plane) : 0);
        const uint32_t width = static_cast<uint32_t>(gOps.surfacePlaneWidth
            ? gOps.surfacePlaneWidth(surface, plane) : 0);
        if (base && pitch && height && width) {
            for (uint32_t y = 0; y < height; ++y)
                memset(base + static_cast<size_t>(y) * pitch, value, width);
            filled = true;
        }
        if (locked && gOps.surfaceUnlock)
            gOps.surfaceUnlock(surface, 0, &seed);
    }
    return filled;
}

bool writeScratch(HevcEncodeContext *encoder, uint32_t index, const void *data,
    uint32_t bytes) {
    auto *base = static_cast<char *>(scratchBase(encoder, index));
    if (!base)
        return false;
    const auto *source = static_cast<const char *>(data);
    for (uint32_t i = 0; i < bytes; ++i)
        base[i] = source[i];
    return true;
}

// Diagnostic: fills a buffer the engine should overwrite with a fixed byte.
// The post-submit dump then distinguishes "engine wrote nothing" from stale
// contents left by a previous owner of the same pages.
void poisonScratch(HevcEncodeContext *encoder, uint32_t index, uint32_t bytes) {
    auto *base = static_cast<char *>(scratchBase(encoder, index));
    if (!base)
        return;
    if (index < kScratchCount && encoder->linear[index].base &&
        encoder->linear[index].size && bytes > encoder->linear[index].size)
        bytes = encoder->linear[index].size;
    for (uint32_t i = 0; i < bytes; ++i)
        base[i] = static_cast<char>(0xCD);
}

// Builds the VPS/SPS/PPS units and the IDR slice header for one picture. The
// parameter sets are cached by the complete coding configuration, including
// transform limits. They already contain emulation prevention; the per-picture
// slice header is raw RBSP and the PAK escapes it while inserting it.
bool preparePictureHeader(HevcEncodeContext *encoder, HevcPictureDesc &picture) {
    // The plug-in schedules I, P and B pictures in decode order. Non-IDR
    // pictures carry the plug-in's POC; a coded P picture also carries the
    // single reference POC its slice header signals.
    const bool idr = picture.nalType >= 16 && picture.nalType <= 23;

    HevcCore::Config config;
    config.width = picture.width;
    config.height = picture.height;
    config.visibleWidth = picture.visibleWidth;
    config.visibleHeight = picture.visibleHeight;
    config.qp = picture.qpY;
    config.fps = 30;
    config.level = 153;
    // The sequence's bit depth follows the surface the plug-in delivers: a
    // P010 source makes this a Main10 sequence, and the SPS, the PPS, the HCP
    // picture state and every surface state then describe 10-bit samples.
    config.main10 = pictureIsTenBit(picture);
    config.cuQpDelta = picture.cuQpDeltaEnabled;
    config.diffCuQpDeltaDepth = picture.diffCuQpDeltaDepth;
    config.minCodingBlockLog2 = picture.minCodingBlockLog2;
    config.maxTransformLog2 = picture.maxTransformLog2;
    config.transformDepth = picture.transformDepth;

    if (!encoder->parameterSets || !(encoder->headerConfig == config)) {
        const HevcCore::ParameterSets sets =
            HevcCore::buildParameterSets(config, true);
        const uint32_t bytes = static_cast<uint32_t>(
            sets.vps.size() + sets.sps.size() + sets.pps.size());
        void *buffer = calloc(1, bytes ? bytes : 1);
        if (!buffer) {
            logMessage("hevc parameter set allocation failed");
            return false;
        }
        auto *out = static_cast<uint8_t *>(buffer);
        const std::vector<uint8_t> *parts[3] = {&sets.vps, &sets.sps, &sets.pps};
        for (const auto *part : parts) {
            memcpy(out, part->data(), part->size());
            out += part->size();
        }
        free(encoder->parameterSets);
        encoder->parameterSets = buffer;
        encoder->parameterSetBytes = bytes;
        encoder->headerConfig = config;
        logValue("parameter set bytes", bytes);
    }

    HevcCore::SliceConfig slice;
    slice.qp = picture.qpY;
    slice.idr = idr;
    slice.poc = picture.poc;
    slice.inter = picture.inter;
    // Inter pictures are coded as low-delay B slices: the HCP states that a
    // VDEnc HEVC slice is B or I only, and the VDEnc picture type, the slice
    // header and the two reference lists all have to describe the same slice.
    slice.lowDelayB = picture.inter && picture.lowDelay;
    slice.refPoc = picture.refPoc;
    slice.rpsCount = picture.rpsCount;
    for (uint32_t i = 0; i < picture.rpsCount && i < 4; ++i)
        slice.rpsPocs[i] = picture.rpsPocs[i];
    // The slice header matches the hardware slice state: no SAO and no
    // deblocking filter until the PAK line buffers are provided.
    slice.saoLuma = false;
    slice.saoChroma = false;
    slice.deblockingFilterDisable = true;
    const std::vector<uint8_t> raw =
        HevcCore::buildSliceHeader(config, slice, false);
    const std::vector<uint8_t> emulated =
        HevcCore::buildSliceHeader(config, slice, true);
    if (raw.empty() || emulated.empty()) {
        logMessage("hevc slice header build failed");
        return false;
    }
    if (raw.size() > encoder->sliceHeaderCapacity) {
        void *buffer = calloc(1, raw.size());
        if (!buffer) {
            logMessage("hevc slice header allocation failed");
            return false;
        }
        free(encoder->sliceHeader);
        encoder->sliceHeader = buffer;
        encoder->sliceHeaderCapacity = static_cast<uint32_t>(raw.size());
    }
    memcpy(encoder->sliceHeader, raw.data(), raw.size());
    encoder->sliceHeaderBytes = static_cast<uint32_t>(raw.size());
    encoder->sliceHeaderEmulatedBits =
        static_cast<uint32_t>(emulated.size()) * 8;

    picture.parameterSets = static_cast<const uint8_t *>(encoder->parameterSets);
    picture.parameterSetBytes = encoder->parameterSetBytes;
    picture.sliceHeader = static_cast<const uint8_t *>(encoder->sliceHeader);
    picture.sliceHeaderBits = encoder->sliceHeaderBytes * 8;
    picture.sliceHeaderEmulatedBits = encoder->sliceHeaderEmulatedBits;
    if (gLog) {
        char *out = gHevcLine;
        append(out, "hevc slice header bytes=");
        appendDec(out, encoder->sliceHeaderBytes);
        append(out, " emulated=");
        appendDec(out, encoder->sliceHeaderEmulatedBits / 8);
        append(out, " idr=");
        appendDec(out, idr ? 1 : 0);
        append(out, " poc=");
        appendDec(out, picture.poc);
        emit(gHevcLine, out);
    }
    return true;
}

// Writes VDENC_CMD1, HCP_PIC_STATE and VDENC_CMD2 into the second-level batch
// buffer for the non-HuC path, mirroring media-driver's picture-level batch
// split. The ring batch then only starts this buffer.
bool prepareSecondLevel(HevcEncodeContext *encoder, HevcPictureDesc &picture) {
    if (!gOps.surfaceBase || !picture.buffers.secondLevel.valid)
        return false;

    const uint32_t secondDwords = hevcSecondLevelCommandDwords(picture);
    if (secondDwords == 0 || secondDwords * 4 > kSecondLevelSize) {
        logMessage("second level does not fit");
        return false;
    }
    auto *second = static_cast<uint32_t *>(calloc(secondDwords, 4));
    if (!second)
        return false;
    const uint32_t written = buildHevcSecondLevelCommands(picture, second,
        secondDwords);
    const bool stored = written != 0 &&
        writeScratch(encoder, kScratchSecondLevel, second, written * 4);
    if (stored && gLog && experimentalEnabled("WEX_HEVC_DUMP", kDumpMarker)) {
        // The picture-level commands live in their own buffer; dump them so a
        // comparison between two runs covers this buffer too.
        for (uint32_t base = 0; base < written; base += 8) {
            char *out = gHevcLine;
            append(out, "second dw");
            appendDec(out, base);
            for (uint32_t i = 0; i < 8 && base + i < written; ++i) {
                *out++ = ' ';
                appendHex(out, second[base + i]);
            }
            emit(gHevcLine, out);
        }
    }
    free(second);
    if (!stored) {
        logMessage("second level write failed");
        return false;
    }
    picture.secondLevelBatch = true;
    return true;
}

// Fills the HuC PAK-integration description: the second-level batch in its own
// buffer, the 192-byte DMEM block and the 16 virtual-address regions. Region
// numbers follow media-driver's SetRegionsHuCPakIntegrate.
bool prepareHuc(HevcEncodeContext *encoder, HevcPictureDesc &picture) {
    if (!gOps.surfaceBase || !picture.buffers.secondLevel.valid ||
        !picture.buffers.dmem.valid)
        return false;

    const uint32_t secondDwords = hevcSecondLevelCommandDwords(picture);
    if (secondDwords == 0 || secondDwords * 4 > kSecondLevelSize) {
        logMessage("huc second level does not fit");
        return false;
    }
    auto *second = static_cast<uint32_t *>(calloc(secondDwords, 4));
    if (!second)
        return false;
    const uint32_t written = buildHevcSecondLevelCommands(picture, second,
        secondDwords);
    const bool stored = written != 0 &&
        writeScratch(encoder, kScratchSecondLevel, second, written * 4);
    free(second);
    if (!stored) {
        logMessage("huc second level write failed");
        return false;
    }

    HevcCore::Huc::PakStitchParams dmemParams;
    dmemParams.pictureWidth = static_cast<uint16_t>(picture.width);
    dmemParams.pictureHeight = static_cast<uint16_t>(picture.height);
    dmemParams.numPipes = 1;
    dmemParams.numTilesPerPipe = 1;
    dmemParams.minCuSize = 3;
    dmemParams.bitDepthLuma = 8;
    dmemParams.bitDepthChroma = 8;
    dmemParams.chromaFormatIdc = 1;
    dmemParams.codec = HevcCore::Huc::Codec::HevcVdEnc;
    dmemParams.stitchEnable = false;
    dmemParams.sliceHeaderSizeInBits = picture.sliceHeaderEmulatedBits;
    const auto dmem = HevcCore::Huc::pakStitchDmem(dmemParams);
    if (!writeScratch(encoder, kScratchDmem, &dmem, sizeof(dmem))) {
        logMessage("huc dmem write failed");
        return false;
    }

    picture.huc.enabled = true;
    picture.huc.kernelDescriptor = HevcCore::Huc::kPakIntegrationKernel;
    picture.huc.dmemAddress = picture.buffers.dmem.address;
    picture.huc.dmemLength = sizeof(dmem);
    picture.huc.dmemOffset = HevcCore::Huc::kDmemOffset;
    auto setRegion = [&picture](uint32_t index, const HevcBuffer &buffer,
        bool writable) {
        if (index >= HevcCore::Huc::kRegionCount || !buffer.address)
            return;
        picture.huc.regions[index].address = buffer.address;
        picture.huc.regions[index].offset = 0;
        picture.huc.regions[index].writable = writable;
        picture.huc.regions[index].used = true;
    };
    setRegion(0, picture.buffers.statistics, false);
    setRegion(1, picture.buffers.aggregatedStats, true);
    setRegion(4, picture.buffers.bitstream, false);
    setRegion(5, picture.buffers.bitstream, true);
    setRegion(6, picture.buffers.brcHistory, true);
    setRegion(7, picture.buffers.secondLevel, false);
    setRegion(9, picture.buffers.brcData, true);
    setRegion(15, picture.buffers.tileRecord, false);
    logValue("huc second level dwords", written);
    logValue("huc dmem address", picture.huc.dmemAddress);
    return true;
}

bool submitBrcKernel(HevcEncodeContext *encoder, const HevcPictureDesc &picture,
    const HevcCore::Huc::Params &params) {
    if (!picture.resolvedAddresses || !gOps.request || !gOps.submit || !gOps.wait ||
        !gOps.hucAuthenticated || gOps.hucAuthenticated() != 1 ||
        !encoder->submitContext || !picture.buffers.dmem.cpuAddress ||
        !picture.buffers.tileRecord.valid || picture.buffers.tileRecord.size < 4096 ||
        picture.buffers.dmem.size < kBrcReadbackOffset + 32)
        return false;
    auto *readback = static_cast<uint8_t *>(picture.buffers.dmem.cpuAddress) +
        kBrcReadbackOffset;
    memset(readback, 0, 32);
    HevcCore::Huc::BrcSubmission submission;
    submission.params = params;
    submission.dummyAddress = picture.buffers.tileRecord.address;
    submission.authenticationVerified = true;
    submission.statusAddress = picture.buffers.dmem.address + kBrcReadbackOffset;
    if (!++encoder->brcSubmissionCount) ++encoder->brcSubmissionCount;
    submission.completionTag = encoder->brcSubmissionCount;
    uint32_t commands[256] {};
    const uint32_t words = HevcCore::Huc::buildBrcSubmission(submission, commands, 256);
    if (!words) return false;

    HevcToken::Resource resources[kPictureResourceCount] {};
    for (uint32_t i = 0; i < kPictureResourceCount; ++i) {
        resources[i].handle = encoder->pictureHandle[i];
        if (!resources[i].handle) return false;
    }
    int32_t result = StatusInvalid;
    uint32_t fence = 0;
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
        void *descriptor = nullptr;
        result = gOps.request(encoder->submitContext, words + 24 + kResolveDeadDwords,
            HevcToken::kTotalDwords, &descriptor);
        if (result != StatusSuccess || !descriptor) break;
        auto *batch = static_cast<uint32_t *>(readPointer(descriptor, kVideoCommandStart));
        auto *base = static_cast<uint32_t *>(readPointer(descriptor, kVideoCommandBase));
        auto *sideband = static_cast<uint32_t *>(readPointer(descriptor, kVideoSidebandStart));
        if (!batch || !sideband) return false;
        memcpy(batch, commands, words * 4);
        const uint32_t offset = base && batch > base ? uint32_t(batch - base) : 0;
        const uint32_t tokenWords = HevcToken::buildResolveToken(sideband,
            HevcToken::kTotalDwords, resources, kPictureResourceCount, offset, words + 16);
        if (!tokenWords) return false;
        write64(descriptor, kVideoSidebandStart, reinterpret_cast<uint64_t>(sideband + tokenWords));
        result = gOps.submit(encoder->submitContext, &fence);
        if (result == 7)
            pthread_mutex_unlock(static_cast<char *>(encoder->submitContext) + kVideoMutex);
        if (result != 4) break;
    }
    if (result != StatusSuccess) return false;
    result = gOps.wait(encoder->submitContext, fence);
    encoder->lastFence = fence;
    logValue("BRC kernel descriptor", params.kernelDescriptor);
    logValue("BRC IMEM loaded", read32(readback, 4));
    logValue("BRC authentication", read32(readback, 12));
    logValue("BRC kernel status", read32(readback, 16));
    encoder->brcLastKernelStatus = read32(readback, 16);
    logValue("BRC kernel completion", read32(readback, 24));
    return result == StatusSuccess && read32(readback, 24) == submission.completionTag &&
        (read32(readback, 4) & HevcCore::Huc::kImemLoadedMask) &&
        (read32(readback, 12) & HevcCore::Huc::kAuthenticationMask) &&
        !(read32(readback, 16) & (1u << 28)); // Gen12 HEVC BRC memory-access error
}

// Every VBR mode checks completed output against the same byte budgets.
bool vbrRateMode(const HevcEncodeContext *encoder) {
    return encoder->rateInputValid && encoder->rateInput.mode == 1 &&
        !encoder->rateInput.constantBitRate;
}

// HuC controls all-intra sessions. Native frame-QP tracking applies to
// inter GOPs, where the admitted controller receives its reference feedback.
bool nativeVbrRateMode(const HevcEncodeContext *encoder) {
    return vbrRateMode(encoder) && encoder->rateInput.gopSize > 1;
}

bool prepareBrcInit(HevcEncodeContext *encoder, const HevcPictureDesc &picture) {
    if (!encoder->rateInputValid || encoder->rateInput.mode > 1 || encoder->rateInput.qp > 51 ||
        !picture.buffers.brcHistory.valid || picture.buffers.brcHistory.size < 2304 ||
        !encoder->linear[kScratchDmem].base || !encoder->linear[kScratchBrcHistory].base ||
        picture.buffers.dmem.cpuAddress != encoder->linear[kScratchDmem].base ||
        picture.buffers.brcHistory.cpuAddress != encoder->linear[kScratchBrcHistory].base)
        return false;
    const auto &input = encoder->rateInput;
    HevcCore::Huc::BrcInitParams params;
    params.mode = input.mode ? HevcCore::Huc::BrcMode::Vbr : HevcCore::Huc::BrcMode::Acqp;
    if (input.constantBitRate && input.mode == 1)
        params.mode = HevcCore::Huc::BrcMode::Cbr;
    const bool nativeVbr = nativeVbrRateMode(encoder);
    if (nativeVbr) {
        if (!input.targetRate || input.maximumRate < input.targetRate) return false;
        params.mode = HevcCore::Huc::BrcMode::Acqp;
    }
    params.width = picture.width; params.height = picture.height;
    params.targetRate = input.targetRate; params.maximumRate = input.maximumRate;
    if (params.mode == HevcCore::Huc::BrcMode::Cbr)
        params.maximumRate = input.targetRate;
    params.vbvSize = input.vbvSize; params.initialFullness = input.initialFullness;
    params.frameRateNumerator = input.frameRateNumerator;
    params.frameRateDenominator = input.frameRateDenominator;
    params.gopSize = input.gopSize;
    // A native frame-QP change must not restart HuC's sequence history.
    // Explicit quality control still reinitializes when its QP changes.
    params.initialQp = !nativeVbr && params.mode == HevcCore::Huc::BrcMode::Acqp ? uint8_t(input.qp) : 26;
    params.bitDepth = pictureIsTenBit(picture) ? 10 : 8;
    params.cuQpControl = picture.rateControlEnabled;
    params.windowFrames = input.windowFrames;
    params.windowMaximumRatio = input.windowMaximumRatio;
    if (params.mode == HevcCore::Huc::BrcMode::Cbr && params.windowFrames)
        params.windowMaximumRatio = 100;
    if (params.mode == HevcCore::Huc::BrcMode::Acqp)
        params.windowFrames = params.windowMaximumRatio = 0;
    HevcCore::Huc::BrcInitDmem dmem;
    if (!HevcCore::Huc::buildBrcInitDmem(params, dmem)) return false;
    if (vbrRateMode(encoder)) {
        HevcCore::FrameBudgetConfig budget {};
        budget.maximumRate = input.maximumRate;
        budget.bufferBits = input.vbvSize;
        budget.initialBits = input.initialFullness;
        budget.fpsNumerator = input.frameRateNumerator;
        budget.fpsDenominator = input.frameRateDenominator;
        budget.windowCount = input.limitCount;
        if (budget.windowCount > 2) return false;
        for (uint32_t i = 0; i < budget.windowCount; ++i)
            budget.windows[i] = {input.limitFrames[i], input.limitBits[i]};
        if (!encoder->vbrBudget.configure(budget)) return false;
    }
    if (encoder->brcInitialized &&
        !memcmp(&dmem, &encoder->brcConfiguration, sizeof(dmem)))
        return true;
    // A new rate/geometry/quality starts a complete history. The native path
    // uses the same Init kernel for sequence reinitialization and reset.
    encoder->brcInitialized = false;
    if (!writeScratch(encoder, kScratchDmem, &dmem, sizeof(dmem))) return false;
    memset(picture.buffers.brcHistory.cpuAddress, 0, 2304);
    HevcCore::Huc::Params kernel;
    kernel.kernelDescriptor = HevcCore::Huc::kHevcBrcInitKernel;
    kernel.dmemAddress = picture.buffers.dmem.address;
    kernel.dmemLength = sizeof(dmem);
    kernel.regions[0] = {picture.buffers.brcHistory.address, 0, true, true};
    if (!submitBrcKernel(encoder, picture, kernel)) return false;
    encoder->brcConfiguration = dmem;
    encoder->brcFrameId = 1; // media-driver's m_storeData begins at one
    encoder->brcInitialized = true;
    return true;
}

bool brcSliceHeaderMatches(const HevcEncodeContext *encoder,
    const HevcPictureDesc &picture, const HevcBrcBatchLayout &layout,
    const uint32_t *commands, uint32_t capacityDwords) {
    const uint32_t state = layout.slice.sliceStateOffset / 4;
    const uint32_t pio = layout.slice.sliceHeaderOffset / 4;
    if (!commands || (layout.slice.sliceStateOffset & 3) ||
        (layout.slice.sliceHeaderOffset & 3) || state >= capacityDwords ||
        capacityDwords - state < 4 || pio >= capacityDwords ||
        capacityDwords - pio < 3 || (commands[pio] & ~0xfffu) != 0x73a20000)
        return false;
    const uint32_t payloadWords = commands[pio] & 0xfff;
    const uint32_t finalBits = (commands[pio + 1] >> 8) & 63;
    if (!payloadWords || payloadWords > capacityDwords - pio - 2 ||
        !finalBits || finalBits > 32 || (commands[pio + 1] & 14) != 12 ||
        ((commands[pio + 1] >> 4) & 15) != 6)
        return false;
    HevcCore::SliceConfig slice;
    slice.qp = (commands[state + 3] >> 6) & 63;
    slice.idr = picture.nalType >= 16 && picture.nalType <= 23;
    slice.poc = picture.poc;
    slice.inter = picture.inter;
    slice.lowDelayB = picture.inter && picture.lowDelay;
    slice.refPoc = picture.refPoc;
    slice.rpsCount = picture.rpsCount;
    for (uint32_t i = 0; i < picture.rpsCount && i < 4; ++i)
        slice.rpsPocs[i] = picture.rpsPocs[i];
    slice.deblockingFilterDisable = true;
    const auto raw = HevcCore::buildSliceHeader(encoder->headerConfig, slice, false);
    const uint32_t bits = (payloadWords - 1) * 32 + finalBits;
    return bits == raw.size() * 8 &&
        !memcmp(raw.data(), commands + pio + 2, raw.size());
}

bool prepareBrcUpdate(HevcEncodeContext *encoder, HevcPictureDesc &picture,
    uint8_t pass) {
    if (!encoder->brcInitialized || pass > 1 ||
        !encoder->linear[kScratchAggregatedStats].base ||
        picture.buffers.aggregatedStats.cpuAddress != encoder->linear[kScratchAggregatedStats].base ||
        picture.buffers.aggregatedStats.size < kBrcPakInfoOffset + 4096 ||
        !encoder->linear[kScratchSecondLevel].base ||
        picture.buffers.secondLevel.cpuAddress != encoder->linear[kScratchSecondLevel].base ||
        picture.buffers.secondLevel.size < kSecondLevelSize ||
        !picture.buffers.brcData.valid ||
        picture.buffers.brcData.size < HevcCore::Huc::kBrcPictureDataSlots *
            HevcCore::Huc::kBrcPictureDataBytes || !picture.buffers.encodedSliceSize.valid ||
        !picture.buffers.statistics.valid || !picture.buffers.hcpScratch.valid)
        return false;
    auto *input = static_cast<uint8_t *>(picture.buffers.aggregatedStats.cpuAddress);
    auto *output = static_cast<uint32_t *>(picture.buffers.secondLevel.cpuAddress);
    HevcBrcBatchLayout layout;
    picture.frameNumber = encoder->brcFrameId;
    picture.brcPass = pass;
    picture.brcPakOnlyPass = pass && encoder->brcConfiguration.BRCFlag != 0;
    const uint32_t words = buildHevcBrcBatchCommands(picture,
        reinterpret_cast<uint32_t *>(input), kSecondLevelSize / 4, layout);
    if (!words) return false;
    layout.update.targetFullness = encoder->brcConfiguration.InitBufFull_U32;
    layout.update.pass = pass;
    layout.update.maximumPasses = encoder->brcConfiguration.BRCFlag ? 2 : 1;
    layout.update.slidingWindow = encoder->brcConfiguration.BRCFlag &&
        encoder->rateInput.windowFrames != 0;
    if (!prepareBrcPictureData(encoder, picture, layout.update)) return false;
    logValue("BRC picture data offset", layout.update.pictureDataOffset);
    if (picture.inter)
        logValue("BRC reference data offset", layout.update.referenceDataOffsets[0]);
    HevcCore::Huc::BrcUpdateDmem dmem;
    if (!HevcCore::Huc::buildBrcUpdateDmem(layout.update, dmem)) return false;
    if (pass) {
        // Intel removes byte_alignment() from the second-pass constant-data
        // bit count so HuC can rewrite and realign a variable-length header.
        const uint8_t last = picture.sliceHeader[(picture.sliceHeaderBits + 7) / 8 - 1];
        if (!last) return false;
        for (uint32_t i = 0; i < 8; ++i) {
            if (last & (1u << i)) {
                layout.slice.sliceHeaderBits -= i + 1;
                break;
            }
        }
    }
    HevcCore::Huc::BrcConstantParams constantParams {
        picture.inter, words * 4, 1, &layout.slice};
    HevcCore::Huc::BrcConstantData constants;
    if (!HevcCore::Huc::buildBrcConstantData(constantParams, constants)) return false;
    memcpy(input + kBrcConstantsOffset, &constants, sizeof(constants));
    if (encoder->brcFrameId == 1 && !pass)
        memset(input + kBrcPakInfoOffset, 0, 4096);
    memset(output, 0, words * 4);
    if (!writeScratch(encoder, kScratchDmem, &dmem, sizeof(dmem))) return false;
    HevcCore::Huc::Params kernel;
    kernel.kernelDescriptor = HevcCore::Huc::kHevcBrcUpdateKernel;
    kernel.dmemAddress = picture.buffers.dmem.address;
    kernel.dmemLength = sizeof(dmem);
    kernel.regions[0] = {picture.buffers.brcHistory.address, 0, true, true};
    kernel.regions[1] = {picture.buffers.statistics.address, 0, false, true};
    kernel.regions[2] = {picture.buffers.hcpScratch.address,
        17 * kHcpScratchRegionBytes, false, true};
    kernel.regions[3] = {picture.buffers.aggregatedStats.address, 0, false, true};
    kernel.regions[4] = {picture.buffers.aggregatedStats.address,
        kBrcConstantsOffset, false, true};
    kernel.regions[5] = {picture.buffers.secondLevel.address, 0, true, true};
    kernel.regions[6] = {picture.buffers.brcData.address, 0, true, true};
    kernel.regions[7] = {picture.buffers.encodedSliceSize.address, 0, false, true};
    kernel.regions[8] = {picture.buffers.aggregatedStats.address,
        kBrcPakInfoOffset, false, true};
    kernel.regions[15] = {picture.buffers.tileRecord.address, 0, true, true};
    if (!submitBrcKernel(encoder, picture, kernel)) return false;
    if (pass && !(encoder->brcLastKernelStatus & (1u << 31)))
        return true; // the first pass remains this picture's final output
    logValue("BRC output picture QP", (output[layout.update.pictureOffset / 4 + 19] >> 8) & 63);
    logValue("BRC output VDEnc QP", (output[layout.update.cmd2Offset / 4 + 27] >> 8) & 63);
    logValue("BRC output slice QP", (output[layout.slice.startOffset / 4 + 3] >> 6) & 63);
    logValue("BRC output CU QP depth", (output[layout.update.pictureOffset / 4 + 4] >> 6) & 3);
    uint32_t qp;
    if (!validateHevcBrcBatchOutput(picture, layout, output, words, qp)) {
        logMessage("BRC output batch rejected");
        return false;
    }
    if (!brcSliceHeaderMatches(encoder, picture, layout, output, words)) {
        logMessage("BRC output slice header rejected");
        return false;
    }
    encoder->brcOutputQp = qp;
    encoder->brcLayout = layout;
    picture.brcBatchOutput = true;
    picture.secondLevelBatch = true;
    picture.brcPictureOffset = layout.update.cmd1Offset;
    picture.brcSliceOffset = layout.slice.startOffset;
    picture.brcBatchBytes = layout.update.batchBytes;
    picture.brcPakInfoAddress = picture.buffers.aggregatedStats.address + kBrcPakInfoOffset;
    return true;
}

bool submitNativeVbrRetry(HevcEncodeContext *encoder, HevcPictureDesc &picture,
    uint32_t &fence) {
    picture.brcLastImageStatusAddress = 0;
    picture.completionTag = 0x80000000u | (encoder->brcSubmissionCount & 0x7fffffffu);
    const uint32_t words = hevcPictureCommandDwords(picture);
    auto *commands = static_cast<uint32_t *>(calloc(words ? words : 1, 4));
    if (!commands) return false;
    if (!words || buildHevcPictureCommands(picture, commands, words) != words) {
        free(commands);
        return false;
    }
    HevcToken::Resource resources[kPictureResourceCount] {};
    for (uint32_t i = 0; i < kPictureResourceCount; ++i)
        resources[i].handle = encoder->pictureHandle[i];
    auto *completion = static_cast<uint8_t *>(picture.buffers.statistics.cpuAddress) +
        picture.buffers.statistics.size - 64;
    // Clear the completed attempt's tag before submitting another full picture.
    write64(completion, 0, 0);
    int32_t result = StatusInvalid;
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
        void *descriptor = nullptr;
        result = gOps.request(encoder->submitContext, words + 24 + kResolveDeadDwords,
            HevcToken::kTotalDwords, &descriptor);
        if (result != StatusSuccess || !descriptor) break;
        auto *batch = static_cast<uint32_t *>(readPointer(descriptor, kVideoCommandStart));
        auto *base = static_cast<uint32_t *>(readPointer(descriptor, kVideoCommandBase));
        auto *sideband = static_cast<uint32_t *>(readPointer(descriptor, kVideoSidebandStart));
        if (!batch || !sideband) { result = StatusInvalid; break; }
        memcpy(batch, commands, words * 4);
        const uint32_t offset = base && batch > base ? uint32_t(batch - base) : 0;
        const uint32_t tokenWords = HevcToken::buildResolveToken(sideband,
            HevcToken::kTotalDwords, resources, kPictureResourceCount, offset, words + 16);
        if (!tokenWords) { result = StatusInvalid; break; }
        write64(descriptor, kVideoSidebandStart, reinterpret_cast<uint64_t>(sideband + tokenWords));
        result = gOps.submit(encoder->submitContext, &fence);
        if (result == 7)
            pthread_mutex_unlock(static_cast<char *>(encoder->submitContext) + kVideoMutex);
        if (result != 4) break;
    }
    free(commands);
    if (result != StatusSuccess) return false;
    result = gOps.wait(encoder->submitContext, fence);
    encoder->lastFence = fence;
    return checkPictureCompletion(encoder, result, completion, picture.completionTag);
}

// All-intra VBR uses HuC for average-rate tracking. A completed candidate
// that exceeds an exact window is retried with the CQP command builder;
// the next HuC update consumes the final attempt's real PAK statistics.
// This is restricted to pictures with no reference-data consumers.
bool prepareAllIntraVbrRetry(HevcEncodeContext *encoder, HevcPictureDesc &picture) {
    if (picture.inter || !vbrRateMode(encoder) || nativeVbrRateMode(encoder) ||
        encoder->brcConfiguration.BRCFlag != 2) return false;
    auto *output = static_cast<uint32_t *>(picture.buffers.secondLevel.cpuAddress);
    if (!output || picture.buffers.secondLevel.cpuAddress !=
        encoder->linear[kScratchSecondLevel].base) return false;
    picture.brcPass = 0;
    picture.brcPakOnlyPass = false;
    HevcBrcBatchLayout layout;
    const uint32_t words = buildHevcBrcBatchCommands(picture, output,
        picture.buffers.secondLevel.size / 4, layout);
    if (!words) return false;
    // Match the HCP statistics QP contract produced by HuC for Main10.
    const uint32_t at = layout.update.pictureOffset / 4 + 19;
    output[at] = (output[at] & ~(63u << 8)) |
        ((picture.qpY + (pictureIsTenBit(picture) ? 12 : 0)) << 8);
    layout.update.targetFullness = encoder->brcConfiguration.InitBufFull_U32;
    layout.update.maximumPasses = 2;
    if (!prepareBrcPictureData(encoder, picture, layout.update)) return false;
    uint32_t qp;
    if (!validateHevcBrcBatchOutput(picture, layout, output, words, qp) ||
        !brcSliceHeaderMatches(encoder, picture, layout, output, words)) return false;
    encoder->brcOutputQp = qp;
    encoder->brcLayout = layout;
    picture.brcPictureOffset = layout.update.cmd1Offset;
    picture.brcSliceOffset = layout.slice.startOffset;
    picture.brcBatchBytes = layout.update.batchBytes;
    return true;
}

bool finishNativeVbrPicture(HevcEncodeContext *encoder, HevcPictureDesc &picture,
    uint32_t &fence) {
    const auto fail = [encoder]() { encoder->submissionFailed = true; return false; };
    const uint32_t available = encoder->vbrBudget.availableBits();
    if (!available) return fail();
    const bool native = nativeVbrRateMode(encoder);
    if (!native) {
        // A second update that declines repacking may leave the SLB empty.
        // Keep the QP from the last validated, actually submitted batch.
        if (picture.inter || encoder->brcConfiguration.BRCFlag != 2 ||
            encoder->brcOutputQp > 51) return fail();
        picture.qpY = encoder->brcOutputQp;
    }
    auto *completion = static_cast<uint8_t *>(picture.buffers.statistics.cpuAddress) +
        picture.buffers.statistics.size - 64;
    while (true) {
        const uint32_t bytes = read32(completion, 8);
        if (!bytes || bytes > picture.buffers.bitstream.size) return fail();
        if (uint64_t(bytes) * 8 <= available) return true;
        if (picture.qpY >= 51) return fail();
        ++picture.qpY;
        logValue("VBR budget retry QP", picture.qpY);
        logValue("VBR budget available bits", available);
        if (!preparePictureHeader(encoder, picture) ||
            !(native ? prepareBrcUpdate(encoder, picture, 0) :
                prepareAllIntraVbrRetry(encoder, picture)) ||
            !submitNativeVbrRetry(encoder, picture, fence)) return fail();
    }
}

bool finishBrcPicture(HevcEncodeContext *encoder, HevcPictureDesc &picture,
    uint32_t &fence) {
    // Explicit ACQP is one pass without weighted prediction, SSC or SAO.
    // VBR may need a full picture retry to meet its output budget.
    if (!encoder->brcConfiguration.BRCFlag)
        return !nativeVbrRateMode(encoder) || finishNativeVbrPicture(encoder, picture, fence);
    if (!prepareBrcUpdate(encoder, picture, 1)) {
        encoder->brcInitialized = false;
        return false;
    }
    fence = encoder->lastFence;
    const bool reencode = (encoder->brcLastKernelStatus & (1u << 31)) != 0;
    logValue("BRC reencode", reencode);
    if (!reencode)
        return !vbrRateMode(encoder) || finishNativeVbrPicture(encoder, picture, fence);
    picture.brcLastImageStatusAddress = picture.completionAddress + 12;
    picture.completionTag = 0x80000000u | (encoder->brcSubmissionCount & 0x7fffffffu);
    const uint32_t words = hevcPictureCommandDwords(picture);
    auto *commands = static_cast<uint32_t *>(calloc(words ? words : 1, 4));
    if (!commands) return false;
    if (!words || buildHevcPictureCommands(picture, commands, words) != words) {
        free(commands);
        return false;
    }
    HevcToken::Resource resources[kPictureResourceCount] {};
    for (uint32_t i = 0; i < kPictureResourceCount; ++i)
        resources[i].handle = encoder->pictureHandle[i];
    auto *completion = static_cast<uint8_t *>(picture.buffers.statistics.cpuAddress) +
        picture.buffers.statistics.size - 64;
    // Preserve the first-pass image control for the LRM preceding repack.
    write64(completion, 0, 0);
    int32_t result = StatusInvalid;
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
        void *descriptor = nullptr;
        result = gOps.request(encoder->submitContext, words + 24 + kResolveDeadDwords,
            HevcToken::kTotalDwords, &descriptor);
        if (result != StatusSuccess || !descriptor) break;
        auto *batch = static_cast<uint32_t *>(readPointer(descriptor, kVideoCommandStart));
        auto *base = static_cast<uint32_t *>(readPointer(descriptor, kVideoCommandBase));
        auto *sideband = static_cast<uint32_t *>(readPointer(descriptor, kVideoSidebandStart));
        if (!batch || !sideband) { result = StatusInvalid; break; }
        memcpy(batch, commands, words * 4);
        const uint32_t offset = base && batch > base ? uint32_t(batch - base) : 0;
        const uint32_t tokenWords = HevcToken::buildResolveToken(sideband,
            HevcToken::kTotalDwords, resources, kPictureResourceCount, offset, words + 16);
        if (!tokenWords) { result = StatusInvalid; break; }
        write64(descriptor, kVideoSidebandStart, reinterpret_cast<uint64_t>(sideband + tokenWords));
        result = gOps.submit(encoder->submitContext, &fence);
        if (result == 7)
            pthread_mutex_unlock(static_cast<char *>(encoder->submitContext) + kVideoMutex);
        if (result != 4) break;
    }
    free(commands);
    if (result != StatusSuccess) return false;
    result = gOps.wait(encoder->submitContext, fence);
    encoder->lastFence = fence;
    return checkPictureCompletion(encoder, result, completion, picture.completionTag) &&
        (!vbrRateMode(encoder) || finishNativeVbrPicture(encoder, picture, fence));
}

int32_t setThunk(void *context, const ContextParamEntry *entry) {
    return hevcSetContextParam(context, entry);
}

int32_t getThunk(void *context, const ContextParamEntry *entry) {
    return hevcGetContextParam(context, entry);
}

int32_t scheduleThunk(void *context, const CommandNode *command, void *refCon) {
    return hevcSchedule(context, command, refCon);
}

void *const gVtable[16] = {
    reinterpret_cast<void *>(&destroyThunk),
    reinterpret_cast<void *>(&unsupported),
    reinterpret_cast<void *>(&unsupported),
    reinterpret_cast<void *>(&setThunk),
    reinterpret_cast<void *>(&getThunk),
    reinterpret_cast<void *>(&scheduleThunk),
    reinterpret_cast<void *>(&unsupported),
    reinterpret_cast<void *>(&unsupported),
    reinterpret_cast<void *>(&unsupported),
    reinterpret_cast<void *>(&unsupported),
    reinterpret_cast<void *>(&unsupported),
    reinterpret_cast<void *>(&unsupported),
    reinterpret_cast<void *>(&unsupported),
    reinterpret_cast<void *>(&unsupported),
    reinterpret_cast<void *>(&unsupported),
    reinterpret_cast<void *>(&unsupported),
};

int32_t destroyThunk(void *context) {
    if (isHevcEncodeContext(context))
        logValue("destroy id", static_cast<HevcEncodeContext *>(context)->id);
    return StatusSuccess;
}

HevcEncodeContext *lookup(void *context) {
    if (!context)
        return nullptr;
    for (uint32_t i = 0; i < kMaxContexts; ++i) {
        if (gContexts[i] == context)
            return gContexts[i];
    }
    return nullptr;
}

}

void setHevcLog(HevcLogFn log) {
    gLog = log;
}

int32_t hevcSetRateControlInput(void *context,
    const HevcPlugin::RateControlInput *input) {
    auto *encoder = lookup(context);
    if (!encoder)
        return StatusInvalidParameter;
    encoder->rateInputValid = input != nullptr;
    if (input)
        encoder->rateInput = *input;
    return StatusSuccess;
}

void setHevcCompletionSignal(void (*signal)()) {
    gCompletionSignal = signal;
}

void setHevcSubmitOps(const HevcSubmitOps &ops) {
    gOps = ops;
}

bool isHevcEncodeContext(const void *context) {
    return lookup(const_cast<void *>(context)) != nullptr;
}

void *findHevcEncodeContext(uint32_t id) {
    for (uint32_t i = 0; i < kMaxContexts; ++i) {
        if (gContexts[i] && gContexts[i]->id == id)
            return gContexts[i];
    }
    return nullptr;
}

void *createHevcEncodeContext(void *driver, const ContextDesc *descriptor) {
    if (!descriptor || descriptor->type != kHevcEncodeContextType ||
        descriptor->subtype != kHevcEncodeContextSubtype)
        return nullptr;
    uint32_t slot = kMaxContexts;
    for (uint32_t i = 0; i < kMaxContexts; ++i) {
        if (!gContexts[i]) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxContexts) {
        logMessage("context registry full");
        return nullptr;
    }
    auto *context = static_cast<HevcEncodeContext *>(
        calloc(1, sizeof(HevcEncodeContext)));
    if (!context) {
        logMessage("context allocation failed");
        return nullptr;
    }
    context->vtable = const_cast<void **>(gVtable);
    context->driver = driver;
    const uint32_t videoOffset = videoContextOffsetForId(descriptor->id);
    context->videoContext = driver
        ? *(static_cast<void **>(static_cast<void *>(
              static_cast<char *>(driver) + videoOffset)))
        : nullptr;
    uint32_t submitOffset = videoOffset;
    if (experimentalEnabled("WEX_HEVC_VC", kVc80Marker))
        submitOffset = kVideoContextOffset;
    else if (experimentalEnabled("WEX_HEVC_VC", kVc96Marker))
        submitOffset = kVideoContextOffset + 16;
    context->submitContext = driver
        ? *(static_cast<void **>(static_cast<void *>(
              static_cast<char *>(driver) + submitOffset)))
        : nullptr;
    context->id = descriptor->id;
    context->width = descriptor->width;
    context->height = descriptor->height;
    context->setParam1 = 4; // driver default before the plugin sets it
    context->getParam2 = 64;
    context->format = descriptor->reserved4[0];
    if (driver)
        context->surfaceManager =
            static_cast<char *>(driver) + kSurfaceManagerOffset;
    gContexts[slot] = context;

    // The plug-in waits for the previous submission before it prepares the
    // next command buffer. A fresh context has nothing in flight, and every
    // picture completes before Schedule returns, so one released semaphore
    // stands in for that empty wait.
    if (gCompletionSignal)
        gCompletionSignal();

    if (gLog) {
        char *out = gHevcLine;
        append(out, "hevc context id=");
        appendDec(out, context->id);
        append(out, " size=");
        appendDec(out, context->width);
        *out++ = 'x';
        appendDec(out, context->height);
        append(out, " format=");
        appendHex(out, context->format);
        append(out, " videoOffset=");
        appendDec(out, videoOffset);
        append(out, " submitOffset=");
        appendDec(out, submitOffset);
        emit(gHevcLine, out);
    }
    return context;
}

int32_t hevcSetContextParam(void *context, const ContextParamEntry *entry) {
    HevcEncodeContext *encoder = lookup(context);
    if (!encoder || !entry)
        return StatusInvalidParameter;
    switch (entry->parameter) {
    case 1:
        // Parameter 1 carries the encoder configuration block. The ICL
        // driver keeps only its first word; do the same and log the value.
        if (entry->value)
            encoder->setParam1 = read32(entry->value, 0);
        logValue("set param1", encoder->setParam1);
        if (!encoder->loggedConfig) {
            encoder->loggedConfig = 1;
            logConfig(entry->value);
        }
        return StatusSuccess;
    case 5:
        if (entry->value) {
            encoder->setParam5[0] = read32(entry->value, 0);
            encoder->setParam5[1] = read32(entry->value, 4);
        }
        if (gLog) {
            char *out = gHevcLine;
            append(out, "hevc set param5=");
            appendDec(out, encoder->setParam5[0]);
            *out++ = ',';
            appendDec(out, encoder->setParam5[1]);
            emit(gHevcLine, out);
        }
        return StatusSuccess;
    case 2:
        // The plugin reads this through the other parameter slot; accept it
        // here as well and answer with the same internal size.
        if (entry->value)
            *static_cast<uint32_t *>(entry->value) = encoder->getParam2;
        return StatusSuccess;
    default:
        logValue("set unknown parameter", entry->parameter);
        return StatusInvalidParameter;
    }
}

int32_t hevcGetContextParam(void *context, const ContextParamEntry *entry) {
    HevcEncodeContext *encoder = lookup(context);
    if (!encoder || !entry)
        return StatusInvalidParameter;
    if (entry->parameter == 2) {
        if (entry->value)
            *static_cast<uint32_t *>(entry->value) = encoder->getParam2;
        logValue("get param2", encoder->getParam2);
        return StatusSuccess;
    }
    logValue("get unknown parameter", entry->parameter);
    return StatusInvalidParameter;
}

static int32_t schedulePicture(void *context, const CommandNode *command,
    void *refCon, bool allowResolve) {
    HevcEncodeContext *encoder = lookup(context);
    if (!encoder || !command)
        return StatusInvalidParameter;
    if (command->type != kHevcEncodeContextType ||
        command->subtype != kHevcEncodeContextSubtype)
        return StatusNotFound;

    if (encoder->submissionFailed)
        return StatusNotFound;

    ++encoder->scheduleCount;
    const void *payload = command->payload;
    const bool logThis = encoder->logged < kMaxLoggedSchedules;
    if (logThis)
        ++encoder->logged;

    if (!payload) {
        logMessage("schedule without payload");
        return StatusInvalidParameter;
    }

    if (gLog && logThis) {
        char *out = gHevcLine;
        append(out, "hevc schedule id=");
        appendDec(out, encoder->id);
        append(out, " count=");
        appendDec(out, encoder->scheduleCount);
        append(out, " payload=");
        appendHex(out, reinterpret_cast<uintptr_t>(payload));
        append(out, " refCon=");
        appendHex(out, reinterpret_cast<uintptr_t>(refCon));
        emit(gHevcLine, out);

        out = gHevcLine;
        append(out, "hevc picture index=");
        appendDec(out, read32(payload, kPayloadReconstructionIndex));
        append(out, " nal=");
        appendDec(out, read32(payload, kPayloadNalType));
        append(out, " poc=");
        appendDec(out, read32(payload, kPayloadPoc));
        append(out, " tid=");
        appendDec(out, read32(payload, kPayloadTemporalId));
        append(out, " slices=");
        appendDec(out, read32(payload, kPayloadSliceCount));
        append(out, " size=");
        appendDec(out, encoder->width);
        *out++ = 'x';
        appendDec(out, encoder->height);
        append(out, " f904=");
        appendDec(out, read32(payload, kPayloadField904));
        append(out, " input=");
        appendHex(out, reinterpret_cast<uintptr_t>(
                            readPointer(payload, kPayloadInputSurface)));
        append(out, " recon=");
        appendHex(out, reinterpret_cast<uintptr_t>(
                            readPointer(payload, kPayloadReconSurface)));
        emit(gHevcLine, out);

        out = gHevcLine;
        append(out, "hevc extra=");
        appendHex(out, reinterpret_cast<uintptr_t>(
                            readPointer(payload, kPayloadExtra)));
        append(out, " extra2=");
        appendHex(out, reinterpret_cast<uintptr_t>(
                            readPointer(payload, kPayloadExtra2)));
        emit(gHevcLine, out);

        const void *extra = readPointer(payload, kPayloadExtra);
        if (extra) {
            for (unsigned off = 0; off < 0x40; off += 16) {
                out = gHevcLine;
                append(out, "hevc extra+");
                appendHex(out, off);
                for (unsigned i = 0; i < 4; ++i) {
                    *out++ = ' ';
                    appendHex(out, read32(extra, off + i * 4));
                }
                emit(gHevcLine, out);
            }
        }

        if (experimentalEnabled("WEX_HEVC_PAYLOAD", kPayloadDumpMarker)) {
            // Only the first 0x400 bytes are documented; the plug-in keeps its
            // reference bookkeeping beyond that and this dump shows whether a
            // pointer to a previously encoded recon surface appears.
            for (unsigned off = 0; off < 0x400; off += 32) {
                out = gHevcLine;
                append(out, "hevc payload+");
                appendHex(out, off);
                for (unsigned i = 0; i < 8; ++i) {
                    *out++ = ' ';
                    appendHex(out, read32(payload, off + i * 4));
                }
                emit(gHevcLine, out);
            }
            const void *blocks[2] = {extra,
                readPointer(payload, kPayloadExtra2)};
            for (unsigned block = 0; block < 2; ++block) {
                if (!blocks[block])
                    continue;
                for (unsigned off = 0; off < 0x100; off += 32) {
                    out = gHevcLine;
                    append(out, "hevc extra");
                    appendDec(out, block + 1);
                    *out++ = '+';
                    appendHex(out, off);
                    for (unsigned i = 0; i < 8; ++i) {
                        *out++ = ' ';
                        appendHex(out, read32(blocks[block], off + i * 4));
                    }
                    emit(gHevcLine, out);
                }
            }
        }
    }

    if (refCon)
        *static_cast<void **>(refCon) = nullptr;

    if (logThis && encoder->scheduleCount == 1) {
        logValue("hardware submission enabled", 1);
        logValue("env alloc", experimentalEnabled("WEX_HEVC_ALLOC",
            kAllocMarker) ? 1 : 0);
        logValue("env build", experimentalEnabled("WEX_HEVC_BUILD",
            kBuildMarker) ? 1 : 0);
    }

    if (!gOps.request || !gOps.submit || !gOps.wait || !encoder->videoContext) {
        if (logThis)
            logMessage("schedule submission path unavailable");
        return StatusNotFound;
    }

    // Rebind every picture: the plug-in cycles its IOSurface pool, and a
    // reconstruction surface can change independently of the source.
    const auto previousInput = encoder->inputSurface;
    const auto previousRecon = encoder->reconSurface;
    attachSurfaceManager(encoder);
    void *const reconSurfacePointer = readPointer(payload, kPayloadReconSurface);
    if (!bindPictureSurface(encoder, readPointer(payload, kPayloadInputSurface),
            "input surface", encoder->inputSurface) ||
        !bindPictureSurface(encoder, reconSurfacePointer,
            "recon surface", encoder->reconSurface))
        return StatusNotFound;
    encoder->payloadReconSurface = reconSurfacePointer;
    HevcPictureDesc surfaceFormatProbe;
    surfaceFormatProbe.source = encoder->inputSurface;
    if (pictureIsTenBit(surfaceFormatProbe)) {
        surfaceFormatProbe.source = encoder->reconSurface;
        if (!pictureIsTenBit(surfaceFormatProbe) ||
            encoder->inputSurface.pitch < uint64_t(encoder->inputSurface.width) * 2 ||
            encoder->reconSurface.pitch < uint64_t(encoder->reconSurface.width) * 2) {
            logMessage("Main10 surface layout mismatch");
            return StatusNotFound;
        }
    }
    if (previousInput.address != encoder->inputSurface.address ||
        previousInput.uvAddress != encoder->inputSurface.uvAddress ||
        previousRecon.address != encoder->reconSurface.address ||
        previousRecon.uvAddress != encoder->reconSurface.uvAddress)
        encoder->resolvedCount = 0;
    if (!encoder->surfacesLogged) {
        encoder->surfacesLogged = 1;
        dumpSurfaceEntry(encoder, "input");
    }

    const bool submitRequested = true;
    // Once the kernel has returned the graphics addresses, the picture
    // sequence can use them directly instead of the renderer's handles. The
    // input surface may change between frames, so the cached addresses only
    // apply while the resolved source handle still matches.
    const bool addressesEnabled =
        encoder->resolvedCount == kPictureResourceCount &&
        encoder->resolvedSourceHandle == encoder->inputSurface.address;
    // A sentinel-only batch proves whether the media engine runs the submitted
    // data-buffer batch at all, independently of the HCP/VDEnc contents.
    const bool pingRequested = addressesEnabled &&
        experimentalEnabled("WEX_HEVC_PING", kPingMarker);
    // The resolve probe takes precedence over the picture sequence: it submits
    // MI_BATCH_BUFFER_END with a VDENC token so the kernel fills in the real
    // graphics addresses, which user space cannot obtain by itself.
    const bool resolveRequested = !addressesEnabled;
    if (resolveRequested && !allowResolve)
        return StatusNotFound;
    // WEX_HEVC_BUILD assembles the sequence and resolves every buffer address
    // without writing it into the submitted batch. WEX_HEVC_ALLOC only
    // exercises the internal buffer allocation.
    bool submitSequence = submitRequested && !resolveRequested;
    const bool buildSequence = submitRequested || resolveRequested ||
        experimentalEnabled("WEX_HEVC_BUILD", kBuildMarker) ||
        experimentalEnabled("WEX_HEVC_HUC", kHucMarker);
    if (buildSequence || experimentalEnabled("WEX_HEVC_ALLOC", kAllocMarker))
        allocateScratch(encoder);

    HevcPictureDesc picture;
    uint32_t commandDwords = 64;
    if (buildSequence) {
        picture.width = encoder->width;
        picture.height = encoder->height;
        if (encoder->inputSurface.valid) {
            if (encoder->inputSurface.width)
                picture.width = encoder->inputSurface.width;
            if (encoder->inputSurface.height)
                picture.height = encoder->inputSurface.height;
        }
        // The HCP picture/tile state, VDEnc frame state and walker must use
        // the same coded extent. A visible 2142-line image needs 2144 coded
        // lines; sending the visible height to only one engine stalls it.
        picture.visibleWidth = picture.width;
        picture.visibleHeight = picture.height;
        picture.width = (picture.width + 7u) & ~7u;
        picture.height = (picture.height + 7u) & ~7u;
        picture.qpY = 26;
        if (experimentalEnabled("WEX_HEVC_QP51", kQpHighMarker))
            picture.qpY = 51;
        else if (experimentalEnabled("WEX_HEVC_QP40", kQpMidMarker))
            picture.qpY = 40;
        else if (experimentalEnabled("WEX_HEVC_QP10", kQpLowMarker))
            picture.qpY = 10;
        picture.codingType = 0;
        picture.reconstructionIndex = read32(payload, kPayloadReconstructionIndex);
        picture.nalType = read32(payload, kPayloadNalType);
        // An IRAP picture begins a new sequence; the decoder's picture buffer
        // is empty afterwards, so the remembered POCs and surfaces from the
        // previous sequence must not reach this picture's keep list.
        if (picture.nalType >= 16 && picture.nalType <= 23)
            resetReferenceHistory(encoder);
        picture.poc = read32(payload, kPayloadPoc);
        picture.source = encoder->inputSurface;
        picture.recon = encoder->reconSurface;
        picture.reference = encoder->reconSurface;
        // The picture writes its downscale into the current pool slot. Only an
        // inter picture reads another picture's downscale, from the slot that
        // picture wrote.
        picture.scaledSlot = encoder->scaledSlot;
        picture.referenceScaledSlot = encoder->scaledSlot;
        picture.referenceScaledValid = false;
        picture.probeHucStatus = experimentalEnabled("WEX_HEVC_HUC_STATUS",
            kHucStatusMarker);
        if (experimentalEnabled("WEX_HEVC_HUC_IMEM", kHucImemMarker)) {
            picture.probeHucStatus = true;
            picture.probeHucKernel = HevcCore::Huc::kHevcBrcInitKernel;
        }
        picture.pictureFlush = experimentalEnabled("WEX_HEVC_PICFLUSH",
            kPictureFlushMarker);
        picture.oneCtb = experimentalEnabled("WEX_HEVC_ONE_CTB",
            kOneCtbMarker);
        encoder->referenceHandle = 0;
        if (!(picture.nalType >= 16 && picture.nalType <= 23) &&
            readPointer(payload, kPayloadRefL0Surface)) {
            // The native builder fills L0 only for inter pictures. The
            // reconstruction index can be zero on those pictures, so it is
            // not a coding-type guard. Admit only an earlier completed recon
            // surface from the cache; no native slice pointer is needed.
            void *const refSurfacePointer =
                readPointer(payload, kPayloadRefL0Surface);
            const uint32_t refHandle = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(refSurfacePointer));
            const uint32_t refPoc = read32(payload, kPayloadRefL0Poc);
            const auto *cached = findRecon(encoder, refHandle);
            if (logThis) {
                logValue("payload ref handle", refHandle);
                logValue("payload ref poc", refPoc);
                logValue("reference cached", cached ? 1 : 0);
                if (cached)
                    logValue("reference address", cached->address);
            }
            if (cached) {
                // B pictures use their list 0 reference the same way; the
                // slice is coded with one shared list. The reference may sit
                // after this picture in display order; the slice header then
                // carries it as a positive RPS entry.
                picture.codingType = 1;
                picture.inter = true;
                // The payload names the reference picture by POC; that is the
                // value the slice header and the HCP POC delta must carry. The
                // cache only supplies the surface address, and its POC can be
                // newer than the payload's when the plug-in recycles a surface
                // that a later picture already wrote.
                picture.refPoc = refPoc;
                if (logThis && refPoc != cached->poc)
                    logValue("reference poc mismatch", cached->poc);
                picture.reference.address = cached->address;
                picture.reference.valid = true;
                picture.referenceScaledSlot = cached->scaledSlot;
                picture.referenceScaledValid = true;
                // Diagnostic: suppress the reference downscale binding while
                // keeping the rest of the inter command stream. Separates the
                // tracked DS read path from the inter syntax path.
                if (experimentalEnabled("WEX_HEVC_NO_DSREF", kNoDsRefMarker))
                    picture.referenceScaledValid = false;
                if (logThis)
                    logValue("reference scaled slot", cached->scaledSlot);
                // The PAK reads the reference for motion compensation. Bind
                // it to the accelerator for this context the same way the
                // current source and recon surfaces are bound. The bind also
                // turns the plug-in's surface object into the resource handle
                // the resolve token lists; the payload pointer is not one.
                HevcSurface bound;
                if (bindPictureSurface(encoder, refSurfacePointer,
                        "reference surface", bound))
                    encoder->referenceHandle = bound.address;
                if (experimentalEnabled("WEX_HEVC_P_REFTOUCH",
                        kRefTouchMarker)) {
                    // Read-only checksum: the lock/unlock pair is the only
                    // difference against the plain inter path, so it separates
                    // "the reference contents stall the engines" from "the
                    // surface has to be prepared through the CPU mapping".
                    logValue("reference luma checksum",
                        lumaChecksum(refSurfacePointer));
                }
                if (experimentalEnabled("WEX_HEVC_P_COPYSRC",
                        kCopySourceMarker)) {
                    void *const inputPointer =
                        readPointer(payload, kPayloadInputSurface);
                    logValue("reference luma checksum before",
                        lumaChecksum(refSurfacePointer));
                    if (copySurfacePlanes(inputPointer, refSurfacePointer))
                        logValue("reference luma checksum copied",
                            lumaChecksum(refSurfacePointer));
                }
                if (experimentalEnabled("WEX_HEVC_P_REFGRAY",
                        kRefGrayMarker)) {
                    logValue("reference luma checksum before",
                        lumaChecksum(refSurfacePointer));
                    if (fillSurfacePlanes(refSurfacePointer, 0x80))
                        logValue("reference luma checksum gray",
                            lumaChecksum(refSurfacePointer));
                }
            }
        }
        fillReferenceSet(encoder, picture);
        fillBuffers(encoder, payload, picture.buffers);
        if (addressesEnabled) {
            capturePictureHandles(encoder, picture);
            applyResolvedAddresses(encoder, picture);
            picture.resolvedAddresses = true;
            if (picture.inter &&
                experimentalEnabled("WEX_HEVC_P_SELF", kSelfRefMarker)) {
                // Diagnostic only: read the current source as the reference.
                picture.reference.address = picture.source.address;
            }
            if (picture.inter && encoder->lastPictureSource &&
                experimentalEnabled("WEX_HEVC_P_PREVSRC", kPrevSourceMarker)) {
                // Diagnostic only: read the previous picture's source.
                picture.reference.address = encoder->lastPictureSource;
                logValue("reference previous source", encoder->lastPictureSource);
            }
            picture.traceCheckpoints = experimentalEnabled(
                "WEX_HEVC_CHECKPOINTS", kCheckpointsMarker);
        }
        picture.skipPak = experimentalEnabled("WEX_HEVC_NOPAK", kNoPakMarker);
        // Inter pictures only: the intra picture keeps its walker so the
        // sequence reaches the P picture at all.
        picture.skipWalker = picture.inter &&
            experimentalEnabled("WEX_HEVC_NOWALK", kNoWalkerMarker);
        picture.pakObjectStreamOut = experimentalEnabled("WEX_HEVC_OBJOUT",
            kObjOutMarker);
        picture.cuObjectAlias = experimentalEnabled("WEX_HEVC_CUREC",
            kCuRecMarker);
        picture.cuObjectAtBase = experimentalEnabled("WEX_HEVC_CUOBJ0",
            kCuObjBaseMarker);
        // Inter pictures only: the intra picture keeps its own VDEnc mode.
        picture.interIntraOnly = picture.inter &&
            experimentalEnabled("WEX_HEVC_P_INTRA", kInterIntraMarker);
        picture.interTemporalMvp = picture.inter &&
            experimentalEnabled("WEX_HEVC_P_TMVP", kInterTemporalMvpMarker);
        // Inter pictures are coded as media-driver's low-delay B form: a VDEnc
        // HEVC slice is B or I only, so a P picture has no valid slice type.
        // Both reference lists name the same picture.
        picture.lowDelay = picture.inter;
        if (picture.inter &&
            experimentalEnabled("WEX_HEVC_NOLCUMAX", kNoLcuMaxMarker))
            picture.lcuMaxBitsizeAllowed = 0;
        // media-driver asserts this is set in VDEnc mode ("VDEnc always needs
        // to enable due to pak fractional QP features"): the PAK carries a
        // fractional QP per CU, and the reference rejects a VDEnc picture with
        // the flag clear. The PPS signals the same flag, so the stream syntax
        // matches.
        picture.cuQpDeltaEnabled = true;
        const bool authenticatedBrc = gOps.hucAuthenticated && gOps.hucAuthenticated() == 1;
        if (encoder->rateInputValid && encoder->rateInput.mode && !authenticatedBrc) {
            logMessage("software bitrate request requires authenticated HuC");
            return StatusNotFound;
        }
        picture.rateControlEnabled = encoder->rateInputValid && authenticatedBrc;
        if (encoder->rateInputValid &&
            (!encoder->rateInput.mode || nativeVbrRateMode(encoder)))
            picture.qpY = encoder->rateInput.qp;
        if (picture.rateControlEnabled && nativeVbrRateMode(encoder) && picture.qpY < 10)
            picture.qpY = 10;
        if (picture.rateControlEnabled)
            picture.pakObjectStreamOut = true;
        (void)experimentalEnabled("WEX_HEVC_CUQP", kCuQpMarker);
        // Diagnostic: code an inter picture without cu_qp_delta. The flag
        // normally feeds the PAK's fractional-QP feature, which is only
        // exercised by coding units that carry coefficients; clearing it on the
        // picture that stalls tests whether that feature is the blocker.
        if (picture.inter &&
            experimentalEnabled("WEX_HEVC_P_NOCUQP", kPNoCuQpMarker))
            picture.cuQpDeltaEnabled = false;
        if (experimentalEnabled("WEX_HEVC_TU16", kTu16Marker))
            picture.maxTransformLog2 = 4;
        if (experimentalEnabled("WEX_HEVC_TU8", kTu8Marker))
            picture.maxTransformLog2 = 3;
        // FFmpeg's VAAPI HEVC setup selects the maximum legal QP depth for
        // adaptive rate control. HuC's per-CU deltas need groups as small as
        // the minimum coding block, rather than a single 64x64 CTU group.
        if (picture.rateControlEnabled && picture.cuQpDeltaEnabled &&
            picture.minCodingBlockLog2 >= 3 && picture.minCodingBlockLog2 <= 6)
            picture.diffCuQpDeltaDepth = 6 - picture.minCodingBlockLog2;
        picture.sliceEndAtFrameEnd = experimentalEnabled(
            "WEX_HEVC_SLICEEND", kSliceEndMarker);
        picture.partialFrameUpdate = experimentalEnabled(
            "WEX_HEVC_PFU", kPartialFrameMarker);
        preparePictureHeader(encoder, picture);
        if (experimentalEnabled("WEX_HEVC_POISON", kPoisonMarker)) {
            poisonScratch(encoder, kScratchPakBse, 2u * 1024u * 1024u);
            poisonScratch(encoder, kScratchBitstream, 1024);
            poisonScratch(encoder, kScratchRowStore, 1024);
            poisonScratch(encoder, kScratchCuRecord, 1u * 1024u * 1024u);
            poisonScratch(encoder, kScratchScaled4x, 1024);
            poisonScratch(encoder, kScratchScaled8x, 1024);
            poisonScratch(encoder, kScratchHcpStreamout, 1024);
            // A resolved picture uses the plug-in's pool allocations, so the
            // scratch poisoning above leaves the buffer the VDEnc and the PAK
            // actually share untouched. Fill that one too, or the extent of a
            // short P picture stays hidden under the previous I picture's
            // objects.
            if (picture.buffers.pakBse.cpuAddress)
                memset(picture.buffers.pakBse.cpuAddress, 0xCD,
                    picture.buffers.pakBse.size < 2u * 1024u * 1024u
                        ? picture.buffers.pakBse.size : 2u * 1024u * 1024u);
            if (picture.buffers.bitstream.cpuAddress && picture.buffers.bitstream.size >= 1024)
                memset(picture.buffers.bitstream.cpuAddress, 0xCD, 1024);
            // The stalled-picture dump measures how many bytes the PAK wrote;
            // a partly encoded slice leaves its prefix in this buffer.
            if (picture.buffers.bitstream.cpuAddress) {
                const uint32_t bytes = picture.buffers.bitstream.size < 4u * 1024u * 1024u
                    ? picture.buffers.bitstream.size : 4u * 1024u * 1024u;
                memset(picture.buffers.bitstream.cpuAddress, 0xCD, bytes);
            }
        }
        if (experimentalEnabled("WEX_HEVC_ZERO", kZeroMarker) &&
            picture.buffers.hcpScratch.cpuAddress) {
            memset(picture.buffers.hcpScratch.cpuAddress, 0,
                picture.buffers.hcpScratch.size);
        }
        if (!pingRequested && picture.buffers.streamIn.valid) {
            HevcCore::VdEnc::StreamInBuffer streamIn(picture.width, picture.height);
            HevcCore::VdEnc::StreamInParams streamParams;
            streamParams.intra = picture.codingType == 0;
            streamParams.targetUsage = 4;
            streamParams.numImePredictors = 0;
            HevcCore::VdEnc::fillStreamIn(streamIn, streamParams);
            if (streamIn.byteSize() <= picture.buffers.streamIn.size) {
                writeScratch(encoder, kScratchStreamIn, streamIn.bytes(),
                    streamIn.byteSize());
                if (gLog) {
                    char *out = gHevcLine;
                    append(out, "hevc stream-in bytes=");
                    appendDec(out, streamIn.byteSize());
                    append(out, " records=");
                    appendDec(out, streamIn.recordCount());
                    emit(gHevcLine, out);
                }
            } else {
                logMessage("hevc stream-in does not fit");
            }
        }
        if (submitSequence && addressesEnabled && !pingRequested &&
            (picture.rateControlEnabled || experimentalEnabled("WEX_HEVC_BRC_INIT",
                "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_BRC_INIT")) &&
            !prepareBrcInit(encoder, picture)) {
            logMessage("BRC Init unavailable");
            return StatusNotFound;
        }
        if (submitSequence && addressesEnabled && !pingRequested && picture.rateControlEnabled) {
            picture.pakObjectStreamOut = encoder->brcConfiguration.BRCFlag ||
                experimentalEnabled("WEX_HEVC_OBJOUT", kObjOutMarker);
            if (!prepareBrcUpdate(encoder, picture, 0)) {
                encoder->brcInitialized = encoder->brcActive = false;
                logMessage("BRC Update unavailable");
                return StatusNotFound;
            }
            encoder->brcActive = true;
        } else if (!picture.rateControlEnabled && encoder->brcActive) {
            encoder->brcActive = encoder->brcInitialized = false;
        }
        if (picture.brcBatchOutput) {
            // HuC already supplied both SLB groups for this picture.
        } else if (!pingRequested && experimentalEnabled("WEX_HEVC_HUC", kHucMarker))
            prepareHuc(encoder, picture);
        else if (!pingRequested && !experimentalEnabled("WEX_HEVC_INLINE", kInlineMarker))
            prepareSecondLevel(encoder, picture);
        if (addressesEnabled && !pingRequested && picture.buffers.statistics.valid &&
            picture.buffers.statistics.cpuAddress && picture.buffers.statistics.size >= 4096) {
            // Keep completion in the final cache line, clear of the frame
            // statistics at the front of the allocation and of the diagnostic
            // checkpoint trace 512 bytes back.
            const uint32_t offset = picture.buffers.statistics.size - 64;
            picture.completionAddress = picture.buffers.statistics.address + offset;
            picture.completionTag = encoder->sequenceCount + 1;
            write64(picture.buffers.statistics.cpuAddress, offset, 0);
            write64(picture.buffers.statistics.cpuAddress, offset + 8, 0);
            if (picture.traceCheckpoints) {
                // The trace buffer belongs to the allocation, not the picture:
                // clear it so the surviving markers belong to this submission.
                for (uint32_t i = 0; i < 11; ++i)
                    write32(picture.buffers.statistics.cpuAddress,
                        picture.buffers.statistics.size - 512 + i * 4, 0);
            }
        }
        commandDwords = pingRequested ? 64 : hevcPictureCommandDwords(picture);
        if (logThis || encoder->scheduleCount % 30 == 0) {
            logValue("picture width", picture.width);
            logValue("picture height", picture.height);
            logValue("picture coding type", picture.codingType);
            logValue("picture reconstruction index", picture.reconstructionIndex);
            logValue("picture nal type", picture.nalType);
            logValue("picture poc", picture.poc);
            logValue("picture inter", picture.inter ? 1 : 0);
            logValue("picture reference poc", picture.refPoc);
            logValue("sequence dwords", commandDwords);
            logValue("huc enabled", picture.huc.enabled ? 1 : 0);
            logValue("bitstream address", picture.buffers.bitstream.address);
            logValue("bitstream valid", picture.buffers.bitstream.valid ? 1 : 0);
            logValue("pakbse address", picture.buffers.pakBse.address);
            logValue("streamin address", picture.buffers.streamIn.address);
            logValue("rowstore address", picture.buffers.rowStore.address);
        }
        const bool hucReady = !picture.huc.enabled ||
            (picture.buffers.secondLevel.valid && picture.buffers.dmem.valid);
        const bool buffersReady = picture.source.valid && picture.recon.valid &&
            picture.buffers.bitstream.valid && picture.buffers.pakBse.valid &&
            picture.buffers.cuRecord.valid && picture.buffers.streamIn.valid &&
            picture.buffers.rowStore.valid && picture.buffers.scaled4x.valid &&
            picture.buffers.scaled8x.valid && picture.buffers.hcpStreamout.valid &&
            picture.buffers.sseRowstore.valid && picture.buffers.hcpScratch.valid &&
            picture.buffers.hcpScratch.size >= kHcpScratchRegionBytes * kHcpScratchRegions &&
            hucReady;
        if (submitSequence && !pingRequested && (!buffersReady || !picture.completionAddress)) {
            logMessage("hevc picture buffers incomplete; placeholder batch used");
            submitSequence = false;
        }
        if (!submitSequence) {
            auto *scratch = static_cast<uint32_t *>(calloc(commandDwords, 4));
            if (scratch) {
                const uint32_t written = buildHevcPictureCommands(picture,
                    scratch, commandDwords);
                free(scratch);
                if (logThis || encoder->scheduleCount % 30 == 0)
                    logValue("sequence built dwords", written);
            }
            commandDwords = 64;
        }
    }

    // Resource list for the kernel resolve token. The kernel returns the
    // 48-bit graphics address and size of every entry.
    HevcToken::Resource resolveResources[kPictureResourceCount] {};
    const char *resolveLabels[kPictureResourceCount] {};
    uint32_t resolveResourceCount = 0;
    if ((resolveRequested || addressesEnabled) && buildSequence) {
        auto addResource = [&](uint32_t handle, const char *label) {
            if (!handle || resolveResourceCount >= kPictureResourceCount)
                return;
            resolveResources[resolveResourceCount].handle = handle;
            resolveLabels[resolveResourceCount] = label;
            ++resolveResourceCount;
        };
        // The token carries handles; with resolved addresses the picture
        // fields already hold graphics addresses, so use the saved handles.
        auto handle = [&](uint32_t index, uint32_t current) {
            return addressesEnabled ? encoder->pictureHandle[index] : current;
        };
        addResource(handle(0, picture.source.address), "source");
        addResource(handle(1, picture.recon.address), "recon");
        addResource(handle(2, picture.buffers.bitstream.address), "bitstream");
        addResource(handle(3, picture.buffers.pakBse.address), "pakbse");
        addResource(handle(4, picture.buffers.cuRecord.address), "curecord");
        addResource(handle(5, picture.buffers.streamIn.address), "streamin");
        addResource(handle(6, picture.buffers.rowStore.address), "rowstore");
        addResource(handle(7, picture.buffers.encodedSliceSize.address), "slicesize");
        addResource(handle(8, picture.buffers.statistics.address), "statistics");
        addResource(handle(9, picture.buffers.secondLevel.address), "secondlevel");
        addResource(handle(10, picture.buffers.dmem.address), "dmem");
        addResource(handle(11, picture.buffers.aggregatedStats.address), "stats");
        addResource(handle(12, picture.buffers.brcHistory.address), "brchistory");
        addResource(handle(13, picture.buffers.brcData.address), "brcdata");
        addResource(handle(14, picture.buffers.tileRecord.address), "tilerecord");
        addResource(handle(15, picture.buffers.scaled4x.address), "scaled4x");
        addResource(handle(16, picture.buffers.scaled8x.address), "scaled8x");
        addResource(handle(17, picture.buffers.hcpStreamout.address), "hcpstreamout");
        addResource(handle(18, picture.buffers.sseRowstore.address), "sserowstore");
        addResource(handle(19, picture.source.uvAddress), "sourceuv");
        addResource(handle(20, picture.recon.uvAddress), "reconuv");
        addResource(handle(21, picture.buffers.hcpScratch.address), "hcpscratch");
        // The reference is resolved with the picture so the kernel maps it for
        // this submission, as media-driver adds every reference resource to
        // the command buffer. An I picture has no reference, so the
        // reconstruction handle fills the last slot.
        addResource(addressesEnabled
                ? encoder->pictureHandle[kReferenceResourceIndex]
                : (encoder->referenceHandle ? encoder->referenceHandle
                                            : picture.recon.address),
            "reference");
        if (resolveResourceCount == 0) {
            logMessage("resolve token has no resources");
            return StatusNotFound;
        }
        if (resolveRequested) {
            commandDwords = kResolveCommandDwords;
        } else {
            // Leave dead space after MI_BATCH_BUFFER_END for the kernel's
            // address patches; the token anchors point there.
            commandDwords += kResolveDeadDwords;
        }
    }
    ++encoder->sequenceCount;

    // Reuse the renderer's IOAccelVideoContext data-buffer path: it allocates
    // the command and sideband buffers, submits them and waits on the fence.
    const bool report = logThis || encoder->scheduleCount % 30 == 0;
    int32_t status = StatusInvalid;
    uint32_t fence = 0;
    uint32_t *resolveToken = nullptr;
    char *resolveBatch = nullptr;
    // The renderer reserves a3 + 3 dwords for tokens the caller writes. The
    // submit path appends its own 3-dword token after the caller's tokens, so
    // the reserve must cover the probe token plus those three dwords.
    const uint32_t sidebandDwords = (resolveRequested || addressesEnabled)
        ? HevcToken::kTotalDwords : 8;
    // The renderer returns 4 while a fresh data buffer is being prepared; the
    // next request picks another buffer and succeeds. Retry once so a frame is
    // not lost on that transition.
    // Resolve on the same context that will execute the picture: each engine
    // has its own address space, so addresses resolved on one context need not
    // be mapped on another.
    void *submitTarget = encoder->submitContext;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        void *descriptor = nullptr;
        resolveToken = nullptr;
        status = gOps.request(submitTarget, commandDwords,
            sidebandDwords, &descriptor);
        if (status != StatusSuccess || !descriptor) {
            logValue("submit request status", static_cast<uint32_t>(status));
            break;
        }
        auto *batch = *reinterpret_cast<uint32_t **>(
            static_cast<char *>(descriptor) + kVideoCommandStart);
        if (!batch) {
            logMessage("submit buffer missing command pointer");
            break;
        }
        if (resolveRequested) {
            for (unsigned i = 0; i < commandDwords; ++i)
                batch[i] = 0;
            batch[0] = kPlaceholderBatchEnd;
            auto *sideband = static_cast<uint32_t *>(
                readPointer(descriptor, kVideoSidebandStart));
            auto *base = static_cast<uint32_t *>(
                readPointer(descriptor, kVideoCommandBase));
            const uint32_t offset = base && batch > base
                ? static_cast<uint32_t>(batch - base) : 0;
            const uint32_t written = sideband
                ? HevcToken::buildResolveToken(sideband,
                    HevcToken::kTotalDwords, resolveResources,
                    resolveResourceCount, offset)
                : 0;
            if (written == 0) {
                logMessage("resolve token does not fit");
                status = StatusInvalid;
                break;
            }
            // The renderer expects the caller to advance the sideband cursor;
            // submit writes its own token at the new position.
            write64(descriptor, kVideoSidebandStart,
                reinterpret_cast<uint64_t>(sideband + written));
            resolveToken = sideband;
            resolveBatch = reinterpret_cast<char *>(batch);
            if (report) {
                logValue("resolve token dwords", written);
                logValue("resolve resources", resolveResourceCount);
                logValue("resolve command offset", offset);
            }
        } else if (submitSequence) {
            uint32_t written = 0;
            if (pingRequested) {
                // Sentinel-only batch. The store targets the statistics buffer
                // and precedes MI_BATCH_BUFFER_END, so a landed write proves
                // the engine executed the submitted batch.
                const uint32_t store[5] = {0x10000003,
                    picture.buffers.statistics.address & ~3u, 0, 0xDEADBEEF, 0};
                for (uint32_t i = 0; i < 5; ++i)
                    batch[i] = store[i];
                batch[5] = 0;
                batch[6] = 0;
                batch[7] = kPlaceholderBatchEnd;
                written = 8;
            } else {
                written = buildHevcPictureCommands(picture, batch,
                    commandDwords);
                if (written == 0) {
                    logMessage("hevc command sequence does not fit");
                    status = StatusInvalid;
                    break;
                }
                if (experimentalEnabled("WEX_HEVC_DUMP", kDumpMarker) && gLog) {
                    // Print the submitted command stream so the field values
                    // can be compared with the driver builders directly.
                    const uint32_t limit = written < 1600 ? written : 1600;
                    for (uint32_t base = 0; base < limit; base += 8) {
                        char *out = gHevcLine;
                        append(out, "batch dw");
                        appendDec(out, base);
                        for (uint32_t i = 0; i < 8 && base + i < limit; ++i) {
                            *out++ = ' ';
                            appendHex(out, batch[base + i]);
                        }
                        emit(gHevcLine, out);
                    }
                }
                if (experimentalEnabled("WEX_HEVC_DUMP", kDumpMarker)) {
                    // Sentinel: prove the batch reached the engine. The store
                    // writes into the statistics buffer before the media commands.
                    const uint32_t store[5] = {0x10000003,
                        picture.buffers.statistics.address & ~3u, 0, 0xDEADBEEF, 0};
                    if (written + 5 <= commandDwords) {
                        for (uint32_t i = written; i > 0; --i)
                            batch[i - 1 + 5] = batch[i - 1];
                        for (uint32_t i = 0; i < 5; ++i)
                            batch[i] = store[i];
                        written += 5;
                    }
                }
            }
            if (report)
                logValue("sequence written dwords", written);
            if (addressesEnabled) {
                // The media submission path expects a VDENC token. Its
                // anchors point past MI_BATCH_BUFFER_END: the kernel patches
                // dead dwords instead of overwriting the real command fields,
                // which already carry resolved addresses.
                auto *sideband = static_cast<uint32_t *>(
                    readPointer(descriptor, kVideoSidebandStart));
                auto *base = static_cast<uint32_t *>(
                    readPointer(descriptor, kVideoCommandBase));
                const uint32_t offset = base && batch > base
                    ? static_cast<uint32_t>(batch - base) : 0;
                const uint32_t anchor = written + 16;
                const uint32_t tokenDwords = sideband
                    ? HevcToken::buildResolveToken(sideband,
                        HevcToken::kTotalDwords, resolveResources,
                        resolveResourceCount, offset, anchor)
                    : 0;
                if (tokenDwords == 0) {
                    logMessage("picture token does not fit");
                    status = StatusInvalid;
                    break;
                }
                write64(descriptor, kVideoSidebandStart,
                    reinterpret_cast<uint64_t>(sideband + tokenDwords));
                resolveToken = sideband;
                if (report) {
                    logValue("picture token dwords", tokenDwords);
                    logValue("picture token anchor", anchor);
                    logValue("picture token offset", offset);
                    logValue("ping batch", pingRequested ? 1 : 0);
                }
            }
        } else {
            for (unsigned i = 0; i < commandDwords; ++i)
                batch[i] = 0;
            batch[0] = kPlaceholderBatchEnd;
        }
        if (report) {
            logPointer("submit descriptor", descriptor);
            logPointer("submit batch", batch);
        }
        status = gOps.submit(submitTarget, &fence);
        // Status 7 is the early return before the renderer unlocks the data
        // buffer; release it so the next request can proceed.
        if (status == 7)
            pthread_mutex_unlock(static_cast<char *>(submitTarget) + kVideoMutex);
        if (status != 4)
            break;
        if (report)
            logMessage("submit retry after status 4");
    }
    if (status != StatusSuccess) {
        if (report)
            logValue("submit status", static_cast<uint32_t>(status));
        return StatusNotFound;
    }
    status = gOps.wait(submitTarget, fence);
    encoder->lastFence = fence;
    if (status == StatusSuccess && resolveRequested && resolveToken) {
        ++encoder->resolveCount;
        uint32_t resolved = 0;
        for (uint32_t i = 0; i < resolveResourceCount; ++i) {
            HevcToken::Resource resource {};
            uint64_t address = 0;
            uint32_t size = 0;
            if (!HevcToken::readResolvedResource(resolveToken,
                    resolveResourceCount, i, &resource, &address, &size))
                continue;
            encoder->resolvedAddress[i] = address;
            encoder->resolvedSize[i] = size;
            ++resolved;
            if (!gLog)
                continue;
            char *out = gHevcLine;
            append(out, "resolve ");
            append(out, resolveLabels[i] ? resolveLabels[i] : "?");
            append(out, " handle=");
            appendDec(out, resource.handle);
            append(out, " address=");
            appendHex(out, address);
            append(out, " size=");
            appendDec(out, size);
            emit(gHevcLine, out);
        }
        encoder->resolvedCount = resolved;
        if (resolveResourceCount > 0)
            encoder->resolvedSourceHandle = resolveResources[0].handle;
        if (resolveResourceCount == kPictureResourceCount)
            encoder->resolvedReferenceHandle =
                resolveResources[kReferenceResourceIndex].handle;
        if (resolved != resolveResourceCount)
            logValue("resolve incomplete", resolveResourceCount - resolved);
        // The kernel also patches the reference slots in the batch; print the
        // first few so a token that resolved but did not reach the sideband
        // copy is distinguishable from one that was never processed.
        for (uint32_t i = 0; i < 4 && resolveBatch; ++i) {
            uint64_t value = 0;
            memcpy(&value, resolveBatch + 148 + 8 * i, sizeof(value));
            if (!gLog)
                break;
            char *out = gHevcLine;
            append(out, "resolve slot ");
            appendDec(out, i);
            append(out, "=");
            appendHex(out, value);
            emit(gHevcLine, out);
        }
        if (submitRequested) {
            if (resolved != kPictureResourceCount)
                return StatusNotFound;
            // Address resolution is preparation for this picture, not a
            // completed frame. Encode the same payload before returning.
            return schedulePicture(context, command, refCon, false);
        }
    }
    if (status == StatusSuccess && submitSequence) {
        // The hardware writes the encoded slice length into the slice-size
        // buffer. Reading it back here gives the frame size the plug-in will
        // export, without touching the bitstream itself.
        const void *sliceSize = scratchBase(encoder, kScratchSliceSize);
        logValue("encoded slice bytes", sliceSize ? read32(sliceSize, 0) : 0);
        if (picture.probeHucStatus) {
            const void *status = scratchBase(encoder, kScratchStatistics);
            const uint32_t at = picture.buffers.statistics.size - 128 + 44;
            logValue("GuC status", status ? read32(status, at - 16) : 0);
            logValue("GuC WOPCM size", status ? read32(status, at - 12) : 0);
            logValue("GuC WOPCM offset", status ? read32(status, at - 8) : 0);
            logValue("BIOS WOPCM", status ? read32(status, at - 4) : 0);
            logValue("HuC status", status ? read32(status, at) : 0);
            logValue("HuC status2", status ? read32(status, at + 4) : 0);
            if (picture.probeHucKernel < 256)
                logValue("HuC status2 after imem", status ? read32(status, at + 8) : 0);
            logValue("HuC load info", status ? read32(status, at + 12) : 0);
        }
        if (experimentalEnabled("WEX_HEVC_DUMP", kDumpMarker)) {
            dumpScratchWords(encoder, "slicesize", kScratchSliceSize, 6);
            dumpScratchWords(encoder, "statistics", kScratchStatistics, 30);
            if (picture.traceCheckpoints) {
                // Each group stores its number into its own dword, so the
                // surviving values show the last group the engine reached.
                // The trace shares the buffer tail with the completion tag.
                const auto *base = static_cast<const uint32_t *>(
                    scratchBase(encoder, kScratchStatistics));
                const uint32_t traceBase = picture.buffers.statistics.size - 512;
                char *out = gHevcLine;
                append(out, "post checkpoints");
                for (uint32_t i = 0; i < 10; ++i) {
                    *out++ = ' ';
                    appendHex(out, base ? read32(base, traceBase + i * 4) : 0);
                }
                emit(gHevcLine, out);
            }
            if (picture.inter && picture.traceCheckpoints) {
                // Interleaved reference/source samples copied by the engine
                // before the media commands ran.
                const auto *base = static_cast<const uint32_t *>(
                    scratchBase(encoder, kScratchStatistics));
                const uint32_t at = picture.buffers.statistics.size - 256;
                char *out = gHevcLine;
                append(out, "post reference probe");
                for (uint32_t i = 0; i < 12; ++i) {
                    *out++ = ' ';
                    appendHex(out, base ? read32(base, at + i * 4) : 0);
                }
                emit(gHevcLine, out);
            }
            dumpScratchWords(encoder, "rowstore", kScratchRowStore, 16);
            dumpScratchWords(encoder, "curecord", kScratchCuRecord, 16);
            dumpScratchWords(encoder, "pakbse", kScratchPakBse, 24);
            if (picture.pakObjectStreamOut)
                dumpPicturePakObjectStream(picture);
            // The encoder usually writes to the plug-in's pool, which is a
            // different allocation from our fallback bitstream scratch.
            dumpBufferWords("bitstream", picture.buffers.bitstream.cpuAddress, 16);
            // The parameter sets occupy the first 61 bytes; the slice header
            // follows them, so this window shows which header the PAK wrote.
            dumpBufferWords("slices", picture.buffers.bitstream.cpuAddress, 16, 14);
            dumpWrittenTail("bitstream", picture.buffers.bitstream.cpuAddress,
                picture.buffers.bitstream.size, 0x1000000);
            if (picture.buffers.statistics.cpuAddress &&
                picture.buffers.statistics.size >= 44 * 4) {
                // Post-walker MMIO samples, written into the tail of the
                // statistics buffer so the VDEnc statistics cannot clobber
                // them.
                logValue("pak byte count",
                    read32(picture.buffers.statistics.cpuAddress,
                        picture.buffers.statistics.size - 128));
                logValue("pak qp count",
                    read32(picture.buffers.statistics.cpuAddress,
                        picture.buffers.statistics.size - 124));
            }
            dumpScratchWords(encoder, "scaled4x", kScratchScaled4x, 8);
            dumpScratchWords(encoder, "scaled8x", kScratchScaled8x, 8);
            dumpScratchWords(encoder, "hcpstreamout", kScratchHcpStreamout, 16);
            dumpScratchWords(encoder, "secondlevel", kScratchSecondLevel, 6);
            dumpScratchWords(encoder, "dmem", kScratchDmem, 6);
            // Stream-out extent per buffer: the VDEnc CU records, the VDEnc
            // statistics, the PAK object buffer and the bitstream.
            dumpEngineBuffer(encoder, "curecord", kScratchCuRecord, 0x40000u);
            dumpEngineBuffer(encoder, "pakbse", kScratchPakBse, 0x40000u);
            dumpEngineBuffer(encoder, "bitstreamscratch", kScratchBitstream, 0x1000);
            dumpScratchWords(encoder, "stats16", kScratchStatistics, 16);
        }
        // The PAK object stream is a POISON/OBJOUT diagnostic; expose it
        // without the full dump set when only the per-CU field diff is needed.
        if (picture.pakObjectStreamOut &&
            experimentalEnabled("WEX_HEVC_OBJRAW", kObjRawMarker) &&
            !experimentalEnabled("WEX_HEVC_DUMP", kDumpMarker))
            dumpPicturePakObjectStream(picture);
    }
    if (submitSequence && !pingRequested) {
        // A stalled hand-over leaves the object stream in memory. Dump it from
        // the failed submission as well, so the extent shows how far the VDEnc
        // got before the ring stopped.
        if (status != StatusSuccess &&
            experimentalEnabled("WEX_HEVC_POISON", kPoisonMarker)) {
            if (picture.pakObjectStreamOut)
                dumpPicturePakObjectStream(picture);
            // The VDEnc writes its CU records into the MbCode surface after
            // the PAK object block, and the separate CU-record buffer when the
            // indirect-CU field points there. Both extents show how far the
            // VDEnc walked before the ring stopped.
            reportBufferExtentAt("pakbse", picture.buffers.pakBse.cpuAddress,
                picture.buffers.pakBse.size);
            reportBufferExtentAt("curecord", picture.buffers.cuRecord.cpuAddress,
                picture.buffers.cuRecord.size);
        }
        if (status != StatusSuccess && gLog) {
            logValue("stall diag",
                (picture.buffers.statistics.cpuAddress ? 1u : 0u) |
                    (picture.traceCheckpoints ? 2u : 0u) |
                    (picture.buffers.bitstream.cpuAddress ? 4u : 0u));
            logValue("stall statistics size", picture.buffers.statistics.size);
        }
        const void *completion = picture.buffers.statistics.cpuAddress
            ? static_cast<const char *>(picture.buffers.statistics.cpuAddress) +
                picture.buffers.statistics.size - 64 : nullptr;
        const bool complete =
            checkPictureCompletion(encoder, status, completion, picture.completionTag);
        if (!complete) {
            // A submission that never completes still leaves the ring progress
            // in memory: the checkpoint stores that ran before the stalled
            // command, the PAK header bytes it consumed, and the VDEnc frame
            // statistics. Log them so a stall can be placed on the command
            // that blocked. The submit pass does not repeat the schedule-time
            // marker setup, so this dump does not depend on the marker.
            if (gLog && picture.buffers.statistics.cpuAddress &&
                picture.buffers.statistics.size >= 512) {
                const auto *trace = static_cast<const uint32_t *>(
                    picture.buffers.statistics.cpuAddress);
                const uint32_t traceBase =
                    picture.buffers.statistics.size - 512;
                char *out = gHevcLine;
                append(out, "stalled checkpoints");
                for (uint32_t i = 0; i < 11; ++i) {
                    *out++ = ' ';
                    appendHex(out, read32(trace, traceBase + i * 4));
                }
                emit(gHevcLine, out);
                dumpBufferWords("stalled bitstream",
                    picture.buffers.bitstream.cpuAddress, 8);
                dumpBufferWords("stalled slices",
                    picture.buffers.bitstream.cpuAddress, 12, 14);
                dumpBufferWords("stalled statistics", trace, 8);
                // Live HCP/engine register samples taken between the per-slice
                // flush and the picture-level flush: frame byte count, image
                // status, slice count, HCP command counter, CABAC status, SE
                // bit count, VCS0 ring head and ring start. Together they say
                // whether the PAK started the slice and how far it got.
                char *status = gHevcLine;
                append(status, "stalled pak status");
                for (uint32_t i = 0; i < 11; ++i) {
                    *status++ = ' ';
                    appendHex(status,
                        read32(trace, picture.buffers.statistics.size - 128 + i * 4));
                }
                emit(gHevcLine, status);
                logValue("stalled slice size",
                    scratchBase(encoder, kScratchSliceSize)
                        ? read32(scratchBase(encoder, kScratchSliceSize), 0) : 0);
                // Extent of the PAK output: POISON fills the buffer with 0xCD,
                // so the last byte the PAK wrote is the point it stopped.
                const auto *encoded = static_cast<const uint8_t *>(
                    picture.buffers.bitstream.cpuAddress);
                if (encoded) {
                    const uint32_t count = picture.buffers.bitstream.size
                        < 4u * 1024u * 1024u ? picture.buffers.bitstream.size
                                             : 4u * 1024u * 1024u;
                    uint32_t last = 0;
                    for (uint32_t i = 0; i < count; ++i)
                        if (encoded[i] != 0xCD)
                            last = i + 1;
                    logValue("stalled bitstream bytes", last);
                }
                // Engine-written buffers: the PAK object block, the CU record
                // area and the separate CU-record streamout target.
                reportBufferExtentAt("pakbse", picture.buffers.pakBse.cpuAddress,
                    picture.buffers.pakBse.size);
                reportBufferExtentAt("curecord", picture.buffers.cuRecord.cpuAddress,
                    picture.buffers.cuRecord.size);
                if (picture.pakObjectStreamOut)
                    dumpPicturePakObjectStream(picture);
            }
            status = StatusNotFound;
        }
        else {
            if (picture.brcBatchOutput && !finishBrcPicture(encoder, picture, fence)) {
                logMessage("BRC second pass failed");
                return StatusNotFound;
            }
            logValue("frame bytes", read32(completion, 8));
            logValue("image status", read32(completion, 12));
            if (picture.traceCheckpoints && picture.buffers.statistics.cpuAddress &&
                picture.buffers.statistics.size >= 128) {
                // Same register set the stall dump prints, so a completing and
                // a stalling inter picture can be compared field by field.
                const auto *trace = static_cast<const uint32_t *>(
                    picture.buffers.statistics.cpuAddress);
                char *status = gHevcLine;
                append(status, "pak status");
                for (uint32_t i = 0; i < 11; ++i) {
                    *status++ = ' ';
                    appendHex(status,
                        read32(trace, picture.buffers.statistics.size - 128 + i * 4));
                }
                emit(gHevcLine, status);
            }
            if (addressesEnabled) {
                // Only a completed picture left a valid reconstruction in the
                // surface, so the reference cache is updated here.
                if (picture.brcBatchOutput && !rememberBrcPictureData(encoder,
                        picture.poc, encoder->brcLayout.update.pictureDataOffset)) {
                    encoder->submissionFailed = true;
                    return StatusNotFound;
                }
                rememberRecon(encoder,
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                        encoder->payloadReconSurface)),
                    picture.poc,
                    static_cast<uint32_t>(encoder->resolvedAddress[1]),
                    picture.scaledSlot);
                encoder->scaledSlot = (picture.scaledSlot + 1) % kScaledPoolSlots;
                encoder->lastPictureSource =
                    static_cast<uint32_t>(encoder->resolvedAddress[0]);
                rememberPoc(encoder, picture.poc);
            }
            if (picture.traceCheckpoints && picture.resolvedAddresses && picture.poc == 0) {
                const uint32_t rows[] = {0, 1, 16, 31, 32, 63, 64, 90, 127, 128,
                    192, 384, 540, 768, 1024, 1072, 1079};
                for (uint32_t group = 0; group < 3; ++group) {
                    for (uint32_t tiled = 0; tiled < 2; ++tiled) {
                        for (uint32_t r = 0; r < 17; ++r) {
                            char *out = gHevcLine;
                            append(out, "gpu sample ");
                            append(out, group == 0 ? "source " : group == 1 ? "before " : "after ");
                            append(out, tiled ? "tileY " : "linear ");
                            appendDec(out, rows[r]);
                            for (uint32_t c = 0; c < 8; ++c) {
                                *out++ = ' ';
                                const uint32_t sample = group * 272 + tiled * 136 + r * 8 + c;
                                appendHex(out, read32(picture.buffers.statistics.cpuAddress,
                                    picture.buffers.statistics.size - 4096 + sample * 4));
                            }
                            emit(gHevcLine, out);
                        }
                    }
                }
            }
            const uint32_t bytes = read32(completion, 8);
            if (!publishPictureResult(encoder, command->payload, refCon, bytes,
                    picture.buffers.bitstream.size, fence)) {
                encoder->submissionFailed = true;
                return StatusNotFound;
            }
            if (picture.rateControlEnabled && vbrRateMode(encoder) &&
                !encoder->vbrBudget.commit(bytes * 8)) {
                encoder->submissionFailed = true;
                return StatusNotFound;
            }
            // The plug-in's last-submission wait is released by the media
            // completion interrupt in the native driver. This path waits for
            // the GPU fence itself, so publish the same completion here.
            if (gCompletionSignal)
                gCompletionSignal();
            if (encoder->realSubmits == 0 &&
                experimentalEnabled("WEX_HEVC_DUMP", kDumpMarker)) {
                dumpEncodedPicture(picture);
                dumpSurfaceSamples("source",
                    readPointer(payload, kPayloadInputSurface));
                dumpSurfaceSamples("recon",
                    readPointer(payload, kPayloadReconSurface));
            }
        }
    }
    dumpSideband(encoder);
    if (encoder->surfacesLogged &&
        experimentalEnabled("WEX_HEVC_DUMP", kDumpMarker) && gOps.surfaceAddress) {
        uint32_t address = 0;
        uint32_t value = 0;
        void *input = readPointer(payload, kPayloadInputSurface);
        if (input && gOps.surfaceAddress(input, encoder->surfaceManager, 0,
                &address, &value)) {
            char *out = gHevcLine;
            append(out, "dump rebind input=");
            appendHex(out, address);
            append(out, " value=");
            appendHex(out, value);
            emit(gHevcLine, out);
        }
    }
    if (report) {
        logValue("submit fence", fence);
        logValue("submit wait status", static_cast<uint32_t>(status));
    }

    if (submitSequence && status == StatusSuccess) {
        ++encoder->realSubmits;
        if (picture.brcBatchOutput)
            ++encoder->brcFrameId;
        if (logThis || encoder->scheduleCount % 30 == 0)
            logMessage("hevc picture command sequence submitted");
    } else if (!submitSequence && (logThis || encoder->scheduleCount % 30 == 0)) {
        logMessage("schedule placeholder batch submitted");
    }
    return status == StatusSuccess ? StatusSuccess : StatusNotFound;
}

int32_t hevcSchedule(void *context, const CommandNode *command, void *refCon) {
    const int32_t status = schedulePicture(context, command, refCon, true);
    if (status != StatusSuccess) {
        // Once BRC work has begun, a failed init/update, batch validation or
        // picture submission leaves history unfit for another submission.
        // A preflight rejection that issued no HuC work can still use CQP.
        auto *encoder = lookup(context);
        if (encoder && encoder->brcSubmissionCount)
            encoder->submissionFailed = true;
    }
    return status;
}

void destroyHevcEncodeContexts() {
    for (uint32_t i = 0; i < kMaxContexts; ++i) {
        if (!gContexts[i])
            continue;
        free(gContexts[i]->parameterSets);
        free(gContexts[i]->sliceHeader);
        for (uint32_t j = 0; j < kScratchCount; ++j) {
            LinearResource &linear = gContexts[i]->linear[j];
            if (linear.resource && gOps.resourceRelease)
                gOps.resourceRelease(linear.resource);
            if (linear.base) {
                vm_deallocate(mach_task_self_,
                    reinterpret_cast<unsigned long>(linear.base),
                    (linear.size + 0xFFFu) & ~0xFFFu);
            }
        }
        free(gContexts[i]);
        gContexts[i] = nullptr;
    }
}

}
