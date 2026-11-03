// Gen12 HuC BRC submissions, including IMEM/authentication gates before START.
// Reference: Intel media-driver HuCBrcInitReset/HuCBrcUpdate and the Gen12 MI
// command definitions, including the four-store SRM coherency sequence.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "../media/hevc_core/huc_commands.cpp"
#include "../media/hevc_core/hw_command.cpp"

int main() {
    using namespace HevcCore::Huc;
    BrcSubmission submission;
    submission.params.kernelDescriptor = kHevcBrcInitKernel;
    submission.params.dmemAddress = 0x400000;
    submission.params.dmemLength = 192;
    submission.params.regions[0] = {0x500000, 0, true, true};
    submission.dummyAddress = 0x900000;
    submission.authenticationVerified = true;
    submission.statusAddress = 0x600000;
    submission.completionTag = 17;
    uint32_t commands[256] {};
    const uint32_t words = buildBrcSubmission(submission, commands, 256);
    assert(words > 65 && words < 256 && !(words & 1));
    assert(commands[0] == 0x0e800000 && commands[1] == 0x03000300);
    uint32_t at = 2, startAt = words, gates = 0, storesBeforeGate = 0;
    bool imem = false, auth = false, dmem = false, regions = false, drained = false;
    bool dummy = false, stream = false;
    for (; at < words; ) {
        const uint32_t header = commands[at];
        if (!header || header == 0x68000100 || header == 0x05000000) { ++at; continue; }
        const uint32_t length = (header & (header >> 29 ? 0xffu : 0x3fu)) + 2;
        assert(at + length <= words);
        if (header == 0x75810003) assert(commands[at + 4] == 8);
        if (header == 0x12080002 && commands[at + 1] == 0x23b0 && !imem) {
            assert(commands[at + 2] == submission.statusAddress + 4); imem = true;
        }
        if (header == 0x10000002 && commands[at + 1] == submission.statusAddress + 12) {
            assert(commands[at + 3] == 1); auth = true;
        }
        if (header == 0x10000002 && !gates && imem && auth) ++storesBeforeGate;
        if (header == 0x1b284002) {
            assert(imem && auth && storesBeforeGate >= 4 && startAt == words);
            assert(commands[at + 1] == 64u);
            assert(commands[at + 2] == submission.statusAddress + gates * 8);
            ++gates;
        }
        if (header == 0x75820004) {
            assert(gates == 1 && commands[at + 1] == 0x400000);
            assert(commands[at + 4] == 0x2000 && commands[at + 5] == 192); dmem = true;
        }
        if (header == 0x7584002f) { assert(dmem); regions = true; }
        if (header == 0x75850009) {
            assert(regions && commands[at + 1] == submission.dummyAddress);
            assert(commands[at + 4] == submission.dummyAddress + 4096);
            dummy = true;
        }
        if (header == 0x75a00003) {
            assert(dummy && commands[at + 1] == 1 && commands[at + 2] == 0x80000000);
            assert(commands[at + 3] == 0 && commands[at + 4] == 0);
            stream = true;
        }
        if (header == 0x75a10000) { assert(gates == 1 && regions && stream); startAt = at; }
        if (header == 0x77800000) {
            assert(startAt < at && commands[at + 1] == 0x10011); drained = true;
        }
        if ((header & ~(1u << 14)) == 0x13000083 && (header & (1u << 14))) {
            assert(drained && commands[at + 1] == submission.statusAddress + 24);
            assert(commands[at + 3] == 17);
        }
        at += length;
    }
    assert(gates == 1 && stream && startAt < words && drained);

    // Probe mode must finish without executing a kernel or touching DMEM.
    submission.execute = false;
    submission.params.dmemAddress = submission.params.dmemLength = 0;
    submission.params.regions[0] = {};
    const uint32_t probeWords = buildBrcSubmission(submission, commands, 256);
    assert(probeWords && probeWords < words);
    for (uint32_t i = 0; i < probeWords; ++i) {
        assert(commands[i] != 0x75a10000 && commands[i] != 0x75820004);
    }
    submission.params.kernelDescriptor = kHevcBrcUpdateKernel;
    assert(buildBrcSubmission(submission, commands, 256));

    submission.execute = true;
    submission.params.dmemAddress = 0x400000;
    submission.params.dmemLength = 192;
    for (uint32_t i = 0; i < 9; ++i)
        submission.params.regions[i] = {0x700000 + i * 0x10000, 0,
            i == 0 || i == 5 || i == 6, true};
    submission.params.regions[15] = {0x800000, 0, true, true};
    assert(buildBrcSubmission(submission, commands, 256));
    submission.params.regions[4].used = false;
    assert(!buildBrcSubmission(submission, commands, 256));
    submission.params.regions[4].used = true;
    submission.params.regions[5] = submission.params.regions[3];
    submission.params.regions[5].writable = true;
    assert(!buildBrcSubmission(submission, commands, 256));
    submission.execute = false;

    // Invalid requests cannot truncate addresses or partially replace a batch.
    std::memset(commands, 0xcd, sizeof(commands));
    uint32_t saved[256]; std::memcpy(saved, commands, sizeof(saved));
    submission.params.kernelDescriptor = kPakIntegrationKernel;
    assert(!buildBrcSubmission(submission, commands, 256));
    submission.params.kernelDescriptor = kHevcBrcUpdateKernel;
    submission.statusAddress = 0xfffffff0;
    assert(!buildBrcSubmission(submission, commands, 256));
    submission.statusAddress = 0x600004;
    assert(!buildBrcSubmission(submission, commands, 256));
    submission.statusAddress = 0x600000;
    assert(!buildBrcSubmission(submission, commands, 1));
    submission.execute = true;
    submission.authenticationVerified = false;
    assert(!buildBrcSubmission(submission, commands, 256));
    assert(!std::memcmp(saved, commands, sizeof(saved)));
    submission.authenticationVerified = true;
    submission.params.dmemLength = 0;
    assert(!buildBrcSubmission(submission, commands, 256));
    assert(!std::memcmp(saved, commands, sizeof(saved)));
    std::puts("PASS HuC BRC readiness gates, drain/readback and transactional batch bounds");
}
