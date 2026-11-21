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

#ifndef MYSTYLE_H
#define MYSTYLE_H

#include <QProxyStyle>
#include <QStyleFactory>
#include <memory>

class MyStyle : public QProxyStyle
{
    Q_OBJECT
    public:
    explicit MyStyle(const QString &baseStyle) :
        QProxyStyle(baseStyle),
        _fusion(QStyleFactory::create("Fusion"))
    {}

    // DSView's style sheets draw spin boxes themselves, but their size still
    // comes from the base style. Desktop styles such as Breeze reserve room for
    // their own larger frame and buttons, which made spin boxes much taller than
    // the combo boxes next to them. Size them as Fusion does.
    QSize sizeFromContents(ContentsType type, const QStyleOption *option,
                           const QSize &size, const QWidget *widget) const override
    {
        if (type == CT_SpinBox && _fusion)
            return _fusion->sizeFromContents(type, option, size, widget);
        return QProxyStyle::sizeFromContents(type, option, size, widget);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption * option = 0, const QWidget * widget = 0 ) {
        int s = QProxyStyle::pixelMetric(metric, option, widget);
        if (metric == QStyle::PM_SmallIconSize) {
            s = 24;
        }
        return s;
    }

    private:
    std::unique_ptr<QStyle> _fusion;
};

#endif // MYSTYLE_H
