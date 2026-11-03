// SPDX-License-Identifier: BSD-3-Clause
// Read-only readiness query. Resolves public macOS frameworks at runtime.
typedef unsigned int u32;
extern void *dlopen(const char *,int);
extern void *dlsym(void *,const char *);
extern int printf(const char *,...);
int main(void) {
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",9);
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",9);
    if (!cf || !io) return 3;
    void *(*matching)(const char *) = dlsym(io,"IOServiceMatching");
    u32 (*service)(u32,void *) = dlsym(io,"IOServiceGetMatchingService");
    u32 (*root_entry)(u32) = dlsym(io,"IORegistryGetRootEntry");
    void *(*property)(u32,void *,void *,u32) = dlsym(io,"IORegistryEntryCreateCFProperty");
    int (*release_entry)(u32) = dlsym(io,"IOObjectRelease");
    void *(*string)(void *,const char *,u32) = dlsym(cf,"CFStringCreateWithCString");
    unsigned long (*type)(void *) = dlsym(cf,"CFGetTypeID");
    unsigned long (*boolean_type)(void) = dlsym(cf,"CFBooleanGetTypeID");
    unsigned char (*boolean_value)(void *) = dlsym(cf,"CFBooleanGetValue");
    void (*release)(void *) = dlsym(cf,"CFRelease");
    if (!matching || !service || !root_entry || !property || !release_entry ||
        !string || !type || !boolean_type || !boolean_value || !release) return 3;
    u32 instance = service(0,matching("WhateverXeMediaRuntime"));
    u32 root = root_entry(0);
    void *key = string(0,"WEXHuCFirmwareAuthenticated",0x08000100);
    void *ready = root && key ? property(root,key,0,0) : 0;
    int authenticated = ready && type(ready) == boolean_type() && boolean_value(ready);
    if (ready) release(ready);
    if (key) release(key);
    if (root) release_entry(root);
    key = string(0,"FirmwareAuthenticated",0x08000100);
    ready = instance && key ? property(instance,key,0,0) : 0;
    authenticated &= ready && type(ready) == boolean_type() && boolean_value(ready);
    if (ready) release(ready);
    if (key) release(key);
    printf("{\"runtimePresent\":%s,\"authenticated\":%s}\n",
        instance ? "true" : "false",authenticated ? "true" : "false");
    if (instance) release_entry(instance);
    return authenticated ? 0 : 1;
}
