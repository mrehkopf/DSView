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

// dsview-ctl: command line client for DSView's control interface. DSView
// must run with --control (or --headless). Each command sends one or two
// JSON-RPC requests and prints the result as JSON; see
// doc/control-interface.md.

#include <cstdio>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QStringList>

namespace {

enum ExitCode
{
    ExitOk = 0,
    ExitError = 1,     // DSView answered with an error
    ExitUsage = 2,
    ExitConnect = 3,   // no DSView to talk to
};

const char *usage_text =
"Usage: dsview-ctl [OPTIONS] COMMAND [ARGS...]\n"
"\n"
"Controls a DSView started with --control or --headless. Results are printed\n"
"as JSON on stdout; errors as JSON on stderr, with a non-zero exit code.\n"
"\n"
"Options:\n"
"  -s, --socket NAME   Socket to connect to (default: DSView's default)\n"
"      --compact       Print JSON on one line\n"
"  -h, --help          Show this help\n"
"\n"
"Commands:\n"
"  info                        DSView, protocol and device information\n"
"  quit                        Close DSView\n"
"  devices                     List devices\n"
"  select INDEX|NAME           Switch to another device\n"
"  config                      List the options, their values and choices\n"
"  get [KEY...]                Show option values\n"
"  set KEY=VALUE...            Change options, e.g.\n"
"                                set samplerate=100M duration=10ms vth=1.8\n"
"                              samples=N sets the duration instead; durations\n"
"                              are rounded up to the next one DSView offers.\n"
"  channels                    List channels\n"
"  enable CH...                Enable exactly these channels, e.g. enable 0-7 12\n"
"  rename CH NAME              Rename a channel\n"
"  capture [OPTIONS]           Capture and wait for it to finish\n"
"      --instant               Start at once, ignoring the trigger\n"
"      --no-wait               Return as soon as the capture runs\n"
"      --timeout SEC           Stop the capture if it takes longer\n"
"      -o, --output FILE       Export the data to FILE afterwards\n"
"      --format FMT            vcd (default), csv or gnuplot\n"
"  status                      Capture state and data size\n"
"  wait [--timeout SEC]        Wait for a running capture to end; a timeout\n"
"                              leaves it running\n"
"  stop                        Stop the running capture\n"
"  export FILE [--format FMT] [--start N] [--end N]\n"
"                              Export the captured data (sample range\n"
"                              N..end, default all)\n"
"  call METHOD [JSON]          Send any request, with JSON params\n";

bool compact = false;

QString default_socket_name()
{
    // Keep in sync with ControlMode::DefaultServerName() in DSView.
#ifdef _WIN32
    QString user = QString::fromLocal8Bit(qgetenv("USERNAME"));
    if (user.isEmpty())
        user = "user";
    return "dsview-control-" + user;
#else
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    return dir + "/dsview-control";
#endif
}

void print_json(FILE *out, const QJsonObject &obj)
{
    QByteArray text = QJsonDocument(obj).toJson(compact ? QJsonDocument::Compact
                                                        : QJsonDocument::Indented);
    if (!text.endsWith('\n'))
        text.append('\n');
    fwrite(text.constData(), 1, text.size(), out);
    fflush(out);
}

void print_error(const QString &message, int code = 0, const QJsonObject &data = QJsonObject())
{
    QJsonObject err;
    if (code != 0)
        err["code"] = code;
    err["message"] = message;
    if (!data.isEmpty())
        err["data"] = data;
    print_json(stderr, err);
}

int usage_error(const QString &message)
{
    fprintf(stderr, "dsview-ctl: %s\nTry 'dsview-ctl --help'.\n", message.toLocal8Bit().constData());
    return ExitUsage;
}

class Client
{
public:
    bool connect(const QString &name)
    {
        _socket.connectToServer(name);
        if (_socket.waitForConnected(3000))
            return true;

        QString why = _socket.errorString();
#ifndef _WIN32
        if (name.toLocal8Bit().size() > 100)
            why += " (the socket path is too long; Unix sockets allow about 100 bytes)";
#endif
        QJsonObject data;
        data["socket"] = name;
        print_error("Can't reach DSView: " + why +
                    ". Start it with --control or --headless.", 0, data);
        return false;
    }

    // Sends a request and waits for its reply. timeout_ms < 0: no limit.
    // Prints errors itself; returns false on any failure.
    bool call(const QString &method, const QJsonObject &params, QJsonObject &result,
              int timeout_ms = 60000)
    {
        QJsonObject req;
        req["jsonrpc"] = "2.0";
        req["id"] = ++_next_id;
        req["method"] = method;
        if (!params.isEmpty())
            req["params"] = params;

        QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact);
        line.append('\n');
        _socket.write(line);
        if (!_socket.waitForBytesWritten(5000)){
            print_error("Can't send to DSView: " + _socket.errorString());
            return false;
        }

        QElapsedTimer clock;
        clock.start();

        while (true){
            int pos;
            while ((pos = _buffer.indexOf('\n')) >= 0){
                QByteArray reply = _buffer.left(pos);
                _buffer.remove(0, pos + 1);

                QJsonObject msg = QJsonDocument::fromJson(reply).object();
                if (msg.value("id").toInt(-1) != _next_id)
                    continue;

                if (msg.contains("error")){
                    QJsonObject err = msg.value("error").toObject();
                    print_error(err.value("message").toString(), err.value("code").toInt(),
                                err.value("data").toObject());
                    return false;
                }
                result = msg.value("result").toObject();
                return true;
            }

            int wait = 1000;
            if (timeout_ms >= 0){
                qint64 left = timeout_ms - clock.elapsed();
                if (left <= 0){
                    print_error("No reply from DSView in time.");
                    return false;
                }
                wait = (int)qMin<qint64>(left, 1000);
            }

            if (_socket.waitForReadyRead(wait)){
                _buffer.append(_socket.readAll());
            }
            else if (_socket.state() != QLocalSocket::ConnectedState){
                print_error("DSView closed the connection.");
                return false;
            }
        }
    }

private:
    QLocalSocket    _socket;
    QByteArray      _buffer;
    int             _next_id = 0;
};

// "0-7", "12", "0,1,2" → channel numbers.
bool parse_channels(const QStringList &args, QJsonArray &out)
{
    for (const QString &arg : args){
        for (const QString &part : arg.split(',', Qt::SkipEmptyParts)){
            bool ok1 = true, ok2 = true;
            int dash = part.indexOf('-');
            int a = dash < 0 ? part.toInt(&ok1) : part.left(dash).toInt(&ok1);
            int b = dash < 0 ? a : part.mid(dash + 1).toInt(&ok2);
            if (!ok1 || !ok2 || a < 0 || b < a)
                return false;
            for (int i = a; i <= b; i++)
                out.append(i);
        }
    }
    return !out.isEmpty();
}

// Takes "--name VALUE" out of args.
bool take_option(QStringList &args, const QStringList &names, QString &value, bool &found)
{
    found = false;
    for (int i = 0; i < args.size(); i++){
        if (!names.contains(args[i]))
            continue;
        if (i + 1 >= args.size())
            return false;
        value = args[i + 1];
        args.removeAt(i);
        args.removeAt(i);
        found = true;
        return true;
    }
    return true;
}

bool take_flag(QStringList &args, const QString &name)
{
    return args.removeAll(name) > 0;
}

// A timeout in seconds as given on the command line ("5", "500ms").
QJsonValue timeout_value(const QString &text)
{
    bool ok;
    double v = text.toDouble(&ok);
    if (ok)
        return v;
    return text;
}

// Client-side limit for a call the server may hold until a timeout of
// its own: that timeout plus some slack, or no limit.
int wait_limit_ms(const QString &timeout)
{
    if (timeout.isEmpty())
        return -1;
    bool ok;
    double v = timeout.toDouble(&ok);
    if (!ok){
        if (timeout.endsWith("ms"))
            v = timeout.chopped(2).toDouble(&ok) / 1000;
        else if (timeout.endsWith("s"))
            v = timeout.chopped(1).toDouble(&ok);
    }
    return ok ? (int)(v * 1000) + 15000 : -1;
}

QString absolute_path(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QStringList args = app.arguments();
    args.removeFirst();

    QString socket_name;
    bool found;

    if (args.isEmpty() || args.contains("-h") || args.contains("--help")){
        fputs(usage_text, args.isEmpty() ? stderr : stdout);
        return args.isEmpty() ? ExitUsage : ExitOk;
    }

    // Global options come before the command.
    while (!args.isEmpty() && args.first().startsWith("-")){
        QString opt = args.takeFirst();
        if (opt == "-s" || opt == "--socket"){
            if (args.isEmpty())
                return usage_error("--socket needs a name");
            socket_name = args.takeFirst();
        }
        else if (opt.startsWith("--socket=")){
            socket_name = opt.mid(9);
        }
        else if (opt == "--compact"){
            compact = true;
        }
        else{
            return usage_error("unknown option " + opt);
        }
    }

    if (args.isEmpty())
        return usage_error("no command given");

    const QString cmd = args.takeFirst();
    if (socket_name.isEmpty())
        socket_name = default_socket_name();

    Client client;
    QJsonObject params;
    QJsonObject result;

    auto connect = [&]() { return client.connect(socket_name); };
    auto simple = [&](const char *method) -> int {
        if (!connect())
            return ExitConnect;
        if (!client.call(method, params, result))
            return ExitError;
        print_json(stdout, result);
        return ExitOk;
    };

    if (cmd == "info" || cmd == "quit" || cmd == "devices" || cmd == "config" ||
        cmd == "channels" || cmd == "status" || cmd == "stop"){
        if (!args.isEmpty())
            return usage_error(cmd + " takes no arguments");
        const char *method =
            cmd == "info" ? "server.info" :
            cmd == "quit" ? "server.quit" :
            cmd == "devices" ? "device.list" :
            cmd == "config" ? "config.list" :
            cmd == "channels" ? "channel.list" :
            cmd == "status" ? "capture.status" : "capture.stop";
        return simple(method);
    }

    if (cmd == "select"){
        if (args.size() != 1)
            return usage_error("select needs one device index or name");
        bool is_index;
        int index = args.first().toInt(&is_index);
        if (is_index)
            params["index"] = index;
        else
            params["name"] = args.first();
        return simple("device.select");
    }

    if (cmd == "get"){
        if (!args.isEmpty())
            params["keys"] = QJsonArray::fromStringList(args);
        return simple("config.get");
    }

    if (cmd == "set"){
        if (args.isEmpty())
            return usage_error("set needs KEY=VALUE arguments");
        for (const QString &arg : args){
            int eq = arg.indexOf('=');
            if (eq <= 0)
                return usage_error("expected KEY=VALUE, got " + arg);
            // Values stay strings; DSView parses units and choice names.
            params[arg.left(eq).trimmed()] = arg.mid(eq + 1).trimmed();
        }
        return simple("config.set");
    }

    if (cmd == "enable"){
        QJsonArray chans;
        if (!parse_channels(args, chans))
            return usage_error("enable needs channel numbers, e.g. 0-7 12");
        params["enabled"] = chans;
        return simple("channel.set");
    }

    if (cmd == "rename"){
        if (args.size() != 2)
            return usage_error("rename needs a channel number and a name");
        bool ok;
        int ch = args[0].toInt(&ok);
        if (!ok)
            return usage_error("not a channel number: " + args[0]);
        params["index"] = ch;
        params["name"] = args[1];
        return simple("channel.set");
    }

    if (cmd == "wait"){
        QString timeout;
        if (!take_option(args, {"--timeout", "-t"}, timeout, found))
            return usage_error("--timeout needs seconds");
        if (!args.isEmpty())
            return usage_error("unexpected argument " + args.first());
        if (found)
            params["timeout"] = timeout_value(timeout);
        if (!connect())
            return ExitConnect;
        if (!client.call("capture.wait", params, result, wait_limit_ms(timeout)))
            return ExitError;
        print_json(stdout, result);
        return ExitOk;
    }

    if (cmd == "capture"){
        QString timeout, output, format;
        bool has_timeout, has_output, has_format;
        if (!take_option(args, {"--timeout", "-t"}, timeout, has_timeout) ||
            !take_option(args, {"--output", "-o"}, output, has_output) ||
            !take_option(args, {"--format", "-f"}, format, has_format))
            return usage_error("an option is missing its value");

        const bool instant = take_flag(args, "--instant");
        const bool no_wait = take_flag(args, "--no-wait");
        if (!args.isEmpty())
            return usage_error("unexpected argument " + args.first());
        if (no_wait && has_output)
            return usage_error("--output needs the capture to finish; drop --no-wait");
        if (has_format && !has_output)
            return usage_error("--format goes with --output");

        params["instant"] = instant;
        params["wait"] = !no_wait;
        if (has_timeout)
            params["timeout"] = timeout_value(timeout);

        if (!connect())
            return ExitConnect;

        QJsonObject capture;
        if (!client.call("capture.start", params, capture,
                         no_wait ? 60000 : wait_limit_ms(has_timeout ? timeout : QString())))
            return ExitError;

        if (!has_output){
            print_json(stdout, capture);
            return ExitOk;
        }

        QJsonObject ex;
        ex["path"] = absolute_path(output);
        if (has_format)
            ex["format"] = format;

        QJsonObject exported;
        if (!client.call("export", ex, exported, -1))
            return ExitError;

        result["capture"] = capture;
        result["export"] = exported;
        print_json(stdout, result);
        return ExitOk;
    }

    if (cmd == "export"){
        QString format, start, end;
        bool has_format, has_start, has_end;
        if (!take_option(args, {"--format", "-f"}, format, has_format) ||
            !take_option(args, {"--start"}, start, has_start) ||
            !take_option(args, {"--end"}, end, has_end))
            return usage_error("an option is missing its value");
        if (args.size() != 1)
            return usage_error("export needs one file name");

        params["path"] = absolute_path(args.first());
        if (has_format)
            params["format"] = format;
        if (has_start)
            params["start"] = start;
        if (has_end)
            params["end"] = end;

        if (!connect())
            return ExitConnect;
        if (!client.call("export", params, result, -1))
            return ExitError;
        print_json(stdout, result);
        return ExitOk;
    }

    if (cmd == "call"){
        if (args.isEmpty() || args.size() > 2)
            return usage_error("call needs a method and optional JSON params");
        const QString method = args[0];
        if (args.size() == 2){
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(args[1].toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
                return usage_error("params must be a JSON object");
            params = doc.object();
        }
        if (!connect())
            return ExitConnect;
        // capture.wait and friends may take as long as they like.
        if (!client.call(method, params, result, -1))
            return ExitError;
        print_json(stdout, result);
        return ExitOk;
    }

    return usage_error("unknown command " + cmd);
}
