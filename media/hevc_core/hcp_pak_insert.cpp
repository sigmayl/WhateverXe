// SPDX-License-Identifier: MIT
#include "hcp_pak_insert.hpp"

#include <assert.h>
#include <string.h>

#include "hw/mhw_vdbox_hcp_hwcmd_g12_X.h"

namespace HevcCore {
namespace Hcp {
namespace {

constexpr Field EndOfSlice {1, 1, 1};
constexpr Field LastHeader {1, 2, 2};
constexpr Field EmulationInsert {1, 3, 3};
constexpr Field SkipEmulationCount {1, 4, 7};
constexpr Field DataBitsInLastDw {1, 8, 13};
constexpr Field SliceHeaderIndicator {1, 14, 14};
constexpr Field HeaderLengthExcludeFrameSize {1, 15, 15};
constexpr Field DataByteOffset {1, 16, 17};
constexpr Field IndirectPayload {1, 31, 31};

constexpr uint32_t kNalEndOfSequence = 36;
constexpr uint32_t kNalEndOfBitstream = 37;

void appendWord(std::vector<uint8_t> &bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 24));
}

}

PakInsertCommand pakInsert(const PakInsertParams &params) {
    mhw_vdbox_hcp_g12_X::HCP_PAK_INSERT_OBJECT_CMD cmd {};
    PakInsertCommand result {packetFrom(cmd), {}};

    auto setHeader = [&result](bool endOfSlice, bool lastHeader, bool emulation,
        uint8_t skipCount, uint32_t dataBits, bool resetPosition) {
        result.header.set(EndOfSlice, endOfSlice ? 1 : 0);
        result.header.set(LastHeader, lastHeader ? 1 : 0);
        result.header.set(EmulationInsert, emulation ? 1 : 0);
        result.header.set(SkipEmulationCount, skipCount);
        result.header.set(DataBitsInLastDw, dataBits);
        result.header.set(SliceHeaderIndicator, resetPosition ? 1 : 0);
        result.header.set(HeaderLengthExcludeFrameSize, 0);
        result.header.set(DataByteOffset, 0);
        result.header.set(IndirectPayload, 0);
    };

    if (params.lastPicInSeq && params.lastPicInStream) {
        setHeader(true, true, false, 0, 16, false);
        appendWord(result.payload, (1u << 16) | ((kNalEndOfSequence << 1) << 24));
        appendWord(result.payload, 1u | (1u << 24));
        appendWord(result.payload, (kNalEndOfBitstream << 1) | (1u << 8));
        result.header.set(Field {0, 0, 11}, result.payload.size() / 4);
        return result;
    }

    if (params.lastPicInSeq || params.lastPicInStream) {
        setHeader(true, true, false, 0, 8, false);
        const uint32_t nalType = params.lastPicInSeq ? kNalEndOfSequence : kNalEndOfBitstream;
        appendWord(result.payload, (1u << 16) | ((nalType << 1) << 24));
        appendWord(result.payload, 1u);
        result.header.set(Field {0, 0, 11}, result.payload.size() / 4);
        return result;
    }

    const uint32_t byteSize = (params.bitSize + 7) / 8;
    uint32_t dataBitsInLastDw = params.bitSize % 32;
    if (dataBitsInLastDw == 0)
        dataBitsInLastDw = 32;

    setHeader(params.endOfSlice, params.lastHeader, params.emulationByteInsert,
        params.skipEmulationByteCount, dataBitsInLastDw, params.resetBitstreamStartingPos);

    if (byteSize) {
        assert(params.data != nullptr);
        const uint32_t padded = (byteSize + 3) & ~3u;
        result.payload.resize(padded, 0);
        memcpy(result.payload.data(), params.data, byteSize);
    }

    result.header.set(Field {0, 0, 11}, result.payload.size() / 4);
    return result;
}

}
}
