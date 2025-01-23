/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
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

#include <math.h>

#include "measuredock.h"
#include "../sigsession.h"
#include "../view/cursor.h"
#include "../view/view.h"
#include "../view/viewport.h"
#include "../view/timemarker.h"
#include "../view/ruler.h"
#include "../view/logicsignal.h"
#include "../data/signaldata.h"
#include "../data/snapshot.h" 
#include "../dialogs/dsdialog.h"
#include "../dialogs/dsmessagebox.h"
#include "../config/appconfig.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"
#include <QObject>
#include <QPainter> 
#include "../appcontrol.h"
#include "../ui/fn.h"
#include "../log.h"
#include "../ui/xtoolbutton.h"

using namespace boost;

namespace pv {
namespace dock {

using namespace pv::view;

MeasureDock::MeasureDock(QWidget *parent, View &view, SigSession *session) :
    QScrollArea(parent),
    _session(session),
    _view(view)
{     
    _widget = new QWidget(this);  

    _dist_pannel = NULL;
    _edge_pannel = NULL;
    _bSetting = false;

    _mouse_groupBox = new QGroupBox(_widget);
    _fen_checkBox = new QCheckBox(_widget);
    _fen_checkBox->setChecked(true);
    _width_label = new QLabel(_widget);
    _period_label = new QLabel(_widget);
    _freq_label = new QLabel(_widget);
    _duty_label = new QLabel(_widget);
    _samples_label = new QLabel(_widget);

    _w_label = new QLabel(_widget);
    _p_label = new QLabel(_widget);
    _f_label = new QLabel(_widget);
    _d_label = new QLabel(_widget);
    _s_label = new QLabel(_widget);

    QGridLayout *mouse_layout = new QGridLayout();
    mouse_layout->setVerticalSpacing(5);
    mouse_layout->setHorizontalSpacing(5);
    mouse_layout->addWidget(_fen_checkBox, 0, 0, 1, 5);
    mouse_layout->addWidget(_w_label, 1, 0);
    mouse_layout->addWidget(_width_label, 1, 1);
    mouse_layout->addWidget(_p_label, 1, 3);
    mouse_layout->addWidget(_period_label, 1, 4);

    mouse_layout->addWidget(_d_label, 2, 0);
    mouse_layout->addWidget(_duty_label, 2, 1);
    mouse_layout->addWidget(_f_label, 2, 3);
    mouse_layout->addWidget(_freq_label, 2, 4);    
 
    mouse_layout->addWidget(_s_label, 3, 0);
    mouse_layout->addWidget(_samples_label, 3, 1);

    _mouse_groupBox->setLayout(mouse_layout);
    mouse_layout->setContentsMargins(5, 15, 5, 5);

    /* cursor distance group */
    _dist_groupBox = new QGroupBox(_widget);
    _dist_groupBox->setMinimumWidth(300);
    _dist_add_btn = new XToolButton(_widget);   

    _dist_layout = new QGridLayout(_widget);
    _dist_layout->setVerticalSpacing(5);
    _dist_layout->addWidget(_dist_add_btn, 0, 0);
    _dist_layout->addWidget(new QLabel(_widget), 0, 1, 1, 3);
    _add_dec_label = new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TIME_SAMPLES), "Time/Samples"), _widget);
    _dist_layout->addWidget(_add_dec_label, 0, 4);
    _dist_layout->addWidget(new QLabel(_widget), 0, 5, 1, 2);
    _dist_layout->setColumnStretch(1, 50);
    _dist_layout->setColumnStretch(6, 100);
    _dist_groupBox->setLayout(_dist_layout);
    _dist_layout->setContentsMargins(5, 15, 5, 5);

    /* cursor edges group */
    _edge_groupBox = new QGroupBox(_widget);
    _edge_groupBox->setMinimumWidth(300);
    _edge_add_btn = new XToolButton(_widget);

    _channel_label = new QLabel(_widget);
    _edge_label = new QLabel(_widget);
    _edge_layout = new QGridLayout(_widget);
    _edge_layout->setVerticalSpacing(5);
    _edge_layout->addWidget(_edge_add_btn, 0, 0);
    _edge_layout->addWidget(new QLabel(_widget), 0, 1, 1, 4);
    _edge_layout->addWidget(_channel_label, 0, 5);
    _edge_layout->addWidget(_edge_label, 0, 6);
    _edge_layout->setColumnStretch(1, 50);
    _edge_groupBox->setLayout(_edge_layout);
    _edge_layout->setContentsMargins(5, 15, 5, 5);

    /* cursors group */
    _time_label = new QLabel(_widget);
    _cursor_groupBox = new QGroupBox(_widget);
    _cursor_layout = new QGridLayout(_widget);
    _cursor_layout->addWidget(_time_label, 0, 2);
    _cursor_layout->addWidget(new QLabel(_widget), 0, 3);
    _cursor_layout->setColumnStretch(3, 1);
    _cursor_layout->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    _cursor_groupBox->setLayout(_cursor_layout);
    _cursor_layout->setContentsMargins(5, 15, 5, 5);

    QVBoxLayout *layout = new QVBoxLayout(_widget);
    layout->addWidget(_mouse_groupBox);
    layout->addWidget(_dist_groupBox);
    layout->addWidget(_edge_groupBox);
    layout->addWidget(_cursor_groupBox);
    layout->addStretch(1);
    _widget->setLayout(layout);

    this->setWidget(_widget);
    _widget->setGeometry(0, 0, sizeHint().width(), 2000);
    _widget->setObjectName("measureWidget");

    add_dist_measure();

    connect(_dist_add_btn, SIGNAL(clicked()), this, SLOT(add_dist_measure()));
    connect(_edge_add_btn, SIGNAL(clicked()), this, SLOT(add_edge_measure()));
    connect(_fen_checkBox, SIGNAL(stateChanged(int)), &_view, SLOT(set_measure_en(int)));
    connect(&_view, SIGNAL(measure_updated()), this, SLOT(measure_updated()));

    ADD_UI(this);
}

MeasureDock::~MeasureDock()
{
    REMOVE_UI(this);
}

void MeasureDock::retranslateUi()
{
    _mouse_groupBox->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MOUSE_MEASUREMENT), "Mouse measurement"));
    _fen_checkBox->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE_FLOATING_MEASUREMENT), "Enable floating measurement"));
    _dist_groupBox->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CURSOR_DISTANCE), "Cursor Distance"));
    _edge_groupBox->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_EDGES), "Edges"));
    _cursor_groupBox->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CURSORS), "Cursors"));

    _channel_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel"));
    _edge_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RIS_OR_FAL_EDGE), "Rising/Falling/Edges"));
    _time_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TIME_SAMPLES), "Time/Samples"));
    _add_dec_label->setText(_time_label->text());

    /*
    _w_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_W), "W: "));
    _p_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_P), "P: "));
    _f_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_F), "F: "));
    _d_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_D), "D: "));
    */

    _w_label->setText("W:");
    _p_label->setText("P:");
    _f_label->setText("F:");
    _d_label->setText("D:");
    _s_label->setText("S:");
}

void MeasureDock::reStyle()
{
    QString iconPath = GetIconPath();

    _dist_add_btn->setIcon(QIcon(iconPath+"/add.svg"));
    _edge_add_btn->setIcon(QIcon(iconPath+"/add.svg"));

    auto mode_rows = get_mode_rows();

    for (auto it = mode_rows->_dist_row_list.begin(); it != mode_rows->_dist_row_list.end(); it++)
    {
        (*it).del_bt->setIcon(QIcon(iconPath+"/del.svg"));
    }

    for (auto it = mode_rows->_edge_row_list.begin(); it != mode_rows->_edge_row_list.end(); it++)
    {
        (*it).del_bt->setIcon(QIcon(iconPath+"/del.svg"));
    }

    for (auto it = mode_rows->_opt_row_list.begin(); it != mode_rows->_opt_row_list.end(); it++)
    {
        (*it).del_bt->setIcon(QIcon(iconPath+"/del.svg"));
    }

    update_dist();
}

void MeasureDock::reload()
{
    if (_session->get_device()->get_work_mode() == LOGIC)
        _edge_groupBox->setVisible(true);
    else
        _edge_groupBox->setVisible(false);

    _bSetting = true;

    build_dist_pannel();
    build_edge_pannel();
    build_cursor_pannel();

    _bSetting = false;

    reCalc();
}

void MeasureDock::measure_updated()
{
    _width_label->setText(_view.get_measure("width"));
    _period_label->setText(_view.get_measure("period"));
    _freq_label->setText(_view.get_measure("frequency"));
    _duty_label->setText(_view.get_measure("duty"));
    _samples_label->setText(_view.get_measure("samples"));
    adjusLabelSize();
}

void MeasureDock::build_dist_pannel()
{
    if (_dist_pannel != NULL){
        _dist_pannel->deleteLater();
        _dist_pannel = NULL;
    }

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);

    QGridLayout  *lay = new QGridLayout();
    _dist_pannel = new QWidget();    
    _dist_pannel->setLayout(lay);
    lay->setColumnStretch(1, 50);
    lay->setColumnStretch(6, 100);
    lay->setVerticalSpacing(5);
    lay->setContentsMargins(0,0,0,0);
    
    int dex = 0;
    QLabel cal_lb;
    cal_lb.setFont(font);
    int bt_w = cal_lb.fontMetrics().horizontalAdvance("22") + 8;

    auto mode_rows = get_mode_rows();

    for (auto &o : mode_rows->_dist_row_list)
    {
        QWidget *row_widget = new QWidget(_widget);
        row_widget->setContentsMargins(0,0,0,0);
        QHBoxLayout *row_layout = new QHBoxLayout(row_widget);
        row_layout->setAlignment(Qt::AlignLeft);
        row_layout->setContentsMargins(0,0,0,0);
        row_layout->setSpacing(0);
        row_widget->setLayout(row_layout);

        QString iconPath = GetIconPath();
        XToolButton *del_btn = new XToolButton(row_widget);
        del_btn->setIcon(QIcon(iconPath+"/del.svg"));
        del_btn->setCheckable(true);
        //tr
        QPushButton *s_btn = new QPushButton("", row_widget);
        //tr
        QPushButton *e_btn = new QPushButton("", row_widget);
        QLabel *r_label = new QLabel(row_widget);
        //tr
        QLabel *g_label = new QLabel("-", row_widget);
        g_label->setContentsMargins(0,0,0,0);

        row_layout->addWidget(del_btn);
        row_layout->addSpacing(5);
        row_layout->addWidget(s_btn);
        row_layout->addWidget(g_label);
        row_layout->addWidget(e_btn);
        row_layout->addSpacing(5);
        row_layout->addWidget(r_label, 100);

        r_label->setFont(font);
        s_btn->setFont(font);
        e_btn->setFont(font);
        g_label->setFont(font);

        s_btn->setFixedWidth(bt_w);
        e_btn->setFixedWidth(bt_w);

        lay->addWidget(row_widget, dex++, 0, 1, 7);

        if (o.r_label != NULL){
            r_label->setText(o.r_label->text());
        }

        o.del_bt = del_btn;
        o.start_bt = s_btn;
        o.end_bt = e_btn;
        o.r_label = r_label;

        if (o.cursor1 != -1){
            o.start_bt->setText(QString::number(o.cursor1));
            set_cursor_btn_color(o.start_bt);
        }
        if (o.cursor2 != -1){
            o.end_bt->setText(QString::number(o.cursor2));
            set_cursor_btn_color(o.end_bt);
        }

        connect(del_btn, SIGNAL(clicked()), this, SLOT(del_dist_measure()));
        connect(s_btn, SIGNAL(clicked()), this, SLOT(popup_all_coursors()));
        connect(e_btn, SIGNAL(clicked()), this, SLOT(popup_all_coursors()));
    }

    _dist_layout->addWidget(_dist_pannel, 1, 0, 1, 7);
}

void MeasureDock::add_dist_measure()
{
    auto mode_rows = get_mode_rows();

    if (mode_rows->_dist_row_list.size() < Max_Measure_Limits)
    {
        cursor_row_info inf;
        inf.cursor1 = -1;
        inf.cursor2 = -1;
        inf.box = NULL;
        inf.del_bt = NULL;
        inf.start_bt = NULL;
        inf.end_bt = NULL;
        inf.r_label = NULL;
        inf.channelIndex = 0;

        mode_rows->_dist_row_list.push_back(inf);

        build_dist_pannel();

        adjusLabelSize();      
    }  
}

void MeasureDock::del_dist_measure()
{
    auto src = dynamic_cast<QToolButton *>(sender());
    assert(src);

    auto mode_rows = get_mode_rows();

    for (auto it = mode_rows->_dist_row_list.begin(); it != mode_rows->_dist_row_list.end(); it++)
    {
        if ((*it).del_bt == src){
            mode_rows->_dist_row_list.erase(it);
            build_dist_pannel();
            break;
        }
    }
}

void MeasureDock::build_edge_pannel()
{
    if (_edge_pannel != NULL)
    {
        _edge_pannel->deleteLater();
        _edge_pannel = NULL;
    }

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    QGridLayout  *lay = new QGridLayout();
    _edge_pannel = new QWidget();   
    _edge_pannel->setLayout(lay);
    lay->setColumnStretch(1, 50);
    lay->setColumnStretch(6, 100);
    lay->setVerticalSpacing(5);
    lay->setContentsMargins(0,0,0,0);
  
    int dex = 0;
    QLabel cal_lb;
    cal_lb.setFont(font);
    int bt_w = cal_lb.fontMetrics().horizontalAdvance("22") + 8;

    auto mode_rows = get_mode_rows();

    for (auto &o : mode_rows->_edge_row_list)
    {
        QWidget *row_widget = new QWidget(_widget);
        row_widget->setContentsMargins(0,0,0,0);
        QHBoxLayout *row_layout = new QHBoxLayout(row_widget);
        row_layout->setAlignment(Qt::AlignLeft);
        row_layout->setContentsMargins(0,0,0,0);
        row_layout->setSpacing(0);
        row_widget->setLayout(row_layout);

        QString iconPath = GetIconPath();
        XToolButton *del_btn = new XToolButton(row_widget);
        del_btn->setIcon(QIcon(iconPath+"/del.svg"));
        del_btn->setCheckable(true);
        //tr
        QPushButton *s_btn = new QPushButton(" ", row_widget);
        //tr
        QPushButton *e_btn = new QPushButton(" ", row_widget);
        QLabel *r_label = new QLabel(row_widget);
        //tr
        QLabel *g_label = new QLabel("-", row_widget);
        g_label->setContentsMargins(0,0,0,0);
        //tr
        QLabel *a_label = new QLabel("@", row_widget);
        a_label->setContentsMargins(0,0,0,0);
        QComboBox *ch_cmb = create_probe_selector(row_widget);
        ch_cmb->setFixedWidth(50);

        if (o.channelIndex < ch_cmb->count()){
            ch_cmb->setCurrentIndex(o.channelIndex);
        }

        row_layout->addWidget(del_btn);
        row_layout->addSpacing(5);
        row_layout->addWidget(s_btn);
        row_layout->addWidget(g_label);
        row_layout->addWidget(e_btn);
        row_layout->addWidget(a_label);
        row_layout->addWidget(ch_cmb);
        row_layout->addSpacing(5);
        row_layout->addWidget(r_label, 100);

        g_label->setFont(font);
        a_label->setFont(font);
        s_btn->setFont(font);
        e_btn->setFont(font);
        r_label->setFont(font);
        ch_cmb->setFont(font);

        s_btn->setFixedWidth(bt_w);
        e_btn->setFixedWidth(bt_w);

        lay->addWidget(row_widget, dex++, 0, 1, 7);

        if (o.r_label != NULL){
            r_label->setText(o.r_label->text());
        }

        o.del_bt = del_btn;
        o.start_bt = s_btn;
        o.end_bt = e_btn;
        o.r_label = r_label;
        o.box = ch_cmb;

        if (o.cursor1 != -1){
            o.start_bt->setText(QString::number(o.cursor1));
            set_cursor_btn_color(o.start_bt);
        }
        if (o.cursor2 != -1){
            o.end_bt->setText(QString::number(o.cursor2));
            set_cursor_btn_color(o.end_bt);
        }

        connect(del_btn, SIGNAL(clicked()), this, SLOT(del_edge_measure()));
        connect(s_btn, SIGNAL(clicked()), this, SLOT(popup_all_coursors()));
        connect(e_btn, SIGNAL(clicked()), this, SLOT(popup_all_coursors()));
        connect(ch_cmb, SIGNAL(currentIndexChanged(int)), this, SLOT(on_edge_channel_selected()));
    }

    _edge_layout->addWidget(_edge_pannel, 1, 0, 1, 7);
}

void MeasureDock::on_edge_channel_selected()
{
    QComboBox *box = dynamic_cast<QComboBox*>(sender());
    auto mode_rows = get_mode_rows();

    if (box != NULL && !_bSetting){        
        for (auto &o : mode_rows->_edge_row_list)
        {
            if (o.box == box){
                o.channelIndex = box->currentIndex();
                break;
            }
        }
    }

    update_edge();
    adjusLabelSize();
}

void MeasureDock::add_edge_measure()
{
    auto mode_rows = get_mode_rows();

    if (mode_rows->_edge_row_list.size() < Max_Measure_Limits)
    {
        cursor_row_info inf;
        inf.cursor1 = -1;
        inf.cursor2 = -1;
        inf.box = NULL;
        inf.del_bt = NULL;
        inf.start_bt = NULL;
        inf.end_bt = NULL;
        inf.r_label = NULL;
        inf.channelIndex = 0;

        mode_rows->_edge_row_list.push_back(inf);
        build_edge_pannel();

        adjusLabelSize();
    } 
}

void MeasureDock::del_edge_measure()
{
    QToolButton* src = dynamic_cast<QToolButton *>(sender());
    assert(src); 
    auto mode_rows = get_mode_rows();

    for (auto it =mode_rows->_edge_row_list.begin(); it != mode_rows->_edge_row_list.end(); it++)
    {
        if ((*it).del_bt == src){
            mode_rows->_edge_row_list.erase(it);
            build_edge_pannel();
            break;
        }
    }
}

void MeasureDock::popup_all_coursors()
{
    auto &cursor_list = _view.get_cursorList();

    if (cursor_list.empty()) {
        QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_PLEASE_INSERT_CURSOR), 
                                       "Please insert cursor before using cursor measure."));
        MsgBox::Show(strMsg);
        return;
    }

    _sel_btn = qobject_cast<QPushButton *>(sender());

    QDialog cursor_dlg(_widget);
    cursor_dlg.setWindowFlags(Qt::FramelessWindowHint | Qt::Popup | Qt::WindowSystemMenuHint |
                              Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint);

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);

    int index = 0;
    QGridLayout *glayout = new QGridLayout(&cursor_dlg);

    for(auto i = cursor_list.begin(); i != cursor_list.end(); i++) {
        QPushButton *cursor_btn = new QPushButton(&cursor_dlg);
        cursor_btn->setText(QString::number(index+1));
        set_cursor_btn_color(cursor_btn);
        cursor_btn->setFont(font);
        glayout->addWidget(cursor_btn, index/4, index%4, 1, 1);

        connect(cursor_btn, SIGNAL(clicked()), &cursor_dlg, SLOT(accept()));
        connect(cursor_btn, SIGNAL(clicked()), this, SLOT(set_sel_cursor()));
        index++;
    }

    QRect sel_btn_rect = _sel_btn->geometry();
    sel_btn_rect.moveTopLeft(_sel_btn->parentWidget()->mapToGlobal(sel_btn_rect.topLeft()));
    cursor_dlg.setGeometry(sel_btn_rect.left(), sel_btn_rect.bottom()+10,
                           cursor_dlg.width(), cursor_dlg.height());
    cursor_dlg.exec();
}

void MeasureDock::set_sel_cursor()
{
    assert(_sel_btn);
    QPushButton *sel_cursor_bt = qobject_cast<QPushButton *>(sender());
    int type = 0;
    cursor_row_info *inf = NULL;
    auto mode_rows = get_mode_rows();

    if (type == 0)
    {
        for (auto &o : mode_rows->_dist_row_list){
            if (o.start_bt == _sel_btn || o.end_bt == _sel_btn){
                inf = &o;
                type = 1;
                break;
            }
        }
    }

    if (type == 0)
    {
        for (auto &o : mode_rows->_edge_row_list){
            if (o.start_bt == _sel_btn || o.end_bt == _sel_btn){
                inf = &o;
                type = 2;
                break;
            }
        }
    } 

    assert(inf);

    _sel_btn->setText(sel_cursor_bt->text());
    set_cursor_btn_color(_sel_btn);

    if (inf->start_bt == _sel_btn){
        inf->cursor1 = sel_cursor_bt->text().toInt();
    }
    else if (inf->end_bt == _sel_btn){
        inf->cursor2 = sel_cursor_bt->text().toInt();
    }

    if (type == 1)
        update_dist();
    else
        update_edge();

    adjusLabelSize();
}

void MeasureDock::update_dist()
{
    auto &cursor_list = _view.get_cursorList();

    QColor bkColor = AppConfig::Instance().GetStyleColor(); 

    auto mode_rows = get_mode_rows();

    for (auto &inf : mode_rows->_dist_row_list)
    {
        if (inf.start_bt == NULL)
            break;

        if (inf.cursor1 != -1) {
            if (inf.cursor1 > (int)cursor_list.size()) {
                inf.start_bt->setText("");
                inf.cursor1 = -1;
            }
        }
        set_cursor_btn_color(inf.start_bt);

        if (inf.cursor2 != -1) {
            if (inf.cursor2 > (int)cursor_list.size()) {
                inf.end_bt->setText("");                
                inf.cursor2 = -1;
            }
        }
        set_cursor_btn_color(inf.end_bt);

        if (inf.cursor1 != -1 && inf.cursor2 != -1) {
            int64_t delta = _view.get_cursor_samples(inf.cursor1-1) -
                            _view.get_cursor_samples(inf.cursor2-1);
            QString delta_text = _view.get_cm_delta(inf.cursor1-1, inf.cursor2-1) +
                                 "/" + QString::number(delta);
            if (delta < 0)
                delta_text.replace('+', '-');
            inf.r_label->setText(delta_text); 
        }
        else {
            inf.r_label->setText(" ");
        }
    }
}

void MeasureDock::update_edge()
{ 
    auto &cursor_list = _view.get_cursorList();
    auto mode_rows = get_mode_rows();

    for (auto &inf : mode_rows->_edge_row_list)
    {
        if (inf.start_bt == NULL)
            break;

        if (inf.cursor1 != -1) {
            if (inf.cursor1 > (int)cursor_list.size()) {
                inf.start_bt->setText("");
                set_cursor_btn_color(inf.start_bt);
                inf.cursor1 = -1;
            }
        }
        if (inf.cursor2 != -1) {
            if (inf.cursor2 > (int)cursor_list.size()) {
                inf.end_bt->setText("");
                set_cursor_btn_color(inf.end_bt);
                inf.cursor2 = -1;
            }
        }

        bool mValid = false;
        if (inf.cursor1 != -1 && inf.cursor2 != -1) {
            uint64_t rising_edges;
            uint64_t falling_edges;

            for(auto s : _session->get_signals()) {
                if (s->signal_type() == SR_CHANNEL_LOGIC
                        && s->enabled()
                        && s->get_index() == inf.box->currentText().toInt())
                  {
                    view::LogicSignal *logicSig = (view::LogicSignal*)s;

                    if (logicSig->edges(_view.get_cursor_samples(inf.cursor2-1),
                            _view.get_cursor_samples(inf.cursor1-1), rising_edges, falling_edges)) 
                    {
                        QString delta_text = QString::number(rising_edges) + "/" +
                                             QString::number(falling_edges) + "/" +
                                             QString::number(rising_edges + falling_edges);
                        inf.r_label->setText(delta_text);
                        mValid = true;
                        break;
                    }
                }
            }
        }

        if (!mValid)
            inf.r_label->setText("-/-/-");
    }
}

void MeasureDock::update_cursor_info()
{ 
    auto &cursor_list = _view.get_cursorList();
    auto mode_rows = get_mode_rows();

    int num_cursors = cursor_list.size();
    int num_rows = mode_rows->_opt_row_list.size();

    if (num_rows == 0){
        return;
    }

    assert(num_cursors == num_rows);

    for(int i = 0; i < num_cursors; i++)
    {   
        if (mode_rows->_opt_row_list[i].info_label != NULL){
            QString cur_pos = _view.get_cm_time(i) + "/" 
                    + QString::number(_view.get_cursor_samples(i));
            mode_rows->_opt_row_list[i].info_label->setText(cur_pos);
        }
    }
}

void MeasureDock::set_cursor_btn_color(QPushButton *btn)
{
    QColor bkColor = AppConfig::Instance().GetStyleColor();
    bool isCursor = false;
    const unsigned int start = btn->text().toInt(&isCursor);
    QColor cursor_color = isCursor ? view::Ruler::GetColorByCursorOrder(start) : bkColor;
    set_cursor_btn_color(btn, cursor_color, bkColor, isCursor);
}

void MeasureDock::set_cursor_btn_color(QPushButton *btn, QColor cursorColor, QColor bkColor, bool isCursor)
{  
    QString border_width = isCursor ? "0px" : "1px";
    QString hoverColor = isCursor ? cursorColor.darker().name() : bkColor.name();
    QString normal = "{background-color:" + cursorColor.name() +
            "; color:black" + "; border-width:" + border_width + ";}";
    QString hover = "{background-color:" + hoverColor +
            "; color:black" + "; border-width:" + border_width + ";}";
    QString style = "QPushButton:hover" + hover +
                    "QPushButton" + normal;
    btn->setStyleSheet(style);
}

QComboBox* MeasureDock::create_probe_selector(QWidget *parent)
{
    DsComboBox *selector = new DsComboBox(parent);
    update_probe_selector(selector);
    return selector;
}

void MeasureDock::update_probe_selector(QComboBox *selector)
{
    selector->clear(); 

    for(auto s : _session->get_signals()) {
        if (s->signal_type() == SR_CHANNEL_LOGIC && s->enabled()){
            selector->addItem(QString::number(s->get_index()));
        }
    }
}

void MeasureDock::adjusLabelSize()
{  
   this->adjust_form_size(this);
}

void MeasureDock::cursor_moving()
{
    if (_view.cursors_shown()) {      
        update_cursor_info();
    }

    update_dist();
}

void MeasureDock::reCalc()
{ 
    update_dist();
    update_edge();
    update_cursor_info();

    adjusLabelSize();
}

void MeasureDock::goto_cursor()
{
    QPushButton *src = qobject_cast<QPushButton *>(sender());
    assert(src);

    int index = 0;
    auto mode_rows = get_mode_rows();

    for (auto it = mode_rows->_opt_row_list.begin(); it != mode_rows->_opt_row_list.end(); it++)
    {
        if ( (*it).goto_bt == src){
            _view.set_cursor_middle(index);
            break;
        }
        index++;
    }
}

void MeasureDock::cursor_update()
{
    update_dist();
    update_edge();
    build_cursor_pannel();
}

void MeasureDock::build_cursor_pannel()
{  
    auto mode_rows = get_mode_rows();

    for (auto &row : mode_rows->_opt_row_list)
    {
        if (row.del_bt != NULL){
            row.del_bt->deleteLater();
            row.goto_bt->deleteLater();
            row.info_label->deleteLater();
        }
    }
    mode_rows->_opt_row_list.clear();

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);

    QLabel cal_lb;
    cal_lb.setFont(font);
    int bt_w = cal_lb.fontMetrics().horizontalAdvance("22") + 8;

    int index = 1;
    int cursor_dex = 0;
    QString iconPath = GetIconPath();
    auto &cursor_list = _view.get_cursorList();

    if (cursor_list.size() == 0){
        return;
    }

    for(auto it = cursor_list.begin(); it != cursor_list.end(); it++) {
        XToolButton *del_btn = new XToolButton(_widget);
        del_btn->setIcon(QIcon(iconPath+"/del.svg"));
        del_btn->setCheckable(true);
        QPushButton *cursor_pushButton = new QPushButton(QString::number(index), _widget);
        set_cursor_btn_color(cursor_pushButton);

        QString cur_pos = _view.get_cm_time(cursor_dex) + "/" 
                    + QString::number(_view.get_cursor_samples(cursor_dex));
        QLabel *curpos_label = new QLabel(cur_pos, _widget); 

        _cursor_layout->addWidget(del_btn, 1+index, 0);
        _cursor_layout->addWidget(cursor_pushButton, 1 + index, 1);
        _cursor_layout->addWidget(curpos_label, 1 + index, 2);
        curpos_label->setFont(font);
        cursor_pushButton->setFont(font);
        cursor_pushButton->setFixedWidth(bt_w);

        connect(del_btn, SIGNAL(clicked()), this, SLOT(del_cursor()));
        connect(cursor_pushButton, SIGNAL(clicked()), this, SLOT(goto_cursor()));

        cursor_opt_info inf = {del_btn, cursor_pushButton, curpos_label, (*it)};
        mode_rows->_opt_row_list.push_back(inf);

        index++;
        cursor_dex++;
    }

    adjusLabelSize();
}

void MeasureDock::del_cursor()
{
    auto *src = qobject_cast<QToolButton *>(sender());
    assert(src);
    
    Cursor* cursor = NULL;
    auto &cursor_list = _view.get_cursorList();
    auto mode_rows = get_mode_rows();
    
    for (auto it = mode_rows->_opt_row_list.begin(); it != mode_rows->_opt_row_list.end(); it++)
    {
        if ((*it).del_bt == src){   
            cursor = (*it).cursor;
            break;
        }
    }

    if (cursor)
        _view.del_cursor(cursor);
    if (cursor_list.empty())
        _view.show_cursors(false);

    cursor_update();
    _view.update();
}

void MeasureDock::UpdateLanguage()
{
    retranslateUi();
}

void MeasureDock::UpdateTheme()
{
    reStyle();
}

void MeasureDock::UpdateFont()
{
    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    ui::set_form_font(this, font);
    font.setPointSizeF(font.pointSizeF() + 1);
    this->parentWidget()->setFont(font);

    adjusLabelSize();
}

void MeasureDock::adjust_form_size(QWidget *wid)
{
    assert(wid);
 
    QGroupBox *mainGroup = _dist_groupBox;

    QString str = "+12345678999ms/12345678999";
    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    QFontMetrics fm(font); 
    int max_label_width = fm.horizontalAdvance(str) + 100;

    auto labels = wid->findChildren<QLabel*>();
    for(auto o : labels)
    { 
        QRect rc = fm.boundingRect(o->text());
        QSize size(rc.width() + 15, rc.height()); 
        o->setFixedSize(size);
    }

    int mouse_info_label_width = fm.horizontalAdvance("############");
    _width_label->setFixedWidth(mouse_info_label_width);
    _period_label->setFixedWidth(mouse_info_label_width);
    _freq_label->setFixedWidth(mouse_info_label_width);
    _duty_label->setFixedWidth(mouse_info_label_width);
    _samples_label->setFixedWidth(mouse_info_label_width);
   
    auto groups = wid->findChildren<QGroupBox*>();
    for(auto o : groups)
    { 
        o->setFixedWidth(max_label_width + 10);
    }

    QWidget *pannel = dynamic_cast<QWidget*>(mainGroup->parent());
    if (pannel != NULL){ 
        pannel->setFixedWidth(max_label_width + 20);
    }
}

row_list_item* MeasureDock::get_mode_rows()
{
    int mode = _session->get_device()->get_work_mode();
    int dex = 0;

    if (mode == LOGIC){
        dex = 0;
    }
    else if (mode == DSO){
        dex = 1;
    }
    else if (mode == ANALOG){
        dex = 2;
    }

    for (int i=0; i<MODE_ROWS_LENGTH; i++)
    {  
        if (i == dex){
            continue;
        }
    
        auto rows = &_mode_rows[i];

        for(auto &o : rows->_dist_row_list){               
            o.del_bt = NULL;
            o.start_bt = NULL;
            o.end_bt = NULL;
            o.r_label = NULL;
            o.box = NULL;
        }

        for(auto &o : rows->_edge_row_list){               
            o.del_bt = NULL;
            o.start_bt = NULL;
            o.end_bt = NULL;
            o.r_label = NULL;
            o.box = NULL;
        }

        for (auto &row : rows->_opt_row_list)
        {
            if (row.del_bt != NULL){
                row.del_bt->deleteLater();
                row.goto_bt->deleteLater();
                row.info_label->deleteLater();
                row.del_bt = NULL;
                row.goto_bt = NULL;
                row.info_label = NULL;
            }
        }
        rows->_opt_row_list.clear();       
    }

    _mode_rows[dex]._mode_type = mode;

    return &_mode_rows[dex];
}

} // namespace dock
} // namespace pv
