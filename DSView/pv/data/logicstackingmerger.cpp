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
 * 
 * LogicStackingMerger:
 * ====================
 * 
 * Take captured logic data from two DSLogic analyzers and combine them into
 * one logic snapshot. Supplementary signals are used to:
 *
 *  - Trigger the secondary analyzer to start its capture when the primary's
 *    trigger condition is met. Due to design limitations only the primary
 *    analyzer's channels may be used to set up the trigger.
 * 
 *  - Align and synchronize the two captures; since there is a small delay for
 *    the secondary analyzer to get triggered, and the two analyzers each have
 *    unrelated clock generators which will drift apart, a common signal can be
 *    supplied to both analyzers to properly align the captures and keep them
 *    from drifting apart (correction is applied to the captured data after
 *    recording).
 *    Reference edges are paired starting at the trigger point and tracked
 *    outwards, so the drift may add up to many edge periods over a capture.
 *    For this to work properly, the common synchronization signal should have
 *    edges that are at least 100 samples apart.
 *
 */

#include "logicstackingmerger.h"
#include "logicsnapshot.h"
#include "../log.h"
#include "../ui/langresource.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string.h>

namespace pv {
namespace data {

namespace {

// Drift beyond this is implausible for two crystal oscillators
const double kMaxDriftPpm = 1000.0;
// Edge quantization alone keeps the typical residual below one sample
const double kMaxMedianResidual = 2.0;
// Pairs closer than this (in samples) to the fitted timing are never trimmed
const double kMinOutlierLimit = 2.0;
// Edge timing noise assumed from sample quantization on both analyzers
const double kMinTimingNoise = 0.5;
// A fit extrapolated further than this is not trusted
const double kMaxTimingError = 2.0;
const size_t kMinSlopePairs = 8;
// Master edges used per polarity
const size_t kMaxReferenceEdges = 1 << 20;
const int kMaxTrimRounds = 3;

template <typename T>
T median_in_place(std::vector<T> &values)
{
    if (values.empty())
        return T();

    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    return values[values.size() / 2];
}

// Least-squares fit of master = scale * secondary + offset over the used pairs
bool fit_affine(const std::vector<std::pair<int64_t, int64_t> > &pairs,
                const std::vector<char> &use,
                long double &scale,
                long double &offset)
{
    long double mean_secondary = 0.0L;
    long double mean_master = 0.0L;
    size_t count = 0;
    for (size_t i = 0; i < pairs.size(); i++){
        if (!use[i])
            continue;

        mean_master += (long double)pairs[i].first;
        mean_secondary += (long double)pairs[i].second;
        count++;
    }

    if (count < 2)
        return false;

    mean_master /= (long double)count;
    mean_secondary /= (long double)count;

    long double numerator = 0.0L;
    long double denominator = 0.0L;
    for (size_t i = 0; i < pairs.size(); i++){
        if (!use[i])
            continue;

        const long double secondary_delta = (long double)pairs[i].second - mean_secondary;
        const long double master_delta = (long double)pairs[i].first - mean_master;
        numerator += secondary_delta * master_delta;
        denominator += secondary_delta * secondary_delta;
    }

    if (denominator == 0.0L)
        return false;

    scale = numerator / denominator;
    offset = mean_master - scale * mean_secondary;
    return true;
}

}

void LogicStackingMerger::SourceBuffer::reset(ds_device_handle h)
{
    handle = h;
    bytes.clear();
    enabled_phys.clear();
    for (int i = 0; i < 64; i++)
        order_by_phys[i] = -1;
    channels_ready = false;
    ended = false;
    have_trigger = false;
    trigger_pos = 0;
}

LogicStackingMerger::LogicStackingMerger()
{
    reset(LogicStackingConfig(), 0, 0, false);
}

void LogicStackingMerger::reset(const LogicStackingConfig &config,
                                uint64_t total_sample_count,
                                uint64_t samplerate,
                                bool instant)
{
    _config = config;
    _master.reset(config.master_handle);
    _secondary.reset(config.secondary_handle);
    _total_sample_count = total_sample_count;
    _samplerate = samplerate;
    _instant = instant;
    _secondary_shift_ready = false;
    _secondary_shift = 0;
    _reference_correction_ready = false;
    _secondary_drift_applied = false;
    _secondary_scale = 1.0L;
    _secondary_anchor_dest = 0;
    _secondary_anchor_source = 0.0L;
    _warning.clear();
}

LogicStackingMerger::SourceBuffer* LogicStackingMerger::source_for_handle(ds_device_handle handle)
{
    if (handle == _master.handle)
        return &_master;
    if (handle == _secondary.handle)
        return &_secondary;
    return NULL;
}

const LogicStackingMerger::SourceBuffer* LogicStackingMerger::source_for_analyzer(int analyzer) const
{
    return analyzer == 0 ? &_master : &_secondary;
}

bool LogicStackingMerger::ensure_channel_order(SourceBuffer &source)
{
    if (source.channels_ready)
        return true;

    GSList *channels = ds_get_device_channels_by_handle(source.handle);
    if (channels == NULL)
        return false;

    int order = 0;
    for (const GSList *l = channels; l; l = l->next){
        const sr_channel *probe = (const sr_channel*)l->data;
        if (probe->type != SR_CHANNEL_LOGIC || !probe->enabled)
            continue;

        if (probe->index < 64){
            source.order_by_phys[probe->index] = order++;
            source.enabled_phys.push_back(probe->index);
        }
    }

    source.channels_ready = source.enabled_phys.empty() == false;
    return source.channels_ready;
}

bool LogicStackingMerger::append_logic(ds_device_handle handle, const sr_datafeed_logic &logic)
{
    SourceBuffer *source = source_for_handle(handle);
    if (source == NULL)
        return false;

    if (logic.format != LA_CROSS_DATA || logic.data == NULL)
        return false;

    if (!ensure_channel_order(*source))
        return false;

    const uint8_t *begin = (const uint8_t*)logic.data;
    source->bytes.insert(source->bytes.end(), begin, begin + logic.length);
    return true;
}

void LogicStackingMerger::set_trigger(ds_device_handle handle, const ds_trigger_pos &trigger_pos)
{
    SourceBuffer *source = source_for_handle(handle);
    if (source == NULL)
        return;

    if (trigger_pos.status & 0x01){
        source->have_trigger = true;
        source->trigger_pos = trigger_pos.real_pos;
    }
}

bool LogicStackingMerger::mark_end(ds_device_handle handle)
{
    SourceBuffer *source = source_for_handle(handle);
    if (source == NULL)
        return false;

    source->ended = true;
    return true;
}

bool LogicStackingMerger::complete() const
{
    return _master.ended && _secondary.ended;
}

uint64_t LogicStackingMerger::master_trigger_pos() const
{
    return _master.have_trigger ? _master.trigger_pos : 0;
}

QString LogicStackingMerger::warning() const
{
    return _warning;
}

void LogicStackingMerger::append_warning(const QString &message)
{
    if (!_warning.isEmpty())
        _warning += "\n";
    _warning += message;
}

uint64_t LogicStackingMerger::source_sample_count(const SourceBuffer &source) const
{
    if (source.enabled_phys.empty())
        return 0;

    const uint64_t block_size = source.enabled_phys.size() * sizeof(uint64_t);
    if (block_size == 0)
        return 0;

    return (source.bytes.size() / block_size) * 64;
}

bool LogicStackingMerger::analyzer_visible(int analyzer,
                                           const std::vector<LogicStackingChannel> &visible_map) const
{
    for (size_t i = 0; i < visible_map.size(); i++){
        if (visible_map[i].analyzer == analyzer)
            return true;
    }

    return false;
}

void LogicStackingMerger::append_coverage_warnings(const std::vector<LogicStackingChannel> &visible_map)
{
    const uint64_t master_samples = source_sample_count(_master);
    const uint64_t secondary_samples = source_sample_count(_secondary);
    ensure_reference_correction();
    const int64_t secondary_shift = secondary_shift_samples();

    if (analyzer_visible(0, visible_map) && master_samples < _total_sample_count){
        append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_MASTER_INCOMPLETE), "Stacking master only provided %1 of %2 requested samples."))
                       .arg(master_samples)
                       .arg(_total_sample_count));
    }

    if (!analyzer_visible(1, visible_map))
        return;

    // Required secondary source range after alignment; out-of-range parts become zero-fill.
    int64_t first_required = -secondary_shift;
    int64_t last_required = (int64_t)_total_sample_count - 1 - secondary_shift;
    if (_total_sample_count == 0){
        first_required = 0;
        last_required = -1;
    }
    else if (_secondary_drift_applied){
        const long double source_a = secondary_source_for_dest(0);
        const long double source_b = secondary_source_for_dest(_total_sample_count - 1);
        first_required = (int64_t)floor(std::min(source_a, source_b));
        last_required = (int64_t)ceil(std::max(source_a, source_b));
    }

    if (first_required < 0 || last_required >= (int64_t)secondary_samples){
        append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_SECONDARY_INCOMPLETE),
                                   "Stacking secondary coverage is incomplete: shift=%1 samples, source=%2 samples, required=%3 / %4."))
                       .arg(secondary_shift)
                       .arg(secondary_samples)
                       .arg(first_required)
                       .arg(last_required));
    }
}

// apply manually set shift to the secondary analyzer's samples
int64_t LogicStackingMerger::manual_shift_samples() const
{
    if (_samplerate == 0 || _config.secondary_manual_shift_ps == 0)
        return 0;

    const long double scaled = (long double)_config.secondary_manual_shift_ps *
                               (long double)_samplerate / 1000000000000.0L;
    return (int64_t)llround(scaled);
}

// Base alignment: trigger positions plus manually entered shift
// Drift correction may replace this offset and change scale
int64_t LogicStackingMerger::secondary_shift_samples()
{
    if (_secondary_shift_ready)
        return _secondary_shift;

    _secondary_shift = manual_shift_samples();

    // instant capture has limited usability...
    if (_instant){
        if (!_config.reference_alignment_enabled)
            append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_INSTANT_MANUAL_SHIFT),
                               "Instant capture used manual alignment only. Trigger point alignment was skipped."));
    }
    // align captures to the corresponding reported trigger points from the analyzers
    else if (_master.have_trigger && _secondary.have_trigger){
        _secondary_shift += (int64_t)_master.trigger_pos - (int64_t)_secondary.trigger_pos;
    }
    else{
        if (!_config.reference_alignment_enabled)
            append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_SYNC_TRIGGER_MISSING),
                               "Sync trigger was not reported by both analyzers. Only manual alignment applied."));
    }

    _secondary_shift_ready = true;
    return _secondary_shift;
}

uint64_t LogicStackingMerger::read_word(const SourceBuffer &source,
                                        int physical_index,
                                        uint64_t block) const
{
    if (physical_index < 0 || physical_index >= 64)
        return 0;

    const int order = source.order_by_phys[physical_index];
    if (order < 0 || source.enabled_phys.empty())
        return 0;

    const uint64_t word_index = block * source.enabled_phys.size() + order;
    const uint64_t byte_index = word_index * sizeof(uint64_t);

    if (byte_index + sizeof(uint64_t) > source.bytes.size())
        return 0;

    uint64_t word = 0;
    memcpy(&word, &source.bytes[(size_t)byte_index], sizeof(word));
    return word;
}

uint64_t LogicStackingMerger::shifted_word(const SourceBuffer &source,
                                           int physical_index,
                                           int64_t source_start) const
{
    if (source_start <= -64)
        return 0;

    if (source_start < 0){
        const unsigned int left_shift = (unsigned int)-source_start;
        return read_word(source, physical_index, 0) << left_shift;
    }

    const uint64_t block = (uint64_t)source_start / 64;
    const unsigned int bit_shift = (unsigned int)((uint64_t)source_start & 63);
    const uint64_t low = read_word(source, physical_index, block);

    if (bit_shift == 0)
        return low;

    const uint64_t high = read_word(source, physical_index, block + 1);
    return (low >> bit_shift) | (high << (64 - bit_shift));
}

bool LogicStackingMerger::bit_at(const SourceBuffer &source,
                                 int physical_index,
                                 int64_t sample) const
{
    if (sample < 0)
        return false;

    const uint64_t block = (uint64_t)sample / 64;
    const unsigned int bit = (unsigned int)((uint64_t)sample & 63);
    return ((read_word(source, physical_index, block) >> bit) & 1) != 0;
}

void LogicStackingMerger::collect_reference_edges(const SourceBuffer &source,
                                                  int physical_index,
                                                  std::vector<int64_t> *rising,
                                                  std::vector<int64_t> *falling,
                                                  std::vector<int64_t> *gaps) const
{
    if (rising != NULL)
        rising->clear();
    if (falling != NULL)
        falling->clear();
    if (gaps != NULL)
        gaps->clear();

    const uint64_t samples = source_sample_count(source);
    if (samples < 2)
        return;

    bool previous = bit_at(source, physical_index, 0);
    const uint64_t blocks = (samples + 63) / 64;
    int64_t last_edge = -1;

    for (uint64_t block = 0; block < blocks; block++){
        const uint64_t word = read_word(source, physical_index, block);
        uint64_t previous_bits = (word << 1) | (previous ? 1ULL : 0ULL);
        uint64_t changed = word ^ previous_bits;

        if (block == 0)
            changed &= ~1ULL;

        while (changed != 0){
            const unsigned int bit = (unsigned int)__builtin_ctzll(changed);
            const uint64_t sample = block * 64 + bit;
            if (sample < samples){
                std::vector<int64_t> *edges = ((word >> bit) & 1) ? rising : falling;
                if (edges != NULL)
                    edges->push_back((int64_t)sample);
                if (gaps != NULL && last_edge >= 0)
                    gaps->push_back((int64_t)sample - last_edge);
                last_edge = (int64_t)sample;
            }
            changed &= changed - 1;
        }

        previous = ((word >> 63) & 1) != 0;
    }
}

int64_t LogicStackingMerger::reference_pair_window(std::vector<int64_t> &gaps) const
{
    if (gaps.empty())
        return std::max<int64_t>(16, (int64_t)(_total_sample_count / 1000));

    return std::max<int64_t>(16, median_in_place(gaps) / 4);
}

// Pair each master edge in [first, last] with the nearest unused secondary edge
// of the same polarity around the position the model predicts
void LogicStackingMerger::pair_reference_edges(const std::vector<int64_t> master_edges[2],
                                               const std::vector<int64_t> secondary_edges[2],
                                               const ReferenceFit &model,
                                               int64_t first,
                                               int64_t last,
                                               int64_t pair_window,
                                               std::vector<ReferencePair> &pairs) const
{
    for (int polarity = 0; polarity < 2; polarity++){
        const std::vector<int64_t> &master = master_edges[polarity];
        const std::vector<int64_t> &secondary = secondary_edges[polarity];
        std::vector<int64_t>::const_iterator next_secondary = secondary.begin();

        for (std::vector<int64_t>::const_iterator it = std::lower_bound(master.begin(), master.end(), first);
             it != master.end() && *it <= last; ++it){
            const long double predicted = ((long double)*it - model.offset) / model.scale;
            std::vector<int64_t>::const_iterator best =
                std::lower_bound(next_secondary, secondary.end(), predicted);

            if (best != next_secondary &&
                (best == secondary.end() ||
                 predicted - (long double)*(best - 1) < (long double)*best - predicted))
                best--;

            if (best == secondary.end())
                continue;

            if (std::fabs((long double)*best - predicted) <= (long double)pair_window){
                pairs.push_back(ReferencePair(*it, *best));
                next_secondary = best + 1;
            }
        }
    }
}

// A few wrong pairs (glitches, spurious matches) are trimmed so they cannot tilt
// the slope. The median residual over all pairs tells how consistent they are.
bool LogicStackingMerger::fit_reference_pairs(const std::vector<ReferencePair> &pairs,
                                              ReferenceFit &fit) const
{
    if (pairs.size() < 2)
        return false;

    std::vector<char> inlier(pairs.size(), 1);
    std::vector<double> residuals(pairs.size());
    std::vector<double> inlier_residuals;

    for (int round = 0; ; round++){
        if (!fit_affine(pairs, inlier, fit.scale, fit.offset))
            return false;

        inlier_residuals.clear();
        for (size_t i = 0; i < pairs.size(); i++){
            const long double expected = fit.scale * (long double)pairs[i].second + fit.offset;
            residuals[i] = std::fabs((double)((long double)pairs[i].first - expected));
            if (inlier[i])
                inlier_residuals.push_back(residuals[i]);
        }

        if (round == kMaxTrimRounds)
            break;

        const double limit = std::max(kMinOutlierLimit, 4.0 * median_in_place(inlier_residuals));
        bool changed = false;
        for (size_t i = 0; i < pairs.size(); i++){
            const char keep = residuals[i] <= limit ? 1 : 0;
            changed = changed || keep != inlier[i];
            inlier[i] = keep;
        }

        if (!changed)
            break;
    }

    int64_t first = std::numeric_limits<int64_t>::max();
    int64_t last = std::numeric_limits<int64_t>::min();
    std::vector<long double> offsets;
    fit.inlier_count = 0;
    fit.inlier_center = 0.0L;
    for (size_t i = 0; i < pairs.size(); i++){
        if (!inlier[i])
            continue;

        first = std::min(first, pairs[i].first);
        last = std::max(last, pairs[i].first);
        fit.inlier_center += (long double)pairs[i].first;
        offsets.push_back((long double)pairs[i].first - (long double)pairs[i].second);
        fit.inlier_count++;
    }

    fit.inlier_center /= (long double)fit.inlier_count;
    fit.inlier_spread = 0.0L;
    for (size_t i = 0; i < pairs.size(); i++){
        if (!inlier[i])
            continue;

        const long double delta = (long double)pairs[i].first - fit.inlier_center;
        fit.inlier_spread += delta * delta;
    }

    fit.inlier_span = last - first;
    fit.median_offset = median_in_place(offsets);
    fit.median_residual = median_in_place(residuals);
    return true;
}

// Few or clustered pairs give a poor slope, which grows into a large error
// where the fitted timing is extrapolated
double LogicStackingMerger::ReferenceFit::error_at(int64_t x) const
{
    if (inlier_count < 2 || inlier_spread <= 0.0L)
        return std::numeric_limits<double>::infinity();

    const double noise = std::max(kMinTimingNoise, 1.4826 * median_residual);
    const long double delta = (long double)x - inlier_center;
    return noise * std::sqrt((double)(1.0L / (long double)inlier_count + delta * delta / inlier_spread));
}

double LogicStackingMerger::ReferenceFit::slope_error_at(int64_t pivot, int64_t x) const
{
    if (inlier_count < 2 || inlier_spread <= 0.0L)
        return std::numeric_limits<double>::infinity();

    const double noise = std::max(kMinTimingNoise, 1.4826 * median_residual);
    return noise * std::fabs((double)(x - pivot)) / std::sqrt((double)inlier_spread);
}

// Update the tracking model from the pairs of one span. The slope is only taken
// over once the pairs cover enough of the span; a short baseline extrapolates badly.
void LogicStackingMerger::refine_reference_model(const std::vector<ReferencePair> &pairs,
                                                 int64_t span,
                                                 ReferenceFit &model) const
{
    if (pairs.empty())
        return;

    ReferenceFit candidate;
    if (pairs.size() >= kMinSlopePairs && fit_reference_pairs(pairs, candidate)){
        const long double ppm = (candidate.scale - 1.0L) * 1000000.0L;
        if (candidate.inlier_count >= kMinSlopePairs &&
            2 * candidate.inlier_span >= span &&
            candidate.median_residual <= kMaxMedianResidual &&
            std::fabs((double)ppm) <= kMaxDriftPpm){
            model = candidate;
            return;
        }
    }

    std::vector<long double> offsets;
    offsets.reserve(pairs.size());
    for (size_t i = 0; i < pairs.size(); i++)
        offsets.push_back((long double)pairs[i].first - model.scale * (long double)pairs[i].second);

    model.offset = median_in_place(offsets);
}

// Pair reference edges outwards from the trigger anchor, widening the span step by
// step and predicting edge positions with the model fitted so far. The drift may
// add up to many edge periods over the capture without breaking the pairing.
bool LogicStackingMerger::fit_reference_timing(int64_t alignment_shift, ReferenceFit &fit)
{
    fit = ReferenceFit();

    if (!LogicStackingConfig::channel_index_valid(_config.drift_master_channel) ||
        !LogicStackingConfig::channel_index_valid(_config.drift_secondary_channel))
        return false;

    const bool use_rising = _config.reference_edge_mode != LogicStackingReferenceFalling;
    const bool use_falling = _config.reference_edge_mode == LogicStackingReferenceFalling ||
                             _config.reference_edge_mode == LogicStackingReferenceBoth;

    // [0] rising, [1] falling
    std::vector<int64_t> master_edges[2];
    std::vector<int64_t> secondary_edges[2];
    std::vector<int64_t> gaps;
    collect_reference_edges(_master, _config.drift_master_channel,
                            use_rising ? &master_edges[0] : NULL,
                            use_falling ? &master_edges[1] : NULL,
                            &gaps);
    collect_reference_edges(_secondary, _config.drift_secondary_channel,
                            use_rising ? &secondary_edges[0] : NULL,
                            use_falling ? &secondary_edges[1] : NULL,
                            NULL);

    if (master_edges[0].size() + master_edges[1].size() < 2 ||
        secondary_edges[0].size() + secondary_edges[1].size() < 2)
        return false;

    const int64_t pair_window = reference_pair_window(gaps);
    std::vector<int64_t>().swap(gaps);

    // A subset pins down offset and drift just as well; thin out dense references
    for (int polarity = 0; polarity < 2; polarity++){
        std::vector<int64_t> &edges = master_edges[polarity];
        const size_t stride = (edges.size() + kMaxReferenceEdges - 1) / kMaxReferenceEdges;
        if (stride <= 1)
            continue;

        size_t kept = 0;
        for (size_t i = 0; i < edges.size(); i += stride)
            edges[kept++] = edges[i];
        edges.resize(kept);
    }

    int64_t first_edge = std::numeric_limits<int64_t>::max();
    int64_t last_edge = std::numeric_limits<int64_t>::min();
    for (int polarity = 0; polarity < 2; polarity++){
        if (master_edges[polarity].empty())
            continue;

        first_edge = std::min(first_edge, master_edges[polarity].front());
        last_edge = std::max(last_edge, master_edges[polarity].back());
    }

    // Start where even the largest accepted drift stays within half the pair window
    const int64_t anchor = reference_anchor();
    int64_t half_span = std::max<int64_t>(pair_window,
        (int64_t)((long double)pair_window * 1000000.0L / (2.0L * kMaxDriftPpm)));

    ReferenceFit model;
    model.offset = (long double)alignment_shift;
    std::vector<ReferencePair> pairs;

    for (;;){
        const int64_t first = anchor - half_span;
        const int64_t last = anchor + half_span;
        pairs.clear();
        pair_reference_edges(master_edges, secondary_edges, model, first, last, pair_window, pairs);

        if (first <= first_edge && last >= last_edge)
            break;

        refine_reference_model(pairs, std::min(last, last_edge) - std::max(first, first_edge), model);
        half_span *= 2;
    }

    return fit_reference_pairs(pairs, fit);
}

// Constant secondary offset: the fitted reference timing at the trigger anchor,
// or the pairs' median offset when the fit cannot be extrapolated that far
void LogicStackingMerger::apply_reference_alignment(const ReferenceFit &fit)
{
    const int64_t anchor = reference_anchor();
    long double reference_shift = fit.median_offset;
    if (fit.error_at(anchor) <= kMaxTimingError)
        reference_shift = (long double)anchor - ((long double)anchor - fit.offset) / fit.scale;

    _secondary_shift = (int64_t)llround(reference_shift) + manual_shift_samples();
    _secondary_shift_ready = true;

    const double shift_ns = _samplerate == 0 ? 0.0 :
        (double)_secondary_shift * 1000000000.0 / (double)_samplerate;
    append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_ALIGNMENT_APPLIED),
                               "Stacking reference alignment applied: secondary offset %1 samples (%2 ns) from %3 reference edges."))
                   .arg(_secondary_shift)
                   .arg(shift_ns, 0, 'f', 3)
                   .arg((qulonglong)fit.inlier_count));
}

// Master sample where the trigger alignment holds; reference tracking starts there
int64_t LogicStackingMerger::reference_anchor() const
{
    if (!_instant && _master.have_trigger && _secondary.have_trigger)
        return (int64_t)_master.trigger_pos;

    return (int64_t)(_total_sample_count / 2);
}

void LogicStackingMerger::update_secondary_anchor()
{
    const int64_t manual_shift = manual_shift_samples();
    _secondary_anchor_dest = (int64_t)(_total_sample_count / 2);
    if (!_instant && _master.have_trigger && _secondary.have_trigger)
        _secondary_anchor_dest = (int64_t)_master.trigger_pos + manual_shift;

    _secondary_anchor_source = (long double)_secondary_anchor_dest -
                               (long double)secondary_shift_samples();
}

void LogicStackingMerger::ensure_reference_correction()
{
    if (_reference_correction_ready)
        return;

    _reference_correction_ready = true;
    _secondary_drift_applied = false;
    _secondary_scale = 1.0L;

    const int64_t base_shift = secondary_shift_samples();
    const int64_t manual_shift = manual_shift_samples();
    update_secondary_anchor();

    if (!_config.reference_alignment_enabled && !_config.drift_correction_enabled)
        return;

    ReferenceFit fit;
    if (!fit_reference_timing(base_shift - manual_shift, fit)){
        if (_config.reference_alignment_enabled)
            append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_ALIGNMENT_FAILED),
                               "Stacking reference alignment skipped: not enough matching reference edges on the selected channels."));
        if (_config.drift_correction_enabled)
            append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_FAILED),
                               "Stacking drift correction skipped: not enough matching reference edges on the selected channels."));
        return;
    }

    // Pairs that do not follow one timing line mean the edges were matched wrongly
    if (fit.median_residual > kMaxMedianResidual){
        if (_config.reference_alignment_enabled)
            append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_ALIGNMENT_REJECTED),
                                       "Stacking reference alignment skipped: reference edge timing is inconsistent (typical deviation %1 samples)."))
                           .arg(fit.median_residual, 0, 'f', 1));
        if (_config.drift_correction_enabled)
            append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_INCONSISTENT),
                                       "Stacking drift correction skipped: reference edge timing is inconsistent (typical deviation %1 samples)."))
                           .arg(fit.median_residual, 0, 'f', 1));
        return;
    }

    if (_config.reference_alignment_enabled){
        apply_reference_alignment(fit);
        update_secondary_anchor();
    }

    if (!_config.drift_correction_enabled)
        return;

    const int64_t last_sample = (int64_t)_total_sample_count - 1;
    const double drift_error = _config.reference_alignment_enabled ?
        std::max(fit.error_at(0), fit.error_at(last_sample)) :
        std::max(fit.slope_error_at(reference_anchor(), 0),
                 fit.slope_error_at(reference_anchor(), last_sample));
    if (drift_error > kMaxTimingError){
        append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_FAILED),
                           "Stacking drift correction skipped: not enough matching reference edges on the selected channels."));
        return;
    }

    const long double ppm = (fit.scale - 1.0L) * 1000000.0L;
    if (fit.scale <= 0.0L || std::fabs((double)ppm) > kMaxDriftPpm){
        append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_REJECTED),
                                   "Stacking drift correction skipped: estimated drift %1 ppm is outside the expected range."))
                       .arg((double)ppm, 0, 'f', 3));
        return;
    }

    _secondary_scale = fit.scale;
    if (_config.reference_alignment_enabled){
        // Alignment+drift means the reference signal determines the whole A2 time mapping
        _secondary_anchor_dest = 0;
        _secondary_anchor_source = -((long double)manual_shift + fit.offset) / fit.scale;
    }
    // Drift-only correction keeps the trigger+manual shift at the anchor and only
    // takes the slope; the fit's offset is free, so a reference-signal offset
    // cannot contribute to false drift correction

    _secondary_drift_applied = true;
    append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_APPLIED),
                               "Stacking drift correction applied: secondary drift %1 ppm from %2 reference edges."))
                   .arg((double)ppm, 0, 'f', 3)
                   .arg((qulonglong)fit.inlier_count));
}

long double LogicStackingMerger::secondary_source_for_dest(uint64_t dest_sample) const
{
    return _secondary_anchor_source +
           ((long double)dest_sample - (long double)_secondary_anchor_dest) /
           _secondary_scale;
}

int64_t LogicStackingMerger::rounded_secondary_source(uint64_t dest_sample) const
{
    return (int64_t)llround(secondary_source_for_dest(dest_sample));
}

uint64_t LogicStackingMerger::scaled_secondary_word(const SourceBuffer &source,
                                                   int physical_index,
                                                   uint64_t dest_sample) const
{
    const int64_t first = rounded_secondary_source(dest_sample);
    const int64_t last = rounded_secondary_source(dest_sample + 63);

    if (last - first == 63)
        return shifted_word(source, physical_index, first);

    uint64_t word = 0;
    for (unsigned int bit = 0; bit < 64; bit++){
        if (bit_at(source, physical_index, rounded_secondary_source(dest_sample + bit)))
            word |= 1ULL << bit;
    }

    return word;
}

bool LogicStackingMerger::build_chunk(uint64_t start_sample,
                                      uint64_t sample_count,
                                      const std::vector<LogicStackingChannel> &visible_map,
                                      std::vector<uint64_t> &words)
{
    if (visible_map.empty())
        return false;

    const uint64_t blocks = (sample_count + 63) / 64;
    words.assign((size_t)(blocks * visible_map.size()), 0);
    ensure_reference_correction();
    const int64_t secondary_shift = secondary_shift_samples();

    // LogicSnapshot stores data in 64-sample words, one word per visible channel
    for (uint64_t block = 0; block < blocks; block++){
        for (size_t ch = 0; ch < visible_map.size(); ch++){
            const LogicStackingChannel &mapped = visible_map[ch];
            const SourceBuffer *source = source_for_analyzer(mapped.analyzer);
            const int64_t source_shift = mapped.analyzer == 0 ? 0 : secondary_shift;
            const uint64_t dest_sample = start_sample + block * 64;
            uint64_t word = 0;
            if (mapped.analyzer == 1 && _secondary_drift_applied){
                word = scaled_secondary_word(*source, mapped.physical_index, dest_sample);
            }
            else{
                word = shifted_word(*source,
                                    mapped.physical_index,
                                    (int64_t)dest_sample - source_shift);
            }

            const uint64_t remaining = _total_sample_count - dest_sample;
            if (remaining < 64){
                const uint64_t mask = remaining == 0 ? 0 : ((1ULL << remaining) - 1);
                word &= mask;
            }

            words[(size_t)(block * visible_map.size() + ch)] = word;
        }
    }

    return true;
}

// build logic snapshot from aligned signals for waveform display
bool LogicStackingMerger::flush_to_snapshot(LogicSnapshot *snapshot,
                                            GSList *visible_channels,
                                            const std::vector<LogicStackingChannel> &visible_map)
{
    if (snapshot == NULL || visible_channels == NULL || visible_map.empty())
        return false;

    if (!_config.is_valid() || _total_sample_count == 0)
        return false;

    if (!ensure_channel_order(_master) || !ensure_channel_order(_secondary))
        return false;

    append_coverage_warnings(visible_map);

    const uint64_t chunk_samples = 4096;
    bool first = true;

    for (uint64_t sample = 0; sample < _total_sample_count; sample += chunk_samples){
        const uint64_t count = std::min(chunk_samples, _total_sample_count - sample);
        std::vector<uint64_t> words;
        if (!build_chunk(sample, count, visible_map, words))
            return false;

        sr_datafeed_logic logic;
        memset(&logic, 0, sizeof(logic));
        logic.length = words.size() * sizeof(uint64_t);
        logic.format = LA_CROSS_DATA;
        logic.unitsize = 1;
        logic.data = words.data();

        if (first){
            snapshot->first_payload(logic, _total_sample_count, visible_channels, true);
            first = false;
        }
        else{
            snapshot->append_payload(logic);
        }

        if (snapshot->memory_failed())
            return false;
    }

    if (!_warning.isEmpty())
        dsv_info("%s", _warning.toLocal8Bit().constData());

    return true;
}

}
}
