// Bounded guards for the Sequoia AppleHDA HDMI/DP event entry.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "kern_xelp_platform.hpp"

namespace XeHDMIAudioGuard {
constexpr size_t ActionOffset=0x4766E, CodecOffset=0x158;
constexpr uint8_t UUID[] {0x0C,0xAA,0xCB,0x05,0xB4,0x71,0x36,0x72,0xBB,0x54,0x03,0x41,0xA6,0xA6,0x3F,0x68};
constexpr uint8_t Entry[] {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x49,0x89,0xF6,0x48,0x89,0xFB};
constexpr uint8_t CodecCall[] {0x48,0x8B,0xBB,0x58,0x01,0,0,0x48,0x8B,0x07,0xFF,0x90,0x70,0x08,0,0};
inline bool eligible(const XELP::Profile &profile,unsigned kernel) {
 return kernel==24 && profile.displayAdmitted() && !profile.overridden() &&
  XELP::same(profile,XELP::identify(profile.identity,profile.revision));
}
inline uint32_t read32(const uint8_t *p) {
 return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
inline bool equal(const uint8_t *a,const uint8_t *b,size_t n) {
 for(size_t i=0;i<n;++i)if(a[i]!=b[i])return false;
 return true;
}
inline bool imageMatches(const uint8_t *image,size_t size,size_t actionOffset) {
 if(!image || size<32 || read32(image)!=0xFEEDFACF || read32(image+4)!=0x01000007 ||
    read32(image+12)!=11 || actionOffset!=ActionOffset || size<ActionOffset+0x49)return false;
 const auto count=read32(image+16),bytes=read32(image+20);
 if(bytes>size-32 || count>bytes/8)return false;
 const size_t end=32+bytes;size_t at=32;bool uuid=false;
 for(uint32_t i=0;i<count;++i) {
  if(end-at<8)return false;
  const auto command=read32(image+at),length=read32(image+at+4);
  if(length<8 || length>end-at)return false;
  if(command==0x1B) {
   if(uuid || length!=24 || !equal(image+at+8,UUID,sizeof(UUID)))return false;
   uuid=true;
  }
  at+=length;
 }
 return at==end && uuid && equal(image+ActionOffset,Entry,sizeof(Entry)) &&
  equal(image+ActionOffset+0x39,CodecCall,sizeof(CodecCall));
}
}
