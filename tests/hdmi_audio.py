"""Replay the production audio guard and optionally verify the exact AppleHDA KC image."""
from pathlib import Path
import argparse
import mmap
import re
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--collection', type=Path)
args = parser.parse_args()
source = (root / 'WhateverXe/kern_hdmi_audio.cpp').read_text()
source = re.sub(r'^#include .*\n', '', source, flags=re.M)
fixture = r'''
#include <cassert>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include <sys/mman.h>
#include "kern_hdmi_audio_guard.hpp"
using mach_vm_address_t=uintptr_t;
constexpr uint32_t kIOReturnBadArgument=0xe00002c2,kIOReturnNotReady=0xe00002d8;
namespace KernelVersion {constexpr unsigned Sequoia=24;}
unsigned kernel=24;unsigned getKernelVersion(){return kernel;}
#define arrsize(a) (sizeof(a)/sizeof(a[0]))
#define SYSLOG(...) do {} while(0)
void *gIOServicePlane=nullptr;
struct IORegistryEntry {
 alignas(8) uint8_t bytes[0x300]{};IORegistryEntry *parent=nullptr;bool property=false;
 IORegistryEntry *getParentEntry(void *){return parent;}
 void setProperty(const char *,bool value){property=value;}
 static IORegistryEntry *getRegistryRoot(){static IORegistryEntry root;return &root;}
};
struct IOService:IORegistryEntry {
 struct Meta {unsigned size=0x300;unsigned getClassSize(){return size;}}meta;
 bool hdmi=true;Meta *getMetaClass(){return &meta;}
 void *metaCast(const char *name){assert(!strcmp(name,"AppleHDAHDMI_DPDriver"));return hdmi?this:nullptr;}
};
unsigned calls=0;IOService *lastDriver;void *lastAssociation;uint32_t nativeResult=0xE00002BC;
uint32_t native(IOService *driver,void *association){++calls;lastDriver=driver;lastAssociation=association;return nativeResult;}
struct KernelPatcher {
 struct KextInfo {const char *id;const char **paths;size_t count;bool flags[1];unsigned reserved;size_t loadIndex;static constexpr size_t Unloaded=~size_t(0);};
 struct RouteRequest {
  const char *symbol;uint32_t (*hook)(IOService*,void*);uint32_t (**original)(IOService*,void*);
  RouteRequest(const char *s,decltype(hook) h,decltype(*original)&o):symbol(s),hook(h),original(&o){}
 };
 size_t routes=0;bool fail=false;uintptr_t delta=0;
 template<class T>T solveSymbol(size_t,const char *symbol,uintptr_t base,size_t){
  assert(strstr(symbol,"handleValidEDIDAction"));return base+XeHDMIAudioGuard::ActionOffset+delta;
 }
 void clearError(){}
 bool routeMultipleShort(size_t,RouteRequest *r,unsigned n,uintptr_t,size_t){
  assert(n==1);++routes;if(fail)return false;*r->original=native;return true;
 }
};
struct {unsigned registrations=0;void onKextLoadForce(KernelPatcher::KextInfo *k){++registrations;k->loadIndex=7;}}lilu;
namespace XeHDMIAudio {
 void init();void processKernel(const XELP::Profile&,IORegistryEntry*,IORegistryEntry*);
 bool processKext(KernelPatcher&,size_t,uintptr_t,size_t);
}
PRODUCTION
void put32(std::vector<uint8_t>&v,size_t at,uint32_t value){memcpy(v.data()+at,&value,4);}
void codec(IOService &s,void *value){memcpy(reinterpret_cast<uint8_t*>(&s)+XeHDMIAudioGuard::CodecOffset,&value,sizeof(value));}
std::vector<uint8_t> image(){
 using namespace XeHDMIAudioGuard;
 std::vector<uint8_t> v(ActionOffset+0x49);
 put32(v,0,0xfeedfacf);put32(v,4,0x01000007);put32(v,12,11);put32(v,16,1);put32(v,20,24);
 put32(v,32,0x1b);put32(v,36,24);memcpy(v.data()+40,UUID,16);
 memcpy(v.data()+ActionOffset,Entry,sizeof(Entry));memcpy(v.data()+ActionOffset+0x39,CodecCall,sizeof(CodecCall));return v;
}
unsigned codecCalls=0;
uint32_t codecAddress(void *){++codecCalls;return 0x12345678;}
int main(int argc,char **argv){
 using namespace XeHDMIAudioGuard;
 IORegistryEntry audio,other,digital;const auto profile=XELP::identify(0xA7808086,4);
 for(kernel=20;kernel<=26;++kernel){
  const auto before=lilu.registrations;XeHDMIAudio::init();assert(lilu.registrations==before+(kernel==24));
  XeHDMIAudio::processKernel(profile,&audio,&digital);assert(enabled==(kernel==24));
 }
 kernel=24;
 for(auto p:{XELP::identify(0xA7801002,4),XELP::identify(0xFFFF8086,4),
             XELP::identify(0xA7808086,4,0x9A498086),XELP::identify(0xA7808086,256)}) {
  XeHDMIAudio::processKernel(p,&audio,&digital);assert(!enabled);
 }
 XeHDMIAudio::processKernel(profile,nullptr,nullptr);assert(!enabled);
 XeHDMIAudio::processKernel(profile,&audio,&digital);assert(enabled);
 auto bytes=image();auto known=[&](const std::vector<uint8_t>&v,size_t off=ActionOffset){return imageMatches(v.data(),v.size(),off);};
 assert(known(bytes));assert(!imageMatches(nullptr,bytes.size(),ActionOffset));
 assert(!known(bytes,ActionOffset+1));
 for(size_t n:{size_t(0),size_t(31),size_t(55),bytes.size()-1})assert(!imageMatches(bytes.data(),n,ActionOffset));
 for(size_t at:{size_t(0),size_t(4),size_t(12),size_t(16),size_t(20),size_t(32),size_t(36)}){
  auto bad=bytes;put32(bad,at,~0U);assert(!known(bad));
 }
 for(size_t i=0;i<16;++i){auto bad=bytes;bad[40+i]^=1;assert(!known(bad));}
 for(size_t i=0;i<sizeof(Entry);++i){auto bad=bytes;bad[ActionOffset+i]^=1;assert(!known(bad));}
 for(size_t i=0;i<sizeof(CodecCall);++i){auto bad=bytes;bad[ActionOffset+0x39+i]^=1;assert(!known(bad));}
 auto duplicate=bytes;put32(duplicate,16,2);put32(duplicate,20,48);memcpy(duplicate.data()+56,duplicate.data()+32,24);assert(!known(duplicate));
 KernelPatcher patcher;
 assert(!XeHDMIAudio::processKext(patcher,8,(uintptr_t)bytes.data(),bytes.size())&&!patcher.routes);
 patcher.delta=1;assert(XeHDMIAudio::processKext(patcher,7,(uintptr_t)bytes.data(),bytes.size())&&!patcher.routes);
 patcher.delta=0;assert(XeHDMIAudio::processKext(patcher,7,(uintptr_t)bytes.data(),bytes.size())&&patcher.routes==1);
 assert(IORegistryEntry::getRegistryRoot()->property);
 patcher.fail=true;XeHDMIAudio::processKext(patcher,7,(uintptr_t)bytes.data(),bytes.size());assert(!IORegistryEntry::getRegistryRoot()->property);
 originalValidEDID=native;
 for(unsigned owner=0;owner<3;++owner)for(bool ready:{false,true})for(bool type:{false,true})for(bool size:{false,true}){
  IORegistryEntry middle;middle.parent=owner==0?&audio:owner==1?&digital:&other;
  IOService driver;driver.parent=&middle;driver.hdmi=type;driver.meta.size=size?0x300:0x150;
  codec(driver,ready?&other:nullptr);calls=0;void *association=(void*)0x1234;
  const bool defer=owner<2&&!ready&&type&&size;
  const auto result=handleValidEDID(&driver,association);
  assert(result==(defer?kIOReturnNotReady:nativeResult));assert(calls==!defer);
  if(!defer)assert(lastDriver==&driver&&lastAssociation==association);
  assert(driver.property==defer);
 }
 assert(handleValidEDID(nullptr,nullptr)==kIOReturnBadArgument);
 // Execute the exact formerly unsafe load/vcall with an initialized codec;
 // a null codec must return before reaching those instructions.
 uint8_t *code=(uint8_t*)mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(code!=MAP_FAILED);
 const uint8_t prologue[]={0x53,0x48,0x89,0xfb};
 memcpy(code+16,prologue,4);memcpy(code+20,CodecCall,sizeof(CodecCall));code[36]=0x5b;code[37]=0xc3;
 assert(!mprotect(code,4096,PROT_READ|PROT_EXEC));originalValidEDID=reinterpret_cast<decltype(originalValidEDID)>(code+16);
 IOService driver;driver.parent=&audio;
 assert(handleValidEDID(&driver,nullptr)==kIOReturnNotReady&&!codecCalls);
 uintptr_t vtable[0x878/8]{};vtable[0x870/8]=(uintptr_t)codecAddress;auto object=(uintptr_t)vtable;codec(driver,&object);
 assert(handleValidEDID(&driver,nullptr)==0x12345678&&codecCalls==1);
 codec(driver,nullptr);assert(handleValidEDID(&driver,nullptr)==kIOReturnNotReady&&codecCalls==1);munmap(code,4096);
 if(argc==2){std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t> actual((std::istreambuf_iterator<char>(f)),{});assert(known(actual));puts("PASS exact AppleHDA UUID, symbol offset and runtime instruction guards");}
 puts("PASS audio guard: OS/physical ownership, malformed images, null codec, valid native vcall, retry and route failure");
}
'''.replace('PRODUCTION', source)
with tempfile.TemporaryDirectory(prefix='wex-hdmi-audio-') as temp:
    p = Path(temp)
    native = []
    if args.collection:
        with args.collection.open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
            at = 32
            base = None
            for _ in range(struct.unpack_from('<I', data, 16)[0]):
                command, size = struct.unpack_from('<II', data, at)
                if command == 0x80000035:
                    va, offset, name, reserved = struct.unpack_from('<QQII', data, at + 8)
                    if data[at + name:at + size].split(b'\0')[0] == b'com.apple.driver.AppleHDA':
                        base = offset
                at += size
            assert base is not None
            at = base + 32
            for _ in range(struct.unpack_from('<I', data, base + 16)[0]):
                command, size = struct.unpack_from('<II', data, at)
                if command == 0x19 and data[at + 8:at + 24].rstrip(b'\0') == b'__TEXT':
                    vm, length, offset, file_size = struct.unpack_from('<4Q', data, at + 24)
                    text = bytes(data[offset:offset + file_size])
                if command == 2:
                    symbols, count, strings, string_size = struct.unpack_from('<4I', data, at + 8)
                at += size
            found = False
            for i in range(count):
                name, typ, section, description, value = struct.unpack_from('<IBBHQ', data, symbols + i * 16)
                if name and data[strings + name:data.find(b'\0', strings + name)].endswith(b'21handleValidEDIDActionEP27HDAGFXHeadAssociationStruct'):
                    assert value - vm == 0x4766E
                    found = True
            assert found
            (p / 'AppleHDA.text').write_bytes(text)
            native = [str(p / 'AppleHDA.text')]
    (p / 'test.cpp').write_text(fixture)
    subprocess.run(['clang++', '-std=c++17', '-O1', '-fsanitize=address,undefined',
                    '-I' + str(root / 'WhateverXe'), str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test'), *native], check=True)
