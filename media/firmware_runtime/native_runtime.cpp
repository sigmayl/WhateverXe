// SPDX-License-Identifier: BSD-3-Clause
// Optional authentication-only firmware owner. Native driver/PCI identities,
// every CPU/MMIO/GGTT mapping and firmware payload are validated before use.
#include "native_guard.hpp"
#include "../hevc_core/uc_runtime.hpp"
#include "../hevc_core/guc_ct.hpp"
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSSerialize.h>

using namespace HevcCore::Uc;
using namespace HevcFirmware;
static void *gFirmwareOwner;

static void flushMapped(void *memory, uint32_t bytes) {
    auto *p = static_cast<uint8_t *>(memory);
    for (uint32_t offset = 0; offset < bytes; offset += 64)
        __asm__ volatile("clflush (%0)" :: "r"(p + offset) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

class WhateverXeMediaRuntime : public IOService {
    OSDeclareDefaultStructors(WhateverXeMediaRuntime)
    enum BufferIndex { GuCImage, HuCImage, HuCRsa, Ads, Log, Ct, BufferCount };
    struct Buffer {
        OSObject *object {};
        uint8_t *cpu {};
        uint32_t bytes {}, gpu {};
    } buffers[BufferCount];
    IOService *owner {};
    OSObject *task {};
    IOWorkLoop *workloop {};
    OSSerializer *proof {};
    bool stopped {};
    uintptr_t base {};
    volatile uint8_t *mmio {};
    RuntimeState state;
    RuntimeConfig config;
    CtChannels channels;

    uint32_t read(uint32_t reg) { return *reinterpret_cast<volatile uint32_t *>(mmio + reg); }
    void write(uint32_t reg, uint32_t value) {
        *reinterpret_cast<volatile uint32_t *>(mmio + reg) = value;
        (void)read(reg);
    }
    void wake(bool acquire) {
        reinterpret_cast<void (*)(void *, bool, uint32_t)>(base + 0x28c2a)(owner,acquire,7);
    }
    bool ownerActive() const { return owner && task && mmio && !owner->isInactive() && !stopped; }
    bool reject(RuntimeFailure reason) { state.failure = reason; return false; }
    bool bindOwner(IOService *);
    bool allocate(BufferIndex, uint32_t);
    bool verifyBuffer(BufferIndex, bool canonical = true);
    bool prepareGgtt();
    bool preflight();
    bool mmioRequest(const uint32_t *, uint32_t, uint32_t);
    bool registerCt();
    bool authenticateHuC();
    bool poll(uint32_t, uint32_t, uint32_t, uint32_t);
    RuntimeOps operations();
    void publish();
    static IOReturn initializeAction(OSObject *, void *, void *, void *, void *);
    static IOReturn refreshAction(OSObject *, void *, void *, void *, void *);
    static bool serializeProof(void *, void *, OSSerialize *);
public:
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
};
OSDefineMetaClassAndStructors(WhateverXeMediaRuntime, IOService)

bool WhateverXeMediaRuntime::bindOwner(IOService *candidate) {
    if (!supportedKernel() || !argument("-allow3d") || !argument("-disablegfxfirmware") ||
        !argument("-wex-hevc-encode") || !argument("-wex-hevc-brc"))
        return reject(RuntimeFailure::Configuration);
    // Never replace another firmware lifetime, including one invalidated by
    // a reset. That owner's memory remains firmware-visible until reboot.
    if (gFirmwareOwner || IORegistryEntry::getRegistryRoot()->getProperty("WEXHuCFirmwareAuthenticated"))
        return reject(RuntimeFailure::BusyHardware);
    if (!candidate || candidate->isInactive() ||
        candidate->getMetaClass()->getClassSize() != 0x1c80 ||
        strcmp(candidate->getMetaClass()->getClassName(),"IntelAccelerator") ||
        !candidate->getProperty("IOGVAHEVCEncode")) return reject(RuntimeFailure::Ownership);
    auto pci = OSDynamicCast(IOPCIDevice,candidate->getProvider());
    if (!physicalMatches(pci)) return reject(RuntimeFailure::Ownership);
    base = donorBase(candidate);
    if (!base || IORegistryEntry::getRegistryRoot()->getProperty("TGLGGTT64Writes") != kOSBooleanTrue ||
        !nativeEntryMatches(base,0x10924,kFactoryFixture,sizeof(kFactoryFixture),true) ||
        !nativeEntryMatches(base,0x10b60,kCpuFixture,sizeof(kCpuFixture),false) ||
        !nativeEntryMatches(base,0x13e7c,kGpuFixture,sizeof(kGpuFixture),true) ||
        !nativeEntryMatches(base,0x8080,kKernelTaskFixture,sizeof(kKernelTaskFixture),false) ||
        !nativeEntryMatches(base,0x2e646,kWorkloopFixture,sizeof(kWorkloopFixture),false))
        return reject(RuntimeFailure::Ownership);
    auto bar = pci->getDeviceMemoryWithRegister(0x10);
    uintptr_t address = 0;
    if (!bar || bar->getLength() != 0x1000000 ||
        !copyMapped(reinterpret_cast<uintptr_t>(candidate) + 0x1240,&address,sizeof(address)) ||
        (address >> 48) != 0xffff || (address & 4095)) return reject(RuntimeFailure::Mapping);
    const uint32_t low = pci->configRead32(0x10), high = pci->configRead32(0x14);
    const uint64_t physical = uint64_t(low & ~15u) | uint64_t(high) << 32;
    IOByteCount span = 0;
    if ((low & 7) != 4 || !physical ||
        bar->getPhysicalSegment(0,&span,kIOMemoryMapperNone) != physical || span < 0x1000000)
        return reject(RuntimeFailure::Mapping);
    for (uint32_t offset = 0; offset < 0x1000000; offset += 4096)
        if ((uint64_t(pmap_find_phys(kernel_pmap,address + offset)) << 12) != physical + offset)
            return reject(RuntimeFailure::Mapping);
    owner = candidate; owner->retain();
    mmio = reinterpret_cast<volatile uint8_t *>(address);
    uintptr_t taskAddress = 0;
    if (!copyMapped(reinterpret_cast<uintptr_t>(owner) + 0x150,&taskAddress,sizeof(taskAddress)) ||
        !taskAddress || !pmap_find_phys(kernel_pmap,taskAddress)) return reject(RuntimeFailure::Ownership);
    task = reinterpret_cast<OSObject *>(taskAddress);
    if (strcmp(task->getMetaClass()->getClassName(),"IGAccelTask") ||
        !reinterpret_cast<bool (*)(void *)>(base + 0x8080)(task)) {
        task = nullptr;
        return reject(RuntimeFailure::Ownership);
    }
    task->retain();
    auto loop = reinterpret_cast<OSObject *(*)(void *)>(base + 0x2e646)(owner);
    workloop = OSDynamicCast(IOWorkLoop,loop);
    if (!workloop) return reject(RuntimeFailure::Ownership);
    workloop->retain();
    return true;
}

bool WhateverXeMediaRuntime::verifyBuffer(BufferIndex index, bool canonical) {
    const auto &b = buffers[index];
    if (!ownerActive() || !b.object || !b.cpu || (reinterpret_cast<uintptr_t>(b.cpu) & 4095) ||
        !validGucGgttRange(b.gpu,b.bytes,4096) || (b.bytes & 4095)) return reject(RuntimeFailure::Mapping);
    for (uint32_t offset = 0; offset < b.bytes; offset += 4096) {
        const uint64_t physical = uint64_t(pmap_find_phys(kernel_pmap,
            reinterpret_cast<uintptr_t>(b.cpu) + offset)) << 12;
        const uint64_t pte = *reinterpret_cast<volatile uint64_t *>(mmio + 0x800000 +
            ((uint64_t(b.gpu) + offset) >> 9));
        uint64_t expected = 0;
        if (!gen12SystemGgttEntry(pte,physical,expected) || (canonical && pte != expected))
            return reject(RuntimeFailure::Mapping);
    }
    return true;
}
bool WhateverXeMediaRuntime::allocate(BufferIndex index, uint32_t bytes) {
    if (!bytes || (bytes & 4095) || !ownerActive()) return reject(RuntimeFailure::Mapping);
    auto &b = buffers[index];
    auto factory = reinterpret_cast<OSObject *(*)(void *,uint64_t,uint32_t,uint32_t)>(base + 0x10924);
    b.object = factory(task,bytes,2,0);
    if (!b.object || b.object->getMetaClass()->getClassSize() != 0x48 ||
        strcmp(b.object->getMetaClass()->getClassName(),"IGSharedMappedBuffer"))
        return reject(RuntimeFailure::Mapping);
    uintptr_t fields[9];
    if (!copyMapped(reinterpret_cast<uintptr_t>(b.object),fields,sizeof(fields)) ||
        fields[2] != reinterpret_cast<uintptr_t>(owner) || fields[3] != reinterpret_cast<uintptr_t>(task) ||
        fields[4] != bytes || !(fields[5] & 255)) return reject(RuntimeFailure::Mapping);
    b.cpu = reinterpret_cast<uint8_t *(*)(void *)>(base + 0x10b60)(b.object);
    const uint64_t gpu = reinterpret_cast<uint64_t (*)(void *)>(base + 0x13e7c)(b.object);
    if (gpu > 0xffffffffu || (reinterpret_cast<uintptr_t>(b.cpu) >> 48) != 0xffff)
        return reject(RuntimeFailure::Mapping);
    b.bytes = bytes; b.gpu = uint32_t(gpu);
    // Populate the native demand-backed CPU mapping before resolving pages.
    // Its independent physical/GGTT proof still precedes every entry write.
    bzero(b.cpu,b.bytes);
    return verifyBuffer(index,false);
}
bool WhateverXeMediaRuntime::prepareGgtt() {
    for (uint32_t i = 0; i < BufferCount; ++i)
        if (!verifyBuffer(BufferIndex(i),false)) return false;
    for (const auto &b : buffers) for (uint32_t offset = 0; offset < b.bytes; offset += 4096) {
        auto pte = reinterpret_cast<volatile uint64_t *>(mmio + 0x800000 +
            ((uint64_t(b.gpu) + offset) >> 9));
        const uint64_t physical = uint64_t(pmap_find_phys(kernel_pmap,
            reinterpret_cast<uintptr_t>(b.cpu) + offset)) << 12;
        uint64_t expected = 0;
        if (!gen12SystemGgttEntry(*pte,physical,expected)) return reject(RuntimeFailure::Mapping);
        if (*pte != expected) {*pte = expected; (void)*pte;}
    }
    for (uint32_t i = 0; i < BufferCount; ++i)
        if (!verifyBuffer(BufferIndex(i))) return false;
    return true;
}
bool WhateverXeMediaRuntime::preflight() {
    if (!parseCssFirmware(kGuCImage,sizeof(kGuCImage),{70,49,4},config.guc) ||
        !parseCssFirmware(kHuCImage,sizeof(kHuCImage),{7,9,3},config.huc))
        return reject(RuntimeFailure::Configuration);
    GucAdsLayout layout;
    GucGen12GtInfo gt;
    WopcmLayout wopcm;
    if (!planGucAuthenticationAds(config.guc.privateDataBytes,layout) ||
        !buildGen12GucGtInfo(read(0x9138),read(0xd08),read(0x9140),1,gt) ||
        !planGen12Wopcm(config.huc.uploadBytes,config.guc.uploadBytes,read(0xc340),read(0xc050),wopcm))
        return reject(RuntimeFailure::Configuration);
    const uint32_t sizes[] {(uint32_t(sizeof(kGuCImage)) + 4095) & ~4095u,
        (uint32_t(sizeof(kHuCImage)) + 4095) & ~4095u,4096,layout.bytes,
        kGucAuthenticationLogBytes,kCtBlobBytes};
    for (uint32_t i = 0; i < BufferCount; ++i) if (!allocate(BufferIndex(i),sizes[i])) return false;
    for (uint32_t i = 0; i < BufferCount; ++i) for (uint32_t j = i + 1; j < BufferCount; ++j)
        if (!(uint64_t(buffers[i].gpu) + buffers[i].bytes <= buffers[j].gpu ||
            uint64_t(buffers[j].gpu) + buffers[j].bytes <= buffers[i].gpu))
            return reject(RuntimeFailure::Mapping);
    memcpy(buffers[GuCImage].cpu,kGuCImage,sizeof(kGuCImage));
    memcpy(buffers[HuCImage].cpu,kHuCImage,sizeof(kHuCImage));
    memcpy(buffers[HuCRsa].cpu,kHuCImage + config.huc.rsaOffset,config.huc.rsaBytes);
    if (!initializeGucAuthenticationAds(buffers[Ads].cpu,buffers[Ads].bytes,buffers[Ads].gpu,
            config.guc.privateDataBytes,gt,layout) ||
        !initializeGucAuthenticationLog(buffers[Log].cpu,buffers[Log].bytes) ||
        !initializeCtChannels(buffers[Ct].cpu,buffers[Ct].bytes,buffers[Ct].gpu,channels) ||
        !buildGen12Guc70AuthenticationParameters(config.guc,0xa780,4,
            buffers[Ads].gpu,buffers[Log].gpu,config.parameters))
        return reject(RuntimeFailure::Configuration);
    config.gucAddress = buffers[GuCImage].gpu; config.hucAddress = buffers[HuCImage].gpu;
    config.adsAddress = buffers[Ads].gpu; config.logAddress = buffers[Log].gpu;
    config.gucRsa = buffers[GuCImage].cpu + config.guc.rsaOffset;
    config.gucRsaBytes = config.guc.rsaBytes;
    return prepareGgtt();
}

bool WhateverXeMediaRuntime::poll(uint32_t reg,uint32_t mask,uint32_t expected,uint32_t milliseconds) {
    for (uint32_t i = 0; i <= milliseconds; ++i) {
        const uint32_t value = read(reg);
        if (value == 0xffffffffu) return false;
        if ((value & mask) == expected) return true;
        if (i != milliseconds) IOSleep(1);
    }
    return false;
}
bool WhateverXeMediaRuntime::mmioRequest(const uint32_t *request,uint32_t dwords,uint32_t expected) {
    if (!request || !dwords || dwords > 4 || (request[0] & 0xf0000000)) return false;
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        for (uint32_t i = 0; i < dwords; ++i) write(0x190240 + i * 4,request[i]);
        __asm__ volatile("mfence" ::: "memory");
        write(0x1901f0,1);
        if (!poll(0x190240,0x80000000,0x80000000,10)) return false;
        uint32_t header = read(0x190240);
        if (((header >> 28) & 7) == 3) {
            bool done = false;
            for (uint32_t i = 0; i <= 1000; ++i) {
                header = read(0x190240);
                if (!(header & 0x80000000)) return false;
                if (((header >> 28) & 7) != 3) {done = true; break;}
                if (i != 1000) IOSleep(1);
            }
            if (!done) return false;
        }
        if (((header >> 28) & 7) == 5) continue;
        return (header & 0xf0000000) == 0xf0000000 && (header & 0x0fffffff) == expected;
    }
    return false;
}
bool WhateverXeMediaRuntime::registerCt() {
    const uint32_t keys[] {0x0906,0x0905,0x0907,0x0903,0x0902,0x0904};
    const uint64_t values[] {channels.receiveDescriptorAddress,channels.receiveAddress,kCtReceiveBytes,
        channels.sendDescriptorAddress,channels.sendAddress,kCtSendBytes};
    for (uint32_t i = 0; i < 6; ++i) {
        uint32_t request[4];
        if (!buildCtSelfConfig(keys[i],values[i],request) || !mmioRequest(request,4,1)) return false;
    }
    uint32_t control[2];
    buildCtControl(true,control);
    return mmioRequest(control,2,0);
}
bool WhateverXeMediaRuntime::authenticateHuC() {
    uint32_t response[256];
    const uint32_t action[] {0x4000,buffers[HuCRsa].gpu};
    for (uint32_t fence = 1; fence <= 3; ++fence) {
        if (writeCtRequest(channels.send,action,2,fence) != CtResult::Ok) return false;
        flushMapped(buffers[Ct].cpu,kCtBlobBytes);
        write(0x1901f0,1);
        bool retry = false;
        for (uint32_t elapsed = 0; elapsed <= 1000; ++elapsed) {
            CtMessage message;
            const auto result = readCtMessage(channels.receive,response,256,message);
            if (result == CtResult::Empty) {if (elapsed != 1000) IOSleep(1); continue;}
            if (result != CtResult::Ok) return false;
            uint32_t data = 0;
            const auto reply = classifyHxgReply(message,response,fence,data);
            if (reply == HxgReply::Other || reply == HxgReply::Busy) continue;
            if (reply == HxgReply::Retry) {retry = true; break;}
            return reply == HxgReply::Success && data == 0;
        }
        if (!retry) return false;
    }
    return false;
}
RuntimeOps WhateverXeMediaRuntime::operations() {
    RuntimeOps ops;
    ops.context = this;
    ops.ownershipValid = [](void *p) {return static_cast<WhateverXeMediaRuntime *>(p)->ownerActive();};
    ops.mappingsValid = [](void *p) {
        auto self = static_cast<WhateverXeMediaRuntime *>(p);
        for (uint32_t i = 0; i < BufferCount; ++i) if (!self->verifyBuffer(BufferIndex(i))) return false;
        return true;
    };
    ops.flushMappings = [](void *p) {
        auto self = static_cast<WhateverXeMediaRuntime *>(p);
        for (const auto &b : self->buffers) flushMapped(b.cpu,b.bytes);
    };
    ops.retainUntilReboot = [](void *p) {
        if (!__sync_bool_compare_and_swap(&gFirmwareOwner,nullptr,p)) return false;
        static_cast<WhateverXeMediaRuntime *>(p)->retain();
        return true;
    };
    ops.read = [](void *p,uint32_t reg) {return static_cast<WhateverXeMediaRuntime *>(p)->read(reg);};
    ops.write = [](void *p,uint32_t reg,uint32_t value) {static_cast<WhateverXeMediaRuntime *>(p)->write(reg,value);};
    ops.sleep = [](void *,uint32_t milliseconds) {IOSleep(milliseconds);};
    ops.registerCt = [](void *p) {return static_cast<WhateverXeMediaRuntime *>(p)->registerCt();};
    ops.authenticateHuC = [](void *p) {return static_cast<WhateverXeMediaRuntime *>(p)->authenticateHuC();};
    return ops;
}
void WhateverXeMediaRuntime::publish() {
    setProperty("FirmwareState",uint32_t(state.phase),32);
    setProperty("FirmwareFailure",uint32_t(state.failure),32);
    setProperty("FirmwareAuthenticated",state.phase == RuntimePhase::Authenticated);
    IOLog("WhateverXe media firmware state=%u failure=%u\n",uint32_t(state.phase),uint32_t(state.failure));
}
IOReturn WhateverXeMediaRuntime::initializeAction(OSObject *target,void *,void *,void *,void *) {
    auto self = static_cast<WhateverXeMediaRuntime *>(target);
    self->wake(true);
    const bool ready = self->preflight() && initializeGen12Runtime(self->config,self->operations(),self->state);
    self->wake(false);
    return ready ? kIOReturnSuccess : kIOReturnNotReady;
}
IOReturn WhateverXeMediaRuntime::refreshAction(OSObject *target,void *result,void *,void *,void *) {
    auto self = static_cast<WhateverXeMediaRuntime *>(target);
    bool ready = false;
    if (self->ownerActive()) {
        self->wake(true);
        ready = refreshGen12Runtime(self->operations(),self->state);
        self->wake(false);
    } else if (self->state.retained) {
        self->state.phase = RuntimePhase::Unavailable;
        self->state.failure = RuntimeFailure::Ownership;
    }
    *static_cast<bool *>(result) = ready;
    return ready ? kIOReturnSuccess : kIOReturnNotReady;
}
bool WhateverXeMediaRuntime::serializeProof(void *target,void *,OSSerialize *serializer) {
    auto self = static_cast<WhateverXeMediaRuntime *>(target);
    bool ready = false;
    if (self->workloop) self->workloop->runAction(refreshAction,self,&ready);
    return (ready ? kOSBooleanTrue : kOSBooleanFalse)->serialize(serializer);
}
bool WhateverXeMediaRuntime::start(IOService *provider) {
    if (!IOService::start(provider) || !argument("-wex-hevc-brc")) return false;
    if (bindOwner(provider) && workloop->runAction(initializeAction,this) == kIOReturnSuccess) {
        proof = OSSerializer::forTarget(this,serializeProof);
        if (proof) IORegistryEntry::getRegistryRoot()->setProperty("WEXHuCFirmwareAuthenticated",proof);
        else {state.phase = RuntimePhase::Unavailable; state.failure = RuntimeFailure::Configuration;}
    }
    publish();
    registerService();
    return true;
}
void WhateverXeMediaRuntime::stop(IOService *provider) {
    stopped = true;
    auto root = IORegistryEntry::getRegistryRoot();
    if (proof && root->getProperty("WEXHuCFirmwareAuthenticated") == proof)
        root->removeProperty("WEXHuCFirmwareAuthenticated");
    if (state.retained) {state.phase = RuntimePhase::Unavailable; state.failure = RuntimeFailure::Ownership;}
    publish();
    IOService::stop(provider);
}
void WhateverXeMediaRuntime::free() {
    if (state.retained) return; // the explicit lifetime retain prevents this path
    if (proof) proof->release();
    for (auto &b : buffers) if (b.object) b.object->release();
    if (workloop) workloop->release();
    if (task) task->release();
    if (owner) owner->release();
    IOService::free();
}
extern "C" kern_return_t runtimeStart(kmod_info_t *,void *) {return KERN_SUCCESS;}
extern "C" kern_return_t runtimeStop(kmod_info_t *,void *) {return gFirmwareOwner ? KERN_FAILURE : KERN_SUCCESS;}
KMOD_EXPLICIT_DECL(org.b00t0x.WhateverXeMediaRuntime,"0.1.0",runtimeStart,runtimeStop)
extern "C" {
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = runtimeStart;
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain = runtimeStop;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
