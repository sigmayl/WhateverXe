// SPDX-License-Identifier: MIT
// Copyright (c) 2014-2021 Intel Corporation
// GuC 70 HXG/CTB ABI from Linux i915's guc_communication_ctb_abi.h,
// guc_messages_abi.h, guc_actions_abi.h and intel_guc_ct.c.
// The Intel MIT notice is included in uc_firmware.hpp.
#ifndef WEX_MEDIA_HEVC_CORE_GUC_CT_HPP
#define WEX_MEDIA_HEVC_CORE_GUC_CT_HPP
#include "uc_firmware.hpp"

namespace HevcCore {
namespace Uc {
constexpr uint32_t kCtBlobBytes = 0x6000;
constexpr uint32_t kCtSendDescriptorOffset = 0;
constexpr uint32_t kCtReceiveDescriptorOffset = 0x800;
constexpr uint32_t kCtSendOffset = 0x1000, kCtSendBytes = 0x1000;
constexpr uint32_t kCtReceiveOffset = 0x2000, kCtReceiveBytes = 0x4000;
struct CtDescriptor {
    uint32_t head {}, tail {}, status {}, reserved[13] {};
};
static_assert(sizeof(CtDescriptor) == 64, "GuC 70 descriptor ABI");
struct CtBuffer {
    CtDescriptor *descriptor {};
    uint32_t *words {};
    uint32_t capacityDwords {};
};
struct CtChannels {
    CtBuffer send, receive;
    uint32_t sendDescriptorAddress {}, sendAddress {};
    uint32_t receiveDescriptorAddress {}, receiveAddress {};
};
enum class CtResult { Ok, Empty, NoSpace, Broken, Invalid, TooSmall };
struct CtMessage {
    uint32_t fence {}, dwords {};
};
enum class HxgReply { Other, Success, Failure, Busy, Retry, Invalid };

// Pure memory operations: registration and the MMIO notify remain native
// adapter responsibilities. Native code must order mapped writes before the
// notify, as i915's intel_guc_write_barrier does.
bool initializeCtChannels(void *memory, uint32_t bytes, uint32_t ggttAddress,
    CtChannels &out);
bool buildCtSelfConfig(uint32_t key, uint64_t value, uint32_t (&out)[4]);
void buildCtControl(bool enabled, uint32_t (&out)[2]);
CtResult writeCtRequest(const CtBuffer &buffer, const uint32_t *action,
    uint32_t dwords, uint32_t fence);
CtResult readCtMessage(const CtBuffer &buffer, uint32_t *payload,
    uint32_t capacityDwords, CtMessage &out);
HxgReply classifyHxgReply(const CtMessage &message, const uint32_t *payload,
    uint32_t expectedFence, uint32_t &data);
}
}
#endif
