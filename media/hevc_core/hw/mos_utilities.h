// SPDX-License-Identifier: MIT
// Minimal stand-in for media-driver's mos_utilities.h. The generated MHW
// command constructors only need MOS_ZeroMemory; the rest of the media-driver
// OS abstraction is not part of this component.
#ifndef WEX_MEDIA_HEVC_CORE_MOS_UTILITIES_H
#define WEX_MEDIA_HEVC_CORE_MOS_UTILITIES_H

#include <cstring>

#ifndef MOS_ZeroMemory
#define MOS_ZeroMemory(pointer, size) std::memset((pointer), 0, (size))
#endif

#endif
