// SPDX-License-Identifier: BSD-3-Clause
// Platform-independent core of the AppleIntelTGLGraphicsVADriver shim. It
// loads the original accelerator, replaces its driver interface with
// forwarding slots, and gives the HEVC path a place to observe or replace
// CreateContexts without disturbing AVC or decode.
#ifndef WEX_MEDIA_VA_SHIM_VA_SHIM_CORE_HPP
#define WEX_MEDIA_VA_SHIM_VA_SHIM_CORE_HPP

#include "avd_abi.hpp"

namespace VaShim {

struct Host {
    CreateAcceleratorFn create {};
    DestroyAcceleratorFn destroy {};
    // Filled from the original accelerator during Shim::create().
    void *interface[kAVDInterfaceCount] {};
};

struct Hooks {
    void *user {};
    void (*event)(void *user, const char *message) {};
    // Runs before the original CreateContexts. A null hook forwards directly.
    // The hook may call forward() to preserve the original behaviour or
    // return its own status and create HEVC contexts itself.
    int32_t (*createContexts)(void *user, void *driver, void *contexts,
        CreateContextsFn forward) {};
    // Called around every forwarded interface slot. status is kTraceEnter
    // before the original call and its result afterwards.
    void (*trace)(void *user, uint32_t slot, int32_t status) {};
    // Run before the original ContextParam (slot 12) and ContextParam2
    // (slot 13). A null hook forwards directly. The hook may reject entries
    // whose context the shipped driver did not create, which would otherwise
    // dereference null.
    int32_t (*contextParam)(void *user, void *driver, void *params,
        ContextParamFn forward) {};
    int32_t (*contextParam2)(void *user, void *driver, void *params,
        ContextParam2Fn forward) {};
    // Runs before the original Schedule (slot 10). A null hook forwards
    // directly. The hook may reject command lists that target contexts the
    // shipped driver did not create, which would otherwise stall or fault.
    int32_t (*schedule)(void *user, void *driver, void *commandList,
        void *refCon, ScheduleFn forward, uintptr_t caller,
        const void *nativeFrame, uintptr_t callerFrame) {};
};

constexpr int32_t kTraceEnter = INT32_MIN;

class Shim {
public:
    Shim();

    // Installs the original entry points. Safe to call once before create().
    void configure(const Host &host, const Hooks &hooks);

    int32_t create(void **out, const void *config, uint32_t *rendererId);
    int32_t destroy(void *accelerator);

    const Host &original() const { return host_; }
    const Hooks &hooks() const { return hooks_; }

    // Used by the forwarding thunks.
    void *originalSlot(uint32_t index) const;

private:
    Host host_ {};
    Hooks hooks_ {};
};

// Returns the accelerator stored at driver+0 by AVD_DriverInterface::CreateDriver.
void *acceleratorOf(void *driver);

// True for the video-processing (pixel-transfer) context the shipped driver
// cannot service. It creates a composite state for only one of the renderer's
// three per-engine slots and leaves the slot its transfer path programs null,
// so the context either faults or produces an empty frame. The CreateContexts
// hook refuses it, which makes AppleGVA convert on the CPU instead.
bool isUnsupportedVideoProcessingContext(const ContextDesc *descriptor);

// True when the CreateContexts list contains only that context type. The call
// returns one status for the whole list, so a mixed list is forwarded.
bool onlyUnsupportedContexts(void *const *descriptors, uint32_t count);

// After an optional scaler fails, AppleGVA can leave holes in its AVC context
// list. The admitted donor rejects those holes before creating the encoder.
// Forward a bounded compact list, preserving capability output and the caller's
// handle indices. The platform hook must admit the donor before calling this.
// Returns false without changing outputs when this is not a sparse AVC list.
bool forwardSparseAvcContexts(void *driver, CreateContextsArgs *args,
    CreateContextsFn forward, int32_t &status);

}

#endif
