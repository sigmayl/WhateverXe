// SPDX-License-Identifier: MIT
// Synthetic layout fixtures from the published GuC 70 ABI; no firmware bytes.
#include "../media/hevc_core/guc_boot.cpp"
#include "../media/hevc_core/uc_firmware.cpp"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>

static uint32_t get(const uint8_t *p, uint32_t offset) {
    return uint32_t(p[offset]) | uint32_t(p[offset + 1]) << 8 |
        uint32_t(p[offset + 2]) << 16 | uint32_t(p[offset + 3]) << 24;
}
int main() {
    using namespace HevcCore::Uc;
    GucAdsLayout layout;
    assert(planGucAuthenticationAds(0x801000, layout));
    GucGen12GtInfo gt;
    assert(buildGen12GucGtInfo(1, 0x007f0001, 0, 1, gt));
    assert(gt.sliceCount == 1 && gt.doorbellsPerSqidi == 128 && gt.vdboxSfcMask == 1);
    const auto savedGt = gt;
    assert(!buildGen12GucGtInfo(0, 0x007f0001, 0, 1, gt));
    assert(!buildGen12GucGtInfo(3, 0x007f0001, 0, 1, gt));
    assert(!buildGen12GucGtInfo(1, 0xffffffff, 0, 1, gt));
    assert(!buildGen12GucGtInfo(1, 0x007f0001, 0xffffffff, 1, gt));
    assert(!memcmp(&gt, &savedGt, sizeof(gt)));
    assert(layout.bytes == 0x808000 && layout.nullCaptureOffset == 0x6000 &&
        layout.privateDataOffset == 0x7000 && layout.privateDataBytes == 0x801000);
    const auto savedLayout = layout;
    assert(!planGucAuthenticationAds(0, layout));
    assert(!planGucAuthenticationAds(0xffffffff, layout));
    assert(!memcmp(&layout, &savedLayout, sizeof(layout)));
    assert(planGucAuthenticationAds(1, layout));
    assert(layout.bytes == 0x8000 && layout.privateDataBytes == 4096);
    assert(planGucAuthenticationAds(0x801000, layout));

    // An unaligned CPU mapping is legal for this byte-wise serializer; GPU
    // pointers stay page aligned. Sentinels cover both allocation boundaries.
    std::vector<uint8_t> storage(layout.bytes + 2, 0xa5);
    auto *p = storage.data() + 1;
    assert(initializeGucAuthenticationAds(p, layout.bytes, 0x400000, 0x801000, gt, layout));
    assert(storage.front() == 0xa5 && storage.back() == 0xa5);
    assert(get(p, 0x1004) == 0x4011dc && get(p, 0x1008) == 0x40123c);
    assert(get(p, 0x1094) == 0x407000);
    assert(get(p, 0x121c) == 500000 && get(p, 0x1220) == 1);
    assert(get(p, 0x1224) == 15 && get(p, 0x1228) == 1);
    for (uint32_t i = 0x123c; i < 0x143c; ++i) assert(p[i] == 32);
    for (uint32_t i = 0x143c; i < 0x147c; i += 4) assert(get(p, i) == 0);
    assert(get(p, 0x147c) == 1 && get(p, 0x1480) == 1 && get(p, 0x1484) == 128);
    for (uint32_t i = 0; i < 0x1000; ++i) assert(p[i] == 0);
    for (uint32_t i = 0x1014; i < 0x1094; ++i) assert(p[i] == 0);
    for (uint32_t i = 0x109c; i < 0x11a4; i += 4) assert(get(p, i) == 0x406000);
    for (uint32_t i = 0x11a4; i < 0x11dc; ++i) assert(p[i] == 0);
    for (uint32_t i = 0x6000; i < layout.bytes; ++i) assert(p[i] == 0);
    const auto savedStorage = storage;
    assert(!initializeGucAuthenticationAds(p, layout.bytes - 1, 0x400000, 0x801000, gt, layout));
    assert(!initializeGucAuthenticationAds(p, layout.bytes, kGucGgttTop - 4096, 0x801000, gt, layout));
    assert(!initializeGucAuthenticationAds(p, layout.bytes, 0x400001, 0x801000, gt, layout));
    auto invalidGt = gt; invalidGt.doorbellsPerSqidi = 0;
    assert(!initializeGucAuthenticationAds(p, layout.bytes, 0x400000, 0x801000, invalidGt, layout));
    assert(storage == savedStorage && !memcmp(&layout, &savedLayout, sizeof(layout)));

    std::vector<uint8_t> logs(kGucAuthenticationLogBytes + 1, 0xa5);
    assert(initializeGucAuthenticationLog(logs.data(), kGucAuthenticationLogBytes));
    for (uint32_t i = 0; i < kGucAuthenticationLogBytes; ++i) assert(logs[i] == 0);
    assert(logs.back() == 0xa5);
    logs[0] = 0x12;
    assert(!initializeGucAuthenticationLog(logs.data(), kGucAuthenticationLogBytes - 1));
    assert(logs[0] == 0x12 && !initializeGucAuthenticationLog(nullptr, kGucAuthenticationLogBytes));

    FirmwareInfo guc {{70,49,4},335104,335104,256,0x801000};
    FirmwareInfo huc {{7,9,3},589632,589632,256,0};
    GucBootParameters params;
    assert(buildGen12Guc70AuthenticationParameters(guc, 0xa780, 4, 0x400000, 0x1000000, params));
    const uint32_t expected[kGucBootDwords] {
        0x010003d5,0x00444000,0x00004000,0x00000040,0x00000800,0xa7800004
    };
    assert(!memcmp(params.words, expected, sizeof(expected)));
    const auto savedParams = params;
    assert(!buildGen12Guc70AuthenticationParameters(guc, 0xa780, 4, 0x400000, 0x800000, params));
    assert(!buildGen12Guc70AuthenticationParameters(guc, 0xa780, 4, 0x400000, 0xb00000, params));
    assert(!buildGen12Guc70AuthenticationParameters(guc, 0x10000, 4, 0x400000, 0x1000000, params));
    assert(!buildGen12Guc70AuthenticationParameters(guc, 0xa780, 0x104, 0x400000, 0x1000000, params));
    assert(!buildGen12Guc70AuthenticationParameters(guc, 0xa780, 4, 0x400000, kGucGgttTop - 4096, params));
    assert(!buildGen12Guc70AuthenticationParameters(huc, 0xa780, 4, 0x400000, 0x1000000, params));
    auto oldGuc = guc; oldGuc.version = {70,6,0};
    assert(!buildGen12Guc70AuthenticationParameters(oldGuc, 0xa780, 4, 0x400000, 0x1000000, params));
    assert(!memcmp(&params, &savedParams, sizeof(params)));

    WopcmLayout wopcm;
    assert(planGen12Wopcm(huc.uploadBytes, guc.uploadBytes, 0, 0, wopcm));
    FirmwareDmaPlan dma;
    assert(buildGen12FirmwareDma(FirmwareTarget::HuC, huc, 0x2000000, wopcm, dma));
    assert(dma.sourceAddress == 0x2000000 && !dma.destinationOffset && dma.copyBytes == 589632);
    assert(dma.destinationSpace == 0x70000 && dma.startControl == 0x02010201 &&
        dma.finishControl == 0x02000000);
    assert(buildGen12FirmwareDma(FirmwareTarget::GuC, guc, 0x2100000, wopcm, dma));
    assert(dma.destinationOffset == 0x2000 && dma.copyBytes == 335104 &&
        dma.startControl == 0x00110011 && dma.finishControl == 0x00100000);
    const auto savedDma = dma;
    auto malformed = guc; malformed.rsaOffset++;
    assert(!buildGen12FirmwareDma(FirmwareTarget::GuC, malformed, 0x2100000, wopcm, dma));
    assert(!buildGen12FirmwareDma(FirmwareTarget::GuC, guc, 0x2100001, wopcm, dma));
    assert(!buildGen12FirmwareDma(FirmwareTarget::GuC, guc, kGucGgttTop - 4096, wopcm, dma));
    auto badWopcm = wopcm; badWopcm.gucBytes = 0x40000; badWopcm.sizeRegister = 0x40001;
    assert(!buildGen12FirmwareDma(FirmwareTarget::GuC, guc, 0x2100000, badWopcm, dma));
    badWopcm = wopcm; badWopcm.gucBase = 0x40000; badWopcm.offsetRegister = 0x40003;
    assert(!buildGen12FirmwareDma(FirmwareTarget::HuC, huc, 0x2000000, badWopcm, dma));
    assert(!memcmp(&dma, &savedDma, sizeof(dma)));
    std::puts("PASS GuC 70 authentication ADS/private data, physical boot identity, host scheduling and firmware DMA fixtures");
}
