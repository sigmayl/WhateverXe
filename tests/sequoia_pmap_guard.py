"""Exercise the runtime call-target guard, including servicing-kernel relocation."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'WhateverXe/kern_xe.cpp').read_text()
helper = re.search(r'static bool tglSequoiaProtectCallMatches\(.*?\n\}', source, re.S).group()
fixture = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <initializer_list>
HELPER
int main() {
 uint8_t code[0xD0] {};
 const auto protect=reinterpret_cast<uintptr_t>(code);
 assert(!tglSequoiaProtectCallMatches(0,protect));
 assert(!tglSequoiaProtectCallMatches(protect,0));
 // Original/servicing distances, signed boundaries and backward calls.
 for(int32_t displacement : {0x2CB71,0x2CBA1,0,-1,INT32_MIN,INT32_MAX}) {
  code[0xCA]=0xE8;memcpy(code+0xCB,&displacement,4);
  const auto target=protect+0xCF+static_cast<intptr_t>(displacement);
  assert(tglSequoiaProtectCallMatches(protect,target));
  assert(!tglSequoiaProtectCallMatches(protect,target+1));
  assert(!tglSequoiaProtectCallMatches(protect,target-1));
  for(unsigned opcode=0;opcode<256;++opcode) {
   code[0xCA]=opcode;
   assert(tglSequoiaProtectCallMatches(protect,target)==(opcode==0xE8));
  }
  code[0xCA]=0xE8;
  for(unsigned bit=0;bit<32;++bit) {
   code[0xCB+bit/8]^=1U<<(bit%8);
   assert(!tglSequoiaProtectCallMatches(protect,target));
   code[0xCB+bit/8]^=1U<<(bit%8);
  }
 }
}
'''.replace('HELPER', helper)
with tempfile.TemporaryDirectory(prefix='sequoia-call-guard-') as temp:
    p = Path(temp)
    (p / 'test.cpp').write_text(fixture)
    subprocess.run(['clang++', '-std=c++17', '-O2', '-fsanitize=address,undefined',
                    str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('PASS: relocated pmap calls, wrong opcodes/targets, null symbols and signed displacements')
