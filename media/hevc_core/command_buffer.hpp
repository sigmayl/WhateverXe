// SPDX-License-Identifier: MIT
// Command-buffer assembly shared by the Gen12 HEVC picture and slice paths:
// byte accounting, GPU address patches and the first/second/slice batch groups
// used by the HuC VDEnc kernel.
#ifndef WEX_MEDIA_HEVC_CORE_COMMAND_BUFFER_HPP
#define WEX_MEDIA_HEVC_CORE_COMMAND_BUFFER_HPP

#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "hw_command.hpp"

namespace HevcCore {

// A 32-bit value written at a byte offset in a finished command buffer. Media
// command buffers use these to bind GPU virtual addresses at submission time.
struct PatchEntry {
    uint32_t offset {};
    uint32_t value {};
};

class PatchList {
public:
    void add(uint32_t offset, uint32_t value);
    void apply(std::vector<uint8_t> &buffer) const;
    const std::vector<PatchEntry> &entries() const { return entries_; }
    size_t size() const { return entries_.size(); }

private:
    std::vector<PatchEntry> entries_;
};

// Batch groups in the order the HuC VDEnc kernel executes them.
enum class GroupKind : uint32_t {
    First = 0,   // HCP_PIPE_MODE_SELECT + MI_BATCH_BUFFER_END
    Second = 1,  // VDENC_CMD1 + HCP_PIC_STATE + VDENC_CMD2 + MI_BATCH_BUFFER_END
    Slice = 2,   // slice state, PAK inserts and weights, one group per slice
};

struct GroupRange {
    GroupKind kind {};
    uint32_t start {};
    uint32_t size {};
};

class BatchBuffer {
public:
    explicit BatchBuffer(uint32_t capacityBytes);

    void beginGroup(GroupKind kind);
    void endGroup();

    void append(const Packet &packet);
    void append(const std::vector<uint8_t> &bytes);
    void append(const void *data, size_t bytes);
    void appendBatchBufferEnd();
    void appendNoop();

    uint32_t size() const { return static_cast<uint32_t>(bytes_.size()); }
    bool overflowed() const { return overflowed_; }
    const std::vector<uint8_t> &bytes() const { return bytes_; }
    const std::vector<GroupRange> &groups() const { return groups_; }

    // Returns the index-th group of a kind, or nullptr when it is absent.
    const GroupRange *group(GroupKind kind, uint32_t index = 0) const;

    PatchList &patches() { return patches_; }
    const PatchList &patches() const { return patches_; }

private:
    uint32_t capacity_ {};
    bool overflowed_ {false};
    bool groupOpen_ {false};
    GroupRange openGroup_ {};
    std::vector<uint8_t> bytes_;
    std::vector<GroupRange> groups_;
    PatchList patches_;
};

// MI_BATCH_BUFFER_END and MI_NOOP words in a Gen12 command stream.
constexpr uint32_t kMiBatchBufferEnd = 0x05000000;
constexpr uint32_t kMiNoop = 0x00000000;

}

#endif
