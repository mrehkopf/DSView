/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef DSVIEW_PV_DIALOGS_STACKINGOPTIONS_H
#define DSVIEW_PV_DIALOGS_STACKINGOPTIONS_H

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QSpinBox>
#include <QVector>

#include "dsdialog.h"
#include "../logicstackingconfig.h"

namespace pv {

class SigSession;

namespace dialogs {

class StackingOptions : public DSDialog
{
    Q_OBJECT

public:
    explicit StackingOptions(QWidget *parent, SigSession *session);

public slots:
    void accept() override;

private slots:
    void on_enabled_changed(bool enabled);
    void on_reference_changed();
    void on_analyzer_count_changed(int count);

private:
    // One grid row; the master (A1) has no sync input or manual alignment
    struct AnalyzerRow
    {
        QLabel *label = NULL;
        QComboBox *device = NULL;
        QDoubleSpinBox *threshold = NULL;
        QComboBox *filter = NULL;
        QComboBox *sync_channel = NULL;
        QDoubleSpinBox *manual_shift_ns = NULL;
        QComboBox *reference_channel = NULL;
    };

    void populate_devices();
    void populate_channel_labels();
    void load_config();
    void select_unused_device(int analyzer);
    void update_rows();
    int analyzer_count() const;
    ds_device_handle selected_handle(const QComboBox *combo) const;
    QString channel_label(int analyzer, int physical_index) const;
    QComboBox* new_channel_combo();
    QComboBox* new_filter_combo();
    void set_filter(QComboBox *combo, int filter);
    void set_channel_combo_label(QComboBox *combo, int index, const QString &label);
    void update_channel_combo_popup_width(QComboBox *combo) const;

private:
    SigSession *_session;
    QVector<LogicStackingAnalyzerInfo> _devices;

    QCheckBox *_enable;
    QSpinBox *_analyzer_count;
    QVector<AnalyzerRow> _rows;
    QCheckBox *_show_sync;
    QCheckBox *_reference_alignment;
    QCheckBox *_drift_correction;
    QComboBox *_reference_edge_mode;

    // The master's threshold and filter are its own device settings
    bool _have_threshold;
    bool _have_filter;
    double _master_threshold;
    int _master_filter;
    // SR_CONF_FILTER choices: id, untranslated driver name
    QVector<QPair<int, QString> > _filter_items;
};

}
}

#endif
