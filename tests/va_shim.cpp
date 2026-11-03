// CPU check for the VA driver shim core: accelerator wrapping, interface
// forwarding and the CreateContexts hook.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../media/va_shim/va_shim_core.cpp"

namespace {

using namespace VaShim;

Accelerator gAccelerator {};
int gCreateCalls;
int gDestroyCalls;
int gCreateDriverCalls;
int gDestroyDriverCalls;
int gCreateContextsCalls;
int gScheduleCalls;
uint32_t gLastRendererId;
void *gLastDriver;
uint32_t *gLastCommand;
void *gLastRefCon;

int32_t mockCreateDriver(void **driverOut, const CreateDriverParams *params);
int32_t mockDestroyDriver(void *driver);
int32_t mockCreateContexts(void *driver, void *contexts);
int32_t mockSchedule(void *driver, uint32_t *commandList, void *refCon);

void initTable() {
    gAccelerator.config = nullptr;
    gAccelerator.functions[0] = reinterpret_cast<void *>(&mockCreateDriver);
    gAccelerator.functions[1] = reinterpret_cast<void *>(&mockDestroyDriver);
    gAccelerator.functions[2] = reinterpret_cast<void *>(&mockCreateContexts);
    gAccelerator.functions[10] = reinterpret_cast<void *>(&mockSchedule);
    for (uint32_t i = 3; i < kAVDInterfaceCount; ++i) {
        if (i != 10)
            gAccelerator.functions[i] = reinterpret_cast<void *>(&mockSchedule);
    }
}

int32_t mockCreate(void **out, const void *, uint32_t *rendererId) {
    ++gCreateCalls;
    initTable();
    *out = &gAccelerator;
    *rendererId = kDaRendererId;
    return StatusSuccess;
}

int32_t mockDestroy(void *accelerator) {
    ++gDestroyCalls;
    assert(accelerator == &gAccelerator);
    return StatusSuccess;
}

int32_t mockCreateDriver(void **driverOut, const CreateDriverParams *params) {
    ++gCreateDriverCalls;
    assert(params->magic == -1);
    assert(params->accelerator == &gAccelerator);
    static uintptr_t driverStorage[4] {};
    driverStorage[0] = reinterpret_cast<uintptr_t>(params->accelerator);
    *driverOut = driverStorage;
    return StatusSuccess;
}

int32_t mockDestroyDriver(void *driver) {
    ++gDestroyDriverCalls;
    assert(driver != nullptr);
    return StatusSuccess;
}

int32_t mockCreateContexts(void *driver, void *contexts) {
    ++gCreateContextsCalls;
    assert(driver != nullptr);
    assert(contexts == reinterpret_cast<void *>(0x1234));
    return StatusSuccess;
}

int32_t mockSchedule(void *driver, uint32_t *commandList, void *refCon) {
    ++gScheduleCalls;
    gLastDriver = driver;
    gLastCommand = commandList;
    gLastRefCon = refCon;
    return StatusSuccess;
}

int32_t hookCalls;

int32_t contextsHook(void *user, void *driver, void *contexts, CreateContextsFn forward) {
    ++hookCalls;
    assert(user == reinterpret_cast<void *>(0x55));
    assert(driver != nullptr);
    assert(contexts == reinterpret_cast<void *>(0x1234));
    assert(forward != nullptr);
    return forward(driver, contexts);
}

void *gSparseDescriptors[2];
int gSparseCalls;
int32_t gSparseStatus;

int32_t mockSparseContexts(void *driver, void *opaque) {
    ++gSparseCalls;
    assert(driver == &gAccelerator);
    auto *args = static_cast<CreateContextsArgs *>(opaque);
    assert(args->list->count == 2 && args->list->reserved == 0x55);
    for (unsigned i = 0; i < 2; ++i) {
        assert(args->list->descriptors[i] == gSparseDescriptors[i]);
        args->list->handles[i] = reinterpret_cast<void *>(uintptr_t(0x100 + i));
    }
    args->capabilities = reinterpret_cast<void *>(0xCAFE);
    return gSparseStatus;
}

}

int main() {
    Host host;
    host.create = mockCreate;
    host.destroy = mockDestroy;

    Hooks hooks;
    hooks.user = reinterpret_cast<void *>(0x55);
    hooks.createContexts = contextsHook;
    hooks.schedule = [](void *, void *driver, void *commands, void *refCon,
        ScheduleFn forward, uintptr_t caller, const void *nativeFrame,
        uintptr_t callerFrame) -> int32_t {
        assert(caller);
        (void)callerFrame;
#if defined(__x86_64__)
        assert(nativeFrame == reinterpret_cast<void *>(0x12345678));
#else
        assert(!nativeFrame);
#endif
        return forward(driver, static_cast<uint32_t *>(commands), refCon);
    };

    Shim shim;
    shim.configure(host, hooks);

    void *accelerator = nullptr;
    uint32_t rendererId = 0;
    assert(shim.create(&accelerator, nullptr, &rendererId) == StatusSuccess);
    assert(accelerator == &gAccelerator);
    assert(gCreateCalls == 1);
    assert(rendererId == kDaRendererId);

    Accelerator *table = static_cast<Accelerator *>(accelerator);
    assert(table->functions[0] != reinterpret_cast<void *>(&mockCreateDriver));
    assert(table->functions[10] != reinterpret_cast<void *>(&mockSchedule));
    assert(table->functions[14] != nullptr);

    CreateDriverParams params {-1, 0, accelerator};
    void *driver = nullptr;
    auto createDriver = reinterpret_cast<CreateDriverFn>(table->functions[0]);
    assert(createDriver(&driver, &params) == StatusSuccess);
    assert(gCreateDriverCalls == 1);
    assert(driver != nullptr);
    assert(acceleratorOf(driver) == accelerator);

    uint32_t command = 0xCAFE;
    auto schedule = reinterpret_cast<ScheduleFn>(table->functions[10]);
#if defined(__x86_64__)
    register uintptr_t frameRegister __asm__("r14") = 0x12345678;
    __asm__ volatile("" : "+r"(frameRegister));
#endif
    assert(schedule(driver, &command, reinterpret_cast<void *>(0x77)) == StatusSuccess);
#if defined(__x86_64__)
    __asm__ volatile("" : "+r"(frameRegister));
    assert(frameRegister == 0x12345678);
#endif
    assert(gScheduleCalls == 1);
    assert(gLastDriver == driver && gLastCommand == &command &&
        gLastRefCon == reinterpret_cast<void *>(0x77));

    auto createContextsFn = reinterpret_cast<CreateContextsFn>(table->functions[2]);
    assert(createContextsFn(driver, reinterpret_cast<void *>(0x1234)) == StatusSuccess);
    assert(hookCalls == 1);
    assert(gCreateContextsCalls == 1);

    auto destroyDriver = reinterpret_cast<DestroyDriverFn>(table->functions[1]);
    assert(destroyDriver(driver) == StatusSuccess);
    assert(gDestroyDriverCalls == 1);

    assert(shim.destroy(accelerator) == StatusSuccess);
    assert(gDestroyCalls == 1);
    assert(shim.destroy(accelerator) == StatusInvalidParameter);

    // A shim without an original create function fails closed.
    Shim missing;
    Host empty;
    missing.configure(empty, Hooks {});
    assert(missing.create(&accelerator, nullptr, &rendererId) == StatusInvalid);

    // The video-processing context the CreateContexts hook refuses. Only
    // type 2 with subtype 0x800 is the pixel-transfer context the shipped
    // driver cannot service; decode, encode and other filters are forwarded.
    {
        ContextDesc descriptor {};
        descriptor.type = 2;
        descriptor.subtype = 0x800;
        assert(isUnsupportedVideoProcessingContext(&descriptor));
        descriptor.subtype = 0x200;
        assert(!isUnsupportedVideoProcessingContext(&descriptor));
        descriptor.subtype = 0x800;
        descriptor.type = 1;
        assert(!isUnsupportedVideoProcessingContext(&descriptor));
        descriptor.type = 32;
        assert(!isUnsupportedVideoProcessingContext(&descriptor));
        assert(!isUnsupportedVideoProcessingContext(nullptr));

        // The whole list is refused only when it consists entirely of that
        // transfer context; a mixed list (for example decode plus transfer)
        // is forwarded.
        ContextDesc decode {};
        decode.type = 1;
        decode.subtype = 0x800;
        ContextDesc transfer {};
        transfer.type = 2;
        transfer.subtype = 0x800;
        void *only[] = {&transfer};
        void *mixed[] = {&transfer, &decode};
        void *decoders[] = {&decode};
        assert(onlyUnsupportedContexts(only, 1));
        assert(!onlyUnsupportedContexts(mixed, 2));
        assert(!onlyUnsupportedContexts(decoders, 1));
        assert(!onlyUnsupportedContexts(nullptr, 1));
        assert(!onlyUnsupportedContexts(only, 0));
    }

    {
        ContextDesc avc {}, statistics {};
        avc.type = 32;
        avc.subtype = 0x4000;
        avc.id = 1001;
        statistics.type = 2;
        statistics.subtype = 0x1000;
        void *descriptors[] = {nullptr, &avc, nullptr, &statistics};
        void *handles[4] {};
        ContextList list {4, 0x55, descriptors, handles};
        CreateContextsArgs args {&list, nullptr};
        gSparseDescriptors[0] = &avc;
        gSparseDescriptors[1] = &statistics;
        const int32_t results[] {StatusSuccess, StatusDevice};
        for (int32_t result : results) {
            for (auto &handle : handles)
                handle = reinterpret_cast<void *>(0xDEAD);
            int32_t status = -1;
            gSparseStatus = result;
            assert(forwardSparseAvcContexts(&gAccelerator, &args,
                mockSparseContexts, status));
            assert(status == result && args.capabilities == reinterpret_cast<void *>(0xCAFE));
            assert(!handles[0] && !handles[2]);
            assert(handles[1] == reinterpret_cast<void *>(0x100));
            assert(handles[3] == reinterpret_cast<void *>(0x101));
            assert(args.list == &list && list.count == 4 && list.reserved == 0x55);
            assert(list.descriptors == descriptors && list.handles == handles);
            assert(!descriptors[0] && descriptors[1] == &avc &&
                !descriptors[2] && descriptors[3] == &statistics);
        }
        assert(gSparseCalls == 2);
        auto rejected = [&] {
            int32_t status = -123;
            assert(!forwardSparseAvcContexts(&gAccelerator, &args,
                mockSparseContexts, status));
            assert(status == -123 && gSparseCalls == 2);
            assert(args.capabilities == reinterpret_cast<void *>(0xCAFE));
            assert(handles[1] == reinterpret_cast<void *>(0x100));
        };
        avc.subtype = kHevcEncodeContextSubtype;
        rejected();
        avc.subtype = 0x4000;
        avc.id = 1002;
        rejected();
        avc.id = 1001;
        descriptors[2] = &avc; // Duplicate encoders are not admitted.
        rejected();
        descriptors[0] = &statistics;
        descriptors[2] = &statistics; // Dense lists keep the native path.
        rejected();
        descriptors[0] = descriptors[2] = nullptr;
        list.count = 17;
        rejected();
        list.count = 0;
        rejected();
        list.count = 4;
        list.descriptors = nullptr;
        rejected();
        list.descriptors = descriptors;
        list.handles = nullptr;
        rejected();
        list.handles = handles;
        args.list = nullptr;
        rejected();
        args.list = &list;
        int32_t status = -123;
        assert(!forwardSparseAvcContexts(nullptr, &args, mockSparseContexts, status));
        assert(!forwardSparseAvcContexts(&gAccelerator, nullptr, mockSparseContexts, status));
        assert(!forwardSparseAvcContexts(&gAccelerator, &args, nullptr, status));
        assert(gSparseCalls == 2 && status == -123);
    }

    puts("PASS VA driver shim forwarding core");
    return 0;
}
