// SPDX-License-Identifier: BSD-3-Clause
// Gen12 HEVC VDEnc buffer sizing follows Intel media-driver's
// CodechalEncodeHevcBase::Initialize and CodechalVdencHevcStateG12::Initialize.
#ifndef WEX_MEDIA_VA_SHIM_HEVC_BUFFER_SIZES_HPP
#define WEX_MEDIA_VA_SHIM_HEVC_BUFFER_SIZES_HPP

#include <stdint.h>

namespace VaShim {

struct HevcBufferSizes {
    uint32_t cuObjectOffset {};
    uint32_t pakBseBytes {};
    uint32_t cuRecordBytes {};
    uint32_t streamInBytes {};
    bool valid {};
};

inline HevcBufferSizes hevcBufferSizes(uint32_t width, uint32_t height) {
    if (!width || !height)
        return {};
    const uint64_t columns = (uint64_t(width) + 63) / 64;
    const uint64_t rows = (uint64_t(height) + 63) / 64;
    // Gen12 reserves one extra CTU row for tile-column size/alignment data.
    const uint64_t ctus = columns * (rows + 1);
    constexpr uint64_t maxAligned = uint64_t(UINT32_MAX) & ~uint64_t(4095);
    // Five PAK DWORDs plus up to 64 eight-DWORD CU records per CTU,
    // doubled for sideband. Bound before multiplying or page rounding.
    constexpr uint64_t bytesPerCtu = 2 * 4 * (5 + 64 * 8);
    if (ctus > (maxAligned - 4095) / bytesPerCtu)
        return {};
    const uint64_t minUnits = ((uint64_t(width) + 15) / 16) *
        ((uint64_t(height) + 15) / 16);
    if (minUnits > (maxAligned - 4095) / 32)
        return {};
    auto page = [](uint64_t bytes) { return (bytes + 4095) & ~uint64_t(4095); };
    const uint64_t objects = page(minUnits * 8 * 4);
    const uint64_t records = page(ctus * bytesPerCtu);
    if (objects > maxAligned - records)
        return {};
    return {uint32_t(objects), uint32_t(objects + records),
        uint32_t(page(ctus * 64 * 32)),
        uint32_t(page(columns * rows * 4 * 64)), true};
}

}
#endif
