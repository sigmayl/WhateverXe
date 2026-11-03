// SPDX-License-Identifier: MIT
#include "huc_commands.hpp"

namespace HevcCore {
namespace Huc {
namespace {

constexpr Field IndirectStreamOut {1, 4, 4};
constexpr Field FirmwareDescriptor {4, 0, 7};
constexpr Field DmemDestination {4, 6, 16};
constexpr Field DmemLengthField {5, 6, 16};

uint32_t commandWord(uint32_t command, uint32_t dwords) {
    // Gen media command headers count DWORDs after the header minus one.
    const uint32_t length = dwords - 2;
    return (3u << 29) | (2u << 27) | (11u << 23) | (command << 16) | length;
}

void setAlignedAddress(Packet &packet, uint32_t dword, uint32_t address,
    uint32_t mask) {
    // The macOS renderer hands out small resource handles (one per bound
    // resource) instead of byte addresses. They must survive unchanged; only
    // real graphics addresses are aligned.
    packet[dword] = address < 0x10000 ? address : (address & mask);
    packet[dword + 1] = 0;
}

}

Packet huCHeader(uint32_t command, uint32_t dwords) {
    Packet packet(dwords);
    packet[0] = commandWord(command, dwords);
    return packet;
}

Packet pipeModeSelect(bool indirectStreamOut) {
    Packet packet = huCHeader(0, 3);
    packet.set(IndirectStreamOut, indirectStreamOut ? 1 : 0);
    return packet;
}

Packet imemState(uint32_t kernelDescriptor) {
    Packet packet = huCHeader(1, 5);
    packet.set(FirmwareDescriptor, kernelDescriptor);
    return packet;
}

Packet dmemState(uint32_t address, uint32_t length, uint32_t offset) {
    Packet packet = huCHeader(2, 6);
    setAlignedAddress(packet, 1, address, ~(kDmemAlignment - 1));
    packet[3] = kMemoryAttributes;
    packet.set(DmemDestination, (offset / kDmemAlignment) & 0x7FF);
    packet.set(DmemLengthField,
        ((length + kDmemAlignment - 1) / kDmemAlignment) & 0x7FF);
    return packet;
}

Packet virtualAddressState(const Region *regions) {
    Packet packet = huCHeader(4, 49);
    for (uint32_t i = 0; i < kRegionCount; ++i) {
        const uint32_t base = 1 + i * 3;
        if (!regions[i].used)
            continue;
        setAlignedAddress(packet, base, regions[i].address + regions[i].offset,
            0xFFFFF000);
        packet[base + 2] = kMemoryAttributes;
    }
    return packet;
}

Packet start(bool lastStreamObject) {
    Packet packet = huCHeader(33, 2);
    packet[1] = lastStreamObject ? 1 : 0;
    return packet;
}

std::vector<Packet> kernelLoad(uint32_t kernelDescriptor) {
    Packet wait(1);
    wait[0] = 0x68000100; // MFX_WAIT, KIN and VRT
    std::vector<Packet> packets;
    packets.push_back(imemState(kernelDescriptor));
    packets.push_back(wait);
    packets.push_back(wait);
    packets.push_back(pipeModeSelect(false));
    packets.push_back(wait);
    return packets;
}

std::vector<Packet> pakIntegrate(const Params &params) {
    std::vector<Packet> packets;
    packets.push_back(imemState(params.kernelDescriptor));
    packets.push_back(pipeModeSelect(params.indirectStreamOut));
    packets.push_back(dmemState(params.dmemAddress, params.dmemLength,
        params.dmemOffset));
    packets.push_back(virtualAddressState(params.regions));
    packets.push_back(start(params.lastStreamObject));
    return packets;
}

std::vector<Packet> runKernel(const Params &params) {
    auto packets = kernelLoad(params.kernelDescriptor);
    packets[3] = pipeModeSelect(params.indirectStreamOut);
    packets.push_back(dmemState(params.dmemAddress, params.dmemLength,
        params.dmemOffset));
    packets.push_back(virtualAddressState(params.regions));
    packets.push_back(start(params.lastStreamObject));
    return packets;
}

uint32_t buildBrcSubmission(const BrcSubmission &s,
    uint32_t *destination, uint32_t capacityDwords) {
    const auto &p = s.params;
    if (!destination || !s.completionTag || s.statusAddress < 0x10000 ||
        (s.statusAddress & 7) || uint64_t(s.statusAddress) + 32 > 0x100000000ULL ||
        (p.kernelDescriptor != kHevcBrcInitKernel &&
            p.kernelDescriptor != kHevcBrcUpdateKernel))
        return 0;
    if (s.execute) {
        if (!s.authenticationVerified || s.dummyAddress < 0x10000 || (s.dummyAddress & 4095) ||
            s.dummyAddress > 0xffffe000 ||
            p.dmemAddress < 0x10000 || (p.dmemAddress & 63) || p.dmemLength != 192 ||
            uint64_t(p.dmemAddress) + 192 > 0x100000000ULL ||
            p.dmemOffset != kDmemOffset || p.indirectStreamOut || !p.lastStreamObject ||
            !p.regions[0].used || !p.regions[0].writable)
            return 0;
        for (const auto &r : p.regions) {
            if (r.used && (r.address < 0x10000 || (r.address & 4095) ||
                (r.offset & 4095) || uint64_t(r.address) + r.offset > 0xfffff000ULL))
                return 0;
        }
        if (p.kernelDescriptor == kHevcBrcUpdateKernel) {
            constexpr uint32_t required[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 15};
            for (uint32_t i : required)
                if (!p.regions[i].used) return 0;
            if (!p.regions[5].writable || !p.regions[6].writable || !p.regions[15].writable ||
                uint64_t(p.regions[3].address) + p.regions[3].offset ==
                    uint64_t(p.regions[5].address) + p.regions[5].offset)
                return 0;
        }
    }
    std::vector<Packet> packets;
    auto store = [&](uint32_t offset, uint32_t value) {
        Packet cmd(5);
        cmd[0] = 0x10000002; // single-DWORD MI_STORE_DATA_IMM + a trailing NOOP
        cmd[1] = s.statusAddress + offset;
        cmd[3] = value;
        packets.push_back(cmd);
    };
    auto read = [&](uint32_t reg, uint32_t offset, bool relative) {
        Packet cmd(4);
        cmd[0] = relative ? 0x12080002 : 0x12000002;
        cmd[1] = reg;
        cmd[2] = s.statusAddress + offset;
        packets.push_back(cmd);
    };
    auto flush = [&](bool completion) {
        Packet cmd(5);
        cmd[0] = 0x13000083; // MI_FLUSH_DW, video cache invalidate
        if (completion) {
            cmd[0] |= 1u << 14;
            cmd[1] = s.statusAddress + 24;
            cmd[3] = s.completionTag;
        }
        packets.push_back(cmd);
    };
    Packet wake(2);
    // Gen12 MI_FORCE_WAKEUP is two DWORDs: DwordLength = dwSize - 2.
    // An extra length DWORD consumes the following IMEM command header.
    wake[0] = 0x0e800000;
    wake[1] = 0x03000300;
    packets.push_back(wake);
    const auto prefix = kernelLoad(p.kernelDescriptor);
    packets.insert(packets.end(), prefix.begin(), prefix.end());
    store(0, kImemLoadedMask);
    read(kStatus2Register, 4, true);
    store(8, kAuthenticationMask);
    // Preserve the native proof in the readback instead of reading the
    // privileged GT register, which returns zero in a nonsecure CS batch.
    store(12, s.authenticationVerified ? kAuthenticationMask : 0);
    if (s.execute) {
        // Gen12 MI_CONDITIONAL_BATCH_BUFFER_END programming note: make SRM
        // writes coherent before the conditional fetch with four dummy SDIs.
        for (uint32_t i = 0; i < 4; ++i) store(28, 0);
        Packet gate(4);
        gate[0] = 0x1b284002; // MAD == IDD, masked, end all levels, PPGTT
        gate[1] = kImemLoadedMask;
        gate[2] = s.statusAddress;
        packets.push_back(gate);
        packets.push_back(dmemState(p.dmemAddress, p.dmemLength, p.dmemOffset));
        packets.push_back(virtualAddressState(p.regions));
        Packet indirect = huCHeader(5, 11);
        indirect[1] = s.dummyAddress;
        indirect[4] = s.dummyAddress + 4096;
        packets.push_back(indirect);
        Packet stream = huCHeader(32, 5);
        stream[1] = 1;
        stream[2] = 1u << 31; // HuC processing, no bitstream input/output
        packets.push_back(stream);
        packets.push_back(start(true));
        Packet drain(2);
        drain[0] = 0x77800000;
        drain[1] = 0x10011; // media-driver HuC completion: flush/wait HEVC
        packets.push_back(drain);
        flush(false);
    }
    read(kStatusRegister, 16, true);
    read(kStatus2Register, 20, true);
    flush(true);
    uint32_t words = 0;
    for (const auto &packet : packets) words += packet.size();
    const uint32_t total = (words + 2) & ~1u;
    if (total > capacityDwords) return 0;
    uint32_t at = 0;
    for (const auto &packet : packets)
        for (uint32_t i = 0; i < packet.size(); ++i) destination[at++] = packet[i];
    if (!(at & 1)) destination[at++] = 0;
    destination[at++] = 0x05000000;
    return at;
}

}
}
