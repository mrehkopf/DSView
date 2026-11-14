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

#include "controlmode.h"

#include <QDir>
#include <QStandardPaths>
#include "../log.h"

namespace pv {
namespace control {

namespace {
    QString     server_name;
    bool        headless = false;
    bool        non_interactive = false;
    QStringList suppressed_dialogs;

    // Only the newest messages are worth reporting; don't grow without bound
    // when nobody collects them.
    const int MaxSuppressedDialogs = 20;
}

void ControlMode::SetServerName(const QString &name)
{
    server_name = name;
}

const QString& ControlMode::ServerName()
{
    return server_name;
}

bool ControlMode::ServerEnabled()
{
    return !server_name.isEmpty();
}

QString ControlMode::DefaultServerName()
{
#ifdef _WIN32
    // A named pipe; QLocalServer adds the \\.\pipe\ prefix.
    QString user = QString::fromLocal8Bit(qgetenv("USERNAME"));
    if (user.isEmpty())
        user = "user";
    return "dsview-control-" + user;
#else
    // A socket file in the per-user runtime directory, so other users on
    // the machine can't reach it.
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    return dir + "/dsview-control";
#endif
}

void ControlMode::SetHeadless(bool enable)
{
    headless = enable;
    if (enable)
        non_interactive = true;
}

bool ControlMode::IsHeadless()
{
    return headless;
}

void ControlMode::SetNonInteractive(bool enable)
{
    non_interactive = enable || headless;
}

bool ControlMode::IsNonInteractive()
{
    return non_interactive;
}

void ControlMode::RecordSuppressedDialog(const QString &title, const QString &text)
{
    QString msg = text.trimmed();
    if (!title.trimmed().isEmpty())
        msg = title.trimmed() + ": " + msg;

    dsv_warn("Dialog not shown (non-interactive): %s", msg.toUtf8().data());

    suppressed_dialogs.append(msg);
    while (suppressed_dialogs.size() > MaxSuppressedDialogs)
        suppressed_dialogs.removeFirst();
}

QStringList ControlMode::TakeSuppressedDialogs()
{
    QStringList list = suppressed_dialogs;
    suppressed_dialogs.clear();
    return list;
}

} // namespace control
} // namespace pv
