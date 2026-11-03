// SPDX-License-Identifier: MIT
// VDENC sideband token used by the shipped driver to hand resource handles to
// the kernel. The kernel's IGAccelVideoContextMedia::process_token_VDENC
// resolves each handle with the context's resource table and writes the
// 64-bit graphics address and size back into the token records, then patches
// the command buffer at the offsets named by the payload indices.
#ifndef WEX_VA_SHIM_HEVC_TOKEN_HPP
#define WEX_VA_SHIM_HEVC_TOKEN_HPP

#include <stdint.h>

namespace VaShim {
namespace HevcToken {

// First dword of the token: {u16 type, u16 total dwords}.
constexpr uint32_t kType = 0x8D00;
constexpr uint32_t kMaxResources = 0x80;

// The kernel reads payload indices up to dword 823 after the two header
// dwords, so the token must reserve that many payload dwords.
constexpr uint32_t kPayloadDwords = 824;
constexpr uint32_t kTotalDwords = 2 + kPayloadDwords;

// A handle the kernel resolves. cacheFlag is bit 0 of the record's fifth
// dword; the shipped driver fills it from the resource's mapping attributes.
struct Resource {
    uint32_t handle {0};
    uint32_t cacheFlag {0};
};

// Payload layout, in dwords after the token header. The kernel's dword N is
// payload[N - 2]; see process_token_VDENC.
constexpr uint32_t kResourceCount = 0;
constexpr uint32_t kFirstRecord = 2;
constexpr uint32_t kRecordStride = 6;
// Record fields the kernel writes back.
constexpr uint32_t kRecordHandle = 0;
constexpr uint32_t kRecordAddressLow = 2;
constexpr uint32_t kRecordAddressHigh = 3;
constexpr uint32_t kRecordSize = 4;
constexpr uint32_t kRecordCacheFlag = 5;
// Command-buffer anchors. Zero points the kernel at the start of the batch.
constexpr uint32_t kBatchAnchorDwords = 774;
constexpr uint32_t kSecondAnchorDwords = 775;
// The sixteen reference slots the kernel patches at
// batch + 4 * anchor + 148 + 8 * slot.
constexpr uint32_t kRegionSlots = 16;
constexpr uint32_t kFirstRegionIndex = 798;

// Builds a token that resolves up to kMaxResources handles and asks the kernel
// to write their addresses into the 16 reference slots. anchorDwords selects
// the command area the kernel patches; the probe uses 0, and the picture path
// points it past MI_BATCH_BUFFER_END so the patches land in dead space.
// Returns the token size in dwords, or 0 when the inputs do not fit.
uint32_t buildResolveToken(uint32_t *token, uint32_t capacityDwords,
    const Resource *resources, uint32_t count, uint32_t commandOffsetDwords,
    uint32_t anchorDwords = 0);

// Extracts one resolved record from a token the kernel has processed.
bool readResolvedResource(const uint32_t *token, uint32_t count, uint32_t index,
    Resource *resource, uint64_t *address, uint32_t *size);

}
}

#endif
