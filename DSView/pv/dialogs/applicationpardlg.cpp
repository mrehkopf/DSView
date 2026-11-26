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
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>
#include <QStyle>
#include <QEvent>

#include "../config/appconfig.h"
#include "../ui/langresource.h"
#include "../appcontrol.h"
#include "../sigsession.h"
#include "../ui/dscombobox.h"
#include "../ui/xspinbox.h"
#include "../log.h"

namespace
{

// A scroll area that asks for the full size of its content. A maximum
// height then makes it scroll instead of growing.
class ContentSizeScrollArea : public QScrollArea
{
public:
    QSize sizeHint() const override
    {
        const int f = 2 * frameWidth();
        QSize sz = widget()->sizeHint() + QSize(f, f);

        if (sz.height() > maximumHeight()){
            sz.rwidth() += verticalScrollBar()->sizeHint().width();
        }
        return sz;
    }
};

// Makes the scroll area shorter when the dialog would be taller than the
// free area of its screen, so that OK and Cancel stay reachable on small or
// highly scaled screens. This runs on the dialog's show event: the layouts
// are set up by then, and the dialog is centred on screen afterwards.
class FitToScreenFilter : public QObject
{
public:
    FitToScreenFilter(QScrollArea *scroll, QWidget *screenRef)
        : _scroll(scroll), _screenRef(screenRef)
    {
    }

    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::Show && obj->isWidgetType()){
            fit(static_cast<QWidget*>(obj));
        }
        return QObject::eventFilter(obj, event);
    }

private:
    void fit(QWidget *dlg)
    {
        // The main window's screen. Not QGuiApplication::screenAt(): global
        // positions are unknown on Wayland.
        QScreen *screen = NULL;
        QWidget *top = _screenRef != NULL ? _screenRef->window() : NULL;
        if (top != NULL && top->windowHandle() != NULL){
            screen = top->windowHandle()->screen();
        }
        if (screen == NULL){
            screen = QGuiApplication::primaryScreen();
        }
        if (screen == NULL){
            return;
        }

        int avail = screen->availableGeometry().height();
        if (!dlg->windowFlags().testFlag(Qt::FramelessWindowHint)){
            // The native title bar is not part of the dialog's height.
            avail -= dlg->style()->pixelMetric(QStyle::PM_TitleBarHeight, NULL, dlg);
        }
        relayout(dlg);
        if (dlg->height() <= avail){
            return;
        }

        const int other = dlg->height() - _scroll->height();
        const int minHeight = 4 * _scroll->fontMetrics().height();
        _scroll->setMaximumHeight(std::max(avail - other, minHeight));
        relayout(dlg);
    }

    // Brings the sizes up to date. The dialog sets its fonts just before it
    // is shown, and the layout requests that follow are only posted, so the
    // layouts still have sizes from the old fonts here.
    void relayout(QWidget *dlg)
    {
        if (_scroll->widget()->layout() != NULL){
            _scroll->widget()->layout()->activate();
        }
        // The scroll area is not told when its content's size changes.
        _scroll->updateGeometry();

        for (QWidget *w = _scroll->parentWidget(); w != NULL; w = w->parentWidget()){
            if (w->layout() != NULL){
                w->layout()->activate();
            }
            if (w == dlg){
                break;
            }
        }
        dlg->adjustSize();
    }

    QScrollArea *_scroll;
    QWidget     *_screenRef;
};

} // namespace

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

    QCheckBox *ck_timeBar = new QCheckBox();
    ck_timeBar->setChecked(app.appOptions.showTimeBar);

    QCheckBox *ck_cursorDeltaBand = new QCheckBox();
    ck_cursorDeltaBand->setChecked(app.appOptions.cursorDeltaBand);

    QCheckBox *ck_thickHighLevel = new QCheckBox();
    ck_thickHighLevel->setChecked(app.appOptions.logicThickHighLevel);

    QCheckBox *ck_snapVScroll = new QCheckBox();
    ck_snapVScroll->setChecked(app.appOptions.logicSnapVScroll);

    QCheckBox *ck_logicSampleDots = new QCheckBox();
    ck_logicSampleDots->setChecked(app.appOptions.logicSampleDots);

    QCheckBox *ck_dsoSampleDots = new QCheckBox();
    ck_dsoSampleDots->setChecked(app.appOptions.dsoSampleDots);

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

    // Toolbar icon size in px, 0 = the style's default.
    QComboBox *cbIconSize = new DsComboBox();
    cbIconSize->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    cbIconSize->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ICON_SIZE_AUTO), "Automatic"), 0);
    for (int size : {24, 32, 40, 48}){
        cbIconSize->addItem(QString("%1 px").arg(size), size);
    }
    if (cbIconSize->findData(app.appOptions.toolbarIconSize) < 0){
        // A size set in the config file by hand.
        cbIconSize->addItem(QString("%1 px").arg(app.appOptions.toolbarIconSize),
                            app.appOptions.toolbarIconSize);
    }
    cbIconSize->setCurrentIndex(cbIconSize->findData(app.appOptions.toolbarIconSize));
   
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

    QDoubleSpinBox *spinBox_lineWidth = new XDoubleSpinBox();
    spinBox_lineWidth->setDecimals(1);
    spinBox_lineWidth->setSingleStep(0.5);
    spinBox_lineWidth->setMinimum(1.0);
    spinBox_lineWidth->setMaximum(4.0);
    spinBox_lineWidth->setValue(app.appOptions.logicSignalLineWidth);
    spinBox_lineWidth->setSuffix(" px");

    QDoubleSpinBox *spinBox_dsoLineWidth = new XDoubleSpinBox();
    spinBox_dsoLineWidth->setDecimals(1);
    spinBox_dsoLineWidth->setSingleStep(0.5);
    spinBox_dsoLineWidth->setMinimum(1.0);
    spinBox_dsoLineWidth->setMaximum(4.0);
    spinBox_dsoLineWidth->setValue(app.appOptions.dsoSignalLineWidth);
    spinBox_dsoLineWidth->setSuffix(" px");

    QSpinBox *spinBox_dsoPersistence = new XSpinBox();
    spinBox_dsoPersistence->setRange(0, 10000);
    spinBox_dsoPersistence->setSingleStep(100);
    spinBox_dsoPersistence->setSpecialValueText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_OFF), "Off"));
    spinBox_dsoPersistence->setValue(app.appOptions.dsoPersistenceMs);
    spinBox_dsoPersistence->setSuffix(" ms");

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
    QSpinBox *spinBox_minFontWidth = new XSpinBox();
    spinBox_minFontWidth->setMinimum(DecoderFontStretchMinimum);
    spinBox_minFontWidth->setMaximum(DecoderFontStretchMaximum);
    spinBox_minFontWidth->setValue(app.appOptions.minDecoderFontWidthPercent);
    spinBox_minFontWidth->setSuffix("%");
    spinBox_minFontWidth->setEnabled(fontWidthEnabled);
    QSpinBox *spinBox_maxFontWidth = new XSpinBox();
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
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_AUTO_SCROLL_LATEAST_DATA), "Auto scroll to latest data")), 2, 0, Qt::AlignLeft); 
    logicLay->addWidget(ck_autoScrollLatestData, 2, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_VERTICAL_SCROLL_ACTION), "Vertical Scroll Action")), 3, 0, Qt::AlignLeft);
    logicLay->addLayout(hl_verticalScrollAction, 3, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SNAP_VSCROLL), "Snap scrolling to channels")), 4, 0, Qt::AlignLeft);
    logicLay->addWidget(ck_snapVScroll, 4, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RULER_UNITS), "Ruler / Cursor units")), 5, 0, Qt::AlignLeft);
    logicLay->addLayout(hl_units, 5, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SIGNAL_LINE_WIDTH), "Signal line width")), 6, 0, Qt::AlignLeft);
    logicLay->addWidget(spinBox_lineWidth, 6, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_THICK_HIGH_LEVEL), "Thicker high level")), 7, 0, Qt::AlignLeft);
    logicLay->addWidget(ck_thickHighLevel, 7, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SAMPLE_DOTS), "Show sample dots")), 8, 0, Qt::AlignLeft);
    logicLay->addWidget(ck_logicSampleDots, 8, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHANNEL_DIVIDER), "Channel divider line")), 9, 0, Qt::AlignLeft);
    logicLay->addWidget(ck_channelDivider, 9, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TIME_BAR), "Show time bar")), 10, 0, Qt::AlignLeft);
    logicLay->addWidget(ck_timeBar, 10, 1, Qt::AlignRight);
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CURSOR_DELTA_BAND), "Shade cursor pairs (1-2, 3-4, ...)")), 11, 0, Qt::AlignLeft);
    logicLay->addWidget(ck_cursorDeltaBand, 11, 1, Qt::AlignRight);

    // Add sliders to logic layout
    logicLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DECODER_DYNAMIC_FONT_WIDTH), "Decoder adaptive font width")), 12, 0, Qt::AlignLeft);
    logicLay->addWidget(ck_decoderDynamicFontWidth, 12, 1, Qt::AlignRight);
    logicLay->addWidget(label_minFontWidth, 13, 0, Qt::AlignLeft);
    logicLay->addWidget(spinBox_minFontWidth, 13, 1, Qt::AlignRight);
    logicLay->addWidget(slider_minFontWidth, 14, 0, Qt::AlignJustify);
    logicLay->addWidget(label_minSample, 14, 1, Qt::AlignCenter);
    logicLay->addWidget(label_maxFontWidth, 15, 0, Qt::AlignLeft);
    logicLay->addWidget(spinBox_maxFontWidth, 15, 1, Qt::AlignRight);
    logicLay->addWidget(slider_maxFontWidth, 16, 0, Qt::AlignJustify);
    logicLay->addWidget(label_maxSample, 16, 1, Qt::AlignCenter);
    logicLay->setColumnMinimumWidth(1, wfm.horizontalAdvance("Example")
        + logicLay->contentsMargins().left()
        + logicLay->contentsMargins().right()
        + 1 );
    lay->addWidget(logicGroup);

    //Scope group
    QGroupBox *dsoGroup = new QGroupBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_GROUP_DSO), "Scope"));
    QGridLayout *dsoLay = new QGridLayout();
    dsoLay->setContentsMargins(10,15,15,10);
    dsoLay->setAlignment(Qt::AlignTop);
    dsoLay->setColumnStretch(0, 1);
    dsoGroup->setLayout(dsoLay);
    dsoLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIG_DISPLAY_MIDDLE), "Show trigger position in the middle")), 0, 0, Qt::AlignLeft);
    dsoLay->addWidget(ck_trigInMid, 0, 1, Qt::AlignRight);
    dsoLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SIGNAL_LINE_WIDTH), "Signal line width")), 1, 0, Qt::AlignLeft);
    dsoLay->addWidget(spinBox_dsoLineWidth, 1, 1, Qt::AlignRight);
    dsoLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SAMPLE_DOTS), "Show sample dots")), 2, 0, Qt::AlignLeft);
    dsoLay->addWidget(ck_dsoSampleDots, 2, 1, Qt::AlignRight);
    dsoLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DSO_PERSISTENCE), "Persistence (fade-out time)")), 3, 0, Qt::AlignLeft);
    dsoLay->addWidget(spinBox_dsoPersistence, 3, 1, Qt::AlignRight);
    lay->addWidget(dsoGroup);

    //UI
    QGroupBox *uiGroup = new QGroupBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_GROUP_UI), "UI"));
    QGridLayout *uiLay = new QGridLayout();
    uiLay->setContentsMargins(10,15,15,10);
    uiLay->setAlignment(Qt::AlignTop);
    uiLay->setColumnStretch(0, 1);
    uiGroup->setLayout(uiLay);
    uiLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISPLAY_PROFILE_IN_BAR), "Profile in bar")), 0, 0, Qt::AlignLeft);
    uiLay->addWidget(ck_profileBar, 0, 1, Qt::AlignRight);
    uiLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_FONT_SIZE), "Font size")), 1, 0, Qt::AlignLeft);
    uiLay->addWidget(ftCbSize, 1, 1, Qt::AlignRight);
    uiLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TOOLBAR_ICON_SIZE), "Toolbar icon size")), 2, 0, Qt::AlignLeft);
    uiLay->addWidget(cbIconSize, 2, 1, Qt::AlignRight);
    uiLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISPLAY_ANTIALIAS), "Antialiasing")), 3, 0, Qt::AlignLeft);
    uiLay->addWidget(ck_antialias, 3, 1, Qt::AlignRight);
    uiLay->addWidget(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DONT_ASK_SAVE_ON_EXIT), "Do not ask to save captured data")), 4, 0, Qt::AlignLeft);
    uiLay->addWidget(ck_dontAskSaveOnExit, 4, 1, Qt::AlignRight);
    lay->addWidget(uiGroup);

    // The groups scroll when the dialog would not fit on the screen.
    QWidget *content = new QWidget();
    content->setLayout(lay);
    ContentSizeScrollArea *scroll = new ContentSizeScrollArea();
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    FitToScreenFilter fitFilter(scroll, parent);
    dlg.installEventFilter(&fitFilter);

    dlg.layout()->addWidget(scroll);
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
        if (app.appOptions.toolbarIconSize != cbIconSize->currentData().toInt()){
            app.appOptions.toolbarIconSize = cbIconSize->currentData().toInt();
            bAppChanged = true;
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
        if (app.appOptions.dsoSignalLineWidth != spinBox_dsoLineWidth->value()) {
            app.appOptions.dsoSignalLineWidth = spinBox_dsoLineWidth->value();
            bAppChanged = true;
        }
        if (app.appOptions.dsoPersistenceMs != spinBox_dsoPersistence->value()) {
            app.appOptions.dsoPersistenceMs = spinBox_dsoPersistence->value();
            bAppChanged = true;
        }
        if (app.appOptions.logicChannelDivider != ck_channelDivider->isChecked()) {
            app.appOptions.logicChannelDivider = ck_channelDivider->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.showTimeBar != ck_timeBar->isChecked()) {
            app.appOptions.showTimeBar = ck_timeBar->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.cursorDeltaBand != ck_cursorDeltaBand->isChecked()) {
            app.appOptions.cursorDeltaBand = ck_cursorDeltaBand->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.logicThickHighLevel != ck_thickHighLevel->isChecked()) {
            app.appOptions.logicThickHighLevel = ck_thickHighLevel->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.logicSnapVScroll != ck_snapVScroll->isChecked()) {
            app.appOptions.logicSnapVScroll = ck_snapVScroll->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.logicSampleDots != ck_logicSampleDots->isChecked()) {
            app.appOptions.logicSampleDots = ck_logicSampleDots->isChecked();
            bAppChanged = true;
        }
        if (app.appOptions.dsoSampleDots != ck_dsoSampleDots->isChecked()) {
            app.appOptions.dsoSampleDots = ck_dsoSampleDots->isChecked();
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

