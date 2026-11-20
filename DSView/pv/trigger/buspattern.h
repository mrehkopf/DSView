/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#ifndef DSVIEW_PV_TRIGGER_BUSPATTERN_H
#define DSVIEW_PV_TRIGGER_BUSPATTERN_H

#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

namespace pv {
namespace trigger {

// Index is bus bit significance, value is the physical channel index.
// A missing bit (-1) has the same constant-zero meaning as in the decoder.
using BusMapping = QVector<int>;
using ChannelPattern = QMap<int, QChar>;

enum class BusValueError {
    None,
    EmptyBus,
    InvalidHex,
    OutOfRange,
    UnmappedBit,
    UnavailableChannel,
    ConflictingBits,
    NoTriggerChannel
};

// Untriggerable channels exist but can't be trigger sources (the secondary
// analyzers' while stacking). Bits on them are left out of assignments,
// whatever their level, instead of failing as unavailable.
// On failure, assignments is left unchanged. No trigger state is changed here.
BusValueError compile_bus_value(const BusMapping &mapping, const QString &text,
    const QSet<int> &available_channels, const QSet<int> &untriggerable_channels,
    ChannelPattern &assignments);
BusValueError compile_bus_value(const BusMapping &mapping, const QString &text,
    const QSet<int> &available_channels, ChannelPattern &assignments);

// Returns false for edges, don't-care bits, or missing physical channels.
// Bits on untriggerable channels are skipped and read as 0.
bool read_bus_value(const BusMapping &mapping, const ChannelPattern &pattern,
    const QSet<int> &untriggerable_channels, quint32 &value);
bool read_bus_value(const BusMapping &mapping, const ChannelPattern &pattern,
    quint32 &value);

// The bus bits whose channel is untriggerable, as a mask by bit significance.
quint32 untriggerable_bus_bits(const BusMapping &mapping,
    const QSet<int> &untriggerable_channels);

// Bus bits as decoder channel names with ranges, e.g. "D0, D8–D15".
QString bus_bit_names(quint32 bits);

// Existing stage fields contain spaced symbols, with the highest channel first.
ChannelPattern decode_stage_pattern(const QString &text, int first_channel);
QString patch_stage_pattern(const QString &text, int first_channel,
    const ChannelPattern &assignments);

} // namespace trigger
} // namespace pv

#endif
