/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
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

#include "timebar.h"

#include <algorithm>
#include <cmath>

#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOption>
#include <QToolTip>
#include <QWheelEvent>

#include "cursor.h"
#include "ruler.h"
#include "view.h"
#include "../sigsession.h"
#include "../deviceagent.h"
#include "../config/appconfig.h"

using namespace std;

namespace pv {
namespace view {

const int TimeBar::BarHeight = 14;

// The track (the whole capture) leaves this much room above and below it,
// for the window box and the trigger flag, which use the full height.
static const int TrackInset = 3;
static const int SideMargin = 1;
// Narrower windows are drawn this wide, so they stay visible and grabbable.
static const double MinWindowWidth = 6.0;
// A right drag up to this many pixels is a click, not a zoom (as in the view).
static const int ClickMaxDrag = 2;

TimeBar::TimeBar(View &parent) :
    QWidget(&parent),
    _view(parent),
    _dragging(false),
    _grab_dt(0),
    _selecting(false),
    _sel_x0(0),
    _sel_x1(0),
    _hover_x(-1)
{
    setMouseTracking(true);
}

bool TimeBar::capture_time(double &total)
{
    SigSession &session = _view.session();

    if (session.get_device()->have_instance() == false)
        return false;

    const uint64_t rate = session.cur_snap_samplerate();
    const uint64_t limit = session.cur_samplelimits();

    if (rate == 0 || limit == 0 || _view.scale() <= 0)
        return false;

    total = limit * 1.0 / rate;
    return true;
}

bool TimeBar::can_navigate()
{
    double total;
    return capture_time(total)
        && _view.session().get_device()->get_work_mode() == LOGIC
        && _view.get_view_width() > 1;
}

QRectF TimeBar::track_rect()
{
    return QRectF(SideMargin, TrackInset,
                  max(width() - 2 * SideMargin, 1),
                  max(height() - 2 * TrackInset, 1));
}

double TimeBar::time_to_x(double t, double total)
{
    const QRectF r = track_rect();
    return r.left() + t / total * r.width();
}

double TimeBar::x_to_time(double x, double total)
{
    const QRectF r = track_rect();
    const double t = (x - r.left()) / r.width() * total;
    return max(0.0, min(t, total));
}

QRectF TimeBar::window_rect(double total)
{
    const QRectF r = track_rect();
    const double t0 = _view.x_offset() * _view.scale();
    const double t1 = t0 + _view.get_view_width() * _view.scale();

    double x0 = time_to_x(max(t0, 0.0), total);
    double x1 = time_to_x(min(t1, total), total);

    if (x1 - x0 < MinWindowWidth) {
        const double centre = (x0 + x1) / 2;
        x0 = centre - MinWindowWidth / 2;
        x1 = centre + MinWindowWidth / 2;
    }
    if (x0 < r.left()) {
        x1 += r.left() - x0;
        x0 = r.left();
    }
    if (x1 > r.right()) {
        x0 -= x1 - r.right();
        x1 = r.right();
    }

    return QRectF(x0, 0, x1 - x0, height());
}

void TimeBar::move_view_to(double t)
{
    const int64_t offset = (int64_t)llround(t / _view.scale());

    // set_scale_offset() clamps to the capture. Calling it without a change
    // would still overwrite the previous zoom, which a double click returns to.
    if (offset != _view.x_offset())
        _view.set_scale_offset(_view.scale(), offset);
}

void TimeBar::zoom_to_span(double t0, double t1)
{
    if (t1 < t0)
        swap(t0, t1);

    const int width = _view.get_view_width();
    double scale = (t1 - t0) / width;
    scale = max(min(scale, _view.get_maxscale()), _view.get_minscale());

    // Centred, so a span shorter than the deepest zoom stays in the middle.
    const int64_t offset = (int64_t)llround((t0 + t1) / 2 / scale - width / 2.0);
    _view.set_scale_offset(scale, offset);
}

QString TimeBar::time_text(double t, double total)
{
    SigSession &session = _view.session();
    const uint64_t rate = session.cur_snap_samplerate();
    const int64_t trigger = (int64_t)session.get_trigger_pos();

    // Counted from the trigger, as the ruler counts.
    if (AppConfig::Instance().appOptions.rulerTimeUnits == RULER_UNIT_SAMPLES)
        return Ruler::format_samples((int64_t)llround(t * rate) - trigger);

    // The unit follows the capture length; the decimals, the time one pixel
    // of the bar stands for.
    const int order = (int)floor(log10(total));
    const int prefix = max(0, min(8, (int)floor((order + 15) / 3.0)));
    const double unit = pow(10.0, prefix * 3 - 15);
    const double resolution = total / track_rect().width() / unit;
    const int precision = max(0, min(6, (int)ceil(-log10(resolution))));

    return Ruler::format_time(t - trigger * 1.0 / rate, prefix, precision);
}

void TimeBar::update_hover(int x)
{
    _hover_x = x;

    double total;
    if (!can_navigate() || !capture_time(total)) {
        unsetCursor();
        QToolTip::hideText();
        update();
        return;
    }

    const QRectF win = window_rect(total);
    if (x >= win.left() && x <= win.right())
        setCursor(Qt::OpenHandCursor);
    else
        setCursor(Qt::PointingHandCursor);

    QToolTip::showText(QCursor::pos(), time_text(x_to_time(x, total), total), this, rect());
    update();
}

void TimeBar::paintEvent(QPaintEvent*)
{
    QStyleOption o;
    o.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &o, &p, this);

    double total;
    if (!capture_time(total))
        return;

    SigSession &session = _view.session();
    const uint64_t rate = session.cur_snap_samplerate();
    const QRectF track = track_rect();
    const QColor fore = palette().color(foregroundRole());

    // The whole capture, and the part of it received so far: less than all
    // of it while a capture runs, or when it was stopped early.
    QColor c = fore;
    c.setAlpha(30);
    p.fillRect(track, c);

    if (session.have_view_data()) {
        const double received = min(session.get_ring_sample_count() * 1.0 / rate, total);
        if (received > 0) {
            c.setAlpha(80);
            p.fillRect(QRectF(track.left(), track.top(),
                              time_to_x(received, total) - track.left(), track.height()), c);
        }
    }

    p.setRenderHint(QPainter::Antialiasing, true);

    // The part the view shows. The outline sits on whole pixels.
    const QRectF win = window_rect(total);
    const QRectF box(floor(win.left()) + 0.5, 0.5,
                     max(ceil(win.right()) - floor(win.left()) - 1.0, 1.0), height() - 1.0);
    QColor winFill = View::Blue;
    winFill.setAlpha(90);
    p.setPen(QPen(View::Blue, 1));
    p.setBrush(winFill);
    p.drawRect(box);

    // Cursors, in their colours, with a dark edge so a blue cursor still
    // shows on the window box.
    QColor halo = palette().color(backgroundRole());
    halo.setAlpha(200);
    for (auto cursor : _view.get_cursorList()) {
        const double x = time_to_x(cursor->index() * 1.0 / rate, total);
        QColor cc = cursor->get_color();
        cc.setAlpha(255);
        p.setPen(QPen(halo, 4, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
        p.setPen(QPen(cc, 2, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
    }

    // The trigger: a red line with a flag on top, as on the ruler.
    if (_view.trig_cursor_shown()) {
        const double x = time_to_x(session.get_trigger_pos() * 1.0 / rate, total);
        p.setPen(QPen(View::Red, 1.5, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(x, 0), QPointF(x, height()));

        const QPointF flag[3] = {
            QPointF(x - 3.5, 0), QPointF(x + 3.5, 0), QPointF(x, 4.5)
        };
        p.setPen(Qt::NoPen);
        p.setBrush(View::Red);
        p.drawPolygon(flag, 3);
    }

    // The span a right drag will zoom to.
    if (_selecting) {
        QColor sel = View::Orange;
        sel.setAlpha(120);
        p.fillRect(QRectF(min(_sel_x0, _sel_x1), 0, abs(_sel_x1 - _sel_x0) + 1, height()), sel);
    }
    // Where a click would go.
    else if (_hover_x >= 0 && !_dragging) {
        QColor hover = fore;
        hover.setAlpha(200);
        p.setPen(QPen(hover, 1));
        p.drawLine(QPointF(_hover_x + 0.5, 0), QPointF(_hover_x + 0.5, height()));
    }
}

void TimeBar::mousePressEvent(QMouseEvent *event)
{
    double total;
    if (!can_navigate() || !capture_time(total))
        return;

    const int x = event->pos().x();

    if (event->button() == Qt::LeftButton) {
        const double t = x_to_time(x, total);
        const double t0 = _view.x_offset() * _view.scale();
        const double span = _view.get_view_width() * _view.scale();
        const QRectF win = window_rect(total);

        // On the window box: drag it from where it was grabbed. Elsewhere:
        // jump there, then drag on from the middle of the window.
        if (x >= win.left() && x <= win.right()) {
            _grab_dt = t - t0;
        }
        else {
            _grab_dt = span / 2;
            move_view_to(t - _grab_dt);
        }

        _dragging = true;
        setCursor(Qt::ClosedHandCursor);
        QToolTip::hideText();
        update();
    }
    else if (event->button() == Qt::RightButton) {
        _selecting = true;
        _sel_x0 = _sel_x1 = x;
        QToolTip::hideText();
        update();
    }
}

void TimeBar::mouseMoveEvent(QMouseEvent *event)
{
    const int x = event->pos().x();

    if (_dragging) {
        double total;
        _hover_x = x;
        if (capture_time(total))
            move_view_to(x_to_time(x, total) - _grab_dt);
    }
    else if (_selecting) {
        _sel_x1 = max(0, min(width() - 1, x));
        update();
    }
    else {
        update_hover(x);
    }
}

void TimeBar::mouseReleaseEvent(QMouseEvent *event)
{
    const int x = event->pos().x();

    if (event->button() == Qt::LeftButton && _dragging) {
        _dragging = false;
        update_hover(x);
    }
    else if (event->button() == Qt::RightButton && _selecting) {
        _selecting = false;

        double total;
        if (abs(_sel_x1 - _sel_x0) > ClickMaxDrag && can_navigate() && capture_time(total))
            zoom_to_span(x_to_time(_sel_x0, total), x_to_time(_sel_x1, total));

        update_hover(x);
    }
}

void TimeBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Not passed on as a press: the first click of the two did that already.
    double total;
    if (!can_navigate() || !capture_time(total))
        return;
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton)
        return;

    if (_view.scale() < _view.get_maxscale()) {
        // The whole capture.
        _view.set_scale_offset(_view.get_maxscale(), _view.get_min_offset());
    }
    else {
        // Back to the zoom from before, around the point double-clicked.
        const double scale = _view.get_prescale();
        if (scale < _view.get_maxscale()) {
            const double t = x_to_time(event->pos().x(), total);
            _view.set_scale_offset(scale, (int64_t)llround(t / scale - _view.get_view_width() / 2.0));
        }
    }

    update_hover(event->pos().x());
}

void TimeBar::wheelEvent(QWheelEvent *event)
{
    double total;
    if (!can_navigate() || !capture_time(total))
        return;

    int x = 0;
    int delta = 0;
    bool isVertical = true;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    x = (int)event->position().x();
    const int anglex = event->angleDelta().x();
    const int angley = event->angleDelta().y();

    if (anglex == 0 || abs(angley) >= abs(anglex)) {
        delta = angley;
    }
    else {
        delta = anglex;
        isVertical = false;
    }
#else
    x = event->x();
    delta = event->delta();
    isVertical = event->orientation() == Qt::Vertical;
#endif

    if (isVertical) {
        double steps = delta / 120.0;
        if (event->inverted())
            steps = -steps;

        // Around the pointer while it is over the window box, which then
        // stays under it; elsewhere around the middle of the view.
        const int width = _view.get_view_width();
        int centre = width / 2;
        const QRectF win = window_rect(total);

        if (x >= win.left() && x <= win.right()) {
            const double px = x_to_time(x, total) / _view.scale() - _view.x_offset();
            centre = (int)max(0.0, min((double)width, round(px)));
        }
        _view.zoom(steps, centre);
    }
    else {
        _view.set_scale_offset(_view.scale(), _view.x_offset() - delta);
    }

    update_hover(x);
    event->accept();
}

void TimeBar::leaveEvent(QEvent*)
{
    _hover_x = -1;
    QToolTip::hideText();
    update();
}

} // namespace view
} // namespace pv
