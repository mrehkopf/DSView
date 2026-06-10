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

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>

#include "../sigsession.h"
#include "../ui/langresource.h"
#include "../view/signal.h"

#include <algorithm>
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
    _reference_alignment = new QCheckBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_ALIGNMENT), "Align using reference signal"), this);
    _drift_correction = new QCheckBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_DRIFT_CORRECTION), "Correct clock drift using reference signal"), this);
    _drift_master_channel = new QComboBox(this);
    _drift_secondary_channel = new QComboBox(this);
    _reference_edge_mode = new QComboBox(this);

    _manual_shift_ns->setRange(-1000000000.0, 1000000000.0);
    _manual_shift_ns->setDecimals(3);
    _manual_shift_ns->setSuffix(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_NS), " ns"));
    _manual_shift_ns->setSingleStep(1.0);

    for (int i = 0; i < 32; i++){
        _sync_channel->addItem(QString(), i);
        _drift_master_channel->addItem(QString(), i);
        _drift_secondary_channel->addItem(QString(), i);
    }

    _reference_edge_mode->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_RISING), "Rising"),
                                  LogicStackingReferenceRising);
    _reference_edge_mode->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_FALLING), "Falling"),
                                  LogicStackingReferenceFalling);
    _reference_edge_mode->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_BOTH), "Both"),
                                  LogicStackingReferenceBoth);

    QFormLayout *form = new QFormLayout;
    form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MASTER), "Master"), _master);
    form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY), "Secondary"), _secondary);
    form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY_SYNC_INPUT), "Secondary sync input"), _sync_channel);
    form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY_MANUAL_SHIFT), "Secondary manual shift"), _manual_shift_ns);
    form->addRow(QString(), _show_sync);
    form->addRow(QString(), _reference_alignment);
    form->addRow(QString(), _drift_correction);
    form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_MASTER_REFERENCE), "Master reference input"), _drift_master_channel);
    form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_SECONDARY_REFERENCE), "Secondary reference input"), _drift_secondary_channel);
    form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_EDGE), "Reference edge"), _reference_edge_mode);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, SIGNAL(accepted()), this, SLOT(accept()));
    connect(buttons, SIGNAL(rejected()), this, SLOT(reject()));
    connect(_enable, SIGNAL(toggled(bool)), this, SLOT(on_enabled_changed(bool)));
    connect(_reference_alignment, SIGNAL(toggled(bool)), this, SLOT(on_reference_changed()));
    connect(_drift_correction, SIGNAL(toggled(bool)), this, SLOT(on_reference_changed()));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(_enable);
    layout->addLayout(form);
    layout->addWidget(buttons);

    populate_devices();
    populate_channel_labels();
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

QString StackingOptions::channel_label(int analyzer, int physical_index) const
{
    const int global_index = analyzer == 0 ? physical_index : 32 + physical_index;
    view::Signal *signal = _session != NULL ? _session->get_signal_by_index(global_index) : NULL;

    // During cold-start remapping the synthetic traces may not exist yet.
    if (signal != NULL && !signal->get_name().trimmed().isEmpty())
        return signal->get_name();

    return QString("A%1-D%2").arg(analyzer + 1).arg(physical_index);
}

void StackingOptions::set_channel_combo_label(QComboBox *combo, int index, const QString &label)
{
    combo->setItemText(index, label);
    combo->setItemData(index, label, Qt::ToolTipRole);
}

void StackingOptions::update_channel_combo_popup_width(QComboBox *combo) const
{
    int width = combo->sizeHint().width();
    for (int i = 0; i < combo->count(); i++)
        width = std::max(width, combo->fontMetrics().horizontalAdvance(combo->itemText(i)));

    // Account for popup frame/padding and the vertical scrollbar stealing text space.
    width += 48;
    combo->view()->setMinimumWidth(width);
    combo->view()->setTextElideMode(Qt::ElideNone);
}

void StackingOptions::populate_channel_labels()
{
    for (int i = 0; i < 32; i++){
        set_channel_combo_label(_sync_channel, i, channel_label(1, i));
        set_channel_combo_label(_drift_master_channel, i, channel_label(0, i));
        set_channel_combo_label(_drift_secondary_channel, i, channel_label(1, i));
    }

    update_channel_combo_popup_width(_sync_channel);
    update_channel_combo_popup_width(_drift_master_channel);
    update_channel_combo_popup_width(_drift_secondary_channel);
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
    _reference_alignment->setChecked(config.reference_alignment_enabled);
    _drift_correction->setChecked(config.drift_correction_enabled);
    _drift_master_channel->setCurrentIndex(qBound(0, config.drift_master_channel, 31));
    _drift_secondary_channel->setCurrentIndex(qBound(0, config.drift_secondary_channel, 31));
    const int edge_index = _reference_edge_mode->findData(config.reference_edge_mode);
    _reference_edge_mode->setCurrentIndex(edge_index >= 0 ? edge_index : 0);
}

void StackingOptions::on_enabled_changed(bool enabled)
{
    _master->setEnabled(enabled);
    _secondary->setEnabled(enabled);
    _sync_channel->setEnabled(enabled);
    _show_sync->setEnabled(enabled);
    _manual_shift_ns->setEnabled(enabled);
    _reference_alignment->setEnabled(enabled);
    _drift_correction->setEnabled(enabled);
    on_reference_changed();
}

void StackingOptions::on_reference_changed()
{
    const bool enabled = _enable->isChecked() &&
                         (_reference_alignment->isChecked() ||
                          _drift_correction->isChecked());
    _drift_master_channel->setEnabled(enabled);
    _drift_secondary_channel->setEnabled(enabled);
    _reference_edge_mode->setEnabled(enabled);
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
    config.reference_alignment_enabled = _reference_alignment->isChecked();
    config.drift_correction_enabled = _drift_correction->isChecked();
    config.drift_master_channel = _drift_master_channel->currentData().toInt();
    config.drift_secondary_channel = _drift_secondary_channel->currentData().toInt();
    config.reference_edge_mode = _reference_edge_mode->currentData().toInt();

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
