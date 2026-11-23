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
#include <QFile>
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
"  config load FILE|--default  Load a configuration file (.dsc): options,\n"
"                              channels, trigger and decoders; --default\n"
"                              loads the device's default configuration\n"
"  config save FILE            Save the configuration to a file (.dsc)\n"
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
"      --format FMT            vcd (default), csv, gnuplot or dsl; the file\n"
"                              name's ending also chooses it\n"
"  status                      Capture state and data size\n"
"  wait [--timeout SEC]        Wait for a running capture to end; a timeout\n"
"                              leaves it running\n"
"  stop                        Stop the running capture\n"
"  export FILE [--format FMT] [--start N] [--end N]\n"
"                              Export the captured data (sample range\n"
"                              N..end, default all). FILE.dsl saves it as a\n"
"                              DSView data file, always the whole capture\n"
"  open FILE                   Open a DSView data file (.dsl) and load its\n"
"                              data; it shows up as a device\n"
"  close                       Close the open data file\n"
"  trigger                     Show the trigger\n"
"  trigger CH=COND... [OPTIONS]\n"
"                              Set the simple trigger to exactly these\n"
"                              conditions, e.g. trigger 0=rising 3=high\n"
"                              COND: none, low, high, rising, falling, edge\n"
"      --clear                 No conditions: captures start at once\n"
"      --position PCT          Share of the capture before the trigger, in %\n"
"      --mode MODE             simple, advanced or serial; advanced and serial\n"
"                              use the stages set up in DSView or with\n"
"                                call trigger.set '{\"stages\": [...]}'\n"
"  decoders [ID]               List the protocol decoders, or show one with\n"
"                              its channels, options and annotation rows\n"
"  decode                      List the decoders added, their state and rows\n"
"  decode add ID KEY=VALUE... [--stack ID [KEY=VALUE...]]... [--label TEXT]\n"
"                              Add a decoder. A KEY naming one of its\n"
"                              channels binds it to a channel number; other\n"
"                              KEYs set options. --stack adds a decoder that\n"
"                              reads the output of the one before, e.g.\n"
"                                decode add 1:i2c scl=1 sda=0 --stack eeprom24xx\n"
"  decode remove INDEX|--all   Remove a decoder\n"
"  decode wait [--timeout SEC] Wait until decoding is done\n"
"  decode results INDEX ROW [OPTIONS]\n"
"                              Annotations of a row (number or id), in time\n"
"                              order, once decoding is done\n"
"      --timeout SEC           Give up waiting for decoding after that long\n"
"      --decoder ID            The stacked decoder whose row it is, when\n"
"                              several have a row of that id\n"
"      --start N, --end N      Annotations that start in this sample range\n"
"      --offset N, --limit N   Skip N of them, return at most N (1000)\n"
"      --all                   Fetch every page\n"
"  edges [CH...] [--start N] [--end N] [--limit N]\n"
"                              The levels at the start, then each sample\n"
"                              where a channel changes (default: all)\n"
"  levels SAMPLE [CH...]       The channels' levels at a sample\n"
"  measure [CH...] [--start N] [--end N]\n"
"                              Edge counts, pulse widths, period, frequency\n"
"                              and duty cycle per channel\n"
"  find CH=COND... [--start N] [--end N] [--limit N]\n"
"                              The samples where the conditions become true,\n"
"                              e.g. find 0=rising 3=high\n"
"  search QUERY [--start N] [--end N] [--limit N] [--all]\n"
"                              Events or states with conditions around them\n"
"                              (doc/search.md). QUERY is JSON or a file\n"
"                              holding it; --all fetches every page\n"
"  call METHOD [JSON]          Send any request, with JSON params\n"
"\n"
"Sample numbers N count from the start of the capture.\n";

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

    if (cmd == "info" || cmd == "quit" || cmd == "devices" || cmd == "channels" ||
        cmd == "status" || cmd == "stop" || cmd == "close" ||
        (cmd == "config" && args.isEmpty())){
        if (!args.isEmpty())
            return usage_error(cmd + " takes no arguments");
        const char *method =
            cmd == "info" ? "server.info" :
            cmd == "quit" ? "server.quit" :
            cmd == "devices" ? "device.list" :
            cmd == "config" ? "config.list" :
            cmd == "channels" ? "channel.list" :
            cmd == "close" ? "file.close" :
            cmd == "status" ? "capture.status" : "capture.stop";
        return simple(method);
    }

    if (cmd == "config"){
        const QString sub = args.takeFirst();
        if (sub == "load"){
            const bool use_default = take_flag(args, "--default");
            if (use_default ? !args.isEmpty() : args.size() != 1)
                return usage_error("config load needs one file name, or --default");
            if (use_default)
                params["default"] = true;
            else
                params["path"] = absolute_path(args.first());
            return simple("config.load");
        }
        if (sub == "save"){
            if (args.size() != 1)
                return usage_error("config save needs one file name");
            params["path"] = absolute_path(args.first());
            return simple("config.save");
        }
        return usage_error("config takes load or save, or nothing to list the options");
    }

    if (cmd == "open"){
        if (args.size() != 1)
            return usage_error("open needs one file name");
        params["path"] = absolute_path(args.first());
        if (!connect())
            return ExitConnect;
        // A big file takes a while to load.
        if (!client.call("file.open", params, result, -1))
            return ExitError;
        print_json(stdout, result);
        return ExitOk;
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

    if (cmd == "trigger"){
        QString position, mode;
        bool has_position, has_mode;
        if (!take_option(args, {"--position", "-p"}, position, has_position) ||
            !take_option(args, {"--mode"}, mode, has_mode))
            return usage_error("an option is missing its value");
        const bool clear = take_flag(args, "--clear");

        QJsonObject conditions;
        for (const QString &arg : args){
            int eq = arg.indexOf('=');
            if (eq <= 0)
                return usage_error("expected CH=CONDITION, got " + arg);
            conditions[arg.left(eq).trimmed()] = arg.mid(eq + 1).trimmed();
        }
        if (clear && !conditions.isEmpty())
            return usage_error("--clear takes no conditions");

        // Conditions replace the whole simple trigger, and select it.
        if (clear || !conditions.isEmpty())
            params["channels"] = conditions;
        if (has_position)
            params["position"] = position;
        if (has_mode)
            params["mode"] = mode;
        return simple(params.isEmpty() ? "trigger.get" : "trigger.set");
    }

    if (cmd == "decoders"){
        if (args.size() > 1)
            return usage_error("decoders takes at most one decoder id");
        if (args.isEmpty())
            return simple("decoder.catalog");
        params["id"] = args.first();
        return simple("decoder.describe");
    }

    if (cmd == "decode"){
        const QString sub = args.isEmpty() ? QString("list") : args.takeFirst();

        if (sub == "list"){
            if (!args.isEmpty())
                return usage_error("unexpected argument " + args.first());
            return simple("decoder.list");
        }

        if (sub == "add"){
            QString label;
            if (!take_option(args, {"--label"}, label, found))
                return usage_error("--label needs a text");
            if (found)
                params["label"] = label;
            if (args.isEmpty() || args.first().startsWith("-"))
                return usage_error("decode add needs a decoder id");

            // Split into the decoder and its stack: ID KEY=VALUE... [--stack ID KEY=VALUE...]...
            QList<QStringList> parts;
            for (const QString &arg : args){
                if (arg == "--stack")
                    parts.append(QStringList());
                else if (parts.isEmpty())
                    parts.append(QStringList{arg});
                else
                    parts.last().append(arg);
            }

            if (!connect())
                return ExitConnect;

            // Which keys are channels: the decoder says.
            QJsonObject desc, req;
            req["id"] = parts.first().first();
            if (!client.call("decoder.describe", req, desc))
                return ExitError;
            QStringList channel_ids;
            for (const QJsonValue &c : desc.value("channels").toArray())
                channel_ids << c.toObject().value("id").toString();

            QJsonObject channels, options;
            QJsonArray stack;
            for (int i = 0; i < parts.size(); i++){
                QStringList part = parts[i];
                if (part.isEmpty() || part.first().contains('='))
                    return usage_error("--stack needs a decoder id");
                const QString id = part.takeFirst();
                QJsonObject opts;
                for (const QString &kv : part){
                    int eq = kv.indexOf('=');
                    if (eq <= 0)
                        return usage_error("expected KEY=VALUE, got " + kv);
                    const QString key = kv.left(eq).trimmed();
                    const QString value = kv.mid(eq + 1).trimmed();
                    if (i == 0 && channel_ids.contains(key)){
                        bool ok;
                        int ch = value.toInt(&ok);
                        if (!ok || ch < 0)
                            return usage_error("channel " + key + " needs a channel number");
                        channels[key] = ch;
                    }
                    else
                        opts[key] = value;
                }
                if (i == 0){
                    params["id"] = id;
                    options = opts;
                }
                else{
                    QJsonObject s;
                    s["id"] = id;
                    s["options"] = opts;
                    stack.append(s);
                }
            }
            params["channels"] = channels;
            if (!options.isEmpty())
                params["options"] = options;
            if (!stack.isEmpty())
                params["stack"] = stack;

            if (!client.call("decoder.add", params, result))
                return ExitError;
            print_json(stdout, result);
            return ExitOk;
        }

        if (sub == "remove"){
            if (take_flag(args, "--all"))
                params["all"] = true;
            else if (args.size() == 1){
                bool ok;
                params["index"] = args.first().toInt(&ok);
                if (!ok)
                    return usage_error("decode remove needs an index or --all");
            }
            else
                return usage_error("decode remove needs an index or --all");
            if (params.contains("all") && !args.isEmpty())
                return usage_error("unexpected argument " + args.first());
            return simple("decoder.remove");
        }

        if (sub == "wait"){
            QString timeout;
            if (!take_option(args, {"--timeout", "-t"}, timeout, found))
                return usage_error("--timeout needs seconds");
            if (!args.isEmpty())
                return usage_error("unexpected argument " + args.first());
            if (found)
                params["timeout"] = timeout_value(timeout);
            if (!connect())
                return ExitConnect;
            if (!client.call("decoder.wait", params, result, wait_limit_ms(timeout)))
                return ExitError;
            print_json(stdout, result);
            return ExitOk;
        }

        if (sub == "results"){
            QString start, end, offset, limit, decoder, timeout;
            bool has_start, has_end, has_offset, has_limit, has_decoder, has_timeout;
            if (!take_option(args, {"--decoder"}, decoder, has_decoder) ||
                !take_option(args, {"--timeout", "-t"}, timeout, has_timeout) ||
                !take_option(args, {"--start"}, start, has_start) ||
                !take_option(args, {"--end"}, end, has_end) ||
                !take_option(args, {"--offset"}, offset, has_offset) ||
                !take_option(args, {"--limit"}, limit, has_limit))
                return usage_error("an option is missing its value");
            const bool all = take_flag(args, "--all");
            if (args.size() != 2)
                return usage_error("decode results needs a decoder index and a row");

            bool ok;
            params["index"] = args[0].toInt(&ok);
            if (!ok)
                return usage_error("the decoder index is a number (decode)");
            bool is_number;
            const int row = args[1].toInt(&is_number);
            params["row"] = is_number ? QJsonValue(row) : QJsonValue(args[1]);
            if (has_decoder)
                params["decoder"] = decoder;
            if (has_start)
                params["start"] = start;
            if (has_end)
                params["end"] = end;
            if (has_offset)
                params["offset"] = offset;
            if (has_limit)
                params["limit"] = limit;

            if (!connect())
                return ExitConnect;
            QJsonObject waited, wait_params;
            if (has_timeout)
                wait_params["timeout"] = timeout_value(timeout);
            if (!client.call("decoder.wait", wait_params, waited, wait_limit_ms(timeout)))
                return ExitError;

            QJsonArray annotations;
            while (true){
                if (!client.call("decoder.results", params, result))
                    return ExitError;
                for (const QJsonValue &a : result.value("annotations").toArray())
                    annotations.append(a);
                const QJsonValue next = result.value("next_offset");
                if (!all || !next.isDouble())
                    break;
                params["offset"] = next.toDouble();
            }
            result["annotations"] = annotations;
            print_json(stdout, result);
            return ExitOk;
        }

        return usage_error("unknown decode command " + sub);
    }

    if (cmd == "edges" || cmd == "measure" || cmd == "levels" || cmd == "find"){
        QString start, end, limit;
        bool has_start = false, has_end = false, has_limit = false;
        if (cmd != "levels" &&
            (!take_option(args, {"--start"}, start, has_start) ||
             !take_option(args, {"--end"}, end, has_end) ||
             (cmd != "measure" && !take_option(args, {"--limit"}, limit, has_limit))))
            return usage_error("an option is missing its value");

        if (cmd == "levels"){
            if (args.isEmpty())
                return usage_error("levels needs a sample number");
            start = args.takeFirst();
            has_start = true;
            params["limit"] = 0;
        }

        if (cmd == "find"){
            QJsonObject pattern;
            for (const QString &arg : args){
                int eq = arg.indexOf('=');
                if (eq <= 0)
                    return usage_error("expected CH=CONDITION, got " + arg);
                pattern[arg.left(eq).trimmed()] = arg.mid(eq + 1).trimmed();
            }
            if (pattern.isEmpty())
                return usage_error("find needs conditions, e.g. find 0=rising 3=high");
            params["pattern"] = pattern;
        }
        else if (!args.isEmpty()){
            QJsonArray channels;
            if (!parse_channels(args, channels))
                return usage_error("bad channel list");
            params["channels"] = channels;
        }

        if (has_start)
            params["start"] = start;
        if (has_end)
            params["end"] = end;
        if (has_limit)
            params["limit"] = limit;

        if (!connect())
            return ExitConnect;
        const char *method = cmd == "measure" ? "data.measure" :
                             cmd == "find" ? "data.find" : "data.edges";
        if (!client.call(method, params, result))
            return ExitError;

        if (cmd == "levels"){
            QJsonObject r;
            r["sample"] = result.value("start");
            r["levels"] = result.value("levels");
            result = r;
        }
        print_json(stdout, result);
        return ExitOk;
    }

    if (cmd == "search"){
        QString start, end, limit;
        bool has_start = false, has_end = false, has_limit = false;
        const bool all = args.removeAll("--all") > 0;
        if (!take_option(args, {"--start"}, start, has_start) ||
            !take_option(args, {"--end"}, end, has_end) ||
            !take_option(args, {"--limit"}, limit, has_limit))
            return usage_error("an option is missing its value");
        if (args.size() != 1)
            return usage_error("search needs one query, as JSON or a file name");

        QByteArray text = args[0].toUtf8();
        if (!args[0].trimmed().startsWith('{')){
            QFile file(args[0]);
            if (!file.open(QIODevice::ReadOnly))
                return usage_error("can't read " + args[0]);
            text = file.readAll();
        }
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(text, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            return usage_error("the query must be a JSON object: " + err.errorString());

        params["query"] = doc.object();
        if (has_start)
            params["start"] = start;
        if (has_end)
            params["end"] = end;
        if (has_limit)
            params["limit"] = limit;

        if (!connect())
            return ExitConnect;
        QJsonArray matches;
        while (true){
            if (!client.call("data.search", params, result))
                return ExitError;
            for (const QJsonValue &m : result.value("matches").toArray())
                matches.append(m);
            const QJsonValue next = result.value("next");
            if (!all || !next.isDouble())
                break;
            params["start"] = next.toDouble();
        }
        result["matches"] = matches;
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
