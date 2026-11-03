// SPDX-License-Identifier: BSD-3-Clause
#ifndef WEX_MEDIA_VA_SHIM_HEVC_PLUGIN_HPP
#define WEX_MEDIA_VA_SHIM_HEVC_PLUGIN_HPP

#include <stdint.h>
#include <string.h>

namespace VaShim {
namespace HevcPlugin {

constexpr const char *kMain10Marker =
    "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_MAIN10";

// AppleGVAHEVCEncoder, macOS 15.8.2. IDA shows that CreateAVDAccelerator's
// output slot is member +8 of the 912-byte AVD_Dispatch object, and the
// renderer output is member +24. The plug-in copies the registry's HEVC
// profile ceiling into +908 before calling the VA driver. Its profile getter
// reads that member when Initialize advertises the accepted pixel formats.
constexpr uintptr_t kCreateReturn = 0x9286;
constexpr uintptr_t kDispatchVtable = 0x33a68;
constexpr uintptr_t kProfileGetter = 0xbab9;
constexpr uint32_t kDispatchProfile = 908;

inline bool matches(const uint8_t *actual, const uint8_t *expected,
    uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; ++i)
        if (actual[i] != expected[i])
            return false;
    return true;
}

// Called only after a successful, admitted VA creation. This updates the
// owning plug-in instance's profile ceiling, without changing the registry
// or executable code. Other AVD callers and other plug-in layouts fail closed.
inline bool enableMain10(void **out, uint32_t *rendererId,
    uintptr_t caller, const uint8_t *image, bool knownDriver, bool enabled) {
    if (!enabled || !knownDriver || !image || !out || !rendererId)
        return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    if (caller != base + kCreateReturn ||
        reinterpret_cast<uintptr_t>(rendererId) !=
            reinterpret_cast<uintptr_t>(out) + 16)
        return false;

    // Indirect CreateAVDAccelerator call and the following instructions.
    static const uint8_t create[] = {
        0xff, 0x93, 0x20, 0x04, 0x00, 0x00,
        0x41, 0x89, 0xc7, 0x48, 0x8b, 0x8d, 0x28, 0x70, 0xff, 0xff};
    // Complete getter, including both the instance and no-instance cases.
    static const uint8_t getter[] = {
        0x48, 0x8b, 0x87, 0xc0, 0xba, 0x0a, 0x00, 0x48, 0x85, 0xc0,
        0x74, 0x0c, 0x55, 0x48, 0x89, 0xe5, 0x8b, 0x80, 0x8c, 0x03,
        0x00, 0x00, 0x5d, 0xc3, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};
    if (!matches(image + kCreateReturn - 6, create, sizeof(create)) ||
        !matches(image + kProfileGetter, getter, sizeof(getter)))
        return false;

    auto *dispatch = reinterpret_cast<uint8_t *>(out) - 8;
    uintptr_t vtable = 0;
    memcpy(&vtable, dispatch, sizeof(vtable));
    if (vtable != base + kDispatchVtable || !*out || *rendererId != 0x1080080)
        return false;
    uint32_t profile = 0;
    memcpy(&profile, dispatch + kDispatchProfile, sizeof(profile));
    if (profile != 1)
        return false;
    profile = 2;
    memcpy(dispatch + kDispatchProfile, &profile, sizeof(profile));
    return true;
}

}
}

#endif
