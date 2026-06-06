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
    struct SourceBuffer
    {
        ds_device_handle handle = NULL_HANDLE;
        std::vector<uint8_t> bytes;
        std::vector<int> enabled_phys;
        int order_by_phys[64];
        bool channels_ready = false;
        bool ended = false;
        bool have_trigger = false;
        uint64_t trigger_pos = 0;

        void reset(ds_device_handle h);
    };

    SourceBuffer* source_for_handle(ds_device_handle handle);
    const SourceBuffer* source_for_analyzer(int analyzer) const;
    bool ensure_channel_order(SourceBuffer &source);
    uint64_t read_word(const SourceBuffer &source, int physical_index, uint64_t block) const;
    uint64_t shifted_word(const SourceBuffer &source, int physical_index, int64_t source_start) const;
    uint64_t source_sample_count(const SourceBuffer &source) const;
    bool analyzer_visible(int analyzer,
                          const std::vector<LogicStackingChannel> &visible_map) const;
    void append_warning(const QString &message);
    void append_coverage_warnings(const std::vector<LogicStackingChannel> &visible_map);
    int64_t manual_shift_samples() const;
    int64_t secondary_shift_samples();
    bool build_chunk(uint64_t start_sample,
                     uint64_t sample_count,
                     const std::vector<LogicStackingChannel> &visible_map,
                     std::vector<uint64_t> &words);

private:
    LogicStackingConfig _config;
    SourceBuffer _master;
    SourceBuffer _secondary;
    uint64_t _total_sample_count;
    uint64_t _samplerate;
    bool _instant;
    bool _secondary_shift_ready;
    int64_t _secondary_shift;
    QString _warning;
};

}
}

#endif
