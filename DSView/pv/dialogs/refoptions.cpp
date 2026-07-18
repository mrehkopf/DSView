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

#include "refoptions.h"

#include <QHBoxLayout>
#include <QFormLayout>

#include "../sigsession.h"
#include "../view/dsosignal.h"
#include "../ui/langresource.h"

using namespace std;

namespace pv {
namespace dialogs {

RefOptions::RefOptions(SigSession *session, QWidget *parent) :
    DSDialog(parent),
    _session(session),
    _button_box(QDialogButtonBox::Close, Qt::Horizontal, this)
{
    setMinimumSize(340, 180);

    _ch_combobox = new DsComboBox(this);
    for (auto s : _session->get_signals()) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *dsoSig = (view::DsoSignal*)s;
            _ch_combobox->addItem(dsoSig->get_name(),
                                  QVariant::fromValue(dsoSig->get_index()));
        }
    }

    _save_btn = new QPushButton(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_REF_SAVE), "Save reference"), this);
    _clear_btn = new QPushButton(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_REF_CLEAR), "Clear all"), this);
    _count_label = new QLabel(this);

    QFormLayout *ch_layout = new QFormLayout();
    ch_layout->addRow(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel"), _ch_combobox);

    QHBoxLayout *btn_layout = new QHBoxLayout();
    btn_layout->addWidget(_save_btn);
    btn_layout->addWidget(_clear_btn);

    _layout = new QVBoxLayout();
    _layout->addLayout(ch_layout);
    _layout->addLayout(btn_layout);
    _layout->addWidget(_count_label);
    _layout->addWidget(&_button_box);

    layout()->addLayout(_layout);
    setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_REF_OPTIONS), "Reference Waveforms"));

    connect(&_button_box, SIGNAL(rejected()), this, SLOT(reject()));
    connect(&_button_box, SIGNAL(accepted()), this, SLOT(accept()));
    connect(_save_btn, SIGNAL(clicked()), this, SLOT(on_save()));
    connect(_clear_btn, SIGNAL(clicked()), this, SLOT(on_clear()));

    update_count();
}

RefOptions::~RefOptions()
{
}

void RefOptions::update_count()
{
    _count_label->setText(
        QString("%1: %2")
            .arg(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_REF_STORED), "Stored references"))
            .arg((int)_session->get_ref_waves().size()));
}

void RefOptions::on_save()
{
    if (_ch_combobox->count() == 0)
        return;

    const int index =
        _ch_combobox->itemData(_ch_combobox->currentIndex()).toInt();

    for (auto s : _session->get_signals()) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *d = (view::DsoSignal*)s;
            if (d->get_index() == index) {
                _session->add_ref_wave(d);
                break;
            }
        }
    }

    update_count();
}

void RefOptions::on_clear()
{
    _session->clear_ref_waves();
    update_count();
}

} // namespace dialogs
} // namespace pv
