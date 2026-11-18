/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
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


#ifndef DSVIEW_PV_TRIGGERDOCK_H
#define DSVIEW_PV_TRIGGERDOCK_H

#include <QDockWidget>
#include <QPushButton>
#include <QLabel>
#include <QRadioButton>
#include <QSlider>
#include <QGroupBox>
#include <QCheckBox>
#include <QTableWidget>
#include <QJsonObject>

#include <QVector>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <vector>
#include "../ui/dscombobox.h"
#include "../interface/icallbacks.h"
#include "../ui/uimanager.h"
#include "keywordlineedit.h"
#include "../trigger/decoderbus.h"

namespace pv {

class SigSession;

namespace dock {

class TriggerDock : public QScrollArea, public IUiWindow
{
    Q_OBJECT

private:
    static const int MinTrigPosition;

public:
    TriggerDock(QWidget *parent, SigSession *session);
    ~TriggerDock();

    void update_view();

    QJsonObject get_session();
    void set_session(QJsonObject ses);

    void device_updated();

    void try_commit_trigger();

    // Control interface (pv/control). Patterns map a channel index to one of
    // X 0 1 R F C; a missing channel means X.
    struct ControlStage
    {
        trigger::ChannelPattern pattern[2];
        bool    negate[2];
        bool    or_logic;
        bool    contiguous;
        int     count;

        ControlStage();
    };

    struct ControlSerial
    {
        trigger::ChannelPattern start;
        trigger::ChannelPattern stop;
        trigger::ChannelPattern clock;
        int     channel;
        int     bits;
        QString value;  // 16 symbols of 0 1 X, bit 15 first
    };

    struct ControlTrigger
    {
        int     mode;   // SIMPLE_TRIGGER, ADV_TRIGGER or SERIAL_TRIGGER
        int     position;
        trigger::ChannelPattern simple;
        QVector<ControlStage> stages;   // the active stages
        ControlSerial serial;
    };

    ControlTrigger control_trigger() const;
    // These expect values checked against the limits below. Set the mode
    // last: changing the stage count enables the advanced widgets.
    void set_control_position(int position);
    void set_control_simple(const trigger::ChannelPattern &conditions);
    void set_control_stages(const QVector<ControlStage> &stages);
    void set_control_mode(int mode);
    int  trigger_channel_count() const { return _cur_ch_num; }
    int  max_position() const;
    // Whether the advanced and serial triggers can be used; if not, why.
    bool advanced_available(QString &reason) const;
    // Enabled channels that can be part of a trigger.
    QSet<int> trigger_channels() const;

signals:
    void trigger_changed();

private:
    void retranslateUi();
    void reStyle();

    void setup_adv_tab();
    void lineEdit_highlight(PopupLineEdit *dst);
    void setup_bus_editor();
    void set_bus_status(const QString &message, const QString &warning = QString());
    void change_bus_pattern(bool clear);
    trigger::ChannelPattern simple_pattern() const;
    trigger::ChannelPattern bus_target_pattern() const;
    QSet<int> available_bus_channels() const;
    QSet<int> untriggerable_bus_channels() const;
    QString bus_partial_warning(const trigger::BusMapping &mapping, const QSet<int> &untriggerable) const;
    QString bus_value_error(trigger::BusValueError error) const;
    void update_trigger_mode();
    // A pattern from a low (channels 0-15) and an optional high (16-31) field.
    trigger::ChannelPattern read_pattern(PopupLineEdit *low, PopupLineEdit *high) const;
    void write_pattern(PopupLineEdit *low, PopupLineEdit *high, const trigger::ChannelPattern &pattern);
    PopupLineEdit *ext32_field(const QVector<PopupLineEdit *> &list, int i) const;
    void control_changed();

      /*
     * commit trigger setting
     * return 0: simple trigger
     *        1: advanced trigger
     */
    bool commit_trigger();

    //IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

private slots:
    void simple_trigger();
    void adv_trigger();
    void widget_enable(int index);
    void value_changed();
    void on_hex_checkbox_click(bool ck);
    void on_serial_value_changed(const QString &v);
    void on_serial_hex_changed();
    void update_bus_editor();
    void update_bus_status();
    void apply_bus_value();
    void clear_bus_value();
    void on_simple_trigger_changed();

private:
    SigSession *_session;

    int _cur_ch_num;
    QWidget *_widget;

    QRadioButton *_simple_radioButton;
    QRadioButton *_adv_radioButton;

    QGroupBox *_bus_group;
    DsComboBox *_bus_combo;
    DsComboBox *_bus_pattern_combo;
    QLineEdit *_bus_value;
    QPushButton *_bus_apply;
    QPushButton *_bus_clear;
    QLabel *_bus_target;
    QLabel *_bus_status;
    QVector<trigger::DecoderBus> _buses;
    trigger::ChannelPattern _acknowledged_simple_trigger;
    // Set through the control interface, which asks for exactly this; no
    // multi-channel warning while it is unchanged.
    trigger::ChannelPattern _control_simple_trigger;
    bool _applying_bus;

    QLabel *_position_label;
    PopupLineEdit *_position_spinBox;
    QSlider *_position_slider;

    QLabel *_stages_label;
    DsComboBox *stages_comboBox;

    QTabWidget *_stage_tabWidget;

    QVector <QGroupBox *> _stage_groupBox_list;
    QVector <QLabel *> _pattern0_label_list;
    QVector <QLabel *> _pattern1_label_list;
    QVector <QLabel *>    _mu_label_list;
    QVector <DsComboBox *> _logic_comboBox_list;
    QVector <PopupLineEdit *> _value0_lineEdit_list;
    QVector <PopupLineEdit *> _value0_ext32_lineEdit_list;
    QVector <PopupLineEdit *> _count_spinBox_list;
    QVector <DsComboBox *> _inv0_comboBox_list;
    QVector <PopupLineEdit *> _value1_lineEdit_list;
    QVector <PopupLineEdit *> _value1_ext32_lineEdit_list;
    QVector <DsComboBox *> _inv1_comboBox_list;
    QVector <QCheckBox *> _contiguous_checkbox_list;

    QTabWidget *_adv_tabWidget;
    QGroupBox *_serial_groupBox;
    QLabel *_serial_start_label;
    PopupLineEdit *_serial_start_lineEdit;
    PopupLineEdit *_serial_start_ext32_lineEdit;
    QLabel *_serial_stop_label;
    PopupLineEdit *_serial_stop_lineEdit;
    PopupLineEdit *_serial_stop_ext32_lineEdit;
    QLabel *_serial_edge_label;
    PopupLineEdit *_serial_edge_lineEdit;
    PopupLineEdit *_serial_edge_ext32_lineEdit;
    QLabel *_serial_data_label;
    DsComboBox *_serial_data_comboBox;
    QLabel *_serial_value_label;
    PopupLineEdit *_serial_value_lineEdit;
    DsComboBox *_serial_bits_comboBox;
    QLabel *_serial_hex_label;
    PopupLineEdit *_serial_hex_lineEdit;
    QLabel *_serial_hex_ck_label;

    QLabel *_serial_note_label;
    QLabel *_data_bits_label;
    bool    _is_serial_val_setting;

    QVector <QLabel *>  _inv_exp_label_list;
    QVector <QLabel *>  _count_exp_label_list;
    QVector <QLabel *>  _contiguous_label_list;
    QVector <QLabel *>  _stage_note_label_list;

};

} // namespace dock
} // namespace pv

#endif // DSVIEW_PV_TRIGGERDOCK_H
