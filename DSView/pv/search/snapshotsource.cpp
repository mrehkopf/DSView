/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#include "snapshotsource.h"
#include "../data/logicsnapshot.h"

namespace pv {
namespace search {

static_assert(MaxChannels >= CHANNEL_MAX_COUNT, "the search must reach every logic channel");

SnapshotSource::SnapshotSource(data::LogicSnapshot *snapshot) :
    _snapshot(snapshot),
    _failed(false)
{
    _version = snapshot->data_version();
    _count = snapshot->get_ring_sample_count();
    _rate = snapshot->samplerate();
}

bool SnapshotSource::has_channel(int channel)
{
    if (channel < 0 || channel >= CHANNEL_MAX_COUNT)
        return false;
    const int r = _snapshot->search_has_channel(_version, channel);
    if (r < 0)
        _failed = true;
    return r == 1;
}

bool SnapshotSource::level(int channel, uint64_t sample)
{
    if (failed())
        return false;
    const int r = _snapshot->search_level(_version, sample, channel);
    if (r < 0){
        _failed = true;
        return false;
    }
    return r == 1;
}

bool SnapshotSource::next_change(int channel, uint64_t pos, bool level, uint64_t last,
                                 uint64_t &found)
{
    if (failed())
        return false;
    const int r = _snapshot->search_next_change(_version, pos, level, last, channel, found);
    if (r < 0){
        _failed = true;
        return false;
    }
    return r == 1;
}

} // namespace search
} // namespace pv
