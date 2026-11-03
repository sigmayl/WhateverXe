// SPDX-License-Identifier: MIT
#include "guc_boot.hpp"

namespace HevcCore {
namespace Uc {
namespace {
constexpr uint32_t pageAlign(uint32_t n) { return (n + 0xfff) & ~0xfffu; }
constexpr uint32_t kNullCaptureOffset = pageAlign(sizeof(GucAdsBlob));
constexpr uint32_t kPrivateDataOffset = kNullCaptureOffset + 4096;
bool validMemory(const void *memory, uint32_t bytes) {
    return memory && bytes <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(memory);
}
void clear(void *memory, uint32_t bytes) {
    auto *p = static_cast<uint8_t *>(memory);
    for (uint32_t i = 0; i < bytes; ++i) p[i] = 0;
}
void put32(uint8_t *memory, uint32_t offset, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) memory[offset + i] = uint8_t(value >> (8 * i));
}
bool disjoint(uint32_t a, uint32_t aBytes, uint32_t b, uint32_t bBytes) {
    return uint64_t(a) + aBytes <= b || uint64_t(b) + bBytes <= a;
}
bool validImage(const FirmwareInfo &image) {
    return image.version.major && image.uploadBytes >= kCssBytes &&
        image.uploadBytes < kGen12WopcmBytes && !(image.uploadBytes & 3) &&
        image.rsaOffset == image.uploadBytes && image.rsaBytes == kRsaBytes;
}
}

bool planGucAuthenticationAds(uint32_t privateDataBytes, GucAdsLayout &out) {
    const uint64_t roundedPrivate = (uint64_t(privateDataBytes) + 4095) & ~uint64_t(4095);
    const uint64_t total = kPrivateDataOffset + roundedPrivate;
    if (!privateDataBytes || total > kGucGgttTop - kGen12WopcmBytes)
        return false;
    GucAdsLayout layout;
    layout.bytes = uint32_t(total);
    layout.nullCaptureOffset = kNullCaptureOffset;
    layout.privateDataOffset = kPrivateDataOffset;
    layout.privateDataBytes = uint32_t(roundedPrivate);
    out = layout;
    return true;
}

bool buildGen12GucGtInfo(uint32_t sliceEnable, uint32_t doorbellsPopulated,
    uint32_t mediaDisable, uint8_t supportedVdboxMask, GucGen12GtInfo &out) {
    if (sliceEnable == 0xffffffffu || doorbellsPopulated == 0xffffffffu ||
        mediaDisable == 0xffffffffu || (sliceEnable & 0xff) != 1 || !supportedVdboxMask)
        return false;
    const uint32_t vdbox = ~mediaDisable & supportedVdboxMask;
    if (!vdbox) return false;
    GucGen12GtInfo info;
    info.sliceCount = 1;
    info.doorbellsPerSqidi = ((doorbellsPopulated >> 16) & 0xff) + 1;
    for (uint32_t i = 0; i < 8; ++i)
        if ((vdbox & (1u << i)) && (!(i & 1) || !(vdbox & (1u << (i - 1)))))
            info.vdboxSfcMask |= 1u << i;
    out = info;
    return true;
}

bool initializeGucAuthenticationAds(void *memory, uint32_t bytes,
    uint32_t ggttAddress, uint32_t privateDataBytes, const GucGen12GtInfo &gt,
    GucAdsLayout &out) {
    GucAdsLayout layout;
    if (!planGucAuthenticationAds(privateDataBytes, layout) || bytes < layout.bytes ||
        !validMemory(memory, layout.bytes) ||
        !validGucGgttRange(ggttAddress, layout.bytes, 4096) || gt.sliceCount != 1 ||
        !gt.doorbellsPerSqidi || gt.doorbellsPerSqidi > 256 || gt.vdboxSfcMask > 0xff)
        return false;
    clear(memory, layout.bytes);
    auto *p = static_cast<uint8_t *>(memory);
    put32(p, offsetof(GucAds, schedulerPolicies), ggttAddress + offsetof(GucAdsBlob, policies));
    put32(p, offsetof(GucAds, gtSystemInfo), ggttAddress + offsetof(GucAdsBlob, systemInfo));
    put32(p, offsetof(GucAds, privateData), ggttAddress + layout.privateDataOffset);
    constexpr uint32_t policyOffset = offsetof(GucAdsBlob, policies);
    put32(p, policyOffset + offsetof(GucPolicies, dpcPromoteTime), 500000);
    put32(p, policyOffset + offsetof(GucPolicies, valid), 1);
    put32(p, policyOffset + offsetof(GucPolicies, maxWorkItems), 15);
    put32(p, policyOffset + offsetof(GucPolicies, flags), 1); // disable GuC engine reset
    constexpr uint32_t mappingOffset = offsetof(GucAdsBlob, systemInfo) +
        offsetof(GucSystemInfo, mapping);
    for (uint32_t i = 0; i < kGucEngineClasses * kGucEngineInstances; ++i)
        p[mappingOffset + i] = kGucEngineInstances; // invalid / not handed to GuC
    constexpr uint32_t genericOffset = offsetof(GucAdsBlob, systemInfo) +
        offsetof(GucSystemInfo, generic);
    put32(p, genericOffset, gt.sliceCount);
    put32(p, genericOffset + 4, gt.vdboxSfcMask);
    put32(p, genericOffset + 8, gt.doorbellsPerSqidi);
    for (uint32_t i = 0; i < 2 * kGucEngineClasses; ++i) {
        put32(p, offsetof(GucAds, captureInstance) + i * 4, ggttAddress + layout.nullCaptureOffset);
        put32(p, offsetof(GucAds, captureClass) + i * 4, ggttAddress + layout.nullCaptureOffset);
    }
    for (uint32_t i = 0; i < 2; ++i)
        put32(p, offsetof(GucAds, captureGlobal) + i * 4, ggttAddress + layout.nullCaptureOffset);
    out = layout;
    return true;
}

bool initializeGucAuthenticationLog(void *memory, uint32_t bytes) {
    if (bytes < kGucAuthenticationLogBytes || !validMemory(memory, kGucAuthenticationLogBytes))
        return false;
    clear(memory, kGucAuthenticationLogBytes);
    return true;
}

bool buildGen12Guc70AuthenticationParameters(const FirmwareInfo &guc,
    uint32_t physicalDeviceId, uint32_t revision, uint32_t adsAddress,
    uint32_t logAddress, GucBootParameters &out) {
    GucAdsLayout ads;
    if (!validImage(guc) || guc.version.major != 70 || guc.version.minor < 7 ||
        !physicalDeviceId || physicalDeviceId >= 0xffff || revision > 0xff ||
        !planGucAuthenticationAds(guc.privateDataBytes, ads) ||
        !validGucGgttRange(adsAddress, ads.bytes, 4096) ||
        !validGucGgttRange(logAddress, kGucAuthenticationLogBytes, 4096) ||
        !disjoint(adsAddress, ads.bytes, logAddress, kGucAuthenticationLogBytes))
        return false;
    GucBootParameters params;
    // valid + 1-MiB capture units + one extra crash unit + 15 extra debug units
    params.words[0] = logAddress | 1u | (1u << 2) | (1u << 4) | (15u << 6);
    params.words[1] = (1u << 14) | (1u << 18) | (1u << 22);
    params.words[2] = 1u << 14; // retain the host scheduler, no SLPC/PXP
    params.words[3] = 1u << 6; // disable verbose logs
    params.words[4] = (adsAddress >> 12) << 1;
    params.words[5] = (physicalDeviceId << 16) | revision;
    out = params;
    return true;
}

bool buildGen12FirmwareDma(FirmwareTarget target, const FirmwareInfo &firmware,
    uint32_t sourceAddress, const WopcmLayout &wopcm, FirmwareDmaPlan &out) {
    if ((target != FirmwareTarget::GuC && target != FirmwareTarget::HuC) ||
        !validImage(firmware) ||
        !validGucGgttRange(sourceAddress, firmware.uploadBytes + kRsaBytes, 4096) ||
        (wopcm.gucBase & 0x3fff) || (wopcm.gucBytes & 0xfff) ||
        wopcm.offsetRegister != (wopcm.gucBase | 3u) ||
        wopcm.sizeRegister != (wopcm.gucBytes | 1u) ||
        uint64_t(wopcm.gucBase) + wopcm.gucBytes > kGen12WopcmBytes - 36 * 1024)
        return false;
    const bool huc = target == FirmwareTarget::HuC;
    if (huc ? uint64_t(firmware.uploadBytes) + 16 * 1024 > wopcm.gucBase :
              uint64_t(firmware.uploadBytes) + 24 * 1024 > wopcm.gucBytes)
        return false;
    FirmwareDmaPlan plan;
    const uint32_t flags = huc ? 1u << 9 : 1u << 4;
    plan.sourceAddress = sourceAddress;
    plan.destinationOffset = huc ? 0 : 0x2000;
    plan.copyBytes = firmware.uploadBytes;
    plan.destinationSpace = 7u << 16; // WOPCM, interpreted with target DMA flag
    plan.startControl = ((flags | 1u) << 16) | flags | 1u;
    plan.finishControl = flags << 16;
    out = plan;
    return true;
}
}
}
