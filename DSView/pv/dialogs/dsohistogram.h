/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
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

#ifndef DSVIEW_PV_DSOHISTOGRAM_H
#define DSVIEW_PV_DSOHISTOGRAM_H

#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QLabel>
#include <QVector>
#include <QString>

#include "dsdialog.h"
#include "../ui/dscombobox.h"

namespace pv {

class SigSession;

namespace dialogs {

// A small bar-chart widget that draws a single histogram (bin counts) with an
// x-axis annotated by its min/max value and a title.
class HistogramPlot : public QWidget
{
    Q_OBJECT

public:
    HistogramPlot(QWidget *parent = NULL);

    // has_mean/mean_value optionally draw a dashed marker line at that x
    // position (e.g. the dataset's mean), which helps gauge how a distorted
    // distribution sits relative to its average at a glance.
    void set_data(const QVector<double> &bins, double x_min, double x_max,
                  const QString &x_unit, const QString &title,
                  bool has_mean = false, double mean_value = 0.0);
    void clear_data();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString fmt_value(double v) const;

private:
    QVector<double> _bins;
    double  _x_min;
    double  _x_max;
    QString _x_unit;
    QString _title;
    bool    _has_mean;
    double  _mean_value;
};

// Value + timing (jitter) histogram of a single DSO channel, with basic
// amplitude and jitter statistics. Computed once from the current snapshot.
class DsoHistogram : public DSDialog
{
    Q_OBJECT

public:
    DsoHistogram(SigSession *session, QWidget *parent);
    ~DsoHistogram();

private slots:
    void on_channel_changed(int index);

private:
    void compute();

private:
    SigSession *_session;

    DsComboBox      *_ch_combobox;
    DsComboBox      *_unit_combobox;
    HistogramPlot   *_value_plot;
    HistogramPlot   *_time_plot;
    QLabel          *_stats_label;
    QVBoxLayout     *_layout;
    QDialogButtonBox _button_box;
};

} // namespace dialogs
} // namespace pv

#endif // DSVIEW_PV_DSOHISTOGRAM_H
