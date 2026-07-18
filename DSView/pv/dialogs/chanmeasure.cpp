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

#include "chanmeasure.h"

#include <math.h>
#include <algorithm>
#include <vector>

#include <QFormLayout>
#include <QHBoxLayout>

#include "../sigsession.h"
#include "../view/dsosignal.h"
#include "../data/dsosnapshot.h"
#include "../ui/langresource.h"

using namespace std;

namespace pv {
namespace dialogs {

namespace {

// Threshold-crossing edges of one DSO channel, in nanoseconds, plus the mean
// period. Uses a mid-level threshold with 5% hysteresis to reject noise.
struct EdgeInfo
{
    bool                valid = false;
    std::vector<double> rising;   // rising-edge times (ns)
    std::vector<double> falling;  // falling-edge times (ns)
    double              period = 0.0;  // ns (mean rising-to-rising)
};

EdgeInfo extract_edges(view::DsoSignal *sig, double dt_ns)
{
    EdgeInfo info;

    if (sig == NULL || !sig->enabled() || dt_ns <= 0)
        return info;

    data::DsoSnapshot *data = sig->data();
    if (data == NULL || data->empty())
        return info;

    const uint64_t total = data->get_sample_count();
    const uint8_t *buf = data->get_samples(0, 0, sig->get_index());
    if (total < 2 || buf == NULL)
        return info;

    const int hw_offset = sig->get_hw_offset();
    const double k = data->get_measure_voltage_factor(sig->get_index());
    const double data_scale = data->get_data_scale(sig->get_index());
    const double vfactor = sig->get_vDial()->get_factor();
    const int vrect_h = sig->get_view_rect().height();
    const double vscale = (vrect_h > 0)
        ? data_scale * k * vfactor * DS_CONF_DSO_VDIVS / vrect_h : 0.0;

    const uint64_t MaxSamples = 8000000;
    const uint64_t n = min<uint64_t>(total, MaxSamples);

    auto volt = [&](uint64_t i) -> double {
        return (hw_offset - (double)buf[i]) * vscale;
    };

    double vmin = 1e300, vmax = -1e300;
    for (uint64_t i = 0; i < n; i++) {
        const double v = volt(i);
        vmin = min(vmin, v);
        vmax = max(vmax, v);
    }
    const double span = (vmax > vmin) ? (vmax - vmin) : 1.0;
    const double mid = (vmax + vmin) / 2.0;
    const double hyst = span * 0.05;
    const double hi = mid + hyst;
    const double lo = mid - hyst;

    // Schmitt-trigger edge detection: flip the high/low state on the upper /
    // lower thresholds. This tolerates finite rise time and noise, which a
    // single-step "crossed mid this sample" test does not.
    bool is_high = (volt(0) >= mid);
    for (uint64_t i = 1; i < n; i++) {
        const double v = volt(i);
        if (!is_high && v >= hi) {
            is_high = true;
            info.rising.push_back(i * dt_ns);
        } else if (is_high && v <= lo) {
            is_high = false;
            info.falling.push_back(i * dt_ns);
        }
    }

    if (info.rising.size() >= 2) {
        double sum = 0;
        for (size_t e = 1; e < info.rising.size(); e++)
            sum += info.rising[e] - info.rising[e - 1];
        info.period = sum / (info.rising.size() - 1);
    }

    info.valid = true;
    return info;
}

// Mean signed delay (ns) from A's edges to B's edges, wrapped into
// (-period/2, +period/2] so it reads as the fractional shift within a cycle.
// Returns false if it cannot be computed.
bool mean_delay(const std::vector<double> &ea, const std::vector<double> &eb,
                double period, double &out_ns)
{
    if (ea.empty() || eb.empty() || period <= 0)
        return false;

    double sum = 0;
    int cnt = 0;
    for (double ta : ea) {
        // Nearest B edge to ta.
        auto it = std::lower_bound(eb.begin(), eb.end(), ta);
        double best = 0;
        bool have = false;
        if (it != eb.end()) {
            best = *it - ta;
            have = true;
        }
        if (it != eb.begin()) {
            const double d = *(it - 1) - ta;
            if (!have || fabs(d) < fabs(best)) {
                best = d;
                have = true;
            }
        }
        if (!have)
            continue;

        // Wrap into (-period/2, +period/2].
        while (best > period / 2)  best -= period;
        while (best <= -period / 2) best += period;

        sum += best;
        cnt++;
    }

    if (cnt == 0)
        return false;

    out_ns = sum / cnt;
    return true;
}

QString fmt_time_ns(double t_ns)
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

QString fmt_freq_hz(double f_hz)
{
    const double a = fabs(f_hz);
    if (a >= 1e6)
        return QString::number(f_hz / 1e6, 'f', 3) + " MHz";
    if (a >= 1e3)
        return QString::number(f_hz / 1e3, 'f', 3) + " kHz";
    return QString::number(f_hz, 'f', 2) + " Hz";
}

} // namespace

DsoChannelMeasure::DsoChannelMeasure(SigSession *session, QWidget *parent) :
    DSDialog(parent),
    _session(session),
    _button_box(QDialogButtonBox::Close, Qt::Horizontal, this)
{
    setMinimumSize(380, 260);

    _srcA_combobox = new DsComboBox(this);
    _srcB_combobox = new DsComboBox(this);
    for (auto s : _session->get_signals()) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *dsoSig = (view::DsoSignal*)s;
            _srcA_combobox->addItem(dsoSig->get_name(),
                                    QVariant::fromValue(dsoSig->get_index()));
            _srcB_combobox->addItem(dsoSig->get_name(),
                                    QVariant::fromValue(dsoSig->get_index()));
        }
    }
    // Default the 2nd source to a different channel when possible.
    if (_srcB_combobox->count() > 1)
        _srcB_combobox->setCurrentIndex(1);

    _result_label = new QLabel(this);
    _result_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _result_label->setWordWrap(true);
    _result_label->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    QFormLayout *src_layout = new QFormLayout();
    src_layout->addRow(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_1ST_SOURCE), "1st Source"), _srcA_combobox);
    src_layout->addRow(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_2ST_SOURCE), "2st Source"), _srcB_combobox);

    _layout = new QVBoxLayout();
    _layout->addLayout(src_layout);
    _layout->addWidget(_result_label, 1);
    _layout->addWidget(&_button_box);

    layout()->addLayout(_layout);
    setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CH_MEASURE),
                 "Channel-to-channel measure"));

    connect(&_button_box, SIGNAL(rejected()), this, SLOT(reject()));
    connect(&_button_box, SIGNAL(accepted()), this, SLOT(accept()));
    connect(_srcA_combobox, SIGNAL(currentIndexChanged(int)),
            this, SLOT(on_source_changed(int)));
    connect(_srcB_combobox, SIGNAL(currentIndexChanged(int)),
            this, SLOT(on_source_changed(int)));

    compute();
}

DsoChannelMeasure::~DsoChannelMeasure()
{
}

void DsoChannelMeasure::on_source_changed(int index)
{
    (void)index;
    compute();
}

void DsoChannelMeasure::compute()
{
    const int idxA = (_srcA_combobox->count() > 0)
        ? _srcA_combobox->itemData(_srcA_combobox->currentIndex()).toInt() : -1;
    const int idxB = (_srcB_combobox->count() > 0)
        ? _srcB_combobox->itemData(_srcB_combobox->currentIndex()).toInt() : -1;

    view::DsoSignal *sigA = NULL;
    view::DsoSignal *sigB = NULL;
    for (auto s : _session->get_signals()) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *d = (view::DsoSignal*)s;
            if (d->get_index() == idxA) sigA = d;
            if (d->get_index() == idxB) sigB = d;
        }
    }

    if (sigA == NULL || sigB == NULL) {
        _result_label->setText(
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CH_NEED_TWO),
                "Select two DSO channels."));
        return;
    }
    if (sigA == sigB) {
        _result_label->setText(
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CH_NEED_DIFF),
                "Select two different channels."));
        return;
    }

    // Prefer the analysed snapshot's own sample rate (matches the waveform
    // painter); fall back to the session's capture-buffer rate.
    double samplerate = (sigA->data() != NULL) ? sigA->data()->samplerate() : 0.0;
    if (samplerate <= 0)
        samplerate = _session->cur_snap_samplerate();
    const double dt_ns = (samplerate > 0) ? 1e9 / samplerate : 0.0;

    EdgeInfo ea = extract_edges(sigA, dt_ns);
    EdgeInfo eb = extract_edges(sigB, dt_ns);

    if (!ea.valid || !eb.valid) {
        _result_label->setText(
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_HIST_NO_DATA),
                "No captured data. Run an acquisition first."));
        return;
    }

    QString text;
    const QString none = "--";

    const QString freqA = (ea.period > 0)
        ? fmt_freq_hz(1e9 / ea.period) : none;
    const QString freqB = (eb.period > 0)
        ? fmt_freq_hz(1e9 / eb.period) : none;

    text += QString("Freq %1: %2\n").arg(sigA->get_name()).arg(freqA);
    text += QString("Freq %1: %2\n\n").arg(sigB->get_name()).arg(freqB);

    // Reference period for phase: the 1st source's period.
    const double period = ea.period;

    double delay_ns = 0, skew_ns = 0;
    const bool have_delay = mean_delay(ea.rising, eb.rising, period, delay_ns);
    const bool have_skew  = mean_delay(ea.falling, eb.falling, period, skew_ns);

    text += QString("%1: %2\n")
                .arg(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CH_DELAY), "Delay (rise)"))
                .arg(have_delay ? fmt_time_ns(delay_ns) : none);
    text += QString("%1: %2\n")
                .arg(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CH_SKEW), "Skew (fall)"))
                .arg(have_skew ? fmt_time_ns(skew_ns) : none);

    QString phase = none;
    if (have_delay && period > 0) {
        double deg = delay_ns / period * 360.0;
        // Present in (-180, 180].
        while (deg > 180)  deg -= 360;
        while (deg <= -180) deg += 360;
        phase = QString::number(deg, 'f', 2) + " °";
    }
    text += QString("%1: %2")
                .arg(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CH_PHASE), "Phase"))
                .arg(phase);

    _result_label->setText(text);
}

} // namespace dialogs
} // namespace pv
