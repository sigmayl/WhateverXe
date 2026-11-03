// SPDX-License-Identifier: MIT
#ifndef WEX_MEDIA_HEVC_GEOMETRY_HPP
#define WEX_MEDIA_HEVC_GEOMETRY_HPP

#include <stdint.h>

namespace HevcCore {
// VDEnc emits both unsplit 32x32 transforms and 16x16 coding units split
// twice into sixteen 4x4 transforms. SPS and HCP must permit both layouts;
// depth one omits the second split flag and desynchronizes CABAC decoding.
constexpr uint32_t kMaxTransformLog2 = 5;
constexpr uint32_t kTransformHierarchyDepth = 2;

// The HCP and VDEnc surface states carry one base address and a chroma row
// offset, so the chroma plane has to be described by its graphics-address
// distance from the luma base. The CPU mapping of an IOSurface pads the planes
// differently (a 368-line picture maps chroma at row 368, the GPU allocation
// at row 384), and the hardware reads the padding instead of the chroma.
// Returns 0 when the pair does not describe a plane at a whole row offset.
inline uint32_t uvRowOffsetFromAddresses(uint64_t luma, uint64_t uv,
    uint32_t pitch) {
    if (!pitch || !luma || uv <= luma || (uv - luma) % pitch)
        return 0;
    return static_cast<uint32_t>((uv - luma) / pitch);
}
}

#endif
