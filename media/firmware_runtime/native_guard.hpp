// SPDX-License-Identifier: BSD-3-Clause
#ifndef WEX_MEDIA_FIRMWARE_NATIVE_GUARD_HPP
#define WEX_MEDIA_FIRMWARE_NATIVE_GUARD_HPP
#include "native_abi.hpp"
#include "runtime_payload.hpp" // generated outside the source tree
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>

extern "C" const char version[];
struct pmap;
extern "C" struct pmap *kernel_pmap;
extern "C" ppnum_t pmap_find_phys(struct pmap *, uint64_t);
namespace HevcFirmware {
inline bool copyMapped(uintptr_t source, void *destination, size_t length) {
    if (!length || (source >> 48) != 0xffff || length > UINTPTR_MAX - source) return false;
    const uintptr_t end = source + length - 1;
    for (uintptr_t page = source & ~uintptr_t(4095);; page += 4096) {
        if (!pmap_find_phys(kernel_pmap, page)) return false;
        if (page == (end & ~uintptr_t(4095))) break;
    }
    memcpy(destination, reinterpret_cast<const void *>(source), length);
    return true;
}
inline bool argument(const char *name) {
    int flag = 0;
    return PE_parse_boot_argn(name, &flag, sizeof(flag));
}
inline bool contains(const char *text, const char *needle) {
    const size_t bytes = strlen(needle);
    for (; *text; ++text) if (!strncmp(text, needle, bytes)) return true;
    return false;
}
inline bool supportedKernel() {
    return contains(version,"Darwin Kernel Version 24.6.0:") &&
        contains(version,"xnu-11417.140.69.713.47~2/RELEASE_X86_64");
}
inline bool imageMatches(uintptr_t base, const uint8_t (&uuid)[16]) {
    uint32_t header[8];
    if (!base || (base & 4095) || !copyMapped(base,header,sizeof(header)) ||
        header[0] != 0xfeedfacf || header[1] != 0x01000007 || header[3] != 0xb ||
        header[4] > 128 || header[5] > 0x10000) return false;
    bool matched = false, seen = false;
    uint32_t offset = 32;
    for (uint32_t i = 0; i < header[4]; ++i) {
        uint32_t command[2];
        if (offset + 8 > 32 + header[5] || !copyMapped(base + offset,command,sizeof(command)) ||
            command[1] < 8 || command[1] > 32 + header[5] - offset) return false;
        if (command[0] == 0x1b && command[1] == 24) {
            uint8_t actual[16];
            if (seen || !copyMapped(base + offset + 8,actual,sizeof(actual))) return false;
            seen = true;
            matched = !memcmp(actual,uuid,sizeof(actual));
        }
        offset += command[1];
    }
    return matched && offset == 32 + header[5];
}
inline bool installedTarget(uintptr_t target) {
    if ((target >> 48) != 0xffff) return false;
    const uintptr_t page = target & ~uintptr_t(4095);
    for (uint32_t offset = 0; offset <= 0x900000; offset += 4096) {
        uint32_t magic = 0;
        const uintptr_t candidate = page - offset;
        if (copyMapped(candidate,&magic,sizeof(magic)) && magic == 0xfeedfacf &&
            imageMatches(candidate,kWhateverXeUuid)) return true;
    }
    return false;
}
inline uintptr_t routedTarget(uintptr_t address, const uint8_t *actual) {
    if (actual[0] == 0xe9) {
        int32_t displacement;
        memcpy(&displacement,actual + 1,sizeof(displacement));
        return uintptr_t(int64_t(address + 5) + displacement);
    }
    if (actual[0] == 0xff && actual[1] == 0x25 && !actual[2] && !actual[3] &&
        !actual[4] && !actual[5]) {
        uintptr_t target;
        memcpy(&target,actual + 6,sizeof(target));
        return target;
    }
    return 0;
}
inline bool donorMatches(uintptr_t base) {
    if (!imageMatches(base,kDriverUuid)) return false;
    uint8_t actual[sizeof(kWakeFixture)];
    if (!copyMapped(base + 0x28c2a,actual,sizeof(actual))) return false;
    if (!memcmp(actual,kWakeFixture,sizeof(actual))) return true;
    if (IORegistryEntry::getRegistryRoot()->getProperty("WhateverXeForceWakeIRQProtected") !=
            kOSBooleanTrue || memcmp(actual + 14,kWakeFixture + 14,sizeof(actual) - 14) ||
        !installedTarget(routedTarget(base + 0x28c2a,actual))) return false;
    constexpr uint8_t count[] {0x41,0xf6,0xc6,0x04,0x74,0x36};
    constexpr uint8_t call[] {0x44,0x21,0xf2,0x41,0x0f,0xb6,0xf7,0x4c,0x89,0xef,
        0xb9,1,0,0,0,0xe8,0xf5,0x47,0,0};
    uint8_t actualCount[sizeof(count)], actualCall[sizeof(call)];
    return copyMapped(base + 0x28d61,actualCount,sizeof(actualCount)) &&
        !memcmp(actualCount,count,sizeof(count)) &&
        copyMapped(base + 0x28da1,actualCall,sizeof(actualCall)) &&
        !memcmp(actualCall,call,sizeof(call));
}
inline uintptr_t donorBase(IOService *owner) {
    uintptr_t vtable = 0, anchors[8];
    if (!copyMapped(reinterpret_cast<uintptr_t>(owner),&vtable,sizeof(vtable)) ||
        !copyMapped(vtable,anchors,sizeof(anchors)) || anchors[7] < 0x23786) return 0;
    const uintptr_t base = anchors[7] - 0x23786;
    return anchors[0] == base + 0x236ce && anchors[1] == base + 0x2375a &&
        donorMatches(base) ? base : 0;
}
inline bool nativeEntryMatches(uintptr_t base, uint32_t offset,
    const uint8_t *expected, uint32_t bytes, bool allowRoute) {
    uint8_t actual[64];
    if (bytes > sizeof(actual) || !copyMapped(base + offset,actual,bytes)) return false;
    if (!memcmp(actual,expected,bytes)) return true;
    return allowRoute && bytes >= 14 &&
        IORegistryEntry::getRegistryRoot()->getProperty("TGLGuCDiagnosticRouted") == kOSBooleanTrue &&
        !memcmp(actual + 14,expected + 14,bytes - 14) &&
        installedTarget(routedTarget(base + offset,actual));
}
inline bool physicalMatches(IOPCIDevice *pci) {
    if (!pci || pci->configRead32(0) != kPhysicalIdentity ||
        pci->configRead8(8) != kPhysicalRevision) return false;
    auto object = pci->copyProperty("WhateverXeDeviceSelection");
    auto data = OSDynamicCast(OSData,object);
    const bool matched = data && data->getLength() == sizeof(kSelection) &&
        !memcmp(data->getBytesNoCopy(),kSelection,sizeof(kSelection));
    if (object) object->release();
    return matched;
}
}
#endif
