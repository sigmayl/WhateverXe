// SPDX-License-Identifier: BSD-3-Clause
// macOS loader for the VA driver shim. The shipped Apple VA driver is kept
// next to this dylib as OriginalVA; all non-HEVC calls are forwarded to it.
#include "hevc_context.hpp"
#include "hevc_plugin.hpp"
#include "va_shim_core.hpp"

#include <stdint.h>

extern "C" void *dlopen(const char *path, int mode);
extern "C" void *dlsym(void *handle, const char *symbol);
extern "C" char *getenv(const char *name);
extern "C" size_t malloc_size(const void *pointer);
extern "C" void *pthread_self(void);
extern "C" void *pthread_get_stackaddr_np(void *thread);
extern "C" unsigned long pthread_get_stacksize_np(void *thread);
extern "C" long write(int fd, const void *buffer, unsigned long count);
extern "C" int open(const char *path, int flags, ...);
extern "C" int close(int fd);
// The encoder XPC service cannot reach the ASL client behind plain syslog();
// Apple's compatibility entry point is what the plug-in itself uses.
extern "C" void syslogExt(int priority, const char *format, ...)
    __asm("_syslog$DARWIN_EXTSN");

// IOSurface and IOAccelerator entry points the shipped driver already imports.
// Declaring them here lets the surface manager path read plane geometry and
// the driver buffer path read the graphics address of a resource.
extern "C" unsigned long IOSurfaceGetBytesPerRow(void *surface);
extern "C" unsigned long IOSurfaceGetBytesPerRowOfPlane(void *surface, unsigned long plane);
extern "C" void *IOSurfaceGetBaseAddressOfPlane(void *surface, unsigned long plane);
extern "C" unsigned long IOSurfaceGetWidth(void *surface);
extern "C" unsigned long IOSurfaceGetHeight(void *surface);
extern "C" unsigned long IOSurfaceGetPlaneCount(void *surface);
extern "C" unsigned long IOSurfaceGetWidthOfPlane(void *surface, unsigned long plane);
extern "C" unsigned long IOSurfaceGetHeightOfPlane(void *surface, unsigned long plane);
extern "C" uint32_t IOSurfaceGetPixelFormat(void *surface);
extern "C" void *IOSurfaceGetBaseAddress(void *surface);
extern "C" int IOSurfaceLock(void *surface, uint32_t options, uint32_t *seed);
extern "C" int IOSurfaceUnlock(void *surface, uint32_t options, uint32_t *seed);

struct DlInfo {
    const char *filename;
    void *base;
    const char *symbol;
    void *address;
};
extern "C" int dladdr(const void *address, DlInfo *info);
extern "C" uint32_t _dyld_image_count(void);
extern "C" const void *_dyld_get_image_header(uint32_t index);
extern "C" const char *_dyld_get_image_name(uint32_t index);

namespace {

constexpr int kRTLDLazy = 1;
constexpr int kRTLDLocal = 4;
constexpr int kO_WRONLY = 1;
constexpr int kO_CREAT = 0x200;
constexpr int kO_APPEND = 0x8;

constexpr const char *kDefaultOriginal =
    "/Library/GPUBundles/AppleIntelTGLGraphicsVADriver.bundle/Contents/MacOS/OriginalVA";
// The encoder XPC service is sandboxed away from /tmp. Try the writable
// locations a user-level XPC service usually keeps before giving up.
constexpr const char *kFallbackLogs[] = {
    "/Users/Shared/wex_va_shim.log",
    "/Library/Logs/wex_va_shim.log",
    "/private/var/tmp/wex_va_shim.log",
    "/tmp/wex_va_shim.log",
};

char gLine[256];
int gLogFd = -2;
bool gVideoLogged = false;
uintptr_t gOriginalBase;
// Set when the loaded image is the 16.0.32 build whose video-processing
// context layout this shim knows.
bool gKnownDriverBuild = false;

int32_t hucAuthenticated() {
    if (!gKnownDriverBuild)
        return 0;
    auto *scope = reinterpret_cast<void *>(-2);
    auto rootEntry = reinterpret_cast<uint32_t (*)(uint32_t)>(
        dlsym(scope, "IORegistryGetRootEntry"));
    auto property = reinterpret_cast<void *(*)(uint32_t, void *, void *, uint32_t)>(
        dlsym(scope, "IORegistryEntryCreateCFProperty"));
    auto releaseEntry = reinterpret_cast<int32_t (*)(uint32_t)>(
        dlsym(scope, "IOObjectRelease"));
    auto string = reinterpret_cast<void *(*)(void *, const char *, uint32_t)>(
        dlsym(scope, "CFStringCreateWithCString"));
    auto type = reinterpret_cast<unsigned long (*)(void *)>(dlsym(scope, "CFGetTypeID"));
    auto booleanType = reinterpret_cast<unsigned long (*)()>(dlsym(scope, "CFBooleanGetTypeID"));
    auto booleanValue = reinterpret_cast<unsigned char (*)(void *)>(dlsym(scope, "CFBooleanGetValue"));
    auto release = reinterpret_cast<void (*)(void *)>(dlsym(scope, "CFRelease"));
    if (!rootEntry || !property || !releaseEntry || !string || !type ||
        !booleanType || !booleanValue || !release)
        return 0;
    const uint32_t root = rootEntry(0);
    if (!root)
        return 0;
    void *key = string(nullptr, "WEXHuCFirmwareAuthenticated", 0x08000100);
    void *value = key ? property(root, key, nullptr, 0) : nullptr;
    const bool ready = value && type(value) == booleanType() && booleanValue(value);
    if (value) release(value);
    if (key) release(key);
    releaseEntry(root);
    return ready ? 1 : 0;
}

void append(char *&out, const char *text) {
    while (*text)
        *out++ = *text++;
}

void appendHex(char *&out, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    *out++ = '0';
    *out++ = 'x';
    char buffer[16];
    int count = 0;
    do {
        buffer[count++] = digits[value & 0xF];
        value >>= 4;
    } while (value);
    while (count)
        *out++ = buffer[--count];
}

bool joinPath(char *out, unsigned long size, const char *dir, const char *name) {
    unsigned long length = 0;
    while (dir[length])
        ++length;
    unsigned long nameLength = 0;
    while (name[nameLength])
        ++nameLength;
    const bool slash = length && dir[length - 1] != '/';
    if (length + (slash ? 1 : 0) + nameLength + 1 > size)
        return false;
    for (unsigned long i = 0; i < length; ++i)
        out[i] = dir[i];
    unsigned long offset = length;
    if (slash)
        out[offset++] = '/';
    for (unsigned long i = 0; i <= nameLength; ++i)
        out[offset + i] = name[i];
    return true;
}

int openLog() {
    char path[512];
    const char *explicitPath = getenv("WEX_VA_LOG");
    if (explicitPath && *explicitPath) {
        const int fd = open(explicitPath, kO_WRONLY | kO_CREAT | kO_APPEND, 0644);
        if (fd >= 0)
            return fd;
    }
    const char *dirs[] = {getenv("TMPDIR"), getenv("HOME")};
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        if (dirs[i] && *dirs[i] && joinPath(path, sizeof(path), dirs[i], ".wex_va_shim.log")) {
            const int fd = open(path, kO_WRONLY | kO_CREAT | kO_APPEND, 0644);
            if (fd >= 0)
                return fd;
        }
    }
    for (unsigned i = 0; i < sizeof(kFallbackLogs) / sizeof(kFallbackLogs[0]); ++i) {
        const int fd = open(kFallbackLogs[i], kO_WRONLY | kO_CREAT | kO_APPEND, 0644);
        if (fd >= 0)
            return fd;
    }
    return -1;
}

void emit(const char *line, unsigned long length) {
    if (gLogFd == -2)
        gLogFd = openLog();
    if (gLogFd >= 0)
        write(gLogFd, line, length);
    write(2, line, length);
    syslogExt(27, "%.*s", static_cast<int>(length), line);
}

void logMessage(const char *message) {
    char *out = gLine;
    append(out, "[WEX VA] ");
    append(out, message);
    *out++ = '\n';
    emit(gLine, static_cast<unsigned long>(out - gLine));
}

void appendDec(char *&out, uint64_t value) {
    char buffer[24];
    int count = 0;
    do {
        buffer[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        *out++ = buffer[--count];
}

void logDecimal(const char *label, uint64_t value) {
    char *out = gLine;
    append(out, "[WEX VA] ");
    append(out, label);
    append(out, "=");
    appendDec(out, value);
    *out++ = '\n';
    emit(gLine, static_cast<unsigned long>(out - gLine));
}

void logSlot(const char *phase, uint32_t slot, int32_t status) {
    char *out = gLine;
    append(out, "[WEX VA] slot ");
    appendDec(out, slot);
    append(out, " ");
    append(out, phase);
    if (status != VaShim::kTraceEnter) {
        append(out, " status=");
        if (status < 0) {
            *out++ = '-';
            appendDec(out, static_cast<uint32_t>(-static_cast<int64_t>(status)));
        } else {
            appendDec(out, static_cast<uint32_t>(status));
        }
    }
    *out++ = '\n';
    emit(gLine, static_cast<unsigned long>(out - gLine));
}

void logDescriptor(uint32_t index, const VaShim::ContextDesc *descriptor) {
    char *out = gLine;
    append(out, "[WEX VA] context ");
    appendDec(out, index);
    append(out, " type=");
    appendDec(out, descriptor->type);
    append(out, " subtype=");
    appendDec(out, descriptor->subtype);
    append(out, " id=");
    appendDec(out, descriptor->id);
    append(out, " size=");
    appendDec(out, descriptor->width);
    *out++ = 'x';
    appendDec(out, descriptor->height);
    *out++ = '\n';
    emit(gLine, static_cast<unsigned long>(out - gLine));
}

void logParamEntry(uint32_t index, const VaShim::ContextParamEntry *entry) {
    char *out = gLine;
    append(out, "[WEX VA] param ");
    appendDec(out, index);
    append(out, " type=");
    appendDec(out, entry->type);
    append(out, " subtype=");
    appendDec(out, entry->subtype);
    append(out, " id=");
    appendDec(out, entry->id);
    append(out, " ctx=");
    appendHex(out, reinterpret_cast<uintptr_t>(entry->context));
    append(out, " parameter=");
    appendDec(out, entry->parameter);
    *out++ = '\n';
    emit(gLine, static_cast<unsigned long>(out - gLine));
}

void logCommandDesc(uint32_t index, const VaShim::CommandDesc *desc) {
    char *out = gLine;
    append(out, "[WEX VA] command ");
    appendDec(out, index);
    append(out, " type=");
    appendDec(out, desc->type);
    append(out, " subtype=");
    appendDec(out, desc->subtype);
    append(out, " id=");
    appendDec(out, desc->id);
    *out++ = '\n';
    emit(gLine, static_cast<unsigned long>(out - gLine));
    const auto *words = reinterpret_cast<const uint64_t *>(desc);
    for (unsigned i = 0; i < 7; ++i) {
        char *line = gLine;
        append(line, "[WEX VA]   desc[");
        appendDec(line, i);
        append(line, "]=");
        appendHex(line, words[i]);
        *line++ = '\n';
        emit(gLine, static_cast<unsigned long>(line - gLine));
    }
}

void logPointer(const char *label, const void *value) {
    char *out = gLine;
    append(out, "[WEX VA] ");
    append(out, label);
    append(out, "=");
    appendHex(out, reinterpret_cast<uintptr_t>(value));
    *out++ = '\n';
    emit(gLine, static_cast<unsigned long>(out - gLine));
}

void logText(const char *label, const char *value) {
    char *out = gLine;
    append(out, "[WEX VA] ");
    append(out, label);
    append(out, "=");
    append(out, value);
    *out++ = '\n';
    emit(gLine, static_cast<unsigned long>(out - gLine));
}

bool loadOriginal();

// The HEVC plug-in waits on a process-global dispatch semaphore after each
// Schedule call. The native driver releases it from the media completion
// interrupt; this synchronous path reaches the same completion and calls the
// plug-in's own signalling stub. Offsets are from AppleGVAHEVCEncoder in
// macOS 15.8.2 and are checked against the timeout string and both function
// prologues first. Calling the stub lets the loader resolve the semaphore's
// actual address instead of assuming a shared-cache data layout.
constexpr uintptr_t kPluginTimeoutString = 0x316b0;
constexpr uintptr_t kPluginWaitFunction = 0xc7e2;
constexpr uintptr_t kPluginSignalFunction = 0x26578;
// __common slot that holds the completion semaphore.
constexpr uintptr_t kPluginSemaphoreGlobal = 0x35948;
constexpr const char *kPluginTimeoutText =
    "waitForIntelENCLastSubmissionCompletion timed out";

void (*gPluginCompletionSignal)();
uintptr_t gPluginImageBase;

bool textContains(const char *text, const char *needle) {
    for (unsigned long i = 0; text[i]; ++i) {
        unsigned long j = 0;
        while (needle[j] && text[i + j] == needle[j])
            ++j;
        if (!needle[j])
            return true;
    }
    return false;
}

bool bytesMatch(const uint8_t *data, const uint8_t *expected,
    unsigned long count) {
    for (unsigned long i = 0; i < count; ++i) {
        if (data[i] != expected[i])
            return false;
    }
    return true;
}

void locatePluginCompletionSignal() {
    if (gPluginCompletionSignal)
        return;
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = _dyld_get_image_name(i);
        if (!name || !textContains(name, "AppleGVAHEVCEncoder"))
            continue;
        const auto base = reinterpret_cast<uintptr_t>(_dyld_get_image_header(i));
        const auto *timeout = reinterpret_cast<const char *>(
            base + kPluginTimeoutString);
        const auto *waitCode = reinterpret_cast<const uint8_t *>(
            base + kPluginWaitFunction);
        const auto *signalCode = reinterpret_cast<const uint8_t *>(
            base + kPluginSignalFunction);
        // push rbp; mov rsp,rbp; push rbx; push rax; mov rdi,rbx
        static const uint8_t kWaitPrologue[] = {
            0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0x48, 0x89, 0xFB};
        // push rbp; mov rsp,rbp; mov rdi,[rip+disp32]; pop rbp; jmp ...
        static const uint8_t kSignalPrologue[] = {
            0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x3D};
        static const uint8_t kSignalTail[] = {0x5D, 0xE9};
        bool matched = bytesMatch(waitCode, kWaitPrologue,
            sizeof(kWaitPrologue));
        matched = matched && bytesMatch(signalCode, kSignalPrologue,
            sizeof(kSignalPrologue));
        matched = matched && bytesMatch(signalCode + 11, kSignalTail,
            sizeof(kSignalTail));
        for (unsigned long j = 0; matched && kPluginTimeoutText[j]; ++j) {
            if (timeout[j] != kPluginTimeoutText[j])
                matched = false;
        }
        if (!matched) {
            logMessage("HEVC plug-in completion layout not recognised");
            return;
        }
        gPluginCompletionSignal = reinterpret_cast<void (*)()>(
            base + kPluginSignalFunction);
        gPluginImageBase = base;
        logPointer("plugin completion signal", reinterpret_cast<void *>(
            gPluginCompletionSignal));
        return;
    }
    logMessage("HEVC encoder plug-in not loaded");
}

void signalPluginCompletion() {
    if (!gPluginCompletionSignal)
        locatePluginCompletionSignal();
    if (!gPluginCompletionSignal)
        return;
    // The stub loads the semaphore from the plug-in's __common slot. Skip the
    // call while that slot is still empty; signalling null would fault.
    if (!gPluginImageBase || !*reinterpret_cast<void *const *>(
            gPluginImageBase + kPluginSemaphoreGlobal))
        return;
    gPluginCompletionSignal();
}

// The shim serves more than one bundle name: AppleGVAHEVCEncoder derives a VA
// bundle path from the GVA codec generation, and the port presents the Gen11
// entry. Prefer the original driver that sits next to this image.
bool adjacentOriginal(char *out, unsigned long size) {
    DlInfo info {};
    if (!dladdr(reinterpret_cast<const void *>(&loadOriginal), &info) || !info.filename)
        return false;
    const char *file = info.filename;
    unsigned long length = 0;
    while (file[length])
        ++length;
    while (length && file[length - 1] != '/')
        --length;
    static const char kName[] = "OriginalVA";
    if (!length || length + sizeof(kName) > size)
        return false;
    for (unsigned long i = 0; i < length; ++i)
        out[i] = file[i];
    for (unsigned long i = 0; i < sizeof(kName); ++i)
        out[length + i] = kName[i];
    return true;
}

void event(void *, const char *message) {
    logMessage(message);
}

// Marker files opt the sandboxed encoder service into the experimental HEVC
// paths; the service does not inherit the user's environment.
int32_t hevcMarker(const char *path) {
    const int fd = open(path, 0);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

// CoreFoundation entry points used to create a plain scratch IOSurface. The
// shipped driver links the same frameworks, so the symbols resolve in the XPC
// service. CVPixelBufferCreate is used because the service allows IOSurface
// creation through CoreVideo's pixel-buffer path.
extern "C" int32_t CVPixelBufferCreate(void *allocator, unsigned long width,
    unsigned long height, uint32_t format, void *attributes, void **out);
extern "C" void *CVPixelBufferGetIOSurface(void *pixelBuffer);
extern "C" void *CFDictionaryCreate(void *allocator, const void **keys,
    const void **values, long count, const void *keyCallbacks,
    const void *valueCallbacks);
extern "C" void CFRelease(void *object);

// dlsym returns the address of the global variable; IOSurface keys are
// CFStringRefs stored in that variable, while the callback tables are structs
// whose address is what CFDictionaryCreate expects.
const void *cfString(const char *name) {
    auto *slot = static_cast<const void *const *>(
        dlsym(reinterpret_cast<void *>(-2), name));
    return slot ? *slot : nullptr;
}

const void *cfAddress(const char *name) {
    return dlsym(reinterpret_cast<void *>(-2), name);
}

void *hevcCreateSurface(uint32_t width, uint32_t height, uint32_t pitch,
    uint32_t format, void **owner) {
    (void)pitch;
    const void *surfaceKey = cfString("kCVPixelBufferIOSurfacePropertiesKey");
    const void *keyCallbacks = cfAddress("kCFTypeDictionaryKeyCallBacks");
    const void *valueCallbacks = cfAddress("kCFTypeDictionaryValueCallBacks");
    if (!surfaceKey || !keyCallbacks || !valueCallbacks) {
        logMessage("iosurface properties unavailable");
        return nullptr;
    }
    void *properties = CFDictionaryCreate(nullptr, nullptr, nullptr, 0,
        keyCallbacks, valueCallbacks);
    if (!properties) {
        logMessage("iosurface properties dictionary failed");
        return nullptr;
    }
    const void *keys[1] = {surfaceKey};
    const void *values[1] = {properties};
    void *attributes = CFDictionaryCreate(nullptr, keys, values, 1,
        keyCallbacks, valueCallbacks);
    CFRelease(properties);
    if (!attributes) {
        logMessage("pixel buffer attributes failed");
        return nullptr;
    }
    void *pixelBuffer = nullptr;
    const int32_t status = CVPixelBufferCreate(nullptr, width, height, format,
        attributes, &pixelBuffer);
    CFRelease(attributes);
    if (status != 0 || !pixelBuffer) {
        logDecimal("pixel buffer create status", static_cast<uint32_t>(status));
        return nullptr;
    }
    void *surface = CVPixelBufferGetIOSurface(pixelBuffer);
    if (!surface) {
        logMessage("pixel buffer has no IOSurface");
        return nullptr;
    }
    // The caller keeps the pixel buffer alive; the IOSurface is owned by it.
    if (owner)
        *owner = pixelBuffer;
    return surface;
}

// The TGL renderer keeps three initialized IOAccelVideoContext objects at
// driver+80/+88/+96. They own the data-buffer request and fence operations the
// shipped AVC path uses, and the HEVC context reuses the first one.
void logVideoContext(void *driver) {
    if (!driver)
        return;
    void *video = *(void **)(static_cast<char *>(driver) + 80);
    logPointer("createContexts video context", video);
    if (!video)
        return;
    logPointer("video accel context", *(void **)(static_cast<char *>(video) + 16));
    logDecimal("video connect", *(uint32_t *)(static_cast<char *>(video) + 24));
    logDecimal("video fence slots", *(uint32_t *)(static_cast<char *>(video) + 64));
}

int32_t createContexts(void *, void *driver, void *contexts, VaShim::CreateContextsFn forward) {
    logPointer("createContexts driver", driver);
    logPointer("createContexts list", contexts);
    // CreateContexts receives a pointer to a pointer to the context list.
    const auto *list = contexts
        ? *static_cast<const VaShim::ContextList *const *>(contexts)
        : nullptr;
    if (list) {
        logDecimal("createContexts count", list->count);
        if (list->descriptors) {
            for (uint32_t i = 0; i < list->count && i < 16; ++i) {
                const auto *descriptor =
                    static_cast<const VaShim::ContextDesc *>(list->descriptors[i]);
                if (descriptor)
                    logDescriptor(i, descriptor);
            }
        }
    }
    if (!list)
        return forward ? forward(driver, contexts) : VaShim::StatusInvalid;
    if (gKnownDriverBuild && list->descriptors &&
        VaShim::onlyUnsupportedContexts(list->descriptors, list->count)) {
        // The caller converts on the CPU when this fails; the shipped
        // driver's own transfer path cannot produce a frame on this GPU.
        logMessage("createContexts: video processing context refused");
        return VaShim::StatusInvalidParameter;
    }
    if (!forward)
        return VaShim::StatusInvalid;
    int32_t status;
    if (!(gKnownDriverBuild && VaShim::forwardSparseAvcContexts(driver,
            static_cast<VaShim::CreateContextsArgs *>(contexts), forward, status)))
        status = forward(driver, contexts);
    logDecimal("createContexts status", static_cast<uint32_t>(status));
    if (status != VaShim::StatusSuccess || !list)
        return status;

    // The shipped driver returns success for the HEVC encode descriptor but
    // creates no object. Publish a shim context through the handle array so
    // the plug-in passes it back in ContextParam and Schedule calls.
    if (list->descriptors && list->handles) {
        for (uint32_t i = 0; i < list->count && i < 16; ++i) {
            const auto *descriptor =
                static_cast<const VaShim::ContextDesc *>(list->descriptors[i]);
            if (!descriptor || descriptor->type != VaShim::kHevcEncodeContextType ||
                descriptor->subtype != VaShim::kHevcEncodeContextSubtype)
                continue;
            void *context = VaShim::createHevcEncodeContext(driver, descriptor);
            if (!context) {
                status = VaShim::StatusNoMemory;
                break;
            }
            list->handles[i] = context;
            logPointer("createContexts handle", context);
            if (!gVideoLogged) {
                gVideoLogged = true;
                logVideoContext(driver);
            }
        }
    }
    return status;
}

void trace(void *, uint32_t slot, int32_t status) {
    if (status == VaShim::kTraceEnter)
        logSlot("enter", slot, VaShim::kTraceEnter);
    else
        logSlot("exit", slot, status);
}

// Returns kHevcNotHandled when the list contains no HEVC encode context and
// the original driver should process it.
int32_t handleParamList(const char *label, void *params, bool get) {
    const auto *list = static_cast<const VaShim::ContextParamList *>(params);
    if (!list)
        return VaShim::kHevcNotHandled;
    logDecimal(label, list->count);
    if (!list->entries)
        return VaShim::kHevcNotHandled;
    const auto *entries =
        static_cast<const VaShim::ContextParamEntry *>(list->entries);

    bool ours = false;
    for (uint32_t i = 0; i < list->count && i < 16; ++i) {
        const VaShim::ContextParamEntry *entry = &entries[i];
        logParamEntry(i, entry);
        if (VaShim::isHevcEncodeContext(entry->context))
            ours = true;
        else if (!entry->context &&
            entry->type == VaShim::kHevcEncodeContextType &&
            entry->subtype == VaShim::kHevcEncodeContextSubtype)
            ours = true;
    }
    // A foreign entry the shipped driver would fault on. This happens on the
    // decode contexts of this GPU, where the per-engine table the
    // pixel-transfer path dereferences was never filled in. Report the
    // parameter as unsupported so the caller keeps its own conversion path and
    // the decoder service stays alive.
    if (!ours)
        return VaShim::kHevcNotHandled;

    // The shipped driver either looks the context up in its own table (slot
    // 12, returns 4) or dereferences the pointer unconditionally (slot 13).
    // Answer the HEVC entries here and leave the driver out of the path.
    int32_t status = VaShim::StatusSuccess;
    for (uint32_t i = 0; i < list->count && i < 16; ++i) {
        const VaShim::ContextParamEntry *entry = &entries[i];
        void *context = entry->context;
        if (!context)
            context = VaShim::findHevcEncodeContext(entry->id);
        if (!VaShim::isHevcEncodeContext(context)) {
            logMessage("context param: skipping foreign context");
            continue;
        }
        const int32_t result = get
            ? VaShim::hevcGetContextParam(context, entry)
            : VaShim::hevcSetContextParam(context, entry);
        if (result != VaShim::StatusSuccess)
            status = result;
    }
    return status;
}

int32_t contextParam(void *, void *driver, void *params, VaShim::ContextParamFn forward) {
    const int32_t handled = handleParamList("contextParam count", params, false);
    if (handled != VaShim::kHevcNotHandled)
        return handled;
    auto *rawParams = static_cast<uint32_t *>(params);
    return forward ? forward(driver, rawParams) : VaShim::StatusInvalid;
}

int32_t contextParam2(void *, void *driver, void *params, VaShim::ContextParam2Fn forward) {
    const int32_t handled = handleParamList("contextParam2 count", params, true);
    if (handled != VaShim::kHevcNotHandled)
        return handled;
    auto *rawParams = static_cast<uint32_t *>(params);
    return forward ? forward(driver, rawParams) : VaShim::StatusInvalid;
}

int32_t schedule(void *, void *driver, void *commandList, void *refCon,
    VaShim::ScheduleFn forward, uintptr_t caller, const void *nativeFrame,
    uintptr_t callerFrame) {
    const auto *list = static_cast<const VaShim::CommandList *>(commandList);
    if (!list)
        return forward ? forward(driver, static_cast<uint32_t *>(commandList), refCon)
                       : VaShim::StatusInvalid;
    logDecimal("schedule count", list->count);
    if (!list->descriptors)
        return forward ? forward(driver, static_cast<uint32_t *>(commandList), refCon)
                       : VaShim::StatusInvalid;

    bool allHevc = list->count != 0;
    for (uint32_t i = 0; i < list->count && i < 16; ++i) {
        const auto *node =
            static_cast<const VaShim::CommandNode *>(list->descriptors[i]);
        if (!node) {
            allHevc = false;
            break;
        }
        logCommandDesc(i, reinterpret_cast<const VaShim::CommandDesc *>(node));
        if (node->type != VaShim::kHevcEncodeContextType ||
            node->subtype != VaShim::kHevcEncodeContextSubtype) {
            allHevc = false;
            break;
        }
    }
    if (!allHevc) {
        auto *rawList = static_cast<uint32_t *>(commandList);
        const int32_t result = forward
            ? forward(driver, rawList, refCon) : VaShim::StatusInvalid;
        VaShim::hevcDumpScheduleResult(driver);
        return result;
    }

    // The shipped driver rejects contexts it did not create (slot 10 looks the
    // handle up in its table and returns 4). Dispatch HEVC commands directly
    // to the shim context.
    const auto *refCons = static_cast<const VaShim::RefConList *>(refCon);
    if (refCons) {
        logDecimal("schedule refCon count", refCons->count);
        logPointer("schedule refCon entries", refCons->entries);
    } else {
        logMessage("schedule refCon null");
    }
    int32_t status = VaShim::StatusSuccess;
    for (uint32_t i = 0; i < list->count && i < 16; ++i) {
        const auto *node =
            static_cast<const VaShim::CommandNode *>(list->descriptors[i]);
        void *context = node->context;
        if (!VaShim::isHevcEncodeContext(context))
            context = VaShim::findHevcEncodeContext(node->id);
        void *entry = nullptr;
        if (refCons && refCons->entries && i < refCons->count)
            entry = &refCons->entries[i];
        if (getenv("WEX_HEVC_PAYLOAD") || hevcMarker(
                "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_PAYLOAD") ||
            (hevcMarker("/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_BRC_INIT") ||
                hevcMarker("/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_BRC"))) {
            locatePluginCompletionSignal();
            VaShim::HevcPlugin::RateControlInput input;
            VaShim::HevcPlugin::RateControlReadStatus reason;
            const bool repairIntra = hevcMarker(
                "/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_BRC");
            const auto readRate = repairIntra ? VaShim::HevcPlugin::prepareRateControl
                : VaShim::HevcPlugin::readRateControl;
            VaShim::HevcPlugin::FrameBounds frameBounds;
            if (nativeFrame) {
                const auto thread = pthread_self();
                const uintptr_t upper = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(thread));
                const auto bytes = pthread_get_stacksize_np(thread);
                // The admitted caller keeps this frame on its active stack.
                // Exclude younger/freed stack storage below our own frame.
                const uintptr_t anchor = reinterpret_cast<uintptr_t>(&frameBounds) + sizeof(frameBounds);
                if (upper >= bytes && anchor < upper) {
                    frameBounds.lower = upper - bytes > anchor ? upper - bytes : anchor;
                    frameBounds.upper = upper;
                    frameBounds.callerFrame = callerFrame;
                }
            }
            const bool valid = readRate(node->payload,
                caller, reinterpret_cast<const uint8_t *>(gPluginImageBase),
                gKnownDriverBuild, true, malloc_size, input, &reason, nativeFrame, &frameBounds,
                hevcMarker("/Library/GPUBundles/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/WEX_HEVC_ACQP"));
            logPointer("BRC plugin caller", reinterpret_cast<void *>(caller));
            logPointer("BRC plugin image", reinterpret_cast<void *>(gPluginImageBase));
            VaShim::hevcSetRateControlInput(context, valid ? &input : nullptr);
            logDecimal("BRC plugin input valid", valid);
            logDecimal("BRC plugin input status", static_cast<uint32_t>(reason));
            if (valid) {
                logDecimal("BRC plugin mode", input.mode);
                logDecimal("BRC plugin target", input.targetRate);
                logDecimal("BRC plugin maximum", input.maximumRate);
                logDecimal("BRC plugin VBV", input.vbvSize);
                logDecimal("BRC plugin FPS numerator", input.frameRateNumerator);
                logDecimal("BRC plugin FPS denominator", input.frameRateDenominator);
                logDecimal("BRC plugin GOP", input.gopSize);
                logDecimal("BRC plugin QP", input.qp);
                logDecimal("BRC plugin quality valid", input.qualityValid);
            }
        }
        status = VaShim::hevcSchedule(context, node, entry);
        if (status != VaShim::StatusSuccess)
            return status;
    }
    return status;
}

VaShim::Shim gShim;
bool gReady = false;

bool loadOriginal() {
    char adjacent[512];
    const char *path = getenv("WEX_VA_ORIGINAL");
    if (!path || !*path) {
        if (!adjacentOriginal(adjacent, sizeof(adjacent)))
            path = kDefaultOriginal;
        else
            path = adjacent;
    }
    logText("original", path);

    void *handle = dlopen(path, kRTLDLazy | kRTLDLocal);
    if (!handle) {
        logMessage("original VA driver load failed");
        return false;
    }

    VaShim::Host host;
    host.create = reinterpret_cast<VaShim::CreateAcceleratorFn>(
        dlsym(handle, "AVD_CreateAVDAccelerator"));
    host.destroy = reinterpret_cast<VaShim::DestroyAcceleratorFn>(
        dlsym(handle, "AVD_DestroyAVDAccelerator"));
    if (!host.create || !host.destroy) {
        logMessage("original AVD entry points missing");
        return false;
    }

    DlInfo originalInfo {};
    if (dladdr(reinterpret_cast<const void *>(host.create), &originalInfo) && originalInfo.base) {
        gOriginalBase = reinterpret_cast<uintptr_t>(originalInfo.base);
        logPointer("original base", reinterpret_cast<void *>(gOriginalBase));
    }
    logPointer("original create", reinterpret_cast<void *>(host.create));

    // The shipped renderer's IOAccel data-buffer path is not exported. These
    // offsets are from the 16.0.32 TGL VA driver: request a data buffer,
    // submit the batch and wait for its fence. Verify the create entry point
    // first so a different driver build fails closed instead of calling
    // unrelated code.
    const uintptr_t createOffset =
        reinterpret_cast<uintptr_t>(host.create) - gOriginalBase;
    logDecimal("original create offset", createOffset);
    if (createOffset == 0x2B903) {
        gKnownDriverBuild = true;
        VaShim::HevcSubmitOps submitOps;
        submitOps.request = reinterpret_cast<int32_t (*)(void *, uint32_t, uint32_t, void **)>(
            gOriginalBase + 0x22708);
        submitOps.submit = reinterpret_cast<int32_t (*)(void *, uint32_t *)>(
            gOriginalBase + 0x227E0);
        submitOps.wait = reinterpret_cast<int32_t (*)(void *, uint32_t)>(
            gOriginalBase + 0x226E6);
        submitOps.registerSurface = reinterpret_cast<int32_t (*)(void *, void *)>(
            gOriginalBase + 0x9B2C);
        submitOps.surfaceAddress = reinterpret_cast<int32_t (*)(void *, void *, uint32_t, uint32_t *, uint32_t *)>(
            gOriginalBase + 0x9ACB);
        submitOps.attachShared = reinterpret_cast<int32_t (*)(void *, void *)>(
            gOriginalBase + 0x9AA7);
        submitOps.bindSurface = reinterpret_cast<int32_t (*)(void *, void *)>(
            gOriginalBase + 0x9E45);
        submitOps.surfacePitch = IOSurfaceGetBytesPerRow;
        submitOps.surfacePlanePitch = IOSurfaceGetBytesPerRowOfPlane;
        submitOps.surfacePlaneBase = IOSurfaceGetBaseAddressOfPlane;
        submitOps.surfaceLock = IOSurfaceLock;
        submitOps.surfaceUnlock = IOSurfaceUnlock;
        submitOps.surfacePlaneCount = IOSurfaceGetPlaneCount;
        submitOps.surfacePlaneWidth = IOSurfaceGetWidthOfPlane;
        submitOps.surfacePlaneHeight = IOSurfaceGetHeightOfPlane;
        submitOps.surfaceWidth = IOSurfaceGetWidth;
        submitOps.surfaceHeight = IOSurfaceGetHeight;
        submitOps.surfaceFormat = IOSurfaceGetPixelFormat;
        submitOps.surfaceBase = IOSurfaceGetBaseAddress;
        submitOps.surfaceBindAccel =
            reinterpret_cast<int32_t (*)(void *, uint32_t, uint32_t)>(
                dlsym(reinterpret_cast<void *>(-2), "IOSurfaceBindAccel"));
        submitOps.clientSharedPrivate =
            reinterpret_cast<void *(*)(void *, uint32_t)>(
                dlsym(reinterpret_cast<void *>(-2),
                    "IOAccelResourceGetClientSharedPrivate"));
        submitOps.createSurface = hevcCreateSurface;
        submitOps.resourceCreate =
            reinterpret_cast<void *(*)(void *, const void *, unsigned long)>(
                dlsym(reinterpret_cast<void *>(-2), "IOAccelResourceCreate"));
        submitOps.resourceClientShared =
            reinterpret_cast<void *(*)(void *, void **)>(
                dlsym(reinterpret_cast<void *>(-2),
                    "IOAccelResourceGetClientShared"));
        submitOps.resourceRelease =
            reinterpret_cast<void (*)(void *)>(
                dlsym(reinterpret_cast<void *>(-2), "IOAccelResourceRelease"));
        submitOps.debugMarker = hevcMarker;
        submitOps.hucAuthenticated = hucAuthenticated;
        VaShim::setHevcSubmitOps(submitOps);
    } else {
        logMessage("unsupported original driver build; submission disabled");
    }

    VaShim::setHevcLog(logMessage);
    VaShim::setHevcCompletionSignal(signalPluginCompletion);
    VaShim::Hooks hooks;
    hooks.event = event;
    hooks.createContexts = createContexts;
    hooks.trace = trace;
    hooks.contextParam = contextParam;
    hooks.contextParam2 = contextParam2;
    hooks.schedule = schedule;
    gShim.configure(host, hooks);
    logMessage("original VA driver loaded");
    return true;
}

}

extern "C" __attribute__((visibility("default")))
int32_t AVD_CreateAVDAccelerator(void **out, const void *config, uint32_t *rendererId) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (!gReady)
        gReady = loadOriginal();
    if (!gReady)
        return VaShim::StatusInvalid;
    const int32_t status = gShim.create(out, config, rendererId);
    if (status == VaShim::StatusSuccess && gKnownDriverBuild &&
        (getenv("WEX_HEVC_MAIN10") || hevcMarker(VaShim::HevcPlugin::kMain10Marker))) {
        locatePluginCompletionSignal();
        if (VaShim::HevcPlugin::enableMain10(out, rendererId, caller,
                reinterpret_cast<const uint8_t *>(gPluginImageBase),
                gKnownDriverBuild, true))
            logMessage("Main10 profile enabled for admitted HEVC plug-in");
    }
    return status;
}

extern "C" __attribute__((visibility("default")))
int32_t AVD_DestroyAVDAccelerator(void *accelerator) {
    if (!gReady)
        return VaShim::StatusInvalidParameter;
    const int32_t status = gShim.destroy(accelerator);
    VaShim::destroyHevcEncodeContexts();
    return status;
}
