// SPDX-License-Identifier: MIT
#include "uc_firmware.hpp"

namespace HevcCore {
namespace Uc {
namespace {
uint32_t word(const uint8_t *image, uint32_t index) {
    const auto *p = image + index * 4;
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
        (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
}

bool parseCssFirmware(const void *image, size_t bytes,
    FirmwareVersion expected, FirmwareInfo &out) {
    if (!image || bytes < kCssBytes || !expected.major)
        return false;
    const auto *data = static_cast<const uint8_t *>(image);
    const uint32_t header = word(data, 1), total = word(data, 6);
    const uint32_t key = word(data, 7), modulus = word(data, 8), exponent = word(data, 9);
    if (word(data, 0) != 6 || word(data, 2) != 0x10000 || word(data, 4) != 0x8086 ||
        uint64_t(key) + modulus + exponent + kCssBytes / 4 != header ||
        key != kRsaBytes / 4 || total <= header)
        return false;
    const uint64_t upload = uint64_t(total - header) * 4 + kCssBytes;
    if (upload >= kGen12WopcmBytes || upload + kRsaBytes > bytes)
        return false;
    const uint32_t version = word(data, 16);
    if (version != (uint32_t(expected.major) << 16 | uint32_t(expected.minor) << 8 |
            expected.patch))
        return false;
    FirmwareInfo result {};
    result.version = expected;
    result.uploadBytes = result.rsaOffset = uint32_t(upload);
    result.rsaBytes = kRsaBytes;
    result.privateDataBytes = word(data, 30);
    out = result;
    return true;
}

bool planGen12Wopcm(uint32_t hucBytes, uint32_t gucBytes,
    uint32_t offsetRegister, uint32_t sizeRegister, WopcmLayout &out) {
    constexpr uint32_t contextReserved = 36 * 1024, hucReserved = 16 * 1024;
    constexpr uint32_t gucReserved = 24 * 1024, offsetMask = 0xffffc000;
    constexpr uint32_t sizeMask = 0xfffff000;
    if (hucBytes < kCssBytes || gucBytes < kCssBytes ||
        hucBytes >= kGen12WopcmBytes || gucBytes >= kGen12WopcmBytes ||
        (hucBytes & 3) || (gucBytes & 3) ||
        bool(offsetRegister & 1) != bool(sizeRegister & 1))
        return false;
    WopcmLayout layout;
    layout.alreadyLocked = offsetRegister & 1;
    if (layout.alreadyLocked) {
        if ((offsetRegister & ~(offsetMask | 3u)) || !(offsetRegister & 2) ||
            (sizeRegister & ~(sizeMask | 1u)))
            return false;
        layout.gucBase = offsetRegister & offsetMask;
        layout.gucBytes = sizeRegister & sizeMask;
        layout.offsetRegister = offsetRegister;
        layout.sizeRegister = sizeRegister;
    } else {
        layout.gucBase = (hucBytes + hucReserved + 0x3fff) & offsetMask;
        if (layout.gucBase >= kGen12WopcmBytes - contextReserved)
            return false;
        layout.gucBytes = (kGen12WopcmBytes - contextReserved - layout.gucBase) & sizeMask;
        layout.offsetRegister = layout.gucBase | 3; // valid, HuC loading agent GuC
        layout.sizeRegister = layout.gucBytes | 1; // lock after readback
    }
    if (uint64_t(layout.gucBase) + layout.gucBytes > kGen12WopcmBytes - contextReserved ||
        uint64_t(hucBytes) + hucReserved > layout.gucBase ||
        uint64_t(gucBytes) + gucReserved > layout.gucBytes)
        return false;
    out = layout;
    return true;
}

bool validGucGgttRange(uint32_t address, uint32_t bytes, uint32_t alignment) {
    return alignment && !(alignment & (alignment - 1)) &&
        address >= kGen12WopcmBytes && !(address & (alignment - 1)) &&
        bytes && uint64_t(address) + bytes <= kGucGgttTop;
}

bool gen12SystemGgttEntry(uint64_t nativeEntry, uint64_t physicalPage, uint64_t &out) {
    constexpr uint64_t addressMask = 0x3ffffffff000ULL; // GEN12_GGTT_PTE_ADDR_MASK
    if (!physicalPage || (physicalPage & ~addressMask) || !(nativeEntry & 1) ||
        (nativeEntry & addressMask) != physicalPage ||
        (nativeEntry & ~(addressMask | 0xfffULL)))
        return false;
    out = physicalPage | 1;
    return true;
}
}
}
