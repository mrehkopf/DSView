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
#include <QDialog>
#include <QDoubleSpinBox>
#include <QVector>

#include "../logicstackingconfig.h"

namespace pv {

class SigSession;

namespace dialogs {

class StackingOptions : public QDialog
{
    Q_OBJECT

public:
    explicit StackingOptions(QWidget *parent, SigSession *session);

public slots:
    void accept() override;

private slots:
    void on_enabled_changed(bool enabled);
    void on_reference_changed();

private:
    void populate_devices();
    void populate_channel_labels();
    void load_config();
    ds_device_handle selected_handle(const QComboBox *combo) const;
    QString channel_label(int analyzer, int physical_index) const;
    void set_channel_combo_label(QComboBox *combo, int index, const QString &label);
    void update_channel_combo_popup_width(QComboBox *combo) const;

private:
    SigSession *_session;
    QVector<LogicStackingAnalyzerInfo> _devices;

    QCheckBox *_enable;
    QComboBox *_master;
    QComboBox *_secondary;
    QComboBox *_sync_channel;
    QCheckBox *_show_sync;
    QDoubleSpinBox *_manual_shift_ns;
    QCheckBox *_reference_alignment;
    QCheckBox *_drift_correction;
    QComboBox *_drift_master_channel;
    QComboBox *_drift_secondary_channel;
    QComboBox *_reference_edge_mode;
};

}
}

#endif
