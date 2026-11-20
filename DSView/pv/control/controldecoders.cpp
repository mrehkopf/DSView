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

#include "controldecoders.h"

#include <cmath>
#include <algorithm>
#include <glib.h>
#include <libsigrokdecode.h>
#include <libsigrok.h>
#include <QStringList>

#include "../sigsession.h"
#include "../dsvdef.h"
#include "../view/signal.h"
#include "../view/decodetrace.h"
#include "../data/decoderstack.h"
#include "../data/decode/decoder.h"
#include "../data/decode/annotation.h"
#include "../data/decode/decoderstatus.h"

namespace pv {
namespace control {
namespace decoders {

namespace {

QJsonArray string_list(const GSList *list)
{
    QJsonArray a;
    for (const GSList *l = list; l; l = l->next)
        a.append(QString::fromUtf8((const char *)l->data));
    return a;
}

QJsonValue variant_json(GVariant *v)
{
    if (v == NULL)
        return QJsonValue();
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE))
        return g_variant_get_double(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
        return (double)g_variant_get_int64(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
        return QString::fromUtf8(g_variant_get_string(v, NULL));
    return QJsonValue();
}

// The option values StoreSession::load_decoders can set: doubles, 64-bit
// integers (Python ints) and strings.
bool supported_option(const srd_decoder_option *opt)
{
    return g_variant_is_of_type(opt->def, G_VARIANT_TYPE_DOUBLE)
        || g_variant_is_of_type(opt->def, G_VARIANT_TYPE_INT64)
        || g_variant_is_of_type(opt->def, G_VARIANT_TYPE_STRING);
}

const char *option_type(const srd_decoder_option *opt)
{
    if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE_DOUBLE))
        return "number";
    if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE_INT64))
        return "integer";
    return "string";
}

QJsonObject options_json(data::decode::Decoder *dec)
{
    const srd_decoder *d = dec->decoder();
    std::map<std::string, GVariant*> &set = dec->options();
    QJsonObject o;

    for (const GSList *l = d->options; l; l = l->next){
        const srd_decoder_option *opt = (const srd_decoder_option *)l->data;
        if (!supported_option(opt))
            continue;
        auto it = set.find(opt->id);
        o[opt->id] = variant_json(it != set.end() && it->second != NULL ? it->second : opt->def);
    }
    return o;
}

// The annotation class with this number, as the decoder's annotations
// tuple lists it. libsigrokdecode prepends each entry, so the list runs
// backwards; DSView's three-element entries start with a type number.
QString class_id(const srd_decoder *dec, int cls)
{
    const int n = (int)g_slist_length(dec->annotations);
    if (cls < 0 || cls >= n)
        return QString();

    char **entry = (char **)g_slist_nth_data(dec->annotations, n - 1 - cls);
    if (entry == NULL || entry[0] == NULL)
        return QString();
    return QString::fromUtf8(g_strv_length(entry) >= 3 ? entry[1] : entry[0]);
}

QString class_desc(const srd_decoder *dec, int cls)
{
    const int n = (int)g_slist_length(dec->annotations);
    char **entry = (char **)g_slist_nth_data(dec->annotations, n - 1 - cls);
    if (entry == NULL)
        return QString();
    const guint len = g_strv_length(entry);
    return QString::fromUtf8(len >= 3 ? entry[2] : len == 2 ? entry[1] : entry[0]);
}

bool takes_input(const srd_decoder *dec, const QString &input)
{
    for (const GSList *l = dec->inputs; l; l = l->next){
        if (input == QString::fromUtf8((const char *)l->data))
            return true;
    }
    return false;
}

bool stacks_on(const srd_decoder *upper, const srd_decoder *lower)
{
    for (const GSList *l = lower->outputs; l; l = l->next){
        if (takes_input(upper, QString::fromUtf8((const char *)l->data)))
            return true;
    }
    return false;
}

const srd_channel *find_channel(const srd_decoder *dec, const QString &id, bool &required)
{
    for (const GSList *l = dec->channels; l; l = l->next){
        const srd_channel *ch = (const srd_channel *)l->data;
        if (id == QString::fromUtf8(ch->id)){
            required = true;
            return ch;
        }
    }
    for (const GSList *l = dec->opt_channels; l; l = l->next){
        const srd_channel *ch = (const srd_channel *)l->data;
        if (id == QString::fromUtf8(ch->id)){
            required = false;
            return ch;
        }
    }
    return NULL;
}

QStringList channel_ids(const srd_decoder *dec)
{
    QStringList ids;
    for (const GSList *l = dec->channels; l; l = l->next)
        ids << QString::fromUtf8(((const srd_channel *)l->data)->id);
    for (const GSList *l = dec->opt_channels; l; l = l->next)
        ids << QString::fromUtf8(((const srd_channel *)l->data)->id);
    return ids;
}

QString value_text(const QJsonValue &v)
{
    if (v.isString())
        return "\"" + v.toString() + "\"";
    if (v.isDouble())
        return QString::number(v.toDouble(), 'g', 15);
    return "?";
}

// One option value from the client, checked against the decoder's type
// and list of values. A number may also come as a string, as the command
// line sends it.
bool check_option(const srd_decoder *dec, const QString &id, const QJsonValue &value,
                  QJsonValue &out, QString &error)
{
    const srd_decoder_option *opt = NULL;
    QStringList known;

    for (const GSList *l = dec->options; l; l = l->next){
        const srd_decoder_option *o = (const srd_decoder_option *)l->data;
        if (!supported_option(o))
            continue;
        known << QString::fromUtf8(o->id);
        if (id == QString::fromUtf8(o->id))
            opt = o;
    }

    if (opt == NULL){
        error = QString("%1 has no option \"%2\". Options: %3.")
                    .arg(dec->id, id, known.isEmpty() ? "none" : known.join(", "));
        return false;
    }

    if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE_STRING)){
        if (!value.isString()){
            error = QString("Option \"%1\" of %2 takes a string.").arg(id, dec->id);
            return false;
        }
        out = value.toString();
    }
    else{
        bool ok = value.isDouble();
        double v = value.toDouble();
        if (value.isString())
            v = value.toString().trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(v)){
            error = QString("Option \"%1\" of %2 takes a number.").arg(id, dec->id);
            return false;
        }
        if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE_INT64)){
            if (v != std::floor(v) || std::fabs(v) > 2147483647.0){
                // StoreSession::load_decoders reads integers as int.
                error = QString("Option \"%1\" of %2 takes an integer.").arg(id, dec->id);
                return false;
            }
        }
        out = v;
    }

    if (opt->values == NULL)
        return true;

    QStringList allowed;
    for (const GSList *l = opt->values; l; l = l->next){
        QJsonValue a = variant_json((GVariant *)l->data);
        if (a == out)
            return true;
        allowed << value_text(a);
    }

    error = QString("Option \"%1\" of %2 can't be %3. Values: %4.")
                .arg(id, dec->id, value_text(out), allowed.join(", "));
    return false;
}

bool check_options(const srd_decoder *dec, const QJsonValue &options,
                   QJsonObject &out, QString &error)
{
    if (options.isUndefined() || options.isNull())
        return true;
    if (!options.isObject()){
        error = QString("The options of %1 must be an object, e.g. {\"id\": value}.").arg(dec->id);
        return false;
    }

    QJsonObject o = options.toObject();
    for (auto it = o.begin(); it != o.end(); ++it){
        QJsonValue v;
        if (!check_option(dec, it.key(), it.value(), v, error))
            return false;
        out[it.key()] = v;
    }
    return true;
}

bool logic_channel_enabled(SigSession *session, int index)
{
    for (view::Signal *s : session->get_signals()){
        if (s->signal_type() == SR_CHANNEL_LOGIC && s->get_index() == index)
            return true;
    }
    return false;
}

} // namespace

const srd_decoder *find(const QString &id)
{
    if (id.isEmpty())
        return NULL;
    return srd_decoder_get_by_id(id.toUtf8().constData());
}

QJsonArray catalog()
{
    std::vector<const srd_decoder*> list;
    for (const GSList *l = srd_decoder_list(); l; l = l->next)
        list.push_back((const srd_decoder *)l->data);

    std::sort(list.begin(), list.end(), [](const srd_decoder *a, const srd_decoder *b){
        return g_ascii_strcasecmp(a->id, b->id) < 0;
    });

    QJsonArray a;
    for (const srd_decoder *d : list){
        QJsonObject o;
        o["id"] = QString::fromUtf8(d->id);
        o["name"] = QString::fromUtf8(d->name);
        o["longname"] = QString::fromUtf8(d->longname);
        o["inputs"] = string_list(d->inputs);
        o["outputs"] = string_list(d->outputs);
        a.append(o);
    }
    return a;
}

QJsonObject describe(const srd_decoder *dec)
{
    QJsonObject o;
    o["id"] = QString::fromUtf8(dec->id);
    o["name"] = QString::fromUtf8(dec->name);
    o["longname"] = QString::fromUtf8(dec->longname);
    o["desc"] = QString::fromUtf8(dec->desc);
    o["inputs"] = string_list(dec->inputs);
    o["outputs"] = string_list(dec->outputs);

    QJsonArray channels;
    for (int pass = 0; pass < 2; pass++){
        for (const GSList *l = pass == 0 ? dec->channels : dec->opt_channels; l; l = l->next){
            const srd_channel *ch = (const srd_channel *)l->data;
            QJsonObject c;
            c["id"] = QString::fromUtf8(ch->id);
            c["name"] = QString::fromUtf8(ch->name);
            c["desc"] = QString::fromUtf8(ch->desc);
            c["required"] = pass == 0;
            channels.append(c);
        }
    }
    o["channels"] = channels;

    QJsonArray options;
    for (const GSList *l = dec->options; l; l = l->next){
        const srd_decoder_option *opt = (const srd_decoder_option *)l->data;
        if (!supported_option(opt))
            continue;
        QJsonObject p;
        p["id"] = QString::fromUtf8(opt->id);
        p["desc"] = QString::fromUtf8(opt->desc);
        p["type"] = option_type(opt);
        p["default"] = variant_json(opt->def);
        if (opt->values != NULL){
            QJsonArray values;
            for (const GSList *v = opt->values; v; v = v->next)
                values.append(variant_json((GVariant *)v->data));
            p["values"] = values;
        }
        options.append(p);
    }
    o["options"] = options;

    QJsonArray classes;
    const int n = (int)g_slist_length(dec->annotations);
    for (int i = 0; i < n; i++){
        QJsonObject c;
        c["id"] = class_id(dec, i);
        c["desc"] = class_desc(dec, i);
        classes.append(c);
    }
    o["classes"] = classes;

    QJsonArray rows;
    for (const GSList *l = dec->annotation_rows; l; l = l->next){
        const srd_decoder_annotation_row *r = (const srd_decoder_annotation_row *)l->data;
        QJsonObject row;
        row["id"] = QString::fromUtf8(r->id);
        row["desc"] = QString::fromUtf8(r->desc);
        QJsonArray cls;
        for (const GSList *c = r->ann_classes; c; c = c->next)
            cls.append(class_id(dec, GPOINTER_TO_INT(c->data)));
        row["classes"] = cls;
        rows.append(row);
    }
    o["rows"] = rows;

    return o;
}

QJsonObject row_json(data::DecoderStack *stack, const data::decode::Row &row, int index)
{
    QJsonObject o;
    o["row"] = index;
    o["decoder"] = QString::fromUtf8(row.decoder()->id);
    o["id"] = row.row() != NULL ? QString::fromUtf8(row.row()->id)
                                : QString::fromUtf8(row.decoder()->id);
    o["title"] = row.title();
    o["annotations"] = (double)stack->row_annotation_count(row);
    return o;
}

QJsonObject entry(SigSession *session, int index, bool capturing)
{
    QJsonObject o;
    view::DecodeTrace *trace = session->get_decoder_trace(index);
    if (trace == NULL)
        return o;

    data::DecoderStack *stack = trace->decoder();
    std::list<data::decode::Decoder*> &list = stack->stack();

    o["index"] = index;
    o["label"] = trace->get_name();

    QJsonArray sub;
    for (data::decode::Decoder *dec : list){
        if (dec == list.front()){
            const srd_decoder *d = dec->decoder();
            o["id"] = QString::fromUtf8(d->id);

            QJsonObject channels;
            for (const srd_channel *ch : dec->binded_probe_list())
                channels[ch->id] = dec->binded_probe_index(ch);
            o["channels"] = channels;
            o["options"] = options_json(dec);
        }
        else{
            QJsonObject s;
            s["id"] = QString::fromUtf8(dec->decoder()->id);
            s["options"] = options_json(dec);
            sub.append(s);
        }
    }
    o["stack"] = sub;

    const bool pending = capturing || session->is_decode_pending(trace);
    o["decoding"] = pending;
    o["progress"] = stack->get_progress();
    if (!stack->error_message().isEmpty())
        o["error"] = stack->error_message();
    o["format"] = format_name(stack->decoder_status()->m_format);

    QJsonArray rows;
    int i = 0;
    for (const data::decode::Row &row : stack->all_rows())
        rows.append(row_json(stack, row, i++));
    o["rows"] = rows;

    return o;
}

bool build(SigSession *session, const QJsonObject &params, QJsonObject &config, QString &error)
{
    const QString id = params.value("id").toString();
    const srd_decoder *dec = find(id);
    if (dec == NULL){
        error = id.isEmpty() ? QString("\"id\" is required: a decoder id from decoder.catalog.")
                             : QString("There is no decoder \"%1\" (see decoder.catalog).").arg(id);
        return false;
    }
    if (!takes_input(dec, "logic")){
        error = QString("%1 decodes the output of another decoder. Add one that reads "
                        "logic signals and put %1 in its \"stack\".").arg(id);
        return false;
    }

    // Channels
    QJsonValue chv = params.value("channels");
    if (!chv.isObject()){
        error = QString("\"channels\" must map the channels of %1 to logic channels, "
                        "e.g. {\"%2\": 0}. Channels: %3.")
                    .arg(id, channel_ids(dec).value(0, "ch"), channel_ids(dec).join(", "));
        return false;
    }

    QJsonObject channels = chv.toObject();
    QJsonArray channel_array;
    for (auto it = channels.begin(); it != channels.end(); ++it){
        bool required;
        if (find_channel(dec, it.key(), required) == NULL){
            error = QString("%1 has no channel \"%2\". Channels: %3.")
                        .arg(id, it.key(), channel_ids(dec).join(", "));
            return false;
        }
        const double v = it.value().toDouble(-1);
        if (!it.value().isDouble() || v < 0 || v != std::floor(v)
            || !logic_channel_enabled(session, (int)v)){
            error = QString("Channel \"%1\" must be an enabled logic channel (channel.list).").arg(it.key());
            return false;
        }
        QJsonObject c;
        c[it.key()] = (int)v;
        channel_array.append(c);
    }

    QStringList missing;
    for (const GSList *l = dec->channels; l; l = l->next){
        const srd_channel *ch = (const srd_channel *)l->data;
        if (!channels.contains(QString::fromUtf8(ch->id)))
            missing << QString::fromUtf8(ch->id);
    }
    if (!missing.isEmpty()){
        error = QString("%1 needs the channels %2.").arg(id, missing.join(", "));
        return false;
    }

    QJsonObject options;
    if (!check_options(dec, params.value("options"), options, error))
        return false;

    // Stacked decoders, each reading the output of the one before
    QJsonValue stv = params.value("stack");
    QJsonArray stack_array;
    if (!stv.isUndefined() && !stv.isNull()){
        if (!stv.isArray()){
            error = "\"stack\" must be a list of decoders, e.g. [{\"id\": \"eeprom24xx\"}].";
            return false;
        }

        const srd_decoder *lower = dec;
        for (const QJsonValue &v : stv.toArray()){
            QJsonObject s = v.isString() ? QJsonObject{{"id", v.toString()}} : v.toObject();
            const QString sid = s.value("id").toString();
            const srd_decoder *upper = find(sid);
            if (upper == NULL){
                error = QString("There is no decoder \"%1\" (see decoder.catalog).").arg(sid);
                return false;
            }
            if (!stacks_on(upper, lower)){
                QStringList outputs;
                for (const GSList *l = lower->outputs; l; l = l->next)
                    outputs << QString::fromUtf8((const char *)l->data);
                error = QString("%1 can't decode the output of %2 (%3).")
                            .arg(sid, lower->id, outputs.isEmpty() ? "it has none" : outputs.join(", "));
                return false;
            }

            QJsonObject sub_options;
            if (!check_options(upper, s.value("options"), sub_options, error))
                return false;

            QJsonObject entry;
            entry["id"] = sid;
            entry["options"] = sub_options;
            stack_array.append(entry);
            lower = upper;
        }
    }

    config = QJsonObject();
    config["id"] = id;
    config["channel"] = channel_array;
    config["options"] = options;
    config["stacked decoders"] = stack_array;

    QJsonValue label = params.value("label");
    if (label.isString() && !label.toString().trimmed().isEmpty())
        config["label"] = label.toString().trimmed();
    else if (!label.isUndefined() && !label.isNull()){
        error = "\"label\" must be a non-empty string.";
        return false;
    }

    return true;
}

bool find_row(data::DecoderStack *stack, const QJsonObject &params,
              data::decode::Row &row, int &row_index, QString &error)
{
    const std::vector<data::decode::Row> rows = stack->all_rows();
    const QJsonValue rv = params.value("row");
    const QString decoder = params.value("decoder").toString();

    if (rv.isDouble()){
        const double v = rv.toDouble();
        if (v < 0 || v != std::floor(v) || v >= rows.size()){
            error = QString("\"row\" must be from 0 to %1 (decoder.list).").arg((int)rows.size() - 1);
            return false;
        }
        row_index = (int)v;
        row = rows[row_index];
        return true;
    }

    if (!rv.isString()){
        error = "\"row\" is required: a row number or id from decoder.list.";
        return false;
    }

    QStringList matches;
    for (int i = 0; i < (int)rows.size(); i++){
        const data::decode::Row &r = rows[i];
        const QString rid = r.row() != NULL ? QString::fromUtf8(r.row()->id)
                                            : QString::fromUtf8(r.decoder()->id);
        const QString did = QString::fromUtf8(r.decoder()->id);
        if (rid != rv.toString() || (!decoder.isEmpty() && did != decoder))
            continue;
        matches << did;
        row_index = i;
        row = r;
    }

    if (matches.size() == 1)
        return true;
    if (matches.isEmpty())
        error = QString("There is no row \"%1\"%2 (decoder.list).")
                    .arg(rv.toString(), decoder.isEmpty() ? "" : " in " + decoder);
    else
        error = QString("Several decoders have a row \"%1\" (%2); say which with \"decoder\".")
                    .arg(rv.toString(), matches.join(", "));
    return false;
}

QJsonObject annotation_json(const data::decode::Row &row,
                            const data::decode::Annotation &ann, double samplerate)
{
    QJsonObject o;
    o["start"] = (double)ann.start_sample();
    o["end"] = (double)ann.end_sample();
    if (samplerate > 0)
        o["time"] = ann.start_sample() / samplerate;

    const std::vector<QString> &texts = ann.annotations();
    o["text"] = texts.empty() ? QString() : texts.front();

    const QString cls = class_id(row.decoder(), ann.format());
    if (!cls.isEmpty())
        o["class"] = cls;
    return o;
}

QString format_name(int format)
{
    switch (format){
        case DecoderDataFormat::hex:   return "hex";
        case DecoderDataFormat::dec:   return "dec";
        case DecoderDataFormat::oct:   return "oct";
        case DecoderDataFormat::bin:   return "bin";
        case DecoderDataFormat::ascii: return "ascii";
    }
    return "hex";
}

} // namespace decoders
} // namespace control
} // namespace pv
