// SPDX-License-Identifier: MIT
#include "../media/hevc_core/huc_brc_const.cpp"
#include <cassert>
#include <array>
#include <cstring>
#include <cstdio>

using namespace HevcCore::Huc;

static uint64_t checksum(const BrcConstantData &data) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&data);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < sizeof(data); ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ULL;
    return hash;
}

int main() {
    BrcSliceLayout slices[] {{496, 128, 496, 548, 564, 64},
        {624, 128, 624, 676, 696, 67}};
    BrcConstantParams params {false, 752, 1, slices};
    BrcConstantData data {};
    assert(buildBrcConstantData(params, data));
    assert(data.SADQPLambdaI[26] == 16 && data.SADQPLambdaP[51] == 292);
    assert(data.RDQPLambdaI[26] == 61 && data.RDQPLambdaP[51] == 21299);
    assert(data.SLCSZ_THRDELTAI_U16[0] == 400);
    assert(data.SLCSZ_THRDELTAI_U16[51] == 100);
    assert(data.Slice[0].SizeOfCMDs == 128);
    assert(data.Slice[0].SliceState_StartInBytes == 496);
    assert(data.Slice[0].SliceHeaderPIO_StartInBytes == 548);
    assert(data.Slice[0].VdencWeightOffset_StartInBytes == 564);
    assert(data.Slice[0].SliceHeader_SizeInBits == 64);
    assert(data.Slice[0].WeightTable_StartInBits == 0xffff);
    assert(data.Slice[0].HcpWeightOffsetL1_StartInBytes == 0xffff);
    assert(data.Slice[1].SizeOfCMDs == 0);
    assert(data.Slice[1].SliceState_StartInBytes == 0xffff);
    assert(data.Slice[69].SliceHeader_SizeInBits == 0);
    assert(data.UPD_TR_TargetSize_U32 == 0 && data.UPD_LA_TargetFulness_U32 == 0);

    // FNV-1a fixtures from Intel's wire declaration and original tables at
    // f55f1c8ea764453553d1cc5add705a32cdca712d, with no active slice layouts.
    auto baseline = data;
    baseline.Slice[0] = baseline.Slice[1];
    assert(checksum(baseline) == 0x446326055305fde5ULL);
    params.inter = true;
    assert(buildBrcConstantData(params, data));
    baseline = data;
    baseline.Slice[0] = baseline.Slice[1];
    assert(checksum(baseline) == 0x7360410cd9cd01bdULL);

    params.sliceCount = 2;
    assert(buildBrcConstantData(params, data));
    assert(data.Slice[1].SliceHeader_SizeInBits == 67);
    assert(data.Slice[1].SliceState_StartInBytes == 624);
    const auto valid = data;
    for (unsigned bad = 0; bad < 14; ++bad) {
        auto invalid = params;
        auto layouts = std::array<BrcSliceLayout, 2> {slices[0], slices[1]};
        invalid.slices = layouts.data();
        switch (bad) {
        case 0: invalid.slices = nullptr; break;
        case 1: invalid.sliceCount = 0; break;
        case 2: invalid.sliceCount = kBrcMaximumSlices + 1; break;
        case 3: invalid.batchBytes = 0x10000; break;
        case 4: invalid.batchBytes = 751; break;
        case 5: layouts[0].commandsBytes = 0; break;
        case 6: layouts[0].startOffset = 497; break;
        case 7: layouts[0].sliceStateOffset = 492; break;
        case 8: layouts[0].sliceHeaderOffset = 544; break;
        case 9: layouts[0].sliceHeaderBits = 0xffff; break;
        case 10: layouts[0].vdencWeightsOffset = 552; break;
        case 11: layouts[0].vdencWeightsOffset = 620; break;
        case 12: layouts[1].startOffset = 620; break;
        case 13: layouts[1].commandsBytes = 132; break;
        }
        assert(!buildBrcConstantData(invalid, data));
        assert(!std::memcmp(&data, &valid, sizeof(data)));
    }
    std::puts("PASS Gen12 BRC constant-data fixtures, slice layouts and range rejection");
}
