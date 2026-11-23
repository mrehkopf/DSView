/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#ifndef DSVIEW_PV_SEARCH_SEARCHQUERY_H
#define DSVIEW_PV_SEARCH_SEARCHQUERY_H

#include <stdint.h>
#include <vector>
#include <QString>
#include <QJsonObject>

// The search query as the user enters it (doc/search.md). Values and times
// are kept as typed; searchengine.h checks and compiles them.

namespace pv {
namespace search {

// A signal and the value it must have: one channel, a parallel decoder's
// bus by name, or an explicit list of channels (D0 first, -1 for a bit that
// is always 0).
struct Term
{
    int channel = -1;
    QString bus;
    std::vector<int> bits;
    QString value;

    inline bool is_channel() const { return bus.isEmpty() && bits.empty(); }
};

// A pattern holds when all its terms do; a list of patterns when any does.
using Pattern = std::vector<Term>;
using Patterns = std::vector<Pattern>;

enum class Quantifier {
    // event conditions
    Occurs, None, AtLeast, AtMost, Exactly,
    // state conditions
    Sometime, Never, Throughout
};

enum class WindowKind {
    Start,      // from..to relative to the match start
    End,        // from..to relative to the match end
    During,     // while the matched state holds
    Until,      // after the match, until the next boundary event
    Since       // since the previous boundary event, until the match
};

struct Condition
{
    bool state = false;             // the subject is a state, not an event
    Patterns subject;
    Quantifier quantifier = Quantifier::Occurs;
    QString count;                  // N for AtLeast, AtMost, Exactly
    WindowKind window = WindowKind::Start;
    QString from;                   // Start and End windows, signed times
    QString to;
    Patterns boundary;              // Until and Since windows
};

struct Query
{
    bool state = false;             // find a state, not an event
    Patterns find;
    QString min_duration;           // states only; empty means no bound
    QString max_duration;
    std::vector<Condition> conditions;
};

inline bool is_event_quantifier(Quantifier q)
{
    return q == Quantifier::Occurs || q == Quantifier::None || q == Quantifier::AtLeast
        || q == Quantifier::AtMost || q == Quantifier::Exactly;
}

inline bool has_count(Quantifier q)
{
    return q == Quantifier::AtLeast || q == Quantifier::AtMost || q == Quantifier::Exactly;
}

//------------------------------------------------------------- errors

enum class ErrorCode {
    None,
    NoPattern,          // nothing to find, no subject, or no boundary event
    EmptyPattern,       // a pattern without signals
    NoCondition,        // a pattern whose signals are all don't care
    EmptyValue,
    BadChannelValue,    // not one of 0 1 X R F C
    BadValue,           // bus value syntax
    ValueTooWide,
    EdgeNotAllowed,     // edges in a comparison, a range or !=
    DontCareNotAllowed, // X in a comparison or a range
    BadRange,
    EdgeInState,        // edges in a state
    UnknownChannel,     // a channel without data
    UnknownBus,
    BusTooWide,
    UnmappedBit,        // a level of 1 or an edge on a bus bit without a channel
    ConflictingTerms,   // a channel that must have two levels at once
    BadTime,
    MissingUnit,
    EmptyWindow,
    BadCount,
    BadDuration,
    DuringNeedsState,
    TooManyConditions,
    NoSamplerate,
    BadQuery            // malformed JSON
};

enum class ErrorPart {
    Subject,            // the patterns to find, or a condition's subject
    Boundary,           // a condition's boundary event
    Window,             // from, to
    Count,
    Duration
};

struct Error
{
    ErrorCode code = ErrorCode::None;
    int condition = -1;             // -1 for what to find
    ErrorPart part = ErrorPart::Subject;
    QString detail;                 // a channel, bus or value

    inline explicit operator bool() const { return code != ErrorCode::None; }
};

// English text for the control interface and the tests. The dialog has
// its own, translated, texts.
QString error_message(const Error &error);

//------------------------------------------------------------- values

// What one bit (a channel, or a bus bit) must do at a sample.
enum class Req : uint8_t { Any, Low, High, Rise, Fall, Change };

inline bool is_edge(Req r)
{
    return r == Req::Rise || r == Req::Fall || r == Req::Change;
}

enum class ValueOp { Pattern, NotEqual, Less, LessEqual, Greater, GreaterEqual, Range, Changes };

// A bus value, D0 first.
struct BusValue
{
    ValueOp op = ValueOp::Pattern;
    std::vector<Req> bits;          // Pattern: one per bus bit
    uint64_t value = 0;             // NotEqual, comparisons, Range low end
    uint64_t mask = 0;              // NotEqual: the bits that are not X
    uint64_t high = 0;              // Range high end

    bool has_edge() const;
};

// 0 1 X R F C, or "changes". Case does not matter.
ErrorCode parse_channel_value(const QString &text, Req &req);

// Hex (default, 0x or $ prefix) with X digits, binary (0b) with 0 1 X R F C
// bits, decimal (0d); "!= v", "< v", "<= v", "> v", ">= v", "a..b",
// "changes". Underscores and spaces are ignored.
ErrorCode parse_bus_value(const QString &text, int width, BusValue &value);

//------------------------------------------------------------- times

struct TimeValue
{
    double value = 0;               // seconds, or samples
    bool samples = false;
};

// A signed time with a unit: s, ms, us (µs), ns, ps, or sa (samples).
// A bare 0 needs no unit.
ErrorCode parse_time(const QString &text, TimeValue &time);
ErrorCode parse_count(const QString &text, uint64_t &count);

// Sample offsets: the first sample at or after the time, and the last one
// at or before it. False without a sample rate for times in seconds.
bool time_to_samples_ceil(const TimeValue &time, double samplerate, int64_t &samples);
bool time_to_samples_floor(const TimeValue &time, double samplerate, int64_t &samples);

//------------------------------------------------------------- JSON

QJsonObject to_json(const Query &query);
bool from_json(const QJsonObject &object, Query &query, QString &error);

QString quantifier_key(Quantifier q);
QString window_key(WindowKind w);

} // namespace search
} // namespace pv

#endif
