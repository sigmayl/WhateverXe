#include "kern_hdmi_audio.hpp"
#include "kern_hdmi_audio_guard.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOService.h>

namespace {
const char *paths[] {"/System/Library/Extensions/AppleHDA.kext/Contents/MacOS/AppleHDA"};
KernelPatcher::KextInfo hda {"com.apple.driver.AppleHDA",paths,arrsize(paths),{true},{},KernelPatcher::KextInfo::Unloaded};
IORegistryEntry *analogDevice,*digitalDevice;
bool enabled;
uint32_t (*originalValidEDID)(IOService *,void *);

uint32_t handleValidEDID(IOService *driver,void *association) {
 if(!driver)return kIOReturnBadArgument;
 bool owned=false;
 for(auto entry=static_cast<IORegistryEntry *>(driver);entry;entry=entry->getParentEntry(gIOServicePlane)) {
  if(entry==analogDevice || entry==digitalDevice){owned=true;break;}
 }
 if(owned && driver->metaCast("AppleHDAHDMI_DPDriver") &&
    driver->getMetaClass()->getClassSize()>=XeHDMIAudioGuard::CodecOffset+sizeof(void *)) {
  void *codec=nullptr;
  memcpy(&codec,reinterpret_cast<const uint8_t *>(driver)+XeHDMIAudioGuard::CodecOffset,sizeof(codec));
  // This callback already runs under the native audio workloop. Failed or
  // unfinished initHardware leaves codec null. The downstream handler checks
  // it, but the action dereferences it first. Do not fabricate an EDID/codec
  // or report success; later notifications can retry after initialization.
  if(!codec) {
   driver->setProperty("WEXHDMIAudioEDIDDeferred",true);
   return kIOReturnNotReady;
  }
 }
 return originalValidEDID(driver,association);
}
}

void XeHDMIAudio::init() {
 if(getKernelVersion()==KernelVersion::Sequoia)lilu.onKextLoadForce(&hda);
}
void XeHDMIAudio::processKernel(const XELP::Profile &profile,IORegistryEntry *analog,IORegistryEntry *digital) {
 enabled=XeHDMIAudioGuard::eligible(profile,getKernelVersion()) && (analog || digital);
 analogDevice=enabled ? analog : nullptr;digitalDevice=enabled ? digital : nullptr;
}
bool XeHDMIAudio::processKext(KernelPatcher &patcher,size_t index,mach_vm_address_t address,size_t size) {
 if(index!=hda.loadIndex)return false;
 if(!enabled)return true;
 const char *symbol="__ZN21AppleHDAHDMI_DPDriver21handleValidEDIDActionEP27HDAGFXHeadAssociationStruct";
 const auto action=patcher.solveSymbol<mach_vm_address_t>(index,symbol,address,size);patcher.clearError();
 bool ready=action>=address && XeHDMIAudioGuard::imageMatches(reinterpret_cast<const uint8_t *>(address),size,action-address);
 if(ready) {
  // The short trampoline copies only position-independent entry instructions;
  // the RIP-relative debug-mask load at +0x11 stays at its original address.
  KernelPatcher::RouteRequest route(symbol,handleValidEDID,originalValidEDID);
  ready=patcher.routeMultipleShort(index,&route,1,address,size);patcher.clearError();
 }
 IORegistryEntry::getRegistryRoot()->setProperty("WEXHDMIAudioEDIDGuardReady",ready);
 SYSLOG("wex","HDMI audio codec guard ready=%u",ready);
 return true;
}
