// CPU check for the Gen11/Gen12 HuC PAK-integration command packets. Header
// words and field placement follow media-driver's mhw_vdbox_huc_g11_X.h.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/hevc_core/huc_commands.cpp"
#include "../media/hevc_core/hw_command.cpp"

int main() {
    using namespace HevcCore;
    using namespace HevcCore::Huc;

    auto pipe = pipeModeSelect(false);
    assert(pipe.size() == 3);
    assert(pipe[0] == 0x75800001);
    assert(pipe[1] == 0);
    assert(pipe[2] == 0);

    auto imem = imemState(kPakIntegrationKernel);
    assert(imem.size() == 5);
    assert(imem[0] == 0x75810003);
    assert(imem[4] == 15);
    const auto load = kernelLoad(kHevcBrcInitKernel);
    assert(load.size() == 5 && load[0][4] == 8);
    assert(load[1][0] == 0x68000100 && load[2][0] == 0x68000100);
    assert(load[3][0] == 0x75800001 && load[4][0] == 0x68000100);
    for (const auto &packet : load)
        assert(packet[0] != 0x75A10000); // no execution during readiness probe

    auto dmem = dmemState(0x402B8000, 192, kDmemOffset);
    assert(dmem.size() == 6);
    assert(dmem[0] == 0x75820004);
    assert(dmem[1] == 0x402B8000);
    assert(dmem[2] == 0);
    assert(dmem[3] == 0);
    assert(dmem[4] == (0x2000 / 64) << 6);
    assert(dmem[5] == 3 << 6);  // 192 bytes is three 64-byte lines

    Region regions[kRegionCount] {};
    regions[0].address = 0x402B8000;
    regions[0].used = true;
    regions[4].address = 0x40300000;
    regions[4].offset = 0x1000;
    regions[4].used = true;
    regions[15].address = 0x40400000;
    regions[15].used = true;

    auto vaddr = virtualAddressState(regions);
    assert(vaddr.size() == 49);
    assert(vaddr[0] == 0x7584002F);
    assert(vaddr[1] == 0x402B8000);
    assert(vaddr[2] == 0);
    assert(vaddr[1 + 4 * 3] == 0x40301000);
    assert(vaddr[1 + 15 * 3] == 0x40400000);
    // Unused regions stay zero.
    assert(vaddr[1 + 1 * 3] == 0);
    assert(vaddr[1 + 1 * 3 + 2] == 0);

    auto startCmd = start(true);
    assert(startCmd.size() == 2);
    assert(startCmd[0] == 0x75A10000);
    assert(startCmd[1] == 1);

    Params params;
    params.dmemAddress = 0x402B8000;
    params.dmemLength = 192;
    for (unsigned i = 0; i < kRegionCount; ++i)
        params.regions[i] = regions[i];

    const auto packets = pakIntegrate(params);
    assert(packets.size() == 5);
    uint32_t dwords = 0;
    for (const auto &packet : packets)
        dwords += packet.size();
    assert(dwords == 65);
    assert(packets[0][0] == 0x75810003);
    assert(packets[1][0] == 0x75800001);
    assert(packets[2][0] == 0x75820004);
    assert(packets[3][0] == 0x7584002F);
    assert(packets[4][0] == 0x75A10000);

    params.kernelDescriptor = kHevcBrcUpdateKernel;
    const auto brc = runKernel(params);
    assert(brc.size() == 8);
    assert(brc[0][4] == 9);
    assert(brc[1][0] == 0x68000100 && brc[2][0] == 0x68000100);
    assert(brc[3][0] == 0x75800001 && brc[4][0] == 0x68000100);
    assert(brc[5][0] == 0x75820004 && brc[6][0] == 0x7584002f);
    assert(brc[7][0] == 0x75a10000);

    printf("PASS Gen12 HuC PAK-integration commands\n");
    return 0;
}
