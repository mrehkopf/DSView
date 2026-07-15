/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 * 
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
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

#include "applicationpardlg.h"
#include "dsdialog.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QString>
#include <QFontDatabase>
#include <QGroupBox>
#include <QLabel>
#include <vector>
#include <QGridLayout>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSizePolicy>
#include <QRadioButton>
#include <QButtonGroup>

#include "../config/appconfig.h"
#include "../ui/langresource.h"
#include "../appcontrol.h"
#include "../sigsession.h"
#include "../ui/dscombobox.h"
#include "../log.h"

namespace pv
{
namespace dialogs
{

ApplicationParamDlg::ApplicationParamDlg()
{
   
}

ApplicationParamDlg::~ApplicationParamDlg()
{
}

void ApplicationParamDlg::bind_font_name_list(QComboBox *box, QString v)
{   
    int selDex = -1;

    QString defName(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DEFAULT_FONT), "Default"));
    box->addItem(defName);

    if (_font_name_list.size() == 0)
    {
        QFontDatabase fDataBase;
        _font_name_list = fDataBase.families();
    }
   
    for (QString family : _font_name_list) {
        if (family.indexOf("[") == -1)
        {
            box->addItem(family);

            if (selDex == -1 && family == v){
                selDex = box->count() - 1;
            }
        }
    }

    if (selDex == -1)
        selDex = 0;

    box->setCurrentIndex(selDex);
}

void ApplicationParamDlg::bind_font_size_list(QComboBox *box, float size)
{   
    int selDex = -1;

    float minSize = 0;
    float maxSize = 0;

    AppConfig::GetFontSizeRange(&minSize, &maxSize);

    for(int i=minSize; i<=maxSize; i++)
    {
        box->addItem(QString::number(i));
        if (i == size){
            selDex = box->count() - 1;
        }
    }
    if (selDex == -1)
        selDex = 2;
    box->setCurrentIndex(selDex);
}

bool ApplicationParamDlg::ShowDlg(QWidget *parent)
{
    const int DecoderFontStretchMinimum = 25;
    const int DecoderFontStretchMaximum = 250;

    DSDialog dlg(parent, true, true);
    dlg.setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISPLAY_OPTIONS), "Display options"));

    QVBoxLayout *lay = new QVBoxLayout();
    lay->setContentsMargins(0,10,0,20);
    lay->setSpacing(8);

    //show config
    AppConfig &app = AppConfig::Instance(); 

    QCheckBox *ck_quickScroll = new QCheckBox();
    ck_quickScroll->setChecked(app.appOptions.quickScroll);

    QCheckBox *ck_trigInMid = new QCheckBox();
    ck_trigInMid->setChecked(app.appOptions.trigPosDisplayInMid);

    QCheckBox *ck_profileBar = new QCheckBox();
    ck_profileBar->setChecked(app.appOptions.displayProfileInBar);

    QCheckBox *ck_abortData = new QCheckBox();
    ck_abortData->setChecked(app.appOptions.swapBackBufferAlways);

    QCheckBox *ck_autoScrollLatestData = new QCheckBox();
    ck_autoScrollLatestData->setChecked(app.appOptions.autoScrollLatestData);

    QCheckBox *ck_channelDivider = new QCheckBox();
    ck_channelDivider->setChecked(app.appOptions.logicChannelDivider);

    QHBoxLayout *hl_verticalScrollAction = new QHBoxLayout();
    QButtonGroup *bg_verticalScrollAction = new QButtonGroup();
    QRadioButton *rb_zoom = new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_VERTICAL_SCROLL_ACTION_SMOOTH_ZOOM), "Zoom"));
    QRadioButton *rb_scroll = new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_VERTICAL_SCROLL_ACTION_VERTICAL_SCROLL), "Scroll"));
    rb_zoom->setChecked(app.appOptions.verticalScrollIsZoom);
    rb_scroll->setChecked(!app.appOptions.verticalScrollIsZoom);
    bg_verticalScrollAction->addButton(rb_zoom);
    bg_verticalScrollAction->addButton(rb_scroll);
    hl_verticalScrollAction->addWidget(rb_zoom);
    hl_verticalScrollAction->addWidget(rb_scroll);
    QCheckBox *ck_antialias = new QCheckBox();
    ck_antialias->setChecked(app.appOptions.antialias);

    QCheckBox *ck_dontAskSaveOnExit = new QCheckBox();
    ck_dontAskSaveOnExit->setChecked(app.appOptions.dontAskSaveOnExit);

    QComboBox *ftCbSize = new DsComboBox();
    ftCbSize->setFixedWidth(50);
    bind_font_size_list(ftCbSize, app.appOptions.fontSize);
   
    QHBoxLayout *hl_units = new QHBoxLayout();
    QButtonGroup *bg_timeUnit = new QButtonGroup();
    QRadioButton *rb_timeUnitTime = new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TIME), "Time"));
    QRadioButton *rb_timeUnitSamples = new QRadioButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SAMPLES), "Samples"));
    if (app.appOptions.rulerTimeUnits == RULER_UNIT_TIME) {
        rb_timeUnitTime->setChecked(true);
    } else {
        rb_timeUnitSamples->setChecked(true);
    }
    bg_timeUnit->addButton(rb_timeUnitTime);
    bg_timeUnit->addButton(rb_timeUnitSamples);
    hl_units->addWidget(rb_timeUnitTime);
    hl_units->addWidget(rb_timeUnitSamples);

    bool fontWidthEnabled = app.appOptions.decoderDynamicFontWidth;
    QCheckBox *ck_decoderDynamicFontWidth = new QCheckBox();
    ck_decoderDynamicFontWidth->setChecked(fontWidthEnabled);

    QDoubleSpinBox *spinBox_lineWidth = new QDoubleSpinBox();
    spinBox_lineWidth->setDecimals(1);
    spinBox_lineWidth->setSingleStep(0.5);
    spinBox_lineWidth->setMinimum(1.0);
    spinBox_lineWidth->setMaximum(4.0);
    spinBox_lineWidth->setValue(app.appOptions.logicSignalLineWidth);
    spinBox_lineWidth->setSuffix(" px");

    // Logic group
    QGroupBox *logicGroup = new QGroupBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_GROUP_LOGIC), "Logic"));
    QGridLayout *logicLay = new QGridLayout();

    // slider + input field for min and max font width
    QSlider *slider_minFontWidth = new QSlider(Qt::Horizontal);
    slider_minFontWidth->setMinimum(DecoderFontStretchMinimum);
    slider_minFontWidth->setMaximum(DecoderFontStretchMaximum);
    slider_minFontWidth->setValue(app.appOptions.minDecoderFontWidthPercent);
    slider_minFontWidth->setTickInterval(25);
    slider_minFontWidth->setTickPosition(QSlider::TicksBelow);
    slider_minFontWidth->setFixedWidth(250);
    slider_minFontWidth->setEnabled(fontWidthEnabled);
    QSlider *slider_maxFontWidth = new QSlider(Qt::Horizontal);
    slider_maxFontWidth->setMinimum(DecoderFontStretchMinimum);
    slider_maxFontWidth->setMaximum(DecoderFontStretchMaximum);
    slider_maxFontWidth->setValue(app.appOptions.maxDecoderFontWidthPercent);
    slider_maxFontWidth->setTickInterval(25);
    slider_maxFontWidth->setTickPosition(QSlider::TicksBelow);
    slider_maxFontWidth->setFixedWidth(250);
    slider_maxFontWidth->setEnabled(fontWidthEnabled);
    QSpinBox *spinBox_minFontWidth = new QSpinBox();
    spinBox_minFontWidth->setMinimum(DecoderFontStretchMinimum);
    spinBox_minFontWidth->setMaximum(DecoderFontStretchMaximum);
    spinBox_minFontWidth->setValue(app.appOptions.minDecoderFontWidthPercent);
    spinBox_minFontWidth->setSuffix("%");
    spinBox_minFontWidth->setEnabled(fontWidthEnabled);
    QSpinBox *spinBox_maxFontWidth = new QSpinBox();
    spinBox_maxFontWidth->setMinimum(DecoderFontStretchMinimum);
    spinBox_maxFontWidth->setMaximum(DecoderFontStretchMaximum);
    spinBox_maxFontWidth->setValue(app.appOptions.maxDecoderFontWidthPercent);
    spinBox_maxFontWidth->setSuffix("%");
    spinBox_maxFontWidth->setEnabled(fontWidthEnabled);

    QLabel *label_minFontWidth = new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MIN_FONT_WIDTH), "Min font width:"));
    QLabel *label_maxFontWidth = new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MAX_FONT_WIDTH), "Max font width:"));

    float decoderFontSize = std::min(10.0f, app.appOptions.fontSize);

    QLabel *label_minSample = new QLabel("Example");
    QFont minFont = parent->font();
    minFont.setStretch(app.appOptions.minDecoderFontWidthPercent);
    minFont.setPointSizeF(decoderFontSize);
    label_minSample->setFont(minFont);
    label_minSample->setEnabled(fontWidthEnabled);

    QLabel *label_maxSample = new QLabel("Example");
    QFont maxFont = parent->font();
    maxFont.setStretch(app.appOptions.maxDecoderFontWidthPercent);
    maxFont.setPointSizeF(decoderFontSize);
    label_maxSample->setFont(maxFont);
    label_maxSample->setEnabled(fontWidthEnabled);

    QFont layoutWidthFont = parent->font();
    layoutWidthFont.setStretch(DecoderFontStretchMaximum);
    layoutWidthFont.setPointSizeF(decoderFontSize);
    QFontMetrics wfm(layoutWidthFont);

    QObject::connect(spinBox_minFontWidth, QOverload<int>::of(&QSpinBox::valueChanged), [slider_minFontWidth, slider_maxFontWidth, label_minSample](int value){
        slider_minFontWidth->setValue(value);
        if(value > slider_maxFontWidth->value()) {
            slider_maxFontWidth->setValue(value);
        }
        QFont f = label_minSample->font();
        f.setStretch(value);
        label_minSample->setFont(f);
    });
    QObject::connect(slider_minFontWidth, &QSlider::valueChanged, [spinBox_minFontWidth](int value){
        spinBox_minFontWidth->setValue(value);
    });

    QObject::connect(spinBox_maxFontWidth, QOverload<int>::of(&QSpinBox::valueChanged), [slider_maxFontWidth, slider_minFontWidth, label_maxSample](int value){
        slider_maxFontWidth->setValue(value);
        if(value < slider_minFontWidth->value()) {
            slider_minFontWidth->setValue(value);
        }
        QFont f = label_maxSample->font();
        f.setStretch(value);
        label_maxSample->setFont(f);
    });
    QObject::connect(slider_maxFontWidth, &QSlider::valueChanged, [spinBox_maxFontWidth](int value){
        spinBox_maxFontWidth->setValue(value);
    });

    QObject::connect(ck_decoderDynamicFontWidth, &QCheckBox::stateChanged,
        [
            slider_minFontWidth, slider_maxFontWidth,
            spinBox_minFontWidth, spinBox_maxFontWidth,
            label_minFontWidth, label_maxFontWidth,
            label_minSample, label_maxSample
        ] (int state) {
        bool enabled = (state == Qt::Checked);
        slider_minFontWidth->setEnabled(enabled);
        slider_maxFontWidth->setEnabled(enabled);
        spinBox_minFontWidth->setEnabled(enabled);
        spinBox_maxFontWidth->setEnabled(enabled);
        label_minFontWidth->setEnabled(enabled);
        label_maxFontWidth->setEnabled(enabled);
        label_minSample->setEnabled(enabled);
        label_maxSample->setEnabled(enabled);
    });

    logicLay->setContentsMargins(10,15,15,10);
    logicLay->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    logicGroup->setLayout(logicLay);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_QUICK_SCROLL), "Quick scroll")), 0, 0, Qt::AlignLeft); 
    logicLay->addWidget(ck_quickScroll, 0, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_USE_ABORT_DATA_REPEAT), "Used abort data")), 1, 0, Qt::AlignLeft); 
    logicLay->addWidget(ck_abortData, 1, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_AUTO_SCROLL_LATEAST_DATA), "Auto scoll latest")), 2, 0, Qt::AlignLeft); 
    logicLay->addWidget(ck_autoScrollLatestData, 2, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_VERTICAL_SCROLL_ACTION), "Vertical Scroll Action")), 3, 0, Qt::AlignLeft);
    logicLay->addLayout(hl_verticalScrollAction, 3, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RULER_UNITS), "Ruler / Cursor units")), 4, 0, Qt::AlignLeft);
    logicLay->addLayout(hl_units, 4, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SIGNAL_LINE_WIDTH), "Signal line width")), 5, 0, Qt::AlignLeft);
    logicLay->addWidget(spinBox_lineWidth, 5, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL_DIVIDER), "Channel divider line")), 6, 0, Qt::AlignLeft);
    logicLay->addWidget(ck_channelDivider, 6, 1, Qt::AlignRight);

    // Add sliders to logic layout
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DECODER_DYNAMIC_FONT_WIDTH), "Decoder adaptive font width")), 7, 0, Qt::AlignLeft);
    logicLay->addWidget(ck_decoderDynamicFontWidth, 7, 1, Qt::AlignRight);
    logicLay->addWidget(label_minFontWidth, 8, 0, Qt::AlignLeft);
    logicLay->addWidget(spinBox_minFontWidth, 8, 1, Qt::AlignRight);
    logicLay->addWidget(slider_minFontWidth, 9, 0, Qt::AlignJustify);
    logicLay->addWidget(label_minSample, 9, 1, Qt::AlignCenter);
    logicLay->addWidget(label_maxFontWidth, 10, 0, Qt::AlignLeft);
    logicLay->addWidget(spinBox_maxFontWidth, 10, 1, Qt::AlignRight);
    logicLay->addWidget(slider_maxFontWidth, 11, 0, Qt::AlignJustify);
    logicLay->addWidget(label_maxSample, 11, 1, Qt::AlignCenter);
    logicLay->setColumnMinimumWidth(1, wfm.horizontalAdvance("Example")
        + logicLay->contentsMargins().left()
        + logicLay->contentsMargins().right()
        + 1 );
    lay->addWidget(logicGroup);

    //Scope group
    QGroupBox *dsoGroup = new QGroupBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_GROUP_DSO), "Scope"));
    QGridLayout *dsoLay = new QGridLayout();
    dsoLay->setContentsMargins(10,15,15,10);
    dsoLay->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    dsoGroup->setLayout(dsoLay);
    dsoLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIG_DISPLAY_MIDDLE), "Tig pos in middle")), 0, 0, Qt::AlignLeft);
    dsoLay->addWidget(ck_trigInMid, 0, 1, Qt::AlignRight);
    lay->addWidget(dsoGroup);

    //UI
    QGroupBox *uiGroup = new QGroupBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_GROUP_UI), "UI"));
    QGridLayout *uiLay = new QGridLayout();
    uiLay->setContentsMargins(10,15,15,10);
    uiLay->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    uiGroup->setLayout(uiLay);
    uiLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISPLAY_PROFILE_IN_BAR), "Profile in bar")), 0, 0, Qt::AlignLeft);
    uiLay->addWidget(ck_profileBar, 0, 1, Qt::AlignRight);
    uiLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_FONT_SIZE), "Font size")), 1, 0, Qt::AlignLeft);
    uiLay->addWidget(ftCbSize, 1, 1, Qt::AlignRight);
    uiLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISPLAY_ANTIALIAS), "Antialiasing")), 2, 0, Qt::AlignLeft);
    uiLay->addWidget(ck_antialias, 2, 1, Qt::AlignRight);
    uiLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DONT_ASK_SAVE_ON_EXIT), "Do not ask to save captured data")), 3, 0, Qt::AlignLeft);
    uiLay->addWidget(ck_dontAskSaveOnExit, 3, 1, Qt::AlignRight);
    lay->addWidget(uiGroup);

    dlg.layout()->addLayout(lay);      
    dlg.layout()->setSizeConstraint(QLayout::SetFixedSize);
    // prevent dlg.exec()'s implicit font update from resetting preview font stretches
    label_minSample->setObjectName("__LOCKED_FONT_MINSAMPLE");
    label_maxSample->setObjectName("__LOCKED_FONT_MAXSAMPLE");
    dlg.exec();
    bool ret = dlg.IsClickYes();

    //save config
    if (ret){

        bool bAppChanged = false;
        bool bFontChanged = false;
        float fSize = ftCbSize->currentText().toFloat();

        if (app.appOptions.quickScroll != ck_quickScroll->isChecked()){
            app.appOptions.quickScroll = ck_quickScroll->isChecked();
            bAppChanged = true;
        }       
        if (app.appOptions.trigPosDisplayInMid != ck_trigInMid->isChecked()){
            app.appOptions.trigPosDisplayInMid = ck_trigInMid->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.displayProfileInBar != ck_profileBar->isChecked()){
            app.appOptions.displayProfileInBar = ck_profileBar->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.swapBackBufferAlways != ck_abortData->isChecked()){
            app.appOptions.swapBackBufferAlways = ck_abortData->isChecked();
            bAppChanged = true;
        }        
        if (app.appOptions.fontSize != fSize){
            app.appOptions.fontSize = fSize;
            bFontChanged = true;
        }
        if (app.appOptions.autoScrollLatestData != ck_autoScrollLatestData->isChecked()){
            app.appOptions.autoScrollLatestData = ck_autoScrollLatestData->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.verticalScrollIsZoom != rb_zoom->isChecked()){
            app.appOptions.verticalScrollIsZoom = rb_zoom->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.rulerTimeUnits != (rb_timeUnitTime->isChecked() ? RULER_UNIT_TIME : RULER_UNIT_SAMPLES)){
            app.appOptions.rulerTimeUnits = (rb_timeUnitTime->isChecked() ? RULER_UNIT_TIME : RULER_UNIT_SAMPLES);
            bAppChanged = true;
        }
        if (app.appOptions.antialias != ck_antialias->isChecked()){
            app.appOptions.antialias = ck_antialias->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.dontAskSaveOnExit != ck_dontAskSaveOnExit->isChecked()){
            app.appOptions.dontAskSaveOnExit = ck_dontAskSaveOnExit->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.decoderDynamicFontWidth != ck_decoderDynamicFontWidth->isChecked()) {
            app.appOptions.decoderDynamicFontWidth = ck_decoderDynamicFontWidth->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.minDecoderFontWidthPercent != slider_minFontWidth->value()) {
            app.appOptions.minDecoderFontWidthPercent = slider_minFontWidth->value();
            bAppChanged = true;
        }
        if (app.appOptions.maxDecoderFontWidthPercent != slider_maxFontWidth->value()) {
            app.appOptions.maxDecoderFontWidthPercent = slider_maxFontWidth->value();
            bAppChanged = true;
        }
        if (app.appOptions.logicSignalLineWidth != spinBox_lineWidth->value()) {
            app.appOptions.logicSignalLineWidth = spinBox_lineWidth->value();
            bAppChanged = true;
        }
        if (app.appOptions.logicChannelDivider != ck_channelDivider->isChecked()) {
            app.appOptions.logicChannelDivider = ck_channelDivider->isChecked();
            bAppChanged = true;
        }
        if (bAppChanged){
            app.SaveApp();
            AppControl::Instance()->GetSession()->broadcast_msg(DSV_MSG_APP_OPTIONS_CHANGED);
        }
        
        if (bFontChanged){
            if (!bAppChanged){
                app.SaveApp();
            }
            AppControl::Instance()->GetSession()->broadcast_msg(DSV_MSG_FONT_OPTIONS_CHANGED);
        }
    }
   
   return ret;
}
 

} //
}//

