// SPDX-License-Identifier: MIT
#include "hw_command.hpp"

#include <assert.h>

namespace HevcCore {

void Packet::set(Field field, uint32_t value) {
    assert(field.dw < words_.size());
    assert(field.hi < 32);
    assert(field.lo <= field.hi);
    const uint32_t width = field.hi - field.lo + 1;
    const uint32_t mask = width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1) << field.lo;
    assert(width >= 32 || (value & ~(mask >> field.lo)) == 0);
    words_[field.dw] = (words_[field.dw] & ~mask) | ((value << field.lo) & mask);
}

uint32_t Packet::get(Field field) const {
    const uint32_t width = field.hi - field.lo + 1;
    const uint32_t mask = width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1;
    return (words_[field.dw] >> field.lo) & mask;
}

void Packet::setAddress(uint32_t dw, uint64_t address) {
    assert(dw + 1 < words_.size());
    assert((address & 0x3F) == 0);
    words_[dw] = static_cast<uint32_t>(address & 0xFFFFFFC0u);
    words_[dw + 1] = static_cast<uint32_t>(address >> 32);
}

std::vector<uint8_t> Packet::bytes() const {
    std::vector<uint8_t> out;
    out.reserve(words_.size() * 4);
    for (uint32_t word : words_) {
        out.push_back(static_cast<uint8_t>(word));
        out.push_back(static_cast<uint8_t>(word >> 8));
        out.push_back(static_cast<uint8_t>(word >> 16));
        out.push_back(static_cast<uint8_t>(word >> 24));
    }
    return out;
}

void commandHeader(Packet &packet, uint32_t command, uint32_t opcode,
    uint32_t pipeline, uint32_t commandType) {
    assert(packet.size() >= 2);
    packet.set(Field {0, 0, 11}, packet.size() - 2);
    packet.set(Field {0, 16, 22}, command);
    packet.set(Field {0, 23, 26}, opcode);
    packet.set(Field {0, 27, 28}, pipeline);
    packet.set(Field {0, 29, 31}, commandType);
}

void vdCommandHeader(Packet &packet, uint32_t subopb) {
    // OPCODE_VDENCPIPE, PIPELINE_MFXCOMMON, COMMAND_TYPE_PARALLELVIDEOPIPE.
    commandHeader(packet, subopb, 1, 2, 3);
}

void hcpCommandHeader(Packet &packet, uint32_t command) {
    // MEDIA_INSTRUCTION_OPCODE_CODECENGINENAME (HCP = 7h).
    commandHeader(packet, command, 7, 2, 3);
}

}
