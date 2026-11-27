/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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

#include "protocolexp.h"
  
#include <QFormLayout>
#include <QListWidget>
#include <QFile>
#include <QFileDialog>
#include <QTextStream>
#include <QProgressDialog>
#include <QFuture>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>

#include "../sigsession.h"
#include "../data/decoderstack.h"
#include "../data/decode/row.h"
#include "../data/decode/annotation.h"
#include "../view/decodetrace.h"
#include "../view/view.h"
#include "../view/cursor.h"
#include <QHBoxLayout>
#include "../data/decodermodel.h"
#include "../config/appconfig.h"
#include "../dsvdef.h"
#include "../utility/encoding.h"
#include "../utility/path.h"
#include "../log.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"
 #include "../utility/formatting.h"

#define EXPORT_DEC_ROW_COUNT_MAX 20

using namespace pv::data::decode;

namespace pv {
namespace dialogs {

// Quotes a field (RFC 4180) when it holds the separator, a quote or a
// line break, so that annotation texts like "a, b" stay in one column.
static QString csv_field(const QString &field, const QString &separator)
{
    if (field.contains(separator) || field.contains('"')
        || field.contains('\n') || field.contains('\r'))
    {
        QString quoted = field;
        quoted.replace("\"", "\"\"");
        return "\"" + quoted + "\"";
    }
    return field;
}

ProtocolExp::ProtocolExp(QWidget *parent, SigSession *session, view::View *view) :
    DSDialog(parent),
    _session(session),
    _button_box(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        Qt::Horizontal, this),
    _export_cancel(false)
{
    _bAbleClose = false;
    _use_range = false;
    _range_start = 0;
    _range_end = 0;

    _format_combobox = new DsComboBox(this);
    //tr
    _format_combobox->addItem("Comma-Separated Values (*.csv)");
    _format_combobox->addItem("Text files (*.txt)");

    _flayout = new QFormLayout();
    _flayout->setVerticalSpacing(5);
    _flayout->setFormAlignment(Qt::AlignLeft);
    _flayout->setLabelAlignment(Qt::AlignLeft);
    _flayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    _flayout->addRow(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_EXPORT_FORMAT), "Export Format: "), this), _format_combobox);

    // The column separator; the item data is the separator itself.
    _separator_combobox = new DsComboBox(this);
    _separator_combobox->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEPARATOR_COMMA), "Comma ( , )"), QString(","));
    _separator_combobox->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEPARATOR_SEMICOLON), "Semicolon ( ; )"), QString(";"));
    _separator_combobox->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEPARATOR_TAB), "Tab"), QString("\t"));
    _separator_combobox->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEPARATOR_PIPE), "Pipe ( | )"), QString("|"));
    _separator_combobox->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEPARATOR_SPACE), "Space"), QString(" "));

    const int sep_index = _separator_combobox->findData(AppConfig::Instance().userHistory.protocolExportSeparator);
    _separator_combobox->setCurrentIndex(sep_index >= 0 ? sep_index : 0);
    _flayout->addRow(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_EXPORT_SEPARATOR), "Separator: "), this), _separator_combobox);

    // Only the annotations that start between two cursors, listed by their
    // numbers on the ruler; the item data is the cursor's sample.
    _range_checkbox = new QCheckBox(this);
    _range_from_combobox = new DsComboBox(this);
    _range_to_combobox = new DsComboBox(this);

    if (view != NULL) {
        for (auto cursor : view->get_cursorList()) {
            const QVariant sample = QVariant::fromValue<qulonglong>(cursor->index());
            _range_from_combobox->addItem(QString::number(cursor->order()), sample);
            _range_to_combobox->addItem(QString::number(cursor->order()), sample);
        }
    }

    QWidget *range_widget = new QWidget(this);
    QHBoxLayout *range_layout = new QHBoxLayout(range_widget);
    range_layout->setContentsMargins(0, 0, 0, 0);
    range_layout->addWidget(_range_checkbox);
    range_layout->addWidget(_range_from_combobox);
    range_layout->addWidget(new QLabel("-", this));
    range_layout->addWidget(_range_to_combobox);
    range_layout->addStretch(1);

    if (_range_from_combobox->count() >= 2) {
        _range_to_combobox->setCurrentIndex(1);
        _range_from_combobox->setEnabled(false);
        _range_to_combobox->setEnabled(false);
        connect(_range_checkbox, &QCheckBox::toggled, this, [this](bool on) {
            _range_from_combobox->setEnabled(on);
            _range_to_combobox->setEnabled(on);
        });
    }
    else {
        range_widget->setEnabled(false);
        range_widget->setToolTip(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_EXPORT_RANGE_NEED_CURSORS),
                                     "Place at least two cursors to use this."));
    }
    _flayout->addRow(new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_EXPORT_BETWEEN_CURSORS), "Only between cursors: "), this), range_widget);

    pv::data::DecoderModel* decoder_model = _session->get_decoder_model();

    const auto decoder_stack = decoder_model->getDecoderStack();
    if (decoder_stack) {
        int row_index = 0;
        auto rows = decoder_stack->get_rows_lshow();

        for (auto i = rows.begin();i != rows.end(); i++) {
            if ((*i).second) {
                QLabel *row_label = new QLabel((*i).first.title(), this);
                QCheckBox *row_sel = new QCheckBox(this);
                if (row_index == 0) {
                    row_sel->setChecked(true);
                }
                _row_label_list.push_back(row_label);
                _row_sel_list.push_back(row_sel);
                _flayout->addRow(row_label, row_sel);
                row_sel->setProperty("index", row_index);
                row_sel->setProperty("title", (*i).first.title());
                row_index++;
            }
        }
    }

    _layout = new QVBoxLayout();
    _layout->addLayout(_flayout);
    _layout->addWidget(&_button_box);

    layout()->addLayout(_layout);
    setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PROTOCOL_EXPORT), "Protocol Export"));

    connect(&_button_box, SIGNAL(accepted()), this, SLOT(onAccept()));
    connect(&_button_box, SIGNAL(rejected()), this, SLOT(onReject()));
    connect(_session->device_event_object(), SIGNAL(device_updated()), this, SLOT(onReject()));

    connect(&m_timer, &QTimer::timeout, this, &ProtocolExp::on_timeout);
    m_timer.setInterval(200);

}

void ProtocolExp::on_timeout()
{
    if (_bAbleClose){
        closeSelf();
    }
}

void ProtocolExp::onReject()
{
    using namespace Qt;

    QDialog::reject();
    this->deleteLater();
}

void ProtocolExp::cancel_export()
{
    _export_cancel = true;
}

void ProtocolExp::closeSelf()
{
    close();
    this->deleteLater();
}

void ProtocolExp::onAccept()
{
    if (_session->have_decoded_result() == false 
            || _row_sel_list.empty())
    {
        QString errMsg = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NO_DECODED_RESULT), "No data to export");
        MsgBox::Show(errMsg);
        return;
    }
 
    QList<QString> supportedFormats;
    for (int i = _format_combobox->count() - 1; i >= 0; i--)
    {
        supportedFormats.push_back(_format_combobox->itemText(i));
    }

    QString filter;
    for (int i = 0; i < supportedFormats.count(); i++)
    {
        filter.append(supportedFormats[i]);
        if (i < supportedFormats.count() - 1)
            filter.append(";;");
    }

    AppConfig &app = AppConfig::Instance();
    QString default_filter = _format_combobox->currentText();
    QString default_name = app.userHistory.protocolExportPath + "/" + "decoder-";

    QString dateTimeString = Formatting::DateTimeToString(_session->get_session_time(), TimeStrigFormatType::TIME_STR_FORMAT_SHORT2);
    default_name += "-" + dateTimeString;

    QString file_name = QFileDialog::getSaveFileName(
        this,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_EXPORT_DATA), "Export Data"),
        default_name, filter,
        &default_filter);

    if (file_name == ""){
        return;
    }

    QFileInfo f(file_name);
    QStringList list = default_filter.split('.').last().split(')');
    QString ext = list.first();
    if (f.suffix().compare(ext))
        //tr
        file_name += "." + ext;

    _separator = _separator_combobox->currentData().toString();
    if (_separator.isEmpty())
        _separator = ",";

    _use_range = _range_checkbox->isEnabled() && _range_checkbox->isChecked();
    if (_use_range) {
        const uint64_t a = _range_from_combobox->currentData().toULongLong();
        const uint64_t b = _range_to_combobox->currentData().toULongLong();
        _range_start = std::min(a, b);
        _range_end = std::max(a, b);
    }

    bool save_history = false;
    QString fname = path::GetDirectoryName(file_name);
    if (fname != app.userHistory.openDir)
    {
        app.userHistory.protocolExportPath = fname;
        save_history = true;
    }
    if (_separator != app.userHistory.protocolExportSeparator)
    {
        app.userHistory.protocolExportSeparator = _separator;
        save_history = true;
    }
    if (save_history)
        app.SaveHistory();
    _fileName = file_name;
 
    QFuture<void> future;
    future = QtConcurrent::run([&]{
                    save_proc();
                    _bAbleClose = true;
               });

    Qt::WindowFlags flags = Qt::CustomizeWindowHint;
    QProgressDialog dlg(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_EXPORT_PROTOCOL_LIST_RESULT), 
                        "Export Protocol List Result... It can take a while."),
                        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CANCEL), "Cancel"), 0, 100, this, flags);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint |
                       Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint);

    QFutureWatcher<void> watcher;

    connect(&watcher, SIGNAL(finished()), &dlg, SLOT(cancel()));
    connect(this, SIGNAL(export_progress(int)), &dlg, SLOT(setValue(int)));
    connect(&dlg, SIGNAL(canceled()), this, SLOT(cancel_export()));

    m_timer.start();
    this->hide();
    watcher.setFuture(future);
    dlg.exec();

    future.waitForFinished();   
}

void ProtocolExp::save_proc()
{
    _export_cancel = false;

    QFile file(_fileName);
    file.open(QIODevice::WriteOnly | QIODevice::Text);
    QTextStream out(&file);
    encoding::set_utf8(out);
    // out.setGenerateByteOrderMark(true); // UTF-8 without BOM
    int row_num = 0;
    ExportRowInfo row_inf_arr[EXPORT_DEC_ROW_COUNT_MAX];
    std::vector<Annotation*> annotations_arr[EXPORT_DEC_ROW_COUNT_MAX];

    for (std::list<QCheckBox *>::const_iterator i = _row_sel_list.begin();
         i != _row_sel_list.end(); i++)
    {
        if ((*i)->isChecked())
        {
            row_inf_arr[row_num].title = (*i)->property("title").toString();
            row_inf_arr[row_num].row_index = (*i)->property("index").toULongLong();
            row_num++;

            if (row_num == EXPORT_DEC_ROW_COUNT_MAX)
                break;
        }
    }

    if (row_num == 0){
        dsv_info("ERROR: There have no decode data row to export.");
        return;
    }

    pv::data::DecoderModel *decoder_model = _session->get_decoder_model();
    const auto decoder_stack = decoder_model->getDecoderStack();
    
    int fd_row_dex = 0;
    const std::map<const Row, bool> rows_lshow = decoder_stack->get_rows_lshow();
    for (auto it = rows_lshow.begin();it != rows_lshow.end(); it++)
    {
        if ((*it).second)
        {
            for (int i=0; i<row_num; i++) {
                if (row_inf_arr[i].row_index == fd_row_dex){
                    row_inf_arr[i].row = &(*it).first;
                    break;
                }
            }      
            fd_row_dex++;
        }
    }

    //get annotation list
    uint64_t total_ann_count = 0;

    for (int i=0; i<row_num; i++)
    {
        decoder_stack->get_annotation_subset(annotations_arr[i], *row_inf_arr[i].row,
                                         0, decoder_stack->sample_count() - 1);

        // Between the cursors: the annotations that start there, as the
        // time column gives the start.
        if (_use_range) {
            auto &anns = annotations_arr[i];
            anns.erase(std::remove_if(anns.begin(), anns.end(), [this](Annotation *a) {
                           return a->start_sample() < _range_start || a->start_sample() > _range_end;
                       }), anns.end());
        }
        total_ann_count += (uint64_t)annotations_arr[i].size();
        sort(annotations_arr[i].begin(), annotations_arr[i].end(), compare_ann_index);  
        row_inf_arr[i].read_index = 0;
    }

    //title
    QString title_str;

    for (int i=0; i<row_num; i++) {
        if (i > 0 && i < row_num){
            title_str.append(_separator);
        }
        title_str.append(csv_field(row_inf_arr[i].title, _separator));
    }

    out << "Id" << _separator << "Time[ns]" << _separator << title_str << "\n";

    uint64_t write_row_dex = 0;
    uint64_t write_ann_num = 0;
    double ns_per_sample = SR_SEC(1) * 1.0 / decoder_stack->samplerate();
    uint64_t sample_index = 0;
    uint64_t sample_index1 = 0;

    while (write_ann_num < total_ann_count && !_export_cancel)
    {    
        bool bFirtColumn = true;

        for (int i=0; i<row_num; i++)
        {   
            if (row_inf_arr[i].read_index >= annotations_arr[i].size())
                continue;
            
            Annotation *ann = annotations_arr[i].at(row_inf_arr[i].read_index);
            sample_index1 = ann->start_sample();

            if (bFirtColumn || sample_index1 < sample_index){
                sample_index = sample_index1;
                bFirtColumn = false;
            }
        }

        QString ann_row_str;

        for (int i=0; i<row_num; i++)
        {   
            if (i > 0 && i < row_num){
                ann_row_str.append(_separator);
            }

            if (row_inf_arr[i].read_index >= annotations_arr[i].size())
                continue;
            
            Annotation *ann = annotations_arr[i].at(row_inf_arr[i].read_index);           

            if (ann->start_sample() == sample_index){
                ann_row_str.append(csv_field(ann->annotations().at(0), _separator));
                row_inf_arr[i].read_index++;
                write_ann_num++;
            }
        }

        write_row_dex++;

        out << QString::number(write_row_dex) << _separator
            << QString::number(sample_index * ns_per_sample, 'f', 2) << _separator
            << ann_row_str << "\n";

        emit export_progress(write_ann_num * 100 / total_ann_count);
    }

    file.close();
}

bool ProtocolExp::compare_ann_index(const data::decode::Annotation *a, 
                    const data::decode::Annotation *b)
{   
    assert(a);
    assert(b);
    return a->start_sample() < b->start_sample();
}

} // namespace dialogs
} // namespace pv
