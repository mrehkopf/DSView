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
#include <QGridLayout>
#include <QGuiApplication>
#include <QMessageBox>
#include <QScreen>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include "../config/appconfig.h"
#include "../sigsession.h"
#include "../ui/dscombobox.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"
#include "../ui/xspinbox.h"
#include "../view/signal.h"

#include <algorithm>
#include <cmath>

namespace pv {
namespace dialogs {

namespace {

// A driver option label or value, translated like the device options show it
QString dsl_text(const QString &text)
{
    const QByteArray bytes = text.toLocal8Bit();
    return QString(LangResource::Instance()->get_lang_text(STR_PAGE_DSL, bytes.data(), bytes.data()));
}

}

StackingOptions::StackingOptions(QWidget *parent, SigSession *session) :
    DSDialog(parent, true),
    _session(session),
    _have_threshold(false),
    _have_filter(false),
    _master_threshold(0.0),
    _master_filter(0)
{
    setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOGIC_STACKING), "Logic Stacking"));

    // Let all controls, including spin boxes, inherit the configured font.
    QFont dialog_font = font();
    dialog_font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    setFont(dialog_font);

    _enable = new QCheckBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE_TWO_ANALYZER_STACKING), "Enable analyzer stacking"), this);
    _analyzer_count = new QSpinBox(this);
    _show_sync = new QCheckBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SHOW_SECONDARY_SYNC_CHANNEL), "Show secondary sync channels"), this);
    _reference_alignment = new QCheckBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_ALIGNMENT), "Align using reference signal"), this);
    _drift_correction = new QCheckBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_DRIFT_CORRECTION), "Correct clock drift using reference signal"), this);
    _reference_edge_mode = new DsComboBox(this);

    _analyzer_count->setRange(2, LogicStackingMaxAnalyzers);

    // Threshold and filter as the device options show them, if the device has them
    DeviceAgent *device = _session->get_device();
    _have_threshold = device->have_instance() &&
                      device->get_config_double(SR_CONF_VTH, _master_threshold);
    if (device->have_instance() && device->get_config_int16(SR_CONF_FILTER, _master_filter)){
        GVariant *gvar = device->get_config_list(NULL, SR_CONF_FILTER);
        if (gvar != NULL){
            const struct sr_list_item *item = (const struct sr_list_item*)g_variant_get_uint64(gvar);
            g_variant_unref(gvar);
            for (; item != NULL && item->id >= 0; item++)
                _filter_items.push_back(qMakePair(item->id, QString(item->name)));
        }
        _have_filter = !_filter_items.isEmpty();
    }

    _reference_edge_mode->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_RISING), "Rising"),
                                  LogicStackingReferenceRising);
    _reference_edge_mode->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_FALLING), "Falling"),
                                  LogicStackingReferenceFalling);
    _reference_edge_mode->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_BOTH), "Both"),
                                  LogicStackingReferenceBoth);

    // One row per analyzer; rows past the analyzer count are hidden
    QGridLayout *grid = new QGridLayout;
    grid->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_DEVICE), "Device"), this), 0, 1);
    if (_have_threshold)
        grid->addWidget(new QLabel(dsl_text("Threshold Level"), this), 0, 3);
    if (_have_filter)
        grid->addWidget(new QLabel(dsl_text("Filter Targets"), this), 0, 4);
    grid->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY_SYNC_INPUT), "Sync input"), this), 0, 5);
    grid->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY_MANUAL_SHIFT), "Manual alignment"), this), 0, 6);
    grid->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_INPUT), "Reference input"), this), 0, 7);

    const QString master_role = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MASTER), "Master");
    const QString secondary_role = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY), "Secondary");

    _rows.resize(LogicStackingMaxAnalyzers);
    for (int analyzer = 0; analyzer < LogicStackingMaxAnalyzers; analyzer++){
        AnalyzerRow &row = _rows[analyzer];
        row.label = new QLabel(QString("%1 (%2)").arg(LogicStackingConfig::analyzer_label(analyzer),
                                                      analyzer == 0 ? master_role : secondary_role), this);
        row.device = new DsComboBox(this);
        row.identify = new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IDENTIFY), "Identify"), this);
        row.identify->setToolTip(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IDENTIFY_TIP),
                                     "Blinks the device's LED red and green for two seconds."));
        row.reference_channel = new_channel_combo();

        grid->addWidget(row.label, analyzer + 1, 0);
        grid->addWidget(row.device, analyzer + 1, 1);
        grid->addWidget(row.identify, analyzer + 1, 2);
        grid->addWidget(row.reference_channel, analyzer + 1, 7);

        connect(row.identify, &QPushButton::clicked, this, [this, analyzer](){ identify(analyzer); });
        connect(row.device, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int){ update_identify_buttons(); });

        if (_have_threshold){
            row.threshold = new XDoubleSpinBox(this);
            row.threshold->setRange(0.0, 5.0);
            row.threshold->setDecimals(1);
            row.threshold->setSingleStep(0.1);
            row.threshold->setSuffix(" V");
            grid->addWidget(row.threshold, analyzer + 1, 3);
        }
        if (_have_filter){
            row.filter = new_filter_combo();
            grid->addWidget(row.filter, analyzer + 1, 4);
        }

        if (analyzer == 0)
            continue;

        row.sync_channel = new_channel_combo();
        row.manual_shift_ns = new XDoubleSpinBox(this);
        row.manual_shift_ns->setRange(-1000000000.0, 1000000000.0);
        row.manual_shift_ns->setDecimals(3);
        row.manual_shift_ns->setSuffix(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_NS), " ns"));
        row.manual_shift_ns->setSingleStep(1.0);

        grid->addWidget(row.sync_channel, analyzer + 1, 5);
        grid->addWidget(row.manual_shift_ns, analyzer + 1, 6);
    }
    grid->setColumnStretch(1, 1);

    QFormLayout *count_form = new QFormLayout;
    count_form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_ANALYZER_COUNT), "Number of analyzers"), _analyzer_count);

    QFormLayout *reference_form = new QFormLayout;
    reference_form->addRow(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_EDGE), "Reference edge"), _reference_edge_mode);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, SIGNAL(accepted()), this, SLOT(accept()));
    connect(buttons, SIGNAL(rejected()), this, SLOT(reject()));
    connect(_enable, SIGNAL(toggled(bool)), this, SLOT(on_enabled_changed(bool)));
    connect(_reference_alignment, SIGNAL(toggled(bool)), this, SLOT(on_reference_changed()));
    connect(_drift_correction, SIGNAL(toggled(bool)), this, SLOT(on_reference_changed()));

    QVBoxLayout *content_layout = new QVBoxLayout;
    content_layout->addWidget(_enable);
    content_layout->addLayout(count_form);
    content_layout->addLayout(grid);
    content_layout->addWidget(_show_sync);
    content_layout->addWidget(_reference_alignment);
    content_layout->addWidget(_drift_correction);
    content_layout->addLayout(reference_form);
    content_layout->addWidget(buttons);
    layout()->addLayout(content_layout);

    // Apply the configured dialog font before measuring the channel popups.
    update_font();
    populate_devices();
    populate_channel_labels();
    load_config();

    connect(_analyzer_count, SIGNAL(valueChanged(int)), this, SLOT(on_analyzer_count_changed(int)));
}

QComboBox* StackingOptions::new_filter_combo()
{
    QComboBox *combo = new DsComboBox(this);
    for (const auto &item : _filter_items)
        combo->addItem(dsl_text(item.second), item.first);
    return combo;
}

void StackingOptions::set_filter(QComboBox *combo, int filter)
{
    const int index = combo->findData(filter);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

QComboBox* StackingOptions::new_channel_combo()
{
    QComboBox *combo = new DsComboBox(this);
    for (int i = 0; i < LogicStackingChannelsPerAnalyzer; i++)
        combo->addItem(QString(), i);
    return combo;
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
        for (AnalyzerRow &row : _rows)
            row.device->addItem(analyzer.display_name(), handle);
    }

    g_free(array);
}

QString StackingOptions::channel_label(int analyzer, int physical_index) const
{
    const int global_index = LogicStackingConfig::global_channel_index(analyzer, physical_index);
    view::Signal *signal = _session != NULL ? _session->get_signal_by_index(global_index) : NULL;

    // During cold-start remapping the synthetic traces may not exist yet.
    if (signal != NULL && !signal->get_name().trimmed().isEmpty())
        return signal->get_name();

    return QString("%1-D%2").arg(LogicStackingConfig::analyzer_label(analyzer)).arg(physical_index);
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
    // The channel count is set in the device options; inputs past it can't be used
    const int channel_count = _session->logic_stacking_config().channel_count;

    for (int analyzer = 0; analyzer < _rows.size(); analyzer++){
        const AnalyzerRow &row = _rows[analyzer];
        QComboBox *combos[2] = {row.reference_channel, row.sync_channel};

        for (QComboBox *combo : combos){
            if (combo == NULL)
                continue;

            QStandardItemModel *model = qobject_cast<QStandardItemModel*>(combo->model());
            for (int i = 0; i < LogicStackingChannelsPerAnalyzer; i++){
                set_channel_combo_label(combo, i, channel_label(analyzer, i));
                if (model != NULL && model->item(i) != NULL)
                    model->item(i)->setEnabled(i < channel_count);
            }
            update_channel_combo_popup_width(combo);
        }
    }
}

void StackingOptions::load_config()
{
    const LogicStackingConfig &config = _session->logic_stacking_config();

    _enable->setChecked(config.enabled);

    const ds_device_handle active = _session->get_device()->handle();
    const int last_channel = qBound(1, config.channel_count, LogicStackingChannelsPerAnalyzer) - 1;
    const int count = LogicStackingConfig::analyzer_count_valid(config.analyzer_count()) ?
                      config.analyzer_count() : 2;

    // Rows past the saved secondaries start out wired and set like the last one
    LogicStackingSecondary pattern;
    if (!config.secondaries.empty()){
        pattern = config.secondaries.back();
        pattern.manual_shift_ps = 0;
    }
    else{
        pattern.reference_channel = config.master_reference_channel;
    }

    for (int analyzer = 0; analyzer < _rows.size(); analyzer++){
        AnalyzerRow &row = _rows[analyzer];
        ds_device_handle handle = NULL_HANDLE;
        int reference_channel = config.master_reference_channel;

        double threshold = _master_threshold;
        int filter = _master_filter;

        if (analyzer == 0){
            handle = config.master_handle != NULL_HANDLE ? config.master_handle : active;
        }
        else{
            const bool saved = analyzer - 1 < (int)config.secondaries.size();
            const LogicStackingSecondary &secondary = saved ? config.secondaries[analyzer - 1] : pattern;
            if (saved)
                handle = secondary.handle;

            // Unset values follow the master, as they did before they were per analyzer
            if (secondary.threshold >= 0.0)
                threshold = secondary.threshold;
            if (secondary.filter >= 0)
                filter = secondary.filter;

            reference_channel = secondary.reference_channel;
            row.sync_channel->setCurrentIndex(qBound(0, secondary.sync_channel, last_channel));
            row.manual_shift_ns->setValue(secondary.manual_shift_ps / 1000.0);
        }

        row.reference_channel->setCurrentIndex(qBound(0, reference_channel, last_channel));
        if (row.threshold != NULL)
            row.threshold->setValue(threshold);
        if (row.filter != NULL)
            set_filter(row.filter, filter);

        row.device->setCurrentIndex(-1);
        for (int i = 0; handle != NULL_HANDLE && i < row.device->count(); i++){
            if ((ds_device_handle)row.device->itemData(i).toULongLong() == handle)
                row.device->setCurrentIndex(i);
        }
    }

    _analyzer_count->setValue(count);
    for (int analyzer = 0; analyzer < count; analyzer++){
        if (_rows[analyzer].device->currentIndex() < 0)
            select_unused_device(analyzer);
    }

    _show_sync->setChecked(config.show_sync_channel);
    _reference_alignment->setChecked(config.reference_alignment_enabled);
    _drift_correction->setChecked(config.drift_correction_enabled);
    const int edge_index = _reference_edge_mode->findData(config.reference_edge_mode);
    _reference_edge_mode->setCurrentIndex(edge_index >= 0 ? edge_index : 0);

    update_rows();
}

int StackingOptions::analyzer_count() const
{
    return qBound(2, _analyzer_count->value(), (int)_rows.size());
}

// Picks the first device no other shown row uses, or none if all are taken
void StackingOptions::select_unused_device(int analyzer)
{
    QComboBox *combo = _rows[analyzer].device;
    const int count = analyzer_count();

    for (int i = 0; i < combo->count(); i++){
        const ds_device_handle handle = (ds_device_handle)combo->itemData(i).toULongLong();
        bool used = false;
        for (int other = 0; other < count && !used; other++){
            if (other != analyzer && selected_handle(_rows[other].device) == handle)
                used = true;
        }

        if (!used){
            combo->setCurrentIndex(i);
            return;
        }
    }

    combo->setCurrentIndex(-1);
}

void StackingOptions::update_rows()
{
    const int count = analyzer_count();
    for (int analyzer = 0; analyzer < _rows.size(); analyzer++){
        const AnalyzerRow &row = _rows[analyzer];
        const bool shown = analyzer < count;
        QWidget *widgets[8] = {row.label, row.device, row.identify, row.threshold, row.filter,
                               row.sync_channel, row.manual_shift_ns, row.reference_channel};
        for (QWidget *widget : widgets){
            if (widget != NULL)
                widget->setVisible(shown);
        }
    }

    on_enabled_changed(_enable->isChecked());
    update_identify_buttons();

    // adjustSize() caps a window at 2/3 of the screen width, which the table
    // exceeds on small screens; then the device names get cut off first
    QSize size = sizeHint();
    if (QGuiApplication::primaryScreen() != NULL)
        size = size.boundedTo(QGuiApplication::primaryScreen()->availableGeometry().size());
    resize(size);
}

void StackingOptions::on_analyzer_count_changed(int count)
{
    (void)count;

    for (int analyzer = 0; analyzer < analyzer_count(); analyzer++){
        if (_rows[analyzer].device->currentIndex() < 0)
            select_unused_device(analyzer);
    }

    update_rows();
}

void StackingOptions::on_enabled_changed(bool enabled)
{
    _analyzer_count->setEnabled(enabled);
    for (const AnalyzerRow &row : _rows){
        row.device->setEnabled(enabled);
        if (row.threshold != NULL)
            row.threshold->setEnabled(enabled);
        if (row.filter != NULL)
            row.filter->setEnabled(enabled);
        if (row.sync_channel != NULL)
            row.sync_channel->setEnabled(enabled);
        if (row.manual_shift_ns != NULL)
            row.manual_shift_ns->setEnabled(enabled);
    }
    _show_sync->setEnabled(enabled);
    _reference_alignment->setEnabled(enabled);
    _drift_correction->setEnabled(enabled);
    on_reference_changed();
}

void StackingOptions::on_reference_changed()
{
    const bool enabled = _enable->isChecked() &&
                         (_reference_alignment->isChecked() ||
                          _drift_correction->isChecked());
    for (const AnalyzerRow &row : _rows)
        row.reference_channel->setEnabled(enabled);
    _reference_edge_mode->setEnabled(enabled);
}

// The buttons work whether or not stacking is enabled, to find the analyzers
void StackingOptions::update_identify_buttons()
{
    for (const AnalyzerRow &row : _rows){
        if (row.identify != NULL)
            row.identify->setEnabled(selected_handle(row.device) != NULL_HANDLE);
    }
}

void StackingOptions::identify(int analyzer)
{
    const ds_device_handle handle = selected_handle(_rows[analyzer].device);
    if (handle == NULL_HANDLE)
        return;

    if (!_session->identify_device(handle)){
        MsgBox::Show(QString(), L_S(STR_PAGE_MSG, S_ID(IDS_MSG_IDENTIFY_FAILED),
            "The device's LED could not be blinked. It may be unplugged or in use by another program."), this);
    }
}

ds_device_handle StackingOptions::selected_handle(const QComboBox *combo) const
{
    if (combo == NULL || combo->currentIndex() < 0)
        return NULL_HANDLE;

    return (ds_device_handle)combo->currentData().toULongLong();
}

void StackingOptions::accept()
{
    const int count = analyzer_count();

    // Start from the current setup: the channel count and selected channels
    // are set in the device options
    const LogicStackingConfig current = _session->logic_stacking_config();
    LogicStackingConfig config = current;
    config.secondaries.clear();
    config.enabled = _enable->isChecked();
    config.master_handle = selected_handle(_rows[0].device);
    config.master_reference_channel = _rows[0].reference_channel->currentData().toInt();
    for (int analyzer = 1; analyzer < count; analyzer++){
        const AnalyzerRow &row = _rows[analyzer];
        LogicStackingSecondary secondary;
        if (analyzer - 1 < (int)current.secondaries.size())
            secondary.channels = current.secondaries[analyzer - 1].channels;
        secondary.handle = selected_handle(row.device);
        secondary.sync_channel = row.sync_channel->currentData().toInt();
        secondary.manual_shift_ps = (int64_t)llround(row.manual_shift_ns->value() * 1000.0);
        secondary.reference_channel = row.reference_channel->currentData().toInt();
        if (row.threshold != NULL)
            secondary.threshold = row.threshold->value();
        if (row.filter != NULL)
            secondary.filter = row.filter->currentData().toInt();
        config.secondaries.push_back(secondary);
    }
    config.show_sync_channel = _show_sync->isChecked();
    config.reference_alignment_enabled = _reference_alignment->isChecked();
    config.drift_correction_enabled = _drift_correction->isChecked();
    config.reference_edge_mode = _reference_edge_mode->currentData().toInt();

    if (config.enabled){
        if (_devices.size() < count){
            QMessageBox::warning(this,
                                 L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOGIC_STACKING), "Logic Stacking"),
                                 QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_CONNECT_TWO_ANALYZERS), "Connect %1 DSLogic U3Pro32 analyzers first."))
                                     .arg(count));
            return;
        }

        // The device options can leave the master with only its reference
        // input; without the reference it captures nothing
        if (!config.has_channel_model()){
            QMessageBox::warning(this,
                                 L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOGIC_STACKING), "Logic Stacking"),
                                 L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_MASTER_NO_CHANNEL), "Enable at least one channel on the master (A1)."));
            return;
        }

        if (!config.is_valid()){
            QMessageBox::warning(this,
                                 L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOGIC_STACKING), "Logic Stacking"),
                                 L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_SELECT_DIFFERENT_ANALYZERS), "Select a different DSLogic U3Pro32 for each analyzer."));
            return;
        }
    }

    if (!_session->set_logic_stacking_config(config)){
        QMessageBox::warning(this,
                             L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOGIC_STACKING), "Logic Stacking"),
                             L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_SESSION_BUSY), "Stacking mode could not be applied while the session is busy."));
        return;
    }

    // After the config: applying it can switch the active device to a new master
    DeviceAgent *master = _session->get_device();
    if (master->have_instance()){
        double threshold = 0.0;
        if (_rows[0].threshold != NULL && master->get_config_double(SR_CONF_VTH, threshold) &&
            threshold != _rows[0].threshold->value())
            master->set_config_double(SR_CONF_VTH, _rows[0].threshold->value());

        int filter = 0;
        if (_rows[0].filter != NULL && master->get_config_int16(SR_CONF_FILTER, filter) &&
            filter != _rows[0].filter->currentData().toInt())
            master->set_config_int16(SR_CONF_FILTER, _rows[0].filter->currentData().toInt());
    }

    DSDialog::accept();
}

}
}
