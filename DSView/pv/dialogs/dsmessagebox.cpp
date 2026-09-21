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


#include "dsmessagebox.h"
#include "shadow.h"

#include <QObject>
#include <QEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QAbstractButton>
#include "../dsvdef.h"
#include "../ui/langresource.h"
#include "../config/appconfig.h"
#include "../ui/fn.h"
#include "../ui/popupdlglist.h"

namespace pv {
namespace dialogs {

DSMessageBox::DSMessageBox(QWidget *parent,const QString title) :
#ifdef Q_OS_LINUX
    QDialog(AppConfig::Instance().IsSystemStyle() ? parent : NULL)
#else
    QDialog(parent)
#endif
{
    (void)parent;
    _layout = NULL;
    _main_widget = NULL;
    _msg = NULL;
    _titlebar = NULL;
    _shadow = NULL;  
    _main_layout = NULL;

    _bClickYes = false;

    bool systemWindow = false;
#ifdef Q_OS_LINUX
    systemWindow = AppConfig::Instance().IsSystemStyle();
#endif
    if (systemWindow){
        setWindowFlags(Qt::Dialog);
    }
    else{
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);
        setAttribute(Qt::WA_TranslucentBackground);
    }

    _main_widget = new QWidget(this);
    _main_layout = new QVBoxLayout(_main_widget);
    _main_widget->setLayout(_main_layout);  

    _msg = new QMessageBox(_main_widget);
    // This message box supplies the contents of our dialog. Without this
    // option Qt can open a second, native dialog when no QSS is active.
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    _msg->setOption(QMessageBox::Option::DontUseNativeDialog);
#endif
    _msg->setWindowFlags(Qt::Widget);
    _layout = new QVBoxLayout(this);

    const QString windowTitle = title.isEmpty()
        ? L_S(STR_PAGE_MSG, S_ID(IDS_MSG_MESSAGE), "Message") : title;
    setWindowTitle(windowTitle);

    if (systemWindow){
        // QMessageBox already provides the style's content margins.
        _layout->setContentsMargins(0, 0, 0, 0);
        _main_layout->setContentsMargins(0, 0, 0, 0);
    }
    else{
        _shadow = new Shadow(this);
        _shadow->setBlurRadius(10.0);
        _shadow->setDistance(3.0);
        _shadow->setColor(QColor(0, 0, 0, 80));
        _main_widget->setAutoFillBackground(true);
        this->setGraphicsEffect(_shadow);

        _titlebar = new toolbars::TitleBar(false, this, NULL, false);
        _titlebar->setTitle(windowTitle);
        _main_layout->addWidget(_titlebar);
    }
    _main_layout->addWidget(_msg);   
    _layout->addWidget(_main_widget);

    setLayout(_layout); 

    connect(_msg, SIGNAL(buttonClicked(QAbstractButton*)), this, SLOT(on_button(QAbstractButton*)));
}


DSMessageBox::~DSMessageBox()
{
    DESTROY_QT_OBJECT(_layout);
    DESTROY_QT_OBJECT(_main_widget);
    DESTROY_QT_OBJECT(_msg);
    DESTROY_QT_OBJECT(_titlebar);
    DESTROY_QT_OBJECT(_shadow);
    DESTROY_QT_OBJECT(_main_layout);

    PopupDlgList::RemoveDlgFromList(this);
}

void DSMessageBox::accept()
{
    using namespace Qt;

    QDialog::accept();
}

void DSMessageBox::reject()
{
    using namespace Qt;

    QDialog::reject();
}
  
QMessageBox* DSMessageBox::mBox()
{
    return _msg;
}
  
void DSMessageBox::on_button(QAbstractButton *btn)
{
    QMessageBox::ButtonRole role = _msg->buttonRole(btn);

    if (role == QMessageBox::AcceptRole || role == QMessageBox::YesRole){
        _bClickYes = true;
         accept();
    } 
    else
        reject();
}

int DSMessageBox::exec()
{
    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    ui::set_form_font(this, font);

    if (_titlebar != NULL){
        _titlebar->update_font();
    }

    PopupDlgList::AddDlgTolist(this);

    return QDialog::exec();
}

} // namespace dialogs
} // namespace pv
