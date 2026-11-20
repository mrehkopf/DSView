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

#ifndef DSVIEW_PV_LOGICSTACKINGCONFIG_H
#define DSVIEW_PV_LOGICSTACKINGCONFIG_H

#include <stdint.h>
#include <vector>
#include <QString>
#include <libsigrok.h>

namespace pv {

// Channels each stacked analyzer contributes; analyzer k owns global channels
// k * 32 .. k * 32 + 31
const int LogicStackingChannelsPerAnalyzer = 32;
// Master plus secondaries. LogicSnapshot's CHANNEL_MAX_COUNT must hold
// LogicStackingMaxAnalyzers * LogicStackingChannelsPerAnalyzer channels.
const int LogicStackingMaxAnalyzers = 8;

enum LogicStackingReferenceEdgeMode
{
    LogicStackingReferenceRising = 0,
    LogicStackingReferenceFalling = 1,
    LogicStackingReferenceBoth = 2
};

// A secondary analyzer, armed on a rising edge at its sync input
struct LogicStackingSecondary
{
    ds_device_handle handle = NULL_HANDLE;
    int sync_channel = 0;
    int64_t manual_shift_ps = 0;
    int reference_channel = 0;

    bool operator==(const LogicStackingSecondary &other) const
    {
        return handle == other.handle &&
               sync_channel == other.sync_channel &&
               manual_shift_ps == other.manual_shift_ps &&
               reference_channel == other.reference_channel;
    }
};

struct LogicStackingConfig
{
    bool enabled = false;
    ds_device_handle master_handle = NULL_HANDLE;
    int master_reference_channel = 0;
    // Analyzers A2, A3, ... in channel order
    std::vector<LogicStackingSecondary> secondaries;
    bool show_sync_channel = false;
    bool reference_alignment_enabled = false;
    bool drift_correction_enabled = false;
    int reference_edge_mode = LogicStackingReferenceRising;

    int analyzer_count() const
    {
        return 1 + (int)secondaries.size();
    }

    bool uses_reference() const
    {
        return reference_alignment_enabled || drift_correction_enabled;
    }

    // The virtual channel model can be restored before physical devices are remapped.
    bool has_channel_model() const
    {
        if (!enabled || !analyzer_count_valid(analyzer_count()))
            return false;

        if (uses_reference() &&
            (!channel_index_valid(master_reference_channel) ||
             !reference_edge_mode_valid(reference_edge_mode)))
            return false;

        for (const LogicStackingSecondary &secondary : secondaries){
            if (!channel_index_valid(secondary.sync_channel))
                return false;
            if (uses_reference() && !channel_index_valid(secondary.reference_channel))
                return false;
        }

        return true;
    }

    bool is_valid() const
    {
        if (!has_channel_model() || master_handle == NULL_HANDLE)
            return false;

        for (size_t i = 0; i < secondaries.size(); i++){
            const ds_device_handle handle = secondaries[i].handle;
            if (handle == NULL_HANDLE || handle == master_handle)
                return false;

            for (size_t j = 0; j < i; j++){
                if (secondaries[j].handle == handle)
                    return false;
            }
        }

        return true;
    }

    bool has_same_channel_model(const LogicStackingConfig &other) const
    {
        // Handles are excluded deliberately; remapping analyzers must not reset traces or decoders.
        // Without stacking there are no synthetic channels, whatever the saved setup.
        if (!enabled && !other.enabled)
            return true;

        if (enabled != other.enabled ||
            show_sync_channel != other.show_sync_channel ||
            secondaries.size() != other.secondaries.size())
            return false;

        for (size_t i = 0; i < secondaries.size(); i++){
            if (secondaries[i].sync_channel != other.secondaries[i].sync_channel)
                return false;
        }

        return true;
    }

    // Analyzer number of a device: 0 for the master, -1 if it isn't stacked
    int handle_analyzer(ds_device_handle handle) const
    {
        if (handle == NULL_HANDLE)
            return -1;
        if (handle == master_handle)
            return 0;

        for (size_t i = 0; i < secondaries.size(); i++){
            if (secondaries[i].handle == handle)
                return (int)i + 1;
        }

        return -1;
    }

    static int global_channel_index(int analyzer, int physical_index)
    {
        return analyzer * LogicStackingChannelsPerAnalyzer + physical_index;
    }

    // "A1" for the master, "A2" for the first secondary, ...
    static QString analyzer_label(int analyzer)
    {
        return QString("A%1").arg(analyzer + 1);
    }

    static bool analyzer_count_valid(int count)
    {
        return count >= 2 && count <= LogicStackingMaxAnalyzers;
    }

    static bool channel_index_valid(int index)
    {
        return index >= 0 && index < LogicStackingChannelsPerAnalyzer;
    }

    static bool reference_edge_mode_valid(int mode)
    {
        return mode == LogicStackingReferenceRising ||
               mode == LogicStackingReferenceFalling ||
               mode == LogicStackingReferenceBoth;
    }
};

struct LogicStackingAnalyzerInfo
{
    ds_device_handle handle = NULL_HANDLE;
    QString name;
    QString unique_id;

    QString display_name() const
    {
        if (unique_id.trimmed().isEmpty())
            return name;

        return QString("%1 [%2]").arg(name, unique_id.trimmed());
    }
};

struct LogicStackingChannel
{
    int analyzer = 0;
    int physical_index = 0;
    int global_index = 0;
    bool visible = true;
    bool sync = false;
};

}

#endif
