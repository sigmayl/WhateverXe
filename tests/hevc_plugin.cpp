// CPU boundary checks for the exact AppleGVA profile-negotiation adapter.
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "../media/va_shim/hevc_plugin.hpp"

int main() {
    using namespace VaShim::HevcPlugin;
    std::vector<uint8_t> image(0x36000);
    // Reproducible instruction fixtures from the accepted plug-in's IDA
    // analysis: indirect AVD creation and its HEVC profile getter.
    const uint8_t call[] = {
        0xff,0x93,0x20,0x04,0x00,0x00,0x41,0x89,0xc7,0x48,0x8b,0x8d,
        0x28,0x70,0xff,0xff};
    const uint8_t getter[] = {
        0x48,0x8b,0x87,0xc0,0xba,0x0a,0x00,0x48,0x85,0xc0,0x74,0x0c,
        0x55,0x48,0x89,0xe5,0x8b,0x80,0x8c,0x03,0x00,0x00,0x5d,0xc3,
        0xb8,0x01,0x00,0x00,0x00,0xc3};
    memcpy(image.data() + 0x9280, call, sizeof(call));
    memcpy(image.data() + 0xbab9, getter, sizeof(getter));
    alignas(8) uint8_t storage[928];
    memset(storage, 0xa5, sizeof(storage));
    auto *dispatch = storage + 8;
    const uintptr_t base = reinterpret_cast<uintptr_t>(image.data());
    auto **out = reinterpret_cast<void **>(dispatch + 8);
    auto *renderer = reinterpret_cast<uint32_t *>(dispatch + 24);
    auto *profile = reinterpret_cast<uint32_t *>(dispatch + 908);
    const uintptr_t vtable = base + 0x33a68;
    memcpy(dispatch, &vtable, sizeof(vtable));
    *out = dispatch;
    *renderer = 0x1080080;
    *profile = 1;
    auto invoke = [&](bool driver = true, bool enabled = true,
        uintptr_t caller = 0) {
        return enableMain10(out, renderer, caller ? caller : base + 0x9286,
            image.data(), driver, enabled);
    };
    auto unchanged = [&]() {
        assert(*profile == 1);
        for (unsigned i = 0; i < 8; ++i) {
            assert(storage[i] == 0xa5);
            assert(storage[920 + i] == 0xa5);
        }
    };
    assert(!invoke(true, false)); unchanged();
    assert(!invoke(false, true)); unchanged();
    assert(!invoke(true, true, base + 0x9287)); unchanged();
    assert(!enableMain10(out, renderer + 1, base + 0x9286,
        image.data(), true, true)); unchanged();
    assert(!enableMain10(nullptr, renderer, base + 0x9286,
        image.data(), true, true)); unchanged();
    // A mismatched owner may be much smaller than AVD_Dispatch. Do not read
    // its profile offset until the owning class has been identified.
    alignas(8) uint8_t otherOwner[32] {};
    auto **otherOut = reinterpret_cast<void **>(otherOwner + 8);
    auto *otherRenderer = reinterpret_cast<uint32_t *>(otherOwner + 24);
    *otherOut = otherOwner;
    *otherRenderer = 0x1080080;
    assert(!enableMain10(otherOut, otherRenderer, base + 0x9286,
        image.data(), true, true)); unchanged();
    *renderer = 0x1080040;
    assert(!invoke()); unchanged();
    *renderer = 0x1080080;
    *out = nullptr;
    assert(!invoke()); unchanged();
    *out = dispatch;
    dispatch[0] ^= 1;
    assert(!invoke()); unchanged();
    dispatch[0] ^= 1;
    image[0x9280] ^= 1;
    assert(!invoke()); unchanged();
    image[0x9280] ^= 1;
    image[0xbab9 + 18] ^= 1; // different profile member
    assert(!invoke()); unchanged();
    image[0xbab9 + 18] ^= 1;
    uint8_t before[sizeof(storage)];
    memcpy(before, storage, sizeof(storage));
    assert(invoke());
    assert(*profile == 2);
    memcpy(before + 8 + 908, profile, sizeof(*profile));
    assert(memcmp(before, storage, sizeof(storage)) == 0);
    assert(!invoke()); // an existing ceiling is preserved
    *profile = 3;
    assert(!invoke() && *profile == 3);
}
