// SPDX-License-Identifier: MIT
// Copyright (c) 2014-2021 Intel Corporation
// Gen12 firmware transport follows Linux i915 intel_uc_fw.c, intel_guc_fw.c
// and intel_huc.c. The Intel MIT notice is included in uc_firmware.hpp.
#ifndef WEX_MEDIA_HEVC_CORE_UC_RUNTIME_HPP
#define WEX_MEDIA_HEVC_CORE_UC_RUNTIME_HPP
#include "guc_boot.hpp"

namespace HevcCore {
namespace Uc {
enum class RuntimePhase : uint32_t {
    Idle, Retained, HuCUploaded, GuCRunning, CtRegistered, Authenticated, Unavailable
};
enum class RuntimeFailure : uint32_t {
    None, Configuration, Ownership, Mapping, BusyHardware, Wopcm, Retain,
    Reset, HuCDma, GuCDma, GuCBoot, CtRegister, CtAuthenticate, HuCAuthenticate
};
struct RuntimeState {
    RuntimePhase phase {RuntimePhase::Idle};
    RuntimeFailure failure {RuntimeFailure::None};
    bool retained {false};
};
struct RuntimeConfig {
    FirmwareInfo guc, huc;
    uint32_t gucAddress {}, hucAddress {}, adsAddress {}, logAddress {};
    const void *gucRsa {};
    uint32_t gucRsaBytes {};
    GucBootParameters parameters;
};
struct RuntimeOps {
    void *context {};
    // Native guards and mapping checks are read-only. The caller serializes
    // the whole operation on the admitted driver's workloop under forcewake.
    bool (*ownershipValid)(void *) {};
    bool (*mappingsValid)(void *) {};
    void (*flushMappings)(void *) {};
    // Retains every firmware/ADS/log/CT allocation and its native owner until
    // reboot, and excludes another uploader. No hardware write precedes it.
    bool (*retainUntilReboot)(void *) {};
    uint32_t (*read)(void *, uint32_t) {};
    void (*write)(void *, uint32_t, uint32_t) {};
    void (*sleep)(void *, uint32_t) {};
    bool (*registerCt)(void *) {};
    bool (*authenticateHuC)(void *) {};
};

// Authentication-only runtime for the admitted Raptor Lake-S firmware pair.
// GuC scheduling remains disabled. Only the GuC reset domain is requested.
// A failed transfer or lost authentication never retries on the same boot;
// all firmware-visible allocations remain retained, including failure paths.
bool initializeGen12Runtime(const RuntimeConfig &, const RuntimeOps &, RuntimeState &);
// A retained allocation alone is insufficient: check current GuC/HuC status
// before publishing readiness. Loss makes this lifetime permanently unavailable.
bool refreshGen12Runtime(const RuntimeOps &, RuntimeState &);
bool gen12FirmwareReady(uint32_t gucStatus, uint32_t hucLoadInfo);
}
}
#endif
