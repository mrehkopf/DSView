/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef DSVIEW_PV_CONTROL_CONTROLDECODERS_H
#define DSVIEW_PV_CONTROL_CONTROLDECODERS_H

#include <vector>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "../data/decode/row.h"

struct srd_decoder;

namespace pv {

class SigSession;

namespace data {
class DecoderStack;
namespace decode {
class Annotation;
}
}

namespace control {

// Protocol decoders as the control interface shows them
// (doc/control-interface.md, "Protocol decoders").
namespace decoders {

// NULL if no decoder has this id.
const srd_decoder *find(const QString &id);

// The installed decoders, in short: id, names, inputs and outputs.
QJsonArray catalog();

// One decoder in full: channels, options, rows and annotation classes.
QJsonObject describe(const srd_decoder *dec);

// The session's decoder at this index, as decoder.list shows it.
// capturing: a capture runs or a data file loads; its decoders decode at
// the end.
QJsonObject entry(SigSession *session, int index, bool capturing);

// Checks the parameters of decoder.add and turns them into a decoder
// object as session and configuration files hold it, for
// StoreSession::load_decoders. False if they don't fit; error says why.
bool build(SigSession *session, const QJsonObject &params,
           QJsonObject &config, QString &error);

// The row that "row" (an index from decoder.list, or a row id) and the
// optional "decoder" (the id of one decoder in the stack) select.
bool find_row(data::DecoderStack *stack, const QJsonObject &params,
              data::decode::Row &row, int &row_index, QString &error);

QJsonObject row_json(data::DecoderStack *stack, const data::decode::Row &row, int index);

QJsonObject annotation_json(const data::decode::Row &row,
                            const data::decode::Annotation &ann, double samplerate);

// The display format of numbers in annotations: hex, dec, oct, bin or ascii.
QString format_name(int format);

} // namespace decoders
} // namespace control
} // namespace pv

#endif // DSVIEW_PV_CONTROL_CONTROLDECODERS_H
