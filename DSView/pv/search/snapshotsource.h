/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#ifndef DSVIEW_PV_SEARCH_SNAPSHOTSOURCE_H
#define DSVIEW_PV_SEARCH_SNAPSHOTSOURCE_H

#include <atomic>
#include "searchengine.h"

namespace pv {

namespace data {
class LogicSnapshot;
}

namespace search {

// The logic snapshot as the engine sees it, safe on a search thread: once
// the data is cleared or replaced (a new capture, another file), every call
// fails and the search stops with RunStatus::DataChanged.
class SnapshotSource : public SampleSource
{
public:
    // Create on the GUI thread, while no capture runs.
    explicit SnapshotSource(data::LogicSnapshot *snapshot);

    uint64_t sample_count() override { return _count; }
    double samplerate() override { return _rate; }
    bool has_channel(int channel) override;
    bool level(int channel, uint64_t sample) override;
    bool next_change(int channel, uint64_t pos, bool level, uint64_t last,
                     uint64_t &found) override;
    bool failed() override { return _failed.load(std::memory_order_relaxed); }

    inline data::LogicSnapshot *snapshot() const { return _snapshot; }
    inline uint64_t version() const { return _version; }

private:
    data::LogicSnapshot *_snapshot;
    uint64_t _version;
    uint64_t _count;
    double _rate;
    std::atomic<bool> _failed;
};

} // namespace search
} // namespace pv

#endif
