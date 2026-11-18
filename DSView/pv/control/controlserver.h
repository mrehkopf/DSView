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
#include <QObject>
#include <QPointer>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QHash>
#include <QList>
#include <QTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include "../interface/icallbacks.h"

class DeviceAgent;

namespace pv {

class SigSession;
class StoreSession;

namespace toolbars {
class SamplingBar;
}

namespace dock {
class TriggerDock;
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

    ControlServer(SigSession *session, toolbars::SamplingBar *sampling_bar,
                  dock::TriggerDock *trigger_dock, QObject *parent);
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
        uint64_t start;
        QTimer  timer;
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
    bool check_ready(const Call &call, bool need_logic, bool allow_running = false);
    bool check_not_file(const Call &call);

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

    QJsonObject device_info();
    QJsonObject capture_status();
    QString state_name();
    QJsonObject trigger_json();

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
    DeviceAgent             *_device;
    toolbars::SamplingBar   *_sampling_bar;
    dock::TriggerDock       *_trigger_dock;
    QLocalServer            _server;
    QHash<QLocalSocket*, QByteArray> _buffers;

    QList<Waiter>           _waiters;
    std::unique_ptr<Export> _export;

    bool                    _stop_requested;
    bool                    _trigger_armed;   // the last capture waited for a trigger
    QString                 _capture_error;
};

} // namespace control
} // namespace pv

#endif // DSVIEW_PV_CONTROL_CONTROLSERVER_H
