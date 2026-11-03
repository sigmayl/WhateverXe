# Experimental HEVC firmware runtime

`WhateverXeMediaRuntime.kext` provides GuC/HuC authentication for the
[HEVC VA shim](../va_shim/README.md). It retains firmware, communication and
working memory until reboot and checks current authentication before the shim
uses HuC. The Apple driver's graphics-firmware loader must be disabled.

This component accepts Raptor Lake-S `8086:A780`, revision 4, on the supported
macOS 15.8.2 kernel with the admitted `AppleIntelTGLGraphics` driver. It also
checks the exact WhateverXe image supplied to its builder. Other device and
driver layouts are refused. Keep the experimental boot arguments disabled for
normal use.

## Build

Build WhateverXe with `python3 tools/build.py`. The runtime builder needs the
local ignored MacKernelSDK, `xcrun` with the kernel compiler/linker tools, and
`clang`/`ld64.lld` for its read-only status utility.

Obtain the unchanged Intel `i915/tgl_guc_70.49.4.bin` and
`i915/tgl_huc_7.9.3.bin` files from
[linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware).
The builder checks their sizes and SHA-256 digests. Firmware files and generated
payloads are local dependencies and are not included in this repository.

```sh
python3 tools/build_hevc_runtime.py \
  --whateverxe build/WhateverXe.kext \
  --guc-firmware /path/to/tgl_guc_70.49.4.bin \
  --huc-firmware /path/to/tgl_huc_7.9.3.bin
```

Supply the exact `WhateverXe.kext` that will be loaded on the same boot.
`--debug` selects a DEBUG runtime build. Outputs are
`build/hevc_runtime/WhateverXeMediaRuntime.kext` and
`build/hevc_runtime/hevc_firmware_status`. The generated kext contains the
unmodified firmware and its [Intel license](LICENSE.i915).

## Configuration

Install the runtime in the bootloader's kext list after WhateverXe and use:

```text
-allow3d -disablegfxfirmware -wex-hevc-encode -wex-hevc-brc
```

`-wex-hevc-brc` enables this optional firmware component. The VA shim selects
bitrate/quality control from software requests after HuC authentication; see its
[configuration](../va_shim/README.md#configuration).

After startup, run the read-only status utility:

```sh
build/hevc_runtime/hevc_firmware_status
```

Successful readiness is reported as
`{"runtimePresent":true,"authenticated":true}`. A failed upload or loss of
authentication disables HuC for that boot. Firmware-visible memory remains
retained, and the runtime cannot be unloaded after a transfer begins. Reboot
before replacing it. The fixed-QP shim path operates independently of HuC.

## Tests and attribution

```sh
python3 tools/test.py
python3 tests/hevc_runtime.py --accelerator /path/to/AppleIntelTGLGraphics
```

The optional native-fixture check requires the exact admitted driver and checks
its private ABI entries. CPU tests cover firmware bounds, WOPCM, GuC boot
parameters, CT communication, upload order, ownership retention and failure
recovery.

Firmware transport and layouts follow Linux
[i915](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/i915/gt/uc).
The core's Intel-derived code retains its MIT notices. Native ABI fixtures come
from IDA analysis of the admitted Apple driver; Apple binaries are not
distributed here.
