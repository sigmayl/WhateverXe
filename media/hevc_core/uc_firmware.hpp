// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2021 Intel Corporation
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//
// Layout/partition rules: Linux i915 intel_uc_fw_abi.h, intel_uc_fw.c and
// intel_wopcm.c. No driver objects, firmware payload or hardware I/O here.
#ifndef WEX_MEDIA_HEVC_CORE_UC_FIRMWARE_HPP
#define WEX_MEDIA_HEVC_CORE_UC_FIRMWARE_HPP
#include <stdint.h>
#include <stddef.h>

namespace HevcCore {
namespace Uc {
constexpr uint32_t kCssBytes = 128;
constexpr uint32_t kRsaBytes = 256;
constexpr uint32_t kGen12WopcmBytes = 2u * 1024 * 1024;
constexpr uint32_t kGucGgttTop = 0xfee00000;

struct FirmwareVersion {
    uint8_t major {}, minor {}, patch {};
};
struct FirmwareInfo {
    FirmwareVersion version;
    uint32_t uploadBytes {}, rsaOffset {}, rsaBytes {};
    // CSS DWORD 30; only GuC uses this ADS allocation size. DWORD 31 is
    // header_info and must not be interpreted as a buffer length.
    uint32_t privateDataBytes {};
};

// CSS + uCode are uploaded; RSA is read separately. Truncated images may
// omit modulus/exponent data. The caller pins the expected firmware version
// and is responsible for the payload hash and native physical/driver guards.
bool parseCssFirmware(const void *image, size_t bytes,
    FirmwareVersion expected, FirmwareInfo &out);

struct WopcmLayout {
    uint32_t gucBase {}, gucBytes {};
    uint32_t offsetRegister {}, sizeRegister {};
    bool alreadyLocked {};
};

// Gen12 reserves 36 KiB for hardware contexts, 16 KiB above HuC, and 24 KiB
// inside the GuC partition. Already locked partitions must fit both images
// and select GuC as HuC loading agent. Partial locks are rejected.
bool planGen12Wopcm(uint32_t hucBytes, uint32_t gucBytes,
    uint32_t offsetRegister, uint32_t sizeRegister, WopcmLayout &out);
bool validGucGgttRange(uint32_t address, uint32_t bytes, uint32_t alignment);

// i915 gen8_ggtt_pte_encode: Gen12 system-memory GGTT entries carry PRESENT,
// whereas PPGTT RW/PAT bits have different meanings in the global table.
// Requires an existing present entry for this exact independently resolved
// physical page. Native callers must retain and validate the owned mapping
// before using the returned entry; this helper performs no page-table writes.
bool gen12SystemGgttEntry(uint64_t nativeEntry, uint64_t physicalPage, uint64_t &out);
}
}
#endif
