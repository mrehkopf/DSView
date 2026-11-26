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

#include "protocoldock.h"
#include "../sigsession.h"
#include "../view/decodetrace.h"
#include "../data/decodermodel.h"
#include "../data/decoderstack.h"
#include "../dialogs/protocollist.h"
#include "../dialogs/protocolexp.h" 
#include "../view/view.h"

#include <QObject>
#include <QHBoxLayout>
#include <QPainter>
#include <QFormLayout>
#include <QStandardItemModel>
#include <QTableView>
#include <QHeaderView>
#include <QScrollBar>
#include <QRegularExpression>
#include <QFuture>
#include <QProgressDialog>
#include <QtConcurrent/QtConcurrent>
#include <QSizePolicy>
#include <assert.h>
#include <map>
#include <string> 
#include <algorithm>
#include <QTableWidgetItem>
#include <QHeaderView>
#include "../ui/msgbox.h"
#include "../dsvdef.h"
#include "../config/appconfig.h"
#include "../data/decode/decoderstatus.h"
#include "../data/decode/decoder.h"
#include "../log.h"
#include "../ui/langresource.h"
#include "../appcontrol.h"
#include "../ui/fn.h"

using namespace std;

namespace pv {
namespace dock {

//-----------ProtocolDock
   
ProtocolDock::ProtocolDock(QWidget *parent, view::View &view, SigSession *session) :
    QScrollArea(parent),
    _view(view)
{
    _session = session;
    _search_stack = NULL;
    _search_list_size = 0;
    _search_dirty = true;
    _search_generation = 0;
    _search_pos = -1;
    _list_cursor = {0, 0, 0};
    _list_cursor_valid = false;
    _nav_column = 0;
    _pro_add_button = NULL;

    //-----------------------------get protocol list
    GSList *l = const_cast<GSList*>(srd_decoder_list());
    std::map<std::string, int> pro_key_table;
    QString repeatNammes;

    for(; l; l = l->next)
    {
        const srd_decoder *const d = (srd_decoder*)l->data;
        assert(d);

        DecoderInfoItem *info = new DecoderInfoItem();
        srd_decoder *dec = (srd_decoder *)(l->data);
        info->_data_handle = dec;
        _decoderInfoList.push_back(info);

        std::string prokey(dec->id);
        if (pro_key_table.find(prokey) != pro_key_table.end())
        {
            if (repeatNammes != "")
                repeatNammes += ",";
            repeatNammes += QString(dec->id);
        }
        else
        {
            pro_key_table[prokey] = 1;
        }
    }
    g_slist_free(l);
  
    sort(_decoderInfoList.begin(), _decoderInfoList.end(), ProtocolDock::protocol_sort_callback);
  
    if (repeatNammes != ""){
        QString err = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODER_REPEAT), "Any decoder have repeated id or name:");
        err += repeatNammes;
        MsgBox::Show(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_ERROR), "error"), err.toUtf8().data());
    }

    //-----------------------------top panel
    QWidget *top_panel = new QWidget();
    top_panel->setMinimumHeight(70);
    _top_panel = top_panel;
    QWidget* bot_panel = new QWidget();

    _pro_add_button = new QPushButton(top_panel);
    _pro_add_button->setFlat(true);
    _del_all_button = new QPushButton(top_panel);
    _del_all_button->setFlat(true);
    _del_all_button->setCheckable(true);
    _pro_keyword_edit = new KeywordLineEdit(top_panel, this);
    _pro_keyword_edit->setReadOnly(true); 
 
    _pro_search_button = new XToolButton(top_panel);
    QHBoxLayout *pro_search_lay = new QHBoxLayout();
    pro_search_lay->setSpacing(2);
    pro_search_lay->addWidget(_pro_add_button);
    pro_search_lay->addWidget(_del_all_button);
    pro_search_lay->addWidget(_pro_keyword_edit, 1); 
    pro_search_lay->addWidget(_pro_search_button);
  
    _top_layout = new QVBoxLayout();
    _top_layout->addLayout(pro_search_lay);
    _top_layout->addStretch(1);
    _top_layout->setSpacing(5);
    top_panel->setLayout(_top_layout); 
 
    //-----------------------------bottom panel
    _bot_set_button = new QPushButton(bot_panel);
    _bot_set_button->setFlat(true);
    _bot_save_button = new QPushButton(bot_panel);
    _bot_save_button->setFlat(true);
    _dn_nav_button = new QPushButton(bot_panel);
    _dn_nav_button->setFlat(true);
    _bot_title_label = new QLabel(bot_panel);

    QHBoxLayout *bot_title_layout = new QHBoxLayout();
    bot_title_layout->setSpacing(2);
    bot_title_layout->addWidget(_bot_set_button);
    bot_title_layout->addWidget(_bot_save_button);
    bot_title_layout->addWidget(_bot_title_label, 1);
    bot_title_layout->addWidget(_dn_nav_button);
    
    _pre_button = new QPushButton(bot_panel);
    _ann_search_button = new QPushButton(bot_panel); //search icon
    _nxt_button = new QPushButton(bot_panel);
    _ann_search_edit = new PopupLineEdit(bot_panel);
    
    _ann_search_button->setFixedWidth(_ann_search_button->height());
    _ann_search_button->setDisabled(true);
   
    QHBoxLayout *ann_search_layout = new QHBoxLayout();
    ann_search_layout->setSpacing(2);
    ann_search_layout->addWidget(_pre_button);
    ann_search_layout->addWidget(_ann_search_button);
    ann_search_layout->addWidget(_ann_search_edit, 1);
    ann_search_layout->addWidget(_nxt_button);

    _table_view = new QTableView(bot_panel);
    _table_view->setModel(_session->get_decoder_model());
    _table_view->setAlternatingRowColors(true);
    _table_view->setShowGrid(false);
    _table_view->horizontalHeader()->setStretchLastSection(true);
    _table_view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    _table_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); 

    _matchs_title_label = new QLabel();
    _matchs_label = new QLabel();  
    QHBoxLayout *match_layout = new QHBoxLayout();
    match_layout->addWidget(_matchs_title_label, 0, Qt::AlignLeft);
    match_layout->addWidget(_matchs_label, 0, Qt::AlignLeft);
    match_layout->addStretch(1);

    QVBoxLayout *bot_layout = new QVBoxLayout();
    bot_layout->addLayout(bot_title_layout);
    bot_layout->addLayout(ann_search_layout);
    bot_layout->addLayout(match_layout);
    bot_layout->addWidget(_table_view);
    bot_panel->setLayout(bot_layout); 

    QSplitter *split_widget = new QSplitter(this);
    split_widget->insertWidget(0, top_panel);
    split_widget->insertWidget(1, bot_panel);
    split_widget->setOrientation(Qt::Vertical);
    split_widget->setCollapsible(0, false);
    split_widget->setCollapsible(1, false);

    this->setWidgetResizable(true);
    this->setWidget(split_widget);
    split_widget->setObjectName("protocolWidget");

    connect(_dn_nav_button, SIGNAL(clicked()),this, SLOT(nav_table_view()));
    connect(_bot_save_button, SIGNAL(clicked()),this, SLOT(export_table_view()));
    connect(_bot_set_button, SIGNAL(clicked()),this, SLOT(set_model()));
    connect(_pre_button, SIGNAL(clicked()),this, SLOT(search_pre()));
    connect(_nxt_button, SIGNAL(clicked()),this, SLOT(search_nxt()));
    connect(_pro_add_button, SIGNAL(clicked()),this, SLOT(on_add_protocol()));
    connect(_del_all_button, SIGNAL(clicked()),this, SLOT(on_del_all_protocol())); 

    connect(this, SIGNAL(protocol_updated()), this, SLOT(update_model()));
    connect(_table_view, SIGNAL(clicked(QModelIndex)), this, SLOT(item_clicked(QModelIndex)));

    connect(_table_view->horizontalHeader(), SIGNAL(sectionResized(int,int,int)), 
                    this, SLOT(column_resize(int, int, int)));

    connect(_pro_search_button, SIGNAL(clicked()), this, SLOT(show_protocol_select()));

    connect(_ann_search_edit, SIGNAL(editingFinished()), this, SLOT(search_changed()));
    // Enter in the search box finds the next match, as the next button does.
    // Queued: a long search shows a modal progress dialog, which must not run
    // inside the key handler of the line edit (on Windows a popup that is
    // closing at that moment).
    connect(_ann_search_edit, SIGNAL(returnPressed()), this, SLOT(search_nxt()),
            Qt::QueuedConnection);


    ADD_UI(this);
}

ProtocolDock::~ProtocolDock()
{
    //destroy protocol item layers
   for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end(); it++){
       DESTROY_QT_LATER(*it);
   }
   _protocol_lay_items.clear();

   //clear protocol infos list
   RELEASE_ARRAY(_decoderInfoList);

   REMOVE_UI(this);
}

void ProtocolDock::retranslateUi()
{
    _ann_search_edit->setPlaceholderText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH), "search"));
    _matchs_title_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MATCHING_ITEMS), "Matching Items:"));
    _bot_title_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PROTOCOL_LIST_VIEWER), "Protocol List Viewer"));
    _pro_keyword_edit->ResetText();
}

void ProtocolDock::reStyle()
{
    QString iconPath = GetIconPath();

    if (_pro_add_button == NULL)
    {
        return;
    }

    _pro_add_button->setIcon(QIcon(iconPath+"/add.svg"));
    _del_all_button->setIcon(QIcon(iconPath+"/del.svg"));
    _bot_set_button->setIcon(QIcon(iconPath+"/gear.svg"));
    _bot_save_button->setIcon(QIcon(iconPath+"/save.svg"));
    _dn_nav_button->setIcon(QIcon(iconPath+"/nav.svg"));
    _pre_button->setIcon(QIcon(iconPath+"/pre.svg"));
    _nxt_button->setIcon(QIcon(iconPath+"/next.svg"));
    _ann_search_button->setIcon(QIcon(iconPath+"/search.svg"));
    _pro_search_button->setIcon(QIcon(iconPath + "/search.svg"));

    for (auto item : _protocol_lay_items){
        item->ResetStyle();
    }
}

int ProtocolDock::decoder_name_cmp(const void *a, const void *b)
{
    return strcmp(((const srd_decoder*)a)->name,
        ((const srd_decoder*)b)->name);
}

int ProtocolDock::get_protocol_index_by_id(QString id)
{
    int dex = 0;
    for (auto info : _decoderInfoList){
        srd_decoder *dec = (srd_decoder *)(info->_data_handle);
        QString proid(dec->id);
        if (id == proid){
            return dex;
        }
        ++dex;
    }
    return -1;
} 

void ProtocolDock::on_add_protocol()
{ 
     if (_decoderInfoList.size() == 0){
        MsgBox::Show(NULL, L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODER_LIST_EMPTY), "Decoder list is empty!"));
        return;
    }
    if (_selected_protocol_id == ""){
        MsgBox::Show(NULL, L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NO_SEL_DECODER), "Please select a decoder!"));
        return;
    }

    int dex = this->get_protocol_index_by_id(_selected_protocol_id);
    assert(dex >= 0);

    //check the base protocol
    srd_decoder *const dec = (srd_decoder *)(_decoderInfoList[dex]->_data_handle);
    QString pro_id(dec->id);
    std::list<data::decode::Decoder*> sub_decoders;
    
    assert(dec->inputs);

    QString input_id = parse_protocol_id((char *)dec->inputs->data);

    if (input_id != "logic")
    {
        pro_id = ""; //reset base protocol

        int base_dex = get_output_protocol_by_id(input_id);
        sub_decoders.push_front(new data::decode::Decoder(dec));

        while (base_dex != -1)
        {
            srd_decoder *base_dec = (srd_decoder *)(_decoderInfoList[base_dex]->_data_handle);
            pro_id = QString(base_dec->id); //change base protocol           

            assert(base_dec->inputs);

            input_id = parse_protocol_id((char *)base_dec->inputs->data);

            if (input_id == "logic")
            {
                break;
            }

            sub_decoders.push_front(new data::decode::Decoder(base_dec));
            pro_id = ""; //reset base protocol
            base_dex = get_output_protocol_by_id(input_id);
        }
    }

    if (pro_id == ""){
        MsgBox::Show(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_ERROR), "error"), 
                     L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FIND_BASE_DECODER_ERROR), "find the base decoder error!"));

        for(auto sub: sub_decoders){
            delete sub;
        }
        sub_decoders.clear();

        return;
    }

    add_protocol_by_id(pro_id, false, sub_decoders);
}

bool ProtocolDock::add_protocol_by_id(QString id, bool silent, std::list<pv::data::decode::Decoder*> &sub_decoders)
{
    if (_session->get_device()->get_work_mode() != LOGIC) {
        dsv_info("Protocol Analyzer\nProtocol Analyzer is only valid in Digital Mode!");
        return false;
    }

    int dex = this->get_protocol_index_by_id(id);
    if (dex == -1){
        dsv_err("Protocol not exists! id:%s", id.toUtf8().data());
        return false;
    }

    srd_decoder *const decoder = (srd_decoder *)(_decoderInfoList[dex]->_data_handle);
    DecoderStatus *dstatus = new DecoderStatus();
    dstatus->m_format = (int)DecoderDataFormat::hex;

    QString protocolName(decoder->name);
    QString protocolId(decoder->id);

    if (sub_decoders.size()){
        auto it = sub_decoders.end();
        it--;
        protocolName = QString((*it)->decoder()->name);
        protocolId = QString((*it)->decoder()->id); 
    }

    pv::view::Trace *trace = NULL;

    if (_session->add_decoder(decoder, silent, dstatus, sub_decoders, trace) == false){
        return false;
    }

    // create item layer
    ProtocolItemLayer *layer = new ProtocolItemLayer(_top_panel, protocolName, this);
    _protocol_lay_items.push_back(layer);
    _top_layout->insertLayout(_protocol_lay_items.size(), layer);
    layer->m_decoderStatus = dstatus; 
    layer->m_protocolId = protocolId;
    layer->_trace = trace;

    // set current protocol format
    string fmt = AppConfig::Instance().GetProtocolFormat(protocolId.toStdString());
    if (fmt != "")
    {
        layer->SetProtocolFormat(fmt.c_str());
        dstatus->m_format = DecoderDataFormat::Parse(fmt.c_str());
    }

    // progress connection
    const auto &decode_sigs = _session->get_decode_signals();   
    protocol_updated();
    connect(decode_sigs.back(), SIGNAL(decoded_progress(int)), this, SLOT(decoded_progress(int)));

    adjustPannelSize();

    return true;
}
 
 void ProtocolDock::on_del_all_protocol(){
     if (_protocol_lay_items.size() == 0){
        MsgBox::Show(NULL, L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NO_DECODER_DEL), "Have no decoder to remove!"), this);
        return;
     }

    QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODER_CONFIRM_DEL_ALL), "Are you sure to remove all decoder?"));
    if (MsgBox::Confirm(strMsg,  this)){
        del_all_protocol();
    }
 } 

void ProtocolDock::del_all_protocol()
{  
    if (_protocol_lay_items.size() > 0)
    {
        _session->clear_all_decoder();
    }
}

void ProtocolDock::decoded_progress(int progress)
{
    const auto &decode_sigs = _session->get_decode_signals();
    unsigned int index = 0;

    for(auto d : decode_sigs) {
        int pg = d->get_progress();
        QString err;

        if (d->decoder()->out_of_memory())
            err = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_OUT_OF_MEMORY), "Out of Memory");

        if (index < _protocol_lay_items.size())
        {
            ProtocolItemLayer &lay =  *(_protocol_lay_items.at(index));
            lay.SetProgress(pg, err);
            
            // have custom data format
            if (pg == 100 && lay.m_decoderStatus != NULL){
                lay.enable_format(lay.m_decoderStatus->m_bNumeric);
            }
        }

        index++;
    }

    if (progress == 0 || progress % 10 == 1){
        update_model();
    }  
}

void ProtocolDock::set_model()
{
    pv::dialogs::ProtocolList *protocollist_dlg = new pv::dialogs::ProtocolList(this, _session);
    protocollist_dlg->exec();
    delete protocollist_dlg;
    resize_table_view(_session->get_decoder_model());

    // The list may show another decoder or other columns now.
    _list_cursor_valid = false;
    _nav_column = 0;
    search_invalidate();

    // clear mark_index of all DecoderStacks
    const auto &decode_sigs = _session->get_decode_signals();
        
    for(auto d : decode_sigs) {
        d->decoder()->set_mark_index(-1);
    }
}

void ProtocolDock::update_model()
{
    pv::data::DecoderModel *decoder_model = _session->get_decoder_model();
    const auto &decode_sigs = _session->get_decode_signals();
    void *old_stack = decoder_model->getDecoderStack();

    if (decode_sigs.size() == 0)
        decoder_model->setDecoderStack(NULL);
    else if (!decoder_model->getDecoderStack())
        decoder_model->setDecoderStack(decode_sigs.at(0)->decoder());
    else {
        unsigned int index = 0;
        for(auto d : decode_sigs) {
            if (d->decoder() == decoder_model->getDecoderStack()) {
                decoder_model->setDecoderStack(d->decoder());
                break;
            }
            index++;
        }
        if (index >= decode_sigs.size())
            decoder_model->setDecoderStack(decode_sigs.at(0)->decoder());
    }

    if (decoder_model->getDecoderStack() != old_stack) {
        _list_cursor_valid = false;
        _nav_column = 0;
    }
    search_invalidate();
    resize_table_view(decoder_model);
}

void ProtocolDock::resize_table_view(data::DecoderModel* decoder_model)
{
    if (decoder_model->getDecoderStack()) {
        for (int i = 0; i < decoder_model->columnCount(QModelIndex()) - 1; i++) {
            _table_view->resizeColumnToContents(i);
            if (_table_view->columnWidth(i) > 200)
                _table_view->setColumnWidth(i, 200);
        }
        int top_row = _table_view->rowAt(0);
        int bom_row = _table_view->rowAt(_table_view->height());
        if (bom_row >= top_row && top_row >= 0) {
            for (int i = top_row; i <= bom_row; i++)
                _table_view->resizeRowToContents(i);
        }
    }
}

void ProtocolDock::item_clicked(const QModelIndex &index)
{
    // The next search starts from the clicked cell.
    select_list_cell(index);
    _search_pos = cursor_match_pos();
    show_search_count();
}

// Marks the annotation of a list cell on the waveform and moves the view to
// it. The cell becomes the starting point of the next search.
void ProtocolDock::select_list_cell(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    pv::data::DecoderModel *decoder_model = _session->get_decoder_model();
    auto decoder_stack = decoder_model->getDecoderStack();

    _nav_column = index.column();

    if (decoder_stack) {
        pv::data::decode::Annotation ann;
        if (decoder_stack->list_annotation(&ann, index.column(), index.row())) {
            const auto &decode_sigs = _session->get_decode_signals();

            for(auto d : decode_sigs) {
                d->decoder()->set_mark_index(-1);
            }

            decoder_stack->set_mark_index((ann.start_sample()+ann.end_sample())/2);
            _session->show_region(ann.start_sample(), ann.end_sample(), false);

            _list_cursor = {ann.start_sample(), (uint32_t)index.row(), (uint16_t)index.column()};
            _list_cursor_valid = true;
        }
    }

    _table_view->resizeRowToContents(index.row());
}

void ProtocolDock::column_resize(int index, int old_size, int new_size)
{
    (void)index;
    (void)old_size;
    (void)new_size;
    pv::data::DecoderModel *decoder_model = _session->get_decoder_model();
    if (decoder_model->getDecoderStack()) {
        int top_row = _table_view->rowAt(0);
        int bom_row = _table_view->rowAt(_table_view->height());
        if (bom_row >= top_row && top_row >= 0) {
            for (int i = top_row; i <= bom_row; i++)
                _table_view->resizeRowToContents(i);
        }
    }
}

void ProtocolDock::export_table_view()
{
    pv::dialogs::ProtocolExp *protocolexp_dlg = new pv::dialogs::ProtocolExp(this, _session);
    protocolexp_dlg->exec();
    delete protocolexp_dlg;
}

void ProtocolDock::nav_table_view()
{
    uint64_t row_index = 0;
    pv::data::DecoderModel *decoder_model = _session->get_decoder_model();

    auto decoder_stack = decoder_model->getDecoderStack();
    if (decoder_stack) {
        uint64_t offset = _view.x_offset() * (decoder_stack->samplerate() * _view.scale());
        std::map<const pv::data::decode::Row, bool> rows = decoder_stack->get_rows_lshow();
        if (_nav_column < 0 || _nav_column >= decoder_model->columnCount(QModelIndex()))
            _nav_column = 0;
        int column = _nav_column;
        for (std::map<const pv::data::decode::Row, bool>::const_iterator i = rows.begin();
            i != rows.end(); i++) {
            if ((*i).second && column-- == 0) {
                row_index = decoder_stack->get_annotation_index((*i).first, offset);
                break;
            }
        }
        QModelIndex index = decoder_model->index(row_index, _nav_column);

        if(index.isValid()){         

            pv::data::decode::Annotation ann;

            if (decoder_stack->list_annotation(&ann, index.column(), index.row()))
            {
                _table_view->scrollTo(index);
                _table_view->setCurrentIndex(index);

                 const auto &decode_sigs = _session->get_decode_signals();

                for(auto d : decode_sigs) {
                    d->decoder()->set_mark_index(-1);
                }

                decoder_stack->set_mark_index((ann.start_sample()+ann.end_sample())/2);
                _view.set_all_update(true);
                _view.update();

                // The next search starts from here.
                _list_cursor = {ann.start_sample(), (uint32_t)index.row(), (uint16_t)index.column()};
                _list_cursor_valid = true;
                _search_pos = cursor_match_pos();
                show_search_count();
            }           
        }
    }
}

//-------------------search in the list of decoded data
//
// The search goes through every column of the list (Address bus, Data bus,
// Instructions...), not only the one that was clicked last, and visits the
// matches in time order. It starts from the cell that was selected last,
// by a click, the navigate button or the search itself.

bool ProtocolDock::search_match_less(const SearchMatch &a, const SearchMatch &b)
{
    if (a.start != b.start)
        return a.start < b.start;
    if (a.col != b.col)
        return a.col < b.col;
    return a.row < b.row;
}

uint64_t ProtocolDock::list_size_total(data::DecoderStack *stack)
{
    uint64_t total = 0;
    const int cols = stack->list_rows_size();

    for (int col = 0; col < cols; col++)
        total += stack->list_annotation_size(col);

    return total;
}

// Do the numeric annotations that follow 'row' in list column 'col' contain
// seq[1], seq[2]... in this order? Text annotations in between (ACK, Start
// and the like) are skipped, but only a few: in a column without any numbers
// the search must not walk to the end of the column for every candidate.
bool ProtocolDock::sequence_follows(data::DecoderStack *stack, int col,
                                    uint64_t row, const QStringList &seq)
{
    const int max_skipped = 16;

    for (int i = 1; i < seq.size(); i++) {
        pv::data::decode::Annotation ann;
        int skipped = 0;

        for (;;) {
            if (!stack->list_annotation(&ann, col, row++))
                return false;
            if (ann.is_numberic())
                break;
            if (++skipped > max_skipped)
                return false;
        }

        const auto &lines = ann.annotations();
        if (lines.empty() || !lines.at(0).contains(seq.at(i), Qt::CaseInsensitive))
            return false;
    }

    return true;
}

// Finds the annotations whose text, as shown in the list, contains 'text'
// (case is ignored). "70-70-70" also finds three numeric values 70, 70, 70
// in a row of one column, the multi-byte search of the user guide.
// Runs in a worker thread for long lists: it must not touch any widget.
void ProtocolDock::find_matches(data::DecoderStack *stack, QString text,
                                std::vector<SearchMatch> &matches)
{
    matches.clear();
    text = text.trimmed();

    if (stack == NULL || text.isEmpty())
        return;

    // A multi-value search needs something on both sides of every '-', so
    // that text like "->" or "-12" is simply searched as it is.
    QStringList seq = text.split('-');
    bool is_seq = seq.size() > 1;

    for (QString &part : seq) {
        part = part.trimmed();
        if (part.isEmpty())
            is_seq = false;
    }

    const int cols = stack->list_rows_size();

    for (int col = 0; col < cols; col++) {
        const uint64_t count = stack->list_annotation_size(col);

        for (uint64_t row = 0; row < count; row++) {
            pv::data::decode::Annotation ann;
            if (!stack->list_annotation(&ann, col, row))
                break;

            const auto &lines = ann.annotations();
            if (lines.empty())
                continue;

            const QString &str = lines.at(0);
            bool found = str.contains(text, Qt::CaseInsensitive);

            if (!found && is_seq && str.contains(seq.at(0), Qt::CaseInsensitive))
                found = sequence_follows(stack, col, row + 1, seq);

            if (found)
                matches.push_back({ann.start_sample(), (uint32_t)row, (uint16_t)col});
        }
    }

    std::sort(matches.begin(), matches.end(), search_match_less);
}

bool ProtocolDock::list_decode_pending(data::DecoderStack *stack)
{
    for (auto trace : _session->get_decode_signals()) {
        if (trace->decoder() == stack)
            return _session->is_decode_pending(trace);
    }
    return false;
}

// Finds the matches again if the search text or the decoded data changed
// since the last search. Returns false if there is nothing to search.
bool ProtocolDock::search_update()
{
    pv::data::DecoderModel *decoder_model = _session->get_decoder_model();
    pv::data::DecoderStack *stack = decoder_model->getDecoderStack();
    const QString text = _ann_search_edit->text().trimmed();

    if (stack == NULL || text.isEmpty()) {
        _search_matches.clear();
        _search_text = text;
        _search_stack = stack;
        _search_list_size = 0;
        _search_dirty = false;
        _search_pos = -1;
        show_search_count();
        return false;
    }

    // A new decode clears the annotation texts on the decode thread, so
    // search only once the list's decoder is done ("..." until then).
    if (list_decode_pending(stack)) {
        _search_dirty = true;
        _search_pos = -1;
        show_search_count();
        return false;
    }

    const uint64_t list_size = list_size_total(stack);

    if (!_search_dirty && text == _search_text && (void*)stack == _search_stack
        && list_size == _search_list_size) {
        return true;
    }

    const uint64_t generation = _search_generation;
    std::vector<SearchMatch> matches;

    if (list_size > ProgressRows) {
        QFuture<void> future = QtConcurrent::run([stack, text, &matches]{
            find_matches(stack, text, matches);
        });

        Qt::WindowFlags flags = Qt::CustomizeWindowHint;
        QProgressDialog dlg(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCHING), "Searching..."),
                            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CANCEL), "Cancel"),0,0,this,flags);
        dlg.setWindowModality(Qt::WindowModal);
        dlg.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint |
                           Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint);
        dlg.setCancelButton(NULL);

        QFutureWatcher<void> watcher;
        connect(&watcher,SIGNAL(finished()),&dlg,SLOT(cancel()));
        watcher.setFuture(future);

        if (!future.isFinished())
            dlg.exec();
        future.waitForFinished();
    }
    else {
        find_matches(stack, text, matches);
    }

    _search_matches.swap(matches);
    _search_text = text;
    _search_stack = stack;
    _search_list_size = list_size;
    // New data may have come in while the worker was searching.
    _search_dirty = (generation != _search_generation);
    _search_pos = -1;
    show_search_count();

    return true;
}

// Index of the match that is the last selected cell, or -1.
int ProtocolDock::cursor_match_pos()
{
    if (!_list_cursor_valid || _search_matches.empty())
        return -1;

    auto it = std::lower_bound(_search_matches.begin(), _search_matches.end(),
                               _list_cursor, search_match_less);

    if (it == _search_matches.end() || search_match_less(_list_cursor, *it))
        return -1;

    return (int)(it - _search_matches.begin());
}

// Selects the next (or previous) match after the last selected cell,
// starting over at the other end of the list.
void ProtocolDock::search_jump(bool forward)
{
    if (!search_update() || _search_matches.empty()) {
        _table_view->clearSelection();
        return;
    }

    const auto first = _search_matches.begin();
    const auto last = _search_matches.end();
    const int count = (int)_search_matches.size();
    int pos;

    if (!_list_cursor_valid) {
        pos = forward ? 0 : count - 1;
    }
    else if (forward) {
        auto it = std::upper_bound(first, last, _list_cursor, search_match_less);
        pos = (it == last) ? 0 : (int)(it - first);
    }
    else {
        auto it = std::lower_bound(first, last, _list_cursor, search_match_less);
        pos = (it == first) ? count - 1 : (int)(it - first) - 1;
    }

    const SearchMatch match = _search_matches[pos];
    QModelIndex index = _session->get_decoder_model()->index(match.row, match.col);

    if (!index.isValid())
        return;

    _table_view->scrollTo(index);
    _table_view->setCurrentIndex(index);
    select_list_cell(index);

    _search_pos = pos;
    show_search_count();
}

void ProtocolDock::search_pre()
{
    search_jump(false);
}

void ProtocolDock::search_nxt()
{
    search_jump(true);
}

// The search text was edited.
void ProtocolDock::search_changed()
{
    show_search_count();
}

// The decoded data, the decoder of the list or its columns changed: search
// again before the next jump.
void ProtocolDock::search_invalidate()
{
    _search_dirty = true;
    _search_generation++;
    show_search_count();
}

// "Matching Items": the number of matches, with the position of the selected
// one, or "..." until the search has run for the current text and data.
void ProtocolDock::show_search_count()
{
    const QString text = _ann_search_edit->text().trimmed();

    if (text.isEmpty())
        _matchs_label->setText("");
    else if (_search_dirty || text != _search_text)
        _matchs_label->setText("...");
    else if (_search_pos >= 0)
        _matchs_label->setText(QString("%1/%2").arg(_search_pos + 1).arg(_search_matches.size()));
    else
        _matchs_label->setText(QString::number(_search_matches.size()));
}

 //-------------------IProtocolItemLayerCallback
void ProtocolDock::OnProtocolSetting(void *handle){
  
    for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end(); it++){
       if ((*it) == handle){ 
             void *key_handel = (*it)->get_protocol_key_handel();
            _session->rst_decoder_by_key_handel(key_handel);
            protocol_updated();
           break;
       } 
   }
}

void ProtocolDock::OnProtocolDelete(void *handle){
    QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODER_CONFIRM_DEL), "Are you sure to remove this decoder?"));

    if (!MsgBox::Confirm(strMsg, this)){
        return;
    } 

    for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end(); it++)
    {
        if ((*it) == handle)
        {
            auto lay = (*it); 
            void *key_handel = lay->get_protocol_key_handel();
            _protocol_lay_items.erase(it);
            DESTROY_QT_LATER(lay);
            _session->remove_decoder_by_key_handel(key_handel);     
            protocol_updated();
            break;
        } 
    }

    adjustPannelSize();
}

bool ProtocolDock::remove_protocol(int index)
{
    view::DecodeTrace *trace = _session->get_decoder_trace(index);
    if (trace == NULL)
        return false;

    void *key_handel = trace->get_key_handel();

    for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end(); it++)
    {
        if ((*it)->get_protocol_key_handel() == key_handel)
        {
            auto lay = (*it);
            _protocol_lay_items.erase(it);
            DESTROY_QT_LATER(lay);
            break;
        }
    }

    _session->remove_decoder_by_key_handel(key_handel);
    protocol_updated();
    adjustPannelSize();
    return true;
}

void ProtocolDock::OnProtocolFormatChanged(QString format, void *handle){
    for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end(); it++){
       if ((*it) == handle){
           auto lay = (*it); 
           AppConfig::Instance().SetProtocolFormat(lay->m_protocolId.toStdString(), format.toStdString());

           if (lay->m_decoderStatus != NULL)
           {
                  lay->m_decoderStatus->m_format = DecoderDataFormat::Parse(format.toStdString().c_str());
                  protocol_updated();
           }
        
           break;
       }
   }
} 
  

bool ProtocolDock::protocol_sort_callback(const DecoderInfoItem *o1, const DecoderInfoItem *o2)
{
    srd_decoder *dec1 = (srd_decoder *)(o1->_data_handle);
    srd_decoder *dec2 = (srd_decoder *)(o2->_data_handle);
    const char *s1 = dec1->name;
    const char *s2 = dec2->name;
    char c1 = 0;
    char c2 = 0;

    while (*s1 && *s2)
    {
        c1 = *s1;
        c2 = *s2;

        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;                               

        if (c1 > c2)
            return false;
        else if (c1 < c2)
            return true;
        
        s1++;
        s2++;
    }

    if (*s1)
        return false;
    else if (*s2)
        return true;

    return true;
}

 QString ProtocolDock::parse_protocol_id(const char *id)
 {
     if (id == NULL || *id == 0){
         assert(false);
     }
     char buf[25];
     strncpy(buf, id, sizeof(buf)-1);
     char *rd = buf;
     char *start = NULL;
     unsigned int len = 0;

     while (*rd && len - 1 < sizeof(buf))
     { 
         if (*rd == '['){
             start = rd++;
         }
         else if (*rd == ']'){
             *rd = 0;
             break;
         }
         ++rd;
         len++;
     }
     if (start == NULL){
         start = const_cast<char*>(id);
     }

     return QString(start);
 }

  int ProtocolDock::get_output_protocol_by_id(QString id)
  {
      int dex = 0;

      for (auto info : _decoderInfoList)
      {
          srd_decoder *dec = (srd_decoder *)(info->_data_handle);
          if (dec->outputs)
          {
              QString output_id = parse_protocol_id((char*)dec->outputs->data);
              if (output_id == id)
              {
                  QString proid(dec->id);
                  if (!proid.startsWith("0:") || output_id == proid){
                      return dex;
                  } 
              }
          }
         
          ++dex;
      }

      return -1;
  }

  void ProtocolDock::BeginEditKeyword()
  {
      show_protocol_select();
  }

  void ProtocolDock::show_protocol_select()
  {
      SearchComboBox *panel = new SearchComboBox(this);

      for (auto info : _decoderInfoList)
      {
          srd_decoder *dec = (srd_decoder *)(info->_data_handle);
          panel->AddDataItem(QString(dec->id), QString(dec->name), info);
      }

      QFont font = this->font();
      font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
      ui::set_form_font(panel, font);

      panel->SetItemClickHandle(this);
      panel->ShowDlg(_pro_keyword_edit);
  }

 void ProtocolDock::OnItemClick(void *sender, void *data_handle)
 {
    (void)sender;

     if (data_handle != NULL){
         DecoderInfoItem *info = (DecoderInfoItem*)data_handle;
         srd_decoder *dec = (srd_decoder *)(info->_data_handle); 
         this->_pro_keyword_edit->SetInputText(QString(dec->name)); 
         _selected_protocol_id = QString(dec->id);
         this->on_add_protocol();       
     }
 }

 void ProtocolDock::reset_view()
 {
    decoded_progress(0);
    update();
 }

 void ProtocolDock::update_view_status()
 {
    bool bEnable = _session->is_working() == false;
    _pro_keyword_edit->setEnabled(bEnable);
    _pro_add_button->setEnabled(bEnable);
    _pro_search_button->setEnabled(bEnable);
 }

void ProtocolDock::update_deocder_item_name(void *trace_handel, const char *name)
{
    for(auto p : _protocol_lay_items){
        if (p->_trace == trace_handel){
            p->set_label_name(QString(name));
            break;
        }
    }
}

void ProtocolDock::clear_decoder_items()
{
    if (_protocol_lay_items.empty())
        return;

    for (auto it = _protocol_lay_items.begin(); it != _protocol_lay_items.end(); it++)
    {
         DESTROY_QT_LATER((*it));
    }

    _protocol_lay_items.clear();
    this->update();
    protocol_updated();

    adjustPannelSize();
}

void ProtocolDock::UpdateLanguage()
{
    retranslateUi();
}

void ProtocolDock::UpdateTheme()
{
    reStyle();
}

void ProtocolDock::UpdateFont()
 {
    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    ui::set_form_font(this, font);
    _table_view->setFont(font);

    // The decoded-results model provides no size hint, so the table keeps its
    // default row height and clips larger fonts. Grow the rows with the font,
    // leaving room for cell margins and descenders.
    QFontMetrics cell_fm(font);
    _table_view->verticalHeader()->setDefaultSectionSize(qRound(cell_fm.height() * 1.5));

    for(auto lay : _protocol_lay_items){
        lay->update_font();
    }

    font.setPointSizeF(font.pointSizeF() + 1);
    this->parentWidget()->setFont(font);
    _table_view->horizontalHeader()->setFont(font);
    _table_view->verticalHeader()->setFont(font);

    
    _table_view->setObjectName("DecodedDataView");
    QString style = "#DecodedDataView QHeaderView{font-size: %1pt}";
    style = style.arg(AppConfig::Instance().appOptions.fontSize);
    _table_view->setStyleSheet(style);

    adjustPannelSize(); 
 }

 void ProtocolDock::adjustPannelSize()
 {
    QString str = "DECODER";
    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    QFontMetrics fm(font);
    QRect rc = fm.boundingRect(str); 

    int lineHeight = rc.height() + 15;
    _pro_keyword_edit->setFixedHeight(rc.height() + 5);
    int pannelHeight = lineHeight * _protocol_lay_items.size() + _pro_keyword_edit->height();

    if (pannelHeight < 100){
        pannelHeight = 100;
    }

    _top_panel->setMinimumHeight(pannelHeight);
 }

} // namespace dock
} // namespace pv
