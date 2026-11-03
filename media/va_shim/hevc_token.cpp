// SPDX-License-Identifier: MIT
#include "hevc_token.hpp"

namespace VaShim {
namespace HevcToken {
namespace {

void setData(uint32_t *data, uint32_t index, uint32_t value) {
    data[index] = value;
}

}

uint32_t buildResolveToken(uint32_t *token, uint32_t capacityDwords,
    const Resource *resources, uint32_t count, uint32_t commandOffsetDwords,
    uint32_t anchorDwords) {
    if (!token || !resources || count == 0 || count > kMaxResources)
        return 0;
    if (capacityDwords < kTotalDwords)
        return 0;

    for (uint32_t i = 0; i < kTotalDwords; ++i)
        token[i] = 0;

    token[0] = kType | (kTotalDwords << 16);
    token[1] = commandOffsetDwords;

    uint32_t *data = token + 2;
    setData(data, kResourceCount, count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t base = kFirstRecord + i * kRecordStride;
        setData(data, base + kRecordHandle, resources[i].handle);
        setData(data, base + kRecordCacheFlag, resources[i].cacheFlag & 1);
    }

    // Indices the kernel uses while patching picture buffers. Record 1 is a
    // real resource, so its resolved address and size satisfy the checks.
    setData(data, 770, 1);
    setData(data, 771, 1);
    setData(data, 772, 0);
    // The probe anchors at the batch start; the picture path points past the
    // batch end so the kernel's patches do not touch executed commands.
    setData(data, kBatchAnchorDwords, anchorDwords);
    setData(data, kSecondAnchorDwords, anchorDwords);
    setData(data, 776, 0);
    setData(data, 777, 0);
    setData(data, 778, 0);
    setData(data, 779, 0);
    for (uint32_t i = 782; i <= 789; ++i)
        setData(data, i, 0);
    setData(data, 790, 1);
    setData(data, 791, 1);
    setData(data, 792, 1);
    setData(data, 793, 1);
    setData(data, 794, 0);
    setData(data, 795, 0);
    setData(data, 796, 1);
    setData(data, 797, 0);

    // The kernel writes the resolved 64-bit addresses into these sixteen
    // slots, at batch + 148 + 8 * slot.
    for (uint32_t i = 0; i < kRegionSlots; ++i) {
        const uint32_t index = i < count ? i : 0;
        setData(data, kFirstRegionIndex + i, index);
    }

    for (uint32_t i = 814; i <= 818; ++i)
        setData(data, i, 1);
    for (uint32_t i = 819; i <= 823; ++i)
        setData(data, i, 0);

    return kTotalDwords;
}

bool readResolvedResource(const uint32_t *token, uint32_t count, uint32_t index,
    Resource *resource, uint64_t *address, uint32_t *size) {
    if (!token || index >= count || index >= kMaxResources)
        return false;
    const uint32_t *data = token + 2;
    const uint32_t base = kFirstRecord + index * kRecordStride;
    const uint32_t low = data[base + kRecordAddressLow];
    const uint32_t high = data[base + kRecordAddressHigh];
    if (!low && !high)
        return false;
    if (resource) {
        resource->handle = data[base + kRecordHandle];
        resource->cacheFlag = data[base + kRecordCacheFlag];
    }
    if (address)
        *address = (static_cast<uint64_t>(high) << 32) | low;
    if (size)
        *size = data[base + kRecordSize];
    return true;
}

}
}
