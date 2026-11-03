// CPU check for the Gen12 HCP QM and FQM packets. Command order and field
// positions follow media-driver's HCP_QM_STATE and HCP_FQM_STATE builders.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/hevc_core/hw_command.cpp"
#include "../media/hevc_core/hw/mhw_vdbox_hcp_hwcmd_g12_X.cpp"
#include "../media/hevc_core/hcp_qm.cpp"

int main() {
    using namespace HevcCore;
    using namespace HevcCore::Hcp;

    QmParams defaults;
    auto qm = qmState(defaults);
    assert(qm.size() == 20);

    // 4x4 intra luma: flat 16 matrix, no DC coefficient.
    assert(qm[0].size() == 18);
    assert(qm[0][0] == 0x73840010);  // HCP command 4, length 16
    assert(qm[0][1] == 0x00000000);
    for (uint32_t i = 2; i < 6; ++i)
        assert(qm[0][i] == 0x10101010);
    for (uint32_t i = 6; i < 18; ++i)
        assert(qm[0][i] == 0);

    // 8x8 intra luma fills all 64 matrix bytes.
    assert(qm[6][1] == 0x00000002);  // size 1, intra, luma
    for (uint32_t i = 2; i < 18; ++i)
        assert(qm[6][i] == 0x10101010);

    // 16x16 intra luma carries the DC coefficient in DWORD 1 bits 12:5.
    assert(qm[12][1] == ((16u << 5) | (2u << 1)));

    // 32x32 has no chroma matrices: intra luma then inter luma.
    assert(qm.size() == 20);
    assert(qm[18][1] == ((16u << 5) | (3u << 1)));
    assert(qm[19][1] == ((16u << 5) | (3u << 1) | 1u));

    QmParams custom = defaults;
    custom.list4x4[1][2][3] = 40;
    custom.dc32x32[1] = 32;
    auto customQm = qmState(custom);
    assert((customQm[5][2] >> 24) == 40);              // 4x4 inter Cr byte 3
    assert(((customQm[19][1] >> 5) & 0xFF) == 32);     // 32x32 inter DC

    auto fqm = fqmState(defaults);
    assert(fqm.size() == 8);
    assert(fqm[0].size() == 34);
    assert(fqm[0][0] == 0x73850020);  // HCP command 5, length 32
    assert(fqm[0][1] == 0);
    // 4x4 uses 16 reciprocal entries followed by zeros.
    for (uint32_t i = 2; i < 10; ++i)
        assert(fqm[0][i] == 0x10001000);
    for (uint32_t i = 10; i < 34; ++i)
        assert(fqm[0][i] == 0);

    // 8x8 fills all 64 reciprocal entries.
    for (uint32_t i = 2; i < 34; ++i)
        assert(fqm[1][i] == 0x10001000);

    // 16x16 and 32x32 carry the DC reciprocal.
    assert(fqm[2][1] == ((0x1000u << 16) | (2u << 1)));
    assert(fqm[3][1] == ((0x1000u << 16) | (3u << 1)));
    assert(fqm[7][1] == ((0x1000u << 16) | (3u << 1) | 1u));

    QmParams customFqm = defaults;
    customFqm.list4x4[1][0][0] = 40;
    auto customFq = fqmState(customFqm);
    assert((customFq[4][2] & 0xFFFF) == reciprocalScalingValue(40));
    assert(reciprocalScalingValue(1) == 0xFFFF);
    assert(reciprocalScalingValue(16) == 0x1000);

    puts("PASS Gen12 HCP HEVC QM and FQM packets");
    return 0;
}
