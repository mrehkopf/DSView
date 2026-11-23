/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#include "searchquery.h"

#include <cmath>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>

namespace pv {
namespace search {

//------------------------------------------------------------- errors

QString error_message(const Error &e)
{
    QString text;
    const QString d = e.detail;

    switch (e.code){
    case ErrorCode::None:               return QString();
    case ErrorCode::NoPattern:
        text = e.part == ErrorPart::Boundary ? QStringLiteral("no boundary event is given")
                                             : QStringLiteral("nothing to look for is given");
        break;
    case ErrorCode::EmptyPattern:       text = QStringLiteral("a pattern has no signals"); break;
    case ErrorCode::NoCondition:        text = QStringLiteral("a pattern has only don't-care values"); break;
    case ErrorCode::EmptyValue:         text = QStringLiteral("the value of %1 is missing").arg(d); break;
    case ErrorCode::BadChannelValue:    text = QStringLiteral("'%1' is not a channel value (use 0, 1, X, R, F or C)").arg(d); break;
    case ErrorCode::BadValue:           text = QStringLiteral("'%1' is not a valid bus value").arg(d); break;
    case ErrorCode::ValueTooWide:       text = QStringLiteral("'%1' does not fit the bus").arg(d); break;
    case ErrorCode::EdgeNotAllowed:     text = QStringLiteral("'%1': edges can't be compared").arg(d); break;
    case ErrorCode::DontCareNotAllowed: text = QStringLiteral("'%1': X can't be used in a comparison").arg(d); break;
    case ErrorCode::BadRange:           text = QStringLiteral("'%1': the range is reversed").arg(d); break;
    case ErrorCode::EdgeInState:        text = QStringLiteral("a state can't contain edges"); break;
    case ErrorCode::UnknownChannel:     text = QStringLiteral("channel %1 has no data").arg(d); break;
    case ErrorCode::UnknownBus:         text = QStringLiteral("there is no parallel decoder bus named '%1'").arg(d); break;
    case ErrorCode::BusTooWide:         text = QStringLiteral("bus %1 has more than 64 bits").arg(d); break;
    case ErrorCode::UnmappedBit:        text = QStringLiteral("'%1' needs a bus bit that has no channel").arg(d); break;
    case ErrorCode::ConflictingTerms:   text = QStringLiteral("channel %1 has conflicting values").arg(d); break;
    case ErrorCode::BadTime:            text = QStringLiteral("'%1' is not a valid time").arg(d); break;
    case ErrorCode::MissingUnit:        text = QStringLiteral("'%1' needs a unit (s, ms, us, ns or sa)").arg(d); break;
    case ErrorCode::EmptyWindow:        text = QStringLiteral("the time window is empty"); break;
    case ErrorCode::BadCount:           text = QStringLiteral("'%1' is not a valid count").arg(d); break;
    case ErrorCode::BadDuration:        text = QStringLiteral("the minimum duration is longer than the maximum"); break;
    case ErrorCode::DuringNeedsState:   text = QStringLiteral("\"during the match\" needs a state to find"); break;
    case ErrorCode::TooManyConditions:  text = QStringLiteral("there are more than 8 conditions"); break;
    case ErrorCode::NoSamplerate:       text = QStringLiteral("times need a sample rate"); break;
    case ErrorCode::BadQuery:           text = QStringLiteral("malformed query: %1").arg(d); break;
    }

    if (e.condition < 0)
        return text;
    return QStringLiteral("condition %1: %2").arg(e.condition + 1).arg(text);
}

//------------------------------------------------------------- values

bool BusValue::has_edge() const
{
    if (op == ValueOp::Changes)
        return true;
    if (op != ValueOp::Pattern)
        return false;
    for (Req r : bits){
        if (is_edge(r))
            return true;
    }
    return false;
}

ErrorCode parse_channel_value(const QString &text, Req &req)
{
    const QString t = text.trimmed().toLower();
    if (t.isEmpty())
        return ErrorCode::EmptyValue;
    if (t == QLatin1String("0"))
        req = Req::Low;
    else if (t == QLatin1String("1"))
        req = Req::High;
    else if (t == QLatin1String("x"))
        req = Req::Any;
    else if (t == QLatin1String("r"))
        req = Req::Rise;
    else if (t == QLatin1String("f"))
        req = Req::Fall;
    else if (t == QLatin1String("c") || t == QLatin1String("changes") || t == QLatin1String("change"))
        req = Req::Change;
    else
        return ErrorCode::BadChannelValue;
    return ErrorCode::None;
}

namespace {

// Digits of one number or pattern, lower case, without spaces. Bits past
// the digits are 0.
ErrorCode parse_digits(const QString &s, int width, std::vector<Req> &bits)
{
    QString body;
    bool binary = false;

    if (s.startsWith(QLatin1String("0x")))
        body = s.mid(2);
    else if (s.startsWith(QLatin1Char('$')))
        body = s.mid(1);
    else if (s.startsWith(QLatin1String("0b"))){
        body = s.mid(2);
        binary = true;
    }
    else
        body = s;

    if (body.isEmpty())
        return ErrorCode::BadValue;

    bits.assign(width, Req::Low);
    const int per_digit = binary ? 1 : 4;
    const int count = body.size();

    for (int i = 0; i < count; i++){
        const QChar c = body.at(count - 1 - i);
        const int first = i * per_digit;
        Req digit[4] = {Req::Low, Req::Low, Req::Low, Req::Low};

        if (binary){
            if (c == QLatin1Char('0'))      digit[0] = Req::Low;
            else if (c == QLatin1Char('1')) digit[0] = Req::High;
            else if (c == QLatin1Char('x')) digit[0] = Req::Any;
            else if (c == QLatin1Char('r')) digit[0] = Req::Rise;
            else if (c == QLatin1Char('f')) digit[0] = Req::Fall;
            else if (c == QLatin1Char('c')) digit[0] = Req::Change;
            else return ErrorCode::BadValue;
        }
        else if (c == QLatin1Char('x')){
            for (int b = 0; b < 4; b++)
                digit[b] = Req::Any;
        }
        else {
            bool ok;
            const int v = QString(c).toInt(&ok, 16);
            if (!ok)
                return ErrorCode::BadValue;
            for (int b = 0; b < 4; b++)
                digit[b] = (v >> b) & 1 ? Req::High : Req::Low;
        }

        for (int b = 0; b < per_digit; b++){
            const int bit = first + b;
            if (bit < width)
                bits[bit] = digit[b];
            else if (digit[b] != Req::Low && digit[b] != Req::Any)
                return ErrorCode::ValueTooWide;
        }
    }
    return ErrorCode::None;
}

void value_and_mask(const std::vector<Req> &bits, uint64_t &value, uint64_t &mask,
                    bool &has_x, bool &has_edge)
{
    value = 0;
    mask = 0;
    has_x = false;
    has_edge = false;
    for (size_t i = 0; i < bits.size(); i++){
        switch (bits[i]){
        case Req::High: value |= 1ULL << i; mask |= 1ULL << i; break;
        case Req::Low:  mask |= 1ULL << i; break;
        case Req::Any:  has_x = true; break;
        default:        has_edge = true; break;
        }
    }
}

ErrorCode parse_plain_number(const QString &s, int width, uint64_t &value)
{
    std::vector<Req> bits;
    ErrorCode r = parse_digits(s, width, bits);
    if (r != ErrorCode::None)
        return r;
    uint64_t mask;
    bool has_x, has_edge;
    value_and_mask(bits, value, mask, has_x, has_edge);
    if (has_edge)
        return ErrorCode::EdgeNotAllowed;
    if (has_x)
        return ErrorCode::DontCareNotAllowed;
    return ErrorCode::None;
}

} // namespace

ErrorCode parse_bus_value(const QString &text, int width, BusValue &out)
{
    if (width < 1 || width > 64)
        return ErrorCode::BusTooWide;

    QString s;
    for (QChar c : text){
        if (!c.isSpace() && c != QLatin1Char('_'))
            s.append(c.toLower());
    }
    if (s.isEmpty())
        return ErrorCode::EmptyValue;

    BusValue v;
    if (s == QLatin1String("changes") || s == QLatin1String("change")){
        v.op = ValueOp::Changes;
        out = v;
        return ErrorCode::None;
    }

    QString rest = s;
    if (s.startsWith(QLatin1String("=="))){ v.op = ValueOp::Pattern; rest = s.mid(2); }
    else if (s.startsWith(QLatin1String("!="))){ v.op = ValueOp::NotEqual; rest = s.mid(2); }
    else if (s.startsWith(QLatin1String("<="))){ v.op = ValueOp::LessEqual; rest = s.mid(2); }
    else if (s.startsWith(QLatin1String(">="))){ v.op = ValueOp::GreaterEqual; rest = s.mid(2); }
    else if (s.startsWith(QLatin1Char('<'))){ v.op = ValueOp::Less; rest = s.mid(1); }
    else if (s.startsWith(QLatin1Char('>'))){ v.op = ValueOp::Greater; rest = s.mid(1); }
    else if (s.startsWith(QLatin1Char('='))){ v.op = ValueOp::Pattern; rest = s.mid(1); }
    else if (s.contains(QLatin1String(".."))) v.op = ValueOp::Range;

    if (rest.isEmpty())
        return ErrorCode::BadValue;

    ErrorCode r = ErrorCode::None;
    switch (v.op){
    case ValueOp::Pattern:
        r = parse_digits(rest, width, v.bits);
        break;

    case ValueOp::NotEqual: {
        std::vector<Req> bits;
        r = parse_digits(rest, width, bits);
        if (r != ErrorCode::None)
            break;
        bool has_x, has_edge;
        value_and_mask(bits, v.value, v.mask, has_x, has_edge);
        if (has_edge)
            r = ErrorCode::EdgeNotAllowed;
        else if (v.mask == 0)
            r = ErrorCode::DontCareNotAllowed;
        break;
    }

    case ValueOp::Range: {
        const QStringList ends = rest.split(QLatin1String(".."));
        if (ends.size() != 2 || ends[0].isEmpty() || ends[1].isEmpty())
            return ErrorCode::BadValue;
        r = parse_plain_number(ends[0], width, v.value);
        if (r == ErrorCode::None)
            r = parse_plain_number(ends[1], width, v.high);
        if (r == ErrorCode::None && v.value > v.high)
            r = ErrorCode::BadRange;
        break;
    }

    default:
        r = parse_plain_number(rest, width, v.value);
        break;
    }

    if (r == ErrorCode::None)
        out = v;
    return r;
}

//------------------------------------------------------------- times

ErrorCode parse_time(const QString &text, TimeValue &time)
{
    QString s;
    for (QChar c : text){
        if (!c.isSpace())
            s.append(c.toLower());
    }
    if (s.isEmpty())
        return ErrorCode::BadTime;

    static const QRegularExpression rx(QStringLiteral(
        "^([+-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:e[+-]?\\d+)?)(ms|us|µs|ns|ps|samples|sample|sa|s)?$"));
    const QRegularExpressionMatch m = rx.match(s);
    if (!m.hasMatch())
        return ErrorCode::BadTime;

    bool ok;
    const double v = m.captured(1).toDouble(&ok);
    if (!ok || !std::isfinite(v))
        return ErrorCode::BadTime;

    const QString unit = m.captured(2);
    TimeValue t;
    if (unit.isEmpty()){
        if (v != 0)
            return ErrorCode::MissingUnit;
        t.value = 0;
    }
    else if (unit == QLatin1String("sa") || unit.startsWith(QLatin1String("sample"))){
        if (v != std::floor(v) || std::fabs(v) > 9e15)
            return ErrorCode::BadTime;
        t.value = v;
        t.samples = true;
    }
    else if (unit == QLatin1String("s"))  t.value = v;
    else if (unit == QLatin1String("ms")) t.value = v * 1e-3;
    else if (unit == QLatin1String("ns")) t.value = v * 1e-9;
    else if (unit == QLatin1String("ps")) t.value = v * 1e-12;
    else                                  t.value = v * 1e-6;

    time = t;
    return ErrorCode::None;
}

ErrorCode parse_count(const QString &text, uint64_t &count)
{
    static const QRegularExpression rx(QStringLiteral("^\\d+$"));
    const QString s = text.trimmed();
    if (!rx.match(s).hasMatch())
        return ErrorCode::BadCount;
    bool ok;
    const qulonglong v = s.toULongLong(&ok);
    if (!ok)
        return ErrorCode::BadCount;
    count = v;
    return ErrorCode::None;
}

namespace {
// Rounding slack, in samples, for times that are a whole number of samples.
const double Slack = 1e-6;
const double MaxSamples = 9e18;
}

bool time_to_samples_ceil(const TimeValue &time, double samplerate, int64_t &samples)
{
    if (time.samples){
        samples = (int64_t)time.value;
        return true;
    }
    if (time.value == 0){
        samples = 0;
        return true;
    }
    if (samplerate <= 0)
        return false;
    const double x = std::ceil(time.value * samplerate - Slack);
    samples = (int64_t)std::max(-MaxSamples, std::min(MaxSamples, x));
    return true;
}

bool time_to_samples_floor(const TimeValue &time, double samplerate, int64_t &samples)
{
    if (time.samples){
        samples = (int64_t)time.value;
        return true;
    }
    if (time.value == 0){
        samples = 0;
        return true;
    }
    if (samplerate <= 0)
        return false;
    const double x = std::floor(time.value * samplerate + Slack);
    samples = (int64_t)std::max(-MaxSamples, std::min(MaxSamples, x));
    return true;
}

//------------------------------------------------------------- JSON

namespace {

const char *const QuantifierKeys[] = {
    "occurs", "none", "at_least", "at_most", "exactly", "sometime", "never", "throughout"
};
const char *const WindowKeys[] = { "start", "end", "during", "until", "since" };

QJsonArray patterns_to_json(const Patterns &patterns)
{
    QJsonArray list;
    for (const Pattern &p : patterns){
        QJsonArray terms;
        for (const Term &t : p){
            QJsonObject o;
            if (!t.bits.empty()){
                QJsonArray bits;
                for (int b : t.bits)
                    bits.append(b);
                o["bits"] = bits;
                if (!t.bus.isEmpty())
                    o["bus"] = t.bus;
            }
            else if (!t.bus.isEmpty())
                o["bus"] = t.bus;
            else
                o["channel"] = t.channel;
            o["value"] = t.value;
            terms.append(o);
        }
        list.append(terms);
    }
    return list;
}

bool patterns_from_json(const QJsonValue &value, Patterns &patterns, QString &error)
{
    patterns.clear();
    if (value.isUndefined() || value.isNull())
        return true;
    if (!value.isArray()){
        error = QStringLiteral("patterns must be an array of arrays of terms");
        return false;
    }
    for (const QJsonValue &pv : value.toArray()){
        if (!pv.isArray()){
            error = QStringLiteral("a pattern must be an array of terms");
            return false;
        }
        Pattern p;
        for (const QJsonValue &tv : pv.toArray()){
            if (!tv.isObject()){
                error = QStringLiteral("a term must be an object");
                return false;
            }
            const QJsonObject o = tv.toObject();
            Term t;
            if (o.contains("bits")){
                if (!o["bits"].isArray() || o["bits"].toArray().isEmpty()){
                    error = QStringLiteral("\"bits\" must be a non-empty array of channels");
                    return false;
                }
                for (const QJsonValue &b : o["bits"].toArray()){
                    if (!b.isDouble()){
                        error = QStringLiteral("\"bits\" must hold channel numbers");
                        return false;
                    }
                    t.bits.push_back(b.toInt());
                }
                t.bus = o["bus"].toString();
            }
            else if (o.contains("bus")){
                t.bus = o["bus"].toString();
                if (t.bus.isEmpty()){
                    error = QStringLiteral("\"bus\" must be a decoder name");
                    return false;
                }
            }
            else if (o["channel"].isDouble())
                t.channel = o["channel"].toInt();
            else {
                error = QStringLiteral("a term needs \"channel\", \"bus\" or \"bits\"");
                return false;
            }
            if (!o["value"].isString()){
                error = QStringLiteral("a term needs a \"value\" string");
                return false;
            }
            t.value = o["value"].toString();
            p.push_back(t);
        }
        patterns.push_back(p);
    }
    return true;
}

QString string_of(const QJsonValue &v)
{
    if (v.isDouble())
        return QString::number(v.toDouble(), 'g', 17);
    return v.toString();
}

} // namespace

QString quantifier_key(Quantifier q)
{
    return QString::fromLatin1(QuantifierKeys[(int)q]);
}

QString window_key(WindowKind w)
{
    return QString::fromLatin1(WindowKeys[(int)w]);
}

QJsonObject to_json(const Query &q)
{
    QJsonObject o;
    o["find"] = q.state ? "state" : "event";
    o["patterns"] = patterns_to_json(q.find);
    if (q.state){
        if (!q.min_duration.isEmpty())
            o["min_duration"] = q.min_duration;
        if (!q.max_duration.isEmpty())
            o["max_duration"] = q.max_duration;
    }

    QJsonArray conds;
    for (const Condition &c : q.conditions){
        QJsonObject co;
        co["subject"] = c.state ? "state" : "event";
        co["patterns"] = patterns_to_json(c.subject);
        co["quantifier"] = quantifier_key(c.quantifier);
        if (has_count(c.quantifier))
            co["count"] = c.count;
        co["window"] = window_key(c.window);
        if (c.window == WindowKind::Start || c.window == WindowKind::End){
            co["from"] = c.from;
            co["to"] = c.to;
        }
        if (c.window == WindowKind::Until || c.window == WindowKind::Since)
            co["boundary"] = patterns_to_json(c.boundary);
        conds.append(co);
    }
    o["conditions"] = conds;
    return o;
}

bool from_json(const QJsonObject &o, Query &q, QString &error)
{
    Query r;

    const QString find = o["find"].toString(QStringLiteral("event"));
    if (find != QLatin1String("event") && find != QLatin1String("state")){
        error = QStringLiteral("\"find\" must be \"event\" or \"state\"");
        return false;
    }
    r.state = find == QLatin1String("state");
    if (!patterns_from_json(o["patterns"], r.find, error))
        return false;
    r.min_duration = string_of(o["min_duration"]);
    r.max_duration = string_of(o["max_duration"]);

    const QJsonValue cv = o["conditions"];
    if (!cv.isUndefined() && !cv.isNull() && !cv.isArray()){
        error = QStringLiteral("\"conditions\" must be an array");
        return false;
    }
    for (const QJsonValue &v : cv.toArray()){
        if (!v.isObject()){
            error = QStringLiteral("a condition must be an object");
            return false;
        }
        const QJsonObject co = v.toObject();
        Condition c;

        const QString subject = co["subject"].toString(QStringLiteral("event"));
        if (subject != QLatin1String("event") && subject != QLatin1String("state")){
            error = QStringLiteral("\"subject\" must be \"event\" or \"state\"");
            return false;
        }
        c.state = subject == QLatin1String("state");
        if (!patterns_from_json(co["patterns"], c.subject, error))
            return false;

        const QString qk = co["quantifier"].toString(c.state ? QStringLiteral("sometime") : QStringLiteral("occurs"));
        bool found = false;
        for (int i = 0; i < 8; i++){
            if (qk == QLatin1String(QuantifierKeys[i])){
                c.quantifier = (Quantifier)i;
                found = true;
            }
        }
        if (!found || is_event_quantifier(c.quantifier) == c.state){
            error = QStringLiteral("quantifier \"%1\" does not fit a %2").arg(qk, subject);
            return false;
        }
        c.count = string_of(co["count"]);

        const QString wk = co["window"].toString(QStringLiteral("start"));
        found = false;
        for (int i = 0; i < 5; i++){
            if (wk == QLatin1String(WindowKeys[i])){
                c.window = (WindowKind)i;
                found = true;
            }
        }
        if (!found){
            error = QStringLiteral("unknown window \"%1\"").arg(wk);
            return false;
        }
        c.from = string_of(co["from"]);
        c.to = string_of(co["to"]);
        if (!patterns_from_json(co["boundary"], c.boundary, error))
            return false;
        r.conditions.push_back(c);
    }

    q = r;
    return true;
}

} // namespace search
} // namespace pv
