#!/usr/bin/env python3
"""Build the optional, guarded HEVC firmware runtime; never install or load it."""
import argparse
import hashlib
import json
import plistlib
import struct
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = {
    'kGuCImage': ('guc_firmware',335360,'2f1f57a1b23d186f2592318d1e07a1365968932841ccb3e7177c516ba006e2f6'),
    'kHuCImage': ('huc_firmware',589888,'dbb1316bac13a76427ffa7827e52b5b9affe8a4cadd23dc22e2a54e35c53bdad'),
}

def macho_uuid(binary):
    if len(binary) < 32:
        raise ValueError('truncated WhateverXe image')
    magic,cpu,_,kind,count,span,_,_ = struct.unpack_from('<8I',binary)
    if (magic,cpu,kind) != (0xfeedfacf,0x01000007,0xb) or count > 128 or span > 0x10000 or 32+span > len(binary):
        raise ValueError('expected an x86_64 WhateverXe kext image')
    found = None
    offset = 32
    for _ in range(count):
        if offset + 8 > 32 + span:
            raise ValueError('truncated Mach-O load commands')
        command,length = struct.unpack_from('<II',binary,offset)
        if length < 8 or offset + length > 32 + span:
            raise ValueError('invalid Mach-O load command')
        if command == 0x1b:
            if length != 24 or found is not None:
                raise ValueError('invalid or duplicate Mach-O UUID')
            found = binary[offset+8:offset+24]
        offset += length
    if found is None or offset != 32 + span:
        raise ValueError('missing UUID or inconsistent Mach-O commands')
    return found

def array(name,data):
    lines = [f'alignas(64) static const uint8_t {name}[] = {{']
    lines += ['    '+','.join(f'0x{x:02x}' for x in data[i:i+24])+','
              for i in range(0,len(data),24)]
    return '\n'.join(lines+['};'])

def run(command):
    subprocess.run(list(map(str,command)),check=True,cwd=ROOT)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--guc-firmware',type=Path,required=True)
    p.add_argument('--huc-firmware',type=Path,required=True)
    p.add_argument('--whateverxe',type=Path,required=True,help='the WhateverXe.kext used on the same boot')
    p.add_argument('--sdk',type=Path,default=ROOT/'MacKernelSDK')
    p.add_argument('--output',type=Path,default=ROOT/'build/hevc_runtime')
    p.add_argument('--debug',action='store_true')
    a = p.parse_args()
    out = a.output.resolve()
    if out.is_relative_to(ROOT) and not out.is_relative_to(ROOT/'build'):
        p.error('generated firmware must be placed under build/ or outside the source tree')
    contents = a.whateverxe.resolve()/'Contents'
    info = plistlib.loads((contents/'Info.plist').read_bytes())
    if info.get('CFBundleIdentifier') != 'org.b00t0x.WhateverXe' or info.get('CFBundleExecutable') != 'WhateverXe':
        p.error('--whateverxe must name a WhateverXe.kext bundle')
    binary = (contents/'MacOS/WhateverXe').read_bytes()
    uuid = macho_uuid(binary)
    images = {}
    manifest = {'firmware':{},'whateverxe_uuid':uuid.hex(),
                'whateverxe_sha256':hashlib.sha256(binary).hexdigest(),
                'configuration':'DEBUG' if a.debug else 'RELEASE'}
    for symbol,(field,length,digest) in FIRMWARE.items():
        data = getattr(a,field).read_bytes()
        if len(data) != length or hashlib.sha256(data).hexdigest() != digest:
            p.error(f'{field}: unsupported firmware image or digest')
        images[symbol] = data
        manifest['firmware'][symbol] = {'bytes':length,'sha256':digest}
    out.mkdir(parents=True,exist_ok=True)
    (out/'runtime_payload.hpp').write_text('// Generated local firmware; do not commit.\n#include <stdint.h>\nnamespace HevcFirmware {\n'+
        array('kWhateverXeUuid',uuid)+'\n'+ '\n'.join(array(name,data) for name,data in images.items())+'\n}\n')
    backend = subprocess.run(['xcrun','clang++','-x','c++','-c','/dev/null','-o','/dev/null',
        '-mllvm','--help-hidden'],capture_output=True,text=True,check=True)
    option = 'disable-atexit-based-global-dtor-lowering'
    destructor = ['-mllvm','-'+option] if option in backend.stdout+backend.stderr else []
    sdk = a.sdk.resolve()
    common = ['xcrun','clang++','-arch','x86_64','-std=c++17','-O0' if a.debug else '-O2',
        '-mkernel','-fapple-kext','-fno-exceptions','-fno-rtti','-fno-stack-protector',
        '-fno-builtin','-fno-asynchronous-unwind-tables','-mno-red-zone','-fstack-usage',
        '-Wframe-larger-than=2048','-Werror=frame-larger-than','-nostdinc',
        '-DKERNEL','-DKERNEL_PRIVATE','-D__APPLE__','-D__MACH__','-DAPPLE_KEXT_ASSERTIONS=1',
        '-DDRIVER_PRIVATE','-DAPPLE','-DNeXT','-fno-common','-fasm-blocks',
        '-mmacosx-version-min=10.6','-I'+str(sdk/'Headers'),'-I'+str(out),
        '-fdebug-compilation-dir=.', '-ffile-prefix-map='+str(ROOT)+'=/WhateverXe',
        '-ffile-prefix-map='+str(sdk)+'=/MacKernelSDK',
        '-ffile-prefix-map='+str(out)+'=/Generated','-Wall','-Wextra',
        '-Wno-unused-parameter','-Wno-deprecated-declarations',*destructor]
    sources = [ROOT/'media/firmware_runtime/native_runtime.cpp'] + [ROOT/'media/hevc_core'/name
        for name in ['uc_firmware.cpp','guc_boot.cpp','guc_ct.cpp','uc_runtime.cpp']]
    objects = []
    for source in sources:
        obj = out/(source.stem+'.o')
        run(common+['-c',source,'-o',obj]); objects.append(obj)
    name = 'WhateverXeMediaRuntime'
    bundle = out/(name+'.kext')
    target = bundle/'Contents'
    (target/'MacOS').mkdir(parents=True,exist_ok=True)
    exe = target/'MacOS'/name
    run(['xcrun','ld','-arch','x86_64','-kext','-undefined','dynamic_lookup','-no_fixup_chains',
         '-macos_version_min','10.6','-o',exe,*objects,sdk/'Library/x86_64/libkmod.a'])
    run(['xcrun','strip','-S',exe])
    undefined = subprocess.check_output(['xcrun','nm','-u',str(exe)],text=True)
    if any(token in undefined for token in ['atexit','chkstk','DispatchE5IORPC','__ZN8HevcCore','WhateverXeMediaRuntime']):
        raise SystemExit('unexpected unresolved kernel/local ABI symbol')
    libs = {key:'10.0.0' for key in ['com.apple.kpi.bsd','com.apple.kpi.dsep','com.apple.kpi.iokit',
            'com.apple.kpi.libkern','com.apple.kpi.mach','com.apple.kpi.unsupported']}
    libs['com.apple.iokit.IOPCIFamily'] = '1.1'
    libs[info['CFBundleIdentifier']] = info['CFBundleVersion']
    identifier = 'org.b00t0x.'+name
    plist = dict(CFBundleIdentifier=identifier,CFBundleExecutable=name,CFBundleName=name,
        CFBundlePackageType='KEXT',CFBundleVersion='0.1.0',OSBundleCompatibleVersion='0.1.0',
        OSBundleRequired='Root',OSBundleLibraries=libs,IOKitPersonalities={identifier:dict(
            CFBundleIdentifier=identifier,IOClass=name,IOMatchCategory=name,IOProviderClass='IntelAccelerator')})
    (target/'Info.plist').write_bytes(plistlib.dumps(plist))
    resources = target/'Resources'
    resources.mkdir(exist_ok=True)
    (resources/'LICENSE.i915').write_bytes((ROOT/'media/firmware_runtime/LICENSE.i915').read_bytes())
    manifest['binary_sha256'] = hashlib.sha256(exe.read_bytes()).hexdigest()
    manifest['source_sha256'] = {str(s.relative_to(ROOT)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources}
    manifest['headers_sha256'] = {str(s.relative_to(ROOT)):hashlib.sha256(s.read_bytes()).hexdigest()
        for directory in ['media/firmware_runtime','media/hevc_core'] for s in sorted((ROOT/directory).glob('*.hpp'))}
    manifest['undefined'] = undefined.splitlines()
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (resources/'firmware-manifest.json').write_text(json.dumps(manifest['firmware'],indent=2)+'\n')
    (out/'libSystem.tbd').write_text("""--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libSystem.B.dylib'
current-version: 1
compatibility-version: 1
exports:
  - targets: [ x86_64-macos ]
    symbols: [ _dlopen, _dlsym, _printf ]
...
""")
    run(['clang','-target','x86_64-apple-macos11','-O2','-fuse-ld=lld','-nostdlib',
         '-Wl,-undefined,dynamic_lookup','-L'+str(out),'-lSystem',
         ROOT/'media/firmware_runtime/runtime_status.c','-o',out/'hevc_firmware_status'])
    with tarfile.open(out/(name+'.tgz'),'w:gz') as archive: archive.add(bundle,arcname=bundle.name)
    print(json.dumps({'bundle':str(bundle),'sha256':manifest['binary_sha256'],'installed':False}))

if __name__ == '__main__':
    main()
