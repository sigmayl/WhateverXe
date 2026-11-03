// SPDX-License-Identifier: MIT
// Packed Gen12 media command packets. Bit positions follow the Intel
// media-driver (iHD) generated MHW command headers so a packet produced here
// can be compared word for word with the reference driver.
#ifndef WEX_MEDIA_HEVC_CORE_HW_COMMAND_HPP
#define WEX_MEDIA_HEVC_CORE_HW_COMMAND_HPP

#include <stdint.h>
#include <string.h>
#include <vector>

namespace HevcCore {

// One register field: DWORD index and inclusive bit range inside that DWORD.
struct Field {
    uint8_t dw;
    uint8_t lo;
    uint8_t hi;
};

// A command packet is a flat list of 32-bit words. The first word is the
// command header; the remaining words are command specific.
class Packet {
public:
    explicit Packet(uint32_t dwords) : words_(dwords, 0) {}

    void set(Field field, uint32_t value);
    uint32_t get(Field field) const;

    // Writes a 64-bit GPU address into a 64B aligned lower/upper address
    // pair. The lower DWORD carries address bits 31:6 and the upper DWORD
    // carries bits 63:32, matching VDENC_64B_Aligned_*_Address.
    void setAddress(uint32_t dw, uint64_t address);

    uint32_t size() const { return static_cast<uint32_t>(words_.size()); }
    uint32_t operator[](uint32_t dw) const { return words_[dw]; }
    uint32_t &operator[](uint32_t dw) { return words_[dw]; }

    const std::vector<uint32_t> &words() const { return words_; }
    std::vector<uint8_t> bytes() const;

private:
    std::vector<uint32_t> words_;
};

// Writes the shared command header DWORD. dwordLength counts the DWORDs after
// the header minus one, which is the opcode length convention used by the
// generated MHW headers (GetOpLength()).
void commandHeader(Packet &packet, uint32_t subopb, uint32_t opcode,
    uint32_t pipeline, uint32_t commandType);

// Header for commands on the parallel video pipe used by VDENC.
void vdCommandHeader(Packet &packet, uint32_t subopb);

// Header for HCP (codec engine 7h) commands. The media instruction command
// field selects the HCP command, for example 0 for pipe mode select.
void hcpCommandHeader(Packet &packet, uint32_t command);

// Copies a command built with the vendored Gen12 MHW structure into a packet
// so both representations can be compared and serialized the same way.
template <typename T>
Packet packetFrom(const T &command) {
    static_assert(sizeof(T) % 4 == 0, "command size must be a multiple of 4 bytes");
    Packet packet(static_cast<uint32_t>(sizeof(T) / 4));
    memcpy(&packet[0], &command, sizeof(T));
    return packet;
}

}

#endif
