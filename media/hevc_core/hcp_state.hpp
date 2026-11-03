// SPDX-License-Identifier: MIT
// Gen12 HCP (HEVC codec engine) picture-level commands. Field layouts follow
// media-driver's mhw_vdbox_hcp_hwcmd_g12_X and the builders in
// mhw_vdbox_hcp_g12_X / mhw_vdbox_hcp_generic.
#ifndef WEX_MEDIA_HEVC_CORE_HCP_STATE_HPP
#define WEX_MEDIA_HEVC_CORE_HCP_STATE_HPP

#include "hw_command.hpp"

namespace HevcCore {
namespace Hcp {

// Media instruction command values for the HCP engine (DWORD 0, bits 22:16).
enum Command : uint32_t {
    CommandPipeModeSelect = 0,
    CommandSurfaceState = 1,
    CommandPipeBufAddrState = 2,
    CommandIndObjBaseAddrState = 3,
    CommandQmState = 4,
    CommandFqmState = 5,
    CommandPicState = 16,
    CommandRefIdxState = 18,
    CommandPakInsertObject = 34,
};

// CODECHAL_HCP_SURFACE_ID values used by the encode path.
enum SurfaceId : uint32_t {
    SurfaceDecoded = 0,
    SurfaceSource = 1,
    SurfaceLast = 2,
    SurfaceGolden = 3,
    SurfaceAltRef = 4,
    SurfaceReference = 5,
};

struct PipeModeSelect {
    bool streamOut {true};          // VDEnc encode keeps stream-out enabled
    bool deblockerStreamOut {false};
    bool advancedRateControl {false};
    bool rdoq {false};
    bool tileBasedEngine {false};
    bool prefetchDisable {false};
    bool separateTileProgramming {false};
    bool frameReconstructionDisable {false};
    uint32_t multiEngineMode {0};   // FE_LEGACY
    uint32_t pipeWorkingMode {0};   // legacy
    uint32_t phaseIndicator {0};
};

enum SurfaceFormatValue : uint32_t {
    // Gen12 AddHcpEncodeSurfaceStateCmd uses the split MSB/LSB layout for
    // 10-bit reconstruction and references, and packed P010 for raw input.
    FormatP010Variant = 3,
    FormatPlanar420_8 = 4,
    FormatP010 = 13,
};

struct SurfaceState {
    uint32_t surfaceId {SurfaceSource};
    uint32_t pitch {};              // bytes
    uint32_t uvRowOffset {};        // chroma plane start, in rows
    uint32_t format {FormatPlanar420_8};
    uint32_t defaultAlpha {};
    uint32_t memoryCompressionEnable {};
    uint32_t compressionType {};
    uint32_t compressionFormat {};
};

// One 64B aligned buffer reference: address plus the surface control DWORD.
struct AddressBlock {
    uint64_t address {};
    uint32_t control {};
};

// Surface tiling as the reference attribute encodes it. media-driver maps a
// Y-tiled or linear surface to TRMODE_NONE and only the sub-tiled modes carry
// a non-zero code (Mhw_ConvertToTRMode).
enum class TileMode : uint32_t {
    None = 0,
    Y = 0,
    YS = 1,
    YF = 2,
};

// HCP_PIPE_BUF_ADDR_STATE DW53: the cache policy shared by every reference
// address plus the tiling mode of the first reference surface. The tiling
// field is bits 13:14, the MOCS index bits 1:6.
uint32_t referenceAttributes(TileMode tiling, uint32_t cachePolicy = 0);

// HCP_PIPE_BUF_ADDR_STATE. References and collocated MV buffers share one
// control DWORD each, as the command has a single attribute field for them.
struct PipeBufAddrParams {
    AddressBlock decodedPicture;      // 4KB aligned
    AddressBlock deblockingLine;
    AddressBlock deblockingTileLine;
    AddressBlock deblockingTileColumn;
    AddressBlock metadataLine;
    AddressBlock metadataTileLine;
    AddressBlock metadataTileColumn;
    AddressBlock saoLine;
    AddressBlock saoTileLine;
    AddressBlock saoTileColumn;
    AddressBlock currentMvTemporal;
    AddressBlock references[8];
    uint32_t referenceControl {};
    AddressBlock originalUncompressed;
    AddressBlock streamout;
    AddressBlock encodedSliceSize;
    AddressBlock lcuIldb;
    AddressBlock collocatedMv[8];
    uint32_t collocatedMvControl {};
    AddressBlock frameStatistics;
    AddressBlock sseSourceRowstore;
    AddressBlock mvUpperRight;
    AddressBlock intraUpperRight;
    AddressBlock intraLeftRecon;
};

// HCP_IND_OBJ_BASE_ADDR_STATE. The bitstream object and its access upper
// bound are decode side; the CU and PAK-BSE objects are encoder side.
struct IndObjParams {
    AddressBlock bitstreamObject;     // 4KB aligned
    AddressBlock bitstreamUpperBound; // 4KB aligned
    AddressBlock cuObject;
    AddressBlock pakBseObject;
    AddressBlock pakBseUpperBound;    // 4KB aligned
};

// One HCP_REF_IDX_STATE entry. frameId selects one of the eight reference
// base addresses in HCP_PIPE_BUF_ADDR_STATE; pocDelta is the picture order
// count difference the PAK needs for the reference list.
struct RefListEntry {
    uint32_t frameId {};
    int32_t pocDelta {};
    bool longTerm {false};
};

// HCP_REF_IDX_STATE for one reference list. A P slice sends list 0 only.
struct RefIdxParams {
    uint32_t list {};
    uint32_t numRefs {1};
    RefListEntry entries[16];
};

// HCP_TILE_CODING_STATE. media-driver sends this once per tile before the
// slice commands. The picture here is a single tile and a single pipe, so the
// tile spans the whole frame in minimum coding blocks (8x8).
struct TileCodingParams {
    uint32_t activeBePipes {1};
    uint32_t tileColumnsInFrame {1};
    uint32_t tileRowStoreSelect {};     // row index % 2
    uint32_t tileColumnStoreSelect {};  // column index % 2
    uint32_t tileStartLcuX {};
    uint32_t tileStartLcuY {};
    bool lastTileOfColumn {true};
    bool lastTileOfRow {true};
    uint32_t tileWidthInMinCbMinus1 {};
    uint32_t tileHeightInMinCbMinus1 {};
    uint32_t bitstreamByteOffset {};
    bool bitstreamByteOffsetEnable {false};
    uint32_t pakFrameStatisticsOffset {};
    uint32_t cuLevelStreamoutOffset {};
    uint32_t sliceSizeStreamoutOffset {};
    uint32_t cuRecordOffset {};
    uint32_t sseRowstoreOffset {};
    uint32_t saoRowstoreOffset {};
    uint32_t tileSizeStreamoutOffset {};
};

Packet pipeModeSelect(const PipeModeSelect &params);
// VD_CONTROL_STATE for the HCP pipe, sent with the initialization bit before
// the pipe mode select. The HCP engine ignores picture commands until it
// receives this and VDENC_CONTROL_STATE.
Packet controlState(bool initialization = true);
Packet surfaceState(const SurfaceState &surface);
Packet pipeBufAddrState(const PipeBufAddrParams &params);
Packet refIdxState(const RefIdxParams &params);
Packet indObjBaseAddrState(const IndObjParams &params);
Packet tileCodingState(const TileCodingParams &params);

}
}

#endif
