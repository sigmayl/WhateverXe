// SPDX-License-Identifier: MIT
#include "command_buffer.hpp"

#include <assert.h>
#include <string.h>

namespace HevcCore {

void PatchList::add(uint32_t offset, uint32_t value) {
    assert((offset & 3) == 0);
    entries_.push_back(PatchEntry {offset, value});
}

void PatchList::apply(std::vector<uint8_t> &buffer) const {
    for (const PatchEntry &entry : entries_) {
        assert(entry.offset + 4 <= buffer.size());
        buffer[entry.offset + 0] = static_cast<uint8_t>(entry.value);
        buffer[entry.offset + 1] = static_cast<uint8_t>(entry.value >> 8);
        buffer[entry.offset + 2] = static_cast<uint8_t>(entry.value >> 16);
        buffer[entry.offset + 3] = static_cast<uint8_t>(entry.value >> 24);
    }
}

BatchBuffer::BatchBuffer(uint32_t capacityBytes) : capacity_(capacityBytes) {}

void BatchBuffer::beginGroup(GroupKind kind) {
    assert(!groupOpen_);
    assert((size() & 3) == 0);
    groupOpen_ = true;
    openGroup_ = GroupRange {kind, size(), 0};
}

void BatchBuffer::endGroup() {
    assert(groupOpen_);
    openGroup_.size = size() - openGroup_.start;
    groups_.push_back(openGroup_);
    groupOpen_ = false;
}

void BatchBuffer::append(const Packet &packet) {
    append(packet.bytes());
}

void BatchBuffer::append(const std::vector<uint8_t> &bytes) {
    if (size() + bytes.size() > capacity_) {
        overflowed_ = true;
        return;
    }
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void BatchBuffer::append(const void *data, size_t bytes) {
    if (bytes == 0)
        return;
    assert(data != nullptr);
    if (size() + bytes > capacity_) {
        overflowed_ = true;
        return;
    }
    const uint8_t *source = static_cast<const uint8_t *>(data);
    bytes_.insert(bytes_.end(), source, source + bytes);
}

void BatchBuffer::appendBatchBufferEnd() {
    append(&kMiBatchBufferEnd, sizeof(kMiBatchBufferEnd));
}

void BatchBuffer::appendNoop() {
    append(&kMiNoop, sizeof(kMiNoop));
}

const GroupRange *BatchBuffer::group(GroupKind kind, uint32_t index) const {
    for (const GroupRange &range : groups_) {
        if (range.kind != kind)
            continue;
        if (index-- == 0)
            return &range;
    }
    return nullptr;
}

}
