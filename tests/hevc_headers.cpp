// CPU check for the Gen12 HEVC parameter-set core. Parses the generated RBSP
// with an independent reader so the test does not depend on the writer's state.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <utility>
#include <vector>
#include "../media/hevc_core/hevc_headers.hpp"
#include "../media/hevc_core/hevc_headers.cpp"

namespace {
struct Reader {
    std::vector<uint8_t> data;
    size_t bit {};
    uint32_t u(unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            assert(bit < data.size() * 8);
            v = (v << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1);
            ++bit;
        }
        return v;
    }
    uint32_t ue() {
        unsigned zeros = 0;
        while (!u(1)) {
            ++zeros;
            assert(zeros < 32);
        }
        return (1u << zeros) - 1 + (zeros ? u(zeros) : 0);
    }
    int32_t se() {
        const uint32_t value = ue();
        return (value & 1) ? -static_cast<int32_t>((value + 1) / 2)
                           : static_cast<int32_t>(value / 2);
    }
};
Reader rbsp(const std::vector<uint8_t> &annexB) {
    assert(annexB.size() > 6);
    assert(annexB[0] == 0 && annexB[1] == 0 && annexB[2] == 0 && annexB[3] == 1);
    std::vector<uint8_t> rbsp;
    unsigned zeros = 0;
    for (size_t i = 6; i < annexB.size(); ++i) {
        const uint8_t b = annexB[i];
        if (zeros >= 2 && b == 3) { zeros = 0; continue; }
        rbsp.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return Reader {std::move(rbsp), 0};
}
uint8_t nalType(const std::vector<uint8_t> &annexB) {
    return annexB[4] >> 1;
}
void profileTierLevel(Reader &r) {
    assert(r.u(2) == 0);       // profile_space
    assert(r.u(1) == 0);       // tier
    (void)r.u(5);              // profile_idc
    (void)r.u(32);             // compatibility flags
    (void)r.u(4);              // source/constraint flags
    assert(r.u(43) == 0);      // reserved constraint bits
    (void)r.u(1);              // inbld
    (void)r.u(8);              // level_idc
}
}

int main() {
    using namespace HevcCore;
    for (const auto &config : {Config {1920, 1080}, Config {1280, 720}, Config {3840, 2160}}) {
        const auto sets = buildParameterSets(config);
        assert(nalType(sets.vps) == NalVps);
        assert(nalType(sets.sps) == NalSps);
        assert(nalType(sets.pps) == NalPps);
        assert(nalType(sets.idrHeader) == NalIdrWRADL);

        auto sps = rbsp(sets.sps);
        assert(sps.u(4) == 0);       // sps_video_parameter_set_id
        assert(sps.u(3) == 0);       // sps_max_sub_layers_minus1
        assert(sps.u(1) == 1);       // sps_temporal_id_nesting_flag
        profileTierLevel(sps);
        assert(sps.ue() == 0);       // sps_seq_parameter_set_id
        assert(sps.ue() == 1);       // chroma_format_idc
        assert(sps.ue() == config.width);
        assert(sps.ue() == config.height);
        assert(sps.u(1) == 0);       // conformance_window_flag
        assert(sps.ue() == (config.main10 ? 2 : 0)); // bit_depth_luma_minus8
        assert(sps.ue() == (config.main10 ? 2 : 0)); // bit_depth_chroma_minus8
        assert(sps.ue() == 4);       // log2_max_pic_order_cnt_lsb_minus4
        assert(sps.u(1) == 1);       // sps_sub_layer_ordering_info_present_flag
        assert(sps.ue() == 4);       // sps_max_dec_pic_buffering_minus1[0]
        assert(sps.ue() == 1);       // sps_max_num_reorder_pics[0]
        assert(sps.ue() == 0);       // sps_max_latency_increase_plus1[0]
        assert(sps.ue() == config.minCodingBlockLog2 - 3); // log2_min_cb_minus3
        // The maximum coding block stays 64x64 whatever the minimum is.
        assert(sps.ue() == 6 - config.minCodingBlockLog2); // log2_diff_max_min
        assert(sps.ue() == 0);       // log2_min_luma_transform_block_size_minus2
        assert(sps.ue() == 3);       // maximum TU remains 32x32
        assert(sps.ue() == 2);       // max_transform_hierarchy_depth_inter
        assert(sps.ue() == 2);       // 16x16 CU -> 8x8 -> 4x4 TU

        auto pps = rbsp(sets.pps);
        assert(pps.ue() == 0 && pps.ue() == 0); // PPS and SPS ids
        assert(pps.u(7) == 0); // dependent/output/extra/sign-hiding/CABAC flags
        assert(pps.ue() == 0 && pps.ue() == 0); // default reference counts
        assert(pps.ue() == 0); // initial QP delta
        assert(pps.u(2) == 0); // constrained intra, transform skip
        assert(pps.u(1) == 0); // CU QP deltas off for the constant-QP path
        assert(pps.ue() == 0 && pps.ue() == 0); // chroma QP offsets
        assert(pps.u(6) == 0); // slice chroma/weights/bypass/tiles/entropy sync
        assert(pps.u(1) == 1); // loop filter across slices
        assert(pps.u(1) == 1); // deblocking control present
        assert(pps.u(1) == 1); // per-slice override permitted
        assert(pps.u(1) == 0); // default deblocking enabled

        // Verify the slice header starts as an IDR I-slice.
        auto slice = rbsp(sets.idrHeader);
        assert(slice.u(1) == 1);     // first_slice_segment_in_pic_flag
        assert(slice.u(1) == 0);     // no_output_of_prior_pics_flag
        assert(slice.ue() == 0);     // slice_pic_parameter_set_id
        assert(slice.ue() == 2);     // slice_type I
    }

    // A 2142-line visible image uses a 2144-line coded picture. Crop units
    // are chroma samples (two luma rows in 4:2:0), not luma pixels.
    Config cropped {3808, 2144};
    cropped.visibleWidth = 3808;
    cropped.visibleHeight = 2142;
    auto cropSps = rbsp(buildParameterSets(cropped).sps);
    (void)cropSps.u(8);
    profileTierLevel(cropSps);
    assert(cropSps.ue() == 0 && cropSps.ue() == 1);
    assert(cropSps.ue() == 3808 && cropSps.ue() == 2144);
    assert(cropSps.u(1) == 1);
    assert(cropSps.ue() == 0 && cropSps.ue() == 0);
    assert(cropSps.ue() == 0 && cropSps.ue() == 1);
    assert(cropSps.ue() == 0 && cropSps.ue() == 0);
    Config full = cropped;
    full.visibleHeight = 2144;
    assert(!(cropped == full));
    cropped.visibleWidth = 3806;
    auto cropBoth = rbsp(buildParameterSets(cropped).sps);
    (void)cropBoth.u(8);
    profileTierLevel(cropBoth);
    assert(cropBoth.ue() == 0 && cropBoth.ue() == 1);
    assert(cropBoth.ue() == 3808 && cropBoth.ue() == 2144);
    assert(cropBoth.u(1) == 1);
    assert(cropBoth.ue() == 0 && cropBoth.ue() == 1);
    assert(cropBoth.ue() == 0 && cropBoth.ue() == 1);

    Config main10;
    // Each legal QP group size has a distinct PPS field. The hardware picture
    // state uses the same depth; BRC selects the minimum coding-block size.
    for (uint32_t depth = 0; depth <= 3; ++depth) {
        Config adaptive;
        adaptive.cuQpDelta = true;
        adaptive.diffCuQpDeltaDepth = depth;
        auto pps = rbsp(buildParameterSets(adaptive).pps);
        assert(pps.ue() == 0 && pps.ue() == 0);
        assert(pps.u(7) == 0);
        assert(pps.ue() == 0 && pps.ue() == 0 && pps.ue() == 0);
        assert(pps.u(2) == 0);
        assert(pps.u(1) == 1);
        assert(pps.ue() == depth);
    }
    main10.main10 = true;
    auto ten = rbsp(buildParameterSets(main10).sps);
    (void)ten.u(8);
    profileTierLevel(ten);
    (void)ten.ue(); (void)ten.ue(); (void)ten.ue(); (void)ten.ue();
    assert(ten.u(1) == 0);
    assert(ten.ue() == 2 && ten.ue() == 2);

    // Raising the smallest coding block moves it in the SPS while the maximum
    // coding block stays 64x64. The HCP picture state takes the same field, so
    // this keeps the VDEnc and the PAK on one coding tree.
    Config minCu32;
    minCu32.minCodingBlockLog2 = 5;
    auto minCuSps = rbsp(buildParameterSets(minCu32).sps);
    (void)minCuSps.u(8);
    profileTierLevel(minCuSps);
    (void)minCuSps.ue(); (void)minCuSps.ue(); (void)minCuSps.ue();
    (void)minCuSps.ue();
    assert(minCuSps.u(1) == 0);            // conformance_window_flag
    (void)minCuSps.ue(); (void)minCuSps.ue(); // bit depths
    (void)minCuSps.ue();                   // log2_max_pic_order_cnt_lsb_minus4
    assert(minCuSps.u(1) == 1);            // sub_layer_ordering_info_present
    (void)minCuSps.ue(); (void)minCuSps.ue(); (void)minCuSps.ue();
    assert(minCuSps.ue() == 2);            // log2_min_cb_minus3 -> 32x32
    assert(minCuSps.ue() == 1);            // log2_diff -> 64x64 maximum

    // The per-picture slice builder carries QP, SAO and deblocking fields.
    SliceConfig sliceConfig;
    sliceConfig.qp = 30;
    sliceConfig.saoLuma = true;
    sliceConfig.saoChroma = true;
    auto header = buildIdrSliceHeader(Config {640, 360}, sliceConfig, false);
    assert(nalType(header) == NalIdrWRADL);
    assert(header[0] == 0 && header[1] == 0 && header[2] == 0 && header[3] == 1);
    auto bits = Reader {std::vector<uint8_t>(header.begin() + 6, header.end()), 0};
    assert(bits.u(1) == 1);      // first_slice_segment_in_pic_flag
    assert(bits.u(1) == 0);      // no_output_of_prior_pics_flag
    assert(bits.ue() == 0);      // pps id
    assert(bits.ue() == 2);      // I slice
    assert(bits.u(1) == 1);      // sao luma
    assert(bits.u(1) == 1);      // sao chroma
    assert(bits.ue() == 7);      // slice_qp_delta se(4)
    assert(bits.u(1) == 1);      // deblocking_filter_override_flag
    assert(bits.u(1) == 0);      // deblocking disabled flag
    assert(bits.ue() == 0);      // beta offset se(0)
    assert(bits.ue() == 0);      // tc offset se(0)
    assert(bits.u(1) == 1);      // loop filter across slices
    assert(bits.u(1) == 1);      // byte_alignment stop bit

    // Without SAO and with deblocking disabled the header carries only the
    // disabled flag: no beta/tc offsets and no across-slices flag.
    SliceConfig noFilter;
    noFilter.qp = 26;
    noFilter.deblockingFilterDisable = true;
    auto plain = buildIdrSliceHeader(Config {640, 360}, noFilter, false);
    auto plainBits = Reader {std::vector<uint8_t>(plain.begin() + 6, plain.end()), 0};
    assert(plainBits.u(1) == 1);   // first_slice_segment_in_pic_flag
    assert(plainBits.u(1) == 0);   // no_output_of_prior_pics_flag
    assert(plainBits.ue() == 0);   // pps id
    assert(plainBits.ue() == 2);   // I slice
    assert(plainBits.u(1) == 0);   // sao luma
    assert(plainBits.u(1) == 0);   // sao chroma
    assert(plainBits.ue() == 0);   // slice_qp_delta se(0)
    assert(plainBits.u(1) == 1);   // deblocking_filter_override_flag
    assert(plainBits.u(1) == 1);   // deblocking disabled flag
    assert(plainBits.u(1) == 1);   // byte_alignment stop bit
    // A non-IDR I picture carries the plug-in's POC and an explicit empty
    // short-term reference set so the decoder can output it in display order.
    SliceConfig trail;
    trail.poc = 5;
    trail.deblockingFilterDisable = true;
    trail.idr = false;
    auto trailHeader = buildSliceHeader(Config {640, 360}, trail, false);
    assert(nalType(trailHeader) == NalTrailR);
    auto trailBits = Reader {std::vector<uint8_t>(trailHeader.begin() + 6,
        trailHeader.end()), 0};
    assert(trailBits.u(1) == 1);   // first_slice_segment_in_pic_flag
    assert(trailBits.ue() == 0);   // pps id (no no_output_of_prior_pics_flag)
    assert(trailBits.ue() == 2);   // I slice
    assert(trailBits.u(8) == 5);   // slice_pic_order_cnt_lsb
    assert(trailBits.u(1) == 0);   // short_term_ref_pic_set_sps_flag
    assert(trailBits.ue() == 0 && trailBits.ue() == 0); // empty RPS
    assert(trailBits.u(1) == 0);   // sao luma
    assert(trailBits.u(1) == 0);   // sao chroma
    assert(trailBits.ue() == 0);   // slice_qp_delta

    // A P slice signals its single list 0 reference as one negative entry of
    // an inline short-term reference picture set.
    SliceConfig p;
    p.idr = false;
    p.inter = true;
    p.poc = 7;
    p.refPoc = 3;
    p.deblockingFilterDisable = true;
    auto pHeader = buildSliceHeader(Config {640, 360}, p, false);
    assert(nalType(pHeader) == NalTrailR);
    auto pBits = Reader {std::vector<uint8_t>(pHeader.begin() + 6,
        pHeader.end()), 0};
    assert(pBits.u(1) == 1);       // first_slice_segment_in_pic_flag
    assert(pBits.ue() == 0);       // pps id
    assert(pBits.ue() == 1);       // P slice
    assert(pBits.u(8) == 7);       // slice_pic_order_cnt_lsb
    assert(pBits.u(1) == 0);       // short_term_ref_pic_set_sps_flag
    assert(pBits.ue() == 1);       // num_negative_pics
    assert(pBits.ue() == 0);       // num_positive_pics
    assert(pBits.ue() == 3);       // delta_poc_s0_minus1: 7 - 3 - 1
    assert(pBits.u(1) == 1);       // used_by_curr_pic_s0_flag
    assert(pBits.u(1) == 0);       // sao luma
    assert(pBits.u(1) == 0);       // sao chroma
    assert(pBits.u(1) == 0);       // num_ref_idx_active_override_flag
    assert(pBits.ue() == 0);       // five_minus_max_num_merge_cand
    assert(pBits.se() == 0);       // slice_qp_delta
    assert(pBits.u(1) == 1);       // deblocking_filter_override_flag
    assert(pBits.u(1) == 1);       // deblocking disabled flag

    // A low-delay B slice carries slice_type 0, the same single reference, and
    // the B-only mvd_l1_zero_flag between the override flag and the merge
    // candidate count. This is the form media-driver's low-delay mode codes.
    SliceConfig ldb = p;
    ldb.lowDelayB = true;
    auto ldbHeader = buildSliceHeader(Config {640, 360}, ldb, false);
    assert(nalType(ldbHeader) == NalTrailR);
    auto ldbBits = Reader {std::vector<uint8_t>(ldbHeader.begin() + 6,
        ldbHeader.end()), 0};
    assert(ldbBits.u(1) == 1);     // first_slice_segment_in_pic_flag
    assert(ldbBits.ue() == 0);     // pps id
    assert(ldbBits.ue() == 0);     // B slice
    assert(ldbBits.u(8) == 7);     // slice_pic_order_cnt_lsb
    assert(ldbBits.u(1) == 0);     // short_term_ref_pic_set_sps_flag
    assert(ldbBits.ue() == 1);     // num_negative_pics
    assert(ldbBits.ue() == 0);     // num_positive_pics
    assert(ldbBits.ue() == 3);     // delta_poc_s0_minus1
    assert(ldbBits.u(1) == 1);     // used_by_curr_pic_s0_flag
    assert(ldbBits.u(1) == 0);     // sao luma
    assert(ldbBits.u(1) == 0);     // sao chroma
    assert(ldbBits.u(1) == 0);     // num_ref_idx_active_override_flag
    assert(ldbBits.u(1) == 0);     // mvd_l1_zero_flag
    assert(ldbBits.ue() == 0);     // five_minus_max_num_merge_cand
    assert(ldbBits.se() == 0);     // slice_qp_delta
    assert(ldbBits.u(1) == 1);     // deblocking_filter_override_flag
    assert(ldbBits.u(1) == 1);     // deblocking disabled flag

    // A POC wrap keeps the reference inside the 8-bit LSB range.
    SliceConfig wrapped;
    wrapped.idr = false;
    wrapped.inter = true;
    wrapped.poc = 1;
    wrapped.refPoc = 254;
    auto wrappedHeader = buildSliceHeader(Config {640, 360}, wrapped, false);
    auto wrappedBits = Reader {std::vector<uint8_t>(wrappedHeader.begin() + 6,
        wrappedHeader.end()), 0};
    assert(wrappedBits.u(1) == 1);
    assert(wrappedBits.ue() == 0);
    assert(wrappedBits.ue() == 1);
    assert(wrappedBits.u(8) == 1);
    assert(wrappedBits.u(1) == 0);
    assert(wrappedBits.ue() == 1 && wrappedBits.ue() == 0);
    assert(wrappedBits.ue() == 2); // shortest signed distance is -3

    // The keep list keeps recently decoded pictures in the DPB. Entries are
    // ordered from the closest picture outwards and each delta is the gap to
    // the previous entry.
    SliceConfig keep;
    keep.idr = false;
    keep.inter = true;
    keep.poc = 7;
    keep.refPoc = 6;
    keep.rpsPocs[0] = 4;
    keep.rpsPocs[1] = 8;
    keep.rpsCount = 2;
    auto keepHeader = buildSliceHeader(Config {640, 360}, keep, false);
    auto keepBits = Reader {std::vector<uint8_t>(keepHeader.begin() + 6,
        keepHeader.end()), 0};
    assert(keepBits.u(1) == 1);
    assert(keepBits.ue() == 0);
    assert(keepBits.ue() == 1);
    assert(keepBits.u(8) == 7);
    assert(keepBits.u(1) == 0);    // inline short-term set
    assert(keepBits.ue() == 2);    // negative: POC 6 and 4
    assert(keepBits.ue() == 1);    // positive: POC 8
    assert(keepBits.ue() == 0);    // 7 -> 6
    assert(keepBits.u(1) == 1);    // 6 is the used reference
    assert(keepBits.ue() == 1);    // 6 -> 4
    assert(keepBits.u(1) == 0);    // 4 is only kept
    assert(keepBits.ue() == 0);    // 7 -> 8
    assert(keepBits.u(1) == 0);    // 8 is only kept

    // The stream concatenates four Annex B units.
    const auto stream = buildParameterSets(Config {1920, 1080}).stream();
    size_t starts = 0;
    for (size_t i = 0; i + 3 < stream.size(); ++i)
        if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 0 && stream[i + 3] == 1) ++starts;
    assert(starts == 4);
    puts("PASS HEVC VPS/SPS/PPS and IDR slice header");
    return 0;
}
