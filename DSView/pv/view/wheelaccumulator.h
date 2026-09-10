/*
 * This file is part of the DSView project.
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 */

#ifndef DSVIEW_PV_VIEW_WHEELACCUMULATOR_H
#define DSVIEW_PV_VIEW_WHEELACCUMULATOR_H

#include <QDateTime>
#include <cmath>

namespace pv {
namespace view {

// A classic notched mouse wheel reports one detent as an angleDelta() of 120.
// High-resolution wheels (libinput/Wayland, free-spinning wheels, trackpads)
// instead send a burst of much smaller deltas that add up to those 120 units.
// Anything that reacts in discrete steps (the DSO horizontal knob, the vertical
// dials in the header, spin boxes) would round every one of those fractions
// down to "no step at all", which is why zooming looks completely dead on such
// devices. This helper sums the fractions up and hands out whole steps only,
// carrying the remainder over to the next event, so one physical detent always
// produces exactly one step no matter how finely the device reports it.
class WheelAccumulator
{
public:
    // unit: how much input makes up one step (120 for raw angleDelta values,
    //       1.0 when the caller already works in detents).
    explicit WheelAccumulator(double unit = 120.0) :
        _unit(unit),
        _accum(0),
        _last_time(0)
    {
    }

    // Returns the number of whole steps contained in amount, keeping the
    // leftover for the following event. Sign follows amount.
    int take(double amount)
    {
        if (amount == 0)
            return 0;

        const int64_t now = QDateTime::currentMSecsSinceEpoch();

        // Drop a stale remainder, and never let opposite directions cancel out
        // a scroll the user just started.
        if (now - _last_time > IdleResetMs || (_accum != 0 && (_accum > 0) != (amount > 0)))
            _accum = 0;

        _last_time = now;
        _accum += amount / _unit;

        const int steps = (int)(_accum > 0 ? std::floor(_accum) : std::ceil(_accum));
        _accum -= steps;

        return steps;
    }

    void reset()
    {
        _accum = 0;
        _last_time = 0;
    }

private:
    static const int64_t IdleResetMs = 500;

    double  _unit;
    double  _accum;
    int64_t _last_time;
};

} // namespace view
} // namespace pv

#endif
