/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#ifndef DSVIEW_PV_SEARCH_SEARCHENGINE_H
#define DSVIEW_PV_SEARCH_SEARCHENGINE_H

#include <stdint.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "searchquery.h"

// Finds the matches of a query in logic data, in one forward pass over the
// capture (doc/search.md has the rules). The data comes through
// SampleSource, so the engine runs on a fake in DSView/tests/search.

namespace pv {
namespace search {

const int MaxConditions = 8;
const int MaxBusWidth = 64;
const int MaxChannels = 256;

class SampleSource
{
public:
    virtual ~SampleSource() {}

    virtual uint64_t sample_count() = 0;
    virtual double samplerate() = 0;
    virtual bool has_channel(int channel) = 0;

    virtual bool level(int channel, uint64_t sample) = 0;

    // The first sample in (pos, last] whose level differs from level, which
    // is the level at pos.
    virtual bool next_change(int channel, uint64_t pos, bool level, uint64_t last,
                             uint64_t &found) = 0;

    // True once the data went away; the search stops.
    virtual bool failed() = 0;
};

//------------------------------------------------------------- compiled form

// One pattern: what each channel must do, plus bus comparisons and "bus
// changes" terms. Channels are the snapshot's channel indexes; -1 in a
// bus is a bit that is always 0.
struct PatternSpec
{
    struct Compare {
        std::vector<int> bits;
        ValueOp op;
        uint64_t value;
        uint64_t mask;
        uint64_t high;
    };

    std::vector<std::pair<int, Req>> needs;     // one entry per channel
    std::vector<Compare> compares;
    std::vector<std::vector<int>> changes;      // at least one of each changes
    bool has_edge = false;
};

using EventSpec = std::vector<PatternSpec>;     // any of them

struct ConditionSpec
{
    bool state = false;
    EventSpec subject;
    Quantifier quantifier = Quantifier::Occurs;
    uint64_t count = 0;
    WindowKind window = WindowKind::Start;
    int64_t from = 0;                           // samples
    int64_t to = 0;
    EventSpec boundary;
};

struct Program
{
    bool state = false;
    EventSpec find;
    bool has_min = false;
    bool has_max = false;
    uint64_t min_duration = 0;                  // samples
    uint64_t max_duration = 0;
    std::vector<ConditionSpec> conditions;
};

using BusResolver = std::function<bool(const QString &name, std::vector<int> &bits)>;
using ChannelCheck = std::function<bool(int channel)>;

// Checks a query and compiles it. Without a sample rate (0) times are only
// checked, and without has_channel any channel is taken to have data.
bool compile(const Query &query, const BusResolver &buses, const ChannelCheck &has_channel,
             double samplerate, Program &program, Error &error);

//------------------------------------------------------------- matches

struct ConditionValue
{
    uint64_t count = 0;         // occurrences in the window (the part in the capture)
    bool has_delta = false;
    int64_t delta = 0;          // samples from the reference to the nearest occurrence
};

struct Match
{
    uint64_t start = 0;
    uint64_t end = 0;           // events: = start; states: the first sample after it
    bool start_unknown = false; // a state that already held at the capture start
    bool end_unknown = false;   // a state that still holds at the capture end
    ConditionValue values[MaxConditions];
};

enum class RunStatus {
    Done,
    Limit,          // stopped after limit matches
    Cancelled,
    DataChanged,    // the source failed
    TooMuchData     // a window holds too many occurrences
};

struct RunResult
{
    RunStatus status = RunStatus::Done;
    uint64_t matches = 0;
    uint64_t next = 0;          // Limit: where to search on from
};

// Reports, in order, the matches that start in [from, to). Conditions look
// at the whole capture, whatever the range. Progress is the sample the
// search has got to.
RunResult run(const Program &program, SampleSource &source, uint64_t from, uint64_t to,
              uint64_t limit, const std::atomic<bool> &cancel, std::atomic<uint64_t> *progress,
              const std::function<void(const Match &)> &sink);

// Whether a condition's count or delta is worth showing.
inline bool reports_value(const ConditionSpec &c)
{
    return !c.state && c.quantifier != Quantifier::None;
}

} // namespace search
} // namespace pv

#endif
