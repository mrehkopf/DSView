/* Regression tests for logic stacking reference alignment and drift correction.
 *
 * Two simulated analyzers sample the same signals. The secondary's clock runs at
 * a known ppm offset and its sync trigger fires with a latency. After merging,
 * the secondary's copy of an independent data signal must line up with the
 * master's copy across the whole capture. */
#include "pv/data/logicstackingmerger.h"
#include "pv/data/logicsnapshot.h"

#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace pv;
using namespace pv::data;

namespace {

const int kReferenceChannel = 3;
const int kDataChannel = 5;
const uint64_t kSamplerate = 250000000ULL;
const uint64_t kSamples = 1ULL << 24;
const ds_device_handle kMaster = 1;
const ds_device_handle kSecondary = 2;

sr_channel channels[2];
GSList *channel_list = NULL;
int failures = 0;

void check(bool success, const std::string &description)
{
    if (!success) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

enum SignalKind { Clock, RandomBits, RandomGaps, Pulses, SinglePulse, Constant };

struct Signal
{
    SignalKind kind;
    double period;
};

struct Scenario
{
    explicit Scenario(const char *n) : name(n) {}

    std::string name;
    Signal reference = {RandomBits, 250};
    double ppm = 30;              // the secondary's clock runs faster by this much
    double trigger_fraction = 0.5;
    double latency = 0;           // sync trigger latency in master samples
    int64_t manual_shift_ps = 0;
    double phase = -1;            // secondary sample phase, random if negative
    bool align = true;
    bool drift = true;
    int edge_mode = LogicStackingReferenceRising;
    int glitches = 0;             // 1-sample pulses on the master's reference only
    bool unrelated = false;       // the secondary's reference carries another signal
};

struct Result
{
    bool merged = false;
    QString warning;
    // master data edge, offset of the matching secondary edge in the merged output
    std::vector<std::pair<int64_t, int64_t> > errors;
};

std::vector<long double> make_edges(const Signal &signal, long double t0, long double t1, std::mt19937_64 &rng)
{
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    std::vector<long double> edges;

    switch (signal.kind) {
    case Clock:
        for (long double t = t0 + uniform(rng) * signal.period / 2; t < t1; t += signal.period / 2)
            edges.push_back(t);
        break;
    case RandomBits: {
        int level = 0;
        for (long double t = t0 + uniform(rng) * signal.period; t < t1; t += signal.period) {
            const int bit = uniform(rng) < 0.5 ? 1 : 0;
            if (bit != level) {
                edges.push_back(t);
                level = bit;
            }
        }
        break;
    }
    case RandomGaps:
        for (long double t = t0 + uniform(rng) * signal.period; t < t1;
             t += signal.period * (1.0 + 3.0 * uniform(rng)))
            edges.push_back(t);
        break;
    case Pulses:
        // 1000-sample pulses, like a pulse-per-second signal
        for (long double t = signal.period / 3; t < t1; t += signal.period) {
            edges.push_back(t);
            edges.push_back(t + 1000);
        }
        break;
    case SinglePulse:
        // A 1000.5-sample pulse at `period`
        edges.push_back(signal.period);
        edges.push_back(signal.period + 1000.5);
        break;
    case Constant:
        break;
    }

    return edges;
}

// LA_CROSS_DATA with the reference and data channels; sample j is taken at time_of(j)
template <typename TimeOf>
std::vector<uint64_t> sample(const std::vector<long double> &reference,
                             const std::vector<long double> &data,
                             uint64_t count,
                             TimeOf time_of)
{
    std::vector<uint64_t> words((count + 63) / 64 * 2, 0);
    const std::vector<long double> *inputs[2] = {&reference, &data};
    size_t next_edge[2] = {0, 0};
    int level[2] = {0, 0};

    for (uint64_t j = 0; j < count; j++) {
        const long double t = time_of(j);
        for (int ch = 0; ch < 2; ch++) {
            const std::vector<long double> &edges = *inputs[ch];
            while (next_edge[ch] < edges.size() && edges[next_edge[ch]] <= t) {
                level[ch] ^= 1;
                next_edge[ch]++;
            }
            if (level[ch])
                words[j / 64 * 2 + ch] |= 1ULL << (j & 63);
        }
    }

    return words;
}

void feed(LogicStackingMerger &merger, ds_device_handle handle, std::vector<uint64_t> &words)
{
    const size_t chunk = 1 << 16;
    for (size_t i = 0; i < words.size(); i += chunk) {
        sr_datafeed_logic logic;
        memset(&logic, 0, sizeof(logic));
        logic.format = LA_CROSS_DATA;
        logic.unitsize = 1;
        logic.data = &words[i];
        logic.length = std::min(chunk, words.size() - i) * sizeof(uint64_t);
        merger.append_logic(handle, logic);
    }
}

std::vector<std::pair<int64_t, int64_t> > data_errors(const std::vector<uint64_t> &words)
{
    // [channel][level after the edge]
    std::vector<int64_t> edges[2][2];
    for (int ch = 0; ch < 2; ch++) {
        int previous = (int)(words[ch] & 1);
        for (uint64_t i = 1; i < kSamples; i++) {
            const int level = (int)((words[i / 64 * 2 + ch] >> (i & 63)) & 1);
            if (level != previous)
                edges[ch][level].push_back((int64_t)i);
            previous = level;
        }
    }

    std::vector<std::pair<int64_t, int64_t> > errors;
    for (int polarity = 0; polarity < 2; polarity++) {
        const std::vector<int64_t> &secondary = edges[1][polarity];
        for (int64_t edge : edges[0][polarity]) {
            // Edges at the very ends may be shifted out of the merged range
            if (edge < 16 || edge >= (int64_t)kSamples - 16)
                continue;

            int64_t error = std::numeric_limits<int64_t>::max();
            std::vector<int64_t>::const_iterator it = std::lower_bound(secondary.begin(), secondary.end(), edge);
            if (it != secondary.end())
                error = *it - edge;
            if (it != secondary.begin() && edge - *(it - 1) < std::llabs(error))
                error = *(it - 1) - edge;
            errors.push_back(std::make_pair(edge, error));
        }
    }

    return errors;
}

Result run(const Scenario &s)
{
    std::mt19937_64 rng(1);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const uint64_t master_trigger = (uint64_t)(kSamples * s.trigger_fraction);
    const uint64_t secondary_trigger = master_trigger + 4096;
    const uint64_t secondary_samples = kSamples + 8192;
    const long double rate = 1.0L + s.ppm * 1e-6L;
    const long double random_phase = uniform(rng);
    const long double phase = s.phase < 0 ? random_phase : s.phase;

    // The secondary's sync trigger fires `latency` master samples after the master's trigger
    auto secondary_time = [&](uint64_t j) {
        return (long double)master_trigger + s.latency + ((long double)j - secondary_trigger + phase) / rate;
    };

    const long double t0 = std::min<long double>(0, secondary_time(0)) - 10;
    const long double t1 = std::max<long double>(kSamples, secondary_time(secondary_samples)) + 10;
    const std::vector<long double> reference = make_edges(s.reference, t0, t1, rng);
    const Signal data_signal = {RandomGaps, 2000};
    const std::vector<long double> data = make_edges(data_signal, t0, t1, rng);

    std::vector<long double> master_reference = reference;
    for (int i = 0; i < s.glitches; i++) {
        const long double t = uniform(rng) * kSamples;
        master_reference.push_back(t);
        master_reference.push_back(t + 1);
    }
    std::sort(master_reference.begin(), master_reference.end());

    std::vector<long double> secondary_reference = reference;
    if (s.unrelated) {
        const Signal other = {RandomGaps, s.reference.period};
        secondary_reference = make_edges(other, t0, t1, rng);
    }

    std::vector<uint64_t> master_words =
        sample(master_reference, data, kSamples, [](uint64_t i) { return (long double)i; });
    std::vector<uint64_t> secondary_words =
        sample(secondary_reference, data, secondary_samples, secondary_time);

    LogicStackingConfig config;
    config.enabled = true;
    config.master_handle = kMaster;
    config.secondary_handle = kSecondary;
    config.secondary_sync_channel = 0;
    config.secondary_manual_shift_ps = s.manual_shift_ps;
    config.reference_alignment_enabled = s.align;
    config.drift_correction_enabled = s.drift;
    config.drift_master_channel = kReferenceChannel;
    config.drift_secondary_channel = kReferenceChannel;
    config.reference_edge_mode = s.edge_mode;

    LogicStackingMerger merger;
    merger.reset(config, kSamples, kSamplerate, false);
    feed(merger, kMaster, master_words);
    feed(merger, kSecondary, secondary_words);

    ds_trigger_pos trigger;
    memset(&trigger, 0, sizeof(trigger));
    trigger.status = 1;
    trigger.real_pos = (uint32_t)master_trigger;
    merger.set_trigger(kMaster, trigger);
    trigger.real_pos = (uint32_t)secondary_trigger;
    merger.set_trigger(kSecondary, trigger);
    merger.mark_end(kMaster);
    merger.mark_end(kSecondary);

    std::vector<LogicStackingChannel> visible(2);
    visible[0].analyzer = 0;
    visible[0].physical_index = kDataChannel;
    visible[0].global_index = kDataChannel;
    visible[1].analyzer = 1;
    visible[1].physical_index = kDataChannel;
    visible[1].global_index = 32 + kDataChannel;

    LogicSnapshot snapshot;
    Result result;
    result.merged = merger.complete() && merger.flush_to_snapshot(&snapshot, channel_list, visible);
    result.warning = merger.warning();
    if (result.merged)
        result.errors = data_errors(snapshot.words());

    return result;
}

int64_t max_abs_error(const Result &r,
                      int64_t first = std::numeric_limits<int64_t>::min(),
                      int64_t last = std::numeric_limits<int64_t>::max())
{
    int64_t worst = r.errors.empty() ? std::numeric_limits<int64_t>::max() : 0;
    for (const std::pair<int64_t, int64_t> &e : r.errors) {
        if (e.first >= first && e.first <= last)
            worst = std::max<int64_t>(worst, std::llabs(e.second));
    }
    return worst;
}

double reported_ppm(const QString &warning)
{
    const QRegularExpressionMatch match =
        QRegularExpression("secondary drift (-?[0-9.]+) ppm").match(warning);
    return match.hasMatch() ? match.captured(1).toDouble() : NAN;
}

bool mentions(const Result &r, const char *text)
{
    return r.warning.contains(text);
}

std::string describe(const Scenario &s, const Result &r, const char *what)
{
    return s.name + ": " + what + " (max error " + std::to_string(max_abs_error(r)) +
           " samples; " + r.warning.toStdString() + ")";
}

// Every data edge of the secondary lines up with the master's copy
void check_aligned(const Scenario &s, int64_t tolerance = 1)
{
    const Result r = run(s);
    check(r.merged, describe(s, r, "merge succeeds"));
    check(max_abs_error(r) <= tolerance, describe(s, r, "data aligned over the whole capture"));
    if (s.drift) {
        check(mentions(r, "drift correction applied"), describe(s, r, "drift correction applied"));
        // The merger reports master relative to secondary
        check(std::fabs(reported_ppm(r.warning) + s.ppm) < 0.2, describe(s, r, "drift estimate"));
    }
}

}

extern "C" GSList* ds_get_device_channels_by_handle(ds_device_handle)
{
    return channel_list;
}

int main()
{
    const int indexes[2] = {kReferenceChannel, kDataChannel};
    for (int i = 0; i < 2; i++) {
        memset(&channels[i], 0, sizeof(channels[i]));
        channels[i].index = indexes[i];
        channels[i].type = SR_CHANNEL_LOGIC;
        channels[i].enabled = TRUE;
        channel_list = g_slist_append(channel_list, &channels[i]);
    }

    // Accumulated drift far exceeds the reference's edge spacing at the capture ends
    Scenario dense_bits("dense random reference, alignment and drift");
    dense_bits.ppm = 60;
    dense_bits.latency = 4;
    check_aligned(dense_bits);

    Scenario dense_clock("dense clock reference, drift only, early trigger");
    dense_clock.reference = {Clock, 250};
    dense_clock.ppm = -40;
    dense_clock.trigger_fraction = 0.1;
    dense_clock.latency = 4;
    dense_clock.manual_shift_ps = 16000; // 4 samples at 250 MHz compensate the latency
    dense_clock.align = false;
    check_aligned(dense_clock);

    Scenario both_edges("both edges, late trigger");
    both_edges.reference = {RandomGaps, 500};
    both_edges.ppm = 80;
    both_edges.trigger_fraction = 0.9;
    both_edges.latency = 6;
    both_edges.edge_mode = LogicStackingReferenceBoth;
    check_aligned(both_edges);

    Scenario sparse("pulse-per-second style reference with three edges");
    sparse.reference = {Pulses, 6000000};
    sparse.latency = 3;
    check_aligned(sparse, 2);

    Scenario glitches("glitches on the master reference");
    glitches.glitches = 2000;
    glitches.latency = 4;
    check_aligned(glitches);

    {
        // Without drift correction the reference only aligns the trigger point;
        // 30 ppm still adds up to 0.3 samples 10000 samples away
        Scenario s("alignment only");
        s.trigger_fraction = 0.25;
        s.latency = 5;
        s.drift = false;
        const Result r = run(s);
        const int64_t trigger = (int64_t)(kSamples / 4);
        check(r.merged, describe(s, r, "merge succeeds"));
        check(mentions(r, "reference alignment applied"), describe(s, r, "alignment applied"));
        check(!mentions(r, "drift"), describe(s, r, "no drift correction"));
        check(max_abs_error(r, trigger - 10000, trigger + 10000) <= 1,
              describe(s, r, "data aligned around the trigger"));
    }

    {
        // Two pairs 1000 samples apart give a slope too poor to extrapolate to
        // the trigger. The secondary's sample phase makes the two analyzers
        // quantize the pulse width differently.
        Scenario s("single pulse far from the trigger");
        s.reference = {SinglePulse, 1000000.25};
        s.phase = 0.5;
        s.ppm = 0;
        s.latency = 4;
        s.edge_mode = LogicStackingReferenceBoth;
        const Result r = run(s);
        check(r.merged, describe(s, r, "merge succeeds"));
        check(mentions(r, "reference alignment applied"), describe(s, r, "alignment applied"));
        check(mentions(r, "drift correction skipped: not enough matching reference edges"),
              describe(s, r, "drift correction skipped"));
        check(max_abs_error(r) <= 1, describe(s, r, "data aligned over the whole capture"));
    }

    {
        Scenario s("unrelated reference signals");
        s.unrelated = true;
        const Result r = run(s);
        check(mentions(r, "reference alignment skipped: reference edge timing is inconsistent"),
              describe(s, r, "alignment rejected as inconsistent"));
        check(mentions(r, "drift correction skipped: reference edge timing is inconsistent"),
              describe(s, r, "drift correction rejected as inconsistent"));
        check(!mentions(r, "applied"), describe(s, r, "nothing applied"));
    }

    {
        Scenario s("reference without edges");
        s.reference = {Constant, 0};
        const Result r = run(s);
        check(mentions(r, "reference alignment skipped: not enough matching reference edges"),
              describe(s, r, "alignment reports missing edges"));
        check(mentions(r, "drift correction skipped: not enough matching reference edges"),
              describe(s, r, "drift correction reports missing edges"));
    }

    {
        Scenario s("implausible drift");
        s.ppm = 3000;
        const Result r = run(s);
        check(!mentions(r, "drift correction applied"), describe(s, r, "drift correction not applied"));
    }

    if (failures == 0)
        std::cout << "All stacking merger tests passed\n";

    return failures == 0 ? 0 : 1;
}
