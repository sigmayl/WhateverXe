// SPDX-License-Identifier: MIT
// HEVC RBSP writer used by the Gen12 encode core. The field order follows
// ITU-T H.265 7.3; callers supply already-validated encoder parameters.
#ifndef WEX_MEDIA_HEVC_BIT_WRITER_HPP
#define WEX_MEDIA_HEVC_BIT_WRITER_HPP
#include <stdint.h>
#include <vector>

namespace HevcCore {
class BitWriter {
public:
    void u(uint32_t value, unsigned bits) {
        // Fixed syntax fields wider than the 32-bit value are zero-extended,
        // e.g. the 43 reserved constraint bits in profile_tier_level.
        if (bits > 32) { u(0, bits - 32); bits = 32; }
        for (unsigned i = bits; i; --i) bit((value >> (i - 1)) & 1);
    }
    void ue(uint32_t value) {
        // Leading zero count plus the value+1 suffix.
        uint32_t v = value + 1;
        unsigned len = 0;
        for (uint32_t t = v; t > 1; t >>= 1) ++len;
        u(0, len);
        u(v, len + 1);
    }
    void se(int32_t value) {
        ue(value <= 0 ? uint32_t(-2 * int64_t(value)) : uint32_t(2 * int64_t(value) - 1));
    }
    void trailing() {
        bit(1);
        while (bits_ & 7) bit(0);
    }
    void byteAlign() { while (bits_ & 7) bit(0); }
    const std::vector<uint8_t> &rbsp() const { return bytes_; }

    // Annex B byte stream. The NAL header is two bytes: forbidden_zero_bit,
    // type, layer 0, temporal id 1. When emulate is false the RBSP is emitted
    // raw; the HuC/PAK insert path asks the hardware to insert emulation
    // prevention bytes itself.
    std::vector<uint8_t> annexB(uint8_t type, bool emulate = true) const {
        std::vector<uint8_t> out;
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        out.push_back(1);
        out.push_back(uint8_t(type << 1));
        out.push_back(1);
        unsigned zeros = 0;
        for (uint8_t b : bytes_) {
            if (emulate && zeros >= 2 && b <= 3) { out.push_back(3); zeros = 0; }
            out.push_back(b);
            zeros = b == 0 ? zeros + 1 : 0;
        }
        return out;
    }

private:
    void bit(unsigned value) {
        acc_ = (acc_ << 1) | (value & 1);
        if (++bits_ == 8) { bytes_.push_back(uint8_t(acc_)); acc_ = 0; bits_ = 0; }
    }
    std::vector<uint8_t> bytes_;
    uint32_t acc_ {}, bits_ {};
};
}
#endif
