#pragma once
#include <Headers/kern_patcher.hpp>
#include "kern_xelp_platform.hpp"
class IORegistryEntry;
namespace XeHDMIAudio {
void init();
void processKernel(const XELP::Profile &,IORegistryEntry *,IORegistryEntry *);
bool processKext(KernelPatcher &,size_t,mach_vm_address_t,size_t);
}
