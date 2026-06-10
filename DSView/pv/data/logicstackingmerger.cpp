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
#include <string.h>

namespace pv {
namespace data {

namespace {

int64_t median_int64(std::vector<int64_t> values)
{
    if (values.empty())
        return 0;

    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
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
                               "Instant capture used manual shift only. Trigger point alignment was skipped."));
    }
    // align captures to the corresponding reported trigger points from the analyzers
    else if (_master.have_trigger && _secondary.have_trigger){
        _secondary_shift += (int64_t)_master.trigger_pos - (int64_t)_secondary.trigger_pos;
    }
    else{
        if (!_config.reference_alignment_enabled)
            append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_SYNC_TRIGGER_MISSING),
                               "Sync trigger was not reported by both analyzers. Only manual shift applied."));
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
                                                  std::vector<ReferenceEdge> &edges) const
{
    edges.clear();

    const uint64_t samples = source_sample_count(source);
    if (samples < 2)
        return;

    bool previous = bit_at(source, physical_index, 0);
    const uint64_t blocks = (samples + 63) / 64;

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
                ReferenceEdge edge;
                edge.sample = (int64_t)sample;
                edge.rising = ((word >> bit) & 1) != 0;
                edges.push_back(edge);
            }
            changed &= changed - 1;
        }

        previous = ((word >> 63) & 1) != 0;
    }
}

int64_t LogicStackingMerger::reference_pair_window(const std::vector<ReferenceEdge> &edges) const
{
    std::vector<int64_t> gaps;
    for (size_t i = 1; i < edges.size(); i++){
        const int64_t gap = edges[i].sample - edges[i - 1].sample;
        if (gap > 0)
            gaps.push_back(gap);
    }

    if (gaps.empty())
        return std::max<int64_t>(16, (int64_t)(_total_sample_count / 1000));

    std::sort(gaps.begin(), gaps.end());
    const int64_t median_gap = gaps[gaps.size() / 2];
    return std::max<int64_t>(16, median_gap / 4);
}

void LogicStackingMerger::add_reference_pairs(bool rising,
                                              const std::vector<ReferenceEdge> &master_edges,
                                              const std::vector<ReferenceEdge> &secondary_edges,
                                              int64_t alignment_shift,
                                              int64_t pair_window,
                                              std::vector<std::pair<int64_t, int64_t> > &pairs) const
{
    std::vector<int64_t> master_samples;
    std::vector<int64_t> secondary_samples;

    for (size_t i = 0; i < master_edges.size(); i++){
        if (master_edges[i].rising == rising)
            master_samples.push_back(master_edges[i].sample);
    }

    for (size_t i = 0; i < secondary_edges.size(); i++){
        if (secondary_edges[i].rising == rising)
            secondary_samples.push_back(secondary_edges[i].sample);
    }

    size_t secondary_index = 0;
    for (size_t i = 0; i < master_samples.size() && secondary_index < secondary_samples.size(); i++){
        const int64_t master_sample = master_samples[i];

        while (secondary_index < secondary_samples.size() &&
               secondary_samples[secondary_index] + alignment_shift < master_sample - pair_window){
            secondary_index++;
        }

        if (secondary_index >= secondary_samples.size())
            break;

        size_t best = secondary_index;
        int64_t best_error = llabs(secondary_samples[best] + alignment_shift - master_sample);

        if (secondary_index + 1 < secondary_samples.size()){
            const int64_t next_error =
                llabs(secondary_samples[secondary_index + 1] + alignment_shift - master_sample);
            if (next_error < best_error){
                best = secondary_index + 1;
                best_error = next_error;
            }
        }

        if (best_error <= pair_window){
            pairs.push_back(std::make_pair(master_sample, secondary_samples[best]));
            secondary_index = best + 1;
        }
    }
}

bool LogicStackingMerger::collect_reference_pairs(
        int64_t alignment_shift,
        std::vector<std::pair<int64_t, int64_t> > &pairs,
        int64_t &pair_window)
{
    pairs.clear();
    pair_window = 0;

    if (!LogicStackingConfig::channel_index_valid(_config.drift_master_channel) ||
        !LogicStackingConfig::channel_index_valid(_config.drift_secondary_channel))
        return false;

    std::vector<ReferenceEdge> master_edges;
    std::vector<ReferenceEdge> secondary_edges;
    collect_reference_edges(_master, _config.drift_master_channel, master_edges);
    collect_reference_edges(_secondary, _config.drift_secondary_channel, secondary_edges);

    if (master_edges.size() < 2 || secondary_edges.size() < 2)
        return false;

    pair_window = reference_pair_window(master_edges);
    switch (_config.reference_edge_mode){
    case LogicStackingReferenceFalling:
        add_reference_pairs(false, master_edges, secondary_edges, alignment_shift, pair_window, pairs);
        break;
    case LogicStackingReferenceBoth:
        add_reference_pairs(true, master_edges, secondary_edges, alignment_shift, pair_window, pairs);
        add_reference_pairs(false, master_edges, secondary_edges, alignment_shift, pair_window, pairs);
        break;
    case LogicStackingReferenceRising:
    default:
        add_reference_pairs(true, master_edges, secondary_edges, alignment_shift, pair_window, pairs);
        break;
    }

    return pairs.size() >= 2;
}

bool LogicStackingMerger::apply_reference_alignment(
        const std::vector<std::pair<int64_t, int64_t> > &pairs,
        int64_t pair_window)
{
    if (pairs.size() < 2)
        return false;

    std::vector<int64_t> offsets;
    for (size_t i = 0; i < pairs.size(); i++)
        offsets.push_back(pairs[i].first - pairs[i].second);

    const int64_t reference_shift = median_int64(offsets);

    std::vector<int64_t> residuals;
    for (size_t i = 0; i < offsets.size(); i++)
        residuals.push_back(llabs(offsets[i] - reference_shift));

    const int64_t median_residual = median_int64(residuals);
    const int64_t max_residual = std::max<int64_t>(16, pair_window / 2);
    if (median_residual > max_residual){
        append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_ALIGNMENT_REJECTED),
                                   "Stacking reference alignment skipped: reference edge offsets vary by %1 samples."))
                       .arg(median_residual));
        return false;
    }

    _secondary_shift = reference_shift + manual_shift_samples();
    _secondary_shift_ready = true;

    const double shift_ns = _samplerate == 0 ? 0.0 :
        (double)_secondary_shift * 1000000000.0 / (double)_samplerate;
    append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_ALIGNMENT_APPLIED),
                               "Stacking reference alignment applied: secondary offset %1 samples (%2 ns) from %3 reference edges."))
                   .arg(_secondary_shift)
                   .arg(shift_ns, 0, 'f', 3)
                   .arg((qulonglong)pairs.size()));

    return true;
}

bool LogicStackingMerger::fit_reference_affine(
        const std::vector<std::pair<int64_t, int64_t> > &pairs,
        long double &scale,
        long double &offset) const
{
    if (pairs.size() < 2)
        return false;

    long double mean_secondary = 0.0L;
    long double mean_master = 0.0L;
    for (size_t i = 0; i < pairs.size(); i++){
        mean_master += (long double)pairs[i].first;
        mean_secondary += (long double)pairs[i].second;
    }

    mean_master /= (long double)pairs.size();
    mean_secondary /= (long double)pairs.size();

    long double numerator = 0.0L;
    long double denominator = 0.0L;
    for (size_t i = 0; i < pairs.size(); i++){
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

    std::vector<std::pair<int64_t, int64_t> > pairs;
    int64_t pair_window = 0;
    if (!collect_reference_pairs(base_shift - manual_shift, pairs, pair_window)){
        if (_config.reference_alignment_enabled)
            append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_ALIGNMENT_FAILED),
                               "Stacking reference alignment skipped: not enough matching reference edges on the selected channels."));
        if (_config.drift_correction_enabled)
            append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_FAILED),
                               "Stacking drift correction skipped: not enough matching reference edges on the selected channels."));
        return;
    }

    if (_config.reference_alignment_enabled){
        if (!apply_reference_alignment(pairs, pair_window)){
            if (_config.drift_correction_enabled)
                append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_FAILED),
                                   "Stacking drift correction skipped: not enough matching reference edges on the selected channels."));
            return;
        }

        update_secondary_anchor();
    }

    if (!_config.drift_correction_enabled)
        return;

    if (_config.reference_alignment_enabled){
        // Alignment+drift means the reference signal determines the whole A2 time mapping
        long double scale = 1.0L;
        long double offset = 0.0L;
        if (!fit_reference_affine(pairs, scale, offset)){
            append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_FAILED),
                               "Stacking drift correction skipped: not enough matching reference edges on the selected channels."));
            return;
        }

        const long double ppm = (scale - 1.0L) * 1000000.0L;
        if (scale <= 0.0L || std::fabs((double)ppm) > 1000.0){
            append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_REJECTED),
                                       "Stacking drift correction skipped: estimated drift %1 ppm is outside the expected range."))
                           .arg((double)ppm, 0, 'f', 3));
            return;
        }

        _secondary_scale = scale;
        _secondary_anchor_dest = 0;
        _secondary_anchor_source = -((long double)manual_shift + offset) / scale;
        _secondary_drift_applied = true;
        append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_APPLIED),
                                   "Stacking drift correction applied: secondary drift %1 ppm from %2 reference edges."))
                       .arg((double)ppm, 0, 'f', 3)
                       .arg((qulonglong)pairs.size()));
        return;
    }

    // Drift-only correction keeps the trigger+manual shift but the slope
    // estimation must allow an arbitrary reference-signal offset, otherwise it
    // may contribute to false drift correction
    long double scale = 1.0L;
    long double offset = 0.0L;
    if (!fit_reference_affine(pairs, scale, offset)){
        append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_FAILED),
                           "Stacking drift correction skipped: not enough matching reference edges on the selected channels."));
        return;
    }

    const long double ppm = (scale - 1.0L) * 1000000.0L;
    if (scale <= 0.0L || std::fabs((double)ppm) > 1000.0){
        append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_REJECTED),
                                   "Stacking drift correction skipped: estimated drift %1 ppm is outside the expected range."))
                       .arg((double)ppm, 0, 'f', 3));
        return;
    }

    _secondary_scale = scale;
    _secondary_drift_applied = true;
    append_warning(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_DRIFT_APPLIED),
                               "Stacking drift correction applied: secondary drift %1 ppm from %2 reference edges."))
                   .arg((double)ppm, 0, 'f', 3)
                   .arg((qulonglong)pairs.size()));
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
