// SPDX-License-Identifier: MIT
#include "guc_ct.hpp"

namespace HevcCore {
namespace Uc {
namespace {
bool aligned(const void *p, uint32_t alignment) {
    return p && !(reinterpret_cast<uintptr_t>(p) & (alignment - 1));
}
bool validBuffer(const CtBuffer &b) {
    return aligned(b.descriptor, 4) && aligned(b.words, 4) &&
        (b.capacityDwords == kCtSendBytes / 4 || b.capacityDwords == kCtReceiveBytes / 4);
}
uint32_t load(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
void store(uint32_t *p, uint32_t value) { __atomic_store_n(p, value, __ATOMIC_RELEASE); }
}

bool initializeCtChannels(void *memory, uint32_t bytes, uint32_t ggttAddress,
    CtChannels &out) {
    if (!aligned(memory, 64) || bytes < kCtBlobBytes ||
        !validGucGgttRange(ggttAddress, kCtBlobBytes, 4096))
        return false;
    auto *data = static_cast<uint8_t *>(memory);
    for (uint32_t i = 0; i < kCtBlobBytes; ++i) data[i] = 0;
    CtChannels channels;
    channels.send = {reinterpret_cast<CtDescriptor *>(data),
        reinterpret_cast<uint32_t *>(data + kCtSendOffset), kCtSendBytes / 4};
    channels.receive = {reinterpret_cast<CtDescriptor *>(data + kCtReceiveDescriptorOffset),
        reinterpret_cast<uint32_t *>(data + kCtReceiveOffset), kCtReceiveBytes / 4};
    channels.sendDescriptorAddress = ggttAddress;
    channels.receiveDescriptorAddress = ggttAddress + kCtReceiveDescriptorOffset;
    channels.sendAddress = ggttAddress + kCtSendOffset;
    channels.receiveAddress = ggttAddress + kCtReceiveOffset;
    out = channels;
    return true;
}

bool buildCtSelfConfig(uint32_t key, uint64_t value, uint32_t (&out)[4]) {
    uint32_t length = 2, bytes = 0, alignment = 0;
    switch (key) {
    case 0x0902: bytes = kCtSendBytes; alignment = 4096; break;
    case 0x0903: bytes = sizeof(CtDescriptor); alignment = 64; break;
    case 0x0905: bytes = kCtReceiveBytes; alignment = 4096; break;
    case 0x0906: bytes = sizeof(CtDescriptor); alignment = 64; break;
    case 0x0904:
        if (value != kCtSendBytes) return false;
        length = 1; break;
    case 0x0907:
        if (value != kCtReceiveBytes) return false;
        length = 1; break;
    default: return false;
    }
    if (bytes && (value > 0xffffffffu ||
            !validGucGgttRange(uint32_t(value), bytes, alignment)))
        return false;
    out[0] = 0x0508; // HOST2GUC_SELF_CFG, HXG request
    out[1] = (key << 16) | length;
    out[2] = uint32_t(value);
    out[3] = uint32_t(value >> 32);
    return true;
}

void buildCtControl(bool enabled, uint32_t (&out)[2]) {
    out[0] = 0x4509; // HOST2GUC_CONTROL_CTB, HXG request
    out[1] = enabled ? 1 : 0;
}

CtResult writeCtRequest(const CtBuffer &b, const uint32_t *action,
    uint32_t dwords, uint32_t fence) {
    if (!validBuffer(b) || !aligned(action, 4) || !dwords || dwords > 255 ||
        !fence || fence > 0xffff || (action[0] & 0xf0000000u) || !(action[0] & 0xffff))
        return CtResult::Invalid;
    const uint32_t head = load(&b.descriptor->head), tail = load(&b.descriptor->tail);
    if (load(&b.descriptor->status)) return CtResult::Broken;
    if (head >= b.capacityDwords || tail >= b.capacityDwords) return CtResult::Invalid;
    const uint32_t used = tail >= head ? tail - head : b.capacityDwords - head + tail;
    if (dwords + 1 > b.capacityDwords - used - 1) return CtResult::NoSpace;
    uint32_t next = tail;
    b.words[next] = (fence << 16) | dwords; // HXG format, all reserved bits zero
    next = (next + 1) % b.capacityDwords;
    for (uint32_t i = 0; i < dwords; ++i) {
        b.words[next] = action[i];
        next = (next + 1) % b.capacityDwords;
    }
    // GuC may observe the descriptor before the subsequent MMIO notify.
    // Order mapped payload writes before publishing its new tail.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    store(&b.descriptor->tail, next);
    return CtResult::Ok;
}

CtResult readCtMessage(const CtBuffer &b, uint32_t *payload,
    uint32_t capacityDwords, CtMessage &out) {
    if (!validBuffer(b) || !aligned(payload, 4)) return CtResult::Invalid;
    const uint32_t head = load(&b.descriptor->head), tail = load(&b.descriptor->tail);
    if (load(&b.descriptor->status)) return CtResult::Broken;
    if (head >= b.capacityDwords || tail >= b.capacityDwords) return CtResult::Invalid;
    if (head == tail) return CtResult::Empty;
    const uint32_t available = tail >= head ? tail - head : b.capacityDwords - head + tail;
    const uint32_t header = b.words[head], count = header & 0xff;
    if ((header & 0xff00) || !count || count + 1 > available) return CtResult::Invalid;
    if (count > capacityDwords) return CtResult::TooSmall;
    uint32_t next = (head + 1) % b.capacityDwords;
    for (uint32_t i = 0; i < count; ++i) {
        payload[i] = b.words[next];
        next = (next + 1) % b.capacityDwords;
    }
    CtMessage message;
    message.fence = header >> 16;
    message.dwords = count;
    out = message;
    store(&b.descriptor->head, next);
    return CtResult::Ok;
}

HxgReply classifyHxgReply(const CtMessage &message, const uint32_t *payload,
    uint32_t expectedFence, uint32_t &data) {
    if (!payload || !message.dwords || !expectedFence || expectedFence > 0xffff)
        return HxgReply::Invalid;
    if (message.fence != expectedFence) return HxgReply::Other;
    const uint32_t header = payload[0];
    if (!(header & 0x80000000u)) return HxgReply::Invalid;
    const uint32_t type = (header >> 28) & 7;
    switch (type) {
    case 7: data = header & 0x0fffffffu; return HxgReply::Success;
    case 6: data = header & 0x0fffffffu; return HxgReply::Failure;
    case 3: return HxgReply::Busy;
    case 5: return HxgReply::Retry;
    default: return HxgReply::Invalid;
    }
}
}
}
