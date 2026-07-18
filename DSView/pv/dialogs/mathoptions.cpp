/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2015 DreamSourceLab <support@dreamsourcelab.com>
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

#include "mathoptions.h"
#include <QCheckBox>
#include <QVariant>
#include <QLabel>
#include <QTabBar>
#include <QBitmap>

#include "../sigsession.h"
#include "../view/view.h"
#include "../view/mathtrace.h"
#include "../data/mathstack.h"
#include "../ui/langresource.h"
#include "../ui/fn.h"
#include "../config/appconfig.h"

using namespace boost;
using namespace std;
using namespace pv::view;

namespace pv {
namespace dialogs {

MathOptions::MathOptions(SigSession *session, QWidget *parent) :
    DSDialog(parent),
    _session(session),
    _button_box(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        Qt::Horizontal, this)
{
    setMinimumSize(300, 300);

    _enable = new QCheckBox(this);

    QLabel *lisa_label = new QLabel(this);
    lisa_label->setPixmap(QPixmap(":/icons/math.svg"));

    _math_group = new QGroupBox(this);
    QGridLayout *type_layout = new QGridLayout();

    // Place operator radios in a 4-column grid (11 operators no longer fit on
    // a single row).
    int rrow = 0, rcol = 0;
    auto place = [&](QRadioButton *b, int t) {
        b->setProperty("type", t);
        _math_radio.append(b);
        type_layout->addWidget(b, rrow, rcol);
        if (++rcol >= 4) { rcol = 0; rrow++; }
    };

    place(new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ADD), "Add"), _math_group), data::MathStack::MATH_ADD);
    place(new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SUBSTRACT), "Substract"), _math_group), data::MathStack::MATH_SUB);
    place(new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MULTIPLY), "Multiply"), _math_group), data::MathStack::MATH_MUL);
    place(new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DIVIDE), "Divide"), _math_group), data::MathStack::MATH_DIV);
    place(new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_INTEGRATE), "Integrate"), _math_group), data::MathStack::MATH_INTEG);
    place(new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DIFFERENTIATE), "Differentiate"), _math_group), data::MathStack::MATH_DIFF);
    place(new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ABSOLUTE), "Absolute"), _math_group), data::MathStack::MATH_ABS);
    place(new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SQUARE), "Square"), _math_group), data::MathStack::MATH_SQUARE);
    place(new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SQRT), "Sqrt"), _math_group), data::MathStack::MATH_SQRT);
    place(new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOWPASS), "Low pass"), _math_group), data::MathStack::MATH_LOWPASS);
    place(new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_HIGHPASS), "High pass"), _math_group), data::MathStack::MATH_HIGHPASS);

    // Moving-average window (samples) used by the low/high-pass filters.
    _filter_label = new QLabel(_math_group);
    _filter_width = new QSpinBox(_math_group);
    _filter_width->setRange(1, 100000);
    _filter_width->setValue(10);
    type_layout->addWidget(_filter_label, rrow + 1, 0, 1, 2);
    type_layout->addWidget(_filter_width, rrow + 1, 2, 1, 2);

    _math_group->setLayout(type_layout);

    _src1_group = new QGroupBox(this);
    _src2_group = new QGroupBox(this);
    QHBoxLayout *src1_layout = new QHBoxLayout();
    QHBoxLayout *src2_layout = new QHBoxLayout();

    src1_layout->setContentsMargins(5, 15, 5, 5);
    src2_layout->setContentsMargins(5, 15, 5, 5);
    type_layout->setContentsMargins(5, 15, 5, 5);

    for(auto s : _session->get_signals()) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *dsoSig = (view::DsoSignal*)s;
            QString index_str = QString::number(dsoSig->get_index());
            QRadioButton *xradio = new QRadioButton(index_str, _src1_group);
            xradio->setProperty("index", dsoSig->get_index());
            src1_layout->addWidget(xradio);
            QRadioButton *yradio = new QRadioButton(index_str, _src2_group);
            yradio->setProperty("index", dsoSig->get_index());
            src2_layout->addWidget(yradio);
            _src1_radio.append(xradio);
            _src2_radio.append(yradio);
        }
    }
    _src1_group->setLayout(src1_layout);
    _src2_group->setLayout(src2_layout);


    auto math = _session->get_math_trace();
    if (math) {
        _enable->setChecked(math->enabled());
        _filter_width->setValue(math->get_math_stack()->get_filter_width());
        for (QVector<QRadioButton *>::const_iterator i = _src1_radio.begin();
            i != _src1_radio.end(); i++) {
            if ((*i)->property("index").toInt() == math->src1()) {
               (*i)->setChecked(true);
                break;
            }
        }
        for (QVector<QRadioButton *>::const_iterator i = _src2_radio.begin();
            i != _src2_radio.end(); i++) {
            if ((*i)->property("index").toInt() == math->src2()) {
               (*i)->setChecked(true);
                break;
            }
        }
        for (QVector<QRadioButton *>::const_iterator i = _math_radio.begin();
            i != _math_radio.end(); i++) {
            if ((*i)->property("type").toInt() == math->get_math_stack()->get_type()) {
                (*i)->setChecked(true);
                break;
            }
        }
    } else {
        _enable->setChecked(false);
        for (QVector<QRadioButton *>::const_iterator i = _src1_radio.begin();
            i != _src1_radio.end(); i++) {
           (*i)->setChecked(true);
            break;
        }
        for (QVector<QRadioButton *>::const_iterator i = _src2_radio.begin();
            i != _src2_radio.end(); i++) {
           (*i)->setChecked(true);
            break;
        }
        for (QVector<QRadioButton *>::const_iterator i = _math_radio.begin();
            i != _math_radio.end(); i++) {
            (*i)->setChecked(true);
            break;
        }
    }

    _layout = new QGridLayout(); 
    _layout->setSpacing(0);
    _layout->addWidget(lisa_label, 0, 0, 1, 2, Qt::AlignCenter);
    _layout->addWidget(_enable, 1, 0, 1, 1);
    _layout->addWidget(_math_group, 2, 0, 1, 2);
    _layout->addWidget(_src1_group, 3, 0, 1, 1);
    _layout->addWidget(_src2_group, 3, 1, 1, 1);
    _layout->addWidget(new QLabel(this), 4, 1, 1, 1);
    _layout->addWidget(&_button_box, 5, 1, 1, 1, Qt::AlignHCenter | Qt::AlignBottom);

    layout()->addLayout(_layout);

    connect(&_button_box, SIGNAL(rejected()), this, SLOT(reject()));
    connect(&_button_box, SIGNAL(accepted()), this, SLOT(accept()));

    ADD_UI(this);
}

MathOptions::~MathOptions()
{
    REMOVE_UI(this);
}

void MathOptions::retranslateUi()
{
    _enable->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE), "Enable"));
    _math_group->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MATH_TYPE), "Math Type"));
    _filter_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_FILTER_WIDTH), "Filter window (samples)"));
    _src1_group->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_1ST_SOURCE), "1st Source"));
    _src2_group->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_2ST_SOURCE), "2st Source"));
    setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MATH_OPTIONS), "Math Options"));
}

void MathOptions::accept()
{
    using namespace Qt;
    QDialog::accept();
}

void MathOptions::Apply()
{
    int src1 = -1;
    int src2 = -1;
    data::MathStack::MathType type = data::MathStack::MATH_ADD;
    for (QVector<QRadioButton *>::const_iterator i = _src1_radio.begin();
        i != _src1_radio.end(); i++) {
        if ((*i)->isChecked()) {
            src1 = (*i)->property("index").toInt();
            break;
        }
    }
    for (QVector<QRadioButton *>::const_iterator i = _src2_radio.begin();
        i != _src2_radio.end(); i++) {
        if ((*i)->isChecked()) {
            src2 = (*i)->property("index").toInt();
            break;
        }
    }
    for (QVector<QRadioButton *>::const_iterator i = _math_radio.begin();
        i != _math_radio.end(); i++) {
        if ((*i)->isChecked()) {
            type = (data::MathStack::MathType)(*i)->property("type").toInt();
            break;
        }
    }
    // Unary operators (integrate/differentiate/abs/square/sqrt/filters) only
    // use the 1st source; fall back to it so a 2nd source need not be picked.
    if (data::MathStack::is_unary(type) && src2 == -1)
        src2 = src1;

    bool enable = (src1 != -1 && src2 != -1 && _enable->isChecked());
    view::DsoSignal *dsoSig1 = NULL;
    view::DsoSignal *dsoSig2 = NULL;

    for(auto s : _session->get_signals()) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *dsoSig = (view::DsoSignal*)s;
            if (dsoSig->get_index() == src1)
                dsoSig1 = dsoSig;
            if (dsoSig->get_index() == src2)
                dsoSig2 = dsoSig;
        }
    }

    if (dsoSig1 != NULL && dsoSig2 != NULL){
        _session->math_rebuild(enable, dsoSig1, dsoSig2, type, _filter_width->value());
    }
}

void MathOptions::reject()
{
    using namespace Qt;
    QDialog::reject();
}

void MathOptions::UpdateLanguage()
{
    retranslateUi();
}

void MathOptions::UpdateTheme()
{
}

void MathOptions::UpdateFont()
{ 
    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    ui::set_form_font(this, font);
}

} // namespace dialogs
} // namespace pv
