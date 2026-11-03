# Gen12 MHW command definitions

These files are copied verbatim from Intel media-driver (iHD), which is
distributed under the MIT license. The copyright notice is part of each file.

- Source: https://github.com/intel/media-driver
- Revision: `f55f1c8ea764453553d1cc5add705a32cdca712d` (2026-10-29)
- Path: `media_driver/agnostic/gen12/hw/vdbox/`
- Files: `mhw_vdbox_vdenc_hwcmd_g12_X.{h,cpp}`,
  `mhw_vdbox_hcp_hwcmd_g12_X.{h,cpp}`

The headers describe the Gen12 VDENC and HCP command registers, including the
HEVC picture and slice state used by the encoder. Keeping the generated
definitions instead of retyping field positions means a packet built here has
the same layout as the reference driver by construction.

`mos_utilities.h` is a local three-line stand-in for the media-driver OS
abstraction header. It provides `MOS_ZeroMemory`, which the generated
constructors use. It is not copied from media-driver.
