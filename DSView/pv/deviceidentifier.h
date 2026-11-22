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

#ifndef DSVIEW_PV_DEVICEIDENTIFIER_H
#define DSVIEW_PV_DEVICEIDENTIFIER_H

#include <vector>
#include <libsigrok.h>

#include "dstimer.h"

namespace pv {

// Blinks a device's LED red and green in turn for two seconds, so the user
// can see which analyzer a device list entry is. Several devices can blink at
// once. Afterwards the LED shows what the device shows on its own again.
class DeviceIdentifier
{
public:
    DeviceIdentifier();

    // Starts blinking, or starts over if the device blinks already. False if
    // the device has no LED, can't be reached or a capture runs.
    bool identify(ds_device_handle handle);

    // Ends every blink now, before a capture starts or DSView exits
    void stop_all();

private:
    void on_step();

private:
    struct Blink
    {
        ds_device_handle handle;
        int step;
    };

    std::vector<Blink> _blinks;
    DsTimer _timer;
};

} // namespace pv

#endif // DSVIEW_PV_DEVICEIDENTIFIER_H
