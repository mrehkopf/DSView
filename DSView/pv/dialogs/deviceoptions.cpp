/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include "deviceoptions.h"
 
#include <QListWidget>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QLayoutItem>
#include <assert.h>

#include "dsmessagebox.h"
#include "../prop/property.h"
#include "../dsvdef.h"
#include "../config/appconfig.h"
#include "../appcontrol.h"
#include "../sigsession.h"
#include "../ui/langresource.h"
#include "../log.h"
#include "../ui/msgbox.h"

using namespace boost;
using namespace std;

//--------------------------ChannelLabel

ChannelLabel::ChannelLabel(IChannelCheck *check, QWidget *parent, int chanIndex)
: QWidget(parent)
{
    _checked = check;
    _index = chanIndex;  

    QGridLayout *lay = new QGridLayout();
    lay->setContentsMargins(0,0,0,0);
    lay->setSpacing(0);
    this->setLayout(lay);
    _lb = new QLabel(QString::number(chanIndex));
    _lb->setAlignment(Qt::AlignCenter);
    _box = new QCheckBox();
    _box->setFixedSize(20,20);
    lay->addWidget(_lb, 0, 0, Qt::AlignCenter);
    lay->addWidget(_box, 1, 0, Qt::AlignCenter);

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    _lb->setFont(font);

    int fh = _lb->fontMetrics().height();
    int w = _lb->fontMetrics().horizontalAdvance(_lb->text()) + 5;
    w = w < 30 ? 30 : w;
    int h = fh + _box->height() + 2;
    setFixedSize(w, h);

    connect(_box, SIGNAL(released()), this, SLOT(on_checked()));
}

void ChannelLabel::on_checked()
{
    assert(_checked);
    _checked->ChannelChecked(_index, _box);
}

//--------------------------DeviceOptions
 
namespace pv {
namespace dialogs {

DeviceOptions::DeviceOptions(QWidget *parent) :
    DSDialog(parent)
{
    _scroll_panel = NULL;
    _container_panel = NULL;   
    _scroll = NULL; 
    _width = 0;
    _groupHeight1 = 0;
    _groupHeight2 = 0;
    _dynamic_panel = NULL;
    _container_lay = NULL;
    _isBuilding = false;
    _cur_analog_tag_index = 0;

    SigSession *session = AppControl::Instance()->GetSession();
    _device_agent = session->get_device();

    this->setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DEVICE_OPTIONS), "Device Options"));
    this->SetTitleSpace(0);
    this->layout()->setSpacing(0);
    this->layout()->setDirection(QBoxLayout::TopToBottom);
    this->layout()->setAlignment(Qt::AlignTop); 

    // scroll panel
    _scroll_panel  = new QWidget();
    QVBoxLayout *scroll_lay = new QVBoxLayout();
    scroll_lay->setContentsMargins(0, 0, 0, 0);
    scroll_lay->setDirection(QBoxLayout::TopToBottom);
    _scroll_panel->setLayout(scroll_lay);
    this->layout()->addWidget(_scroll_panel);

    // container
    _container_panel = new QWidget();      
    _container_lay = new QVBoxLayout();
    _container_lay->setDirection(QBoxLayout::TopToBottom);
    _container_lay->setAlignment(Qt::AlignTop);
    _container_lay->setContentsMargins(0, 0, 0, 0);
    _container_lay->setSpacing(5);
    _container_panel->setLayout(_container_lay);
    scroll_lay->addWidget(_container_panel);

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
   
    // mode group box
    QString props_title = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MODE), "Mode");
    if (_device_agent->is_logic_stacking())
        props_title = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SHARED_OPTIONS), "Stacking Common Options");
    QGroupBox *props_box = new QGroupBox(props_title, this);
    props_box->setFont(font);
    props_box->setMinimumHeight(70);
    props_box->setAlignment(Qt::AlignTop);
    QLayout *props_lay = get_property_form(props_box);
    props_lay->setContentsMargins(5, 20, 5, 5);
    props_box->setLayout(props_lay);
    _container_lay->addWidget(props_box);

    QWidget *minWid = new QWidget();
    minWid->setFixedHeight(1);
    minWid->setMinimumWidth(300);
    _container_lay->addWidget(minWid);

    // chnnels group box; while stacking it edits a copy of the stacking setup
    if (_device_agent->is_logic_stacking())
        _stacking = _device_agent->logic_stacking_config();
    this->build_dynamic_panel();

    // space
    QWidget *space = new QWidget();
    space->setMinimumHeight(5);
    this->layout()->addWidget(space);

    //button
    auto button_box = new QDialogButtonBox(QDialogButtonBox::Ok, Qt::Horizontal, this);
	this->layout()->addWidget(button_box); 
   
    _device_agent->get_config_int16(SR_CONF_OPERATION_MODE, _opt_mode);

    if (_device_agent->is_demo())
        _demo_operation_mode = _device_agent->get_demo_operation_mode();

    try_resize_scroll();
  
    connect(&_mode_check_timer, SIGNAL(timeout()), this, SLOT(mode_check_timeout()));
    connect(button_box, SIGNAL(accepted()), this, SLOT(accept()));

    _mode_check_timer.setInterval(100);
    if (!_device_agent->is_logic_stacking())
        _mode_check_timer.start();
}

DeviceOptions::~DeviceOptions()
{
    for(auto p : _probe_options_binding_list) {
        delete p;
    }
    _probe_options_binding_list.clear();
}

void DeviceOptions::ChannelChecked(int index, QObject *object)
{
    QCheckBox* sc = dynamic_cast<QCheckBox*>(object);

    // Stacked channel boxes carry the global channel index
    if (!_stacking_boxes.empty() && sc != NULL){
        const int analyzer = index / pv::LogicStackingChannelsPerAnalyzer;
        const int input = index % pv::LogicStackingChannelsPerAnalyzer;
        uint32_t *mask = analyzer == 0 ? &_stacking.master_channels :
                         analyzer <= (int)_stacking.secondaries.size() ?
                             &_stacking.secondaries[analyzer - 1].channels : NULL;
        if (mask != NULL){
            if (sc->isChecked())
                *mask |= 1u << input;
            else
                *mask &= ~(1u << input);
        }
        return;
    }

    channel_checkbox_clicked(sc);
}

void DeviceOptions::accept()
{
	using namespace Qt;
    bool hasEnabled = false;

	// Commit the properties
    const auto &dev_props = _device_options_binding.properties();

    for(auto p : dev_props) {
		p->commit();
	}

    if (_device_agent->is_logic_stacking()) {
        // Only the channel count and selected inputs are edited here
        SigSession *session = AppControl::Instance()->GetSession();
        pv::LogicStackingConfig config = session->logic_stacking_config();
        config.channel_count = _stacking.channel_count;
        config.master_channels = _stacking.master_channels;
        for (size_t i = 0; i < config.secondaries.size() && i < _stacking.secondaries.size(); i++)
            config.secondaries[i].channels = _stacking.secondaries[i].channels;

        if (config.captured_channels(0) == 0){
            MsgBox::Show(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_MASTER_NO_CHANNEL),
                             "Enable at least one channel on the master (A1)."));
            return;
        }

        if (!session->set_logic_stacking_config(config)){
            MsgBox::Show(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_STACKING_SESSION_BUSY),
                             "Stacking mode could not be applied while the session is busy."));
            return;
        }

        QDialog::accept();
        return;
    }

    // Commit the probes
    int mode = _device_agent->get_work_mode();
    if (mode == LOGIC || mode == ANALOG) {
        int index = 0;
        for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
            sr_channel *const probe = (sr_channel*)l->data;
            assert(probe);
            probe->enabled = _probes_checkBox_list.at(index)->isChecked();
            index++;
            if (probe->enabled)
                hasEnabled = true;
        }
    }
    else {
        hasEnabled = true;
    }

    if (hasEnabled) {
        auto it = _probe_options_binding_list.begin();
        while(it != _probe_options_binding_list.end()) {
            const auto &probe_props = (*it)->properties();

            for(auto p :probe_props) {
                p->commit();
            }
            it++;
        }

        QDialog::accept();        
    }
    else { 
        QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_ALL_CHANNEL_DISABLE), "All channel disabled! Please enable at least one channel."));
        MsgBox::Show(strMsg);        
    }
}

void DeviceOptions::reject()
{
    using namespace Qt;

    QDialog::reject();
}

QLayout * DeviceOptions::get_property_form(QWidget * parent)
{
    QGridLayout *const layout = new QGridLayout(parent);
    layout->setVerticalSpacing(2);
    const auto &properties =_device_options_binding.properties();

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);

    int i = 0;
    for(auto p : properties)
	{ 
        const QString label = p->labeled_widget() ? QString() : p->label();       
        QString lable_text = "";

        if (label != ""){
            QByteArray bytes = label.toLocal8Bit();
            const char *lang_str = LangResource::Instance()->get_lang_text(STR_PAGE_DSL, bytes.data(), bytes.data());
            lable_text = QString(lang_str);
        }

        QLabel *lb = new QLabel(lable_text, parent);
        lb->setFont(font);
        layout->addWidget(lb, i, 0);
        
        if (label ==  QString("Operation Mode")){
            QWidget *wid = p->get_widget(parent, true);
            wid->setFont(font);
            layout->addWidget(wid, i, 1);
        }
        else{
            QWidget *wid = p->get_widget(parent);
            wid->setFont(font);
            layout->addWidget(wid, i, 1);
        }
        layout->setRowMinimumHeight(i, 22);
        i++;
	}

    _groupHeight1 = parent->sizeHint().height() + 30; 
 
    parent->setFixedHeight(_groupHeight1); 

    return layout;
}

void DeviceOptions::logic_probes(QVBoxLayout &layout)
{
	using namespace Qt;

    layout.setSpacing(2);

    int row1 = 0;
    int row2 = 0;
    int vld_ch_num = 0;
    int cur_ch_num = 0;
    int total_ch_num = 0;
    int contentHeight = 0;
 
    _probes_checkBox_list.clear();

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
  
    //channel count checked
    if (_device_agent->get_work_mode()== LOGIC) {
        GVariant * gvar_opts = _device_agent->get_config_list(NULL, SR_CONF_CHANNEL_MODE);

        if (gvar_opts != NULL)
        {
            struct sr_list_item *plist = (struct sr_list_item*)g_variant_get_uint64(gvar_opts);
            g_variant_unref(gvar_opts);

            int ch_mode = 0;
            _device_agent->get_config_int16(SR_CONF_CHANNEL_MODE, ch_mode);
            _channel_mode_indexs.clear();

            while (plist != NULL && plist->id >= 0)
            {
                row1++;
                QString mode_bt_text = LangResource::Instance()->get_lang_text(STR_PAGE_DSL, plist->name, plist->name);
                QRadioButton *mode_button = new QRadioButton(mode_bt_text);
                mode_button->setFont(font);
                ChannelModePair mode_index;
                mode_index.key = mode_button;
                mode_index.value = plist->id;
                _channel_mode_indexs.push_back(mode_index);
                
                layout.addWidget(mode_button); 
                contentHeight += mode_button->sizeHint().height();  //radio button height
                
                connect(mode_button, SIGNAL(pressed()), this, SLOT(channel_check()));
 
                if (plist->id == ch_mode)
                    mode_button->setChecked(true); 

                plist++;
            }
        }
    }

    _device_agent->get_config_int16(SR_CONF_VLD_CH_NUM, vld_ch_num);
    if(!_device_agent->get_config_int16(SR_CONF_TOTAL_CH_NUM, total_ch_num)) {
        total_ch_num = vld_ch_num;
    };
    // channels
    QWidget *channel_pannel = new QWidget();
    QGridLayout *channel_grid = new QGridLayout();
    channel_grid->setContentsMargins(0,0,0,0);
    channel_grid->setSpacing(0);
    channel_pannel->setLayout(channel_grid);

    int channel_row = 0;
    int channel_column = 0;
    int channel_line_height = 0;
    row2++;
    const GSList *l = _device_agent->get_channels();
    for (int i = 0; i < total_ch_num; i++) {
        if(l) {
		    sr_channel *const probe = (sr_channel*)l->data;
            if (probe->enabled)
                cur_ch_num++;

            if (cur_ch_num > vld_ch_num)
                probe->enabled = false;

            ChannelLabel *ch_item = new ChannelLabel(this, NULL, probe->index);
            channel_grid->addWidget(ch_item, channel_row, channel_column++, Qt::AlignCenter);
            _probes_checkBox_list.push_back(ch_item->getCheckBox());
            ch_item->getCheckBox()->setCheckState(probe->enabled ? Qt::Checked : Qt::Unchecked);
            channel_line_height = ch_item->height();

            if (channel_column == 8){
                channel_column = 0;
                channel_row++;
                if (i < total_ch_num - 1){
                    row2++;
                }
            }
            l = l->next;
        } else {
            // display the same checkbox but disabled and unchecked
            ChannelLabel *ch_item = new ChannelLabel(this, NULL, i);
            channel_grid->addWidget(ch_item, channel_row, channel_column++, Qt::AlignCenter);
            ch_item->getCheckBox()->setEnabled(false);
            ch_item->getCheckBox()->setCheckState(Qt::Unchecked);
            ch_item->getLabel()->setEnabled(false);
            channel_line_height = ch_item->height();
            if (channel_column == 8){
                channel_column = 0;
                channel_row++;
                
                if (i < total_ch_num - 1){
                    row2++;
                }
            }
        }
	}
    layout.addWidget(channel_pannel);

    // space
    QWidget *space = new QWidget();
    space->setFixedHeight(10);
    layout.addWidget(space);
    contentHeight += 10;
 
    // buttons
    QHBoxLayout *line_lay = new QHBoxLayout();
    layout.addLayout(line_lay);
    line_lay->setSpacing(10);

    QPushButton *enable_all_probes = new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE_ALL), "Enable All"));
    QPushButton *disable_all_probes = new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISABLE_ALL), "Disable All"));
    enable_all_probes->setMaximumHeight(33);
    disable_all_probes->setMaximumHeight(33);
    enable_all_probes->setFont(font);
    disable_all_probes->setFont(font);

    // Same width for both, wide enough for the longer (translated) label.
    int bt_width = std::max(enable_all_probes->sizeHint().width(),
                            disable_all_probes->sizeHint().width());
    enable_all_probes->setFixedWidth(bt_width);
    disable_all_probes->setFixedWidth(bt_width);
    line_lay->setAlignment(Qt::AlignLeft);

    this->update_font(); 

    contentHeight += enable_all_probes->sizeHint().height();
    contentHeight += channel_line_height * row2 + 50;

    connect(enable_all_probes, SIGNAL(clicked()),
            this, SLOT(enable_all_probes()));
    connect(disable_all_probes, SIGNAL(clicked()),
            this, SLOT(disable_all_probes()));

    line_lay->addWidget(enable_all_probes);
    line_lay->addWidget(disable_all_probes);

    _groupHeight2 = contentHeight + (row1 + row2) * 2 + 38;

#ifdef Q_OS_DARWIN
    _groupHeight2 += 5;
#endif

    _dynamic_panel->setFixedHeight(_groupHeight2); 
}

// Inputs whose analyzer setting needs more channels than channel_count
QString DeviceOptions::stacking_count_conflicts(int channel_count) const
{
    const QString sync_input = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY_SYNC_INPUT), "Sync input");
    const QString reference_input = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_INPUT), "Reference input");
    QStringList inputs;

    for (int analyzer = 0; analyzer < _stacking.analyzer_count(); analyzer++){
        const QString label = pv::LogicStackingConfig::analyzer_label(analyzer);
        if (analyzer > 0 && _stacking.secondaries[analyzer - 1].sync_channel >= channel_count)
            inputs << QString("%1 %2: D%3").arg(label, sync_input)
                                         .arg(_stacking.secondaries[analyzer - 1].sync_channel);
        if (_stacking.uses_reference() && _stacking.reference_channel(analyzer) >= channel_count)
            inputs << QString("%1 %2: D%3").arg(label, reference_input)
                                         .arg(_stacking.reference_channel(analyzer));
    }

    return inputs.join(", ");
}

// One row of channel modes for all analyzers, then each analyzer's inputs
void DeviceOptions::stacking_probes(QVBoxLayout &layout)
{
    layout.setSpacing(2);
    _stacking_count_buttons.clear();
    _stacking_boxes.clear();

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    int contentHeight = 0;

    for (const pv::LogicStackingChannelMode &mode : pv::LogicStackingChannelModes){
        const QString text = LangResource::Instance()->get_lang_text(STR_PAGE_DSL, mode.name, mode.name);
        QRadioButton *button = new QRadioButton(text);
        button->setFont(font);
        button->setChecked(mode.channel_count == _stacking.channel_count);

        // A sync or reference input outside the mode's channels can't be used
        const QString conflicts = stacking_count_conflicts(mode.channel_count);
        if (!conflicts.isEmpty()){
            button->setEnabled(false);
            button->setToolTip(QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_INPUTS_OUTSIDE),
                                           "Move these inputs in the Stacking settings first: %1")).arg(conflicts));
        }

        layout.addWidget(button);
        contentHeight += button->sizeHint().height() + 2;
        connect(button, SIGNAL(toggled(bool)), this, SLOT(on_stacking_count_changed()));
        _stacking_count_buttons.push_back(std::make_pair(button, mode.channel_count));
    }

    const QString master_role = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MASTER), "Master");
    const QString secondary_role = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY), "Secondary");

    for (int analyzer = 0; analyzer < _stacking.analyzer_count(); analyzer++){
        QLabel *title = new QLabel(QString("%1 (%2)").arg(pv::LogicStackingConfig::analyzer_label(analyzer),
                                                          analyzer == 0 ? master_role : secondary_role));
        title->setFont(font);
        layout.addWidget(title);
        contentHeight += title->sizeHint().height() + 6;

        QWidget *channel_panel = new QWidget();
        QGridLayout *channel_grid = new QGridLayout();
        channel_grid->setContentsMargins(0, 0, 0, 0);
        channel_grid->setSpacing(0);
        channel_panel->setLayout(channel_grid);

        std::vector<QCheckBox*> boxes;
        int line_height = 0;
        for (int input = 0; input < pv::LogicStackingChannelsPerAnalyzer; input++){
            ChannelLabel *item = new ChannelLabel(this, NULL,
                pv::LogicStackingConfig::global_channel_index(analyzer, input));
            item->getLabel()->setText(QString::number(input));
            channel_grid->addWidget(item, input / 16, input % 16, Qt::AlignCenter);
            boxes.push_back(item->getCheckBox());
            line_height = item->height();
        }

        layout.addWidget(channel_panel);
        contentHeight += line_height * 2 + 2;
        _stacking_boxes.push_back(boxes);
    }

    QWidget *space = new QWidget();
    space->setFixedHeight(10);
    layout.addWidget(space);
    contentHeight += 10;

    QHBoxLayout *line_lay = new QHBoxLayout();
    layout.addLayout(line_lay);
    line_lay->setSpacing(10);
    line_lay->setAlignment(Qt::AlignLeft);

    QPushButton *enable_all_probes = new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE_ALL), "Enable All"));
    QPushButton *disable_all_probes = new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISABLE_ALL), "Disable All"));
    enable_all_probes->setMaximumHeight(33);
    disable_all_probes->setMaximumHeight(33);
    enable_all_probes->setFont(font);
    disable_all_probes->setFont(font);
    int bt_width = std::max(enable_all_probes->sizeHint().width(),
                            disable_all_probes->sizeHint().width());
    enable_all_probes->setFixedWidth(bt_width);
    disable_all_probes->setFixedWidth(bt_width);
    line_lay->addWidget(enable_all_probes);
    line_lay->addWidget(disable_all_probes);
    connect(enable_all_probes, SIGNAL(clicked()), this, SLOT(enable_all_probes()));
    connect(disable_all_probes, SIGNAL(clicked()), this, SLOT(disable_all_probes()));

    this->update_font();
    contentHeight += enable_all_probes->sizeHint().height();

    update_stacking_channels();

    // The group box's title and margins come on top of its content
    _groupHeight2 = std::max(contentHeight + 50, _dynamic_panel->sizeHint().height() + 10);
    _dynamic_panel->setFixedHeight(_groupHeight2);
}

// Boxes follow the edited setup: inputs past the channel count are off, and
// sync and reference inputs are always captured
void DeviceOptions::update_stacking_channels()
{
    const QString sync_input = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SECONDARY_SYNC_INPUT), "Sync input");
    const QString reference_input = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STACKING_REFERENCE_INPUT), "Reference input");

    for (int analyzer = 0; analyzer < (int)_stacking_boxes.size(); analyzer++){
        const uint32_t selected = _stacking.selected_channels(analyzer);
        const uint32_t forced = _stacking.forced_channels(analyzer);
        const bool has_sync = analyzer > 0 && analyzer <= (int)_stacking.secondaries.size();

        for (int input = 0; input < (int)_stacking_boxes[analyzer].size(); input++){
            QCheckBox *box = _stacking_boxes[analyzer][input];
            const bool available = _stacking.channel_available(input);
            const bool is_forced = available && ((forced >> input) & 1) != 0;

            box->setEnabled(available && !is_forced);
            box->setChecked(available && (is_forced || ((selected >> input) & 1) != 0));

            QString tip;
            if (is_forced)
                tip = has_sync && _stacking.secondaries[analyzer - 1].sync_channel == input ?
                      sync_input : reference_input;
            box->setToolTip(tip);
        }
    }
}

void DeviceOptions::on_stacking_count_changed()
{
    for (const auto &entry : _stacking_count_buttons){
        if (entry.first->isChecked())
            _stacking.channel_count = entry.second;
    }

    update_stacking_channels();
}

void DeviceOptions::set_all_probes(bool set)
{ 
    for (auto box : _probes_checkBox_list) {
        box->setCheckState(set ? Qt::Checked : Qt::Unchecked);
    }
}

void DeviceOptions::enable_max_probes() {
    int cur_ch_num = 0; 
    for (auto box : _probes_checkBox_list) {
        if (box->isChecked())
            cur_ch_num++;
    }

    int vld_ch_num;

    if (_device_agent->get_config_int16(SR_CONF_VLD_CH_NUM, vld_ch_num) == false)
        return;

    while(cur_ch_num < vld_ch_num  && cur_ch_num < (int)_probes_checkBox_list.size()) {
        auto box = _probes_checkBox_list[cur_ch_num];
        if (box->isChecked() == false) {
            box->setChecked(true);
            cur_ch_num++;
        } 
    }
}

void DeviceOptions::enable_all_probes()
{   
    bool stream_mode; 

    if (!_stacking_boxes.empty()){
        _stacking.master_channels = pv::LogicStackingAllChannels;
        for (auto &secondary : _stacking.secondaries)
            secondary.channels = pv::LogicStackingAllChannels;
        update_stacking_channels();
        return;
    }

    if (_device_agent->get_config_bool(SR_CONF_STREAM, stream_mode)) {
        if (stream_mode) {
            enable_max_probes();
            return;
        }
    }

	set_all_probes(true);
}

void DeviceOptions::disable_all_probes()
{
    // Sync and reference inputs stay on
    if (!_stacking_boxes.empty()){
        _stacking.master_channels = 0;
        for (auto &secondary : _stacking.secondaries)
            secondary.channels = 0;
        update_stacking_channels();
        return;
    }

	set_all_probes(false);
}

void DeviceOptions::zero_adj()
{
    using namespace Qt;
    QDialog::accept();

    QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_AUTO_CALIB_START), 
                                   "Auto Calibration program will be started. Don't connect any probes. \nIt can take a while!"));
    bool bRet = MsgBox::Confirm(strMsg);

    if (bRet) {
        _device_agent->set_config_bool(SR_CONF_ZERO, true);
    } 
    else {
        _device_agent->set_config_bool(SR_CONF_ZERO, false);
    }
}

void DeviceOptions::on_calibration()
{
    using namespace Qt;
    QDialog::accept();
    _device_agent->set_config_bool(SR_CONF_CALI, true);
}

void DeviceOptions::mode_check_timeout()
{
    if (_isBuilding)
        return;

    if (_device_agent->is_hardware())
    {
        bool test;
        int mode;

        if (_device_agent->get_config_int16(SR_CONF_OPERATION_MODE, mode)) {
            if (mode != _opt_mode) { 
                _opt_mode = mode; 
                build_dynamic_panel();
                try_resize_scroll();
            }
        }

        if (_device_agent->get_config_bool(SR_CONF_TEST, test)) {
            if (test) { 
                for (auto box : _probes_checkBox_list) {
                    box->setCheckState(Qt::Checked);
                    box->setDisabled(true);
                }
            }
        } 
    }
    else if (_device_agent->is_demo())
    {
        QString opt_mode = _device_agent->get_demo_operation_mode();
        if (opt_mode != _demo_operation_mode){
            _demo_operation_mode = opt_mode;
            build_dynamic_panel();
            try_resize_scroll();
        }
    }    
}

void DeviceOptions::channel_check()
{
    QRadioButton* bt = dynamic_cast<QRadioButton*>(sender());
    assert(bt);

    int mode_index = -1;

    for( auto p : _channel_mode_indexs){
        if (p.key == bt){
            mode_index = p.value;
            break;
        }
    }
    assert(mode_index >= 0);
    _device_agent->set_config_int16(SR_CONF_CHANNEL_MODE, mode_index);
  
    build_dynamic_panel();
    try_resize_scroll();
}

void DeviceOptions::analog_channel_check()
{
    QCheckBox* sc=dynamic_cast<QCheckBox*>(sender());
    if(sc != NULL) 
    {
        for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
            sr_channel *const probe = (sr_channel*)l->data;
            
            if (sc->property("index").toInt() == probe->index){
               _device_agent->set_config_bool(SR_CONF_PROBE_MAP_DEFAULT, sc->isChecked(), probe);
            }
        }
    }

    _lst_probe_enabled_status.clear();    
    for (auto ck : _probes_checkBox_list){
        _lst_probe_enabled_status.push_back(ck->isChecked());
    }

    build_dynamic_panel();
    try_resize_scroll();
}

void DeviceOptions::on_analog_channel_enable()
{
    QCheckBox* sc = dynamic_cast<QCheckBox*>(sender());
    channel_checkbox_clicked(sc);
}

void DeviceOptions::channel_checkbox_clicked(QCheckBox *sc)
{
    if (_device_agent->get_work_mode() == LOGIC) {
        if (sc == NULL || !sc->isChecked())
            return;

        bool stream_mode;
        if (_device_agent->get_config_bool(SR_CONF_STREAM, stream_mode) == false)
            return;

        if (!stream_mode)
            return;

        int cur_ch_num = 0; 
        for (auto box : _probes_checkBox_list) {
            if (box->isChecked())
                cur_ch_num++;
        }

        int vld_ch_num;
        if (_device_agent->get_config_int16(SR_CONF_VLD_CH_NUM, vld_ch_num) == false)
            return;

        if (cur_ch_num > vld_ch_num) {
            QString msg_str(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_MAX_CHANNEL_COUNT_WARNING), "max count of channels!"));
            msg_str = msg_str.replace("{0}", QString::number(vld_ch_num) );
            MsgBox::Show(msg_str);

            sc->setChecked(false);
        }
    }
    else if (_device_agent->get_work_mode() == ANALOG) {
        if (sc != NULL) {
            QGridLayout *const layout = (QGridLayout *)sc->property("Layout").value<void *>();
            int i = layout->count();

            int ck_index = -1;
            int i_dex = 0;
            bool map_default = false;
            
            for(auto ck : _probes_checkBox_list){
                if (ck == sc){
                    ck_index = i_dex;
                    break;
                }
                i_dex++;
            }

            if (ck_index != -1)
            {
                _device_agent->get_config_bool(SR_CONF_PROBE_MAP_DEFAULT,
                                map_default, _dso_channel_list[ck_index], NULL);
            }

            while(i--)
            {
                QWidget* w = layout->itemAt(i)->widget();

                if (w->objectName() == "map-enable"){
                    QCheckBox *map_ckbox = dynamic_cast<QCheckBox*>(w);
                    map_ckbox->isChecked();
                }

                if (w->property("Enable").isNull()) {                    

                    if (map_default && w->objectName() == "map-row"){
                        w->setEnabled(false);
                    }
                    else{
                        w->setEnabled(sc->isChecked());
                    }
                }
            }
        }
    }
} 

void DeviceOptions::analog_probes(QGridLayout &layout)
{
    using namespace Qt;
 
    _probes_checkBox_list.clear();
    for(auto p : _probe_options_binding_list) {
        delete p;
    }
    _probe_options_binding_list.clear();
    _dso_channel_list.clear();

    QTabWidget *tabWidget = new QTabWidget();
    tabWidget->setTabPosition(QTabWidget::North);
    tabWidget->setUsesScrollButtons(false);

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);

    int ch_dex = 0;
    
    for (const GSList *l = _device_agent->get_channels(); l; l = l->next) {
        sr_channel *const probe = (sr_channel*)l->data;
        assert(probe);

        _dso_channel_list.push_back(probe);

        QWidget *probe_widget = new QWidget(tabWidget);
        QGridLayout *probe_layout = new QGridLayout(probe_widget);
        probe_widget->setLayout(probe_layout); 

        bool ch_enabled = probe->enabled;
        if ((size_t)ch_dex < _lst_probe_enabled_status.size()){
            ch_enabled = _lst_probe_enabled_status[ch_dex];
        }

        ch_dex++;

        QCheckBox *probe_checkBox = new QCheckBox(this);
        QVariant vlayout = QVariant::fromValue((void *)probe_layout);
        probe_checkBox->setProperty("Layout", vlayout);
        probe_checkBox->setProperty("Enable", true);
        probe_checkBox->setChecked(ch_enabled);
       // probe_checkBox->setCheckState(probe->enabled ? Qt::Checked : Qt::Unchecked);
        _probes_checkBox_list.push_back(probe_checkBox);

        QLabel *en_label = new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ENABLE), "Enable: "), this);
        en_label->setFont(font);
        en_label->setProperty("Enable", true);
        probe_layout->addWidget(en_label, 0, 0, 1, 1);
        probe_layout->addWidget(probe_checkBox, 0, 1, 1, 3);
        
        auto *probe_options_binding = new pv::prop::binding::ProbeOptions(probe);
        const auto &properties = probe_options_binding->properties();
        int i = 1;
        
        for(auto p : properties)
        {
            const QString label = p->labeled_widget() ? QString() : p->label();
            QLabel *lb = new QLabel(label, probe_widget);
            lb->setFont(font);
            probe_layout->addWidget(lb, i, 0, 1, 1);

            QWidget * pow = p->get_widget(probe_widget);
            pow->setEnabled(probe_checkBox->isChecked());
            pow->setFont(font);

            if (p->name().contains("Map Default")) {
                pow->setProperty("index", probe->index);
                connect(pow, SIGNAL(clicked()), this, SLOT(analog_channel_check()));
            }
            else {
                if (probe_checkBox->isChecked() && p->name().contains("Map")) {
                    bool map_default = true;

                    _device_agent->get_config_bool(SR_CONF_PROBE_MAP_DEFAULT, map_default, probe, NULL);

                    if (map_default)
                        pow->setEnabled(false);

                    pow->setObjectName("map-row");
                }
            }
            probe_layout->addWidget(pow, i, 1, 1, 3);
            i++;
        }
        _probe_options_binding_list.push_back(probe_options_binding);

        connect(probe_checkBox, SIGNAL(released()), this, SLOT(on_analog_channel_enable()));

        QString tabName = QString::fromUtf8(probe->name);
        tabName += " ";

        tabWidget->addTab(probe_widget, tabName);
    }

    layout.addWidget(tabWidget, 0, 0, 1, 1);

    this->update_font();
    _groupHeight2 = tabWidget->sizeHint().height() + 50;
    _dynamic_panel->setFixedHeight(_groupHeight2); 

    connect(tabWidget, SIGNAL(currentChanged(int)), this, SLOT(on_anlog_tab_changed(int)));
    tabWidget->setCurrentIndex(_cur_analog_tag_index);
}

void DeviceOptions::on_anlog_tab_changed(int index)
{
    _cur_analog_tag_index = index;
}

QString DeviceOptions::dynamic_widget(QLayout *lay)
 { 
    int mode = _device_agent->get_work_mode();

    if (mode == LOGIC) {
        QVBoxLayout *grid = dynamic_cast<QVBoxLayout*>(lay);
        assert(grid);
        if (_device_agent->is_logic_stacking())
            stacking_probes(*grid);
        else
            logic_probes(*grid);
        //tr
        return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel");
    } 
    else if (mode == DSO) {
        bool have_zero;
      
        if (_device_agent->get_config_bool(SR_CONF_HAVE_ZERO, have_zero)) {
            QGridLayout *grid = dynamic_cast<QGridLayout*>(lay);
            assert(grid);

            QFont font = this->font();
            font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);

            if (have_zero) {
                auto config_button = new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_AUTO_CALIBRATION), "Auto Calibration"), this);
                config_button->setFont(font); 
                grid->addWidget(config_button, 0, 0, 1, 1);
                connect(config_button, SIGNAL(clicked()), this, SLOT(zero_adj()));

                auto cali_button = new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MANUAL_CALIBRATION), "Manual Calibration"), this);
                cali_button->setFont(font);
                grid->addWidget(cali_button, 1, 0, 1, 1);
                connect(cali_button, SIGNAL(clicked()), this, SLOT(on_calibration()));

                config_button->setFixedHeight(35);
                cali_button->setFixedHeight(35);

                _groupHeight2 = 135;
                _dynamic_panel->setFixedHeight(_groupHeight2); 

                //tr
                return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CALIBRATION), "Calibration");
            }
        }
    } 
    else if (mode == ANALOG) {
        QGridLayout *grid = dynamic_cast<QGridLayout*>(lay);
        assert(grid);
        analog_probes(*grid);
        //tr
        return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL), "Channel");
    }
    return NULL;
}

void DeviceOptions::build_dynamic_panel()
{
    _isBuilding = true;

    if (_dynamic_panel != NULL){
        _dynamic_panel->deleteLater();
        _dynamic_panel = NULL;
    }

    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);

    if (_dynamic_panel == NULL)
    {  
        _dynamic_panel = new QGroupBox("group", _dynamic_panel);
        _dynamic_panel->setFont(font);
        _container_lay->addWidget(_dynamic_panel);

        if (_device_agent->get_work_mode() == LOGIC)
            _dynamic_panel->setLayout(new QVBoxLayout());
        else
            _dynamic_panel->setLayout(new QGridLayout());
    }
 
    QString title = dynamic_widget(_dynamic_panel->layout());
    QGroupBox *box = dynamic_cast<QGroupBox*>(_dynamic_panel);
    box->setFont(font);
    box->setTitle(title);

    if (title == ""){ 
        box->setVisible(false);
    }

    _dynamic_panel->layout()->setContentsMargins(5, 20, 5, 5);

    _isBuilding = false;
}

void DeviceOptions::try_resize_scroll()
{
    this->update_font();

    // content area height
    int contentHeight = _groupHeight1 + _groupHeight2 + 20; // +space
    float sk = QGuiApplication::primaryScreen()->logicalDotsPerInch() / 96;

    int srcHeight = 800;
    int w = _width;


#ifdef _WIN32
    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    QFontMetrics fm(font); 

    auto labels = this->findChildren<QLabel*>();
    int max_label_width = 0;
    for(auto o : labels)
    { 
        QRect rc = fm.boundingRect(o->text());
        QSize size(rc.width() + 15, rc.height());         
        o->setFixedSize(size);

        if (size.width() > max_label_width){
            max_label_width = size.width();
        }
    }

    if (_device_agent->get_work_mode() == LOGIC
            && _device_agent->is_demo()){
        _dynamic_panel->setFixedWidth(max_label_width + 250);
    }
#endif

    QScrollArea *scroll = _scroll;
    if (scroll == NULL)
    {
        scroll = new QScrollArea(_scroll_panel);
        scroll->setWidget(_container_panel);
        scroll->setWidgetResizable(true);
        scroll->setStyleSheet("QScrollArea{border:none;}");
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        _scroll_panel->layout()->addWidget(scroll);
        _scroll = scroll;
    }

    // Only set minimum widths and let the layouts stretch the content to the
    // full dialog width, so its right edge lines up with the OK button.
    // Never shrink below an earlier width, the dialog should not jump around
    // when switching modes.
    w = std::max(w, _container_panel->sizeHint().width());
    _width = w;
    contentHeight = std::max(contentHeight, _container_lay->sizeHint().height());
    _container_panel->setMinimumWidth(w);
    _container_panel->setFixedHeight(contentHeight);

    auto set_scroll_size = [&](int sclw, int scrollh){
        _scroll->setMinimumWidth(sclw);
        _scroll->setMaximumWidth(QWIDGETSIZE_MAX);
        _scroll->setFixedHeight(scrollh);
        _scroll_panel->setMinimumWidth(sclw);
        _scroll_panel->setMaximumWidth(QWIDGETSIZE_MAX);
        _scroll_panel->setFixedHeight(scrollh);
    };

    this->setMinimumSize(0, 0);
    this->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

    // Measure the dialog with all content visible; the layout accounts for
    // title bar, spacing and the OK button box.
    set_scroll_size(w, contentHeight);
    QSize hint = this->sizeHint();

    if (sk * hint.height() > srcHeight)
    {
        // Too tall for the screen: shrink the scroll area and make room
        // for the vertical scroll bar.
        int scrollh = srcHeight - (hint.height() - contentHeight);
        set_scroll_size(w + _scroll->verticalScrollBar()->sizeHint().width(), scrollh);
        hint = QSize(this->sizeHint().width(), srcHeight);
    }

    this->setFixedSize(hint);
}

void DeviceOptions::keyPressEvent(QKeyEvent *event) 
{
    if (event->key() == Qt::Key_Escape) {
        event->ignore();
        return;
    }

    QDialog::keyPressEvent(event);
}

} // namespace dialogs
} // namespace pv
