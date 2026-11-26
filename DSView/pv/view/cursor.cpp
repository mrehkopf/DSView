/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include "cursor.h"

#include "ruler.h"
#include "view.h"

#include <QBrush>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <assert.h>
#include <stdio.h>
#include "../dsvdef.h"
#include "ruler.h"

namespace pv {
namespace view {

const QColor Cursor::LineColour(32, 74, 135);
const QColor Cursor::FillColour(52, 101, 164);
const QColor Cursor::HighlightColour(83, 130, 186);
const QColor Cursor::TextColour(Qt::white);
const int Cursor::Offset = 1;
const int Cursor::ArrowSize = 10;
const int Cursor::CloseSize = 10;

Cursor::Cursor(View &view, int order, uint64_t sampleIndex) :
    TimeMarker(view, sampleIndex)
{
   _order = order;
   _number_width = 0;
   _compact = false;
}

// The time from the trigger, as on the ruler.
QString Cursor::label_text(bool show_samples)
{
    using pv::view::Ruler;

    const int64_t rel_index = (int64_t)_index - (int64_t)_view.session().get_trigger_pos();
    return show_samples
        ? Ruler::format_samples(rel_index)
        : Ruler::format_real_time(rel_index, _view.session().cur_snap_samplerate());
}

void Cursor::prepare_label(QPainter &p, bool show_samples)
{
    compute_text_size(p, label_text(show_samples));
}

QRect Cursor::get_label_rect(const QRect &rect, bool &visible, bool has_hoff)
{
    const double samples_per_pixel = _view.session().cur_snap_samplerate() * _view.scale();
    const double cur_offset = _index / samples_per_pixel;
    if (cur_offset < _view.x_offset() ||
        cur_offset > (_view.x_offset() + _view.width())) {
        visible = false;
        return QRect(-1, -1, 0, 0);
    }
    const int64_t x = _view.index2pixel(_index, has_hoff);

    const int text_width = _compact ? _number_width : _text_size.width();
    const QSize label_size(
		text_width + View::LabelPadding.width() * 2,
		_text_size.height() + View::LabelPadding.height() * 2);
    const int top = rect.height() - label_size.height() -
		Cursor::Offset - Cursor::ArrowSize - 0.5f;
    const int height = label_size.height();

    visible = true;
    return QRect(x - label_size.width() / 2, top, label_size.width(), height);
}

QRect Cursor::get_close_rect(const QRect &rect)
{
    // A compact label has no room for the close box.
    if (_compact)
        return QRect();
    return QRect(rect.right() - CloseSize, rect.top(), CloseSize, CloseSize);
}

void Cursor::paint_label(QPainter &p, const QRect &rect,
            unsigned int prefix, bool has_hoff, bool show_samples)
{
    using pv::view::Ruler;
    bool visible;

    (void)prefix;
    // Sized for the text drawn, the time from the trigger.
    const QString text = label_text(show_samples);

    compute_text_size(p, text);
    const QRect r(get_label_rect(rect, visible, has_hoff));
    if (!visible)
        return;

    if (_compact) {
        // Only the number, in the cursor's colour.
        p.setPen(Qt::transparent);
        p.setBrush(Ruler::GetColorByCursorOrder(_order));
        p.drawRect(r);
        const QPoint points[] = {
            QPoint(r.left() + r.width() / 2 - ArrowSize / 2, r.bottom()),
            QPoint(r.left() + r.width() / 2 + ArrowSize / 2, r.bottom()),
            QPoint(r.left() + r.width() / 2, rect.bottom()),
        };
        p.drawPolygon(points, countof(points));
        p.setPen(Qt::black);
        p.drawText(r, Qt::AlignCenter | Qt::AlignVCenter, QString::number(_order));
        return;
    }

    const QRect close(get_close_rect(r));

    p.setPen(Qt::transparent);

    if (close.contains(QPoint(_view.hover_point().x(), _view.hover_point().y())))
        p.setBrush(Ruler::GetColorByCursorOrder(_order));
    else if (r.contains(QPoint(_view.hover_point().x(), _view.hover_point().y())))
        p.setBrush(View::Orange);
    else
        p.setBrush(Ruler::GetColorByCursorOrder(_order));

    p.drawRect(r);

    const QPoint points[] = {
        QPoint(r.left() + r.width() / 2 - ArrowSize, r.bottom()),
        QPoint(r.left() + r.width() / 2 + ArrowSize, r.bottom()),
        QPoint(r.left() + r.width() / 2, rect.bottom()),
    };
    p.drawPolygon(points, countof(points));

    if (close.contains(QPoint(_view.hover_point().x(), _view.hover_point().y())))
        p.setBrush(View::Red);
    else
        p.setBrush(View::Orange);
    p.drawRect(close);
    p.setPen(Qt::black);
    p.drawLine(close.left() + 2, close.top() + 2, close.right() - 2, close.bottom() - 2);
    p.drawLine(close.left() + 2, close.bottom() - 2, close.right() - 2, close.top() + 2);

	p.drawText(r, Qt::AlignCenter | Qt::AlignVCenter, text);

    const QRect arrowRect = QRect(r.bottomLeft().x(), r.bottomLeft().y(), r.width(), ArrowSize);
    p.drawText(arrowRect, Qt::AlignCenter | Qt::AlignVCenter, QString::number(_order));
}

void Cursor::paint_fix_label(QPainter &p, const QRect &rect,
    unsigned int prefix, QChar label, QColor color, bool has_hoff, bool show_samples,
    int labels_bottom)
{
    using pv::view::Ruler;
    bool visible;

    (void)prefix;
    const QString text = show_samples
        ? Ruler::format_samples(_index)
        : Ruler::format_real_time(_index, _view.session().cur_snap_samplerate());

    compute_text_size(p, text);
    QRect r(get_label_rect(rect, visible, has_hoff));
    if (!visible)
        return;

    // Without a time in the box, only a thin band of it is needed above the
    // arrow. Start it below the time labels, keeping at least 3 pixel rows.
    if (!has_hoff && labels_bottom >= 0)
        r.setTop(qMin(qMax(r.top(), labels_bottom + 2), r.bottom() - 2));

    p.setPen(Qt::transparent);
    p.setBrush(color);
    p.drawRect(r);

    const QPoint points[] = {
        QPoint(r.left() + r.width() / 2 - ArrowSize, r.bottom()),
        QPoint(r.left() + r.width() / 2 + ArrowSize, r.bottom()),
        QPoint(r.left() + r.width() / 2, rect.bottom()),
    };
    p.drawPolygon(points, countof(points));

    p.setPen(Qt::white);
    if (has_hoff)
        p.drawText(r, Qt::AlignCenter | Qt::AlignVCenter, text);

    const QRect arrowRect = QRect(r.bottomLeft().x(), r.bottomLeft().y(), r.width(), ArrowSize);
    p.drawText(arrowRect, Qt::AlignCenter | Qt::AlignVCenter, label);
}

void Cursor::compute_text_size(QPainter &p, const QString &text)
{
    _text_size = p.boundingRect(QRect(), 0, text).size();
    _number_width = p.boundingRect(QRect(), 0, QString::number(_order)).width();
}
 
} // namespace view
} // namespace pv
