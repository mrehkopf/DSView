/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2026 Schildkroet
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

#ifndef DSVIEW_PV_CHANMEASURE_H
#define DSVIEW_PV_CHANMEASURE_H

#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QLabel>

#include "dsdialog.h"
#include "../ui/dscombobox.h"

namespace pv {

class SigSession;

namespace dialogs {

// Channel-to-channel timing measurements (phase, delay, skew) between two DSO
// channels, computed from the current snapshot.
class DsoChannelMeasure : public DSDialog
{
    Q_OBJECT

public:
    DsoChannelMeasure(SigSession *session, QWidget *parent);
    ~DsoChannelMeasure();

private slots:
    void on_source_changed(int index);

private:
    void compute();

private:
    SigSession *_session;

    DsComboBox      *_srcA_combobox;
    DsComboBox      *_srcB_combobox;
    QLabel          *_result_label;
    QVBoxLayout     *_layout;
    QDialogButtonBox _button_box;
};

} // namespace dialogs
} // namespace pv

#endif // DSVIEW_PV_CHANMEASURE_H
