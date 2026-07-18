/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2026 Schildkroet
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef DSVIEW_PV_DATA_DSOEDGEDETECT_H
#define DSVIEW_PV_DATA_DSOEDGEDETECT_H

#include <stdint.h>
#include <algorithm>
#include <vector>

namespace pv {
namespace data {

// Sample indices of the rising/falling edges found by dso_detect_edges().
struct DsoEdgeSet
{
    std::vector<uint64_t> rising;
    std::vector<uint64_t> falling;
};

// Schmitt-trigger edge detection over a DSO sample range: tracks a high/low
// state and flips it once the signal clears the upper/lower threshold (5% of
// hysteresis around the value's midpoint), recording the sample index of each
// transition. This tolerates finite rise time and noise, unlike a single-step
// "crossed mid this sample" test.
//
// value_at(i) must return the signal's value for sample index i, in whatever
// unit is convenient at the call site (raw ADC counts, volts, hw_offset-
// relative counts, ...) - only relative comparisons are made, so the unit
// does not matter as long as it is used consistently across the whole range.
// Returns an empty set if there are fewer than 2 samples or the signal is
// flat (max <= min), since no threshold could then be established.
template <typename ValueFn>
DsoEdgeSet dso_detect_edges(uint64_t sample_count, ValueFn &&value_at)
{
    DsoEdgeSet edges;

    if (sample_count < 2)
        return edges;

    double vmin = 1e300, vmax = -1e300;
    for (uint64_t i = 0; i < sample_count; i++) {
        const double v = value_at(i);
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
    }
    if (vmax <= vmin)
        return edges;   // flat signal: no threshold to detect edges against

    const double mid = (vmin + vmax) / 2.0;
    const double hyst = (vmax - vmin) * 0.05;
    const double hi = mid + hyst;
    const double lo = mid - hyst;

    bool is_high = (value_at(0) >= mid);
    for (uint64_t i = 1; i < sample_count; i++) {
        const double v = value_at(i);
        if (!is_high && v >= hi) {
            is_high = true;
            edges.rising.push_back(i);
        } else if (is_high && v <= lo) {
            is_high = false;
            edges.falling.push_back(i);
        }
    }

    return edges;
}

} // namespace data
} // namespace pv

#endif // DSVIEW_PV_DATA_DSOEDGEDETECT_H
