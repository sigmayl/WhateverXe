// SPDX-License-Identifier: BSD-3-Clause
#include "../media/va_shim/hevc_properties.hpp"
#include <cassert>
#include <array>
#include <map>
#include <vector>
#include <cstdio>
#include <fstream>
#include <iterator>
using namespace VaShim::HevcProperties;
namespace {
struct Number { int64_t value; unsigned long type = 7; bool exact = true; };
struct Dictionary { std::map<const void *, const void *> values; };
int constantKey, averageKey, qualityKey, metadata;
int32_t nativeStatus, ready = 1;
unsigned calls, finalizations;
uintptr_t storage(void *owner) { return reinterpret_cast<uintptr_t>(owner); }
int32_t nativeSet(void *, const void *key, const void *) {
    assert(key != &constantKey); ++calls; return nativeStatus;
}
int32_t nativeCopy(void *, const void *, void *, void **out) {
    if (out) *out = nullptr; return unsupported;
}
int32_t nativeSupported(void *, void **out) {
    *out = new Dictionary{{{&averageKey, &metadata}}}; return 0;
}
void nativeFinalize(void *) { ++finalizations; }
void configure(Bridge &bridge) {
    bridge.originalSet = nativeSet; bridge.originalCopy = nativeCopy;
    bridge.originalSupported = nativeSupported; bridge.originalFinalize = nativeFinalize;
    auto &o = bridge.ops;
    o.storage = storage;
    o.equal = [](const void *a, const void *b) { return a == b; };
    o.type = [](const void *n) { return static_cast<const Number *>(n)->type; };
    o.numberType = []() -> unsigned long { return 7; };
    o.numberValue = [](const void *n, int t, void *v) -> unsigned char {
        assert(t == 4); *static_cast<int64_t *>(v) = static_cast<const Number *>(n)->value;
        return static_cast<const Number *>(n)->exact;
    };
    o.number = [](void *, int t, const void *v) -> void * {
        assert(t == 4); return new Number{*static_cast<const int64_t *>(v)};
    };
    o.dictionaryCopy = [](void *, long, const void *d) -> void * {
        return new Dictionary(*static_cast<const Dictionary *>(d));
    };
    o.dictionaryGet = [](const void *d, const void *key) -> const void * {
        auto &m = static_cast<const Dictionary *>(d)->values;
        auto i = m.find(key); return i == m.end() ? nullptr : i->second;
    };
    o.dictionarySet = [](void *d, const void *key, const void *value) {
        static_cast<Dictionary *>(d)->values[key] = value;
    };
    // set() releases only temporary CFNumbers; supported() releases a Dictionary.
    o.release = [](const void *n) { delete static_cast<const Number *>(n); };
    o.authenticated = []() -> int32_t { return ready; };
    o.constantKey = &constantKey; o.averageKey = &averageKey;
}
int32_t supportedHook(void *, void **) { return 0; }
void checkTables() {
    std::vector<uint8_t> image(0x34000);
    auto word = [&](size_t at, uint32_t v) { memcpy(image.data() + at, &v, 4); };
    auto ptr = [&](size_t at, uintptr_t v) { memcpy(image.data() + at, &v, 8); };
    auto bytes = [&](size_t at, std::initializer_list<uint8_t> v) {
        for (auto b : v) image[at++] = b;
    };
    const uintptr_t base = reinterpret_cast<uintptr_t>(image.data());
    word(0, 0xfeedfacf); word(4, 0x01000007); word(12, 8); word(16, 1); word(20, 24);
    word(32, 0x1b); word(36, 24);
    bytes(40, {0xfd,0x24,0xeb,0x7c,0x0b,0x0e,0x34,0x38,0xa4,0x3b,0x13,0x3e,0x93,0xe9,0x4e,0xfa});
    ptr(0x338e0, base + 0x338f0); ptr(0x338e8, base + 0x3394c);
    word(0x338f0, 1); ptr(0x338f4, 20192); word(0x3394c, 2);
    for (auto p : {std::pair<size_t,size_t>{0x33904,0x411f}, {0x3391c,0x42c9},
            {0x33924,0x48bc}, {0x33968,0x6b17}, {0x33950,0x54fc}, {0x33958,0x5d1a}, {0x33960,0x52c7}}) ptr(p.first, base + p.second);
    bytes(0x411f, {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x83,0xec,0x30,0x49,0x89,0xfe});
    bytes(0x48bc, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18,0x49,0x89,0xd6});
    bytes(0x42c9, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28,0x48,0x89,0xcb});
    bytes(0x6b17, {0x48,0x85,0xf6,0x74,0x4d,0x55,0x48,0x89,0xe5,0x53,0x50});
    Tables tables;
    auto build = [&](bool known = true) { return buildTables(image.data(), known,
        tables, nativeSet, nativeCopy, supportedHook, nativeFinalize); };
    assert(build());
    Set installed; memcpy(&installed, tables.base + 52, 8); assert(installed == nativeSet);
    const auto saved = tables;
    assert(!build(false));
    for (size_t at : {size_t(0),size_t(4),size_t(12),size_t(40),size_t(36),size_t(0x338e0),
            size_t(0x338f0),size_t(0x338f4),size_t(0x3394c),size_t(0x33904),
            size_t(0x3391c),size_t(0x33924),size_t(0x33968),size_t(0x33950),size_t(0x33958),size_t(0x33960),size_t(0x411f),size_t(0x48bc),size_t(0x42c9),size_t(0x6b17)}) {
        image[at] ^= 1; assert(!build()); image[at] ^= 1;
        assert(!memcmp(&tables, &saved, sizeof(tables)));
    }
}
}
void checkNativeTables(const char *path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)), {});
    assert(image.size() >= 0x34000);
    const uintptr_t base = reinterpret_cast<uintptr_t>(image.data());
    auto rebase = [&](size_t at) {
        uintptr_t address; memcpy(&address, image.data() + at, 8);
        if (address) { address += base; memcpy(image.data() + at, &address, 8); }
    };
    // Reproduce only dyld's pointer rebases for these two packed class tables.
    rebase(0x338e0); rebase(0x338e8);
    for (size_t at = 0x338f0 + 20; at + 8 <= 0x338f0 + 92; at += 8) rebase(at);
    for (size_t at = 0x3394c + 4; at + 8 <= 0x3394c + 128; at += 8) rebase(at);
    Tables tables;
    assert(buildTables(image.data(), true, tables,
        nativeSet, nativeCopy, supportedHook, nativeFinalize));
    // Other native methods must be preserved, including unaligned pointers.
    assert(!memcmp(tables.base + 28, image.data() + 0x338f0 + 28, 16));
    assert(!memcmp(tables.encoder + 4, image.data() + 0x3394c + 4, 24));
}
int main(int argc, char **argv) {
    Bridge b; configure(b);
    std::array<int, 34> owners {};
    void *owner = &owners[0], *second = &owners[1];
    const uintptr_t config = storage(owner) + 136;
    Number rate{8000000}, rate2{4000000};
    assert(!b.set(owner, &constantKey, &rate) && b.constantFor(config));
    assert(!b.constantFor(0) && !b.constantFor(storage(second) + 136));
    void *value;
    assert(!b.copy(owner, &constantKey, nullptr, &value));
    assert(static_cast<Number *>(value)->value == 8000000); delete static_cast<Number *>(value);
    assert(!b.set(second, &constantKey, &rate2));
    assert(!b.set(owner, &qualityKey, &rate2) && b.constantFor(config));
    Number wrongType{5,8};
    assert(!b.set(owner, &averageKey, nullptr) && b.constantFor(config));
    assert(!b.set(owner, &averageKey, &wrongType) && b.constantFor(config));
    nativeStatus = -123;
    assert(b.set(owner, &constantKey, &rate2) == -123 && b.constantFor(config));
    assert(b.set(owner, &averageKey, &rate2) == -123 && b.constantFor(config));
    nativeStatus = 0;
    const unsigned before = calls;
    for (Number bad : {Number{-1}, Number{INT64_MAX}, Number{5,8}, Number{5,7,false}})
        assert(b.set(owner, &constantKey, &bad) == invalid);
    ready = 0;
    assert(b.set(owner, &constantKey, &rate2) == unsupported && calls == before);
    // Capability discovery must not advertise CBR without authenticated HuC.
    assert(!b.supported(owner, &value));
    assert(!static_cast<Dictionary *>(value)->values.count(&constantKey));
    delete static_cast<Dictionary *>(value);
    ready = 1;
    b.ops.release = [](const void *p) { delete static_cast<const Dictionary *>(p); };
    assert(!b.supported(owner, &value));
    assert(static_cast<Dictionary *>(value)->values[&constantKey] == &metadata);
    delete static_cast<Dictionary *>(value);
    b.ops.dictionaryCopy = [](void *, long, const void *) -> void * { return nullptr; };
    assert(b.supported(owner, &value) == allocation && !value);
    configure(b);
    assert(!b.set(owner, &averageKey, &rate2) && !b.constantFor(config));
    assert(b.constantFor(storage(second) + 136));
    assert(!b.set(second, &constantKey, nullptr) && !b.constantFor(storage(second) + 136));
    assert(!b.set(owner, &constantKey, &rate));
    b.finalize(owner); assert(finalizations == 1 && !b.constantFor(config));
    // Reused owner addresses start with no CBR request.
    assert(!b.copy(owner, &constantKey, nullptr, &value));
    assert(!static_cast<Number *>(value)->value); delete static_cast<Number *>(value);
    nativeStatus = -123;
    for (auto &o : owners) assert(b.set(&o, &constantKey, &rate) == -123);
    nativeStatus = 0;
    for (unsigned i = 0; i < 32; ++i) assert(!b.set(&owners[i], &constantKey, &rate));
    assert(b.set(&owners[32], &constantKey, &rate) == allocation);
    b.finalize(&owners[0]);
    assert(!b.set(&owners[32], &constantKey, &rate));
    checkTables();
    if (argc == 2) checkNativeTables(argv[1]);
    else assert(argc == 1);
    puts("PASS software HEVC CBR property, session isolation and exact plug-in identity");
}
