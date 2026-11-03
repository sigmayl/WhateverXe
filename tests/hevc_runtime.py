#!/usr/bin/env python3
"""Check paired runtime image identity and optional exact native ABI fixtures."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hevc_runtime_builder',ROOT/'tools/build_hevc_runtime.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)

def rejects(data):
    try:
        builder.macho_uuid(data)
    except ValueError:
        return
    raise AssertionError('unsupported or malformed image accepted')

def fixture(uuid=bytes(range(16))):
    return struct.pack('<8I',0xfeedfacf,0x01000007,3,0xb,1,24,0,0)+struct.pack('<II',0x1b,24)+uuid

image = fixture()
assert builder.macho_uuid(image) == bytes(range(16))
rejects(image[:31])
rejects(image[:-1])
for index,value in [(0,0xfeedface),(1,0x0100000c),(3,6),(4,129),(5,0x10001)]:
    wrong = bytearray(image)
    struct.pack_into('<I',wrong,index*4,value)
    rejects(wrong)
wrong = bytearray(image)
struct.pack_into('<I',wrong,36,4)
rejects(wrong)
duplicate = bytearray(image[:32]+image[32:]+image[32:])
struct.pack_into('<II',duplicate,16,2,48)
rejects(duplicate)
assert [item[1] for item in builder.FIRMWARE.values()] == [335360,589888]

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--accelerator',type=Path)
a = p.parse_args()
if a.accelerator:
    data = a.accelerator.read_bytes()
    assert hashlib.sha256(data).hexdigest() == '4cd1298968756763bdfe7f8bda5950590caa91556ded3c837c92c71b82356c34'
    header = (ROOT/'media/firmware_runtime/native_abi.hpp').read_text()
    def array(name):
        body = re.search(r'constexpr uint8_t '+name+r'\[\]\s*\{([^}]+)\}',header)[1]
        return bytes(int(part.strip(),0) for part in body.split(',') if part.strip())
    assert builder.macho_uuid(data) == array('kDriverUuid')
    for name,offset in [('kWakeFixture',0x28c2a),('kFactoryFixture',0x10924),
            ('kCpuFixture',0x10b60),('kGpuFixture',0x13e7c),
            ('kKernelTaskFixture',0x8080),('kWorkloopFixture',0x2e646)]:
        expected = array(name)
        assert data[offset:offset+len(expected)] == expected, name
print('PASS HEVC runtime paired UUID parsing and native ABI fixtures' if a.accelerator else
      'PASS HEVC runtime paired UUID parsing and firmware pins')
