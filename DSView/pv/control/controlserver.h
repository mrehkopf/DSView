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

#ifndef DSVIEW_PV_CONTROL_CONTROLSERVER_H
#define DSVIEW_PV_CONTROL_CONTROLSERVER_H

#include <memory>
#include <vector>
#include <QObject>
#include <QPointer>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QHash>
#include <QList>
#include <QTimer>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include "../interface/icallbacks.h"
#include "controlhost.h"

class DeviceAgent;

namespace pv {

class SigSession;
class StoreSession;

namespace toolbars {
class SamplingBar;
}

namespace data {
class LogicSnapshot;
}

namespace dock {
class TriggerDock;
class ProtocolDock;
}

namespace control {

// JSON-RPC 2.0 over a local socket, one message per line. Lets scripts and
// agents set up the analyzer, run captures and export the data, through the
// same code paths as the toolbar. Everything runs on the GUI thread; calls
// that wait (capture, export) reply when the work is done, without blocking
// the event loop. The protocol is described in doc/control-interface.md.
class ControlServer : public QObject, public IMessageListener
{
    Q_OBJECT

public:
    static const int ProtocolVersion = 1;

    ControlServer(SigSession *session, IControlHost *host,
                  toolbars::SamplingBar *sampling_bar,
                  dock::TriggerDock *trigger_dock,
                  dock::ProtocolDock *protocol_dock, QObject *parent);
    ~ControlServer();

    bool listen(const QString &name);

    // MainWindow::on_session_error passes the error on before it handles it.
    void notify_session_error(const QString &title, const QString &details);

    // IMessageListener
    void OnMessage(int msg) override;

private slots:
    void on_new_connection();
    void on_ready_read();
    void on_disconnected();
    void on_export_poll();
    void on_load_timeout();
    void on_decode_poll();

private:
    struct Call
    {
        QPointer<QLocalSocket> socket;
        QJsonValue id;   // undefined for a notification: no reply
    };

    struct Waiter
    {
        Call    call;
        QTimer  *timer;
        bool    stop_on_timeout;
    };

    struct Export
    {
        Call    call;
        std::unique_ptr<StoreSession> store;
        QString path;
        QString format;
        bool    save;   // a .dsl file (DSView's Save), not an export
        uint64_t start;
        QTimer  timer;
    };

    struct DecodeWaiter
    {
        Call            call;
        QElapsedTimer   started;
        double          timeout;    // seconds; 0: none
    };

    // decoder.results: one row's annotations in time order. Decoders
    // store them in the order they produce them, which isn't always that.
    struct ResultOrder
    {
        void                    *stack = NULL;  // compared only
        uint64_t                generation = 0;
        int                     row = -1;
        std::vector<uint64_t>   index;          // annotation numbers, by start
        std::vector<uint64_t>   start;          // their start samples
    };

    struct LoadCall
    {
        Call        call;
        QJsonObject result;   // the reply, before the device and status are added
    };

    struct ConfigOption;

    void handle_line(QLocalSocket *socket, const QByteArray &line);
    void dispatch(const Call &call, const QString &method, const QJsonObject &params);

    void reply(const Call &call, QJsonObject result);
    void reply_error(const Call &call, int code, const QString &message,
                     QJsonObject data = QJsonObject());
    void send(const Call &call, const QJsonObject &msg);

    // Checks shared by the methods that change something. Replies with an
    // error and returns false when the call can't go ahead.
    bool check_idle(const Call &call, bool allow_running);
    bool check_ready(const Call &call, bool need_logic, bool allow_running = false);
    bool check_not_file(const Call &call);
    bool check_hardware(const Call &call);

    void m_server_info(const Call &call, const QJsonObject &params);
    void m_server_quit(const Call &call, const QJsonObject &params);
    void m_device_list(const Call &call, const QJsonObject &params);
    void m_device_select(const Call &call, const QJsonObject &params);
    void m_config_list(const Call &call, const QJsonObject &params);
    void m_config_get(const Call &call, const QJsonObject &params);
    void m_config_set(const Call &call, const QJsonObject &params);
    void m_channel_list(const Call &call, const QJsonObject &params);
    void m_channel_set(const Call &call, const QJsonObject &params);
    void m_capture_start(const Call &call, const QJsonObject &params);
    void m_capture_wait(const Call &call, const QJsonObject &params);
    void m_capture_status(const Call &call, const QJsonObject &params);
    void m_capture_stop(const Call &call, const QJsonObject &params);
    void m_export(const Call &call, const QJsonObject &params);
    void m_trigger_get(const Call &call, const QJsonObject &params);
    void m_trigger_set(const Call &call, const QJsonObject &params);
    void m_file_open(const Call &call, const QJsonObject &params);
    void m_file_close(const Call &call, const QJsonObject &params);
    void m_config_load(const Call &call, const QJsonObject &params);
    void m_config_save(const Call &call, const QJsonObject &params);
    void m_decoder_catalog(const Call &call, const QJsonObject &params);
    void m_decoder_describe(const Call &call, const QJsonObject &params);
    void m_decoder_list(const Call &call, const QJsonObject &params);
    void m_decoder_add(const Call &call, const QJsonObject &params);
    void m_decoder_remove(const Call &call, const QJsonObject &params);
    void m_decoder_wait(const Call &call, const QJsonObject &params);
    void m_decoder_results(const Call &call, const QJsonObject &params);

    void m_data_edges(const Call &call, const QJsonObject &params);
    void m_data_measure(const Call &call, const QJsonObject &params);
    void m_data_find(const Call &call, const QJsonObject &params);

    // The captured logic data and the "start"/"end" range of a data call.
    // Replies with an error and returns NULL if there is none.
    data::LogicSnapshot *data_range(const Call &call, const QJsonObject &params,
                                    uint64_t &start, uint64_t &end);
    // The "channels" parameter: channels with data, all of them if absent.
    bool data_channels(const Call &call, const QJsonObject &params,
                       data::LogicSnapshot *snapshot, std::vector<int> &channels);

    QJsonArray decoder_list();
    // A decoder decodes or will: decoding, or a capture runs or a data file
    // loads (decoders run at the end of a capture).
    bool decoding_busy();
    // The "index" parameter: a decoder of the session. Replies with an
    // error and returns -1 if there is none.
    int decoder_index(const Call &call, const QJsonObject &params);

    QJsonObject device_info();
    QJsonObject capture_status();
    QString state_name();
    QJsonObject trigger_json();
    bool data_triggered();

    // A data file's samples arrive through a replay that MainWindow starts
    // after the device switch. Calls that select a file reply once it ends.
    void reply_when_loaded(const Call &call, const QJsonObject &result = QJsonObject());
    void finish_loading(const QString &error);
    QJsonObject load_result();

    std::vector<ConfigOption> device_options();
    QJsonObject option_json(const ConfigOption &opt, bool with_choices);
    QJsonValue option_value(const ConfigOption &opt);
    bool set_option(const ConfigOption &opt, const QJsonValue &value, QString &error);
    QJsonObject current_values();

    void add_waiter(const Call &call, double timeout, bool stop_on_timeout);
    void finish_waiters();
    void on_waiter_timeout(QTimer *timer);

private:
    SigSession              *_session;
    IControlHost            *_host;
    DeviceAgent             *_device;
    toolbars::SamplingBar   *_sampling_bar;
    dock::TriggerDock       *_trigger_dock;
    dock::ProtocolDock      *_protocol_dock;
    QLocalServer            _server;
    QHash<QLocalSocket*, QByteArray> _buffers;

    QList<Waiter>           _waiters;
    std::unique_ptr<Export> _export;

    bool                    _loading;         // a data file's replay is due or running
    bool                    _load_started;
    QTimer                  _load_timer;
    QList<LoadCall>         _load_calls;

    QList<DecodeWaiter>     _decode_waiters;
    ResultOrder             _result_order;
    QTimer                  _decode_poll;

    bool                    _stop_requested;
    bool                    _trigger_armed;   // the last capture waited for a trigger
    QString                 _capture_error;
};

} // namespace control
} // namespace pv

#endif // DSVIEW_PV_CONTROL_CONTROLSERVER_H
