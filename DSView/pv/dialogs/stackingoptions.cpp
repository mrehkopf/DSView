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

#include "stackingoptions.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>

#include "../sigsession.h"
#include "../ui/langresource.h"

#include <cmath>

namespace pv {
namespace dialogs {

StackingOptions::StackingOptions(QWidget *parent, SigSession *session) :
    QDialog(parent),
    _session(session)
{
    setWindowTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOGIC_STACKING), "Logic Stacking"));

    _enable = new QCheckBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE_TWO_ANALYZER_STACKING), "Enable two-analyzer stacking"), this);
    _master = new QComboBox(this);
    _secondary = new QComboBox(this);
    _sync_channel = new QComboBox(this);
    _show_sync = new QCheckBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SHOW_SECONDARY_SYNC_CHANNEL), "Show secondary sync channel"), this);
    _manual_shift_ns = new QDoubleSpinBox(this);

    _manual_shift_ns->setRange(-1000000000.0, 1000000000.0);
    _manual_shift_ns->setDecimals(3);
    _manual_shift_ns->setSuffix(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_NS), " ns"));
    _manual_shift_ns->setSingleStep(1.0);

    for (int i = 0; i < 32; i++)
        _sync_channel->addItem(QString("D%1").arg(i), i);

    QFormLayout *form = new QFormLayout;
    form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MASTER), "Master"), _master);
    form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY), "Secondary"), _secondary);
    form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY_SYNC_INPUT), "Secondary sync input"), _sync_channel);
    form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY_MANUAL_SHIFT), "Secondary manual shift"), _manual_shift_ns);
    form->addRow(QString(), _show_sync);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, SIGNAL(accepted()), this, SLOT(accept()));
    connect(buttons, SIGNAL(rejected()), this, SLOT(reject()));
    connect(_enable, SIGNAL(toggled(bool)), this, SLOT(on_enabled_changed(bool)));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(_enable);
    layout->addLayout(form);
    layout->addWidget(buttons);

    populate_devices();
    load_config();
    on_enabled_changed(_enable->isChecked());
}

void StackingOptions::populate_devices()
{
    ds_device_base_info *array = NULL;
    int count = 0;

    if (ds_get_device_list(&array, &count) != SR_OK || array == NULL)
        return;

    for (int i = 0; i < count; i++){
        const ds_device_base_info *info = array + i;
        const QString name = QString::fromLocal8Bit(info->name);
        if (!name.contains("DSLogic U3Pro32", Qt::CaseInsensitive))
            continue;

        LogicStackingAnalyzerInfo analyzer;
        analyzer.handle = info->handle;
        analyzer.name = name;
        analyzer.unique_id = QString::fromLocal8Bit(info->unique_id);
        _devices.push_back(analyzer);

        const QVariant handle = QVariant::fromValue((unsigned long long)info->handle);
        _master->addItem(analyzer.display_name(), handle);
        _secondary->addItem(analyzer.display_name(), handle);
    }

    g_free(array);
}

void StackingOptions::load_config()
{
    const LogicStackingConfig &config = _session->logic_stacking_config();

    _enable->setChecked(config.enabled);

    const ds_device_handle active = _session->get_device()->handle();
    const ds_device_handle master = config.master_handle != NULL_HANDLE ? config.master_handle : active;
    const ds_device_handle secondary = config.secondary_handle;

    for (int i = 0; i < _master->count(); i++){
        if ((ds_device_handle)_master->itemData(i).toULongLong() == master)
            _master->setCurrentIndex(i);
        if ((ds_device_handle)_secondary->itemData(i).toULongLong() == secondary)
            _secondary->setCurrentIndex(i);
    }

    if (secondary == NULL_HANDLE && _secondary->count() > 1){
        int secondary_index = _master->currentIndex() == 0 ? 1 : 0;
        _secondary->setCurrentIndex(secondary_index);
    }

    _sync_channel->setCurrentIndex(qBound(0, config.secondary_sync_channel, 31));
    _show_sync->setChecked(config.show_sync_channel);
    _manual_shift_ns->setValue(config.secondary_manual_shift_ps / 1000.0);
}

void StackingOptions::on_enabled_changed(bool enabled)
{
    _master->setEnabled(enabled);
    _secondary->setEnabled(enabled);
    _sync_channel->setEnabled(enabled);
    _show_sync->setEnabled(enabled);
    _manual_shift_ns->setEnabled(enabled);
}

ds_device_handle StackingOptions::selected_handle(const QComboBox *combo) const
{
    if (combo == NULL || combo->currentIndex() < 0)
        return NULL_HANDLE;

    return (ds_device_handle)combo->currentData().toULongLong();
}

void StackingOptions::accept()
{
    LogicStackingConfig config;
    config.enabled = _enable->isChecked();
    config.master_handle = selected_handle(_master);
    config.secondary_handle = selected_handle(_secondary);
    config.secondary_sync_channel = _sync_channel->currentData().toInt();
    config.show_sync_channel = _show_sync->isChecked();
    config.secondary_manual_shift_ps = (int64_t)llround(_manual_shift_ns->value() * 1000.0);

    if (config.enabled){
        if (_devices.size() < 2){
            QMessageBox::warning(this,
                                 L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOGIC_STACKING), "Logic Stacking"),
                                 L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_CONNECT_TWO_ANALYZERS), "Connect two DSLogic U3Pro32 analyzers first."));
            return;
        }

        if (!config.is_valid()){
            QMessageBox::warning(this,
                                 L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOGIC_STACKING), "Logic Stacking"),
                                 L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_SELECT_DIFFERENT_ANALYZERS), "Select two different DSLogic U3Pro32 analyzers."));
            return;
        }
    }

    if (!_session->set_logic_stacking_config(config)){
        QMessageBox::warning(this,
                             L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOGIC_STACKING), "Logic Stacking"),
                             L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_SESSION_BUSY), "Stacking mode could not be applied while the session is busy."));
        return;
    }

    QDialog::accept();
}

}
}
