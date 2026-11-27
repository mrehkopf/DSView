/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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


#ifndef DSVIEW_PV_PROTOCOLEXP_H
#define DSVIEW_PV_PROTOCOLEXP_H

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QCheckBox> 
#include <QString>
#include <QTimer>
 
#include "../prop/binding/deviceoptions.h"
#include "../toolbars/titlebar.h"
#include "dsdialog.h"
#include "../ui/dscombobox.h"

namespace pv {

class SigSession;

namespace view {
class View;
}

namespace data {
namespace decode {
class Row;
class Annotation;
}
}

namespace dialogs {

class ProtocolExp : public DSDialog
{
    Q_OBJECT

private:
    struct ExportRowInfo
    {
        QString title;
        const data::decode::Row *row;
        int     row_index;
        uint64_t    read_index;
    };

public:
    // view gives the cursors for "only between cursors"; it may be NULL.
    ProtocolExp(QWidget *parent, SigSession *session, view::View *view = NULL);

protected:   
    void save_proc();
    static bool compare_ann_index(const data::decode::Annotation *a, 
                    const data::decode::Annotation *b);

    void closeSelf();

signals:
    void export_progress(int percent);

private slots:
    void cancel_export();
    void onAccept();
    void onReject();
    void on_timeout();

private:
    SigSession *_session;

    toolbars::TitleBar *_titlebar;
    DsComboBox *_format_combobox;
    DsComboBox *_separator_combobox;
    QCheckBox  *_range_checkbox;
    DsComboBox *_range_from_combobox;
    DsComboBox *_range_to_combobox;
    std::list<QCheckBox *> _row_sel_list;
    std::list<QLabel *> _row_label_list;
    QFormLayout *_flayout;
    QVBoxLayout *_layout;
    QDialogButtonBox _button_box;

    bool _export_cancel;
    QString     _fileName; 
    QString     _separator;
    bool        _use_range;
    uint64_t    _range_start;
    uint64_t    _range_end;
    QTimer      m_timer; 
    bool        _bAbleClose;
};

} // namespace dialogs
} // namespace pv

#endif // DSVIEW_PV_PROTOCOLEXP_H
