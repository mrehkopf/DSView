/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef DSVIEW_PV_DATA_LOGICSTACKINGMERGER_H
#define DSVIEW_PV_DATA_LOGICSTACKINGMERGER_H

#include <stdint.h>
#include <QString>
#include <utility>
#include <vector>
#include <libsigrok.h>
#include "../logicstackingconfig.h"

namespace pv {
namespace data {

class LogicSnapshot;

class LogicStackingMerger
{
public:
    LogicStackingMerger();

    void reset(const LogicStackingConfig &config,
               uint64_t total_sample_count,
               uint64_t samplerate,
               bool instant);

    bool append_logic(ds_device_handle handle, const sr_datafeed_logic &logic);
    void set_trigger(ds_device_handle handle, const ds_trigger_pos &trigger_pos);
    bool mark_end(ds_device_handle handle);
    bool complete() const;

    bool flush_to_snapshot(LogicSnapshot *snapshot,
                           GSList *visible_channels,
                           const std::vector<LogicStackingChannel> &visible_map);

    uint64_t master_trigger_pos() const;
    QString warning() const;

private:
    // One analyzer's captured data; [0] is the master. For a secondary it
    // also holds the mapping from merged (master) samples to its own samples.
    struct SourceBuffer
    {
        ds_device_handle handle = NULL_HANDLE;
        int analyzer = 0;
        int64_t manual_shift_ps = 0;
        int reference_channel = 0;
        std::vector<uint8_t> bytes;
        std::vector<int> enabled_phys;
        int order_by_phys[64];
        bool channels_ready = false;
        bool ended = false;
        bool have_trigger = false;
        uint64_t trigger_pos = 0;

        // Base alignment: trigger positions plus manual shift
        bool shift_ready = false;
        int64_t shift = 0;
        // source sample = anchor_source + (dest sample - anchor_dest) / scale
        bool drift_applied = false;
        long double scale = 1.0L;
        int64_t anchor_dest = 0;
        long double anchor_source = 0.0L;

        void reset(ds_device_handle h, int analyzer_index);
        QString label() const;
    };

    // master sample, secondary sample
    typedef std::pair<int64_t, int64_t> ReferencePair;

    struct ReferenceFit
    {
        // master = scale * secondary + offset
        long double scale = 1.0L;
        long double offset = 0.0L;
        size_t inlier_count = 0;
        int64_t inlier_span = 0;
        // mean master sample of the inliers and their squared deviations from it
        long double inlier_center = 0.0L;
        long double inlier_spread = 0.0L;
        long double median_offset = 0.0L;
        double median_residual = 0.0;

        // Expected timing error from edge quantization at master sample x,
        // for the fitted line or for its slope alone pivoted at `pivot`
        double error_at(int64_t x) const;
        double slope_error_at(int64_t pivot, int64_t x) const;
    };

    // The master's reference edges, shared by every secondary's fit
    struct MasterReference
    {
        bool ready = false;
        // [0] rising, [1] falling, thinned out if dense
        std::vector<int64_t> edges[2];
        int64_t pair_window = 0;
        int64_t first_edge = 0;
        int64_t last_edge = 0;
    };

    SourceBuffer* source_for_handle(ds_device_handle handle);
    const SourceBuffer& master() const;
    bool ensure_channel_order(SourceBuffer &source);
    uint64_t read_word(const SourceBuffer &source, int physical_index, uint64_t block) const;
    uint64_t shifted_word(const SourceBuffer &source, int physical_index, int64_t source_start) const;
    bool bit_at(const SourceBuffer &source, int physical_index, int64_t sample) const;
    uint64_t source_sample_count(const SourceBuffer &source) const;
    bool analyzer_visible(int analyzer,
                          const std::vector<LogicStackingChannel> &visible_map) const;
    void append_warning(const QString &message);
    void append_coverage_warnings(const std::vector<LogicStackingChannel> &visible_map);
    bool triggers_aligned(const SourceBuffer &secondary) const;
    int64_t manual_shift_samples(const SourceBuffer &secondary) const;
    int64_t shift_samples(SourceBuffer &secondary);
    void collect_reference_edges(const SourceBuffer &source,
                                 int physical_index,
                                 std::vector<int64_t> *rising,
                                 std::vector<int64_t> *falling,
                                 std::vector<int64_t> *gaps) const;
    int64_t reference_pair_window(std::vector<int64_t> &gaps) const;
    bool use_rising_edges() const;
    bool use_falling_edges() const;
    const MasterReference& master_reference();
    void pair_reference_edges(const std::vector<int64_t> master_edges[2],
                              const std::vector<int64_t> secondary_edges[2],
                              const ReferenceFit &model,
                              int64_t first,
                              int64_t last,
                              int64_t pair_window,
                              std::vector<ReferencePair> &pairs) const;
    bool fit_reference_pairs(const std::vector<ReferencePair> &pairs, ReferenceFit &fit) const;
    void refine_reference_model(const std::vector<ReferencePair> &pairs,
                                int64_t span,
                                ReferenceFit &model) const;
    bool fit_reference_timing(const SourceBuffer &secondary,
                              int64_t alignment_shift,
                              ReferenceFit &fit);
    void apply_reference_alignment(SourceBuffer &secondary, const ReferenceFit &fit);
    int64_t reference_anchor(const SourceBuffer &secondary) const;
    void update_anchor(SourceBuffer &secondary);
    void ensure_reference_correction();
    void correct_secondary(SourceBuffer &secondary);
    long double source_for_dest(const SourceBuffer &secondary, uint64_t dest_sample) const;
    int64_t rounded_source(const SourceBuffer &secondary, uint64_t dest_sample) const;
    uint64_t scaled_word(const SourceBuffer &secondary,
                         int physical_index,
                         uint64_t dest_sample) const;
    bool build_chunk(uint64_t start_sample,
                     uint64_t sample_count,
                     const std::vector<LogicStackingChannel> &visible_map,
                     std::vector<uint64_t> &words);

private:
    LogicStackingConfig _config;
    std::vector<SourceBuffer> _sources;
    uint64_t _total_sample_count;
    uint64_t _samplerate;
    bool _instant;
    bool _reference_correction_ready;
    MasterReference _master_reference;
    QString _warning;
};

}
}

#endif
