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

#ifndef DSVIEW_PV_CONTROL_CONTROLHOST_H
#define DSVIEW_PV_CONTROL_CONTROLHOST_H

#include <QString>

class ISessionDataGetter;

namespace pv {
namespace control {

// What the server needs from MainWindow, where opening data files and
// loading and storing configuration files live.
class IControlHost
{
public:
    virtual ~IControlHost() {}

    // Opens a .dsl file as the current device, as File > Open does but
    // without asking to save captured data. False if the file can't be
    // read; a device is selected either way.
    virtual bool control_open_file(const QString &path) = 0;
    // Loads and stores a configuration file (.dsc), as File > Config does.
    virtual bool control_load_config(const QString &path) = 0;
    virtual bool control_store_config(const QString &path) = 0;
    // The file File > Config > Default loads for the current device.
    virtual QString control_default_config() = 0;
    // The settings a .dsl file holds next to the data.
    virtual ISessionDataGetter *control_session_data() = 0;
    // Closes DSView as the window's close button does, but without asking
    // to save captured data.
    virtual void control_quit() = 0;
};

} // namespace control
} // namespace pv

#endif // DSVIEW_PV_CONTROL_CONTROLHOST_H
