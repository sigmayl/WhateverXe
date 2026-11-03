#!/usr/bin/env python3
"""Cross-build the macOS VA driver shim. The target has no Command Line Tools,
so this uses clang with the lld Mach-O linker and no SDK libraries; libSystem
symbols are resolved at load time."""
import argparse
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'va_shim'
CORE = ROOT / 'media' / 'hevc_core'
SOURCES = [ROOT / 'media' / 'va_shim' / 'va_shim_core.cpp',
           ROOT / 'media' / 'va_shim' / 'hevc_context.cpp',
           ROOT / 'media' / 'va_shim' / 'hevc_commands.cpp',
           ROOT / 'media' / 'va_shim' / 'hevc_token.cpp',
           ROOT / 'media' / 'va_shim' / 'va_shim_darwin.cpp',
           CORE / 'hw_command.cpp',
           CORE / 'command_buffer.cpp',
           CORE / 'hw' / 'mhw_vdbox_hcp_hwcmd_g12_X.cpp',
           CORE / 'hw' / 'mhw_vdbox_vdenc_hwcmd_g12_X.cpp',
           CORE / 'hcp_state.cpp',
           CORE / 'hcp_picture.cpp',
           CORE / 'hcp_qm.cpp',
           CORE / 'hcp_slice.cpp',
           CORE / 'hcp_pak_insert.cpp',
           CORE / 'vdenc_state.cpp',
           CORE / 'vdenc_picture.cpp',
           CORE / 'vdenc_walker.cpp',
           CORE / 'vdenc_weights.cpp',
           CORE / 'picture_commands.cpp',
           CORE / 'huc_commands.cpp',
           CORE / 'huc_dmem.cpp',
           CORE / 'huc_brc_dmem.cpp',
           CORE / 'huc_brc_const.cpp',
           CORE / 'hevc_headers.cpp',
           CORE / 'vdenc_streamin.cpp']

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--clang', default='clang++')
p.add_argument('--output', type=Path, default=OUT / 'AppleIntelTGLGraphicsVADriver')
a = p.parse_args()

for tool in (a.clang, 'ld64.lld', 'llvm-nm'):
    if not shutil.which(tool):
        raise SystemExit(f'required tool not found: {tool}')

a.output.parent.mkdir(parents=True, exist_ok=True)
cmd = [
    a.clang,
    '-target', 'x86_64-apple-macos11',
    '-std=c++17',
    '-O2',
    '-fno-exceptions',
    '-fno-rtti',
    '-fuse-ld=lld',
    '-dynamiclib',
    '-nostdlib',
    '-nostdinc++',
    '-Wl,-undefined,dynamic_lookup',
    '-Wl,-install_name,@loader_path/AppleIntelTGLGraphicsVADriver',
    '-I', str(ROOT),
    '-I', str(ROOT / 'media' / 'va_shim' / 'compat'),
    '-o', str(a.output),
    *map(str, SOURCES),
]
subprocess.run(cmd, check=True)

# References to symbols this shim defines itself have to resolve at link time.
# The dylib is linked with -undefined dynamic_lookup, so an unresolved one
# becomes a lazy stub instead of a link error, and the first call jumps to
# address zero inside the decoder or encoder XPC service.
undefined = subprocess.run(['llvm-nm', '-u', str(a.output)],
    capture_output=True, text=True, check=True).stdout
leftover = sorted({line.split()[-1] for line in undefined.splitlines()
                   if '__ZN6VaShim' in line or '__ZN8HevcCore' in line})
if leftover:
    raise SystemExit('unresolved shim symbols: ' + ', '.join(leftover))

print(a.output)
