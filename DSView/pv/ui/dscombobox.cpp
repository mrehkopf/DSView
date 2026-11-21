/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 * 
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
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

#include "dscombobox.h"
#include <QFontMetrics>
#include <QString>
#include <QGuiApplication>
#include <QScreen>
#include <QAbstractItemView>
#include <QAbstractItemDelegate>
#include <QScrollBar>
#include <QStyleOption>
#include "../config/appconfig.h"

DsComboBox::DsComboBox(QWidget *parent) 
    :QComboBox(parent)
{
    _bPopup = false;
    _popupWidth = -1;
    _popupMinWidth = 0;
    QComboBox::setSizeAdjustPolicy(QComboBox::AdjustToContents);   
}

DsComboBox::~DsComboBox()
{

}

void DsComboBox::measureSize()
{
    int num = this->count();
    int maxWidth = 0;
    int height = 30;
    QFontMetrics fm = this->fontMetrics();

    for (int i=0; i<num; i++){
        QString text = this->itemText(i);
        QRect rc = fm.boundingRect(text);
 
        if (rc.width() > maxWidth){
            maxWidth = rc.width();
        }
        height = rc.height();
    }

    QString style = QString("QAbstractItemView{min-width:%1px; min-height:%2px;}")
                .arg(maxWidth + 30)
                .arg(height + 5);
    this->setStyleSheet(style);
}

// With a list popup, Qt makes the popup as wide as the combo box. The vertical
// scroll bar and the style sheet's item padding then leave the items too
// little room, and they are elided. Give the view the width its items need
// instead: the popup container takes the view's minimum width into account,
// while the combo box itself keeps its size.
//
// Menu-like popups (SH_ComboBox_Popup, e.g. Fusion) are left to Qt, which
// widens them to the combo box's contents. Their item size hint is a menu
// item's, with a check column and minimum width the popup does not paint.
void DsComboBox::fitPopupWidth()
{
    QStyleOptionComboBox comboOpt;
    initStyleOption(&comboOpt);
    if (style()->styleHint(QStyle::SH_ComboBox_Popup, &comboOpt, this))
        return;

    QAbstractItemView *v = view();
    QAbstractItemDelegate *delegate = v->itemDelegate();
    if (delegate == nullptr)
        return;

    v->ensurePolished();

    // Set up the item option as the combo box's list view does, so that the
    // delegate sizes the items as they are painted.
    QStyleOptionViewItem opt;
    opt.initFrom(v);
    opt.state &= ~(QStyle::State_MouseOver | QStyle::State_HasFocus);
    opt.font = font();
    if (v->iconSize().isValid()){
        opt.decorationSize = v->iconSize();
    }
    else{
        int pm = v->style()->pixelMetric(QStyle::PM_ListViewIconSize, nullptr, v);
        opt.decorationSize = QSize(pm, pm);
    }
    opt.decorationPosition = QStyleOptionViewItem::Left;
    opt.decorationAlignment = Qt::AlignCenter;
    opt.displayAlignment = Qt::AlignLeft | Qt::AlignVCenter;
    opt.textElideMode = v->textElideMode();
    opt.rect = QRect();
    opt.showDecorationSelected = true;
    opt.viewItemPosition = QStyleOptionViewItem::OnlyOne;
    opt.locale = v->locale();
    opt.widget = v;

    // A hovered or selected item can have its own border (the themes frame
    // the hovered one), which takes room from the text. Take the widest.
    const QStyle::State states[] = {
        QStyle::State_None,
        QStyle::State_MouseOver,
        QStyle::State_Selected,
        QStyle::State_MouseOver | QStyle::State_Selected,
    };

    int itemWidth = 0;
    for (int row = 0; row < count(); row++){
        QModelIndex index = model()->index(row, modelColumn(), rootModelIndex());

        for (QStyle::State state : states){
            QStyleOptionViewItem itemOpt(opt);
            itemOpt.state |= state;
            itemWidth = qMax(itemWidth, delegate->sizeHint(itemOpt, index).width());
        }
    }

    // The view has no frame of its own, but the style sheet's border and
    // padding count as one.
    QMargins frame = v->contentsMargins();
    int width = itemWidth + frame.left() + frame.right();

    // The list scrolls once it has more rows than the popup shows.
    if (v->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff
        && count() > maxVisibleItems()){
        width += v->verticalScrollBar()->sizeHint().width();
    }

    // A minimum width other code set on the view stays a lower bound, and a
    // fixed width stays as it is.
    if (v->minimumWidth() != _popupWidth)
        _popupMinWidth = v->minimumWidth();
    _popupWidth = qBound(_popupMinWidth, width, v->maximumWidth());
    v->setMinimumWidth(_popupWidth);
}

void DsComboBox::showPopup()
{
    _bPopup = true;

#ifdef Q_OS_DARWIN

    measureSize();
    QComboBox::showPopup();

    QWidget *popup = this->findChild<QFrame*>();
    auto rc = popup->geometry();
    int x = rc.left() + 6;
    int y = rc.top();
    int w = rc.right() - rc.left();
    int h = rc.bottom() - rc.top() + 20;
    popup->setGeometry(x, y, w, h);

    int sy = QGuiApplication::primaryScreen()->size().height(); 
    if (sy <= 1080){
        popup->setMaximumHeight(750); 
    }

    popup->setStyleSheet(AppConfig::Instance().IsSystemStyle() ? QString()
        : "background-color:" + AppConfig::Instance().GetStyleColor().name());

#else
    if (count() > 0)
        fitPopupWidth();
    QComboBox::showPopup();
#endif

}

void DsComboBox::hidePopup()
{
    QComboBox::hidePopup();
    _bPopup = false;
}
 