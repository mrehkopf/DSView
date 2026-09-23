/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
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

#include "searchcombobox.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPoint>
#include <QLineEdit>
#include <QScrollBar>
#include <QGuiApplication>
#include <QScreen>
#include <QRect>
#include "../config/appconfig.h"
#include "../appcontrol.h"
#include "../ui/fn.h"

//----------------------ComboButtonItem

ComboButtonItem::ComboButtonItem(QWidget *parent, ISearchItemClick *click, void *data_handle)
:QPushButton(parent)
{
    _click = click;
    _data_handle = data_handle;
}

void ComboButtonItem::mousePressEvent(QMouseEvent *e)
{
    (void)e;
    
    if (_click != NULL){
        _click->OnItemClick(this, _data_handle);
    }
}

//----------------------SearchComboBox

SearchComboBox::SearchComboBox(QWidget *parent)
    : QDialog(parent)
{ 
    _bShow = false;
    _bClosing = false;
    _item_click = NULL;

    // A Qt::Popup is anchored to its parent window by the platform, so the
    // requested position is honoured on Wayland as well, where a plain
    // Qt::Dialog cannot place itself and ends up centered by the compositor.
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
}

SearchComboBox::~SearchComboBox(){
    //release resource
    for (auto o : _items){
        delete o;
    }
    _items.clear();  
}

void SearchComboBox::ShowDlg(QWidget *editline)
{
    if (_bShow)
        return;   

    _bShow = true;
    
    int w = 350; 
    int h = 550;
    int eh = 20;

    QPoint topLeft;
    QRect avail;

    if (editline != NULL)
    {
        w = editline->width();
        topLeft = editline->mapToGlobal(QPoint(0, 0));

        QScreen *screen = QGuiApplication::screenAt(topLeft);
        if (screen == NULL){
            screen = QGuiApplication::primaryScreen();
        }
        if (screen != NULL){
            avail = screen->availableGeometry();
            h = qMin(h, avail.height());
        }
    } 

    this->setFixedSize(w, h);

    QVBoxLayout *grid = new QVBoxLayout(this);
    this->setLayout(grid);
    grid->setContentsMargins(0,0,0,0);
    grid->setAlignment(Qt::AlignTop);
    grid->setSpacing(2);

    QLineEdit *edit = new QLineEdit(this);
    edit->setMaximumWidth(this->width()); 
    grid->addWidget(edit);    
    eh = edit->height();

    QWidget *panel= new QWidget(this);
    panel->setContentsMargins(0,0,0,0);
    panel->setFixedSize(w, h - eh);
    grid->addWidget(panel);   

    QWidget *listPanel =  new QWidget(panel);
    QVBoxLayout *listLay = new QVBoxLayout(listPanel);
    listLay->setContentsMargins(2, 2, 20, 2);
    listLay->setSpacing(0);
    listLay->setAlignment(Qt::AlignTop);

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);

    for(auto o : _items)
    { 
        ComboButtonItem *bt = new ComboButtonItem(panel, this, o);
        bt->setText(o->_name);
        bt->setObjectName("flat");
        bt->setMaximumWidth(w - 20);
        bt->setMinimumWidth(w - 20);
        o->_control = bt;
        bt->setFont(font);

        listLay->addWidget(bt);        
    } 
 
    _scroll = new QScrollArea(panel);
    _scroll->setWidget(listPanel);
    _scroll->setStyleSheet("QScrollArea{border:none;}");
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setFixedSize(w, h - eh);

    if (editline != NULL)
    {
        // Drop down directly below the edit line, flipping above it when there
        // is not enough room left on the screen.
        QPoint pos(topLeft.x(), topLeft.y() + editline->height());

        if (avail.isNull() == false)
        {
            if (pos.y() + h > avail.bottom())
            {
                if (topLeft.y() - h >= avail.top()){
                    pos.setY(topLeft.y() - h);
                }
                else{
                    pos.setY(qMax(avail.top(), avail.bottom() - h));
                }
            }

            if (pos.x() + w > avail.right()){
                pos.setX(qMax(avail.left(), avail.right() - w));
            }
        }

        this->move(pos);
    } 

    connect(edit, SIGNAL(textEdited(const QString &)), 
                    this, SLOT(on_keyword_changed(const QString &)));

    this->show();

    edit->setFocus();
}

void SearchComboBox::AddDataItem(QString id, QString name, void *data_handle)
{
    SearchDataItem *item = new SearchDataItem();
    item->_id = id;
    item->_name = name;
    item->_data_handle = data_handle;
    this->_items.push_back(item);
}

 void SearchComboBox::changeEvent(QEvent *event)
 {
    if (event->type() == QEvent::ActivationChange){
        if (this->isActiveWindow() == false){
            close_and_release();
            return;
        }
    }
    
    QWidget::changeEvent(event);
 }

 void SearchComboBox::closeEvent(QCloseEvent *event)
 {
    QDialog::closeEvent(event);

    // Covers the closes triggered by Qt itself, not only close_and_release().
    if (_bClosing == false){
        _bClosing = true;
        this->deleteLater();
    }
 }

 void SearchComboBox::mousePressEvent(QMouseEvent *event)
 {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint pt = event->position().toPoint();
#else
    const QPoint pt = event->pos();
#endif

    // As a popup this receives the clicks made outside of it as well.
    if (this->rect().contains(pt) == false){
        close_and_release();
        return;
    }

    QDialog::mousePressEvent(event);
 }

 void SearchComboBox::keyPressEvent(QKeyEvent *event)
 {
    if (event->key() == Qt::Key_Escape){
        close_and_release();
        return;
    }

    QDialog::keyPressEvent(event);
 }

 void SearchComboBox::close_and_release()
 {
    if (_bClosing)
        return;

    _bClosing = true;
    this->close();
    this->deleteLater();
 }

 void SearchComboBox::OnItemClick(void *sender, void *data_handle)
 {
    (void)sender;

     if (data_handle != NULL && _item_click){
         SearchDataItem *item = (SearchDataItem*)data_handle;
         ISearchItemClick *click = _item_click;
         void *handle = item->_data_handle;
         close_and_release();
         click->OnItemClick(this, handle);
     }
 }

 void SearchComboBox::on_keyword_changed(const QString &value)
 {
     if (_items.size() == 0)
        return;

     for(auto o : _items)
     {
         if (value == "" 
            || o->_name.indexOf(value, 0, Qt::CaseInsensitive) >= 0
            || o->_id.indexOf(value, 0, Qt::CaseInsensitive) >= 0){
                if (o->_control->isHidden()){
                    o->_control->show();
                }
         }
         else if (o->_control->isHidden() == false){
             o->_control->hide();
         }
     }
     
    _scroll->verticalScrollBar()->setValue(0);
 }
