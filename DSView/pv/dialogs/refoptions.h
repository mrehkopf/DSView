/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2026 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef DSVIEW_PV_REFOPTIONS_H
#define DSVIEW_PV_REFOPTIONS_H

#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>

#include "dsdialog.h"
#include "../ui/dscombobox.h"

namespace pv {

class SigSession;

namespace dialogs {

// Save the current DSO channel as a frozen reference waveform, or clear the
// stored references. References are overlaid on the live view.
class RefOptions : public DSDialog
{
    Q_OBJECT

public:
    RefOptions(SigSession *session, QWidget *parent);
    ~RefOptions();

private slots:
    void on_save();
    void on_clear();

private:
    void update_count();

private:
    SigSession *_session;

    DsComboBox      *_ch_combobox;
    QPushButton     *_save_btn;
    QPushButton     *_clear_btn;
    QLabel          *_count_label;
    QVBoxLayout     *_layout;
    QDialogButtonBox _button_box;
};

} // namespace dialogs
} // namespace pv

#endif // DSVIEW_PV_REFOPTIONS_H
