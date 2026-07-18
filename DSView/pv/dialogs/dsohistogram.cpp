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

#include "dsohistogram.h"

#include <math.h>
#include <algorithm>
#include <vector>

#include <QPainter>
#include <QFormLayout>

#include "../sigsession.h"
#include "../view/dsosignal.h"
#include "../data/dsosnapshot.h"
#include "../ui/langresource.h"

using namespace std;

namespace pv {
namespace dialogs {

//------------------------------------------------------------------- HistogramPlot

HistogramPlot::HistogramPlot(QWidget *parent) :
    QWidget(parent),
    _x_min(0),
    _x_max(0)
{
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void HistogramPlot::set_data(const QVector<double> &bins, double x_min,
                             double x_max, const QString &x_unit,
                             const QString &title)
{
    _bins = bins;
    _x_min = x_min;
    _x_max = x_max;
    _x_unit = x_unit;
    _title = title;
    update();
}

void HistogramPlot::clear_data()
{
    _bins.clear();
    update();
}

void HistogramPlot::paintEvent(QPaintEvent *event)
{
    (void)event;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    QColor fore(palette().color(foregroundRole()));
    QColor bar = fore;
    bar.setAlpha(160);

    // Size the title and axis-label bands to the actual font so they are never
    // clipped (the UI font can be larger, e.g. in the German translation).
    const int text_h = p.fontMetrics().height();

    const int left = 4;
    const int right = width() - 4;
    const int top = text_h + 4;                 // room for the title
    const int bottom = height() - text_h - 4;   // room for the axis labels

    // Title.
    p.setPen(fore);
    p.drawText(QRect(left, 0, right - left, top),
               Qt::AlignLeft | Qt::AlignVCenter, _title);

    if (bottom - top < 4)
        return;

    // Plot frame.
    p.setPen(QPen(fore, 1));
    p.drawRect(QRect(left, top, right - left, bottom - top));

    if (_bins.isEmpty()) {
        p.drawText(QRect(left, top, right - left, bottom - top),
                   Qt::AlignCenter, "--");
        return;
    }

    double peak = 0;
    for (double v : _bins)
        peak = max(peak, v);
    if (peak <= 0)
        peak = 1;

    const int plot_w = right - left;
    const int plot_h = bottom - top;
    const int n = _bins.size();

    p.setPen(Qt::NoPen);
    p.setBrush(bar);
    for (int i = 0; i < n; i++) {
        const int x0 = left + (int)((double)i / n * plot_w);
        const int x1 = left + (int)((double)(i + 1) / n * plot_w);
        const int h = (int)(_bins[i] / peak * (plot_h - 1));
        if (h > 0)
            p.fillRect(QRect(x0, bottom - h, max(1, x1 - x0 - 1), h), bar);
    }

    // Axis min/max annotations, in the band below the frame.
    p.setPen(fore);
    p.drawText(QRect(left, bottom, plot_w, height() - bottom),
               Qt::AlignLeft | Qt::AlignVCenter,
               QString::number(_x_min, 'f', 2) + " " + _x_unit);
    p.drawText(QRect(left, bottom, plot_w, height() - bottom),
               Qt::AlignRight | Qt::AlignVCenter,
               QString::number(_x_max, 'f', 2) + " " + _x_unit);
}

//------------------------------------------------------------------- DsoHistogram

DsoHistogram::DsoHistogram(SigSession *session, QWidget *parent) :
    DSDialog(parent),
    _session(session),
    _button_box(QDialogButtonBox::Close, Qt::Horizontal, this)
{
    setMinimumSize(552, 460);

    _ch_combobox = new DsComboBox(this);
    for (auto s : _session->get_signals()) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *dsoSig = (view::DsoSignal*)s;
            _ch_combobox->addItem(dsoSig->get_name(),
                                  QVariant::fromValue(dsoSig->get_index()));
        }
    }

    _value_plot = new HistogramPlot(this);
    _time_plot = new HistogramPlot(this);

    _stats_label = new QLabel(this);
    _stats_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _stats_label->setWordWrap(true);
    _stats_label->setTextFormat(Qt::RichText);

    // Voltage unit selector (values are computed in mV internally).
    _unit_combobox = new DsComboBox(this);
    _unit_combobox->addItem("mV", QVariant::fromValue(1.0));
    _unit_combobox->addItem("V",  QVariant::fromValue(0.001));
    // DsComboBox forces AdjustToContents, which makes setMinimumWidth() a
    // no-op; size it by a minimum contents length instead so the item text
    // (plus the drop-down arrow) is not cropped.
    _unit_combobox->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _unit_combobox->setMinimumContentsLength(5);

    QLabel *ch_label = new QLabel(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel"), this);
    QLabel *unit_label = new QLabel(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_UNIT), "Unit"), this);
    QHBoxLayout *ch_layout = new QHBoxLayout();
    ch_layout->addWidget(ch_label);
    ch_layout->addWidget(_ch_combobox);
    ch_layout->addStretch(1);
    ch_layout->addWidget(unit_label);
    ch_layout->addWidget(_unit_combobox);

    _layout = new QVBoxLayout();
    _layout->addLayout(ch_layout);
    _layout->addWidget(_value_plot, 1);
    _layout->addWidget(_time_plot, 1);
    _layout->addWidget(_stats_label);
    _layout->addWidget(&_button_box);

    layout()->addLayout(_layout);
    setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_HISTOGRAM), "Histogram / Jitter"));

    connect(&_button_box, SIGNAL(rejected()), this, SLOT(reject()));
    connect(&_button_box, SIGNAL(accepted()), this, SLOT(accept()));
    connect(_ch_combobox, SIGNAL(currentIndexChanged(int)),
            this, SLOT(on_channel_changed(int)));
    connect(_unit_combobox, SIGNAL(currentIndexChanged(int)),
            this, SLOT(on_channel_changed(int)));

    compute();
}

DsoHistogram::~DsoHistogram()
{
}

void DsoHistogram::on_channel_changed(int index)
{
    (void)index;
    compute();
}

// Format a time interval given in nanoseconds.
static QString fmt_time_ns(double t_ns)
{
    const double a = fabs(t_ns);
    if (a >= 1e9)
        return QString::number(t_ns / 1e9, 'f', 3) + " s";
    if (a >= 1e6)
        return QString::number(t_ns / 1e6, 'f', 3) + " ms";
    if (a >= 1e3)
        return QString::number(t_ns / 1e3, 'f', 3) + " us";
    return QString::number(t_ns, 'f', 2) + " ns";
}

static QString fmt_freq_hz(double f_hz)
{
    const double a = fabs(f_hz);
    if (a >= 1e6)
        return QString::number(f_hz / 1e6, 'f', 3) + " MHz";
    if (a >= 1e3)
        return QString::number(f_hz / 1e3, 'f', 3) + " kHz";
    return QString::number(f_hz, 'f', 2) + " Hz";
}

void DsoHistogram::compute()
{
    _value_plot->clear_data();
    _time_plot->clear_data();

    int index = -1;
    if (_ch_combobox->count() > 0)
        index = _ch_combobox->itemData(_ch_combobox->currentIndex()).toInt();

    view::DsoSignal *dsoSig = NULL;
    for (auto s : _session->get_signals()) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *d = (view::DsoSignal*)s;
            if (d->get_index() == index) {
                dsoSig = d;
                break;
            }
        }
    }

    if (dsoSig == NULL || !dsoSig->enabled()) {
        _stats_label->setText(
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_HIST_NO_CHANNEL),
                "No enabled DSO channel selected."));
        return;
    }

    data::DsoSnapshot *data = dsoSig->data();
    if (data == NULL || data->empty()) {
        _stats_label->setText(
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_HIST_NO_DATA),
                "No captured data. Run an acquisition first."));
        return;
    }

    const uint64_t total = data->get_sample_count();
    const uint8_t *buf = data->get_samples(0, 0, dsoSig->get_index());
    if (total < 2 || buf == NULL) {
        _stats_label->setText(
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_HIST_NO_DATA),
                "No captured data. Run an acquisition first."));
        return;
    }

    // Raw-sample -> millivolt conversion, mirroring DsoSignal::get_voltage().
    const int hw_offset = dsoSig->get_hw_offset();
    const double k = data->get_measure_voltage_factor(dsoSig->get_index());
    const double data_scale = data->get_data_scale(dsoSig->get_index());
    const double vfactor = dsoSig->get_vDial()->get_factor();
    const int vrect_h = dsoSig->get_view_rect().height();
    const double vscale = (vrect_h > 0)
        ? data_scale * k * vfactor * DS_CONF_DSO_VDIVS / vrect_h : 0.0;

    // Keep the dialog responsive on very deep captures.
    const uint64_t MaxSamples = 8000000;
    const uint64_t n = min<uint64_t>(total, MaxSamples);

    auto volt = [&](uint64_t i) -> double {
        return (hw_offset - (double)buf[i]) * vscale;
    };

    // --- value (amplitude) histogram + stats ---
    double vmin = 1e300, vmax = -1e300, vsum = 0;
    for (uint64_t i = 0; i < n; i++) {
        const double v = volt(i);
        vmin = min(vmin, v);
        vmax = max(vmax, v);
        vsum += v;
    }
    const double vmean = vsum / n;
    const double vspan = (vmax > vmin) ? (vmax - vmin) : 1.0;

    // Selected display unit (values are computed in mV): factor + precision.
    const double vfac = _unit_combobox->itemData(
        _unit_combobox->currentIndex()).toDouble();
    const QString vunit = _unit_combobox->currentText();
    const int vprec = (vfac < 1.0) ? 4 : 2;   // more decimals when showing V

    const int NBINS = 128;
    QVector<double> vbins(NBINS, 0.0);
    for (uint64_t i = 0; i < n; i++) {
        int b = (int)((volt(i) - vmin) / vspan * (NBINS - 1));
        b = max(0, min(NBINS - 1, b));
        vbins[b] += 1.0;
    }
    _value_plot->set_data(vbins, vmin * vfac, vmax * vfac, vunit,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_HIST_VALUE), "Value distribution"));

    // --- timing (jitter) histogram + stats ---
    // Use the analysed snapshot's own sample rate (this is what the waveform
    // painter uses); cur_snap_samplerate() reads the capture buffer, which can
    // read back as 0 for the view buffer and silently disable jitter analysis.
    double samplerate = data->samplerate();
    if (samplerate <= 0)
        samplerate = _session->cur_snap_samplerate();
    const double dt_ns = (samplerate > 0) ? 1e9 / samplerate : 0.0;

    // Detect edges directly on the raw ADC samples (0..255). This keeps the
    // jitter measurement independent of the voltage scaling, which can read
    // back as zero in some states and would otherwise flatten the signal.
    int raw_min = 255, raw_max = 0;
    for (uint64_t i = 0; i < n; i++) {
        const int r = buf[i];
        raw_min = min(raw_min, r);
        raw_max = max(raw_max, r);
    }
    const double raw_mid = (raw_min + raw_max) / 2.0;
    const double raw_hyst = (raw_max - raw_min) * 0.05;

    // Schmitt-trigger edge detection: track the high/low state and flip it on
    // the upper/lower hysteresis thresholds. A rising edge is recorded each
    // time the state goes low->high. This handles finite rise time and noise
    // (unlike a single-step "crossed mid this sample" test).
    std::vector<uint64_t> edges;   // rising-edge sample indices
    if (raw_max > raw_min) {
        const double hi = raw_mid + raw_hyst;
        const double lo = raw_mid - raw_hyst;
        bool is_high = (buf[0] >= raw_mid);
        for (uint64_t i = 1; i < n; i++) {
            const int r = buf[i];
            if (!is_high && r >= hi) {
                is_high = true;
                edges.push_back(i);
            } else if (is_high && r <= lo) {
                is_high = false;
            }
        }
    }

    // Build the statistics as a two-column table (metric | value) so the
    // amplitude and timing figures line up and read cleanly.
    auto row = [](const QString &k, const QString &v) {
        return QString("<tr><td>%1</td>"
                       "<td align='right'>%2</td></tr>").arg(k).arg(v);
    };
    auto sep = []() {
        return QString("<tr><td colspan='2'><hr/></td></tr>");
    };

    auto vstr = [&](double mv_value) {
        return QString::number(mv_value * vfac, 'f', vprec) + " " + vunit;
    };

    QString stats = "<table width='100%' cellspacing='4'>";
    stats += row(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel"),
                 "CH" + dsoSig->get_name());
    stats += row("Samples", QString::number(n));
    stats += sep();
    stats += row("Vmax", vstr(vmax));
    stats += row("Vmin", vstr(vmin));
    stats += row("Vpp",  vstr(vmax - vmin));
    stats += row("Vmean", vstr(vmean));

    if (edges.size() >= 3 && dt_ns > 0) {
        std::vector<double> periods;   // ns
        periods.reserve(edges.size() - 1);
        for (size_t e = 1; e < edges.size(); e++)
            periods.push_back((edges[e] - edges[e - 1]) * dt_ns);

        double pmin = 1e300, pmax = -1e300, psum = 0;
        for (double p : periods) {
            pmin = min(pmin, p);
            pmax = max(pmax, p);
            psum += p;
        }
        const double pmean = psum / periods.size();
        double var = 0;
        for (double p : periods)
            var += (p - pmean) * (p - pmean);
        const double pstd = sqrt(var / periods.size());   // RMS jitter
        const double ppk = pmax - pmin;                     // pk-pk jitter

        const int TBINS = 128;
        QVector<double> tbins(TBINS, 0.0);
        const double pspan = (pmax > pmin) ? (pmax - pmin) : 1.0;
        for (double p : periods) {
            int b = (int)((p - pmin) / pspan * (TBINS - 1));
            b = max(0, min(TBINS - 1, b));
            tbins[b] += 1.0;
        }
        _time_plot->set_data(tbins, pmin, pmax, "ns",
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_HIST_PERIOD),
                "Period distribution (jitter)"));

        stats += sep();
        stats += row("Edges", QString::number((qulonglong)edges.size()));
        stats += row("Mean period", fmt_time_ns(pmean));
        stats += row("Frequency", fmt_freq_hz(1e9 / pmean));
        stats += row("RMS jitter", fmt_time_ns(pstd));
        stats += row("Pk-pk jitter", fmt_time_ns(ppk));
    } else {
        stats += sep();
        stats += QString("<tr><td colspan='2'>%1</td></tr>").arg(
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_HIST_NO_JITTER),
                "Not enough edges for jitter analysis."));
        stats += row("Edges found", QString::number((qulonglong)edges.size()));
    }

    stats += "</table>";
    _stats_label->setText(stats);
}

} // namespace dialogs
} // namespace pv
