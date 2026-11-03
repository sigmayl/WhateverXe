// CPU bounds check for Gen12 HEVC buffers across picture sizes. The reference
// footprints below count worst-case PAK/CU output rather than encoded bytes.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/va_shim/hevc_buffer_sizes.hpp"

int main() {
    const uint32_t widths[] {640, 1280, 1920, 2560, 3808, 3840, 4096, 7680};
    const uint32_t heights[] {360, 720, 1080, 1088, 1440, 2142, 2144, 2160, 4320};
    for (uint32_t width : widths) {
        for (uint32_t height : heights) {
            const auto sizes = VaShim::hevcBufferSizes(width, height);
            assert(sizes.valid);
            const uint64_t cols = (uint64_t(width) + 63) / 64;
            const uint64_t rows = (uint64_t(height) + 63) / 64;
            const uint64_t objects = ((uint64_t(width) + 15) / 16) *
                ((uint64_t(height) + 15) / 16) * 32;
            assert(sizes.cuObjectOffset >= objects && !(sizes.cuObjectOffset & 4095));
            assert(uint64_t(sizes.pakBseBytes) >= sizes.cuObjectOffset + cols * (rows + 1) * 4136);
            assert(sizes.cuRecordBytes >= cols * (rows + 1) * 64 * 32);
            assert(sizes.streamInBytes >= cols * rows * 256);
            assert(!(sizes.pakBseBytes & 4095) && !(sizes.cuRecordBytes & 4095));
        }
    }
    const auto uhd = VaShim::hevcBufferSizes(3840, 2160);
    assert(uhd.pakBseBytes > 2 * 1024 * 1024);
    assert(uhd.cuRecordBytes > 1024 * 1024);
    assert(!VaShim::hevcBufferSizes(0, 2160).valid);
    assert(!VaShim::hevcBufferSizes(3840, 0).valid);
    assert(!VaShim::hevcBufferSizes(UINT32_MAX, UINT32_MAX).valid);
    puts("PASS HEVC resolution-dependent buffer capacity");
}
