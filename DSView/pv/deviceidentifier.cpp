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

#include "deviceidentifier.h"

#include "log.h"

namespace pv {

namespace {

const int StepMs = 200;
// Red on even steps, green on odd ones: five red-green cycles
const int StepCount = 10;

}

DeviceIdentifier::DeviceIdentifier()
{
    _timer.SetCallback([this](){ on_step(); });
}

bool DeviceIdentifier::identify(ds_device_handle handle)
{
    if (handle == NULL_HANDLE || ds_is_collecting())
        return false;

    int ret = ds_set_device_led_by_handle(handle, DS_LED_RED);
    if (ret != SR_OK){
        dsv_info("Can't blink the LED of device %p: error %d.", (void*)handle, ret);
        return false;
    }

    bool found = false;
    for (Blink &blink : _blinks){
        if (blink.handle == handle){
            blink.step = 1;
            found = true;
        }
    }
    if (!found)
        _blinks.push_back({handle, 1});

    _timer.Start(StepMs);
    return true;
}

void DeviceIdentifier::on_step()
{
    for (auto it = _blinks.begin(); it != _blinks.end();){
        const bool last = it->step >= StepCount;
        const int leds = last ? DS_LED_IDLE : (it->step % 2 ? DS_LED_GREEN : DS_LED_RED);

        // A device that fails (unplugged, say) stops blinking
        if (ds_set_device_led_by_handle(it->handle, leds) != SR_OK || last){
            it = _blinks.erase(it);
        }
        else{
            it->step++;
            ++it;
        }
    }

    if (_blinks.empty())
        _timer.Stop();
}

void DeviceIdentifier::stop_all()
{
    for (const Blink &blink : _blinks)
        ds_set_device_led_by_handle(blink.handle, DS_LED_IDLE);

    _blinks.clear();
    _timer.Stop();
}

} // namespace pv
