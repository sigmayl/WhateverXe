// CPU check for the Gen12 VDENC picture-level packets. The expected word
// vectors were produced by compiling media-driver's generated
// mhw_vdbox_vdenc_hwcmd_g12_X definition (MIT, Intel Corporation) in a scratch
// directory and driving it with the same parameters the builders receive here.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "../media/hevc_core/hw_command.hpp"
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/vdenc_state.hpp"
#include "../media/hevc_core/vdenc_state.cpp"

namespace {
using HevcCore::Packet;

void expect(const Packet &packet, const std::vector<uint32_t> &words) {
    assert(packet.size() == words.size());
    for (uint32_t i = 0; i < words.size(); ++i) {
        if (packet[i] != words[i]) {
            fprintf(stderr, "DWORD %u: got %08x, want %08x\n", i, packet[i], words[i]);
            assert(false);
        }
    }
}

struct Registers {
    uint32_t dw, lo, hi;
    uint32_t operator()(const Packet &p) const {
        const uint32_t width = hi - lo + 1;
        return (p[dw] >> lo) & (width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1);
    }
};
}

int main() {
    using namespace HevcCore;
    using namespace HevcCore::VdEnc;

    VdEnc::PipeModeSelect mode;
    auto pipe = VdEnc::pipeModeSelect(mode);
    expect(pipe, {0x70800004, 0x01268080, 0x002B030A, 0x04010303, 0, 0});
    // Header: 6 DWORDs, DWORD_LENGTH counts the payload minus one.
    assert(Registers {0, 0, 11}(pipe) == 4);
    assert(Registers {0, 23, 26}(pipe) == 1);  // OPCODE_VDENCPIPE
    assert(Registers {0, 27, 28}(pipe) == 2);  // PIPELINE_MFXCOMMON
    assert(Registers {0, 29, 31}(pipe) == 3);  // PARALLELVIDEOPIPE
    assert(Registers {1, 15, 16}(pipe) == 1);  // 4:2:0
    assert(Registers {1, 12, 14}(pipe) == 0);  // 8-bit
    assert(Registers {1, 18, 18}(pipe) == 1);  // random access picture
    // PAK object stream-out stays off for a single-pass picture; the VDEnc
    // feeds the PAK over the internal path.
    assert(Registers {1, 6, 6}(pipe) == 0);
    assert(Registers {1, 7, 7}(pipe) == 1);    // TLB prefetch stays on
    // The G12 restriction: HME region prefetch must be clear while TLB
    // prefetch is enabled.
    assert(Registers {2, 0, 0}(pipe) == 0);
    // P010 selects a 10-bit pipeline and the P010 chroma type stays caller set.
    VdEnc::PipeModeSelect main10;
    main10.main10 = true;
    assert(Registers {1, 12, 14}(VdEnc::pipeModeSelect(main10)) == 2);

    VdEnc::SurfaceState source {1920, 1080, 1920, 1088};
    auto src = VdEnc::srcSurfaceState(source);
    expect(src, {0x70810004, 0, 0x10DC77F0, 0x20703BFB, 0x440, 0x440});
    assert(Registers {2, 4, 17}(src) == 1919);
    assert(Registers {2, 18, 31}(src) == 1079);
    assert(Registers {3, 3, 19}(src) == 1919);
    assert(Registers {3, 20, 22}(src) == 7);
    assert(Registers {3, 27, 31}(src) == 4);

    auto ref = VdEnc::refSurfaceState(source, 0);
    expect(ref, {0x70820004, 0, 0x10DC77F0, 0x20003BFB, 0x440, 0x440});
    assert(Registers {1, 0, 2}(ref) == 0);
    auto golden = VdEnc::refSurfaceState(source, 5);
    assert(Registers {1, 0, 2}(golden) == 5);

    VdEnc::SurfaceState ds4x {960, 540, 960, 544};
    VdEnc::SurfaceState ds8x {480, 270, 480, 272};
    expect(VdEnc::dsRefSurfaceState(ds4x, ds8x),
        {0x70830008, 0, 0x04341DF0, 0x20000EFB, 0x110, 0x110,
            0x086C3BF0, 0x20001DFB, 0x220, 0x220});

    VdEnc::PipeBufAddr buffers;
    buffers.originalUncompressed.address = 0x0000000112345000ull;
    buffers.streaminData.address = 0x0000000113000000ull;
    buffers.rowStoreScratch.address = 0x0000000114000000ull;
    buffers.colocatedMv.address = 0x0000000115000000ull;
    buffers.fwdRef0.address = 0x0000000116000000ull;
    buffers.bwdRef0.address = 0x0000000117000000ull;
    buffers.statisticsStreamout.address = 0x0000000118000000ull;
    buffers.cuRecordStreamout.address = 0x0000000119000000ull;
    buffers.lcuPakObjCmd.address = 0x000000011A000000ull;
    buffers.tileRowStore.address = 0x000000011B000000ull;
    auto buf = VdEnc::pipeBufAddrState(buffers);
    assert(buf.size() == 71);
    assert(Registers {0, 0, 11}(buf) == 69);
    // 64B aligned address pairs: bits 31:6 then bits 63:32.
    assert(buf[10] == 0x12345000 && buf[11] == 1);
    assert(buf[16] == 0x14000000 && buf[17] == 1);  // 0x114000000
    assert(buf[46] == 0x1A000000 && buf[47] == 1);
    assert(buf[61] == 0);                          // weights histogram offset
    assert(buf[62] == 0x1B000000 && buf[63] == 1);  // 0x11B000000
    // Control DWORD lands directly after each address pair.
    VdEnc::PipeBufAddr tagged = buffers;
    tagged.fwdRef0.control = 0x00003F00;
    assert(VdEnc::pipeBufAddrState(tagged)[24] == 0x00003F00);

    puts("PASS Gen12 VDENC HEVC picture state packets");
    return 0;
}
