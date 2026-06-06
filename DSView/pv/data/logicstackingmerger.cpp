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

#include "logicstackingmerger.h"
#include "logicsnapshot.h"
#include "../log.h"
#include "../ui/langresource.h"

#include <algorithm>
#include <cmath>
#include <string.h>

namespace pv {
namespace data {

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

// align captures at their trigger points, corrected by manually specified shift.
// TODO: use a common signal supplied to all analyzers to perform auto alignment (offset + drift)
int64_t LogicStackingMerger::secondary_shift_samples()
{
    if (_secondary_shift_ready)
        return _secondary_shift;

    _secondary_shift = manual_shift_samples();

    // Trigger positions are ring-buffer positions reported by each analyzer after capture.
    if (_instant){
        append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_INSTANT_MANUAL_SHIFT),
                           "Instant capture used manual shift only. Trigger point alignment was skipped."));
    }
    else if (_master.have_trigger && _secondary.have_trigger){
        _secondary_shift += (int64_t)_master.trigger_pos - (int64_t)_secondary.trigger_pos;
    }
    else{
        append_warning(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_SYNC_TRIGGER_MISSING),
                           "Sync trigger was not reported by both analyzers. Only manual shift was applied."));
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

bool LogicStackingMerger::build_chunk(uint64_t start_sample,
                                      uint64_t sample_count,
                                      const std::vector<LogicStackingChannel> &visible_map,
                                      std::vector<uint64_t> &words)
{
    if (visible_map.empty())
        return false;

    const uint64_t blocks = (sample_count + 63) / 64;
    words.assign((size_t)(blocks * visible_map.size()), 0);
    const int64_t secondary_shift = secondary_shift_samples();

    // LogicSnapshot stores data in 64-sample words, one word per visible channel
    for (uint64_t block = 0; block < blocks; block++){
        for (size_t ch = 0; ch < visible_map.size(); ch++){
            const LogicStackingChannel &mapped = visible_map[ch];
            const SourceBuffer *source = source_for_analyzer(mapped.analyzer);
            const int64_t source_shift = mapped.analyzer == 0 ? 0 : secondary_shift;
            const uint64_t dest_sample = start_sample + block * 64;
            uint64_t word = shifted_word(*source,
                                         mapped.physical_index,
                                         (int64_t)dest_sample - source_shift);

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
