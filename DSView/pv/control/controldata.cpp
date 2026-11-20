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

#include "controldata.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QElapsedTimer>

#include "../data/logicsnapshot.h"

namespace pv {
namespace control {
namespace logicdata {

const int TimeBudget = 500;

// Looking at the clock every this many edges
const uint64_t ClockInterval = 4096;

namespace {

double rate_of(data::LogicSnapshot *snapshot)
{
    const double rate = snapshot->samplerate();
    return rate > 0 ? rate : 1;
}

QJsonArray channel_list(uint64_t bits, const std::vector<int> &channels)
{
    QJsonArray a;
    for (size_t i = 0; i < channels.size(); i++){
        if (bits & (1ULL << i))
            a.append(channels[i]);
    }
    return a;
}

QJsonObject position(uint64_t sample, double rate)
{
    QJsonObject o;
    o["sample"] = (double)sample;
    o["time"] = sample / rate;
    return o;
}

// Durations in samples; shown in seconds.
struct Stat
{
    uint64_t    count = 0;
    uint64_t    min = 0;
    uint64_t    max = 0;
    double      sum = 0;

    void add(uint64_t v)
    {
        if (count == 0 || v < min)
            min = v;
        if (v > max)
            max = v;
        sum += v;
        count++;
    }

    QJsonValue json(double rate) const
    {
        if (count == 0)
            return QJsonValue();
        QJsonObject o;
        o["count"] = (double)count;
        o["min"] = min / rate;
        o["max"] = max / rate;
        o["mean"] = sum / count / rate;
        return o;
    }
};

} // namespace

//------------------------------------------------------------- EdgeWalker

EdgeWalker::EdgeWalker(data::LogicSnapshot *snapshot, const std::vector<int> &channels,
                       uint64_t start, uint64_t end) :
    _snapshot(snapshot),
    _end(end)
{
    for (int index : channels){
        Channel c;
        c.index = index;
        c.pos = start;
        c.level = start < end && snapshot->get_sample(start, index);
        c.has_next = false;
        c.next = 0;
        advance(c);
        _ch.push_back(c);
    }
}

void EdgeWalker::advance(Channel &c)
{
    c.has_next = false;
    if (c.pos + 1 >= _end)
        return;

    // get_nxt_edge looks from index up to end (inclusive) for the first
    // sample whose level differs from last_sample, and puts it in index.
    uint64_t index = c.pos;
    if (!_snapshot->get_nxt_edge(index, c.level, _end - 1, 1, c.index))
        return;
    if (index <= c.pos || index >= _end)
        return;

    c.has_next = true;
    c.next = index;
}

bool EdgeWalker::next(uint64_t &sample, uint64_t &rising, uint64_t &falling)
{
    bool found = false;
    uint64_t first = 0;

    for (const Channel &c : _ch){
        if (c.has_next && (!found || c.next < first)){
            first = c.next;
            found = true;
        }
    }
    if (!found)
        return false;

    sample = first;
    rising = 0;
    falling = 0;

    for (size_t i = 0; i < _ch.size(); i++){
        Channel &c = _ch[i];
        if (c.has_next && c.next == first){
            if (c.level)
                falling |= 1ULL << i;
            else
                rising |= 1ULL << i;
            c.level = !c.level;
            c.pos = first;
            advance(c);
        }
    }
    return true;
}

//------------------------------------------------------------- queries

QJsonObject edges(data::LogicSnapshot *snapshot, const std::vector<int> &channels,
                  uint64_t start, uint64_t end, uint64_t limit)
{
    const double rate = rate_of(snapshot);
    EdgeWalker walker(snapshot, channels, start, end);

    QJsonObject levels;
    for (size_t i = 0; i < channels.size(); i++)
        levels[QString::number(channels[i])] = walker.level(i) ? 1 : 0;

    QJsonArray list;
    uint64_t sample, rising, falling;
    uint64_t last = start;
    bool more = false;

    while (walker.next(sample, rising, falling)){
        if ((uint64_t)list.size() >= limit){
            more = true;
            break;
        }
        QJsonObject e = position(sample, rate);
        if (rising)
            e["rising"] = channel_list(rising, channels);
        if (falling)
            e["falling"] = channel_list(falling, channels);
        list.append(e);
        last = sample;
    }

    QJsonObject r;
    r["start"] = (double)start;
    r["end"] = (double)end;
    r["samplerate"] = snapshot->samplerate();
    r["levels"] = levels;
    r["edges"] = list;
    // The edges after a sample: continuing from the last one reported
    // starts with its new levels.
    r["next"] = more ? QJsonValue((double)last) : QJsonValue();
    return r;
}

QJsonObject measure(data::LogicSnapshot *snapshot, const std::vector<int> &channels,
                    uint64_t start, uint64_t end)
{
    const double rate = rate_of(snapshot);
    QJsonArray results;
    QElapsedTimer clock;
    clock.start();

    for (size_t ch = 0; ch < channels.size(); ch++){
        const int channel = channels[ch];
        // Each channel's share of the time, plus what the ones before left.
        const qint64 deadline = TimeBudget * (qint64)(ch + 1) / (qint64)channels.size();
        EdgeWalker walker(snapshot, std::vector<int>(1, channel), start, end);
        const bool level = walker.level(0);

        uint64_t rising_count = 0, falling_count = 0, edges = 0;
        bool have_rise = false, have_fall = false, have_edge = false;
        uint64_t last_rise = 0, last_fall = 0, first_edge = 0, last_edge = 0;
        Stat high, low, period;
        double duty_high = 0, duty_total = 0;
        bool truncated = false;

        uint64_t sample, rising, falling;
        while (walker.next(sample, rising, falling)){
            if (!have_edge)
                first_edge = sample;
            have_edge = true;
            last_edge = sample;

            if (rising){
                rising_count++;
                if (have_fall)
                    low.add(sample - last_fall);
                if (have_rise){
                    period.add(sample - last_rise);
                    // A whole period: the high part ended at the fall in it.
                    if (have_fall && last_fall > last_rise){
                        duty_high += last_fall - last_rise;
                        duty_total += sample - last_rise;
                    }
                }
                last_rise = sample;
                have_rise = true;
            }
            else{
                falling_count++;
                if (have_rise)
                    high.add(sample - last_rise);
                last_fall = sample;
                have_fall = true;
            }

            if (++edges % ClockInterval == 0 && clock.elapsed() >= deadline){
                truncated = true;
                break;
            }
        }

        QJsonObject m;
        m["channel"] = channel;
        m["level"] = level ? 1 : 0;
        m["rising"] = (double)rising_count;
        m["falling"] = (double)falling_count;
        m["first_edge"] = have_edge ? QJsonValue((double)first_edge) : QJsonValue();
        m["last_edge"] = have_edge ? QJsonValue((double)last_edge) : QJsonValue();
        // Whole pulses and periods only: the ones cut by start or end are left out.
        m["high"] = high.json(rate);
        m["low"] = low.json(rate);
        m["period"] = period.json(rate);
        m["frequency"] = period.count ? QJsonValue(rate * period.count / period.sum) : QJsonValue();
        m["duty"] = duty_total > 0 ? QJsonValue(100.0 * duty_high / duty_total) : QJsonValue();
        if (truncated){
            // Ran out of time after this edge; measuring from it (edges
            // after start count) covers the rest.
            m["truncated"] = true;
            m["next"] = (double)last_edge;
        }
        results.append(m);
    }

    QJsonObject r;
    r["start"] = (double)start;
    r["end"] = (double)end;
    r["samplerate"] = snapshot->samplerate();
    r["channels"] = results;
    return r;
}

QJsonObject find(data::LogicSnapshot *snapshot, const QMap<int, QChar> &pattern,
                 uint64_t start, uint64_t end, uint64_t limit)
{
    const double rate = rate_of(snapshot);

    std::vector<int> channels;
    std::vector<char> conds;
    bool edge_condition = false;
    for (auto it = pattern.cbegin(); it != pattern.cend(); ++it){
        channels.push_back(it.key());
        conds.push_back(it.value().toLatin1());
        if (QStringLiteral("RFC").contains(it.value()))
            edge_condition = true;
    }

    // At a sample with these levels (bit per channel) and changes from the
    // one before.
    auto holds = [&](uint64_t levels, uint64_t rising, uint64_t falling){
        for (size_t i = 0; i < conds.size(); i++){
            const uint64_t bit = 1ULL << i;
            bool ok = true;
            switch (conds[i]){
                case '0': ok = !(levels & bit); break;
                case '1': ok = (levels & bit) != 0; break;
                case 'R': ok = (rising & bit) != 0; break;
                case 'F': ok = (falling & bit) != 0; break;
                case 'C': ok = ((rising | falling) & bit) != 0; break;
            }
            if (!ok)
                return false;
        }
        return true;
    };

    // A match is a sample where the pattern becomes true: its edges happen
    // there, or, for levels only, they hold there and not at the sample
    // before. That doesn't depend on where the search starts, so
    // continuing from "next" repeats nothing.
    auto matches_at = [&](uint64_t prev, uint64_t cur, bool first_sample){
        const uint64_t rising = first_sample ? 0 : (~prev & cur);
        const uint64_t falling = first_sample ? 0 : (prev & ~cur);
        if (!holds(cur, rising, falling))
            return false;
        return edge_condition || first_sample || !holds(prev, 0, 0);
    };

    QJsonArray found;
    uint64_t examined = 0;
    QElapsedTimer clock;
    clock.start();
    uint64_t last = start;
    bool stopped = false;

    if (start < end){
        uint64_t cur = 0, prev = 0;
        for (size_t i = 0; i < channels.size(); i++){
            if (snapshot->get_sample(start, channels[i]))
                cur |= 1ULL << i;
            if (start > 0 && snapshot->get_sample(start - 1, channels[i]))
                prev |= 1ULL << i;
        }
        if (matches_at(prev, cur, start == 0))
            found.append(position(start, rate));

        EdgeWalker walker(snapshot, channels, start, end);
        uint64_t sample, rising, falling;

        while ((uint64_t)found.size() < limit){
            if (examined % ClockInterval == ClockInterval - 1 && clock.elapsed() >= TimeBudget){
                stopped = true;
                break;
            }
            if (!walker.next(sample, rising, falling))
                break;
            examined++;
            last = sample;

            prev = cur;
            cur = (cur | rising) & ~falling;
            if (matches_at(prev, cur, false))
                found.append(position(sample, rate));
        }

        if ((uint64_t)found.size() >= limit){
            // There may be more.
            last = (uint64_t)found.last().toObject().value("sample").toDouble();
            stopped = last + 1 < end;
        }
    }

    QJsonObject r;
    r["start"] = (double)start;
    r["end"] = (double)end;
    r["samplerate"] = snapshot->samplerate();
    r["matches"] = found;
    // Search on from here; null when the range is done.
    r["next"] = stopped ? QJsonValue((double)(last + 1)) : QJsonValue();
    return r;
}

} // namespace logicdata
} // namespace control
} // namespace pv
