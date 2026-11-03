# Experimental HEVC VA shim

This user-space wrapper loads the shipped `AppleIntelTGLGraphicsVADriver`,
forwards its AVD interface, and supplies an experimental HEVC encode context.
It is separate from the kexts and provides 8-bit or Main10 hardware encoding,
with software-selected CBR, VBR and adaptive quality control. Main10 requires
a client requesting that profile with a 10-bit biplanar input and the supported
macOS 15.8.2 encoder plug-in layout. Single-reference inter prediction follows
the native picture requests and preserves intra pictures and keyframe boundaries.

The submission adapter accepts the supported 16.0.32 VA driver layout. The
kernel's existing driver and physical-device admission guards still apply.
Decode requests are forwarded to the shipped driver; unsupported video
processing contexts are refused so the caller can use its conversion fallback.

## Current limitations

- The experimental NV12 and P010 picture paths support 4K HEVC submission and
  VideoToolbox file export. Coded padding and conformance-window signaling
  preserve the selected visible dimensions. The constant-QP path uses a fixed
  slice QP; rate control enables adaptive QP groups. Each picture is
  coded as an I picture or, when the plug-in marks it as inter, as a
  low-delay B picture whose two reference lists name the same earlier picture -
  the only inter slice form the encoder accepts. Both carry the plug-in's
  picture order count so display order survives reordering.
- True B pictures (two different references), multiple slices, SAO and
  deblocking are not implemented.
- The software CBR property bridge has CPU and native ABI coverage; end-to-end
  VideoToolbox CBR on hardware still needs validation.
- Each picture has a hardware completion tag. A fence released after a GPU
  reset is treated as failure, and that context stops submitting pictures.

## Configuration

Hardware submission, GPU address resolution, inter prediction and Main10
negotiation are available by default after the experimental HEVC path is
selected. Software requests choose the profile, input format, picture types and
rate-control mode. These features do not require marker files.

HuC rate/quality control requires the
[firmware runtime](../firmware_runtime/README.md) with `-wex-hevc-brc` at startup.
Its firmware, driver and physical-device guards apply independently of the
shim's profile guards.

| Software property | Control mode |
| --- | --- |
| `AverageBitRate` with a positive bits/second value | VBR |
| `ConstantBitRate` with a positive integer bits/second value | CBR |
| Quality only, without a bitrate request | Adaptive QP when HuC is ready; fixed QP otherwise |

The admitted encoder's supported-property query advertises `ConstantBitRate`
when HuC is authenticated. Its value and mode are stored separately for each
encoder session. Setting `AverageBitRate` selects VBR; setting `ConstantBitRate`
selects CBR. Clearing `ConstantBitRate` clears the bitrate request. Quality is
normalized from 0 to 1; higher values select lower QP within the firmware's
supported 10–51 range. A quality-only request without HuC uses fixed QP; without
a quality value, the fixed-QP default is 26.
Rate or quality changes must be made after draining queued pictures. VBR keeps
the output already inside a bitrate window when rates or limits change.

`DataRateLimits` accepts up to two byte/second window pairs, each spanning at
most 256 frames. Limits can reduce the effective average bitrate passed by
the native plug-in. Without explicit limits, VBR uses a maximum rate twice
the target and a two-second VBV budget; CBR uses the target as its maximum.
VBR follows the requested average bitrate for intra-only and single-reference
encoding and checks each completed picture against the VBV and explicit byte
windows. Fractional VBR window durations are
rounded up to whole pictures while retaining their exact byte budget. Startup
and low-detail content can differ from the target average. A window too small
for a picture within the supported QP range stops that session's submissions.

Once BRC work has started, a failed submission stops that encode context.
The constant-QP path operates independently of HuC. Missing authentication or
unsupported parameters refuse a software bitrate request rather than silently
ignoring its budget. Diagnostic marker files in the installed shim's `MacOS`
directory are optional and are not part of normal capability selection.

The optional `-wex-hevc-encode` boot argument selects this experimental path
and requires a separately built shim. Keep it disabled for normal use. See the
[project requirements and configuration](../../README.md#configuration).

## Build and test

```sh
python3 tools/build_va_shim.py
python3 tools/test.py
```

The shim builder needs `clang++`, `ld64.lld` and `llvm-nm`; it cross-builds an
x86_64 macOS dylib without SDK headers or a C++ runtime dependency. Output:
`build/va_shim/AppleIntelTGLGraphicsVADriver`.

CPU tests cover packet layouts and order, resolution-dependent PAK/CU buffer
capacity, coded and visible picture dimensions, GPU surface pitches, isolated HCP scratch regions, surface
rebinding, resource tokens, first-frame preparation, matching SPS/HCP transform
limits, 8-bit/Main10 surface states and parameter-set cache invalidation,
guarded native rate/quality inputs, software CBR properties and session isolation,
BRC DMEM/SLB layouts, reference feedback,
adaptive-QP controls, the guarded Main10 profile adapter and the output byte-range/fence
contract. They do not establish hardware
encoding support or image quality.

The shim also preserves native H.264 hardware encoding when an unavailable
optional image converter leaves empty entries in AppleGVA's context request.
It forwards the remaining contexts to the admitted TGL driver and preserves
the returned capabilities and handle positions. This does not add a converter
or change codec negotiation in applications.

## Source attribution

The hardware commands and HEVC state builders follow
[Intel media-driver](https://github.com/intel/media-driver). Vendored files,
revision information and license attribution are described in the
[command definitions](../hevc_core/hw/README.md). Apple driver binaries are
local dependencies and are not distributed in this source tree.
