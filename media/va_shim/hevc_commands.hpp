// SPDX-License-Identifier: BSD-3-Clause
// Gen12 HEVC VDEnc/HCP picture command sequence for the shim. The packet
// builders live in media/hevc_core; this module maps the shim's picture,
// surface and buffer descriptions onto them and serializes one flat batch.
#ifndef WEX_MEDIA_VA_SHIM_HEVC_COMMANDS_HPP
#define WEX_MEDIA_VA_SHIM_HEVC_COMMANDS_HPP

#include <stdint.h>
#include "../hevc_core/hevc_geometry.hpp"
#include "../hevc_core/huc_brc_const.hpp"
#include "../hevc_core/huc_brc_dmem.hpp"

namespace VaShim {

// A bound IOSurface as seen by the GPU. The address fields are the 32-bit
// resource handles the renderer's surface manager returns; the kernel resolves
// them to graphics addresses when it processes the VDENC sideband token. Pitch
// is in bytes and uvRowOffset is the chroma plane start in rows.
struct HevcSurface {
    uint32_t address {0};
    uint32_t uvAddress {0};
    uint32_t pitch {0};
    uint32_t uvRowOffset {0};
    uint32_t width {0};
    uint32_t height {0};
    uint32_t format {0};
    bool valid {false};
};

// A driver buffer referenced by the command sequence.
struct HevcBuffer {
    uint32_t address {0};
    uint32_t size {0};
    bool valid {false};
    void *cpuAddress {nullptr};
};

// Buffers the picture sequence writes to or reads from. They come from the
// driver buffer pools or from the context's own allocations.
struct HevcBuffers {
    HevcBuffer bitstream;
    HevcBuffer pakBse;
    HevcBuffer cuRecord;
    HevcBuffer streamIn;
    HevcBuffer rowStore;
    HevcBuffer encodedSliceSize;
    HevcBuffer statistics;
    HevcBuffer secondLevel;
    // HuC PAK-integration resources.
    HevcBuffer dmem;
    HevcBuffer aggregatedStats;
    HevcBuffer brcHistory;
    HevcBuffer brcData;
    HevcBuffer tileRecord;
    // VDEnc writes scaled reconstruction pictures even for an I frame.
    HevcBuffer scaled4x;
    HevcBuffer scaled8x;
    HevcBuffer hcpStreamout;
    HevcBuffer sseRowstore;
    // Independent regions for the HCP line/column and temporal buffers.
    HevcBuffer hcpScratch;
};

constexpr uint32_t kHcpScratchRegionBytes = 512u * 1024;
// 0 deblocking line, 1/2 deblocking tile line/column, 3/4/5 metadata,
// 6/7/8 SAO, 9 current MV temporal, 10 LCU ILDB streamout, 11/12/13 upper-right
// and left recon columns, 14 collocated MV, 15 VDEnc tile row store,
// 16 VDEnc cumulative CU count streamout, 17 PAK frame statistics streamout.
// media-driver keeps the two statistics surfaces apart: m_resVdencStatsBuffer
// receives the VDEnc stream-out, m_resPakStatsBuffer the PAK's per-tile record.
constexpr uint32_t kHcpScratchRegions = 18;

// One HUC_VIRTUAL_ADDR_STATE region. used=false leaves the command dwords
// zeroed.
struct HevcHucRegion {
    uint32_t address {0};
    uint32_t offset {0};
    bool writable {false};
    bool used {false};
};

// HuC PAK-integration state for one picture. The second-level batch (buffers
// .secondLevel) holds VDENC_CMD1 / HCP_PIC_STATE / VDENC_CMD2 and is
// referenced as region 7.
struct HevcHuc {
    bool enabled {false};
    uint32_t kernelDescriptor {15};
    uint32_t dmemAddress {0};
    uint32_t dmemLength {0};
    uint32_t dmemOffset {0x2000};
    HevcHucRegion regions[16];
};

struct HevcPictureDesc {
    uint32_t width {1920};
    uint32_t height {1088};
    // Visible input extent before minimum-coding-block padding. Zero means
    // the full coded extent is visible; the SPS carries any right/bottom crop.
    uint32_t visibleWidth {0};
    uint32_t visibleHeight {0};
    uint32_t qpY {26};
    uint32_t frameNumber {0};
    // AVD picture coding type: 0 I, 1 P, 2 B.
    uint32_t codingType {0};
    // Set when the native picture has a completed L0 reference. VDEnc codes
    // it as a low-delay B slice with that reference shared by both lists.
    bool inter {false};
    uint32_t refPoc {0};
    // Pictures whose reference state must survive this picture. They are
    // listed in the slice's short-term reference picture set with
    // used_by_curr_pic clear, so a following B/P picture can still find them.
    uint32_t rpsPocs[4] {};
    uint32_t rpsCount {0};
    // The plug-in's reconstruction slot cycles independently of coding type.
    uint32_t reconstructionIndex {0};
    uint32_t nalType {19};
    uint32_t poc {0};
    // The PAK split flags must describe every TU selected by VDEnc, using
    // the same transform limits as the SPS.
    // Smallest luma coding block, as a log2 size. The SPS and the HCP picture
    // state must agree; the default is the HEVC minimum, 8x8.
    uint32_t minCodingBlockLog2 {3};
    uint32_t diffCuQpDeltaDepth {0};
    uint32_t maxTransformLog2 {HevcCore::kMaxTransformLog2};
    uint32_t transformDepth {HevcCore::kTransformHierarchyDepth};
    // sps_temporal_mvp_enabled_flag as written by the SPS builder. The HCP
    // picture state carries its inverse (TemporalMvPredDisable), and the
    // reference derives that field from the SPS, not from the slice flag.
    // This path writes zero in the slice header, so the SPS and the picture
    // state stay off until a collocated-MV buffer is produced per picture.
    bool temporalMvpEnabled {false};
    // Diagnostic: keep the P slice syntax and reference list, but ask the VDEnc
    // for intra-only picture mode, so every coding unit it supplies is intra.
    bool interIntraOnly {false};
    // Diagnostic: VDENC_CMD2.DW2.TemporalMvpEnableFlag for inter pictures. The
    // gen12 reference derives it from sps_temporal_mvp_enabled_flag for every
    // non-I picture; this path leaves it clear because the slice header does
    // not signal temporal MVP.
    bool interTemporalMvp {false};
    // HCP_PIC_STATE.DW6.LcuMaxBitsizeAllowed. media-driver forwards the
    // application's HevcPicParams field, which is zero (no limit) for constant
    // QP. This path keeps a bound so an LCU cannot overrun the slice budget.
    uint32_t lcuMaxBitsizeAllowed {81920};
    // Low-delay B profile: VDEnc picture type 3, DW8/DW9 in their low-delay
    // form, a shared L0/L1 reference, RandomAccess clear, and the HCP slice
    // state's isLowDelay bit set.
    bool lowDelay {false};
    HevcSurface source;
    HevcSurface recon;
    HevcSurface reference;
    HevcBuffers buffers;
    HevcHuc huc;
    // media-driver runs VDENC_CMD1, HCP_PIC_STATE and VDENC_CMD2 from a
    // second-level batch started with MI_BATCH_BUFFER_START, not inline in
    // the ring batch. Set once buffers.secondLevel holds that batch.
    bool secondLevelBatch {false};
    // HuC BRC Update edits this complete SLB. Execute its picture and slice
    // groups at the byte offsets reported by buildHevcBrcBatchCommands, so
    // hardware QP and the packed slice header come from the same output.
    bool brcBatchOutput {false};
    uint32_t brcPictureOffset {0};
    uint32_t brcSliceOffset {0};
    uint32_t brcBatchBytes {0};
    // PAK frame byte count for the next BRC Update, written after draining.
    uint32_t brcPakInfoAddress {0};
    bool brcPakOnlyPass {false};
    uint32_t brcPass {0};
    // First-pass image status must survive RC6 while HuC decides on repack.
    uint32_t brcLastImageStatusAddress {0};
    // VPS/SPS/PPS Annex B units inserted before the first slice header. The
    // payload already includes per-NAL emulation prevention; the combined
    // insert copies it verbatim. A null pointer leaves the insert out.
    const uint8_t *parameterSets {nullptr};
    uint32_t parameterSetBytes {0};
    // Pre-packed slice header (start code, NAL header and raw RBSP). A null
    // pointer leaves the PAK insert out of the sequence.
    const uint8_t *sliceHeader {nullptr};
    uint32_t sliceHeaderBits {0};
    // Size after emulation prevention, for the HuC DMEM slice-header field.
    uint32_t sliceHeaderEmulatedBits {0};
    // Diagnostic only: interleave MI_STORE_DATA_IMM checkpoints that write the
    // group number into the statistics buffer. A landed value identifies the
    // last packet group the engine executed before it stopped.
    bool traceCheckpoints {false};
    // Diagnostic HuC readiness samples, taken after the picture drains.
    bool probeHucStatus {false};
    uint32_t probeHucKernel {0xffffffffu};
    // Set once the 32-bit handles have been replaced by graphics addresses.
    // Upper bounds can then be base plus allocation size; a handle cannot.
    bool resolvedAddresses {false};
    // Diagnostic only: leave out the NAL and slice-header PAK inserts so the
    // walker runs without the PAK having any header payload to write.
    bool skipPak {false};
    // Diagnostic only: leave out the VDEnc walker so no PAK objects are
    // produced. Tells whether the PAK executes its slice commands on their own
    // or only once the VDEnc feeds it objects.
    bool skipWalker {false};
    // Diagnostic: end the picture-level command group with a video-pipeline
    // cache-invalidate flush before the slice-level commands. media-driver
    // submits the two levels as separate command buffers, so each picture-level
    // submission ends with a batch-boundary flush; this reproduces that
    // boundary inside one batch.
    bool pictureFlush {false};
    // Diagnostic: let the VDEnc walk one CTB instead of the whole picture.
    // Distinguishes a stall that scales with the object-stream volume from one
    // that happens before any object is handed over.
    bool oneCtb {false};
    // Ask the VDEnc to write its LCU PAK object commands to the object buffer
    // instead of handing them to the PAK over the internal path.
    bool pakObjectStreamOut {false};
    // Diagnostic: name the VDEnc's CU-record buffer as the PAK's indirect CU
    // object surface, so a CU record written there is read back by the PAK.
    bool cuObjectAlias {false};
    // Diagnostic: start the PAK's indirect CU object at the same base as the
    // VDEnc's LCU PAK object command stream. The gen12 softlet HEVC VDEnc packet
    // leaves m_mvOffset at zero, so both sides use the buffer base; the legacy
    // packet adds the 16x16 object block before the CU records.
    bool cuObjectAtBase {false};
    // Downscale recon slots. The VDEnc writes this picture's own 4x/8x
    // downscale into scaledSlot of the shared pool and reads the reference
    // picture's downscale from referenceScaledSlot, which is the slot the
    // reference picture wrote. Keeping the two apart is what lets an inter
    // picture write its own downscale without destroying the reference's.
    uint32_t scaledSlot {0};
    uint32_t referenceScaledSlot {0};
    bool referenceScaledValid {false};
    // Signal per-CU QP deltas. The Gen12 HCP expects this in VDEnc mode and
    // codes the syntax whenever a transform block has coefficients.
    bool cuQpDeltaEnabled {false};
    // HuC BRC/ACQP changes the HCP rate-control mode independently of CQP.
    bool rateControlEnabled {false};
    // Diagnostic only: end the last slice one CTB past the frame instead of
    // the (0,0) pair media-driver programs, to test whether the PAK reads the
    // next-slice position as its stop point.
    bool sliceEndAtFrameEnd {false};
    // Diagnostic only: HCP_PIC_STATE.DW19.PartialFrameUpdateMode. The gen12
    // reference clears it; setting it tests the picture-state row controls
    // the PAK uses while it walks the frame.
    bool partialFrameUpdate {false};
    // A post-sync write after all media work distinguishes normal completion
    // from a fence released by a GPU reset. Zero disables the write.
    uint32_t completionAddress {0};
    uint32_t completionTag {0};
};

// DWORDs needed for the sequence, including the trailing MI_BATCH_BUFFER_END.
uint32_t hevcPictureCommandDwords(const HevcPictureDesc &desc);

// Serializes the sequence into destination and returns the number of DWORDs
// written, or 0 when destination is too small.
uint32_t buildHevcPictureCommands(const HevcPictureDesc &desc,
    uint32_t *destination, uint32_t capacityDwords);

// Second-level batch: VDENC_CMD1, HCP_PIC_STATE, VDENC_CMD2 and
// MI_BATCH_BUFFER_END. Written into its own buffer so the HuC kernel reads it
// through region 7.
uint32_t hevcSecondLevelCommandDwords(const HevcPictureDesc &desc);
uint32_t buildHevcSecondLevelCommands(const HevcPictureDesc &desc,
    uint32_t *destination, uint32_t capacityDwords);

struct HevcBrcBatchLayout {
    HevcCore::Huc::BrcUpdateParams update;
    HevcCore::Huc::BrcSliceLayout slice;
};

// Intel's complete BRC SLB: pipe mode, picture state and slice groups, each
// ending in MI_BATCH_BUFFER_END. The slice group includes PAK headers and
// eight padding DWORDs for HuC's header edits. Offsets are bytes from SLB base.
// The current encode path admits one slice and no weighted prediction.
uint32_t buildHevcBrcBatchCommands(const HevcPictureDesc &desc,
    uint32_t *destination, uint32_t capacityDwords, HevcBrcBatchLayout &layout);

// Check HuC's output before executing it: bounded packet groups, unchanged
// geometry/bit depth/slice type, and coherent picture/VDEnc QPs. A differing
// slice QP requires CU-QP syntax; the header adapter validates that slice QP.
// The returned QP is written only after the whole output has passed validation.
bool validateHevcBrcBatchOutput(const HevcPictureDesc &desc,
    const HevcBrcBatchLayout &layout, const uint32_t *commands,
    uint32_t capacityDwords, uint32_t &qp);

// Gen12 NV12 Y-tiled scaled reconstruction layout, matching iHD's tracked
// buffers. The returned address is supplied separately by the resource map.
// True when the picture's source surface is a P010 10-bit layout.
bool pictureIsTenBit(const HevcPictureDesc &desc);

HevcSurface hevcScaledSurface(uint32_t width, uint32_t height, bool eightX);
uint32_t hevcScaledSurfaceBytes(const HevcSurface &surface);

}

#endif
