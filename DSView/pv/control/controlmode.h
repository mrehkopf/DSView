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

#ifndef DSVIEW_PV_CONTROL_CONTROLMODE_H
#define DSVIEW_PV_CONTROL_CONTROLMODE_H

#include <QString>
#include <QStringList>

namespace pv {
namespace control {

// Process-wide switches for driving DSView through the control interface
// (doc/control-interface.md). main() sets them from the command line before
// any window exists; they don't change afterwards.
class ControlMode
{
public:
    // Socket the control server listens on. Empty: no server.
    static void SetServerName(const QString &name);
    static const QString& ServerName();
    static bool ServerEnabled();

    // Where the server listens and dsview-ctl connects when no name is
    // given. Keep in sync with DefaultServerName() in tools/dsview-ctl.cpp.
    static QString DefaultServerName();

    // No display: Qt runs on the offscreen platform. Implies non-interactive.
    static void SetHeadless(bool enable);
    static bool IsHeadless();

    // Modal dialogs are not shown. DSMessageBox and DSDialog return
    // "rejected" (No/Cancel) at once, and the text is kept so the control
    // server can pass it on with its next reply.
    static void SetNonInteractive(bool enable);
    static bool IsNonInteractive();

    static void RecordSuppressedDialog(const QString &title, const QString &text);
    static QStringList TakeSuppressedDialogs();
};

} // namespace control
} // namespace pv

#endif // DSVIEW_PV_CONTROL_CONTROLMODE_H
