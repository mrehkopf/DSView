/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#include "decoderbus.h"

#include <libsigrokdecode.h>
#include "../sigsession.h"
#include "../view/decodetrace.h"
#include "../data/decoderstack.h"
#include "../data/decode/decoder.h"

namespace pv {
namespace trigger {

QVector<DecoderBus> parallel_decoder_buses(SigSession &session)
{
    QVector<DecoderBus> buses;
    for (auto trace : session.get_decode_signals()) {
        for (auto decoder : trace->decoder()->stack()) {
            if (QString::fromUtf8(decoder->decoder()->id) != QStringLiteral("parallel"))
                continue;

            DecoderBus bus{decoder, trace->get_name(), BusMapping(32, -1)};
            int width = 0;
            for (auto channel : decoder->binded_probe_list()) {
                const QString id = QString::fromUtf8(channel->id);
                if (!id.startsWith(QLatin1Char('d')))
                    continue;
                bool ok;
                const int bit = id.mid(1).toInt(&ok);
                if (!ok || bit < 0 || bit >= 32)
                    continue;
                bus.mapping[bit] = decoder->binded_probe_index(channel);
                width = qMax(width, bit + 1);
            }
            bus.mapping.resize(width);
            buses.push_back(bus);
        }
    }
    return buses;
}

} // namespace trigger
} // namespace pv
