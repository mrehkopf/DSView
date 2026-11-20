/* Regression tests for logic stacking reference alignment and drift correction.
 *
 * Simulated analyzers sample the same signals. Each secondary's clock runs at a
 * known ppm offset and its sync trigger fires with a latency. After merging,
 * every secondary's copy of an independent data signal must line up with the
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
// Secondary k (A{k+2}) has handle kSecondary + k
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

struct Secondary
{
    double ppm = 30;              // the secondary's clock runs faster by this much
    double latency = 0;           // sync trigger latency in master samples
    int64_t manual_shift_ps = 0;
    double phase = -1;            // secondary sample phase, random if negative
    bool unrelated = false;       // the secondary's reference carries another signal
};

// The Secondary fields describe A2; `more` adds A3, A4, ...
struct Scenario : Secondary
{
    explicit Scenario(const char *n) : name(n) {}

    std::string name;
    Signal reference = {RandomBits, 250};
    double trigger_fraction = 0.5;
    bool align = true;
    bool drift = true;
    bool instant = false;
    int edge_mode = LogicStackingReferenceRising;
    int glitches = 0;             // 1-sample pulses on the master's reference only
    std::vector<Secondary> more;

    std::vector<Secondary> secondaries() const
    {
        std::vector<Secondary> all(1, *this);
        all.insert(all.end(), more.begin(), more.end());
        return all;
    }
};

typedef std::vector<std::pair<int64_t, int64_t> > Errors;

struct Result
{
    bool merged = false;
    QString warning;
    // per secondary: master data edge, offset of the matching secondary edge in the merged output
    std::vector<Errors> errors;
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

// Compares merged channel `column` with the master's copy in column 0
Errors data_errors(const std::vector<uint64_t> &words, size_t columns, size_t column)
{
    // [master, secondary][level after the edge]
    std::vector<int64_t> edges[2][2];
    const size_t compared[2] = {0, column};
    for (int ch = 0; ch < 2; ch++) {
        const size_t col = compared[ch];
        int previous = (int)(words[col] & 1);
        for (uint64_t i = 1; i < kSamples; i++) {
            const int level = (int)((words[i / 64 * columns + col] >> (i & 63)) & 1);
            if (level != previous)
                edges[ch][level].push_back((int64_t)i);
            previous = level;
        }
    }

    Errors errors;
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
    const std::vector<Secondary> secondaries = s.secondaries();
    const size_t count = secondaries.size();
    const uint64_t master_trigger = (uint64_t)(kSamples * s.trigger_fraction);
    const long double random_phase = uniform(rng);

    std::vector<uint64_t> secondary_trigger(count);
    std::vector<uint64_t> secondary_samples(count);
    std::vector<long double> rate(count);
    std::vector<long double> phase(count);
    for (size_t k = 0; k < count; k++) {
        // Later analyzers buffer a little more pre-trigger data
        secondary_trigger[k] = master_trigger + 4096 + 512 * k;
        secondary_samples[k] = kSamples + 8192 + 512 * k;
        rate[k] = 1.0L + secondaries[k].ppm * 1e-6L;
        const long double drawn = k == 0 ? random_phase : uniform(rng);
        phase[k] = secondaries[k].phase < 0 ? drawn : secondaries[k].phase;
    }

    // Secondary k's sync trigger fires `latency` master samples after the master's trigger
    auto secondary_time = [&](size_t k, uint64_t j) {
        return (long double)master_trigger + secondaries[k].latency +
               ((long double)j - secondary_trigger[k] + phase[k]) / rate[k];
    };

    long double t0 = -10;
    long double t1 = (long double)kSamples + 10;
    for (size_t k = 0; k < count; k++) {
        t0 = std::min(t0, secondary_time(k, 0) - 10);
        t1 = std::max(t1, secondary_time(k, secondary_samples[k]) + 10);
    }
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

    std::vector<std::vector<long double> > secondary_reference(count, reference);
    for (size_t k = 0; k < count; k++) {
        if (secondaries[k].unrelated) {
            const Signal other = {RandomGaps, s.reference.period};
            secondary_reference[k] = make_edges(other, t0, t1, rng);
        }
    }

    LogicStackingConfig config;
    config.enabled = true;
    config.master_handle = kMaster;
    config.master_reference_channel = kReferenceChannel;
    config.reference_alignment_enabled = s.align;
    config.drift_correction_enabled = s.drift;
    config.reference_edge_mode = s.edge_mode;
    for (size_t k = 0; k < count; k++) {
        LogicStackingSecondary secondary;
        secondary.handle = kSecondary + (ds_device_handle)k;
        secondary.sync_channel = 0;
        secondary.manual_shift_ps = secondaries[k].manual_shift_ps;
        secondary.reference_channel = kReferenceChannel;
        config.secondaries.push_back(secondary);
    }

    LogicStackingMerger merger;
    merger.reset(config, kSamples, kSamplerate, s.instant);

    {
        std::vector<uint64_t> master_words =
            sample(master_reference, data, kSamples, [](uint64_t i) { return (long double)i; });
        feed(merger, kMaster, master_words);
    }
    for (size_t k = 0; k < count; k++) {
        std::vector<uint64_t> secondary_words =
            sample(secondary_reference[k], data, secondary_samples[k],
                   [&](uint64_t j) { return secondary_time(k, j); });
        feed(merger, kSecondary + (ds_device_handle)k, secondary_words);
    }

    ds_trigger_pos trigger;
    memset(&trigger, 0, sizeof(trigger));
    trigger.status = 1;
    trigger.real_pos = (uint32_t)master_trigger;
    merger.set_trigger(kMaster, trigger);
    merger.mark_end(kMaster);
    for (size_t k = 0; k < count; k++) {
        trigger.real_pos = (uint32_t)secondary_trigger[k];
        merger.set_trigger(kSecondary + (ds_device_handle)k, trigger);
        merger.mark_end(kSecondary + (ds_device_handle)k);
    }

    std::vector<LogicStackingChannel> visible(count + 1);
    for (size_t a = 0; a <= count; a++) {
        visible[a].analyzer = (int)a;
        visible[a].physical_index = kDataChannel;
        visible[a].global_index = LogicStackingConfig::global_channel_index((int)a, kDataChannel);
    }

    LogicSnapshot snapshot;
    Result result;
    result.merged = merger.complete() && merger.flush_to_snapshot(&snapshot, channel_list, visible);
    result.warning = merger.warning();
    if (result.merged) {
        for (size_t k = 0; k < count; k++)
            result.errors.push_back(data_errors(snapshot.words(), visible.size(), k + 1));
    }

    return result;
}

int64_t max_abs_error(const Errors &errors,
                      int64_t first = std::numeric_limits<int64_t>::min(),
                      int64_t last = std::numeric_limits<int64_t>::max())
{
    int64_t worst = errors.empty() ? std::numeric_limits<int64_t>::max() : 0;
    for (const std::pair<int64_t, int64_t> &e : errors) {
        if (e.first >= first && e.first <= last)
            worst = std::max<int64_t>(worst, std::llabs(e.second));
    }
    return worst;
}

// Worst error over all secondaries
int64_t max_abs_error(const Result &r,
                      int64_t first = std::numeric_limits<int64_t>::min(),
                      int64_t last = std::numeric_limits<int64_t>::max())
{
    int64_t worst = r.errors.empty() ? std::numeric_limits<int64_t>::max() : 0;
    for (const Errors &errors : r.errors)
        worst = std::max(worst, max_abs_error(errors, first, last));
    return worst;
}

// Secondary k is analyzer A{k+2}
std::string label(size_t k)
{
    return "A" + std::to_string(k + 2);
}

double reported_ppm(const QString &warning, size_t k = 0)
{
    const QRegularExpressionMatch match =
        QRegularExpression(QString::fromStdString(label(k)) + " drift (-?[0-9.]+) ppm").match(warning);
    return match.hasMatch() ? match.captured(1).toDouble() : NAN;
}

bool mentions(const Result &r, const std::string &text)
{
    return r.warning.contains(QString::fromStdString(text));
}

int occurrences(const Result &r, const std::string &text)
{
    return r.warning.count(QString::fromStdString(text));
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
        const std::vector<Secondary> secondaries = s.secondaries();
        for (size_t k = 0; k < secondaries.size(); k++) {
            check(mentions(r, "drift correction applied: " + label(k)),
                  describe(s, r, ("drift correction applied to " + label(k)).c_str()));
            // The merger reports master relative to secondary
            check(std::fabs(reported_ppm(r.warning, k) + secondaries[k].ppm) < 0.2,
                  describe(s, r, ("drift estimate of " + label(k)).c_str()));
        }
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
        check(mentions(r, "drift correction skipped for A2: not enough matching reference edges"),
              describe(s, r, "drift correction skipped"));
        check(max_abs_error(r) <= 1, describe(s, r, "data aligned over the whole capture"));
    }

    {
        Scenario s("unrelated reference signals");
        s.unrelated = true;
        const Result r = run(s);
        check(mentions(r, "reference alignment skipped for A2: reference edge timing is inconsistent"),
              describe(s, r, "alignment rejected as inconsistent"));
        check(mentions(r, "drift correction skipped for A2: reference edge timing is inconsistent"),
              describe(s, r, "drift correction rejected as inconsistent"));
        check(!mentions(r, "applied"), describe(s, r, "nothing applied"));
    }

    {
        Scenario s("reference without edges");
        s.reference = {Constant, 0};
        const Result r = run(s);
        check(mentions(r, "reference alignment skipped for A2: not enough matching reference edges"),
              describe(s, r, "alignment reports missing edges"));
        check(mentions(r, "drift correction skipped for A2: not enough matching reference edges"),
              describe(s, r, "drift correction reports missing edges"));
    }

    {
        Scenario s("implausible drift");
        s.ppm = 3000;
        const Result r = run(s);
        check(!mentions(r, "drift correction applied"), describe(s, r, "drift correction not applied"));
    }

    {
        // Without a reference each secondary is aligned by its own trigger position
        Scenario s("four analyzers, trigger alignment only");
        s.ppm = 0;
        s.phase = 0;
        s.align = false;
        s.drift = false;
        s.more.resize(2);
        s.more[0].ppm = 0;
        s.more[0].phase = 0;
        s.more[1].ppm = 0;
        s.more[1].phase = 0;
        check_aligned(s, 0);
    }

    // Four analyzers: every secondary has its own drift, trigger latency and
    // sample phase, and is aligned to the master on its own
    Scenario four("four analyzers, alignment and drift");
    four.ppm = 45;
    four.latency = 4;
    four.more.resize(2);
    four.more[0].ppm = -55;
    four.more[0].latency = 7;
    four.more[1].ppm = 80;
    four.more[1].latency = 2;
    four.more[1].phase = 0.25;
    check_aligned(four);

    Scenario four_drift("four analyzers, drift only with manual alignment");
    four_drift.reference = {Clock, 250};
    four_drift.align = false;
    four_drift.ppm = -35;
    four_drift.latency = 4;
    four_drift.manual_shift_ps = 16000;
    four_drift.more.resize(2);
    four_drift.more[0].ppm = 20;
    four_drift.more[0].latency = 6;
    four_drift.more[0].manual_shift_ps = 24000;
    four_drift.more[1].ppm = 65;
    four_drift.more[1].latency = 3;
    four_drift.more[1].manual_shift_ps = 12000;
    check_aligned(four_drift);

    {
        // A miswired reference on A3 must not affect A2 or A4
        Scenario s("one secondary with an unrelated reference");
        s.latency = 4;
        s.more.resize(2);
        s.more[0].unrelated = true;
        s.more[1].ppm = -40;
        s.more[1].latency = 5;
        const Result r = run(s);
        check(r.merged, describe(s, r, "merge succeeds"));
        check(mentions(r, "reference alignment skipped for A3: reference edge timing is inconsistent"),
              describe(s, r, "A3 alignment rejected"));
        check(mentions(r, "drift correction skipped for A3: reference edge timing is inconsistent"),
              describe(s, r, "A3 drift correction rejected"));
        check(mentions(r, "drift correction applied: A2") && mentions(r, "drift correction applied: A4"),
              describe(s, r, "A2 and A4 corrected"));
        check(r.errors.size() == 3 && max_abs_error(r.errors[0]) <= 1 && max_abs_error(r.errors[2]) <= 1,
              describe(s, r, "A2 and A4 aligned"));
        // Every analyzer samples the same data signal, so only A3's uncorrected
        // drift (30 ppm, about 250 samples at the capture ends) shows that its
        // columns really come from A3
        check(r.errors.size() == 3 && max_abs_error(r.errors[1]) > 50,
              describe(s, r, "A3 keeps its drift"));
    }

    {
        // Instant captures report the skipped trigger alignment once, not per secondary
        Scenario s("instant capture with three secondaries");
        s.instant = true;
        s.align = false;
        s.drift = false;
        s.more.resize(2);
        const Result r = run(s);
        check(r.merged, describe(s, r, "merge succeeds"));
        check(occurrences(r, "Instant capture used manual alignment only") == 1,
              describe(s, r, "instant warning once"));
        check(!mentions(r, "Sync trigger"), describe(s, r, "no missing trigger warning"));
    }

    {
        // The merge waits for every analyzer's END
        LogicStackingConfig config;
        config.enabled = true;
        config.master_handle = kMaster;
        config.secondaries.resize(2);
        config.secondaries[0].handle = kSecondary;
        config.secondaries[1].handle = kSecondary + 1;
        LogicStackingMerger merger;
        merger.reset(config, kSamples, kSamplerate, false);
        check(!merger.mark_end(kSecondary + 2), "END from an unknown analyzer is ignored");
        merger.mark_end(kSecondary + 1);
        merger.mark_end(kMaster);
        check(!merger.complete(), "merge waits for the remaining secondary");
        merger.mark_end(kSecondary);
        check(merger.complete(), "merge completes once every analyzer ended");

        LogicStackingConfig duplicate = config;
        duplicate.secondaries[1].handle = kSecondary;
        check(!duplicate.is_valid(), "the same analyzer can't be stacked twice");
        check(duplicate.has_channel_model(), "duplicate handles keep the channel model");
        check(config.has_same_channel_model(duplicate), "handles are not part of the channel model");
        LogicStackingConfig more = config;
        more.secondaries.push_back(more.secondaries[0]);
        check(!config.has_same_channel_model(more), "adding an analyzer changes the channel model");

        // A profile that saved a disabled setup must not look like a model change
        LogicStackingConfig off;
        LogicStackingConfig saved_off = more;
        saved_off.enabled = false;
        saved_off.secondaries[0].sync_channel = 5;
        check(off.has_same_channel_model(saved_off) && saved_off.has_same_channel_model(off),
              "disabled setups share the (empty) channel model");
        check(!off.has_same_channel_model(config), "enabling stacking changes the channel model");
    }

    if (failures == 0)
        std::cout << "All stacking merger tests passed\n";

    return failures == 0 ? 0 : 1;
}
