/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#include "buspattern.h"

#include <QRegularExpression>

namespace pv {
namespace trigger {

BusValueError compile_bus_value(const BusMapping &mapping, const QString &text,
    const QSet<int> &available_channels, ChannelPattern &assignments)
{
    if (mapping.isEmpty() || mapping.size() > 32)
        return BusValueError::EmptyBus;

    static const QRegularExpression hex(QStringLiteral("^(?:0[xX])?[0-9a-fA-F]+$"));
    const QString input = text.trimmed();
    if (!hex.match(input).hasMatch())
        return BusValueError::InvalidHex;

    bool ok;
    const quint64 value = input.toULongLong(&ok, 16);
    if (!ok || value >= (quint64(1) << mapping.size()))
        return BusValueError::OutOfRange;

    ChannelPattern result;
    for (int bit = 0; bit < mapping.size(); ++bit) {
        const int channel = mapping.at(bit);
        const QChar level = (value & (quint64(1) << bit)) ? QLatin1Char('1') : QLatin1Char('0');
        if (channel == -1) {
            if (level == QLatin1Char('1'))
                return BusValueError::UnmappedBit;
            continue;
        }
        if (channel < 0 || channel >= 32 || !available_channels.contains(channel))
            return BusValueError::UnavailableChannel;
        if (result.contains(channel) && result.value(channel) != level)
            return BusValueError::ConflictingBits;
        result.insert(channel, level);
    }
    if (result.isEmpty())
        return BusValueError::EmptyBus;

    assignments = result;
    return BusValueError::None;
}

bool read_bus_value(const BusMapping &mapping, const ChannelPattern &pattern,
    quint32 &value)
{
    if (mapping.isEmpty() || mapping.size() > 32)
        return false;

    quint32 result = 0;
    bool connected = false;
    for (int bit = 0; bit < mapping.size(); ++bit) {
        const int channel = mapping.at(bit);
        if (channel == -1)
            continue;
        const QChar level = pattern.value(channel);
        if (level != QLatin1Char('0') && level != QLatin1Char('1'))
            return false;
        if (level == QLatin1Char('1'))
            result |= quint32(1) << bit;
        connected = true;
    }
    if (!connected)
        return false;
    value = result;
    return true;
}

ChannelPattern decode_stage_pattern(const QString &text, int first_channel)
{
    ChannelPattern result;
    const int count = (text.size() + 1) / 2;
    for (int i = 0; i < count; ++i)
        result.insert(first_channel + count - i - 1, text.at(i * 2).toUpper());
    return result;
}

QString patch_stage_pattern(const QString &text, int first_channel,
    const ChannelPattern &assignments)
{
    QString result = text;
    const int count = (text.size() + 1) / 2;
    for (int i = 0; i < count; ++i) {
        const int channel = first_channel + count - i - 1;
        if (assignments.contains(channel))
            result[i * 2] = assignments.value(channel);
    }
    return result;
}

} // namespace trigger
} // namespace pv
