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
#include <QString>
#include <libsigrok.h>

namespace pv {

struct LogicStackingConfig
{
    bool enabled = false;
    ds_device_handle master_handle = NULL_HANDLE;
    ds_device_handle secondary_handle = NULL_HANDLE;
    int secondary_sync_channel = 0;
    bool show_sync_channel = false;
    int64_t secondary_manual_shift_ps = 0;

    // The virtual 64-channel model can be restored before physical devices are remapped.
    bool has_channel_model() const
    {
        return enabled &&
               secondary_sync_channel >= 0 &&
               secondary_sync_channel < 32;
    }

    bool is_valid() const
    {
        return has_channel_model() &&
               master_handle != NULL_HANDLE &&
               secondary_handle != NULL_HANDLE &&
               master_handle != secondary_handle;
    }

    bool has_same_channel_model(const LogicStackingConfig &other) const
    {
        // Handles are excluded deliberately; remapping analyzers must not reset traces or decoders.
        return enabled == other.enabled &&
               secondary_sync_channel == other.secondary_sync_channel &&
               show_sync_channel == other.show_sync_channel;
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
