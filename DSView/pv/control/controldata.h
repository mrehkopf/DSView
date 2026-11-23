/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
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

#ifndef DSVIEW_PV_CONTROL_CONTROLDATA_H
#define DSVIEW_PV_CONTROL_CONTROLDATA_H

#include <stdint.h>
#include <vector>
#include <QJsonObject>
#include <QMap>
#include <QChar>

namespace pv {

namespace data {
class LogicSnapshot;
}

namespace control {

// Queries on the captured logic data (doc/control-interface.md, "Data").
// Positions are sample numbers; end is exclusive.
namespace logicdata {

// The edges of some channels, in sample order, after start and before end.
class EdgeWalker
{
public:
    EdgeWalker(data::LogicSnapshot *snapshot, const std::vector<int> &channels,
               uint64_t start, uint64_t end);

    // The next sample where any of the channels changes. The bits of rising
    // and falling are positions in the channel list. False at the end.
    bool next(uint64_t &sample, uint64_t &rising, uint64_t &falling);

    // A channel's level at the last sample next() returned, or at start.
    inline bool level(size_t i) const { return _ch[i].level; }

private:
    struct Channel
    {
        int         index;
        bool        level;
        uint64_t    pos;        // where level holds
        bool        has_next;
        uint64_t    next;       // first sample with the other level
    };

    void advance(Channel &c);

    data::LogicSnapshot     *_snapshot;
    std::vector<Channel>    _ch;
    uint64_t                _end;
};

// How long a measure or find call may work, in ms. They run on the GUI
// thread; a call that runs out reports where it stopped.
extern const int TimeBudget;

// data.edges: the levels at start, then each sample where a channel
// changes, up to limit of them.
QJsonObject edges(data::LogicSnapshot *snapshot, const std::vector<int> &channels,
                  uint64_t start, uint64_t end, uint64_t limit);

// data.measure: edge counts, pulse widths, period, frequency and duty
// cycle of each channel.
QJsonObject measure(data::LogicSnapshot *snapshot, const std::vector<int> &channels,
                    uint64_t start, uint64_t end);

// data.find: the samples where a pattern of conditions (0 1 R F C, by
// channel) becomes true, up to limit of them.
QJsonObject find(data::LogicSnapshot *snapshot, const QMap<int, QChar> &pattern,
                 uint64_t start, uint64_t end, uint64_t limit);

} // namespace logicdata
} // namespace control
} // namespace pv

#endif // DSVIEW_PV_CONTROL_CONTROLDATA_H
