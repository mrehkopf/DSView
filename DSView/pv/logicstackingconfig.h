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
// Inputs selected on an analyzer, bit n for Dn
const uint32_t LogicStackingAllChannels = 0xFFFFFFFFu;

// The U3Pro32 buffer modes stacking can use: fewer channels allow higher
// sample rates and more depth per channel
struct LogicStackingChannelMode
{
    int channel_count;
    int mode_id;
    // The driver's name, translated through the device option strings
    const char *name;
};

static const LogicStackingChannelMode LogicStackingChannelModes[] = {
    {32, DSL_BUFFER250x32, "Use Channels 0~31 (Max 250MHz)"},
    {16, DSL_BUFFER500x16, "Use Channels 0~15 (Max 500MHz)"},
    {8, DSL_BUFFER1000x8, "Use Channels 0~7 (Max 1GHz)"},
};

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
    // Input threshold in V and SR_FILTER_* filter; negative uses the
    // master's, as sessions from before per-analyzer settings do
    double threshold = -1.0;
    int filter = -1;
    // Inputs to capture and show; the sync input is captured regardless
    uint32_t channels = LogicStackingAllChannels;
    // Device name and unique id (the USB port), kept while the analyzer is
    // unplugged so it can be found again; see LogicStackingConfig::relink()
    QString name;
    QString unique_id;

    bool operator==(const LogicStackingSecondary &other) const
    {
        return handle == other.handle &&
               sync_channel == other.sync_channel &&
               manual_shift_ps == other.manual_shift_ps &&
               reference_channel == other.reference_channel &&
               threshold == other.threshold &&
               filter == other.filter &&
               channels == other.channels &&
               name == other.name &&
               unique_id == other.unique_id;
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
    // Channels every analyzer uses, one of LogicStackingChannelModes; the
    // global channel numbering stays 32 per analyzer whatever this is
    int channel_count = LogicStackingChannelsPerAnalyzer;
    // The master's selected inputs
    uint32_t master_channels = LogicStackingAllChannels;
    // The master's device name and unique id, as for a secondary
    QString master_name;
    QString master_unique_id;

    int analyzer_count() const
    {
        return 1 + (int)secondaries.size();
    }

    ds_device_handle analyzer_handle(int analyzer) const
    {
        return analyzer == 0 ? master_handle : secondaries[analyzer - 1].handle;
    }

    const QString &analyzer_name(int analyzer) const
    {
        return analyzer == 0 ? master_name : secondaries[analyzer - 1].name;
    }

    const QString &analyzer_unique_id(int analyzer) const
    {
        return analyzer == 0 ? master_unique_id : secondaries[analyzer - 1].unique_id;
    }

    // Whether a device unique id names the USB port, "USB 8-3.2.1". Where the
    // port is unknown it is the bus and address, "USB 8.61", which change
    // whenever the device is plugged in again, as in sessions of older builds.
    static bool is_port_id(const QString &unique_id)
    {
        return unique_id.startsWith("USB ") && unique_id.contains('-');
    }

    // Takes the name and unique id of each analyzer whose device is in the
    // list. An analyzer whose device isn't (unplugged, or not found when a
    // session was loaded) gets the device now plugged into its USB port, if
    // no other analyzer has it. Returns whether a handle changed.
    bool relink(const ds_device_base_info *devices, int count)
    {
        const int analyzers = analyzer_count();
        std::vector<bool> listed(analyzers, false);
        std::vector<bool> claimed(count > 0 ? count : 0, false);
        bool changed = false;

        for (int analyzer = 0; analyzer < analyzers; analyzer++){
            const ds_device_handle handle = analyzer_handle(analyzer);
            for (int i = 0; i < count && handle != NULL_HANDLE; i++){
                if (claimed[i] || devices[i].handle != handle)
                    continue;

                claimed[i] = true;
                listed[analyzer] = true;
                set_analyzer_device(analyzer, devices[i]);
                break;
            }
        }

        for (int analyzer = 0; analyzer < analyzers; analyzer++){
            if (listed[analyzer] || !is_port_id(analyzer_unique_id(analyzer)))
                continue;

            for (int i = 0; i < count; i++){
                if (claimed[i] ||
                    QString::fromLocal8Bit(devices[i].unique_id) != analyzer_unique_id(analyzer))
                    continue;

                claimed[i] = true;
                changed = changed || devices[i].handle != analyzer_handle(analyzer);
                set_analyzer_device(analyzer, devices[i]);
                break;
            }
        }

        return changed;
    }

    void set_analyzer_device(int analyzer, const ds_device_base_info &device)
    {
        ds_device_handle &handle = analyzer == 0 ? master_handle : secondaries[analyzer - 1].handle;
        QString &name = analyzer == 0 ? master_name : secondaries[analyzer - 1].name;
        QString &unique_id = analyzer == 0 ? master_unique_id : secondaries[analyzer - 1].unique_id;

        handle = device.handle;
        name = QString::fromLocal8Bit(device.name);
        unique_id = QString::fromLocal8Bit(device.unique_id);
    }

    bool uses_reference() const
    {
        return reference_alignment_enabled || drift_correction_enabled;
    }

    // Inputs the channel mode leaves
    uint32_t available_channels() const
    {
        return channel_count >= 32 ? LogicStackingAllChannels : ((1u << channel_count) - 1);
    }

    bool channel_available(int index) const
    {
        return index >= 0 && index < channel_count;
    }

    uint32_t selected_channels(int analyzer) const
    {
        if (analyzer == 0)
            return master_channels;
        if (analyzer > 0 && analyzer <= (int)secondaries.size())
            return secondaries[analyzer - 1].channels;
        return 0;
    }

    int reference_channel(int analyzer) const
    {
        if (analyzer == 0)
            return master_reference_channel;
        return secondaries[analyzer - 1].reference_channel;
    }

    // Inputs that are always captured: the sync input, and the reference
    // input while it is used
    uint32_t forced_channels(int analyzer) const
    {
        uint32_t mask = 0;
        if (analyzer > 0 && analyzer <= (int)secondaries.size() &&
            channel_available(secondaries[analyzer - 1].sync_channel))
            mask |= 1u << secondaries[analyzer - 1].sync_channel;
        if (uses_reference() && analyzer >= 0 && analyzer <= (int)secondaries.size() &&
            channel_available(reference_channel(analyzer)))
            mask |= 1u << reference_channel(analyzer);
        return mask;
    }

    // Inputs the analyzer captures
    uint32_t captured_channels(int analyzer) const
    {
        return (selected_channels(analyzer) | forced_channels(analyzer)) & available_channels();
    }

    // Inputs that get a channel in the stacked channel list: the selected ones
    // and the reference input; a sync input only while sync channels are shown
    uint32_t visible_channels(int analyzer) const
    {
        uint32_t mask = captured_channels(analyzer);
        if (analyzer > 0 && analyzer <= (int)secondaries.size() && !show_sync_channel){
            const int sync = secondaries[analyzer - 1].sync_channel;
            const bool also_reference = uses_reference() && secondaries[analyzer - 1].reference_channel == sync;
            if (!also_reference && channel_available(sync))
                mask &= ~(1u << sync);
        }
        return mask;
    }

    // The virtual channel model can be restored before physical devices are remapped.
    bool has_channel_model() const
    {
        if (!enabled || !analyzer_count_valid(analyzer_count()) ||
            !channel_count_valid(channel_count))
            return false;

        if (uses_reference() &&
            (!channel_available(master_reference_channel) ||
             !reference_edge_mode_valid(reference_edge_mode)))
            return false;

        for (const LogicStackingSecondary &secondary : secondaries){
            if (!channel_available(secondary.sync_channel))
                return false;
            if (uses_reference() && !channel_available(secondary.reference_channel))
                return false;
        }

        // The master paces the capture and must record something
        return captured_channels(0) != 0;
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

        // The stacked channel list follows from the analyzers' visible inputs
        if (enabled != other.enabled ||
            secondaries.size() != other.secondaries.size())
            return false;

        for (int analyzer = 0; analyzer < analyzer_count(); analyzer++){
            if (visible_channels(analyzer) != other.visible_channels(analyzer))
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

    static bool channel_count_valid(int count)
    {
        return channel_mode_id(count) >= 0;
    }

    // The driver's channel mode for a channel count, -1 if stacking has none
    static int channel_mode_id(int count)
    {
        for (const LogicStackingChannelMode &mode : LogicStackingChannelModes){
            if (mode.channel_count == count)
                return mode.mode_id;
        }
        return -1;
    }

    static int channel_count_of(uint32_t mask)
    {
        int count = 0;
        for (; mask != 0; mask &= mask - 1)
            count++;
        return count;
    }

    // Masks are saved as decimal strings, since JSON numbers in Qt read back
    // as int; anything unreadable selects all inputs
    static QString channels_to_string(uint32_t mask)
    {
        return QString::number(mask);
    }

    static uint32_t channels_from_string(const QString &text)
    {
        bool ok = false;
        const uint32_t mask = text.toUInt(&ok);
        return ok ? mask : LogicStackingAllChannels;
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
