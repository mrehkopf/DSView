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

#ifndef DSVIEW_PV_VIEW_TIMEBAR_H
#define DSVIEW_PV_VIEW_TIMEBAR_H

#include <QWidget>
#include <QRectF>
#include <stdint.h>

namespace pv {
namespace view {

class View;

/**
 * Thin bar above the time ruler (logic mode) that stands for the whole
 * capture. It shows which part of the capture is in the window, the part
 * received so far, the trigger and the cursors, and moves the view:
 *
 *  - left click: centre the view there; drag: scroll through the capture
 *    (dragging the window box keeps the point that was grabbed);
 *  - right drag: zoom the view to the span dragged over;
 *  - double click: the whole capture, and back to the previous zoom;
 *  - wheel: zoom, around the pointer while it is over the window box.
 *
 * Hovering shows the time (or sample) under the pointer, counted from the
 * trigger as on the ruler.
 */
class TimeBar : public QWidget
{
    Q_OBJECT

public:
    static const int BarHeight;

    explicit TimeBar(View &parent);

private:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;

    // The capture length in seconds, false when there is nothing to show.
    bool capture_time(double &total);
    bool can_navigate();

    QRectF track_rect();
    double time_to_x(double t, double total);
    double x_to_time(double x, double total);

    // The window box: what the view shows, at least MinWindowWidth wide.
    QRectF window_rect(double total);

    // Scrolls the view, keeping the zoom, so it starts at time t.
    void move_view_to(double t);
    void zoom_to_span(double t0, double t1);

    QString time_text(double t, double total);
    void update_hover(int x);

private:
    View &_view;

    bool    _dragging;
    double  _grab_dt;       // time from the window's start to the grabbed point

    bool    _selecting;     // right drag in progress
    int     _sel_x0;
    int     _sel_x1;

    int     _hover_x;       // -1 when the mouse is not over the bar
};

} // namespace view
} // namespace pv

#endif // DSVIEW_PV_VIEW_TIMEBAR_H
