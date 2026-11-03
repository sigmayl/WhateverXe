// SPDX-License-Identifier: MIT
// Copyright (c) 2014-2021 Intel Corporation
// GuC 70 ADS, logging and boot parameters from Linux i915 intel_guc_fwif.h,
// intel_guc_ads.c, intel_guc_log.c and intel_guc.c. The Intel MIT notice is
// included in uc_firmware.hpp. No firmware payload or native driver objects.
#ifndef WEX_MEDIA_HEVC_CORE_GUC_BOOT_HPP
#define WEX_MEDIA_HEVC_CORE_GUC_BOOT_HPP
#include "uc_firmware.hpp"

namespace HevcCore {
namespace Uc {
constexpr uint32_t kGucEngineClasses = 16, kGucEngineInstances = 32;
constexpr uint32_t kGucBootDwords = 14;

struct GucRegisterSet {
    uint32_t address;
    uint16_t count, reserved;
};
struct GucAds {
    GucRegisterSet registerSets[kGucEngineClasses][kGucEngineInstances];
    uint32_t reserved0, schedulerPolicies, gtSystemInfo, reserved1, controlData;
    uint32_t goldenContext[kGucEngineClasses], engineStateBytes[kGucEngineClasses];
    uint32_t privateData, reserved2;
    uint32_t captureInstance[2][kGucEngineClasses], captureClass[2][kGucEngineClasses];
    uint32_t captureGlobal[2], workaroundAddressLow, workaroundAddressHigh, workaroundBytes;
    uint32_t reserved[11];
};
struct GucPolicies {
    uint32_t queueDepth[kGucEngineClasses];
    uint32_t dpcPromoteTime, valid, maxWorkItems, flags, reserved[4];
};
struct GucSystemInfo {
    uint8_t mapping[kGucEngineClasses][kGucEngineInstances];
    uint32_t enabledMasks[kGucEngineClasses], generic[16];
};
struct GucEngineUsageRecord {
    uint32_t contextIndex, lastSwitchStamp, reserved0, totalRuntime, reserved[4];
};
struct GucAdsBlob {
    GucAds ads;
    GucPolicies policies;
    GucSystemInfo systemInfo;
    GucEngineUsageRecord usage[kGucEngineClasses][kGucEngineInstances];
};
static_assert(sizeof(GucRegisterSet) == 8 && sizeof(GucAds) == 0x11dc, "GuC 70 ADS ABI");
static_assert(offsetof(GucAds, schedulerPolicies) == 0x1004 &&
    offsetof(GucAds, gtSystemInfo) == 0x1008 &&
    offsetof(GucAds, privateData) == 0x1094 &&
    offsetof(GucAds, workaroundAddressLow) == 0x11a4, "GuC 70 ADS pointer offsets");
static_assert(sizeof(GucPolicies) == 0x60 && sizeof(GucSystemInfo) == 0x280 &&
    sizeof(GucAdsBlob) == 0x54bc, "GuC 70 static ADS blob ABI");

struct GucAdsLayout {
    uint32_t bytes {}, nullCaptureOffset {}, privateDataOffset {}, privateDataBytes {};
};
struct GucGen12GtInfo {
    uint32_t sliceCount {}, vdboxSfcMask {}, doorbellsPerSqidi {};
};

// Gen12 Xe-LP physical facts from intel_sseu.c, intel_engine_cs.c and
// intel_guc_ads.c. The native adapter supplies the platform's admitted
// VDBOX mask; this does not enable GuC scheduling or hand it a context.
bool buildGen12GucGtInfo(uint32_t sliceEnable, uint32_t doorbellsPopulated,
    uint32_t mediaDisable, uint8_t supportedVdboxMask, GucGen12GtInfo &out);

// For authentication with the host scheduler retained: no engine, context or
// save/restore list is handed to GuC. All capture pointers reference one empty
// list; firmware-private memory is separate and page aligned. Native code must
// still verify firmware readiness before exposing HuC work to clients.
bool planGucAuthenticationAds(uint32_t privateDataBytes, GucAdsLayout &out);
bool initializeGucAuthenticationAds(void *memory, uint32_t bytes,
    uint32_t ggttAddress, uint32_t privateDataBytes, const GucGen12GtInfo &gt,
    GucAdsLayout &out);

// i915's normal-build allocation: one state page, 64 KiB debug, 8 KiB crash,
// 1 MiB capture. Verbose logging and asynchronous log notifications stay off.
constexpr uint32_t kGucAuthenticationLogBytes = 0x113000;
bool initializeGucAuthenticationLog(void *memory, uint32_t bytes);
struct GucBootParameters { uint32_t words[kGucBootDwords] {}; };

// Gen12 Xe-LP GuC >=70.7 uses the PRE_PARSER/POLLCS/TSC workarounds. Uses the
// caller's physical device and revision; admission belongs to the native adapter.
// Both buffers must fit the GuC GGTT window and must not overlap.
bool buildGen12Guc70AuthenticationParameters(const FirmwareInfo &guc,
    uint32_t physicalDeviceId, uint32_t revision, uint32_t adsAddress,
    uint32_t logAddress, GucBootParameters &out);

enum class FirmwareTarget { GuC, HuC };
struct FirmwareDmaPlan {
    uint32_t sourceAddress {}, destinationOffset {}, copyBytes {};
    uint32_t destinationSpace {}, startControl {}, finishControl {};
};
// Describes i915's CSS+uCode DMA only; GuC RSA and HuC CT authentication are
// separate operations. The native adapter proves the GGTT PTEs and retains
// buffers, forcewake and hardware ownership throughout each transfer.
bool buildGen12FirmwareDma(FirmwareTarget target, const FirmwareInfo &firmware,
    uint32_t sourceAddress, const WopcmLayout &wopcm, FirmwareDmaPlan &out);
}
}
#endif
