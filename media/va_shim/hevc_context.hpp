// SPDX-License-Identifier: BSD-3-Clause
// HEVC encode context for the TGL VA driver shim. The shipped driver creates
// no object for descriptor type 32/subtype 0x1000, so the shim supplies the
// object and the three interface methods the encoder plugin calls: set and get
// context parameters, and Schedule.
#ifndef WEX_MEDIA_VA_SHIM_HEVC_CONTEXT_HPP
#define WEX_MEDIA_VA_SHIM_HEVC_CONTEXT_HPP

#include "avd_abi.hpp"
#include "hevc_brc_plugin.hpp"

namespace VaShim {

// Diagnostic sink. Lines end with a newline; the sink owns formatting.
using HevcLogFn = void (*)(const char *line);
void setHevcLog(HevcLogFn log);
// Native adapter supplies a validated snapshot for the frame being scheduled.
int32_t hevcSetRateControlInput(void *context,
    const HevcPlugin::RateControlInput *input);

// Called once for every picture whose GPU fence and completion marker were
// observed. The plug-in waits on its own completion semaphore after Schedule;
// the native driver signals it from the media interrupt path, which this
// synchronous submission does not use.
void setHevcCompletionSignal(void (*signal)());

// Entry points of the shipped renderer's IOAccel data-buffer path. They are
// resolved from the loaded original driver when the shim starts.
struct HevcSubmitOps {
    int32_t (*request)(void *videoContext, uint32_t dwords, uint32_t auxDwords, void **descriptor) {};
    int32_t (*submit)(void *videoContext, uint32_t *fence) {};
    int32_t (*wait)(void *videoContext, uint32_t fence) {};
    // Renderer surface manager. registerSurface adds the IOSurface to the
    // pool; surfaceAddress returns the plane address from its client-shared
    // resource. Both are used to bind the picture payload's surfaces.
    int32_t (*registerSurface)(void *surfaceManager, void *surface) {};
    int32_t (*surfaceAddress)(void *surface, void *surfaceManager, uint32_t plane,
        uint32_t *address, uint32_t *value) {};
    // The surface manager needs its IOAccelShared reference and pixel-pool
    // geometry before it can create resources. The shipped driver does this
    // while creating a context; the shim does it for the HEVC context.
    int32_t (*attachShared)(void *shared, void *surfaceManager) {};
    // Marks a registered surface as in use and returns the IOSurface plane
    // geometry to the caller.
    int32_t (*bindSurface)(void *surfaceManager, void *surface) {};
    // IOSurface accessors used to fill the surface-state pitch and geometry.
    unsigned long (*surfacePitch)(void *surface) {};
    unsigned long (*surfacePlanePitch)(void *surface, unsigned long plane) {};
    void *(*surfacePlaneBase)(void *surface, unsigned long plane) {};
    unsigned long (*surfaceWidth)(void *surface) {};
    unsigned long (*surfaceHeight)(void *surface) {};
    unsigned long (*surfacePlaneCount)(void *surface) {};
    unsigned long (*surfacePlaneWidth)(void *surface, unsigned long plane) {};
    unsigned long (*surfacePlaneHeight)(void *surface, unsigned long plane) {};
    uint32_t (*surfaceFormat)(void *surface) {};
    // CPU access to a picture surface needs the IOSurface lock held.
    int32_t (*surfaceLock)(void *surface, uint32_t options, uint32_t *seed) {};
    int32_t (*surfaceUnlock)(void *surface, uint32_t options, uint32_t *seed) {};
    // Binds an IOSurface to the accelerator so the kernel maps it into the
    // media address space. The shipped AVC path calls this before using the
    // surface in a batch.
    int32_t (*surfaceBindAccel)(void *surface, uint32_t accelerator, uint32_t flags) {};
    // CPU mapping of a scratch surface. The HuC DMEM data and the second-level
    // batch are written through this pointer before submission.
    void *(*surfaceBase)(void *surface) {};
    // Kernel-side per-resource page. The public client-shared page publishes
    // the resource descriptor at +0x100; this one is checked for a 64-bit
    // graphics address next to it.
    void *(*clientSharedPrivate)(void *resource, uint32_t index) {};
    // Creates a plain IOSurface the surface manager can register. The returned
    // plane 0 address is used as the internal scratch buffer. owner receives
    // the retaining CVPixelBuffer, which the context keeps for its lifetime.
    void *(*createSurface)(uint32_t width, uint32_t height, uint32_t pitch,
        uint32_t format, void **owner) {};
    // Driver-owned linear resources. The shipped driver allocates its internal
    // command and scratch buffers through IOAccelResourceCreate with client VM
    // as the backing store; those resources publish a real graphics address at
    // clientShared+0x100, while IOSurface-backed resources publish only the
    // surface-manager texture id. The HuC virtual-address regions and the
    // media pipe-buffer addresses need the real address form.
    void *(*resourceCreate)(void *shared, const void *descriptor,
        unsigned long size) {};
    void *(*resourceClientShared)(void *resource, void **privateOut) {};
    void (*resourceRelease)(void *resource) {};
    // Debug markers let the encoder XPC service opt into the experimental
    // command path without inheriting the user's shell environment.
    int32_t (*debugMarker)(const char *path) {};
    // Kernel-owned authentication/firmware-lifetime proof, checked before
    // every HuC batch. IMEM's hardware gate then verifies current readiness.
    int32_t (*hucAuthenticated)() {};
};
void setHevcSubmitOps(const HevcSubmitOps &ops);

// Allocates and registers a context for an HEVC encode descriptor. The
// returned pointer is what CreateContexts must publish through its handle
// array. Returns null when the registry is full or allocation fails.
void *createHevcEncodeContext(void *driver, const ContextDesc *descriptor);

// True when the pointer is one of the shim-created contexts. The driver passes
// this pointer back in ContextParam entries and in command nodes.
bool isHevcEncodeContext(const void *context);

// Looks an existing context up by descriptor id, for command nodes whose
// context handle is still null.
void *findHevcEncodeContext(uint32_t id);

// SetContextParam (parameter 1 and 5) and GetContextParam (parameter 2).
int32_t hevcSetContextParam(void *context, const ContextParamEntry *entry);
int32_t hevcGetContextParam(void *context, const ContextParamEntry *entry);

// Schedule for one command node. Parses the picture payload, binds the input
// and reconstruction surfaces, builds the Gen12 VDEnc/HCP sequence, resolves
// its GPU resources and submits the picture sequence.
// refCon is the address of the caller's output pointer slot. On success it
// receives a context-owned render/media fence record, as in the native driver.
int32_t hevcSchedule(void *context, const CommandNode *command, void *refCon);

// Releases every registered context. Called when the accelerator goes away.
void destroyHevcEncodeContexts();

// Diagnostic for the stock AVC path. When WEX_HEVC_DUMP is set, scans the
// renderer's data buffers for HCP commands and logs their address fields. This
// answers whether media commands carry resource handles or resolved graphics
// addresses in this driver build.
void hevcDumpScheduleResult(void *driver);

// Returned by the parameter helpers when the entry does not belong to an HEVC
// context and the caller should forward the original call.
constexpr int32_t kHevcNotHandled = 0x7fffffff;

}

#endif
