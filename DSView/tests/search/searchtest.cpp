/*
 * This file is part of DSView, licensed under GPLv2 or later.
 *
 * Tests for the logic data search (pv/search): value and time parsing,
 * the JSON form, a few worked examples, and the engine against a
 * brute-force reference that follows doc/search.md sample by sample.
 */

#include "../../pv/search/searchengine.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <QJsonDocument>

using namespace pv::search;

namespace {

int failures = 0;

#define CHECK(cond) do { if (!(cond)) { failures++; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

using Levels = std::vector<std::vector<bool>>;

//------------------------------------------------------------- fake data

class FakeSource : public SampleSource
{
public:
    explicit FakeSource(const Levels &levels, double rate = 1) : _lv(levels), _rate(rate)
    {
        _n = levels.empty() ? 0 : levels[0].size();
        _changes.resize(levels.size());
        for (size_t ch = 0; ch < levels.size(); ch++){
            for (uint64_t t = 1; t < _n; t++){
                if (levels[ch][t] != levels[ch][t - 1])
                    _changes[ch].push_back(t);
            }
        }
    }

    uint64_t sample_count() override { return _n; }
    double samplerate() override { return _rate; }
    bool has_channel(int ch) override { return ch >= 0 && ch < (int)_lv.size(); }

    bool level(int ch, uint64_t t) override
    {
        if (gone())
            return false;
        if (t >= _n)
            bad_calls++;
        return _lv[ch][t];
    }

    bool next_change(int ch, uint64_t pos, bool level, uint64_t last, uint64_t &found) override
    {
        if (gone())
            return false;
        if (pos >= _n || _lv[ch][pos] != level || last >= _n)
            bad_calls++;
        auto it = std::upper_bound(_changes[ch].begin(), _changes[ch].end(), pos);
        if (it == _changes[ch].end() || *it > last)
            return false;
        found = *it;
        return true;
    }

    bool failed() override { return _failed; }

    // After this many level/next_change calls, the data is "gone": calls
    // return nothing useful and failed() is true.
    void fail_after(long calls) { _fail_after = calls; _calls = 0; _failed = false; }

    int bad_calls = 0;

private:
    bool gone()
    {
        if (_fail_after >= 0 && ++_calls > _fail_after)
            _failed = true;
        return _failed;
    }

    long _fail_after = -1;
    long _calls = 0;
    bool _failed = false;
    const Levels &_lv;
    double _rate;
    uint64_t _n;
    std::vector<std::vector<uint64_t>> _changes;
};

//------------------------------------------------------------- reference

bool req_ok(Req r, bool p, bool c)
{
    switch (r){
    case Req::Any:    return true;
    case Req::Low:    return !c;
    case Req::High:   return c;
    case Req::Rise:   return !p && c;
    case Req::Fall:   return p && !c;
    case Req::Change: return p != c;
    }
    return false;
}

struct Reference
{
    const Levels &lv;
    int64_t n;
    std::map<QString, std::vector<int>> buses;

    std::vector<int> bits_of(const Term &term) const
    {
        if (!term.bits.empty())
            return term.bits;
        return buses.at(term.bus);
    }

    bool bit(const std::vector<int> &bits, size_t i, int64_t t) const
    {
        return bits[i] >= 0 && lv[bits[i]][t];
    }

    bool term_has_edge(const Term &term) const
    {
        if (term.is_channel()){
            Req r;
            parse_channel_value(term.value, r);
            return is_edge(r);
        }
        BusValue v;
        parse_bus_value(term.value, (int)bits_of(term).size(), v);
        return v.has_edge();
    }

    bool pattern_has_edge(const Pattern &p) const
    {
        for (const Term &t : p){
            if (term_has_edge(t))
                return true;
        }
        return false;
    }

    // At t, with t - 1 as the sample before (t itself at 0).
    bool term_ok(const Term &term, int64_t t) const
    {
        const int64_t p = t > 0 ? t - 1 : t;
        if (term.is_channel()){
            Req r;
            parse_channel_value(term.value, r);
            return req_ok(r, lv[term.channel][p], lv[term.channel][t]);
        }
        const std::vector<int> bits = bits_of(term);
        BusValue v;
        parse_bus_value(term.value, (int)bits.size(), v);
        uint64_t value = 0;
        for (size_t i = 0; i < bits.size(); i++){
            if (bit(bits, i, t))
                value |= 1ULL << i;
        }
        switch (v.op){
        case ValueOp::Pattern:
            for (size_t i = 0; i < bits.size(); i++){
                if (!req_ok(v.bits[i], bit(bits, i, p), bit(bits, i, t)))
                    return false;
            }
            return true;
        case ValueOp::Changes:
            for (size_t i = 0; i < bits.size(); i++){
                if (bits[i] >= 0 && bit(bits, i, p) != bit(bits, i, t))
                    return true;
            }
            return false;
        case ValueOp::NotEqual:     return (value & v.mask) != (v.value & v.mask);
        case ValueOp::Less:         return value < v.value;
        case ValueOp::LessEqual:    return value <= v.value;
        case ValueOp::Greater:      return value > v.value;
        case ValueOp::GreaterEqual: return value >= v.value;
        case ValueOp::Range:        return value >= v.value && value <= v.high;
        }
        return false;
    }

    bool pattern_ok(const Pattern &p, int64_t t) const
    {
        for (const Term &term : p){
            if (!term_ok(term, t))
                return false;
        }
        return true;
    }

    std::vector<char> occurrences(const Patterns &ps) const
    {
        std::vector<char> occ(n, 0);
        for (int64_t t = 1; t < n; t++){
            for (const Pattern &p : ps){
                const bool ok = pattern_has_edge(p) ? pattern_ok(p, t)
                                                    : pattern_ok(p, t) && !pattern_ok(p, t - 1);
                if (ok){
                    occ[t] = 1;
                    break;
                }
            }
        }
        return occ;
    }

    std::vector<char> holds(const Patterns &ps) const
    {
        std::vector<char> h(n, 0);
        for (int64_t t = 0; t < n; t++){
            for (const Pattern &p : ps){
                if (pattern_ok(p, t)){
                    h[t] = 1;
                    break;
                }
            }
        }
        return h;
    }
};

// Occurrence lookups over a 0/1 array.
struct Index
{
    std::vector<int64_t> prefix, next, prev;

    explicit Index(const std::vector<char> &v)
    {
        const int64_t n = (int64_t)v.size();
        prefix.assign(n + 1, 0);
        next.assign(n + 1, n);
        prev.assign(n, -1);
        for (int64_t t = 0; t < n; t++)
            prefix[t + 1] = prefix[t] + v[t];
        for (int64_t t = n - 1; t >= 0; t--)
            next[t] = v[t] ? t : next[t + 1];
        for (int64_t t = 0; t < n; t++)
            prev[t] = v[t] ? t : (t > 0 ? prev[t - 1] : -1);
    }

    int64_t count(int64_t a, int64_t b) const { return prefix[b + 1] - prefix[a]; }
};

struct RefCandidate
{
    int64_t s, e, end_ref, after;
    bool su, eu;
};

int64_t ceil_samples(const QString &text, double rate)
{
    TimeValue tv;
    parse_time(text, tv);
    int64_t s = 0;
    time_to_samples_ceil(tv, rate, s);
    return s;
}

int64_t floor_samples(const QString &text, double rate)
{
    TimeValue tv;
    parse_time(text, tv);
    int64_t s = 0;
    time_to_samples_floor(tv, rate, s);
    return s;
}

std::vector<Match> reference_search(const Query &q, const Reference &ref, double rate,
                                    uint64_t from, uint64_t to, uint64_t limit)
{
    const int64_t n = ref.n;
    const int64_t last = n - 1;
    std::vector<RefCandidate> cands;

    if (!q.state){
        const std::vector<char> occ = ref.occurrences(q.find);
        for (int64_t t = std::max<int64_t>(1, from); t < (int64_t)std::min<uint64_t>(to, n); t++){
            if (occ[t])
                cands.push_back(RefCandidate{t, t, t, t + 1, false, false});
        }
    }
    else {
        const std::vector<char> h = ref.holds(q.find);
        const bool has_min = !q.min_duration.isEmpty();
        const bool has_max = !q.max_duration.isEmpty();
        const int64_t dmin = has_min ? ceil_samples(q.min_duration, rate) : 0;
        const int64_t dmax = has_max ? floor_samples(q.max_duration, rate) : 0;
        for (int64_t t = 0; t < n;){
            if (!h[t]){
                t++;
                continue;
            }
            int64_t e = t;
            while (e < n && h[e])
                e++;
            RefCandidate k{t, e, e, e, t == 0, e == n};
            const int64_t d = e - t;
            bool ok;
            if (!k.su && !k.eu)
                ok = (!has_min || d >= dmin) && (!has_max || d <= dmax);
            else
                ok = !has_max && (!has_min || d >= dmin);
            if (ok && (uint64_t)t >= from && (uint64_t)t < to)
                cands.push_back(k);
            t = e;
        }
    }

    struct CondData {
        std::vector<char> subject;
        std::vector<char> boundary;
    };
    std::vector<CondData> data;
    for (const Condition &c : q.conditions){
        CondData d;
        d.subject = c.state ? ref.holds(c.subject) : ref.occurrences(c.subject);
        if (c.window == WindowKind::Until || c.window == WindowKind::Since)
            d.boundary = ref.occurrences(c.boundary);
        data.push_back(d);
    }
    std::vector<Index> subj, bound;
    for (const CondData &d : data){
        subj.emplace_back(d.subject);
        bound.emplace_back(d.boundary.empty() ? std::vector<char>(n, 0) : d.boundary);
    }

    std::vector<Match> out;
    for (const RefCandidate &k : cands){
        Match m;
        m.start = k.s;
        m.end = k.e;
        m.start_unknown = k.su;
        m.end_unknown = k.eu;
        bool pass = true;

        for (size_t i = 0; i < q.conditions.size() && pass; i++){
            const Condition &c = q.conditions[i];
            int64_t a = 0, b = 0, r = 0;
            bool cut = false, undecided = false;

            switch (c.window){
            case WindowKind::Start:
                undecided = k.su;
                r = k.s;
                a = r + ceil_samples(c.from, rate);
                b = r + floor_samples(c.to, rate);
                break;
            case WindowKind::End:
                undecided = k.eu;
                r = k.end_ref;
                a = r + ceil_samples(c.from, rate);
                b = r + floor_samples(c.to, rate);
                break;
            case WindowKind::During:
                r = k.s;
                a = k.s;
                b = k.e - 1;
                cut = k.su || k.eu;
                break;
            case WindowKind::Until: {
                undecided = k.eu;
                r = k.end_ref;
                a = k.after;
                const int64_t g = k.after < n ? bound[i].next[k.after] : n;
                if (g < n)
                    b = g - 1;
                else {
                    b = last;
                    cut = true;
                }
                break;
            }
            case WindowKind::Since: {
                undecided = k.su;
                r = k.s;
                const int64_t g = k.s > 0 ? bound[i].prev[k.s - 1] : -1;
                if (g >= 0)
                    a = g + 1;
                else {
                    a = 0;
                    cut = true;
                }
                b = k.s - 1;
                break;
            }
            }
            if (undecided){
                pass = false;
                break;
            }
            const int64_t lo = c.state ? 0 : 1;
            if (a < lo){
                a = lo;
                cut = true;
            }
            if (b > last){
                b = last;
                cut = true;
            }

            if (c.state){
                bool any = false, all = true;
                if (a <= b){
                    const int64_t cnt = subj[i].count(a, b);
                    any = cnt > 0;
                    all = cnt == b - a + 1;
                }
                switch (c.quantifier){
                case Quantifier::Sometime:   pass = any; break;
                case Quantifier::Never:      pass = !cut && !any; break;
                case Quantifier::Throughout: pass = !cut && all; break;
                default: pass = false; break;
                }
                continue;
            }

            int64_t cnt = 0;
            ConditionValue &v = m.values[i];
            if (a <= b){
                cnt = subj[i].count(a, b);
                if (cnt){
                    int64_t before = -1, after = -1;
                    const int64_t pb = std::min(r, b);
                    if (pb >= 0 && pb < n && subj[i].prev[pb] >= a)
                        before = subj[i].prev[pb];
                    const int64_t pa = std::max(r, a);
                    if (pa <= b && subj[i].next[pa] <= b)
                        after = subj[i].next[pa];
                    v.has_delta = true;
                    if (before >= 0 && (after < 0 || r - before <= after - r))
                        v.delta = before - r;
                    else
                        v.delta = after - r;
                }
            }
            v.count = cnt;
            uint64_t want = 0;
            if (has_count(c.quantifier))
                parse_count(c.count, want);
            switch (c.quantifier){
            case Quantifier::Occurs:  pass = cnt >= 1; break;
            case Quantifier::None:    pass = !cut && cnt == 0; break;
            case Quantifier::AtLeast: pass = (uint64_t)cnt >= want; break;
            case Quantifier::AtMost:  pass = !cut && (uint64_t)cnt <= want; break;
            case Quantifier::Exactly: pass = !cut && (uint64_t)cnt == want; break;
            default: pass = false; break;
            }
        }

        if (pass){
            out.push_back(m);
            if (out.size() >= limit)
                break;
        }
    }
    return out;
}

bool same(const Match &a, const Match &b, size_t conditions)
{
    if (a.start != b.start || a.end != b.end || a.start_unknown != b.start_unknown
        || a.end_unknown != b.end_unknown)
        return false;
    for (size_t i = 0; i < conditions; i++){
        if (a.values[i].count != b.values[i].count || a.values[i].has_delta != b.values[i].has_delta)
            return false;
        if (a.values[i].has_delta && a.values[i].delta != b.values[i].delta)
            return false;
    }
    return true;
}

std::vector<Match> engine_search(const Program &p, SampleSource &src, uint64_t from, uint64_t to,
                                 uint64_t limit, RunResult *result = nullptr)
{
    std::vector<Match> out;
    std::atomic<bool> cancel(false);
    RunResult r = run(p, src, from, to, limit, cancel, nullptr,
                      [&](const Match &m){ out.push_back(m); });
    if (result)
        *result = r;
    return out;
}

Term ch(int channel, const char *value)
{
    Term t;
    t.channel = channel;
    t.value = QString::fromLatin1(value);
    return t;
}

Term bus(const char *name, const char *value)
{
    Term t;
    t.bus = QString::fromLatin1(name);
    t.value = QString::fromLatin1(value);
    return t;
}

//------------------------------------------------------------- parsing

void test_parsing()
{
    BusValue v;
    CHECK(parse_bus_value("0x21XX", 16, v) == ErrorCode::None);
    CHECK(v.op == ValueOp::Pattern && v.bits.size() == 16);
    for (int i = 0; i < 8; i++)
        CHECK(v.bits[i] == Req::Any);
    CHECK(v.bits[8] == Req::High && v.bits[9] == Req::Low && v.bits[13] == Req::High && v.bits[15] == Req::Low);

    BusValue w;
    CHECK(parse_bus_value("$21xx", 16, w) == ErrorCode::None && w.bits == v.bits);
    CHECK(parse_bus_value("21 XX", 16, w) == ErrorCode::None && w.bits == v.bits);
    CHECK(parse_bus_value("= 0x21XX", 16, w) == ErrorCode::None && w.bits == v.bits);

    CHECK(parse_bus_value("0b10XR", 4, v) == ErrorCode::None);
    CHECK(v.bits[0] == Req::Rise && v.bits[1] == Req::Any && v.bits[2] == Req::Low && v.bits[3] == Req::High);
    CHECK(v.has_edge());
    CHECK(parse_bus_value("0b1R", 8, v) == ErrorCode::None);
    CHECK(v.bits[0] == Req::Rise && v.bits[1] == Req::High && v.bits[7] == Req::Low);
    CHECK(parse_bus_value("0b1111_FFFF", 8, v) == ErrorCode::None && v.bits[0] == Req::Fall && v.bits[7] == Req::High);
    CHECK(parse_bus_value("0bC", 1, v) == ErrorCode::None && v.bits[0] == Req::Change);

    CHECK(parse_bus_value("0x1FF", 8, v) == ErrorCode::ValueTooWide);
    CHECK(parse_bus_value("0xXFF", 8, v) == ErrorCode::None);
    CHECK(parse_bus_value("0x3FF", 10, v) == ErrorCode::None);
    CHECK(parse_bus_value("0x7FF", 10, v) == ErrorCode::ValueTooWide);
    CHECK(parse_bus_value("0b0001", 2, v) == ErrorCode::None);
    CHECK(parse_bus_value("0b1R1", 2, v) == ErrorCode::ValueTooWide);
    CHECK(parse_bus_value("1_0000", 17, v) == ErrorCode::None && v.bits[16] == Req::High);
    CHECK(parse_bus_value("1_0000", 16, v) == ErrorCode::ValueTooWide);

    CHECK(parse_bus_value("!= 0xFF", 8, v) == ErrorCode::None);
    CHECK(v.op == ValueOp::NotEqual && v.value == 0xFF && v.mask == 0xFF);
    CHECK(parse_bus_value("!=0x1X", 8, v) == ErrorCode::None && v.value == 0x10 && v.mask == 0xF0);
    CHECK(parse_bus_value("!=0b1R", 8, v) == ErrorCode::EdgeNotAllowed);
    CHECK(parse_bus_value("!=XX", 8, v) == ErrorCode::DontCareNotAllowed);
    CHECK(parse_bus_value("<0x8000", 16, v) == ErrorCode::None && v.op == ValueOp::Less && v.value == 0x8000);
    CHECK(parse_bus_value("<= $10", 16, v) == ErrorCode::None && v.op == ValueOp::LessEqual && v.value == 0x10);
    CHECK(parse_bus_value(">0b11", 16, v) == ErrorCode::None && v.op == ValueOp::Greater && v.value == 3);
    CHECK(parse_bus_value(">=FF", 16, v) == ErrorCode::None && v.op == ValueOp::GreaterEqual);
    CHECK(parse_bus_value("<0x1X", 16, v) == ErrorCode::DontCareNotAllowed);
    CHECK(parse_bus_value("<0b1R", 16, v) == ErrorCode::EdgeNotAllowed);
    CHECK(parse_bus_value("0x2100..0x213F", 16, v) == ErrorCode::None);
    CHECK(v.op == ValueOp::Range && v.value == 0x2100 && v.high == 0x213F);
    CHECK(parse_bus_value("2140..2100", 16, v) == ErrorCode::BadRange);
    CHECK(parse_bus_value("..2100", 16, v) == ErrorCode::BadValue);
    CHECK(parse_bus_value("changes", 16, v) == ErrorCode::None && v.op == ValueOp::Changes && v.has_edge());
    CHECK(parse_bus_value("Changes", 16, v) == ErrorCode::None);

    CHECK(parse_bus_value("", 8, v) == ErrorCode::EmptyValue);
    CHECK(parse_bus_value("  ", 8, v) == ErrorCode::EmptyValue);
    CHECK(parse_bus_value("0xG", 8, v) == ErrorCode::BadValue);
    CHECK(parse_bus_value("0x", 8, v) == ErrorCode::BadValue);
    CHECK(parse_bus_value("0b", 8, v) == ErrorCode::BadValue);
    CHECK(parse_bus_value("0b102", 8, v) == ErrorCode::BadValue);
    CHECK(parse_bus_value("!=", 8, v) == ErrorCode::BadValue);
    CHECK(parse_bus_value("0x1", 65, v) == ErrorCode::BusTooWide);
    CHECK(parse_bus_value("0xFFFFFFFFFFFFFFFF", 64, v) == ErrorCode::None);
    CHECK(parse_bus_value("<0xFFFFFFFFFFFFFFFF", 64, v) == ErrorCode::None && v.value == ~0ULL);

    Req r;
    CHECK(parse_channel_value("r", r) == ErrorCode::None && r == Req::Rise);
    CHECK(parse_channel_value(" F ", r) == ErrorCode::None && r == Req::Fall);
    CHECK(parse_channel_value("X", r) == ErrorCode::None && r == Req::Any);
    CHECK(parse_channel_value("c", r) == ErrorCode::None && r == Req::Change);
    CHECK(parse_channel_value("2", r) == ErrorCode::BadChannelValue);
    CHECK(parse_channel_value("", r) == ErrorCode::EmptyValue);

    TimeValue t;
    CHECK(parse_time("100ns", t) == ErrorCode::None && !t.samples && std::abs(t.value - 1e-7) < 1e-18);
    CHECK(parse_time("-2.5us", t) == ErrorCode::None && std::abs(t.value + 2.5e-6) < 1e-18);
    CHECK(parse_time("2µs", t) == ErrorCode::None && std::abs(t.value - 2e-6) < 1e-18);
    CHECK(parse_time("3 ms", t) == ErrorCode::None && std::abs(t.value - 3e-3) < 1e-15);
    CHECK(parse_time("1e3ns", t) == ErrorCode::None && std::abs(t.value - 1e-6) < 1e-18);
    CHECK(parse_time("1s", t) == ErrorCode::None && t.value == 1);
    CHECK(parse_time("40sa", t) == ErrorCode::None && t.samples && t.value == 40);
    CHECK(parse_time("-3 samples", t) == ErrorCode::None && t.samples && t.value == -3);
    CHECK(parse_time("0", t) == ErrorCode::None && t.value == 0);
    CHECK(parse_time("5", t) == ErrorCode::MissingUnit);
    CHECK(parse_time("1.5sa", t) == ErrorCode::BadTime);
    CHECK(parse_time("abc", t) == ErrorCode::BadTime);
    CHECK(parse_time("", t) == ErrorCode::BadTime);
    CHECK(parse_time("5 parsecs", t) == ErrorCode::BadTime);

    int64_t s;
    parse_time("30ns", t);
    CHECK(time_to_samples_ceil(t, 250e6, s) && s == 8);
    CHECK(time_to_samples_floor(t, 250e6, s) && s == 7);
    parse_time("20ns", t);
    CHECK(time_to_samples_ceil(t, 250e6, s) && s == 5);
    CHECK(time_to_samples_floor(t, 250e6, s) && s == 5);
    parse_time("-10ns", t);
    CHECK(time_to_samples_ceil(t, 250e6, s) && s == -2);
    CHECK(time_to_samples_floor(t, 250e6, s) && s == -3);
    CHECK(!time_to_samples_ceil(t, 0, s));
    parse_time("7sa", t);
    CHECK(time_to_samples_floor(t, 0, s) && s == 7);

    uint64_t n;
    CHECK(parse_count("12", n) == ErrorCode::None && n == 12);
    CHECK(parse_count("-1", n) == ErrorCode::BadCount);
    CHECK(parse_count("", n) == ErrorCode::BadCount);
}

void test_compile_errors()
{
    std::map<QString, std::vector<int>> buses;
    buses["A"] = {0, 1, -1, 3};
    BusResolver resolve = [&](const QString &name, std::vector<int> &bits){
        auto it = buses.find(name);
        if (it == buses.end())
            return false;
        bits = it->second;
        return true;
    };
    ChannelCheck has = [](int c){ return c >= 0 && c < 4; };

    auto compile_code = [&](const Query &q, double rate = 1){
        Program p;
        Error e;
        compile(q, resolve, has, rate, p, e);
        return e.code;
    };

    Query q;
    CHECK(compile_code(q) == ErrorCode::NoPattern);
    q.find = {{}};
    CHECK(compile_code(q) == ErrorCode::EmptyPattern);
    q.find = {{ch(0, "X")}};
    CHECK(compile_code(q) == ErrorCode::NoCondition);
    q.find = {{ch(7, "1")}};
    CHECK(compile_code(q) == ErrorCode::UnknownChannel);
    q.find = {{ch(0, "1"), ch(0, "0")}};
    CHECK(compile_code(q) == ErrorCode::ConflictingTerms);
    q.find = {{ch(0, "R"), ch(0, "1")}};
    CHECK(compile_code(q) == ErrorCode::None);
    q.find = {{bus("B", "1")}};
    CHECK(compile_code(q) == ErrorCode::UnknownBus);
    q.find = {{bus("A", "0b0100")}};
    CHECK(compile_code(q) == ErrorCode::UnmappedBit);
    q.find = {{bus("A", "0b0X11")}};
    CHECK(compile_code(q) == ErrorCode::None);
    q.find = {{bus("A", "0b1011"), ch(1, "0")}};
    CHECK(compile_code(q) == ErrorCode::ConflictingTerms);

    q.state = true;
    q.find = {{ch(0, "R")}};
    CHECK(compile_code(q) == ErrorCode::EdgeInState);
    q.find = {{bus("A", "changes")}};
    CHECK(compile_code(q) == ErrorCode::EdgeInState);
    q.find = {{ch(0, "1")}};
    q.min_duration = "10ns";
    q.max_duration = "5ns";
    CHECK(compile_code(q, 0) == ErrorCode::BadDuration);
    CHECK(compile_code(q, 1e9) == ErrorCode::BadDuration);
    q.max_duration = "5";
    CHECK(compile_code(q) == ErrorCode::MissingUnit);
    q.min_duration.clear();
    q.max_duration.clear();
    CHECK(compile_code(q) == ErrorCode::None);

    Condition c;
    c.subject = {{ch(1, "C")}};
    c.from = "0";
    c.to = "1us";
    q.conditions = {c};
    CHECK(compile_code(q, 0) == ErrorCode::None);
    CHECK(compile_code(q, 1e6) == ErrorCode::None);
    c.from = "2us";
    q.conditions = {c};
    CHECK(compile_code(q, 0) == ErrorCode::EmptyWindow);
    c.from = "1ns";
    c.to = "2ns";
    q.conditions = {c};
    CHECK(compile_code(q, 100e6) == ErrorCode::EmptyWindow);
    c.from = "0";
    c.to = "1us";
    c.quantifier = Quantifier::AtLeast;
    c.count = "x";
    q.conditions = {c};
    CHECK(compile_code(q) == ErrorCode::BadCount);
    c.count = "2";
    c.window = WindowKind::Until;
    q.conditions = {c};
    CHECK(compile_code(q) == ErrorCode::NoPattern);
    c.boundary = {{ch(2, "F")}};
    q.conditions = {c};
    CHECK(compile_code(q) == ErrorCode::None);
    c.quantifier = Quantifier::Sometime;
    q.conditions = {c};
    CHECK(compile_code(q) == ErrorCode::BadQuery);

    q.state = false;
    q.find = {{ch(0, "R")}};
    c.quantifier = Quantifier::Occurs;
    c.window = WindowKind::During;
    q.conditions = {c};
    CHECK(compile_code(q) == ErrorCode::DuringNeedsState);
    q.conditions.assign(9, Condition());
    CHECK(compile_code(q) == ErrorCode::TooManyConditions);
}

void test_json()
{
    Query q;
    q.state = true;
    q.find = {{ch(3, "0"), bus("Addr", "0x21XX")}, {ch(1, "1")}};
    q.min_duration = "10ns";
    Condition c;
    c.subject = {{ch(2, "C")}};
    c.quantifier = Quantifier::AtMost;
    c.count = "4";
    c.window = WindowKind::Since;
    Term t;
    t.bits = {0, -1, 2};
    t.value = "0b1X0";
    c.boundary = {{t}};
    q.conditions.push_back(c);
    Condition d;
    d.state = true;
    d.subject = {{ch(0, "1")}};
    d.quantifier = Quantifier::Throughout;
    d.window = WindowKind::End;
    d.from = "-5ns";
    d.to = "0";
    q.conditions.push_back(d);

    const QJsonObject o = to_json(q);
    Query back;
    QString error;
    CHECK(from_json(o, back, error));
    CHECK(to_json(back) == o);
    CHECK(back.conditions.size() == 2 && back.conditions[0].boundary[0][0].bits.size() == 3);

    QJsonObject bad = o;
    bad["find"] = "nothing";
    CHECK(!from_json(bad, back, error));
    const QJsonDocument doc = QJsonDocument::fromJson(
        "{\"patterns\":[[{\"channel\":1,\"value\":\"F\"}]],"
        "\"conditions\":[{\"patterns\":[[{\"channel\":2,\"value\":\"C\"}]],"
        "\"quantifier\":\"at_least\",\"count\":3,\"window\":\"start\",\"from\":\"0\",\"to\":\"5sa\"}]}");
    CHECK(from_json(doc.object(), back, error));
    CHECK(!back.state && back.conditions.size() == 1 && back.conditions[0].count == "3");
}

//------------------------------------------------------------- worked examples

Levels make_levels(int channels, int n, bool initial = true)
{
    return Levels(channels, std::vector<bool>(n, initial));
}

void set_range(Levels &lv, int ch, int from, int to, bool level)
{
    for (int t = from; t < to; t++)
        lv[ch][t] = level;
}

void test_examples()
{
    std::atomic<bool> cancel(false);
    auto no_buses = [](const QString &, std::vector<int> &){ return false; };

    // A /WR write with no /ROMSEL activity in the next 2 us, at 250 MHz.
    {
        Levels lv = make_levels(2, 3000);
        set_range(lv, 0, 100, 120, false);
        set_range(lv, 0, 1000, 1020, false);
        set_range(lv, 0, 2000, 2020, false);
        set_range(lv, 1, 1050, 2700, false);
        FakeSource src(lv, 250e6);

        Query q;
        q.find = {{ch(0, "F")}};
        Condition c;
        c.subject = {{ch(1, "C")}};
        c.quantifier = Quantifier::None;
        c.from = "0";
        c.to = "2us";
        q.conditions = {c};
        Program p;
        Error e;
        CHECK(compile(q, no_buses, nullptr, 250e6, p, e));
        CHECK(p.conditions[0].to == 500);
        std::vector<Match> m = engine_search(p, src, 0, ~0ULL, ~0ULL);
        CHECK(m.size() == 2 && m[0].start == 100 && m[1].start == 2000);

        // Edges within 0..2 us: the ROMSEL fall 50 samples after the second write.
        q.conditions[0].quantifier = Quantifier::Occurs;
        CHECK(compile(q, no_buses, nullptr, 250e6, p, e));
        m = engine_search(p, src, 0, ~0ULL, ~0ULL);
        CHECK(m.size() == 1 && m[0].start == 1000 && m[0].values[0].delta == 50);
    }

    // Low pulses of at most 3 samples; the last one is cut by the end.
    {
        Levels lv = make_levels(1, 60);
        set_range(lv, 0, 10, 12, false);
        set_range(lv, 0, 20, 25, false);
        set_range(lv, 0, 30, 33, false);
        set_range(lv, 0, 58, 60, false);
        FakeSource src(lv);

        Query q;
        q.state = true;
        q.find = {{ch(0, "0")}};
        q.max_duration = "3sa";
        Program p;
        Error e;
        CHECK(compile(q, no_buses, nullptr, 1, p, e));
        std::vector<Match> m = engine_search(p, src, 0, ~0ULL, ~0ULL);
        CHECK(m.size() == 2 && m[0].start == 10 && m[0].end == 12 && m[1].start == 30);

        q.max_duration.clear();
        q.min_duration = "2sa";
        CHECK(compile(q, no_buses, nullptr, 1, p, e));
        m = engine_search(p, src, 0, ~0ULL, ~0ULL);
        CHECK(m.size() == 4 && m[3].start == 58 && m[3].end_unknown);
    }

    // Reads since the previous write: at least 3.
    {
        Levels lv = make_levels(2, 80);
        for (int t : {10, 20, 30, 40, 50, 60, 70})
            set_range(lv, 0, t, t + 2, false);
        set_range(lv, 1, 25, 27, false);
        FakeSource src(lv);

        Query q;
        q.find = {{ch(0, "F")}};
        Condition c;
        c.subject = {{ch(0, "F")}};
        c.quantifier = Quantifier::AtLeast;
        c.count = "3";
        c.window = WindowKind::Since;
        c.boundary = {{ch(1, "F")}};
        q.conditions = {c};
        Program p;
        Error e;
        CHECK(compile(q, no_buses, nullptr, 1, p, e));
        std::vector<Match> m = engine_search(p, src, 0, ~0ULL, ~0ULL);
        CHECK(m.size() == 2 && m[0].start == 60 && m[0].values[0].count == 3
              && m[0].values[0].delta == -10 && m[1].values[0].count == 4);
    }

    // A level pattern matches where it becomes true, once per region.
    {
        Levels lv = make_levels(2, 40, false);
        set_range(lv, 0, 5, 30, true);
        set_range(lv, 1, 10, 12, true);
        FakeSource src(lv);
        Query q;
        q.find = {{ch(0, "1"), ch(1, "0")}};
        Program p;
        Error e;
        CHECK(compile(q, no_buses, nullptr, 1, p, e));
        std::vector<Match> m = engine_search(p, src, 0, ~0ULL, ~0ULL);
        CHECK(m.size() == 2 && m[0].start == 5 && m[1].start == 12);
    }
}

//------------------------------------------------------------- random

struct Random
{
    std::mt19937_64 rng;
    explicit Random(uint64_t seed) : rng(seed) {}
    int below(int n) { return (int)(rng() % (uint64_t)n); }
    bool chance(int percent) { return below(100) < percent; }
};

Levels random_levels(Random &r, int channels, int n, bool sparse)
{
    static const int dense[] = {1, 2, 3, 6, 15, 40, 200};
    static const int few[] = {1, 20, 3000, 40000, 120000, 120000, 120000};
    Levels lv(channels, std::vector<bool>(n));
    for (int ch = 0; ch < channels; ch++){
        const int maxlen = (sparse ? few : dense)[r.below(7)];
        bool l = r.chance(50);
        const bool constant = r.chance(5);
        for (int t = 0; t < n;){
            const int len = 1 + r.below(maxlen);
            for (int i = 0; i < len && t < n; i++, t++)
                lv[ch][t] = l;
            if (!constant)
                l = !l;
        }
    }
    return lv;
}

const int Channels = 6;

QString bits_to_text(const std::vector<Req> &bits, bool hex)
{
    QString s;
    if (hex){
        for (int i = ((int)bits.size() + 3) / 4 - 1; i >= 0; i--){
            bool any = false;
            int v = 0;
            for (int b = 0; b < 4; b++){
                const int k = i * 4 + b;
                if (k < (int)bits.size() && bits[k] == Req::Any)
                    any = true;
                if (k < (int)bits.size() && bits[k] == Req::High)
                    v |= 1 << b;
            }
            s += any ? QStringLiteral("X") : QString::number(v, 16).toUpper();
        }
        return QStringLiteral("0x") + s;
    }
    for (int i = (int)bits.size() - 1; i >= 0; i--){
        switch (bits[i]){
        case Req::Any:    s += 'X'; break;
        case Req::Low:    s += '0'; break;
        case Req::High:   s += '1'; break;
        case Req::Rise:   s += 'R'; break;
        case Req::Fall:   s += 'F'; break;
        case Req::Change: s += 'C'; break;
        }
    }
    return QStringLiteral("0b") + s;
}

QString random_bus_value(Random &r, int width, bool state)
{
    const uint64_t top = 1ULL << width;
    switch (r.below(state ? 4 : 5)){
    case 0: {
        // Whole nibbles of X keep hex possible.
        std::vector<Req> bits(width);
        bool edge = false;
        const bool nibble_x = r.chance(30);
        for (int i = 0; i < width; i++){
            const int k = r.below(state ? 3 : 6);
            bits[i] = k == 0 ? Req::Any : k == 1 ? Req::Low : k == 2 ? Req::High
                    : k == 3 ? Req::Rise : k == 4 ? Req::Fall : Req::Change;
            if (nibble_x)
                bits[i] = Req::Any;
            edge |= is_edge(bits[i]);
        }
        bool hexable = !edge;
        for (int i = 0; i < width && hexable; i += 4){
            bool any = false, level = false;
            for (int b = i; b < std::min(width, i + 4); b++){
                any |= bits[b] == Req::Any;
                level |= bits[b] != Req::Any;
            }
            hexable = !(any && level);
        }
        return bits_to_text(bits, hexable && r.chance(60));
    }
    case 1: {
        const uint64_t v = r.rng() % top;
        return QStringLiteral("!= 0x") + QString::number(v, 16);
    }
    case 2: {
        static const char *const ops[] = {"<", "<=", ">", ">="};
        const uint64_t v = r.rng() % top;
        return QString::fromLatin1(ops[r.below(4)]) + QStringLiteral("0x") + QString::number(v, 16);
    }
    case 3: {
        uint64_t a = r.rng() % top, b = r.rng() % top;
        if (a > b)
            std::swap(a, b);
        return QStringLiteral("%1..%2").arg(a, 0, 16).arg(b, 0, 16);
    }
    default:
        return QStringLiteral("changes");
    }
}

Pattern random_pattern(Random &r, bool state, int max_terms = 3)
{
    static const char *const levels[] = {"0", "1", "X"};
    static const char *const any[] = {"0", "1", "X", "R", "F", "C", "r", "f"};
    Pattern p;
    const int terms = 1 + r.below(max_terms);
    for (int i = 0; i < terms; i++){
        if (r.chance(65)){
            const char *v = state ? levels[r.below(3)] : any[r.below(8)];
            p.push_back(ch(r.below(Channels), v));
        }
        else if (r.chance(80)){
            Term t;
            t.bus = QStringLiteral("B");
            t.value = random_bus_value(r, 4, state);
            p.push_back(t);
        }
        else {
            Term t;
            t.bits = {r.below(Channels), -1, r.below(Channels)};
            if (t.bits[0] == t.bits[2])
                t.bits.pop_back();
            t.value = random_bus_value(r, (int)t.bits.size(), state);
            p.push_back(t);
        }
    }
    return p;
}

Patterns random_patterns(Random &r, bool state)
{
    Patterns ps;
    const int n = r.chance(70) ? 1 : 2;
    for (int i = 0; i < n; i++)
        ps.push_back(random_pattern(r, state));
    return ps;
}

QString random_offset(Random &r, int range)
{
    return QStringLiteral("%1sa").arg(r.below(2 * range + 1) - range);
}

Query random_query(Random &r, int range)
{
    Query q;
    q.state = r.chance(40);
    q.find = random_patterns(r, q.state);
    if (q.state){
        if (r.chance(40))
            q.min_duration = QStringLiteral("%1sa").arg(r.below(8));
        if (r.chance(40))
            q.max_duration = QStringLiteral("%1sa").arg(r.below(30));
    }
    const int conds = r.below(4);
    for (int i = 0; i < conds; i++){
        Condition c;
        c.state = r.chance(30);
        c.subject = random_patterns(r, c.state);
        if (c.state){
            static const Quantifier qs[] = {Quantifier::Sometime, Quantifier::Never, Quantifier::Throughout};
            c.quantifier = qs[r.below(3)];
        }
        else {
            static const Quantifier qs[] = {Quantifier::Occurs, Quantifier::None, Quantifier::AtLeast,
                                            Quantifier::AtMost, Quantifier::Exactly};
            c.quantifier = qs[r.below(5)];
            c.count = QString::number(r.below(4));
        }
        static const WindowKind ws[] = {WindowKind::Start, WindowKind::End, WindowKind::During,
                                        WindowKind::Until, WindowKind::Since};
        c.window = ws[r.below(5)];
        if (c.window == WindowKind::During && !q.state)
            c.window = WindowKind::Start;
        int a = r.below(2 * range + 1) - range, b = r.below(2 * range + 1) - range;
        if (a > b)
            std::swap(a, b);
        if (r.chance(20))
            a = 0;
        c.from = QStringLiteral("%1sa").arg(a);
        c.to = QStringLiteral("%1sa").arg(b);
        if (c.window == WindowKind::Until || c.window == WindowKind::Since){
            c.boundary.clear();
            const int n = r.chance(80) ? 1 : 2;
            for (int k = 0; k < n; k++)
                c.boundary.push_back(random_pattern(r, false, 2));
        }
        q.conditions.push_back(c);
    }
    return q;
}

void print_query(const Query &q)
{
    std::printf("%s\n", QJsonDocument(to_json(q)).toJson(QJsonDocument::Compact).constData());
}

void print_match(const char *label, const Match &m, size_t conds)
{
    std::printf("  %s start=%llu end=%llu su=%d eu=%d", label, (unsigned long long)m.start,
                (unsigned long long)m.end, m.start_unknown, m.end_unknown);
    for (size_t i = 0; i < conds; i++)
        std::printf(" [c%zu n=%llu d=%s%lld]", i, (unsigned long long)m.values[i].count,
                    m.values[i].has_delta ? "" : "-", (long long)m.values[i].delta);
    std::printf("\n");
}

// Returns the number of queries compared.
int random_round(Random &r, int queries, int n, int range, bool sparse = false)
{
    const Levels lv = random_levels(r, Channels, n, sparse);
    FakeSource src(lv);
    Reference ref{lv, n, {}};
    ref.buses[QStringLiteral("B")] = r.chance(50) ? std::vector<int>{2, 3, 4, 5}
                                                 : std::vector<int>{2, -1, 4, 5};
    BusResolver resolve = [&](const QString &name, std::vector<int> &bits){
        auto it = ref.buses.find(name);
        if (it == ref.buses.end())
            return false;
        bits = it->second;
        return true;
    };
    ChannelCheck has = [](int c){ return c >= 0 && c < Channels; };

    int compared = 0;
    for (int i = 0; i < queries; i++){
        Query q;
        Program p;
        Error e;
        int tries = 0;
        do {
            q = random_query(r, range);
        } while (!compile(q, resolve, has, 1, p, e) && ++tries < 50);
        if (e)
            continue;

        uint64_t from = 0, to = ~0ULL, limit = ~0ULL;
        if (r.chance(25)){
            from = r.below(n);
            to = from + 1 + r.below(n);
        }
        if (r.chance(15))
            limit = 1 + r.below(5);

        RunResult res;
        const std::vector<Match> got = engine_search(p, src, from, to, limit, &res);
        const std::vector<Match> want = reference_search(q, ref, 1, from, to, limit);
        compared++;

        bool ok = got.size() == want.size() && res.status != RunStatus::Cancelled;
        for (size_t k = 0; ok && k < got.size(); k++)
            ok = same(got[k], want[k], q.conditions.size());
        if (ok && limit != ~0ULL && got.size() == limit)
            ok = res.status == RunStatus::Limit && res.next == got.back().start + 1;
        if (!ok){
            failures++;
            std::printf("FAIL random query (n=%d from=%llu to=%llu limit=%llu): ", n,
                        (unsigned long long)from, (unsigned long long)to, (unsigned long long)limit);
            print_query(q);
            std::printf("  engine %zu matches, reference %zu\n", got.size(), want.size());
            for (size_t k = 0; k < std::max(got.size(), want.size()); k++){
                if (k < got.size() && k < want.size() && same(got[k], want[k], q.conditions.size()))
                    continue;
                if (k < got.size())
                    print_match("engine   ", got[k], q.conditions.size());
                if (k < want.size())
                    print_match("reference", want[k], q.conditions.size());
                break;
            }
            if (failures > 20)
                return compared;
        }
    }
    if (src.bad_calls){
        failures++;
        std::printf("FAIL %d source calls with a wrong level or position\n", src.bad_calls);
    }
    return compared;
}

// A source that fails part way, and a cancel from the sink: the run must
// say so, and what it reported must be a prefix of the full result.
void test_interrupted()
{
    Random r(777);
    int checked = 0;
    for (int round = 0; round < 40; round++){
        const int n = 2000 + r.below(3000);
        const Levels lv = random_levels(r, Channels, n, false);
        FakeSource src(lv);
        BusResolver resolve = [](const QString &, std::vector<int> &bits){
            bits = {2, 3, 4, 5};
            return true;
        };
        ChannelCheck has = [](int c){ return c >= 0 && c < Channels; };
        for (int i = 0; i < 10; i++){
            Query q;
            Program p;
            Error e;
            do {
                q = random_query(r, 12);
            } while (!compile(q, resolve, has, 1, p, e));

            src.fail_after(-1);
            const std::vector<Match> full = engine_search(p, src, 0, ~0ULL, ~0ULL);

            // Fail somewhere in the run.
            src.fail_after(r.below(20000));
            RunResult res;
            const std::vector<Match> got = engine_search(p, src, 0, ~0ULL, ~0ULL, &res);
            bool ok = got.size() <= full.size();
            for (size_t k = 0; ok && k < got.size(); k++)
                ok = same(got[k], full[k], q.conditions.size());
            if (src.failed())
                ok = ok && res.status == RunStatus::DataChanged;
            else
                ok = ok && res.status == RunStatus::Done && got.size() == full.size();
            if (!ok){
                failures++;
                std::printf("FAIL interrupted run: %zu of %zu matches, status %d, failed %d: ",
                            got.size(), full.size(), (int)res.status, src.failed());
                print_query(q);
            }

            // Cancel after the first match.
            src.fail_after(-1);
            std::atomic<bool> cancel(false);
            std::vector<Match> some;
            res = run(p, src, 0, ~0ULL, ~0ULL, cancel, nullptr, [&](const Match &m){
                some.push_back(m);
                cancel = true;
            });
            ok = some.size() <= full.size() && (full.empty() || !some.empty());
            for (size_t k = 0; ok && k < some.size(); k++)
                ok = same(some[k], full[k], q.conditions.size());
            ok = ok && (res.status == RunStatus::Cancelled || some.size() == full.size());
            if (!ok){
                failures++;
                std::printf("FAIL cancelled run: %zu of %zu matches, status %d: ",
                            some.size(), full.size(), (int)res.status);
                print_query(q);
            }
            checked++;
        }
    }
    std::printf("%d interrupted runs checked\n", checked);
}

} // namespace

int main()
{
    test_parsing();
    test_compile_errors();
    test_json();
    test_examples();
    test_interrupted();

    Random r(12345);
    int compared = 0;
    for (int round = 0; round < 300 && failures <= 20; round++)
        compared += random_round(r, 20, 20 + r.below(400), 12);
    // Long captures, so streams skip ahead instead of walking.
    for (int round = 0; round < 6 && failures <= 20; round++)
        compared += random_round(r, 15, 150000 + r.below(100000), 40, true);

    std::printf("%d random queries compared\n", compared);
    if (failures){
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("all search tests passed\n");
    return 0;
}
