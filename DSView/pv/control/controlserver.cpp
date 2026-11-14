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

#include "controlserver.h"

#include <cmath>
#include <algorithm>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <glib.h>
#include <libsigrok.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "controlmode.h"
#include "../sigsession.h"
#include "../deviceagent.h"
#include "../storesession.h"
#include "../toolbars/samplingbar.h"
#include "../appcontrol.h"
#include "../log.h"
#include "DSView/config.h"

namespace pv {
namespace control {

namespace {

// JSON-RPC 2.0 error codes, plus our own (positive) ones. Listed in
// doc/control-interface.md.
enum ErrorCode
{
    ErrParse            = -32700,
    ErrInvalidRequest   = -32600,
    ErrMethodNotFound   = -32601,
    ErrInvalidParams    = -32602,
    ErrInternal         = -32603,

    ErrBusy             = 1,
    ErrNoDevice         = 2,
    ErrNotSupported     = 3,
    ErrTimeout          = 4,
    ErrCaptureFailed    = 5,
    ErrExportFailed     = 6,
    ErrDialogOpen       = 7,
    ErrNoData           = 8,
};

const int MaxLineLength = 1024 * 1024;
const int ExportPollInterval = 50; // ms

const char *mode_name(int mode)
{
    switch (mode){
        case LOGIC:  return "logic";
        case DSO:    return "dso";
        case ANALOG: return "analog";
    }
    return "unknown";
}

// "100M", "100MHz", "1.8V", "10ms", "2e6" or a JSON number. A bare number
// is in the base unit (Hz, V, s, samples). Prefixes: p n u m k M G, so
// "m" is milli and "M" is mega. The unit itself is optional.
bool parse_quantity(const QJsonValue &value, const QString &unit, double &out)
{
    if (value.isDouble()){
        out = value.toDouble();
        return std::isfinite(out);
    }

    if (!value.isString())
        return false;

    static const QRegularExpression re(
        "^\\s*([-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?)\\s*([pnuµmkKMG]?)\\s*([A-Za-z]*)\\s*$");

    QRegularExpressionMatch m = re.match(value.toString());
    if (!m.hasMatch())
        return false;

    QString suffix = m.captured(3);
    if (!suffix.isEmpty() && suffix.compare(unit, Qt::CaseInsensitive) != 0)
        return false;

    double v = m.captured(1).toDouble();
    QString prefix = m.captured(2);

    if (prefix == "p") v *= 1e-12;
    else if (prefix == "n") v *= 1e-9;
    else if (prefix == "u" || prefix == "µ") v *= 1e-6;
    else if (prefix == "m") v *= 1e-3;
    else if (prefix == "k" || prefix == "K") v *= 1e3;
    else if (prefix == "M") v *= 1e6;
    else if (prefix == "G") v *= 1e9;

    out = v;
    return std::isfinite(out);
}

bool parse_bool(const QJsonValue &value, bool &out)
{
    if (value.isBool()){
        out = value.toBool();
        return true;
    }
    if (value.isDouble()){
        out = value.toDouble() != 0;
        return true;
    }
    if (value.isString()){
        QString s = value.toString().trimmed().toLower();
        if (s == "true" || s == "on" || s == "yes" || s == "1"){
            out = true;
            return true;
        }
        if (s == "false" || s == "off" || s == "no" || s == "0"){
            out = false;
            return true;
        }
    }
    return false;
}

// Optional non-negative integer parameter.
bool param_uint64(const QJsonObject &params, const char *name, uint64_t &out, bool &present)
{
    present = params.contains(name) && !params.value(name).isNull();
    if (!present)
        return true;

    double v;
    if (!parse_quantity(params.value(name), "", v) || v < 0 || v != std::floor(v))
        return false;

    out = (uint64_t)v;
    return true;
}

// Optional timeout in seconds; 0 or absent means no timeout.
bool param_timeout(const QJsonObject &params, double &out)
{
    out = 0;
    if (!params.contains("timeout") || params.value("timeout").isNull())
        return true;
    return parse_quantity(params.value("timeout"), "s", out) && out >= 0;
}

QString rate_string(uint64_t rate)
{
    char *s = sr_samplerate_string(rate);
    QString str(s);
    g_free(s);
    return str;
}

// Not sr_time_string(): that follows the locale's decimal separator.
QString time_string(double ns)
{
    const double s = ns / SR_SEC(1);
    if (s >= 1)
        return QString::number(s, 'g', 6) + " s";
    if (s >= 1e-3)
        return QString::number(s * 1e3, 'g', 6) + " ms";
    if (s >= 1e-6)
        return QString::number(s * 1e6, 'g', 6) + " us";
    return QString::number(ns, 'g', 6) + " ns";
}

} // namespace

// A device option the control interface can read and (unless read_only)
// set: the options the device options dialog shows, under stable names.
struct ControlServer::ConfigOption
{
    int         key;
    QString     id;
    QString     label;
    bool        read_only;
};

namespace {

struct OptionName
{
    int         key;
    const char  *id;
    bool        read_only;
};

// Order matters: config.set applies options in this order, because the
// operation and channel modes change which values the others accept.
const OptionName option_names[] = {
    {SR_CONF_OPERATION_MODE, "operation_mode", false},
    {SR_CONF_CHANNEL_MODE,   "channel_mode",   false},
    {SR_CONF_BUFFER_OPTIONS, "stop_option",    false},
    {SR_CONF_THRESHOLD,      "threshold",      false},
    {SR_CONF_VTH,            "vth",            false},
    {SR_CONF_FILTER,         "filter",         false},
    {SR_CONF_RLE_SUPPORT,    "rle_compress",   false},
    {SR_CONF_CLOCK_TYPE,     "external_clock", false},
    {SR_CONF_CLOCK_EDGE,     "clock_negedge",  false},
    {SR_CONF_EXT_SAMPLERATE, "ext_clock_rate", false},
    {SR_CONF_MAX_HEIGHT,     "max_height",     false},
    // Changing the demo pattern reloads demo data; not done here yet.
    {SR_CONF_PATTERN_MODE,   "pattern",        true},
};

// Keys the device options dialog also offers while stacking; they are
// written to both analyzers.
bool stacking_shared_option(int key)
{
    return key == SR_CONF_VTH ||
           key == SR_CONF_FILTER ||
           key == SR_CONF_CLOCK_TYPE ||
           key == SR_CONF_CLOCK_EDGE ||
           key == SR_CONF_EXT_SAMPLERATE;
}

} // namespace

ControlServer::ControlServer(SigSession *session, toolbars::SamplingBar *sampling_bar,
                             QObject *parent) :
    QObject(parent),
    _session(session),
    _device(session->get_device()),
    _sampling_bar(sampling_bar),
    _stop_requested(false),
    _trigger_armed(false)
{
    // After MainWindow, so the GUI has dealt with a message before a
    // client hears about it.
    _session->add_msg_listener(this);

    connect(&_server, SIGNAL(newConnection()), this, SLOT(on_new_connection()));
}

ControlServer::~ControlServer()
{
    _session->remove_msg_listener(this);

    for (Waiter &w : _waiters)
        delete w.timer;
    _waiters.clear();

    if (_export){
        _export->timer.stop();
        _export->store->cancel();
        _export->store->wait();
        _session->set_saving(false);
        _export.reset();
    }

    _server.close();
}

bool ControlServer::listen(const QString &name)
{
    // Check for another DSView first: with socket options set, Qt binds
    // elsewhere and renames the socket into place, silently replacing a
    // live one instead of failing.
    {
        QLocalSocket probe;
        probe.connectToServer(name);
        if (probe.waitForConnected(500)){
            probe.disconnectFromServer();
            dsv_err("Control server: another DSView already listens on %s; use --control=NAME.",
                    name.toUtf8().data());
            return false;
        }
    }

#ifndef _WIN32
    // Nobody answers, so a socket file there was left by a DSView that
    // crashed. Anything else at that path is not ours to replace.
    struct stat st;
    QByteArray path = QFile::encodeName(name);
    if (::lstat(path.constData(), &st) == 0){
        if (!S_ISSOCK(st.st_mode)){
            dsv_err("Control server: %s exists and is not a socket.", name.toUtf8().data());
            return false;
        }
        QLocalServer::removeServer(name);
        dsv_info("Control server: removed the stale socket %s", name.toUtf8().data());
    }
#endif

    _server.setSocketOptions(QLocalServer::UserAccessOption);

    if (_server.listen(name)){
        dsv_info("Control server listening on: %s", name.toUtf8().data());
        return true;
    }

    QString why = _server.errorString();
    if (why.isEmpty())
        why = "unknown error";
#ifndef _WIN32
    // sockaddr_un holds about 100 bytes; Qt says no more than "invalid name".
    if (name.toLocal8Bit().size() > 100)
        why += " (the socket path is too long; Unix sockets allow about 100 bytes)";
#endif
    dsv_err("Control server: failed to listen on %s: %s", name.toUtf8().data(),
            why.toUtf8().data());
    return false;
}

//------------------------------------------------------------- connections

void ControlServer::on_new_connection()
{
    while (QLocalSocket *socket = _server.nextPendingConnection()){
        _buffers.insert(socket, QByteArray());
        connect(socket, SIGNAL(readyRead()), this, SLOT(on_ready_read()));
        connect(socket, SIGNAL(disconnected()), this, SLOT(on_disconnected()));
        dsv_dbg("Control server: client connected.");
    }
}

void ControlServer::on_disconnected()
{
    QLocalSocket *socket = qobject_cast<QLocalSocket*>(sender());
    if (socket == NULL)
        return;

    _buffers.remove(socket);

    // Nobody to tell any more. A running capture keeps going.
    for (int i = _waiters.size() - 1; i >= 0; i--){
        if (_waiters[i].call.socket == socket){
            delete _waiters[i].timer;
            _waiters.removeAt(i);
        }
    }

    socket->deleteLater();
    dsv_dbg("Control server: client disconnected.");
}

void ControlServer::on_ready_read()
{
    QLocalSocket *socket = qobject_cast<QLocalSocket*>(sender());
    if (socket == NULL || !_buffers.contains(socket))
        return;

    _buffers[socket].append(socket->readAll());

    // Look the buffer up again for every line: a handler may run a nested
    // event loop (a dialog), in which clients connect and disconnect.
    while (true){
        auto it = _buffers.find(socket);
        if (it == _buffers.end())
            return;

        int pos = it->indexOf('\n');
        if (pos < 0)
            break;

        QByteArray line = it->left(pos).trimmed();
        it->remove(0, pos + 1);

        if (!line.isEmpty())
            handle_line(socket, line);
    }

    if (_buffers.value(socket).size() > MaxLineLength){
        Call call;
        call.socket = socket;
        call.id = QJsonValue::Null;
        reply_error(call, ErrInvalidRequest, "Request line too long.");
        socket->disconnectFromServer();
    }
}

void ControlServer::handle_line(QLocalSocket *socket, const QByteArray &line)
{
    Call call;
    call.socket = socket;
    call.id = QJsonValue::Null;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(line, &err);

    if (err.error != QJsonParseError::NoError){
        reply_error(call, ErrParse, "Invalid JSON: " + err.errorString());
        return;
    }
    if (!doc.isObject()){
        reply_error(call, ErrInvalidRequest, "Expected one request object per line (no batches).");
        return;
    }

    QJsonObject req = doc.object();
    call.id = req.contains("id") ? req.value("id") : QJsonValue(QJsonValue::Undefined);

    QJsonValue method = req.value("method");
    if (!method.isString()){
        if (call.id.isUndefined())
            call.id = QJsonValue::Null;
        reply_error(call, ErrInvalidRequest, "Missing \"method\".");
        return;
    }

    QJsonValue params = req.value("params");
    if (!params.isUndefined() && !params.isNull() && !params.isObject()){
        reply_error(call, ErrInvalidParams, "\"params\" must be an object.");
        return;
    }

    dsv_dbg("Control server: %s", line.constData());
    dispatch(call, method.toString(), params.toObject());
}

void ControlServer::dispatch(const Call &call, const QString &method, const QJsonObject &params)
{
    typedef void (ControlServer::*Handler)(const Call&, const QJsonObject&);
    static const struct { const char *name; Handler handler; } methods[] = {
        {"server.info",     &ControlServer::m_server_info},
        {"server.quit",     &ControlServer::m_server_quit},
        {"device.list",     &ControlServer::m_device_list},
        {"device.select",   &ControlServer::m_device_select},
        {"config.list",     &ControlServer::m_config_list},
        {"config.get",      &ControlServer::m_config_get},
        {"config.set",      &ControlServer::m_config_set},
        {"channel.list",    &ControlServer::m_channel_list},
        {"channel.set",     &ControlServer::m_channel_set},
        {"capture.start",   &ControlServer::m_capture_start},
        {"capture.wait",    &ControlServer::m_capture_wait},
        {"capture.status",  &ControlServer::m_capture_status},
        {"capture.stop",    &ControlServer::m_capture_stop},
        {"export",          &ControlServer::m_export},
    };

    for (const auto &m : methods){
        if (method == m.name){
            (this->*m.handler)(call, params);
            return;
        }
    }

    QJsonArray names;
    for (const auto &m : methods)
        names.append(m.name);

    QJsonObject data;
    data["methods"] = names;
    reply_error(call, ErrMethodNotFound, "Unknown method: " + method, data);
}

void ControlServer::send(const Call &call, const QJsonObject &msg)
{
    if (call.id.isUndefined())
        return; // a notification
    if (call.socket.isNull() || call.socket->state() != QLocalSocket::ConnectedState)
        return;

    QByteArray out = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    out.append('\n');
    call.socket->write(out);
    call.socket->flush();
}

void ControlServer::reply(const Call &call, QJsonObject result)
{
    QStringList messages = ControlMode::TakeSuppressedDialogs();
    if (!messages.isEmpty())
        result["messages"] = QJsonArray::fromStringList(messages);

    QJsonObject msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = call.id;
    msg["result"] = result;
    send(call, msg);
}

void ControlServer::reply_error(const Call &call, int code, const QString &message, QJsonObject data)
{
    QStringList messages = ControlMode::TakeSuppressedDialogs();
    if (!messages.isEmpty())
        data["messages"] = QJsonArray::fromStringList(messages);

    QJsonObject error;
    error["code"] = code;
    error["message"] = message;
    if (!data.isEmpty())
        error["data"] = data;

    QJsonObject msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = call.id;
    msg["error"] = error;
    send(call, msg);
}

bool ControlServer::check_not_file(const Call &call)
{
    if (_device->have_instance() && _device->is_file()){
        reply_error(call, ErrNotSupported,
            "A data file is open, not a device. Select a device first (device.select).");
        return false;
    }
    return true;
}

bool ControlServer::check_ready(const Call &call, bool need_logic, bool allow_running)
{
    if (!ControlMode::IsNonInteractive() && QApplication::activeModalWidget() != NULL){
        reply_error(call, ErrDialogOpen,
            "A dialog is open in DSView. Close it, or start DSView with --non-interactive.");
        return false;
    }
    if (_export){
        reply_error(call, ErrBusy, "An export is running.");
        return false;
    }
    if (_session->is_doing_action() || _session->is_saving()){
        reply_error(call, ErrBusy, "DSView is busy with another action.");
        return false;
    }
    if (!allow_running && _session->is_working()){
        reply_error(call, ErrBusy, "A capture is running. Stop it first (capture.stop).");
        return false;
    }
    if (!_device->have_instance()){
        reply_error(call, ErrNoDevice, "No device is selected.");
        return false;
    }
    if (need_logic && _device->get_work_mode() != LOGIC){
        QJsonObject data;
        data["mode"] = mode_name(_device->get_work_mode());
        reply_error(call, ErrNotSupported,
            "Only logic analyzer mode is supported by the control interface so far.", data);
        return false;
    }
    return true;
}

//------------------------------------------------------------- state

QString ControlServer::state_name()
{
    if (_export)
        return "exporting";
    if (_session->is_working())
        return "running";
    return "idle";
}

QJsonObject ControlServer::device_info()
{
    QJsonObject dev;

    if (!_device->have_instance())
        return dev;

    dev["name"] = _device->name();
    dev["driver"] = _device->driver_name();
    dev["type"] = _device->is_hardware() ? "hardware" : _device->is_demo() ? "demo" : "file";
    dev["mode"] = mode_name(_device->get_work_mode());
    dev["stacking"] = _device->is_logic_stacking();

    if (_device->is_file())
        dev["path"] = _device->path();

    return dev;
}

QJsonObject ControlServer::capture_status()
{
    QJsonObject st;
    st["state"] = state_name();

    if (!_device->have_instance())
        return st;

    const bool running = _session->is_working();
    const bool have_data = !running && _session->have_view_data();

    if (running || have_data)
        st["trigger_armed"] = _trigger_armed;

    if (running && _device->is_hardware() && _device->get_work_mode() == LOGIC){
        bool triggered = false;
        int progress = 0;
        if (_session->get_capture_status(triggered, progress)){
            st["triggered"] = _trigger_armed && triggered;
            st["progress"] = progress;
        }
    }

    st["has_data"] = have_data;

    if (have_data){
        const uint64_t rate = _session->cur_snap_samplerate();
        const int64_t samples = _session->get_ring_sample_count();
        // The hardware's trigger report; without an armed trigger, the
        // position is just where the data starts.
        const bool triggered = _trigger_armed && _session->trigd();

        st["triggered"] = triggered;
        st["samples"] = (double)samples;
        st["samplerate"] = (double)rate;
        if (rate > 0)
            st["duration"] = (double)samples / (double)rate;
        if (triggered)
            st["trigger_sample"] = (double)_session->get_trigger_pos();
    }

    if (_stop_requested)
        st["stopped"] = true;
    if (!_capture_error.isEmpty())
        st["error"] = _capture_error;

    return st;
}

void ControlServer::OnMessage(int msg)
{
    switch (msg)
    {
        // Also for captures started from the toolbar, so the status always
        // describes the latest one.
        case DSV_MSG_START_COLLECT_WORK_PREV:
            _capture_error.clear();
            _stop_requested = false;
            // MainWindow has committed the trigger dock's settings by now.
            // Instant captures reset the trigger instead.
            _trigger_armed = !_session->is_instant() && ds_trigger_get_en() != 0;
            break;

        case DSV_MSG_END_COLLECT_WORK:
            finish_waiters();
            break;

        case DSV_MSG_CURRENT_DEVICE_DETACHED:
            if (!_waiters.isEmpty()){
                _capture_error = "The device was disconnected.";
                finish_waiters();
            }
            break;
    }
}

void ControlServer::notify_session_error(const QString &title, const QString &details)
{
    QString err = title;
    if (!details.isEmpty())
        err += ": " + QString(details).replace('\n', ' ');
    _capture_error = err;
}

//------------------------------------------------------------- server/device

void ControlServer::m_server_info(const Call &call, const QJsonObject &params)
{
    (void)params;

    QJsonObject r;
    r["name"] = "DSView";
    r["version"] = DS_VERSION_STRING;
    r["protocol"] = ProtocolVersion;
    r["pid"] = (double)QCoreApplication::applicationPid();
    r["socket"] = _server.fullServerName();
    r["headless"] = ControlMode::IsHeadless();
    r["non_interactive"] = ControlMode::IsNonInteractive();
    r["state"] = state_name();
    r["device"] = _device->have_instance() ? QJsonValue(device_info()) : QJsonValue();
    reply(call, r);
}

void ControlServer::m_server_quit(const Call &call, const QJsonObject &params)
{
    (void)params;

    if (_session->is_working()){
        _stop_requested = true;
        _session->stop_capture();
    }

    QJsonObject r;
    r["quitting"] = true;
    reply(call, r);

    // After the reply is out. Closing the main window takes the normal exit
    // path, which saves the device profile and releases the device.
    QTimer::singleShot(0, qApp, [](){
        QWidget *top = AppControl::Instance()->GetTopWindow();
        if (top != NULL)
            top->close();
        else
            QCoreApplication::quit();
    });
}

void ControlServer::m_device_list(const Call &call, const QJsonObject &params)
{
    (void)params;

    int count = 0;
    int active = -1;
    struct ds_device_base_info *list = _session->get_device_list(count, active);

    QJsonArray devices;
    for (int i = 0; list != NULL && i < count; i++){
        QJsonObject d;
        d["index"] = i;
        d["name"] = QString::fromLocal8Bit(list[i].name);
        QString uid = QString::fromLocal8Bit(list[i].unique_id).trimmed();
        if (!uid.isEmpty())
            d["unique_id"] = uid;
        d["active"] = (i == active);
        devices.append(d);
    }
    if (list != NULL)
        g_free(list);

    QJsonObject r;
    r["devices"] = devices;
    reply(call, r);
}

void ControlServer::m_device_select(const Call &call, const QJsonObject &params)
{
    if (!ControlMode::IsNonInteractive() && QApplication::activeModalWidget() != NULL){
        reply_error(call, ErrDialogOpen,
            "A dialog is open in DSView. Close it, or start DSView with --non-interactive.");
        return;
    }
    if (_export || _session->is_doing_action() || _session->is_saving()){
        reply_error(call, ErrBusy, "DSView is busy with another action.");
        return;
    }
    if (_session->is_working()){
        reply_error(call, ErrBusy, "A capture is running. Stop it first (capture.stop).");
        return;
    }

    int count = 0;
    int active = -1;
    struct ds_device_base_info *list = _session->get_device_list(count, active);
    if (list == NULL){
        reply_error(call, ErrInternal, "Can't read the device list.");
        return;
    }

    int found = -1;

    if (params.contains("index")){
        double idx = params.value("index").toDouble(-1);
        if (idx >= 0 && idx < count && idx == std::floor(idx))
            found = (int)idx;
    }
    else if (params.value("name").isString()){
        // The name, the unique id, or "name [unique id]" as the GUI shows it.
        QString want = params.value("name").toString().trimmed();
        int matches = 0;
        for (int i = 0; i < count; i++){
            QString name = QString::fromLocal8Bit(list[i].name);
            QString uid = QString::fromLocal8Bit(list[i].unique_id).trimmed();
            QString shown = uid.isEmpty() ? name : QString("%1 [%2]").arg(name, uid);
            if (want.compare(name, Qt::CaseInsensitive) == 0 ||
                (!uid.isEmpty() && want.compare(uid, Qt::CaseInsensitive) == 0) ||
                want.compare(shown, Qt::CaseInsensitive) == 0){
                found = i;
                matches++;
            }
        }
        if (matches > 1){
            g_free(list);
            reply_error(call, ErrInvalidParams,
                "Several devices match that name; use the index or unique id.");
            return;
        }
    }
    else{
        g_free(list);
        reply_error(call, ErrInvalidParams, "Give \"index\" or \"name\".");
        return;
    }

    if (found < 0){
        g_free(list);
        reply_error(call, ErrInvalidParams, "No such device. See device.list.");
        return;
    }

    ds_device_handle handle = list[found].handle;
    g_free(list);

    if (handle != _device->handle()){
        if (!_sampling_bar->select_device(handle) || _device->handle() != handle){
            reply_error(call, ErrInternal, "The device could not be opened.");
            return;
        }
        _stop_requested = false;
        _capture_error.clear();
    }

    QJsonObject r;
    r["device"] = device_info();
    reply(call, r);
}

//------------------------------------------------------------- config

std::vector<ControlServer::ConfigOption> ControlServer::device_options()
{
    std::vector<ConfigOption> list;

    if (!_device->have_instance())
        return list;

    std::vector<int> keys;
    GVariant *gvar_opts = _device->get_config_list(NULL, SR_CONF_DEVICE_OPTIONS);
    if (gvar_opts != NULL){
        gsize num = 0;
        const int32_t *opts = (const int32_t *)g_variant_get_fixed_array(
            gvar_opts, &num, sizeof(int32_t));
        for (gsize i = 0; i < num; i++)
            keys.push_back(opts[i]);
        g_variant_unref(gvar_opts);
    }

    // The options dialog shows the channel mode separately.
    if (_device->get_work_mode() == LOGIC){
        GVariant *gvar = _device->get_config_list(NULL, SR_CONF_CHANNEL_MODE);
        if (gvar != NULL){
            keys.push_back(SR_CONF_CHANNEL_MODE);
            g_variant_unref(gvar);
        }
    }

    for (const OptionName &n : option_names){
        if (std::find(keys.begin(), keys.end(), n.key) == keys.end())
            continue;
        if (_device->is_logic_stacking() && !stacking_shared_option(n.key))
            continue;

        const struct sr_config_info *info = _device->get_config_info(n.key);
        if (info == NULL)
            continue;

        ConfigOption opt;
        opt.key = n.key;
        opt.id = n.id;
        opt.label = info->name;
        opt.read_only = n.read_only;
        list.push_back(opt);
    }

    return list;
}

QJsonValue ControlServer::option_value(const ConfigOption &opt)
{
    GVariant *gvar = _device->get_config(opt.key);
    if (gvar == NULL)
        return QJsonValue();

    QJsonValue v;

    if (g_variant_is_of_type(gvar, G_VARIANT_TYPE_BOOLEAN))
        v = (bool)g_variant_get_boolean(gvar);
    else if (g_variant_is_of_type(gvar, G_VARIANT_TYPE_DOUBLE))
        v = g_variant_get_double(gvar);
    else if (g_variant_is_of_type(gvar, G_VARIANT_TYPE_UINT64))
        v = (double)g_variant_get_uint64(gvar);
    else if (g_variant_is_of_type(gvar, G_VARIANT_TYPE_STRING))
        v = QString::fromUtf8(g_variant_get_string(gvar, NULL));
    else if (g_variant_is_of_type(gvar, G_VARIANT_TYPE_INT16)){
        // A list option: report the item's name.
        int id = g_variant_get_int16(gvar);
        v = id;
        GVariant *gl = _device->get_config_list(NULL, opt.key);
        if (gl != NULL){
            const struct sr_list_item *p = (const struct sr_list_item*)g_variant_get_uint64(gl);
            for (; p != NULL && p->id >= 0; p++){
                if (p->id == id){
                    v = QString::fromUtf8(p->name);
                    break;
                }
            }
            g_variant_unref(gl);
        }
    }
    else if (g_variant_is_of_type(gvar, G_VARIANT_TYPE_BYTE))
        v = (int)g_variant_get_byte(gvar);

    g_variant_unref(gvar);
    return v;
}

QJsonObject ControlServer::option_json(const ConfigOption &opt, bool with_choices)
{
    QJsonObject o;
    o["key"] = opt.id;
    o["label"] = opt.label;
    o["value"] = option_value(opt);
    if (opt.read_only)
        o["read_only"] = true;

    const struct sr_config_info *info = _device->get_config_info(opt.key);
    int type = info ? info->datatype : 0;

    switch (type){
        case SR_T_BOOL:     o["type"] = "bool"; break;
        case SR_T_FLOAT:    o["type"] = "number"; break;
        case SR_T_UINT64:   o["type"] = "integer"; break;
        case SR_T_LIST:
        case SR_T_CHAR:     o["type"] = "choice"; break;
        default:            o["type"] = "other"; break;
    }

    // The ranges the device options dialog uses.
    if (opt.key == SR_CONF_VTH){
        o["unit"] = "V";
        o["min"] = 0.0;
        o["max"] = 5.0;
    }
    else if (opt.key == SR_CONF_EXT_SAMPLERATE){
        o["unit"] = "Hz";
        o["min"] = 10000.0;
        o["max"] = 1000000000.0;
    }

    if (!with_choices)
        return o;

    GVariant *gl = _device->get_config_list(NULL, opt.key);
    if (gl == NULL)
        return o;

    QJsonArray choices;
    if (type == SR_T_LIST && g_variant_is_of_type(gl, G_VARIANT_TYPE_UINT64)){
        const struct sr_list_item *p = (const struct sr_list_item*)g_variant_get_uint64(gl);
        for (; p != NULL && p->id >= 0; p++)
            choices.append(QString::fromUtf8(p->name));
    }
    else if (g_variant_is_of_type(gl, G_VARIANT_TYPE_STRING_ARRAY)){
        gsize n = 0;
        const gchar **strv = g_variant_get_strv(gl, &n);
        for (gsize i = 0; i < n; i++)
            choices.append(QString::fromUtf8(strv[i]));
        g_free(strv);
    }
    g_variant_unref(gl);

    if (!choices.isEmpty())
        o["choices"] = choices;

    return o;
}

bool ControlServer::set_option(const ConfigOption &opt, const QJsonValue &value, QString &error)
{
    if (opt.read_only){
        error = QString("\"%1\" can't be changed through the control interface.").arg(opt.id);
        return false;
    }

    const struct sr_config_info *info = _device->get_config_info(opt.key);
    const int type = info ? info->datatype : 0;
    GVariant *gvar = NULL;

    if (type == SR_T_BOOL){
        bool b;
        if (!parse_bool(value, b)){
            error = QString("\"%1\" needs true or false.").arg(opt.id);
            return false;
        }
        gvar = g_variant_new_boolean(b);
    }
    else if (opt.key == SR_CONF_VTH){
        double v;
        if (!parse_quantity(value, "V", v) || v < 0 || v > 5.0){
            error = "\"vth\" needs a voltage from 0 to 5 V.";
            return false;
        }
        gvar = g_variant_new_double(v);
    }
    else if (opt.key == SR_CONF_EXT_SAMPLERATE){
        double v;
        if (!parse_quantity(value, "Hz", v) || v < 10000 || v > 1e9){
            error = "\"ext_clock_rate\" needs a frequency from 10 kHz to 1 GHz.";
            return false;
        }
        gvar = g_variant_new_uint64((uint64_t)std::llround(v));
    }
    else if (type == SR_T_LIST){
        GVariant *gl = _device->get_config_list(NULL, opt.key);
        if (gl == NULL){
            error = QString("\"%1\" has no choices on this device.").arg(opt.id);
            return false;
        }
        const struct sr_list_item *p = (const struct sr_list_item*)g_variant_get_uint64(gl);
        QStringList names;
        int id = -1;
        for (; p != NULL && p->id >= 0; p++){
            names.append(QString::fromUtf8(p->name));
            if (value.isString() &&
                value.toString().trimmed().compare(QString::fromUtf8(p->name), Qt::CaseInsensitive) == 0)
                id = p->id;
            else if (value.isDouble() && value.toDouble() == p->id)
                id = p->id;
        }
        g_variant_unref(gl);

        if (id < 0){
            error = QString("\"%1\" must be one of: %2").arg(opt.id, names.join(", "));
            return false;
        }
        gvar = g_variant_new_int16(id);
    }
    else if (type == SR_T_CHAR){
        GVariant *gl = _device->get_config_list(NULL, opt.key);
        QStringList names;
        QString match;
        if (gl != NULL && g_variant_is_of_type(gl, G_VARIANT_TYPE_STRING_ARRAY)){
            gsize n = 0;
            const gchar **strv = g_variant_get_strv(gl, &n);
            for (gsize i = 0; i < n; i++){
                QString s = QString::fromUtf8(strv[i]);
                names.append(s);
                if (value.isString() && value.toString().trimmed().compare(s, Qt::CaseInsensitive) == 0)
                    match = s;
            }
            g_free(strv);
        }
        if (gl != NULL)
            g_variant_unref(gl);

        if (match.isEmpty()){
            error = QString("\"%1\" must be one of: %2").arg(opt.id, names.join(", "));
            return false;
        }
        gvar = g_variant_new_string(match.toUtf8().data());
    }
    else{
        error = QString("\"%1\" can't be set through the control interface.").arg(opt.id);
        return false;
    }

    // As the device options dialog does it.
    bool ok = _device->is_logic_stacking() ? _device->set_stacking_shared_config(opt.key, gvar)
                                           : _device->set_config(opt.key, gvar);
    if (!ok){
        error = QString("The device rejected \"%1\".").arg(opt.id);
        return false;
    }

    // Switching the channel mode changes how many channels may be on;
    // switch off the surplus like the options dialog does.
    if (opt.key == SR_CONF_CHANNEL_MODE){
        int vld = 0;
        if (_device->get_config_int16(SR_CONF_VLD_CH_NUM, vld) && vld > 0){
            int on = 0;
            for (const GSList *l = _device->get_channels(); l; l = l->next){
                sr_channel *probe = (sr_channel*)l->data;
                if (probe->enabled && ++on > vld)
                    probe->enabled = false;
            }
        }
    }

    return true;
}

QJsonObject ControlServer::current_values()
{
    QJsonObject v;

    if (!_device->have_instance())
        return v;

    if (_device->get_work_mode() == LOGIC){
        const uint64_t rate = _sampling_bar->selected_sample_rate();
        const double duration = _sampling_bar->selected_duration();
        v["samplerate"] = (double)rate;
        v["duration"] = duration / SR_SEC(1);
        v["samples"] = (double)toolbars::SamplingBar::sample_count_for(duration, rate);
    }

    for (const ConfigOption &opt : device_options())
        v[opt.id] = option_value(opt);

    return v;
}

void ControlServer::m_config_list(const Call &call, const QJsonObject &params)
{
    (void)params;

    if (!check_ready(call, true, true))
        return;

    QJsonArray options;

    // Sample rate and depth, as offered by the toolbar.
    const uint64_t rate = _sampling_bar->selected_sample_rate();

    QJsonObject sr;
    sr["key"] = "samplerate";
    sr["label"] = "Sample rate";
    sr["type"] = "number";
    sr["unit"] = "Hz";
    sr["value"] = (double)rate;
    QJsonArray rates;
    for (uint64_t r : _sampling_bar->sample_rate_choices())
        rates.append((double)r);
    sr["choices"] = rates;
    options.append(sr);

    QJsonObject du;
    du["key"] = "duration";
    du["label"] = "Capture duration";
    du["type"] = "number";
    du["unit"] = "s";
    du["value"] = _sampling_bar->selected_duration() / SR_SEC(1);
    QJsonArray durations;
    QJsonArray rle;
    for (const auto &c : _sampling_bar->duration_choices()){
        durations.append(c.duration / SR_SEC(1));
        if (c.rle)
            rle.append(c.duration / SR_SEC(1));
    }
    du["choices"] = durations;
    if (!rle.isEmpty())
        du["rle_choices"] = rle;
    du["description"] = "Rounded up to the next choice. Choices in rle_choices only fit with RLE compression.";
    options.append(du);

    QJsonObject sc;
    sc["key"] = "samples";
    sc["label"] = "Sample count";
    sc["type"] = "integer";
    sc["value"] = (double)toolbars::SamplingBar::sample_count_for(
        _sampling_bar->selected_duration(), rate);
    sc["description"] = "Sets the duration instead: the shortest choice that holds this many samples.";
    options.append(sc);

    for (const ConfigOption &opt : device_options())
        options.append(option_json(opt, true));

    QJsonObject r;
    r["options"] = options;
    reply(call, r);
}

void ControlServer::m_config_get(const Call &call, const QJsonObject &params)
{
    if (!check_ready(call, true, true))
        return;

    QJsonObject all = current_values();
    QJsonValue keys = params.value("keys");

    if (keys.isUndefined() || keys.isNull()){
        QJsonObject r;
        r["values"] = all;
        reply(call, r);
        return;
    }

    if (!keys.isArray()){
        reply_error(call, ErrInvalidParams, "\"keys\" must be a list of option names.");
        return;
    }

    QJsonObject values;
    for (const QJsonValue &k : keys.toArray()){
        QString key = k.toString();
        if (!all.contains(key)){
            QJsonObject data;
            data["keys"] = QJsonArray::fromStringList(all.keys());
            reply_error(call, ErrInvalidParams, "Unknown option: " + key, data);
            return;
        }
        values[key] = all.value(key);
    }

    QJsonObject r;
    r["values"] = values;
    reply(call, r);
}

void ControlServer::m_config_set(const Call &call, const QJsonObject &params)
{
    if (!check_ready(call, true) || !check_not_file(call))
        return;

    if (params.isEmpty()){
        reply_error(call, ErrInvalidParams, "Nothing to set. See config.list for the options.");
        return;
    }

    std::vector<ConfigOption> opts = device_options();

    // Check the names before changing anything.
    QStringList known;
    known << "samplerate" << "duration" << "samples";
    for (const ConfigOption &o : opts)
        known << o.id;

    for (const QString &key : params.keys()){
        if (!known.contains(key)){
            QJsonObject data;
            data["keys"] = QJsonArray::fromStringList(known);
            reply_error(call, ErrInvalidParams, "Unknown option: " + key, data);
            return;
        }
    }
    if (params.contains("duration") && params.contains("samples")){
        reply_error(call, ErrInvalidParams, "Give \"duration\" or \"samples\", not both.");
        return;
    }

    QJsonObject applied;
    auto fail = [&](const QString &msg){
        // Options before the failing one have been set already.
        QJsonObject data;
        data["applied"] = applied;
        data["values"] = current_values();
        reply_error(call, ErrInvalidParams, msg, data);
    };

    // Device options first, in option_names order: they change which rates
    // and durations exist.
    bool options_changed = false;
    for (const ConfigOption &opt : opts){
        if (!params.contains(opt.id))
            continue;

        QString err;
        if (!set_option(opt, params.value(opt.id), err)){
            if (options_changed)
                _sampling_bar->apply_device_options();
            fail(err);
            return;
        }
        applied[opt.id] = option_value(opt);
        options_changed = true;
    }

    if (options_changed)
        _sampling_bar->apply_device_options();

    if (params.contains("samplerate")){
        double v;
        if (!parse_quantity(params.value("samplerate"), "Hz", v) || v <= 0){
            fail("\"samplerate\" needs a frequency, e.g. 100M or 100MHz.");
            return;
        }
        const uint64_t rate = (uint64_t)std::llround(v);
        if (!_sampling_bar->select_sample_rate(rate)){
            QStringList names;
            for (uint64_t r : _sampling_bar->sample_rate_choices())
                names << rate_string(r);
            fail(QString("Sample rate %1 is not available. Choices: %2")
                     .arg(rate_string(rate), names.join(", ")));
            return;
        }
        // What the toolbar ended up with (Internal Test mode keeps its own).
        applied["samplerate"] = (double)_sampling_bar->selected_sample_rate();
    }

    if (params.contains("duration") || params.contains("samples")){
        const uint64_t rate = _sampling_bar->selected_sample_rate();
        double want_ns = 0;
        uint64_t want_samples = 0;
        const bool by_samples = params.contains("samples");

        if (by_samples){
            double v;
            if (!parse_quantity(params.value("samples"), "", v) || v < 1){
                fail("\"samples\" needs a positive count, e.g. 1M.");
                return;
            }
            want_samples = (uint64_t)std::ceil(v);
        }
        else{
            double v;
            if (!parse_quantity(params.value("duration"), "s", v) || v <= 0){
                fail("\"duration\" needs a time, e.g. 10ms.");
                return;
            }
            want_ns = v * SR_SEC(1);
        }

        // The shortest choice that is long enough. The list is longest first.
        std::vector<toolbars::SamplingBar::DurationChoice> choices = _sampling_bar->duration_choices();
        bool found = false;
        double pick = 0;
        for (const auto &c : choices){
            bool enough = by_samples
                ? toolbars::SamplingBar::sample_count_for(c.duration, rate) >= want_samples
                : c.duration >= want_ns * (1 - 1e-9);
            if (enough){
                pick = c.duration;
                found = true;
            }
        }

        if (!found){
            const double max = choices.empty() ? 0 : choices.front().duration;
            fail(QString("Too long for this sample rate; the most is %1 (%2 samples).")
                     .arg(time_string(max))
                     .arg(toolbars::SamplingBar::sample_count_for(max, rate)));
            return;
        }

        _sampling_bar->select_duration(pick);
        const double actual = _sampling_bar->selected_duration();
        applied["duration"] = actual / SR_SEC(1);
        applied["samples"] = (double)toolbars::SamplingBar::sample_count_for(
            actual, _sampling_bar->selected_sample_rate());
    }

    QJsonObject r;
    r["applied"] = applied;
    r["values"] = current_values();
    reply(call, r);
}

//------------------------------------------------------------- channels

void ControlServer::m_channel_list(const Call &call, const QJsonObject &params)
{
    (void)params;

    if (!check_ready(call, true, true))
        return;

    QJsonArray channels;
    for (const GSList *l = _device->get_channels(); l; l = l->next){
        const sr_channel *probe = (const sr_channel*)l->data;
        QJsonObject ch;
        ch["index"] = probe->index;
        ch["name"] = QString::fromUtf8(probe->name);
        ch["enabled"] = (bool)probe->enabled;
        channels.append(ch);
    }

    QJsonObject r;
    r["channels"] = channels;

    int vld = 0;
    if (!_device->is_logic_stacking() && _device->get_config_int16(SR_CONF_VLD_CH_NUM, vld))
        r["max_enabled"] = vld;
    if (_device->is_logic_stacking())
        r["read_only"] = true;

    reply(call, r);
}

void ControlServer::m_channel_set(const Call &call, const QJsonObject &params)
{
    if (!check_ready(call, true) || !check_not_file(call))
        return;

    if (_device->is_logic_stacking()){
        reply_error(call, ErrNotSupported,
            "Channels can't be changed while stacking; all stacked channels stay on.");
        return;
    }

    std::vector<sr_channel*> probes;
    for (const GSList *l = _device->get_channels(); l; l = l->next)
        probes.push_back((sr_channel*)l->data);

    auto find = [&](int index) -> sr_channel* {
        for (sr_channel *p : probes)
            if (p->index == index)
                return p;
        return NULL;
    };

    // The new enable state, checked before anything changes.
    std::vector<bool> enable;
    for (sr_channel *p : probes)
        enable.push_back(p->enabled);

    sr_channel *named = NULL;
    QString new_name;

    if (params.contains("enabled") && params.value("enabled").isArray()){
        // Exactly these channels on, the rest off.
        if (params.contains("index") || params.contains("name")){
            reply_error(call, ErrInvalidParams,
                "Give a list of channels in \"enabled\", or one \"index\" with \"enabled\"/\"name\".");
            return;
        }
        std::fill(enable.begin(), enable.end(), false);
        for (const QJsonValue &v : params.value("enabled").toArray()){
            double idx = v.toDouble(-1);
            sr_channel *p = (idx == std::floor(idx)) ? find((int)idx) : NULL;
            if (p == NULL){
                reply_error(call, ErrInvalidParams, "No such channel: " +
                    QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact)));
                return;
            }
            enable[std::find(probes.begin(), probes.end(), p) - probes.begin()] = true;
        }
    }
    else if (params.contains("index")){
        double idx = params.value("index").toDouble(-1);
        sr_channel *p = (idx == std::floor(idx)) ? find((int)idx) : NULL;
        if (p == NULL){
            reply_error(call, ErrInvalidParams, "No such channel. See channel.list.");
            return;
        }
        if (params.contains("enabled")){
            bool b;
            if (!parse_bool(params.value("enabled"), b)){
                reply_error(call, ErrInvalidParams, "\"enabled\" needs true or false.");
                return;
            }
            enable[std::find(probes.begin(), probes.end(), p) - probes.begin()] = b;
        }
        if (params.contains("name")){
            new_name = params.value("name").toString().trimmed();
            if (new_name.isEmpty()){
                reply_error(call, ErrInvalidParams, "\"name\" can't be empty.");
                return;
            }
            named = p;
        }
        if (!params.contains("enabled") && !params.contains("name")){
            reply_error(call, ErrInvalidParams, "Nothing to change: give \"enabled\" or \"name\".");
            return;
        }
    }
    else{
        reply_error(call, ErrInvalidParams,
            "Give a list of channels in \"enabled\", or one \"index\" with \"enabled\"/\"name\".");
        return;
    }

    const int on = (int)std::count(enable.begin(), enable.end(), true);
    int vld = 0;
    if (on == 0){
        reply_error(call, ErrInvalidParams, "At least one channel must stay enabled.");
        return;
    }
    if (_device->get_config_int16(SR_CONF_VLD_CH_NUM, vld) && vld > 0 && on > vld){
        reply_error(call, ErrInvalidParams,
            QString("At most %1 channels can be on in this channel mode.").arg(vld));
        return;
    }

    // As the device options dialog commits them.
    for (size_t i = 0; i < probes.size(); i++)
        probes[i]->enabled = enable[i];

    if (named != NULL)
        _device->set_channel_name(named->index, new_name.toUtf8().data());

    _sampling_bar->apply_device_options();

    m_channel_list(call, QJsonObject());
}

//------------------------------------------------------------- capture

void ControlServer::m_capture_start(const Call &call, const QJsonObject &params)
{
    if (!check_ready(call, true) || !check_not_file(call))
        return;

    bool instant = false;
    bool wait = false;
    double timeout = 0;

    if (params.contains("instant") && !parse_bool(params.value("instant"), instant)){
        reply_error(call, ErrInvalidParams, "\"instant\" needs true or false.");
        return;
    }
    if (params.contains("wait") && !parse_bool(params.value("wait"), wait)){
        reply_error(call, ErrInvalidParams, "\"wait\" needs true or false.");
        return;
    }
    if (!param_timeout(params, timeout)){
        reply_error(call, ErrInvalidParams, "\"timeout\" needs a time in seconds, e.g. 5 or 500ms.");
        return;
    }

    _capture_error.clear();
    _stop_requested = false;
    ControlMode::TakeSuppressedDialogs(); // only report what this capture brings up

    QString err;
    if (!_sampling_bar->start_capture(instant, err)){
        reply_error(call, ErrCaptureFailed, err);
        return;
    }

    if (wait){
        // A capture-and-wait that runs out of time is abandoned: stop it.
        add_waiter(call, timeout, true);
        return;
    }

    reply(call, capture_status());
}

void ControlServer::m_capture_wait(const Call &call, const QJsonObject &params)
{
    double timeout = 0;
    if (!param_timeout(params, timeout)){
        reply_error(call, ErrInvalidParams, "\"timeout\" needs a time in seconds, e.g. 5 or 500ms.");
        return;
    }

    if (!_session->is_working()){
        QJsonObject st = capture_status();
        if (!_capture_error.isEmpty())
            reply_error(call, ErrCaptureFailed, _capture_error, st);
        else
            reply(call, st);
        return;
    }

    // Waiting separately from the start: a timeout leaves the capture
    // running, so the client can wait again.
    add_waiter(call, timeout, false);
}

void ControlServer::m_capture_status(const Call &call, const QJsonObject &params)
{
    (void)params;
    reply(call, capture_status());
}

void ControlServer::m_capture_stop(const Call &call, const QJsonObject &params)
{
    (void)params;

    if (!_session->is_working()){
        reply(call, capture_status());
        return;
    }

    _stop_requested = true;
    _session->stop_capture();
    _sampling_bar->update_view_status();

    // Waiters, and this reply, hear about the end from END_COLLECT_WORK.
    add_waiter(call, 10, false);
}

void ControlServer::add_waiter(const Call &call, double timeout, bool stop_on_timeout)
{
    Waiter w;
    w.call = call;
    w.stop_on_timeout = stop_on_timeout;
    w.timer = NULL;

    if (timeout > 0){
        w.timer = new QTimer();
        w.timer->setSingleShot(true);
        QTimer *t = w.timer;
        connect(t, &QTimer::timeout, this, [this, t](){ on_waiter_timeout(t); });
        t->start((int)std::min(timeout * 1000.0, 2147483647.0));
    }

    _waiters.append(w);
}

void ControlServer::on_waiter_timeout(QTimer *timer)
{
    for (int i = 0; i < _waiters.size(); i++){
        if (_waiters[i].timer != timer)
            continue;

        Waiter w = _waiters.takeAt(i);
        w.timer->deleteLater();

        if (w.stop_on_timeout && _session->is_working()){
            _stop_requested = true;
            _session->stop_capture();
            _sampling_bar->update_view_status();
            reply_error(w.call, ErrTimeout, "Timed out; the capture was stopped.", capture_status());
        }
        else{
            reply_error(w.call, ErrTimeout, "Timed out; the capture is still running.", capture_status());
        }
        return;
    }
}

void ControlServer::finish_waiters()
{
    if (_waiters.isEmpty())
        return;

    QList<Waiter> waiters = _waiters;
    _waiters.clear();

    QJsonObject st = capture_status();

    for (Waiter &w : waiters){
        if (w.timer != NULL){
            w.timer->stop();
            w.timer->deleteLater();
        }
        if (!_capture_error.isEmpty())
            reply_error(w.call, ErrCaptureFailed, _capture_error, st);
        else
            reply(w.call, st);
    }
}

//------------------------------------------------------------- export

void ControlServer::m_export(const Call &call, const QJsonObject &params)
{
    if (!check_ready(call, true))
        return;

    QString path = params.value("path").toString();
    if (path.isEmpty()){
        reply_error(call, ErrInvalidParams, "\"path\" is required.");
        return;
    }
    if (!QDir::isAbsolutePath(path)){
        reply_error(call, ErrInvalidParams,
            "\"path\" must be absolute; DSView's working directory is not the client's.");
        return;
    }
    path = QDir::cleanPath(path);

    // Default from the file name, else VCD.
    QString format = params.value("format").toString().toLower();
    if (format.isEmpty()){
        QString suffix = QFileInfo(path).suffix().toLower();
        format = (suffix == "csv" || suffix == "gnuplot") ? suffix : "vcd";
    }

    static const QStringList formats = {"vcd", "csv", "gnuplot"};
    bool known = false;
    for (const struct sr_output_module **m = sr_output_list(); *m != NULL; m++){
        if (format == (*m)->id && formats.contains(format))
            known = true;
    }
    if (!known){
        QJsonObject data;
        data["formats"] = QJsonArray::fromStringList(formats);
        reply_error(call, ErrInvalidParams, "Unknown export format: " + format, data);
        return;
    }

    uint64_t start = 0, end = 0;
    bool has_start, has_end;
    if (!param_uint64(params, "start", start, has_start) || !param_uint64(params, "end", end, has_end)){
        reply_error(call, ErrInvalidParams, "\"start\" and \"end\" are sample numbers.");
        return;
    }

    if (!_session->have_view_data()){
        reply_error(call, ErrNoData, "There is no captured data to export.");
        return;
    }

    const uint64_t total = (uint64_t)_session->get_ring_sample_count();
    if (has_start && start >= total){
        reply_error(call, ErrInvalidParams, QString("\"start\" is beyond the data (%1 samples).").arg(total));
        return;
    }
    if (has_end && (end > total || end <= start)){
        reply_error(call, ErrInvalidParams,
            QString("\"end\" must be after \"start\" and at most %1.").arg(total));
        return;
    }

    QFileInfo dir(QFileInfo(path).absolutePath());
    if (!dir.isDir() || !dir.isWritable()){
        reply_error(call, ErrInvalidParams, "Can't write to " + dir.absoluteFilePath());
        return;
    }

    std::unique_ptr<Export> ex(new Export());
    ex->call = call;
    ex->path = path;
    ex->format = format;
    ex->start = start & ~(uint64_t)7; // StoreSession widens to whole bytes
    ex->store.reset(new StoreSession(_session));
    ex->store->SetExportTarget(path, format);
    // StoreSession takes 0 as "from the start" / "to the end".
    ex->store->SetDataRange(start, has_end ? end : 0);

    if (!ex->store->export_start()){
        QString err = ex->store->error();
        reply_error(call, ErrExportFailed, err.isEmpty() ? QString("The export could not be started.") : err);
        return;
    }

    _session->set_saving(true);

    _export = std::move(ex);
    _export->timer.setInterval(ExportPollInterval);
    connect(&_export->timer, SIGNAL(timeout()), this, SLOT(on_export_poll()));
    _export->timer.start();
}

void ControlServer::on_export_poll()
{
    if (!_export || _export->store->is_busy())
        return;

    std::unique_ptr<Export> ex = std::move(_export);
    ex->timer.stop();
    ex->store->wait();

    // As the export progress dialog does when it closes.
    _session->set_saving(false);
    _session->broadcast_msg(DSV_MSG_SAVE_COMPLETE);

    uint64_t written = 0, total = 0;
    ex->store->get_progress(&written, &total);

    QFileInfo file(ex->path);
    QString err = ex->store->error();

    if (ex->store->has_error() || !err.isEmpty() || !file.exists() || file.size() == 0){
        reply_error(ex->call, ErrExportFailed, err.isEmpty() ? QString("The export failed.") : err);
        return;
    }

    QJsonObject r;
    r["path"] = ex->path;
    r["format"] = ex->format;
    r["bytes"] = (double)file.size();
    r["start"] = (double)ex->start;
    r["samples"] = (double)total;
    r["samplerate"] = (double)_session->cur_snap_samplerate();
    if (_trigger_armed && _session->trigd())
        r["trigger_sample"] = (double)_session->get_trigger_pos();

    QJsonArray channels;
    for (const GSList *l = _device->get_channels(); l; l = l->next){
        const sr_channel *probe = (const sr_channel*)l->data;
        if (probe->enabled)
            channels.append(QString::fromUtf8(probe->name));
    }
    r["channels"] = channels;

    reply(ex->call, r);
}

} // namespace control
} // namespace pv
