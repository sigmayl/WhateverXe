// SPDX-License-Identifier: MIT
#include "uc_runtime.hpp"
#include <string.h>

namespace HevcCore {
namespace Uc {
namespace {
constexpr uint32_t kGucStatus = 0xc000, kHucLoadInfo = 0xc1dc;
constexpr uint32_t kReset = 0x941c, kDmaControl = 0xc314;
constexpr uint32_t kGucReadyMask = 0xc000ff00, kGucReady = 0x8000f000;

bool fail(RuntimeState &state, RuntimeFailure reason) {
    state.failure = reason;
    if (state.retained) state.phase = RuntimePhase::Unavailable;
    return false;
}
bool poll(const RuntimeOps &ops, uint32_t reg, uint32_t mask,
    uint32_t expected, uint32_t milliseconds) {
    for (uint32_t i = 0; i <= milliseconds; ++i) {
        const uint32_t value = ops.read(ops.context, reg);
        if (value == 0xffffffffu) return false;
        if ((value & mask) == expected) return true;
        if (i != milliseconds) ops.sleep(ops.context, 1);
    }
    return false;
}
bool dma(const RuntimeConfig &config, const RuntimeOps &ops,
    FirmwareTarget target, const WopcmLayout &wopcm) {
    const bool huc = target == FirmwareTarget::HuC;
    FirmwareDmaPlan plan;
    if (!buildGen12FirmwareDma(target, huc ? config.huc : config.guc,
            huc ? config.hucAddress : config.gucAddress, wopcm, plan)) return false;
    ops.write(ops.context, 0xc300, plan.sourceAddress);
    ops.write(ops.context, 0xc304, 0);
    ops.write(ops.context, 0xc308, plan.destinationOffset);
    ops.write(ops.context, 0xc30c, plan.destinationSpace);
    ops.write(ops.context, 0xc310, plan.copyBytes);
    ops.write(ops.context, kDmaControl, plan.startControl);
    const bool complete = poll(ops, kDmaControl, 1, 0, 100);
    ops.write(ops.context, kDmaControl, plan.finishControl);
    return complete;
}
bool validConfig(const RuntimeConfig &config) {
    const auto &g = config.guc;
    const auto &h = config.huc;
    if (g.version.major != 70 || g.version.minor != 49 || g.version.patch != 4 ||
        h.version.major != 7 || h.version.minor != 9 || h.version.patch != 3 ||
        g.uploadBytes != 335104 || h.uploadBytes != 589632 ||
        g.privateDataBytes != 0x801000 || h.privateDataBytes ||
        g.rsaOffset != g.uploadBytes || h.rsaOffset != h.uploadBytes ||
        g.rsaBytes != kRsaBytes || h.rsaBytes != kRsaBytes ||
        !config.gucRsa || config.gucRsaBytes != kRsaBytes ||
        !validGucGgttRange(config.gucAddress, g.uploadBytes + kRsaBytes, 4096) ||
        !validGucGgttRange(config.hucAddress, h.uploadBytes + kRsaBytes, 4096)) return false;
    GucBootParameters expected;
    if (!buildGen12Guc70AuthenticationParameters(g, 0xa780, 4,
            config.adsAddress, config.logAddress, expected)) return false;
    return !memcmp(&expected, &config.parameters, sizeof(expected));
}
}

bool gen12FirmwareReady(uint32_t gucStatus, uint32_t hucLoadInfo) {
    return gucStatus != 0xffffffffu && hucLoadInfo != 0xffffffffu &&
        (gucStatus & kGucReadyMask) == kGucReady && (hucLoadInfo & 1u);
}

bool refreshGen12Runtime(const RuntimeOps &ops, RuntimeState &state) {
    if (!state.retained || state.phase != RuntimePhase::Authenticated) return false;
    if (!ops.context || !ops.ownershipValid || !ops.read ||
        !ops.ownershipValid(ops.context)) return fail(state, RuntimeFailure::Ownership);
    if (!gen12FirmwareReady(ops.read(ops.context, kGucStatus),
            ops.read(ops.context, kHucLoadInfo)))
        return fail(state, RuntimeFailure::HuCAuthenticate);
    return true;
}

bool initializeGen12Runtime(const RuntimeConfig &config,
    const RuntimeOps &ops, RuntimeState &state) {
    if (state.retained) {
        if (state.phase == RuntimePhase::Authenticated) return refreshGen12Runtime(ops, state);
        return false;
    }
    if (!validConfig(config) || !ops.context || !ops.ownershipValid ||
        !ops.mappingsValid || !ops.flushMappings || !ops.retainUntilReboot ||
        !ops.read || !ops.write || !ops.sleep || !ops.registerCt || !ops.authenticateHuC)
        return fail(state, RuntimeFailure::Configuration);
    if (!ops.ownershipValid(ops.context)) return fail(state, RuntimeFailure::Ownership);
    if (!ops.mappingsValid(ops.context)) return fail(state, RuntimeFailure::Mapping);
    const uint32_t reset = ops.read(ops.context, kReset);
    const uint32_t control = ops.read(ops.context, kDmaControl);
    const uint32_t status = ops.read(ops.context, kGucStatus);
    const uint32_t hucInfo = ops.read(ops.context, kHucLoadInfo);
    if (reset || control == 0xffffffffu || status == 0xffffffffu ||
        hucInfo == 0xffffffffu || (control & 1) || (hucInfo & 1) ||
        ((status >> 8) & 255) == 0xf0) return fail(state, RuntimeFailure::BusyHardware);
    WopcmLayout wopcm;
    if (!planGen12Wopcm(config.huc.uploadBytes, config.guc.uploadBytes,
            ops.read(ops.context, 0xc340), ops.read(ops.context, 0xc050), wopcm))
        return fail(state, RuntimeFailure::Wopcm);
    ops.flushMappings(ops.context);
    if (!ops.retainUntilReboot(ops.context)) return fail(state, RuntimeFailure::Retain);
    state.retained = true;
    state.phase = RuntimePhase::Retained;
    state.failure = RuntimeFailure::None;

    // i915 gen6_hw_domain_reset, GEN11_GRDOM_GUC only. Host/media engines are
    // not reset or handed to GuC. Locked WOPCM is rechecked after this reset.
    ops.write(ops.context, kReset, 8);
    if (!poll(ops, kReset, 8, 0, 100) || !poll(ops, kGucStatus, 1, 1, 100))
        return fail(state, RuntimeFailure::Reset);
    if (!planGen12Wopcm(config.huc.uploadBytes, config.guc.uploadBytes,
            ops.read(ops.context, 0xc340), ops.read(ops.context, 0xc050), wopcm))
        return fail(state, RuntimeFailure::Wopcm);
    if (!wopcm.alreadyLocked) {
        ops.write(ops.context, 0xc050, wopcm.gucBytes);
        if (ops.read(ops.context, 0xc050) != wopcm.sizeRegister)
            return fail(state, RuntimeFailure::Wopcm);
        ops.write(ops.context, 0xc340, wopcm.gucBase | 2);
        if (ops.read(ops.context, 0xc340) != wopcm.offsetRegister)
            return fail(state, RuntimeFailure::Wopcm);
    }
    ops.write(ops.context, 0xcee8, 1); // GuC GGTT TLB invalidate
    if (!dma(config, ops, FirmwareTarget::HuC, wopcm)) return fail(state, RuntimeFailure::HuCDma);
    state.phase = RuntimePhase::HuCUploaded;
    ops.write(ops.context, 0xc180, 0);
    for (uint32_t i = 0; i < kGucBootDwords; ++i)
        ops.write(ops.context, 0xc184 + i * 4, config.parameters.words[i]);
    ops.write(ops.context, 0xc064, 0x8607); // Gen12 Xe-LP guc_prepare_xfer
    ops.write(ops.context, 0x13816c, 1);
    for (uint32_t i = 0; i < kRsaBytes / 4; ++i) {
        uint32_t rsa;
        memcpy(&rsa, static_cast<const uint8_t *>(config.gucRsa) + i * 4, sizeof(rsa));
        ops.write(ops.context, 0xc200 + i * 4, rsa);
    }
    if (!dma(config, ops, FirmwareTarget::GuC, wopcm)) return fail(state, RuntimeFailure::GuCDma);
    if (!poll(ops, kGucStatus, kGucReadyMask, kGucReady, 3000))
        return fail(state, RuntimeFailure::GuCBoot);
    state.phase = RuntimePhase::GuCRunning;
    if (!ops.registerCt(ops.context)) return fail(state, RuntimeFailure::CtRegister);
    state.phase = RuntimePhase::CtRegistered;
    if (!ops.authenticateHuC(ops.context)) return fail(state, RuntimeFailure::CtAuthenticate);
    if (!poll(ops, kHucLoadInfo, 1, 1, 1000)) return fail(state, RuntimeFailure::HuCAuthenticate);
    state.phase = RuntimePhase::Authenticated;
    return refreshGen12Runtime(ops, state);
}
}
}
