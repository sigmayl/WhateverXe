// SPDX-License-Identifier: MIT
// Synthetic firmware headers; no distributed firmware payload.
#include "../media/hevc_core/uc_firmware.cpp"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>

static void set(std::array<uint8_t, 4097> &bytes, uint32_t index, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[1 + index * 4 + i] = uint8_t(value >> (i * 8));
}
int main() {
    using namespace HevcCore::Uc;
    std::array<uint8_t, 4097> image {};
    set(image, 0, 6); set(image, 1, 161); set(image, 2, 0x10000);
    set(image, 4, 0x8086); set(image, 6, 161 + 256);
    set(image, 7, 64); set(image, 8, 64); set(image, 9, 1);
    set(image, 16, 0x460c01);
    set(image, 30, 0x801000); set(image, 31, 0x400100);
    FirmwareInfo info;
    assert(parseCssFirmware(image.data() + 1, 1408, {70,12,1}, info));
    assert(info.uploadBytes == 1152 && info.rsaOffset == 1152 && info.rsaBytes == 256);
    assert(info.privateDataBytes == 0x801000);
    const auto saved = info;
    assert(!parseCssFirmware(image.data() + 1, 1407, {70,12,1}, info));
    assert(!memcmp(&info, &saved, sizeof(info)));
    assert(!parseCssFirmware(image.data() + 1, 1408, {70,12,2}, info));
    set(image, 1, 160);
    assert(!parseCssFirmware(image.data() + 1, 4096, {70,12,1}, info));
    set(image, 1, 161); set(image, 6, 160);
    assert(!parseCssFirmware(image.data() + 1, 4096, {70,12,1}, info));
    set(image, 6, 0xffffffff);
    assert(!parseCssFirmware(image.data() + 1, 4096, {70,12,1}, info));
    set(image, 6, 417); set(image, 7, 63);
    assert(!parseCssFirmware(image.data() + 1, 4096, {70,12,1}, info));
    set(image, 7, 64); set(image, 4, 0x1002);
    assert(!parseCssFirmware(image.data() + 1, 4096, {70,12,1}, info));

    WopcmLayout layout;
    assert(planGen12Wopcm(589632, 335104, 0, 0, layout));
    assert(layout.gucBase == 0x94000 && layout.gucBytes == 0x163000);
    assert(layout.offsetRegister == 0x94003 && layout.sizeRegister == 0x163001);
    WopcmLayout locked;
    assert(planGen12Wopcm(589632,335104,layout.offsetRegister,layout.sizeRegister,locked));
    assert(locked.alreadyLocked && locked.gucBase == layout.gucBase);
    const auto savedLayout = locked;
    assert(!planGen12Wopcm(589632,335104,0x40003,0x100001,locked));
    assert(!memcmp(&locked, &savedLayout, sizeof(locked)));
    assert(!planGen12Wopcm(589632,335104,layout.offsetRegister,0,locked));
    assert(!planGen12Wopcm(589632,335104,0,layout.sizeRegister,locked));
    assert(!planGen12Wopcm(589632,335104,layout.gucBase|1,layout.sizeRegister,locked));
    assert(!planGen12Wopcm(589632,335104,layout.gucBase|3,0x200001,locked));
    assert(!planGen12Wopcm(0xfffffffc,335104,0,0,locked));
    assert(!planGen12Wopcm(589632,0xfffffffc,0,0,locked));
    assert(validGucGgttRange(0x400000,4096,4096));
    assert(validGucGgttRange(kGucGgttTop-4096,4096,4096));
    assert(!validGucGgttRange(0x100000,4096,4096));
    assert(!validGucGgttRange(0x400001,4096,4096));
    assert(!validGucGgttRange(kGucGgttTop-4096,8192,4096));
    assert(!validGucGgttRange(0xfffff000,8192,4096));
    assert(!validGucGgttRange(0x400000,4096,3));
    // i915 gen8_ggtt_pte_encode uses only the physical page and PRESENT for
    // system RAM. PPGTT RW/PAT bits must not select local memory in GGTT.
    uint64_t entry = 0;
    constexpr uint64_t physical = 0x12345678000ULL;
    assert(gen12SystemGgttEntry(physical | 0x8b, physical, entry));
    assert(entry == (physical | 1));
    assert(gen12SystemGgttEntry(physical | 1, physical, entry));
    assert(entry == (physical | 1));
    const uint64_t savedEntry = entry;
    assert(!gen12SystemGgttEntry(physical | 0x8b, physical + 4096, entry));
    assert(!gen12SystemGgttEntry(physical | 0x8a, physical, entry));
    assert(!gen12SystemGgttEntry(physical | 0x8b, physical + 1, entry));
    assert(!gen12SystemGgttEntry(1, 0, entry));
    assert(!gen12SystemGgttEntry((1ULL << 46) | 1, 1ULL << 46, entry));
    assert(!gen12SystemGgttEntry(physical | (1ULL << 52) | 1, physical, entry));
    assert(entry == savedEntry);
    std::puts("PASS CSS/RSA bounds, pinned firmware versions and locked Gen12 WOPCM layouts");
}
