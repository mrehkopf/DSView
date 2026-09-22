/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#ifndef DSVIEW_PV_TRIGGER_DECODERBUS_H
#define DSVIEW_PV_TRIGGER_DECODERBUS_H

#include "buspattern.h"

namespace pv {
class SigSession;

namespace trigger {

struct DecoderBus {
    // Used only to retain the selected instance across refreshes, never dereferenced.
    const void *source;
    QString name;
    BusMapping mapping;
};

QVector<DecoderBus> parallel_decoder_buses(SigSession &session);

} // namespace trigger
} // namespace pv

#endif
