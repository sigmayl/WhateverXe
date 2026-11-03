// SPDX-License-Identifier: MIT
#include "hevc_headers.hpp"
#include "bit_writer.hpp"
#include <string.h>

namespace HevcCore {
namespace {
constexpr uint32_t MaxSubLayers = 0;

void profileTierLevel(BitWriter &w, const Config &c) {
    const uint32_t profile = c.main10 ? 2 : 1;
    w.u(0, 2);          // general_profile_space
    w.u(0, 1);          // general_tier_flag: Main
    w.u(profile, 5);    // general_profile_idc
    for (unsigned i = 0; i < 32; ++i)
        w.u(i == profile || (profile == 2 && i == 1) ? 1 : 0, 1);
    w.u(1, 1);          // general_progressive_source_flag
    w.u(0, 1);          // general_interlaced_source_flag
    w.u(1, 1);          // general_non_packed_constraint_flag
    w.u(1, 1);          // general_frame_only_constraint_flag
    // Main/Main10 use the reserved constraint form followed by inbld.
    w.u(0, 43);
    w.u(0, 1);          // general_inbld_flag
    w.u(c.level, 8);    // general_level_idc
    // MaxSubLayers is zero: no profile_present/level_present flags or fields.
    (void)MaxSubLayers;
}

void vps(BitWriter &w, const Config &c) {
    w.u(0, 4);          // vps_video_parameter_set_id
    w.u(1, 1);          // vps_base_layer_internal_flag
    w.u(1, 1);          // vps_base_layer_available_flag
    w.u(0, 6);          // vps_max_layers_minus1
    w.u(0, 3);          // vps_max_sub_layers_minus1
    w.u(1, 1);          // vps_temporal_id_nesting_flag
    w.u(0xffff, 16);    // vps_reserved_0xffff_16bits
    profileTierLevel(w, c);
    w.u(1, 1);          // vps_sub_layer_ordering_info_present_flag
    w.ue(4);            // vps_max_dec_pic_buffering_minus1[0]
    w.ue(1);            // vps_max_num_reorder_pics[0]
    w.ue(0);            // vps_max_latency_increase_plus1[0]
    w.u(0, 6);          // vps_max_layer_id
    w.ue(0);            // vps_num_layer_sets_minus1
    w.u(0, 1);          // vps_timing_info_present_flag
    w.u(0, 1);          // vps_extension_flag
    w.trailing();
}

void sps(BitWriter &w, const Config &c) {
    w.u(0, 4);          // sps_video_parameter_set_id
    w.u(0, 3);          // sps_max_sub_layers_minus1
    w.u(1, 1);          // sps_temporal_id_nesting_flag
    profileTierLevel(w, c);
    w.ue(0);            // sps_seq_parameter_set_id
    w.ue(1);            // chroma_format_idc: 4:2:0
    w.ue(c.width);
    w.ue(c.height);
    const uint32_t visibleWidth = c.visibleWidth ? c.visibleWidth : c.width;
    const uint32_t visibleHeight = c.visibleHeight ? c.visibleHeight : c.height;
    const bool cropped = visibleWidth != c.width || visibleHeight != c.height;
    w.u(cropped, 1);    // conformance_window_flag
    if (cropped) {
        w.ue(0);       // conf_win_left_offset
        w.ue((c.width - visibleWidth) / 2);
        w.ue(0);       // conf_win_top_offset
        w.ue((c.height - visibleHeight) / 2); // 4:2:0 chroma units
    }
    w.ue(c.main10 ? 2 : 0); // bit_depth_luma_minus8
    w.ue(c.main10 ? 2 : 0);
    w.ue(4);            // log2_max_pic_order_cnt_lsb_minus4
    w.u(1, 1);          // sps_sub_layer_ordering_info_present_flag
    w.ue(4);            // sps_max_dec_pic_buffering_minus1[0]
    w.ue(1);            // sps_max_num_reorder_pics[0]
    w.ue(0);            // sps_max_latency_increase_plus1[0]
    w.ue(c.minCodingBlockLog2 - 3); // log2_min_luma_coding_block_size_minus3
    w.ue(6 - c.minCodingBlockLog2); // log2_diff_max_min_luma_coding_block_size: 64
    w.ue(0);            // log2_min_luma_transform_block_size_minus2
    w.ue(c.maxTransformLog2 - 2); // log2_diff_max_min_luma_transform_block_size
    w.ue(c.transformDepth); // max_transform_hierarchy_depth_inter
    w.ue(c.transformDepth); // max_transform_hierarchy_depth_intra
    w.u(0, 1);          // scaling_list_enabled_flag
    w.u(1, 1);          // amp_enabled_flag
    w.u(1, 1);          // sample_adaptive_offset_enabled_flag
    w.u(0, 1);          // pcm_enabled_flag
    w.ue(0);            // num_short_term_ref_pic_sets
    w.u(0, 1);          // long_term_ref_pics_present_flag
    w.u(c.temporalMvp ? 1 : 0, 1); // sps_temporal_mvp_enabled_flag
    w.u(1, 1);          // strong_intra_smoothing_enabled_flag
    w.u(0, 1);          // vui_parameters_present_flag
    w.u(0, 1);          // sps_extension_present_flag
    w.trailing();
}

void pps(BitWriter &w, const Config &c) {
    w.ue(0);            // pps_pic_parameter_set_id
    w.ue(0);            // pps_seq_parameter_set_id
    w.u(0, 1);          // dependent_slice_segments_enabled_flag
    w.u(0, 1);          // output_flag_present_flag
    w.u(0, 3);          // num_extra_slice_header_bits
    w.u(0, 1);          // sign_data_hiding_enabled_flag
    w.u(0, 1);          // cabac_init_present_flag
    w.ue(0);            // num_ref_idx_l0_default_active_minus1
    w.ue(0);            // num_ref_idx_l1_default_active_minus1
    w.se(0);            // init_qp_minus26
    w.u(0, 1);          // constrained_intra_pred_flag
    w.u(0, 1);          // transform_skip_enabled_flag
    // This path encodes constant QP and has no per-CU QP source (no ACQP,
    // BRC or VDEnc stream-in), so the whole slice uses one QP. Signalling CU
    // deltas the VDEnc never filled in produced out-of-range syntax.
    w.u(c.cuQpDelta ? 1 : 0, 1); // cu_qp_delta_enabled_flag
    if (c.cuQpDelta)
        w.ue(c.diffCuQpDeltaDepth); // diff_cu_qp_delta_depth
    w.se(0);            // pps_cb_qp_offset
    w.se(0);            // pps_cr_qp_offset
    w.u(0, 1);          // pps_slice_chroma_qp_offsets_present_flag
    w.u(0, 1);          // weighted_pred_flag
    w.u(0, 1);          // weighted_bipred_flag
    w.u(0, 1);          // transquant_bypass_enabled_flag
    w.u(0, 1);          // tiles_enabled_flag
    w.u(0, 1);          // entropy_coding_sync_enabled_flag
    w.u(1, 1);          // pps_loop_filter_across_slices_enabled_flag
    w.u(1, 1);          // deblocking_filter_control_present_flag
    w.u(1, 1);          // deblocking_filter_override_enabled_flag
    w.u(0, 1);          // pps_deblocking_filter_disabled_flag
    w.se(0);            // pps_beta_offset_div2
    w.se(0);            // pps_tc_offset_div2
    w.u(0, 1);          // pps_scaling_list_data_present_flag
    w.u(0, 1);          // lists_modification_present_flag
    w.ue(0);            // log2_parallel_merge_level_minus2
    w.u(0, 1);          // slice_segment_header_extension_present_flag
    w.u(0, 1);          // pps_extension_present_flag
    w.trailing();
}

void sliceHeader(BitWriter &w, const Config &c, const SliceConfig &slice) {
    // log2_max_pic_order_cnt_lsb_minus4 is 4, so the LSB field is 8 bits.
    constexpr uint32_t PocLsbBits = 8;
    w.u(1, 1);          // first_slice_segment_in_pic_flag
    if (slice.idr)
        w.u(0, 1);      // no_output_of_prior_pics_flag
    w.ue(0);            // slice_pic_parameter_set_id
    // 0 B, 1 P, 2 I. A VDEnc HEVC slice is B or I only, so the inter pictures
    // this path codes are low-delay B: list 1 mirrors list 0.
    w.ue(slice.lowDelayB && slice.inter ? 0 : (slice.inter ? 1 : 2)); // slice_type
    if (!slice.idr) {
        // Every non-IDR picture carries a POC so the decoder can output the
        // plug-in's reordered frames in display order. num_short_term_ref_pic
        // _sets is zero in the SPS, so the slice carries an explicit set: a
        // P slice lists its one L0 reference as a negative entry, an I slice
        // carries an empty set.
        w.u(slice.poc & ((1u << PocLsbBits) - 1), PocLsbBits);
        w.u(0, 1);      // short_term_ref_pic_set_sps_flag
        // The inline set carries the used reference plus the pictures that
        // must stay in the DPB for later pictures. POC differences are taken
        // modulo the 8-bit LSB and reduced to the shortest signed distance.
        const auto signedDelta = [](uint32_t poc, uint32_t ref) {
            int32_t d = static_cast<int32_t>((ref - poc)
                & ((1u << PocLsbBits) - 1));
            if (d > (1 << (PocLsbBits - 1)))
                d -= (1 << PocLsbBits);
            return d;
        };
        int32_t deltas[5] {};
        uint32_t count = 0;
        const auto addPoc = [&](uint32_t ref) {
            const int32_t d = signedDelta(slice.poc, ref);
            if (d == 0)
                return;
            for (uint32_t i = 0; i < count; ++i)
                if (deltas[i] == d)
                    return;
            if (count < 5)
                deltas[count++] = d;
        };
        if (slice.inter)
            addPoc(slice.refPoc);
        for (uint32_t i = 0; i < slice.rpsCount && i < 4; ++i)
            addPoc(slice.rpsPocs[i]);
        const int32_t refDelta = slice.inter
            ? signedDelta(slice.poc, slice.refPoc) : 0;
        const auto containsDelta = [&](int32_t d) {
            for (uint32_t i = 0; i < count; ++i)
                if (deltas[i] == d)
                    return true;
            return false;
        };
        uint32_t negative = 0;
        uint32_t positive = 0;
        for (uint32_t i = 0; i < count; ++i)
            (deltas[i] < 0 ? negative : positive) += 1;
        w.ue(negative); // num_negative_pics
        w.ue(positive); // num_positive_pics
        // Negative entries run from the closest picture outwards; each syntax
        // element is the gap to the previous entry.
        int32_t previous = 0;
        for (int32_t d = -1; d >= -128; --d) {
            if (!containsDelta(d))
                continue;
            w.ue(previous - d - 1); // delta_poc_s0_minus1
            w.u(slice.inter && d == refDelta ? 1 : 0, 1); // used_by_curr_pic_s0_flag
            previous = d;
        }
        previous = 0;
        for (int32_t d = 1; d <= 127; ++d) {
            if (!containsDelta(d))
                continue;
            w.ue(d - previous - 1); // delta_poc_s1_minus1
            w.u(slice.inter && d == refDelta ? 1 : 0, 1); // used_by_curr_pic_s1_flag
            previous = d;
        }
        // sps_temporal_mvp_enabled_flag is zero on this path, so the slice
        // header carries no temporal-MVP bit at all. Writing one anyway shifts
        // every following syntax element by a bit, which the slice header's
        // byte alignment cannot absorb.
        if (c.temporalMvp)
            w.u(0, 1);  // slice_temporal_mvp_enabled_flag
    }
    w.u(slice.saoLuma ? 1 : 0, 1);   // slice_sao_luma_flag
    w.u(slice.saoChroma ? 1 : 0, 1); // slice_sao_chroma_flag
    if (slice.inter && !slice.idr) {
        // Every P/B slice carries the override flag; the PPS default already
        // selects one reference in each list. lists_modification_present,
        // weighted_pred and cabac_init_present are zero, and temporal MVP is
        // off, so only the B-only mvd_l1_zero_flag sits in between, followed
        // by five_minus_max_num_merge_cand.
        w.u(0, 1);      // num_ref_idx_active_override_flag
        if (slice.lowDelayB)
            w.u(0, 1);  // mvd_l1_zero_flag
        w.ue(0);        // five_minus_max_num_merge_cand
    }
    w.se(static_cast<int32_t>(slice.qp) - 26); // slice_qp_delta
    // The PPS permits per-slice overrides. The override flag must precede
    // the disabled flag and optional beta/tc offsets.
    w.u(1, 1);          // deblocking_filter_override_flag
    w.u(slice.deblockingFilterDisable ? 1 : 0, 1);
    if (!slice.deblockingFilterDisable) {
        w.se(0);        // slice_beta_offset_div2
        w.se(0);        // slice_tc_offset_div2
    }
    // The across-slices flag is present only when a loop filter is in use.
    if (slice.saoLuma || slice.saoChroma || !slice.deblockingFilterDisable)
        w.u(1, 1);      // slice_loop_filter_across_slices_enabled_flag
    // pps_slice_segment_header_extension_present_flag is zero.
    // byte_alignment(): one bit followed by zero bits.
    w.trailing();
}
}

ParameterSets buildParameterSets(const Config &config, bool emulate) {
    ParameterSets out;
    BitWriter v, s, p, slice;
    vps(v, config);
    sps(s, config);
    pps(p, config);
    const SliceConfig defaults;
    sliceHeader(slice, config, defaults);
    out.vps = v.annexB(NalVps, emulate);
    out.sps = s.annexB(NalSps, emulate);
    out.pps = p.annexB(NalPps, emulate);
    out.idrHeader = slice.annexB(NalIdrWRADL, emulate);
    return out;
}

std::vector<uint8_t> buildSliceHeader(const Config &config,
    const SliceConfig &slice, bool emulate) {
    BitWriter w;
    sliceHeader(w, config, slice);
    return w.annexB(slice.idr ? NalIdrWRADL : NalTrailR, emulate);
}

std::vector<uint8_t> buildIdrSliceHeader(const Config &config,
    const SliceConfig &slice, bool emulate) {
    BitWriter w;
    SliceConfig idr = slice;
    idr.idr = true;
    sliceHeader(w, config, idr);
    return w.annexB(NalIdrWRADL, emulate);
}
}
