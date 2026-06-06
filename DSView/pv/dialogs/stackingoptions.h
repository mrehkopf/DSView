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

private:
    void populate_devices();
    void load_config();
    ds_device_handle selected_handle(const QComboBox *combo) const;

private:
    SigSession *_session;
    QVector<LogicStackingAnalyzerInfo> _devices;

    QCheckBox *_enable;
    QComboBox *_master;
    QComboBox *_secondary;
    QComboBox *_sync_channel;
    QCheckBox *_show_sync;
    QDoubleSpinBox *_manual_shift_ns;
};

}
}

#endif
