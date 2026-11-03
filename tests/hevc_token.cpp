// CPU check for the VDENC sideband token that asks the kernel to resolve
// resource handles and patch their addresses into the command buffer.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../media/va_shim/hevc_token.cpp"

namespace {

using namespace VaShim;

void buildAndCheck() {
    HevcToken::Resource resources[HevcToken::kRegionSlots];
    for (uint32_t i = 0; i < HevcToken::kRegionSlots; ++i) {
        resources[i].handle = 10 + i;
        resources[i].cacheFlag = i & 1;
    }
    uint32_t token[HevcToken::kTotalDwords + 8];
    const uint32_t written = HevcToken::buildResolveToken(token,
        HevcToken::kTotalDwords, resources, HevcToken::kRegionSlots, 0);
    assert(written == HevcToken::kTotalDwords);

    // Header: type in the low word, total size in the high word, and the
    // command offset in the second dword.
    assert(token[0] == (HevcToken::kType | (HevcToken::kTotalDwords << 16)));
    assert(token[1] == 0);

    const uint32_t *data = token + 2;
    assert(data[HevcToken::kResourceCount] == HevcToken::kRegionSlots);
    for (uint32_t i = 0; i < HevcToken::kRegionSlots; ++i) {
        const uint32_t base = HevcToken::kFirstRecord +
            i * HevcToken::kRecordStride;
        assert(data[base + HevcToken::kRecordHandle] == 10 + i);
        assert(data[base + HevcToken::kRecordCacheFlag] == (i & 1));
    }

    // The probe leaves the anchors at the start of the batch and points each
    // reference slot at the matching record.
    assert(data[HevcToken::kBatchAnchorDwords] == 0);
    assert(data[HevcToken::kSecondAnchorDwords] == 0);
    // The picture path moves both anchors past the executed batch.
    assert(HevcToken::buildResolveToken(token, HevcToken::kTotalDwords,
        resources, HevcToken::kRegionSlots, 0, 2048) ==
        HevcToken::kTotalDwords);
    assert(data[HevcToken::kBatchAnchorDwords] == 2048);
    assert(data[HevcToken::kSecondAnchorDwords] == 2048);
    for (uint32_t i = 0; i < HevcToken::kRegionSlots; ++i)
        assert(data[HevcToken::kFirstRegionIndex + i] == i);

    // The kernel reads these index fields; every one must name a real record.
    const uint32_t indexes[] = {770, 771, 790, 791, 792, 793, 796,
        814, 815, 816, 817, 818};
    for (uint32_t i = 0; i < sizeof(indexes) / sizeof(indexes[0]); ++i)
        assert(data[indexes[i]] < HevcToken::kRegionSlots);
    assert(data[770] == 1);
    assert(data[791] == 1);
    assert(data[814] == 1);
}

void readBack() {
    uint32_t token[HevcToken::kTotalDwords] {};
    HevcToken::Resource resources[2] = {{7, 0}, {8, 1}};
    assert(HevcToken::buildResolveToken(token, HevcToken::kTotalDwords,
        resources, 2, 0) == HevcToken::kTotalDwords);

    // Emulate the kernel writing the resolved value back into record 0.
    uint32_t *data = token + 2;
    data[HevcToken::kFirstRecord + HevcToken::kRecordAddressLow] = 0x12345000;
    data[HevcToken::kFirstRecord + HevcToken::kRecordAddressHigh] = 0x2;
    data[HevcToken::kFirstRecord + HevcToken::kRecordSize] = 4096;

    HevcToken::Resource read {};
    uint64_t address = 0;
    uint32_t size = 0;
    assert(HevcToken::readResolvedResource(token, 2, 0, &read, &address, &size));
    assert(read.handle == 7);
    assert(address == 0x212345000ull);
    assert(size == 4096);
    assert(!HevcToken::readResolvedResource(token, 2, 1, nullptr, nullptr,
        nullptr));
    assert(!HevcToken::readResolvedResource(token, 2, 2, nullptr, nullptr,
        nullptr));
}

void rejectsBadInput() {
    uint32_t token[HevcToken::kTotalDwords] {};
    HevcToken::Resource resources[1] = {{1, 0}};
    assert(HevcToken::buildResolveToken(nullptr, HevcToken::kTotalDwords,
        resources, 1, 0) == 0);
    assert(HevcToken::buildResolveToken(token, HevcToken::kTotalDwords - 1,
        resources, 1, 0) == 0);
    assert(HevcToken::buildResolveToken(token, HevcToken::kTotalDwords,
        resources, 0, 0) == 0);
    assert(HevcToken::buildResolveToken(token, HevcToken::kTotalDwords,
        resources, HevcToken::kMaxResources + 1, 0) == 0);
}

void additionalScratchResources() {
    // The picture now has more resources than the token's sixteen reference
    // patch slots. All records must still be emitted and readable.
    HevcToken::Resource resources[21] {};
    for (uint32_t i = 0; i < 21; ++i)
        resources[i].handle = 100 + i;
    uint32_t token[HevcToken::kTotalDwords + 1] {};
    token[HevcToken::kTotalDwords] = 0xdeadbeef;
    assert(HevcToken::buildResolveToken(token, HevcToken::kTotalDwords,
        resources, 21, 12, 2048) == HevcToken::kTotalDwords);
    auto *data = token + 2;
    for (uint32_t i = 0; i < 21; ++i) {
        const auto record = HevcToken::kFirstRecord + i * HevcToken::kRecordStride;
        assert(data[record] == 100 + i);
        data[record + HevcToken::kRecordAddressLow] = 0x40000000 + i * 0x10000;
        uint64_t address = 0;
        assert(HevcToken::readResolvedResource(token, 21, i, nullptr, &address, nullptr));
        assert(address == 0x40000000 + i * 0x10000);
    }
    assert(data[HevcToken::kBatchAnchorDwords] == 2048);
    assert(token[HevcToken::kTotalDwords] == 0xdeadbeef);
}

}

int main() {
    buildAndCheck();
    readBack();
    rejectsBadInput();
    additionalScratchResources();
    printf("hevc_token: ok\n");
    return 0;
}
