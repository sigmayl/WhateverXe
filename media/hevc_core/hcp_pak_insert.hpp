// SPDX-License-Identifier: MIT
// Gen12 HCP_PAK_INSERT_OBJECT builder. Header encoding and the end-of-stream
// payloads follow media-driver's MhwVdboxHcpInterfaceG12::AddHcpPakInsertObject.
#ifndef WEX_MEDIA_HEVC_CORE_HCP_PAK_INSERT_HPP
#define WEX_MEDIA_HEVC_CORE_HCP_PAK_INSERT_HPP

#include <stdint.h>
#include <vector>

#include "hw_command.hpp"

namespace HevcCore {
namespace Hcp {

struct PakInsertParams {
    const uint8_t *data {};
    uint32_t bitSize {};
    bool lastHeader {false};
    bool endOfSlice {false};
    bool emulationByteInsert {false};
    uint8_t skipEmulationByteCount {};
    bool resetBitstreamStartingPos {false};
    bool lastPicInSeq {false};
    bool lastPicInStream {false};
};

// A PAK insert command consists of its header packet and the inline payload
// bytes that follow it in the command buffer. When a last-picture flag is set
// the payload holds the generated EOS/EOB NAL units.
struct PakInsertCommand {
    Packet header;
    std::vector<uint8_t> payload;
};

// Largest inline payload that fits the 12-bit DWORD_LENGTH field.
constexpr uint32_t kPakInsertMaxPayloadBytes = ((1u << 12) - 1) * 4;

PakInsertCommand pakInsert(const PakInsertParams &params);

}
}

#endif
