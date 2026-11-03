// SPDX-License-Identifier: MIT
// Gen11/Gen12 HuC command packets used by the HEVC VDEnc PAK-integration
// kernel. Field layout follows media-driver's mhw_vdbox_huc_g11_X.h; the
// command headers are the Bh media instructions (opcode 11) with command type
// parallel video pipe.
#ifndef WEX_MEDIA_HEVC_CORE_HUC_COMMANDS_HPP
#define WEX_MEDIA_HEVC_CORE_HUC_COMMANDS_HPP

#include <stdint.h>
#include <vector>

#include "hw_command.hpp"

namespace HevcCore {
namespace Huc {

// Kernel descriptor 15 is the PAK-integration kernel the VDEnc HEVC encoder
// uses. The descriptor indexes a kernel that the kernel driver loaded into
// WOPCM; user space never uploads the binary itself.
constexpr uint32_t kPakIntegrationKernel = 15;
constexpr uint32_t kHevcBrcInitKernel = 8;
constexpr uint32_t kHevcBrcUpdateKernel = 9;

// Gen12 mhw_mmio_g12.h, offsets inside the executing VDBOX's CS window.
constexpr uint32_t kStatusRegister = 0x2000;
constexpr uint32_t kStatus2Register = 0x23b0;
constexpr uint32_t kImemLoadedMask = 1u << 6;
// i915 intel_guc_reg.h: Gen11+ authentication uses this GT register, not
// HUC_STATUS's bit 0. This address is absolute rather than CS-relative.
constexpr uint32_t kKernelLoadInfoRegister = 0xc1dc;
constexpr uint32_t kAuthenticationMask = 1;
constexpr uint32_t kGucStatusRegister = 0xc000;
constexpr uint32_t kGucWopcmSizeRegister = 0xc050;
constexpr uint32_t kGucWopcmOffsetRegister = 0xc340;
constexpr uint32_t kBiosWopcmRegister = 0x90e0;

// HUC_DMEM_OFFSET_RTOS_GEMS: DMEM window the kernel expects its data at.
constexpr uint32_t kDmemOffset = 0x2000;
constexpr uint32_t kDmemAlignment = 64;
constexpr uint32_t kRegionCount = 16;

// Cacheability attributes for the DMEM source and the virtual-address
// regions. Zero selects the uncached MOCS entry.
constexpr uint32_t kMemoryAttributes = 0;

// One HUC_VIRTUAL_ADDR_STATE region. The address must be 4KB aligned; offset
// is a byte offset inside that resource.
struct Region {
    uint32_t address {};
    uint32_t offset {};
    bool writable {false};
    bool used {false};
};

struct Params {
    uint32_t kernelDescriptor {kPakIntegrationKernel};
    uint32_t dmemAddress {};
    uint32_t dmemLength {};
    uint32_t dmemOffset {kDmemOffset};
    Region regions[kRegionCount] {};
    // Enables the HuC indirect stream-out object. The PAK-integration kernel
    // writes the bitstream through the virtual-address region instead, so the
    // encoder keeps this disabled.
    bool indirectStreamOut {false};
    // Last stream object flag for HUC_START.
    bool lastStreamObject {true};
};

// Bh media command header: command type 3 (parallel video pipe), pipeline 2,
// opcode 11, and the HuC command selector.
Packet huCHeader(uint32_t command, uint32_t dwords);

Packet pipeModeSelect(bool indirectStreamOut);
Packet imemState(uint32_t kernelDescriptor);
Packet dmemState(uint32_t address, uint32_t length, uint32_t offset);
Packet virtualAddressState(const Region *regions);
Packet start(bool lastStreamObject);

// Gen12 AddHucImemStateCmd waits after IMEM, and AddHucPipeModeSelectCmd
// waits before/after its mode switch. This prefix loads but does not run a
// firmware kernel; readiness can be sampled before issuing HUC_START.
std::vector<Packet> kernelLoad(uint32_t kernelDescriptor);

// BRC Init/Update use the waited Gen12 prefix before their DMEM/regions/start.
std::vector<Packet> runKernel(const Params &params);

struct BrcSubmission {
    Params params;
    // Intel HuCBrcDummyStreamObject supplies one byte through the HuC
    // indirect input even though BRC consumes its data from DMEM/regions.
    // This names a separate, retained 4 KiB graphics allocation.
    uint32_t dummyAddress {};
    // The native adapter verifies the kernel's firmware-ownership property
    // before issuing IMEM. GT authentication registers are privileged and
    // cannot be sampled by a nonsecure media batch. Hardware additionally
    // permits a valid IMEM load only while its authentication signal is set.
    bool authenticationVerified {false};
    // Eight DWORDs: IMEM mask/sample, authentication mask/sample, STATUS,
    // STATUS2 after the drain, completion tag and one coherency scratch word.
    uint32_t statusAddress {}, completionTag {};
    bool execute {true};
};

// Standalone Gen12 BRC batch. Require native authentication before IMEM, then
// sample IMEM and terminate the batch if it is absent. Four dummy stores provide
// the Gen12 SRM-to-conditional-command coherency sequence. A completed probe
// has a tag but no START; an aborted execution has samples and no tag.
// Only descriptors 8/9 and resolved, aligned graphics addresses are admitted.
// An invalid request or a short destination leaves the destination unchanged.
uint32_t buildBrcSubmission(const BrcSubmission &submission,
    uint32_t *destination, uint32_t capacityDwords);

// IMEM, pipe mode select, DMEM, virtual addresses and HUC_START, in the order
// media-driver's HucPakIntegrate emits them.
std::vector<Packet> pakIntegrate(const Params &params);

}
}

#endif
