/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#include "searchengine.h"

#include <algorithm>
#include <deque>
#include <map>

namespace pv {
namespace search {

//------------------------------------------------------------- compile

namespace {

// Two requirements on one channel at once.
bool merge_req(Req a, Req b, Req &out)
{
    if (a == b || b == Req::Any){ out = a; return true; }
    if (a == Req::Any){ out = b; return true; }

    auto pair = [&](Req x, Req y){ return (a == x && b == y) || (a == y && b == x); };

    if (pair(Req::Rise, Req::High) || pair(Req::Rise, Req::Change) || pair(Req::Change, Req::High)){
        out = Req::Rise;
        return true;
    }
    if (pair(Req::Fall, Req::Low) || pair(Req::Fall, Req::Change) || pair(Req::Change, Req::Low)){
        out = Req::Fall;
        return true;
    }
    return false;
}

struct Compiler
{
    const BusResolver &buses;
    const ChannelCheck &has_channel;
    Error &error;
    int condition = -1;
    ErrorPart part = ErrorPart::Subject;

    bool fail(ErrorCode code, const QString &detail = QString())
    {
        error.code = code;
        error.condition = condition;
        error.part = part;
        error.detail = detail;
        return false;
    }

    bool channel_ok(int ch)
    {
        if (ch < 0 || ch >= MaxChannels)
            return false;
        return !has_channel || has_channel(ch);
    }

    bool pattern(const Pattern &p, bool state, PatternSpec &out)
    {
        if (p.empty())
            return fail(ErrorCode::EmptyPattern);

        std::map<int, Req> needs;
        PatternSpec spec;
        int referenced = 0;

        auto add = [&](int ch, Req r){
            auto it = needs.find(ch);
            if (it == needs.end()){
                needs[ch] = r;
                return true;
            }
            Req m;
            if (!merge_req(it->second, r, m))
                return false;
            it->second = m;
            return true;
        };

        for (const Term &t : p){
            if (t.is_channel()){
                if (!channel_ok(t.channel))
                    return fail(ErrorCode::UnknownChannel, QString::number(t.channel));
                Req r;
                const ErrorCode ec = parse_channel_value(t.value, r);
                if (ec != ErrorCode::None)
                    return fail(ec, ec == ErrorCode::EmptyValue ? QString::number(t.channel) : t.value);
                if (r != Req::Any && !add(t.channel, r))
                    return fail(ErrorCode::ConflictingTerms, QString::number(t.channel));
                continue;
            }

            std::vector<int> bits = t.bits;
            const QString name = t.bus.isEmpty() ? QStringLiteral("bus") : t.bus;
            if (bits.empty()){
                if (!buses || !buses(t.bus, bits) || bits.empty())
                    return fail(ErrorCode::UnknownBus, t.bus);
            }
            if ((int)bits.size() > MaxBusWidth)
                return fail(ErrorCode::BusTooWide, name);
            for (int b : bits){
                if (b < -1 || (b >= 0 && !channel_ok(b)))
                    return fail(ErrorCode::UnknownChannel, QString::number(b));
            }

            BusValue v;
            const ErrorCode ec = parse_bus_value(t.value, (int)bits.size(), v);
            if (ec != ErrorCode::None)
                return fail(ec, ec == ErrorCode::EmptyValue ? name : t.value);

            switch (v.op){
            case ValueOp::Pattern:
                for (size_t i = 0; i < bits.size(); i++){
                    const Req r = v.bits[i];
                    if (r == Req::Any)
                        continue;
                    if (bits[i] < 0){
                        if (r == Req::Low)
                            continue;
                        return fail(ErrorCode::UnmappedBit, t.value);
                    }
                    if (!add(bits[i], r))
                        return fail(ErrorCode::ConflictingTerms, QString::number(bits[i]));
                }
                break;

            case ValueOp::Changes: {
                std::vector<int> chs;
                for (int b : bits){
                    if (b >= 0 && std::find(chs.begin(), chs.end(), b) == chs.end())
                        chs.push_back(b);
                }
                if (chs.empty())
                    return fail(ErrorCode::UnmappedBit, t.value);
                referenced += (int)chs.size();
                spec.changes.push_back(chs);
                spec.has_edge = true;
                break;
            }

            default: {
                PatternSpec::Compare c;
                c.bits = bits;
                c.op = v.op;
                c.value = v.op == ValueOp::NotEqual ? (v.value & v.mask) : v.value;
                c.mask = v.mask;
                c.high = v.high;
                for (int b : bits)
                    referenced += b >= 0;
                spec.compares.push_back(c);
                break;
            }
            }
        }

        for (auto &n : needs){
            spec.needs.push_back(n);
            if (is_edge(n.second))
                spec.has_edge = true;
        }
        referenced += (int)spec.needs.size();

        if (state && spec.has_edge)
            return fail(ErrorCode::EdgeInState);
        if (referenced == 0)
            return fail(ErrorCode::NoCondition);

        out = spec;
        return true;
    }

    bool event(const Patterns &ps, bool state, EventSpec &out)
    {
        if (ps.empty())
            return fail(ErrorCode::NoPattern);
        out.clear();
        for (const Pattern &p : ps){
            PatternSpec spec;
            if (!pattern(p, state, spec))
                return false;
            out.push_back(spec);
        }
        return true;
    }

    // In validation mode (no sample rate) only the syntax is checked.
    bool time(const QString &text, double samplerate, bool ceil, int64_t &samples, TimeValue &tv)
    {
        const ErrorCode ec = parse_time(text, tv);
        if (ec != ErrorCode::None)
            return fail(ec, text);
        samples = 0;
        if (samplerate <= 0 && !tv.samples)
            return true;
        const bool ok = ceil ? time_to_samples_ceil(tv, samplerate, samples)
                             : time_to_samples_floor(tv, samplerate, samples);
        if (!ok)
            return fail(ErrorCode::NoSamplerate);
        return true;
    }
};

// Whether a > b can be told without a sample rate.
bool later(const TimeValue &a, const TimeValue &b)
{
    if (a.samples != b.samples && a.value != 0 && b.value != 0)
        return false;
    return a.value > b.value;
}

} // namespace

bool compile(const Query &query, const BusResolver &buses, const ChannelCheck &has_channel,
             double samplerate, Program &program, Error &error)
{
    error = Error();
    Compiler c{buses, has_channel, error};
    Program prog;
    const bool convert = samplerate > 0;

    prog.state = query.state;
    if (!c.event(query.find, query.state, prog.find))
        return false;

    if (query.state){
        c.part = ErrorPart::Duration;
        TimeValue tmin, tmax;
        int64_t smin = 0, smax = 0;
        prog.has_min = !query.min_duration.trimmed().isEmpty();
        prog.has_max = !query.max_duration.trimmed().isEmpty();
        if (prog.has_min && !c.time(query.min_duration, samplerate, true, smin, tmin))
            return false;
        if (prog.has_max && !c.time(query.max_duration, samplerate, false, smax, tmax))
            return false;
        if ((prog.has_min && tmin.value < 0) || (prog.has_max && tmax.value < 0))
            return c.fail(ErrorCode::BadDuration);
        prog.min_duration = (uint64_t)std::max<int64_t>(smin, 0);
        prog.max_duration = (uint64_t)std::max<int64_t>(smax, 0);
        if (prog.has_min && prog.has_max){
            if (convert ? prog.min_duration > prog.max_duration : later(tmin, tmax))
                return c.fail(ErrorCode::BadDuration);
        }
    }

    if (query.conditions.size() > (size_t)MaxConditions){
        c.part = ErrorPart::Subject;
        return c.fail(ErrorCode::TooManyConditions);
    }

    for (size_t i = 0; i < query.conditions.size(); i++){
        const Condition &qc = query.conditions[i];
        ConditionSpec cs;
        c.condition = (int)i;
        c.part = ErrorPart::Subject;

        cs.state = qc.state;
        if (!c.event(qc.subject, qc.state, cs.subject))
            return false;
        if (is_event_quantifier(qc.quantifier) == qc.state)
            return c.fail(ErrorCode::BadQuery, quantifier_key(qc.quantifier));
        cs.quantifier = qc.quantifier;

        if (has_count(qc.quantifier)){
            c.part = ErrorPart::Count;
            const ErrorCode ec = parse_count(qc.count, cs.count);
            if (ec != ErrorCode::None)
                return c.fail(ec, qc.count);
        }

        cs.window = qc.window;
        c.part = ErrorPart::Window;
        switch (qc.window){
        case WindowKind::Start:
        case WindowKind::End: {
            TimeValue tf, tt;
            if (!c.time(qc.from, samplerate, true, cs.from, tf))
                return false;
            if (!c.time(qc.to, samplerate, false, cs.to, tt))
                return false;
            if (convert || (tf.samples && tt.samples) ? cs.from > cs.to : later(tf, tt))
                return c.fail(ErrorCode::EmptyWindow);
            break;
        }
        case WindowKind::During:
            if (!query.state)
                return c.fail(ErrorCode::DuringNeedsState);
            break;
        case WindowKind::Until:
        case WindowKind::Since:
            c.part = ErrorPart::Boundary;
            if (!c.event(qc.boundary, false, cs.boundary))
                return false;
            break;
        }
        prog.conditions.push_back(cs);
    }

    program = prog;
    return true;
}

//------------------------------------------------------------- run

namespace {

// Walking forward this far beats starting over at a later sample.
const uint64_t SeekGap = 1 << 16;
// The most occurrences a condition keeps for its windows.
const size_t MaxRetained = 1 << 24;

struct Bits
{
    uint64_t w[4] = {0, 0, 0, 0};

    inline void set(int i) { w[i >> 6] |= 1ULL << (i & 63); }
    inline void flip(int i) { w[i >> 6] ^= 1ULL << (i & 63); }
    inline bool test(int i) const { return (w[i >> 6] >> (i & 63)) & 1; }
    inline void clear() { w[0] = w[1] = w[2] = w[3] = 0; }

    inline bool subset_of(const Bits &o) const
    {
        return !(w[0] & ~o.w[0]) && !(w[1] & ~o.w[1]) && !(w[2] & ~o.w[2]) && !(w[3] & ~o.w[3]);
    }
    inline bool intersects(const Bits &o) const
    {
        return (w[0] & o.w[0]) || (w[1] & o.w[1]) || (w[2] & o.w[2]) || (w[3] & o.w[3]);
    }
    inline Bits and_not(const Bits &o) const
    {
        Bits r;
        for (int i = 0; i < 4; i++)
            r.w[i] = w[i] & ~o.w[i];
        return r;
    }
};

struct Context
{
    SampleSource &src;
    const std::atomic<bool> &cancel;
    uint64_t last;
    uint64_t ticks = 0;
    bool stopped = false;
    bool too_much = false;

    Context(SampleSource &s, const std::atomic<bool> &c, uint64_t l) : src(s), cancel(c), last(l) {}

    inline bool check()
    {
        if ((++ticks & 0x3FF) == 0 && (cancel.load(std::memory_order_relaxed) || src.failed()))
            stopped = true;
        return !stopped;
    }
};

// The change points of some channels, in order.
class Walker
{
public:
    Walker(Context &ctx, const std::vector<int> &channels) : _ctx(ctx), _channels(channels) {}

    // Levels at pos; next() goes on from there.
    void reset(uint64_t pos)
    {
        _heap.clear();
        cur.clear();
        changed.clear();
        for (size_t i = 0; i < _channels.size(); i++){
            const bool l = _ctx.src.level(_channels[i], pos);
            if (l)
                cur.set((int)i);
            uint64_t n;
            if (_ctx.src.next_change(_channels[i], pos, l, _ctx.last, n))
                _heap.push_back(std::make_pair(n, (int)i));
        }
        std::make_heap(_heap.begin(), _heap.end(), Later());
        prev = cur;
    }

    bool next(uint64_t &t)
    {
        if (!_ctx.check() || _heap.empty())
            return false;

        t = _heap.front().first;
        prev = cur;
        changed.clear();
        while (!_heap.empty() && _heap.front().first == t){
            std::pop_heap(_heap.begin(), _heap.end(), Later());
            const int i = _heap.back().second;
            _heap.pop_back();
            cur.flip(i);
            changed.set(i);
            uint64_t n;
            if (_ctx.src.next_change(_channels[i], t, cur.test(i), _ctx.last, n)){
                _heap.push_back(std::make_pair(n, i));
                std::push_heap(_heap.begin(), _heap.end(), Later());
            }
        }
        return true;
    }

    Bits cur, prev, changed;

private:
    struct Later {
        bool operator()(const std::pair<uint64_t, int> &a, const std::pair<uint64_t, int> &b) const
        {
            return a.first > b.first;
        }
    };

    Context &_ctx;
    std::vector<int> _channels;
    std::vector<std::pair<uint64_t, int>> _heap;
};

// An event spec, compiled against the channels one stream walks (driving)
// and the ones it only looks at when the edges are there (probes).
class Matcher
{
public:
    Matcher(Context &ctx, const EventSpec &spec, bool state) : _ctx(ctx)
    {
        std::vector<int> dpos(MaxChannels, -1), ppos(MaxChannels, -1);
        auto drive = [&](int ch){
            if (ch >= 0 && dpos[ch] < 0){
                dpos[ch] = (int)driving.size();
                driving.push_back(ch);
            }
        };

        for (const PatternSpec &p : spec){
            const bool all = state || !p.has_edge;
            for (const auto &n : p.needs){
                if (all || is_edge(n.second))
                    drive(n.first);
            }
            for (const auto &g : p.changes)
                for (int ch : g)
                    drive(ch);
            if (all){
                for (const auto &c : p.compares)
                    for (int b : c.bits)
                        drive(b);
            }
        }

        auto probe = [&](int ch){
            if (ch >= 0 && dpos[ch] < 0 && ppos[ch] < 0){
                ppos[ch] = (int)probes.size();
                probes.push_back(ch);
            }
        };
        for (const PatternSpec &p : spec){
            for (const auto &n : p.needs)
                probe(n.first);
            for (const auto &c : p.compares)
                for (int b : c.bits)
                    probe(b);
        }
        _stamp.assign(probes.size(), 0);
        _value.assign(probes.size(), false);

        for (const PatternSpec &p : spec){
            Pat c;
            c.has_edge = p.has_edge;
            for (const auto &n : p.needs){
                const int d = dpos[n.first];
                switch (n.second){
                case Req::Rise:   c.rise.set(d); break;
                case Req::Fall:   c.fall.set(d); break;
                case Req::Change: c.change.set(d); break;
                case Req::High:
                case Req::Low:
                    if (d >= 0){
                        if (n.second == Req::High)
                            c.high.set(d);
                        else
                            c.low.set(d);
                    }
                    else
                        c.probe_levels.push_back(std::make_pair(ppos[n.first], n.second == Req::High));
                    break;
                default:
                    break;
                }
            }
            for (const auto &g : p.changes){
                Bits b;
                for (int ch : g)
                    b.set(dpos[ch]);
                c.changes.push_back(b);
            }
            for (const auto &cmp : p.compares){
                Cmp k;
                for (int b : cmp.bits){
                    if (b < 0)
                        k.bits.push_back(Ref{0, 0});
                    else if (dpos[b] >= 0)
                        k.bits.push_back(Ref{1, dpos[b]});
                    else
                        k.bits.push_back(Ref{2, ppos[b]});
                }
                k.op = cmp.op;
                k.value = cmp.value;
                k.mask = cmp.mask;
                k.high = cmp.high;
                c.compares.push_back(k);
            }
            _pats.push_back(c);
        }
    }

    // At a change point of the driving channels.
    bool occurs(const Walker &w, uint64_t t)
    {
        _gen++;
        const Bits rising = w.cur.and_not(w.prev);
        const Bits falling = w.prev.and_not(w.cur);

        for (const Pat &p : _pats){
            if (p.has_edge){
                if (!p.rise.subset_of(rising) || !p.fall.subset_of(falling)
                    || !p.change.subset_of(w.changed))
                    continue;
                bool all = true;
                for (const Bits &g : p.changes){
                    if (!g.intersects(w.changed)){
                        all = false;
                        break;
                    }
                }
                if (all && levels(p, w.cur, t))
                    return true;
            }
            else if (levels(p, w.cur, t) && !levels(p, w.prev, t - 1))
                return true;
        }
        return false;
    }

    // States have no probes.
    bool holds(const Bits &cur)
    {
        for (const Pat &p : _pats){
            if (levels(p, cur, 0))
                return true;
        }
        return false;
    }

    std::vector<int> driving;
    std::vector<int> probes;

private:
    struct Ref { int kind; int index; };     // 0: always 0, 1: driving, 2: probe
    struct Cmp {
        std::vector<Ref> bits;
        ValueOp op;
        uint64_t value, mask, high;
    };
    struct Pat {
        bool has_edge = false;
        Bits rise, fall, change, high, low;
        std::vector<Bits> changes;
        std::vector<std::pair<int, bool>> probe_levels;
        std::vector<Cmp> compares;
    };

    bool probe(int i, uint64_t t)
    {
        if (_stamp[i] != _gen){
            _value[i] = _ctx.src.level(probes[i], t);
            _stamp[i] = _gen;
        }
        return _value[i];
    }

    bool levels(const Pat &p, const Bits &cur, uint64_t t)
    {
        if (!p.high.subset_of(cur) || p.low.intersects(cur))
            return false;
        for (const auto &pl : p.probe_levels){
            if (probe(pl.first, t) != pl.second)
                return false;
        }
        for (const Cmp &c : p.compares){
            uint64_t v = 0;
            for (size_t i = 0; i < c.bits.size(); i++){
                const Ref &r = c.bits[i];
                const bool bit = r.kind == 1 ? cur.test(r.index) : r.kind == 2 ? probe(r.index, t) : false;
                if (bit)
                    v |= 1ULL << i;
            }
            bool ok = false;
            switch (c.op){
            case ValueOp::NotEqual:     ok = (v & c.mask) != c.value; break;
            case ValueOp::Less:         ok = v < c.value; break;
            case ValueOp::LessEqual:    ok = v <= c.value; break;
            case ValueOp::Greater:      ok = v > c.value; break;
            case ValueOp::GreaterEqual: ok = v >= c.value; break;
            case ValueOp::Range:        ok = v >= c.value && v <= c.high; break;
            default: break;
            }
            if (!ok)
                return false;
        }
        return true;
    }

    Context &_ctx;
    std::vector<Pat> _pats;
    std::vector<uint64_t> _stamp;
    std::vector<bool> _value;
    uint64_t _gen = 0;
};

// The occurrences of an event, in order.
class EventStream
{
public:
    EventStream(Context &ctx, const EventSpec &spec) :
        _ctx(ctx), _m(ctx, spec, false), _w(ctx, _m.driving) {}

    // Occurrences at p and after; there are none at sample 0.
    void seek(uint64_t p)
    {
        if (p < 1)
            p = 1;
        _started = true;
        _pending_valid = false;
        _done = false;
        _pos = p - 1;
        if (p > _ctx.last)
            _done = true;
        else
            _w.reset(p - 1);
    }

    inline bool started() const { return _started; }
    // The last sample looked at.
    inline uint64_t position() const { return _pos; }

    bool peek(uint64_t &t)
    {
        if (_pending_valid){
            t = _pending;
            return true;
        }
        if (_done)
            return false;
        uint64_t c;
        while (_w.next(c)){
            _pos = c;
            if (_m.occurs(_w, c)){
                _pending = c;
                _pending_valid = true;
                t = c;
                return true;
            }
        }
        _done = true;
        return false;
    }

    inline void pop() { _pending_valid = false; }

private:
    Context &_ctx;
    Matcher _m;
    Walker _w;
    bool _started = false;
    bool _done = true;
    bool _pending_valid = false;
    uint64_t _pending = 0;
    uint64_t _pos = 0;
};

struct Run
{
    uint64_t s;
    uint64_t e;         // the first sample after the run
    bool s_open;        // started before s (at s = 0: before the capture)
    bool e_open;        // holds to the end of the capture
};

// The runs where a state holds, in order.
class StateStream
{
public:
    StateStream(Context &ctx, const EventSpec &spec) :
        _ctx(ctx), _m(ctx, spec, true), _w(ctx, _m.driving) {}

    // Runs that reach p or later. One that holds at p - 1 (or at 0 for p
    // = 0) starts there, open.
    void seek(uint64_t p)
    {
        _started = true;
        _pending_valid = false;
        _done = false;
        if (p > _ctx.last){
            _done = true;
            _in = false;
            _pos = p;
            return;
        }
        const uint64_t q = p > 0 ? p - 1 : 0;
        _w.reset(q);
        _pos = q;
        _in = _m.holds(_w.cur);
        _run_s = q;
        _run_open = true;
    }

    inline bool started() const { return _started; }
    inline uint64_t position() const { return _pos; }

    bool peek(Run &r)
    {
        if (_pending_valid){
            r = _pending;
            return true;
        }
        if (_done)
            return false;
        uint64_t c;
        while (_w.next(c)){
            _pos = c;
            const bool h = _m.holds(_w.cur);
            if (h && !_in){
                _in = true;
                _run_s = c;
                _run_open = false;
            }
            else if (!h && _in){
                _in = false;
                _pending = Run{_run_s, c, _run_open, false};
                _pending_valid = true;
                r = _pending;
                return true;
            }
        }
        _done = true;
        if (_ctx.stopped || !_in)
            return false;
        _in = false;
        _pending = Run{_run_s, _ctx.last + 1, _run_open, true};
        _pending_valid = true;
        r = _pending;
        return true;
    }

    inline void pop() { _pending_valid = false; }

private:
    Context &_ctx;
    Matcher _m;
    Walker _w;
    bool _started = false;
    bool _done = true;
    bool _pending_valid = false;
    Run _pending{0, 0, false, false};
    bool _in = false;
    uint64_t _run_s = 0;
    bool _run_open = false;
    uint64_t _pos = 0;
};

struct Candidate
{
    uint64_t s;
    uint64_t e;         // events: = s; states: the first sample after
    uint64_t end_ref;   // the reference for "relative to the end"
    uint64_t after;     // the first sample after the match
    bool su, eu;        // start or end unknown
};

struct ConditionRun
{
    const ConditionSpec *spec = nullptr;
    std::unique_ptr<EventStream> events;
    std::unique_ptr<StateStream> states;
    std::unique_ptr<EventStream> boundary;
    std::deque<uint64_t> occ;
    std::deque<Run> runs;
    bool have_g = false;
    uint64_t last_g = 0;
};

// Windows only move forward from one candidate to the next, so what lies
// before a window is dropped.
bool ensure_events(Context &ctx, ConditionRun &c, uint64_t a, uint64_t b)
{
    std::deque<uint64_t> &occ = c.occ;
    EventStream &st = *c.events;

    while (!occ.empty() && occ.front() < a)
        occ.pop_front();
    if (occ.empty() && (!st.started() || st.position() + SeekGap < a))
        st.seek(a);

    uint64_t t;
    while (st.peek(t) && t < a)
        st.pop();
    while (st.peek(t) && t <= b){
        occ.push_back(t);
        st.pop();
        if (occ.size() > MaxRetained){
            ctx.too_much = true;
            ctx.stopped = true;
            return false;
        }
    }
    return !ctx.stopped;
}

bool ensure_runs(Context &ctx, ConditionRun &c, uint64_t a, uint64_t b)
{
    std::deque<Run> &runs = c.runs;
    StateStream &st = *c.states;

    while (!runs.empty() && runs.front().e <= a)
        runs.pop_front();
    if (runs.empty() && (!st.started() || st.position() + SeekGap < a))
        st.seek(a);

    Run r;
    while (st.peek(r) && r.e <= a)
        st.pop();
    while (st.peek(r) && r.s <= b){
        runs.push_back(r);
        st.pop();
        if (runs.size() > MaxRetained){
            ctx.too_much = true;
            ctx.stopped = true;
            return false;
        }
    }
    return !ctx.stopped;
}

bool evaluate(Context &ctx, ConditionRun &c, const Candidate &k, ConditionValue &value)
{
    const ConditionSpec &cs = *c.spec;
    const int64_t last = (int64_t)ctx.last;
    int64_t a = 0, b = 0, ref = 0;
    bool cut = false;

    switch (cs.window){
    case WindowKind::Start:
        if (k.su)
            return false;
        ref = (int64_t)k.s;
        a = ref + cs.from;
        b = ref + cs.to;
        break;

    case WindowKind::End:
        if (k.eu)
            return false;
        ref = (int64_t)k.end_ref;
        a = ref + cs.from;
        b = ref + cs.to;
        break;

    case WindowKind::During:
        ref = (int64_t)k.s;
        a = (int64_t)k.s;
        b = (int64_t)k.e - 1;
        cut = k.su || k.eu;
        break;

    case WindowKind::Until: {
        if (k.eu)
            return false;
        ref = (int64_t)k.end_ref;
        a = (int64_t)k.after;
        EventStream &g = *c.boundary;
        if (!g.started() || g.position() + SeekGap < k.after)
            g.seek(k.after);
        uint64_t t;
        bool found = false;
        while (g.peek(t)){
            if (t >= k.after){
                found = true;
                break;
            }
            g.pop();
        }
        if (ctx.stopped)
            return false;
        if (found)
            b = (int64_t)t - 1;
        else {
            b = last;
            cut = true;
        }
        break;
    }

    case WindowKind::Since: {
        if (k.su)
            return false;
        ref = (int64_t)k.s;
        EventStream &g = *c.boundary;
        if (!g.started())
            g.seek(1);
        uint64_t t;
        while (g.peek(t) && t < k.s){
            c.last_g = t;
            c.have_g = true;
            g.pop();
        }
        if (ctx.stopped)
            return false;
        if (c.have_g)
            a = (int64_t)c.last_g + 1;
        else {
            a = 0;
            cut = true;
        }
        b = (int64_t)k.s - 1;
        break;
    }
    }

    const int64_t lo = cs.state ? 0 : 1;
    if (a < lo){
        a = lo;
        cut = true;
    }
    if (b > last){
        b = last;
        cut = true;
    }

    if (cs.state){
        bool any = false, all = true;
        if (a <= b){
            if (!ensure_runs(ctx, c, (uint64_t)a, (uint64_t)b))
                return false;
            any = !c.runs.empty();
            all = any && c.runs.front().s <= (uint64_t)a && c.runs.front().e > (uint64_t)b;
        }
        switch (cs.quantifier){
        case Quantifier::Sometime:   return any;
        case Quantifier::Never:      return !cut && !any;
        case Quantifier::Throughout: return !cut && all;
        default:                     return false;
        }
    }

    uint64_t count = 0;
    if (a <= b){
        if (!ensure_events(ctx, c, (uint64_t)a, (uint64_t)b))
            return false;
        const auto lo_it = std::lower_bound(c.occ.begin(), c.occ.end(), (uint64_t)a);
        const auto hi_it = std::upper_bound(lo_it, c.occ.end(), (uint64_t)b);
        count = (uint64_t)(hi_it - lo_it);
        if (count){
            const auto r = std::lower_bound(lo_it, hi_it, (uint64_t)ref);
            const bool has_after = r != hi_it;
            const bool has_before = r != lo_it;
            const int64_t d_after = has_after ? (int64_t)*r - ref : 0;
            const int64_t d_before = has_before ? ref - (int64_t)*(r - 1) : 0;
            value.has_delta = true;
            if (has_before && (!has_after || d_before <= d_after))
                value.delta = -d_before;
            else
                value.delta = d_after;
        }
    }
    value.count = count;

    switch (cs.quantifier){
    case Quantifier::Occurs:  return count >= 1;
    case Quantifier::None:    return !cut && count == 0;
    case Quantifier::AtLeast: return count >= cs.count;
    case Quantifier::AtMost:  return !cut && count <= cs.count;
    case Quantifier::Exactly: return !cut && count == cs.count;
    default:                  return false;
    }
}

bool duration_ok(const Program &prog, const Candidate &k)
{
    const uint64_t d = k.e - k.s;
    if (!k.su && !k.eu)
        return (!prog.has_min || d >= prog.min_duration) && (!prog.has_max || d <= prog.max_duration);
    // Longer than it looks: only a lower bound can be sure.
    return !prog.has_max && (!prog.has_min || d >= prog.min_duration);
}

} // namespace

RunResult run(const Program &prog, SampleSource &source, uint64_t from, uint64_t to,
              uint64_t limit, const std::atomic<bool> &cancel, std::atomic<uint64_t> *progress,
              const std::function<void(const Match &)> &sink)
{
    RunResult res;
    const uint64_t n = source.sample_count();
    to = std::min(to, n);
    if (n == 0 || from >= to || limit == 0)
        return res;

    Context ctx(source, cancel, n - 1);

    std::unique_ptr<EventStream> anchor_events;
    std::unique_ptr<StateStream> anchor_states;
    if (prog.state){
        anchor_states.reset(new StateStream(ctx, prog.find));
        anchor_states->seek(from);
    }
    else {
        anchor_events.reset(new EventStream(ctx, prog.find));
        anchor_events->seek(std::max<uint64_t>(from, 1));
    }

    std::vector<ConditionRun> conds(prog.conditions.size());
    for (size_t i = 0; i < conds.size(); i++){
        const ConditionSpec &cs = prog.conditions[i];
        conds[i].spec = &cs;
        if (cs.state)
            conds[i].states.reset(new StateStream(ctx, cs.subject));
        else
            conds[i].events.reset(new EventStream(ctx, cs.subject));
        if (cs.window == WindowKind::Until || cs.window == WindowKind::Since)
            conds[i].boundary.reset(new EventStream(ctx, cs.boundary));
    }

    Match m;
    while (true){
        Candidate k;
        if (anchor_events){
            uint64_t t;
            if (!anchor_events->peek(t) || t >= to)
                break;
            anchor_events->pop();
            k = Candidate{t, t, t, t + 1, false, false};
        }
        else {
            Run r;
            if (!anchor_states->peek(r))
                break;
            anchor_states->pop();
            // One that started before the range isn't in it.
            if (r.s_open && from > 0)
                continue;
            if (r.s >= to)
                break;
            k = Candidate{r.s, r.e, r.e, r.e, r.s_open, r.e_open};
            if (!duration_ok(prog, k))
                continue;
        }

        if (progress)
            progress->store(k.s, std::memory_order_relaxed);

        bool pass = true;
        for (size_t i = 0; i < conds.size(); i++){
            m.values[i] = ConditionValue();
            if (!evaluate(ctx, conds[i], k, m.values[i])){
                pass = false;
                break;
            }
        }
        // A stream that stopped early may have passed a condition wrongly.
        // A failed source drains the walkers before check() notices, so
        // ask it directly.
        if (ctx.stopped || source.failed())
            break;
        if (!pass)
            continue;

        m.start = k.s;
        m.end = k.e;
        m.start_unknown = k.su;
        m.end_unknown = k.eu;
        sink(m);
        res.matches++;
        if (res.matches >= limit){
            res.status = RunStatus::Limit;
            res.next = k.s + 1;
            break;
        }
    }

    if (ctx.too_much)
        res.status = RunStatus::TooMuchData;
    else if (source.failed())
        res.status = RunStatus::DataChanged;
    else if (ctx.stopped)
        res.status = RunStatus::Cancelled;
    return res;
}

} // namespace search
} // namespace pv
