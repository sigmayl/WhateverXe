// SPDX-License-Identifier: BSD-3-Clause
// Per-session ConstantBitRate support for the admitted Intel HEVC plug-in.
#ifndef WEX_HEVC_PROPERTIES_HPP
#define WEX_HEVC_PROPERTIES_HPP
#include "hevc_plugin.hpp"
#include <stddef.h>

namespace VaShim { namespace HevcProperties {
using Set = int32_t (*)(void *, const void *, const void *);
using Copy = int32_t (*)(void *, const void *, void *, void **);
using Supported = int32_t (*)(void *, void **);
using Finalize = void (*)(void *);
constexpr int32_t unsupported = -12900, invalid = -12902, allocation = -12904;
struct Ops {
    uintptr_t (*storage)(void *) {};
    bool (*equal)(const void *, const void *) {};
    unsigned long (*type)(const void *) {}, (*numberType)() {};
    unsigned char (*numberValue)(const void *, int, void *) {};
    void *(*number)(void *, int, const void *) {};
    void *(*dictionaryCopy)(void *, long, const void *) {};
    const void *(*dictionaryGet)(const void *, const void *) {};
    void (*dictionarySet)(void *, const void *, const void *) {};
    void (*release)(const void *) {};
    int32_t (*authenticated)() {};
    const void *constantKey {}, *averageKey {};
};

class Bridge {
    struct Entry { void *owner {}; uintptr_t config {}; uint32_t rate {}; };
    Entry entries[32] {};
    uint8_t busy {};
    struct Lock {
        uint8_t &flag;
        explicit Lock(uint8_t &f) : flag(f) {
            while (__atomic_test_and_set(&flag, __ATOMIC_ACQUIRE)) {}
        }
        ~Lock() { __atomic_clear(&flag, __ATOMIC_RELEASE); }
    };
    Entry *find(void *owner, bool create = false, uintptr_t config = 0) {
        Entry *empty = nullptr;
        for (auto &e : entries) {
            if (e.owner == owner) return &e;
            if (!e.owner && !empty) empty = &e;
        }
        if (create && empty) { empty->owner = owner; empty->config = config; }
        return create ? empty : nullptr;
    }
public:
    Ops ops;
    Set originalSet {};
    Copy originalCopy {};
    Supported originalSupported {};
    Finalize originalFinalize {};
    bool constantFor(uintptr_t config) {
        Lock lock(busy);
        for (const auto &e : entries)
            if (e.owner && e.config == config) return e.rate != 0;
        return false;
    }
    int32_t set(void *owner, const void *key, const void *value) {
        const bool constant = ops.equal(key, ops.constantKey);
        if (!constant) {
            const int32_t status = originalSet(owner, key, value);
            // The native setter silently ignores null and non-number values.
            if (!status && value && ops.equal(key, ops.averageKey) &&
                    ops.type(value) == ops.numberType()) {
                Lock lock(busy);
                if (auto *e = find(owner)) *e = {};
            }
            return status;
        }
        int64_t rate = 0;
        if (value && (ops.type(value) != ops.numberType() ||
                !ops.numberValue(value, 4, &rate) || rate < 0 || rate > INT32_MAX))
            return invalid;
        if (rate && ops.authenticated() != 1) return unsupported;
        const uintptr_t storage = ops.storage(owner);
        if (!storage || storage > UINTPTR_MAX - 136) return invalid;
        bool created = false;
        {
            Lock lock(busy);
            if (rate && !find(owner)) {
                if (!find(owner, true, storage + 136)) return allocation;
                created = true;
            }
        }
        void *zero = value ? nullptr : ops.number(nullptr, 4, &rate);
        if (!value && !zero) return allocation;
        const int32_t status = originalSet(owner, ops.averageKey, value ? value : zero);
        if (zero) ops.release(zero);
        {
            Lock lock(busy);
            if (auto *e = find(owner)) {
                if (!status) { if (rate) e->rate = uint32_t(rate); else *e = {}; }
                else if (created) *e = {};
            }
        }
        return status;
    }
    int32_t copy(void *owner, const void *key, void *allocator, void **out) {
        if (!ops.equal(key, ops.constantKey)) return originalCopy(owner, key, allocator, out);
        if (!out) return invalid;
        int64_t rate = 0;
        {
            Lock lock(busy);
            if (auto *e = find(owner)) rate = e->rate;
        }
        *out = ops.number(allocator, 4, &rate);
        return *out ? 0 : allocation;
    }
    int32_t supported(void *owner, void **out) {
        const int32_t status = originalSupported(owner, out);
        if (status || !out || !*out || ops.authenticated() != 1) return status;
        const void *attributes = ops.dictionaryGet(*out, ops.averageKey);
        if (!attributes) return status;
        void *copy = ops.dictionaryCopy(nullptr, 0, *out);
        if (!copy) { ops.release(*out); *out = nullptr; return allocation; }
        ops.dictionarySet(copy, ops.constantKey, attributes);
        ops.release(*out);
        *out = copy;
        return 0;
    }
    void finalize(void *owner) {
        {
            Lock lock(busy);
            if (auto *e = find(owner)) *e = {};
        }
        originalFinalize(owner);
    }
};

// The supported macOS 15.8.2 image uses packed CMBaseClass/encoder method
// records. Clone them; the loader publishes only their two aligned pointers.
struct Tables { uint8_t base[92] {}, encoder[128] {}; };
inline bool buildTables(const uint8_t *image, bool knownDriver, Tables &out,
    Set set, Copy copy, Supported supported, Finalize finalize) {
    if (!image || !knownDriver || !set || !copy || !supported || !finalize) return false;
    uint32_t header[8]; memcpy(header, image, sizeof(header));
    if (header[0] != 0xfeedfacf || header[1] != 0x01000007 || header[3] != 8 ||
        header[4] > 128 || header[5] > 0x10000) return false;
    const uint8_t uuid[] {0xfd,0x24,0xeb,0x7c,0x0b,0x0e,0x34,0x38,
        0xa4,0x3b,0x13,0x3e,0x93,0xe9,0x4e,0xfa};
    bool identity = false;
    uint32_t at = 32;
    for (uint32_t i = 0; i < header[4]; ++i) {
        uint32_t command[2];
        if (at + 8 > 32 + header[5]) return false;
        memcpy(command, image + at, 8);
        if (command[1] < 8 || command[1] > 32 + header[5] - at) return false;
        if (command[0] == 0x1b && command[1] == 24) {
            if (identity || !HevcPlugin::matches(image + at + 8, uuid, 16)) return false;
            identity = true;
        }
        at += command[1];
    }
    if (!identity || at != 32 + header[5]) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    auto pointer = [image](uint32_t at) { uintptr_t p; memcpy(&p, image + at, 8); return p; };
    auto word = [image](uint32_t at) { uint32_t v; memcpy(&v, image + at, 4); return v; };
    if (pointer(0x338d8) != 0 ||
        pointer(0x338e0) != base + 0x338f0 || pointer(0x338e8) != base + 0x3394c ||
        word(0x338f0) != 1 || pointer(0x338f4) != 20192 || word(0x3394c) != 2 ||
        pointer(0x33904) != base + 0x411f || pointer(0x3391c) != base + 0x42c9 ||
        pointer(0x33924) != base + 0x48bc || pointer(0x33968) != base + 0x6b17 ||
        pointer(0x33950) != base + 0x54fc || pointer(0x33958) != base + 0x5d1a ||
        pointer(0x33960) != base + 0x52c7) return false;
    static const uint8_t setter[] {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18,0x49,0x89,0xd6};
    static const uint8_t getter[] {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28,0x48,0x89,0xcb};
    static const uint8_t dict[] {0x48,0x85,0xf6,0x74,0x4d,0x55,0x48,0x89,0xe5,0x53,0x50};
    static const uint8_t finalizer[] {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,
        0x48,0x83,0xec,0x30,0x49,0x89,0xfe};
    if (!HevcPlugin::matches(image + 0x48bc, setter, sizeof(setter)) ||
        !HevcPlugin::matches(image + 0x42c9, getter, sizeof(getter)) ||
        !HevcPlugin::matches(image + 0x6b17, dict, sizeof(dict)) ||
        !HevcPlugin::matches(image + 0x411f, finalizer, sizeof(finalizer))) return false;
    Tables tables;
    memcpy(tables.base, image + 0x338f0, sizeof(tables.base));
    memcpy(tables.encoder, image + 0x3394c, sizeof(tables.encoder));
    memcpy(tables.base + 20, &finalize, 8);
    memcpy(tables.base + 44, &copy, 8);
    memcpy(tables.base + 52, &set, 8);
    memcpy(tables.encoder + 28, &supported, 8);
    out = tables;
    return true;
}
} }
#endif
