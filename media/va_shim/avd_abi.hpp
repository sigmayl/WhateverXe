// SPDX-License-Identifier: BSD-3-Clause
// Minimal AVD accelerator ABI used by AppleIntelTGLGraphicsVADriver and the
// AppleGVA encoders. The layout is recovered from the shipped 16.0.32 driver:
// AVD_CreateAVDAccelerator returns a 0x90 byte object whose first word is the
// configuration copy and whose following 16 words are the driver interface.
#ifndef WEX_MEDIA_VA_SHIM_AVD_ABI_HPP
#define WEX_MEDIA_VA_SHIM_AVD_ABI_HPP

#include <stdint.h>

namespace VaShim {

constexpr uint32_t kAVDAcceleratorSize = 0x90;
constexpr uint32_t kAVDInterfaceCount = 16;

// Renderer identity the paired TGL VA donor returns.
constexpr uint32_t kDaRendererId = 0x01080080;

enum Status : int32_t {
    StatusSuccess = 0,
    StatusError = 1,
    StatusNotFound = 3,
    StatusInvalid = 4,
    StatusInvalidParameter = 5,
    StatusNoMemory = 7,
    StatusIOKit = 9,
    StatusDevice = 10,
};

struct Accelerator {
    void *config;                     // +0
    void *functions[kAVDInterfaceCount]; // +8, the driver interface table
};

// AVD_DriverInterface::CreateDriver parameter block. The plugin passes a
// stack object whose first word is -1 and whose second word is the
// accelerator returned by AVD_CreateAVDAccelerator.
struct CreateDriverParams {
    int32_t magic;       // -1
    int32_t reserved;
    void *accelerator;   // +8
};

static_assert(sizeof(CreateDriverParams) == 16, "CreateDriver params are 16 bytes");

// CreateContexts argument. The plug-in passes a pointer to this header; each
// descriptor is the head of a linked list whose first node describes the
// context. Layout follows the shipped driver's renderer.
struct ContextList {
    uint32_t count;      // +0
    uint32_t reserved;   // +4
    void **descriptors;  // +8
    void **handles;      // +16, filled by the driver
};

// CreateContexts receives both the list pointer and a capability output. The
// caller uses the second word to decide whether initialization succeeded.
struct CreateContextsArgs {
    ContextList *list;
    void *capabilities;
};

static_assert(sizeof(CreateContextsArgs) == 16, "CreateContexts args are 16 bytes");

struct ContextDesc {
    uint32_t type;       // +0
    uint32_t reserved;   // +4
    uint64_t subtype;    // +8
    uint32_t id;         // +16, e.g. 1001-1003
    uint32_t reserved1;  // +20
    uint32_t reserved2;  // +24
    uint32_t reserved3;  // +28
    uint32_t width;      // +32
    uint32_t height;     // +36
    uint32_t reserved4[4]; // +40
    void *next;          // +56
};

static_assert(sizeof(ContextDesc) == 64, "context descriptor is 64 bytes");

// Context parameter calls (slots 12 and 13) pass a count and a pointer to a
// contiguous array of these entries. +24 is the driver-created context object;
// a missing context leaves it null, which the shipped driver dereferences
// without a check.
struct ContextParamList {
    uint32_t count;      // +0
    uint32_t reserved;   // +4
    void *entries;       // +8, first ContextParamEntry
};

struct ContextParamEntry {
    uint32_t type;       // +0
    uint32_t reserved;   // +4
    uint64_t subtype;    // +8
    uint32_t id;         // +16
    uint32_t reserved1;  // +20
    void *context;       // +24
    uint32_t parameter;  // +32
    uint32_t reserved2;  // +36
    void *value;         // +40
};

static_assert(sizeof(ContextParamEntry) == 48, "context parameter entry is 48 bytes");

// Schedule (slot 10) receives a count and an array of pointers to command
// descriptors. The first three fields identify the owning context.
struct CommandList {
    uint32_t count;      // +0
    uint32_t reserved;   // +4
    void **descriptors;  // +8
};

struct CommandDesc {
    uint32_t type;       // +0
    uint32_t reserved;   // +4
    uint64_t subtype;    // +8
    uint32_t id;         // +16
    uint32_t reserved1;  // +20
};

// Full command node passed to Schedule. The encoder plugin allocates 56 bytes
// per node: the context handle and the picture payload follow the identifying
// header, and a second node may be chained through +48.
struct CommandNode {
    uint32_t type;       // +0
    uint32_t reserved;   // +4
    uint64_t subtype;    // +8
    uint32_t id;         // +16
    uint32_t reserved1;  // +20
    void *context;       // +24, driver context handle
    void *payload;       // +32, picture parameters
    void *reserved2;     // +40
    void *next;          // +48, optional chained command
};

static_assert(sizeof(CommandNode) == 56, "command node is 56 bytes");

// Schedule's third argument: an array of per-command refCon pointers. The
// encoder passes one entry per command node.
struct RefConList {
    uint32_t count;      // +0
    uint32_t reserved;   // +4
    void **entries;      // +8
};

// The HEVC encode context the shipped TGL driver does not provide.
constexpr uint32_t kHevcEncodeContextType = 32;
constexpr uint64_t kHevcEncodeContextSubtype = 0x1000;

using CreateAcceleratorFn = int32_t (*)(void **out, const void *config, uint32_t *rendererId);
using DestroyAcceleratorFn = int32_t (*)(void *accelerator);

// Interface slots seen in the shipped driver. Later slots are forwarded with
// their exact argument counts but stay otherwise opaque.
using CreateDriverFn = int32_t (*)(void **driverOut, const CreateDriverParams *params);
using DestroyDriverFn = int32_t (*)(void *driver);
using CreateContextsFn = int32_t (*)(void *driver, void *contexts);
using DestroyContextsFn = int32_t (*)(void *driver, uint32_t *handles);
using QueryFn = int32_t (*)(void *driver, int32_t *query);
using CreateBufferFn = int32_t (*)(void *driver, void *descriptor);
using DestroyBufferFn = int32_t (*)(void *driver, int32_t handle);
using SurfaceInfoFn = int32_t (*)(void *driver, uint32_t *query);
using SetParameterFn = int32_t (*)(void *driver, void *params);
using GetParameterFn = int32_t (*)(void *driver, int32_t parameter);
using ScheduleFn = int32_t (*)(void *driver, uint32_t *commandList, void *refCon);
using FenceFn = int32_t (*)(void *driver, uint32_t *handles);
using ContextParamFn = int32_t (*)(void *driver, uint32_t *params);
using ContextParam2Fn = int32_t (*)(void *driver, uint32_t *params);
using SubmitFn = int32_t (*)(void *driver, void *a, void *b, void *c, void *d, void *e);
using SyncFn = int32_t (*)(void *driver, void *params);

}

#endif
