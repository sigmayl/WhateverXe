// SPDX-License-Identifier: BSD-3-Clause
#include "va_shim_core.hpp"

#include <string.h>

namespace VaShim {
namespace {

constexpr uint32_t kMaxAccelerators = 8;

struct Entry {
    void *accelerator;
    Shim *shim;
};

Entry gEntries[kMaxAccelerators] {};
volatile int gLock = 0;

void lock() {
    while (__atomic_test_and_set(&gLock, __ATOMIC_ACQUIRE)) {
    }
}

void unlock() {
    __atomic_clear(&gLock, __ATOMIC_RELEASE);
}

Shim *find(void *accelerator) {
    if (!accelerator)
        return nullptr;
    lock();
    Shim *result = nullptr;
    for (uint32_t i = 0; i < kMaxAccelerators; ++i) {
        if (gEntries[i].accelerator == accelerator) {
            result = gEntries[i].shim;
            break;
        }
    }
    unlock();
    return result;
}

bool add(void *accelerator, Shim *shim) {
    lock();
    bool inserted = false;
    for (uint32_t i = 0; i < kMaxAccelerators; ++i) {
        if (!gEntries[i].accelerator) {
            gEntries[i].accelerator = accelerator;
            gEntries[i].shim = shim;
            inserted = true;
            break;
        }
    }
    unlock();
    return inserted;
}

void remove(void *accelerator) {
    lock();
    for (uint32_t i = 0; i < kMaxAccelerators; ++i) {
        if (gEntries[i].accelerator == accelerator) {
            gEntries[i].accelerator = nullptr;
            gEntries[i].shim = nullptr;
            break;
        }
    }
    unlock();
}

template <typename Fn>
Fn slot(Shim *shim, uint32_t index) {
    return reinterpret_cast<Fn>(shim->originalSlot(index));
}

void trace(Shim *shim, uint32_t index, int32_t status) {
    const Hooks &hooks = shim->hooks();
    if (hooks.trace)
        hooks.trace(hooks.user, index, status);
}

template <typename Fn, typename... Args>
int32_t forwardTraced(Shim *shim, uint32_t index, Fn forward, Args... args) {
    trace(shim, index, kTraceEnter);
    const int32_t status = forward ? forward(args...) : StatusInvalid;
    trace(shim, index, status);
    return status;
}

int32_t wrapCreateDriver(void **driverOut, const CreateDriverParams *params) {
    Shim *shim = find(params ? params->accelerator : nullptr);
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 0, slot<CreateDriverFn>(shim, 0), driverOut, params);
}

int32_t wrapDestroyDriver(void *driver) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 1, slot<DestroyDriverFn>(shim, 1), driver);
}

int32_t wrapCreateContexts(void *driver, void *contexts) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    const Hooks &hooks = shim->hooks();
    if (hooks.createContexts) {
        trace(shim, 2, kTraceEnter);
        const int32_t status = hooks.createContexts(
            hooks.user, driver, contexts, slot<CreateContextsFn>(shim, 2));
        trace(shim, 2, status);
        return status;
    }
    return forwardTraced(shim, 2, slot<CreateContextsFn>(shim, 2), driver, contexts);
}

int32_t wrapDestroyContexts(void *driver, uint32_t *handles) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 3, slot<DestroyContextsFn>(shim, 3), driver, handles);
}

int32_t wrapQuery(void *driver, int32_t *query) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 4, slot<QueryFn>(shim, 4), driver, query);
}

int32_t wrapCreateBuffer(void *driver, void *descriptor) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 5, slot<CreateBufferFn>(shim, 5), driver, descriptor);
}

int32_t wrapDestroyBuffer(void *driver, int32_t handle) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 6, slot<DestroyBufferFn>(shim, 6), driver, handle);
}

int32_t wrapSurfaceInfo(void *driver, uint32_t *query) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 7, slot<SurfaceInfoFn>(shim, 7), driver, query);
}

int32_t wrapSetParameter(void *driver, void *params) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 8, slot<SetParameterFn>(shim, 8), driver, params);
}

int32_t wrapGetParameter(void *driver, int32_t parameter) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 9, slot<GetParameterFn>(shim, 9), driver, parameter);
}

// Capture the admitted plug-in's callee-saved R14 before a C++ prologue can
// use it. sub_2611C keeps the current frame there across the AVD tail call.
// The native adapter independently validates the caller, instructions, object
// bounds and helper/frame link before reading it. Foreign calls only forward.
extern "C" __attribute__((noinline, visibility("hidden")))
int32_t wexVaScheduleCaptured(void *driver, uint32_t *commandList, void *refCon,
    uintptr_t caller, const void *nativeFrame, uintptr_t callerFrame) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    const Hooks &hooks = shim->hooks();
    if (hooks.schedule) {
        trace(shim, 10, kTraceEnter);
        const int32_t status = hooks.schedule(
            hooks.user, driver, commandList, refCon, slot<ScheduleFn>(shim, 10),
            caller, nativeFrame, callerFrame);
        trace(shim, 10, status);
        return status;
    }
    return forwardTraced(shim, 10, slot<ScheduleFn>(shim, 10), driver, commandList, refCon);
}

#if defined(__x86_64__)
__attribute__((naked)) int32_t wrapSchedule(void *, uint32_t *, void *) {
#if defined(__APPLE__)
    __asm__("movq (%rsp), %rcx\n\tmovq %r14, %r8\n\tmovq %rbp, %r9\n\tjmp _wexVaScheduleCaptured");
#else
    __asm__("movq (%rsp), %rcx\n\tmovq %r14, %r8\n\tmovq %rbp, %r9\n\tjmp wexVaScheduleCaptured");
#endif
}
#else
int32_t wrapSchedule(void *driver, uint32_t *commands, void *refCon) {
    return wexVaScheduleCaptured(driver, commands, refCon,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)), nullptr, 0);
}
#endif

int32_t wrapFence(void *driver, uint32_t *handles) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 11, slot<FenceFn>(shim, 11), driver, handles);
}

int32_t wrapContextParam(void *driver, uint32_t *params) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    const Hooks &hooks = shim->hooks();
    if (hooks.contextParam) {
        trace(shim, 12, kTraceEnter);
        const int32_t status = hooks.contextParam(
            hooks.user, driver, params, slot<ContextParamFn>(shim, 12));
        trace(shim, 12, status);
        return status;
    }
    return forwardTraced(shim, 12, slot<ContextParamFn>(shim, 12), driver, params);
}

int32_t wrapContextParam2(void *driver, uint32_t *params) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    const Hooks &hooks = shim->hooks();
    if (hooks.contextParam2) {
        trace(shim, 13, kTraceEnter);
        const int32_t status = hooks.contextParam2(
            hooks.user, driver, params, slot<ContextParam2Fn>(shim, 13));
        trace(shim, 13, status);
        return status;
    }
    return forwardTraced(shim, 13, slot<ContextParam2Fn>(shim, 13), driver, params);
}

int32_t wrapSubmit(void *driver, void *a, void *b, void *c, void *d, void *e) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 14, slot<SubmitFn>(shim, 14), driver, a, b, c, d, e);
}

int32_t wrapSync(void *driver, void *params) {
    Shim *shim = find(acceleratorOf(driver));
    if (!shim)
        return StatusInvalid;
    return forwardTraced(shim, 15, slot<SyncFn>(shim, 15), driver, params);
}

void *const kThunks[kAVDInterfaceCount] = {
    reinterpret_cast<void *>(&wrapCreateDriver),
    reinterpret_cast<void *>(&wrapDestroyDriver),
    reinterpret_cast<void *>(&wrapCreateContexts),
    reinterpret_cast<void *>(&wrapDestroyContexts),
    reinterpret_cast<void *>(&wrapQuery),
    reinterpret_cast<void *>(&wrapCreateBuffer),
    reinterpret_cast<void *>(&wrapDestroyBuffer),
    reinterpret_cast<void *>(&wrapSurfaceInfo),
    reinterpret_cast<void *>(&wrapSetParameter),
    reinterpret_cast<void *>(&wrapGetParameter),
    reinterpret_cast<void *>(&wrapSchedule),
    reinterpret_cast<void *>(&wrapFence),
    reinterpret_cast<void *>(&wrapContextParam),
    reinterpret_cast<void *>(&wrapContextParam2),
    reinterpret_cast<void *>(&wrapSubmit),
    reinterpret_cast<void *>(&wrapSync),
};

}

void *acceleratorOf(void *driver) {
    return driver ? *static_cast<void **>(driver) : nullptr;
}

bool isUnsupportedVideoProcessingContext(const ContextDesc *descriptor) {
    // The plug-in's video-processing descriptor: type 2 with subtype 0x800.
    // The driver builds a renderer for it but creates a composite state for
    // only one of the renderer's three per-engine slots; its transfer
    // parameter and command paths program the missing slot, so the context
    // either faults or submits an empty frame.
    constexpr uint32_t kVideoProcessingType = 2;
    constexpr uint64_t kVideoProcessingSubtype = 0x800;
    return descriptor && descriptor->type == kVideoProcessingType &&
        descriptor->subtype == kVideoProcessingSubtype;
}

bool onlyUnsupportedContexts(void *const *descriptors, uint32_t count) {
    constexpr uint32_t kMaxDescriptors = 64;
    if (!descriptors || count == 0 || count > kMaxDescriptors)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!isUnsupportedVideoProcessingContext(
                static_cast<const ContextDesc *>(descriptors[i])))
            return false;
    }
    return true;
}

bool forwardSparseAvcContexts(void *driver, CreateContextsArgs *args,
    CreateContextsFn forward, int32_t &status) {
    constexpr uint32_t kMaxContexts = 16;
    auto *list = args ? args->list : nullptr;
    if (!driver || !forward || !list || !list->descriptors || !list->handles ||
        !list->count || list->count > kMaxContexts)
        return false;
    bool avc = false, sparse = false;
    for (uint32_t i = 0; i < list->count; ++i) {
        const auto *descriptor = static_cast<const ContextDesc *>(list->descriptors[i]);
        if (!descriptor)
            sparse = true;
        else if (descriptor->type == 32) {
            // Keep HEVC and other encoder ABIs on their existing path.
            if (descriptor->subtype != 0x4000 || descriptor->id != 1001 || avc)
                return false;
            avc = true;
        }
    }
    if (!avc || !sparse)
        return false;

    void *descriptors[kMaxContexts] {}, *handles[kMaxContexts] {};
    uint32_t indices[kMaxContexts] {};
    ContextList compact {0, list->reserved, descriptors, handles};
    for (uint32_t i = 0; i < list->count; ++i) {
        if (list->descriptors[i]) {
            indices[compact.count] = i;
            descriptors[compact.count++] = list->descriptors[i];
        } else {
            list->handles[i] = nullptr;
        }
    }
    CreateContextsArgs forwarded {&compact, args->capabilities};
    status = forward(driver, &forwarded);
    args->capabilities = forwarded.capabilities;
    for (uint32_t i = 0; i < compact.count; ++i)
        list->handles[indices[i]] = handles[i];
    return true;
}

Shim::Shim() = default;

void Shim::configure(const Host &host, const Hooks &hooks) {
    host_ = host;
    hooks_ = hooks;
}

void *Shim::originalSlot(uint32_t index) const {
    return index < kAVDInterfaceCount ? host_.interface[index] : nullptr;
}

int32_t Shim::create(void **out, const void *config, uint32_t *rendererId) {
    if (!host_.create)
        return StatusInvalid;
    int32_t status = host_.create(out, config, rendererId);
    if (status != StatusSuccess || !out || !*out)
        return status;

    Accelerator *accelerator = static_cast<Accelerator *>(*out);
    for (uint32_t i = 0; i < kAVDInterfaceCount; ++i)
        host_.interface[i] = accelerator->functions[i];

    if (!add(accelerator, this)) {
        if (host_.destroy)
            host_.destroy(accelerator);
        return StatusNoMemory;
    }

    if (hooks_.event)
        hooks_.event(hooks_.user, "accelerator created");
    for (uint32_t i = 0; i < kAVDInterfaceCount; ++i)
        accelerator->functions[i] = kThunks[i];
    return StatusSuccess;
}

int32_t Shim::destroy(void *accelerator) {
    if (!accelerator || find(accelerator) != this)
        return StatusInvalidParameter;
    int32_t status = host_.destroy ? host_.destroy(accelerator) : StatusInvalid;
    remove(accelerator);
    if (hooks_.event)
        hooks_.event(hooks_.user, "accelerator destroyed");
    return status;
}

}
