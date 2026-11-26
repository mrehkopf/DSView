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


#ifndef DSVIEW_PV_PROTOCOLDOCK_H
#define DSVIEW_PV_PROTOCOLDOCK_H

#include <libsigrokdecode.h>

#include <QDockWidget>
#include <QPushButton>
#include <QLabel> 
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSplitter>
#include <QTableView>
#include <vector>
#include <mutex>
#include <list>
#include "../data/decodermodel.h"
#include "protocolitemlayer.h"
#include "keywordlineedit.h"
#include "searchcombobox.h"
#include "../interface/icallbacks.h"
#include "../ui/xtoolbutton.h"
#include "../ui/uimanager.h"

struct DecoderInfoItem{
    void  *_data_handle; //srd_decoder* type
};

namespace pv {

class SigSession;
  
namespace data {
    class DecoderModel;
    class DecoderStack;
    namespace decode{
        class Decoder;
    }
}

namespace view {
class View;
}

namespace dock {
  
class ProtocolDock : public QScrollArea, 
public IProtocolItemLayerCallback, 
public IKeywordActive,
public ISearchItemClick,
public IDecoderPannel,
public IUiWindow
{
    Q_OBJECT

public:
    static const uint64_t ProgressRows = 100000;

public:
    ProtocolDock(QWidget *parent, view::View &view, SigSession *session);
    ~ProtocolDock();

    void del_all_protocol();
    bool add_protocol_by_id(QString id, bool silent, std::list<pv::data::decode::Decoder*> &sub_decoders);
    // Removes the session's decoder at this index without asking, for the
    // control interface. False if there is none.
    bool remove_protocol(int index);

    void reset_view();
    void update_view_status();

private:
    void retranslateUi();
    void reStyle();
    int get_protocol_index_by_id(QString id);
    static QString parse_protocol_id(const char *id);
    int get_output_protocol_by_id(QString id);

    static int decoder_name_cmp(const void *a, const void *b);
    void resize_table_view(data::DecoderModel *decoder_model);
    static bool protocol_sort_callback(const DecoderInfoItem *o1, const DecoderInfoItem *o2);

    //IProtocolItemLayerCallback
    void OnProtocolSetting(void *handle) override;
    void OnProtocolDelete(void *handle) override;
    void OnProtocolFormatChanged(QString format, void *handle) override;

    //IKeywordActive
    void BeginEditKeyword() override; 

    //ISearchItemClick
    void OnItemClick(void *sender, void *data_handle) override;

    //IDecoderPannel
    void update_deocder_item_name(void *trace_handel, const char *name) override;
    void clear_decoder_items() override;

    //IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

    void adjustPannelSize();

    // An annotation in the list that matches the search text. Matches are
    // kept in time order, over all columns of the list.
    struct SearchMatch
    {
        uint64_t start; // first sample of the annotation
        uint32_t row;   // row in the list
        uint16_t col;   // column in the list (annotation row of the decoder)
    };

    static bool search_match_less(const SearchMatch &a, const SearchMatch &b);
    static void find_matches(data::DecoderStack *stack, QString text,
                             std::vector<SearchMatch> &matches);
    static bool sequence_follows(data::DecoderStack *stack, int col,
                                 uint64_t row, const QStringList &seq);
    static uint64_t list_size_total(data::DecoderStack *stack);

    bool list_decode_pending(data::DecoderStack *stack);
    bool search_update();
    void search_jump(bool forward);
    int cursor_match_pos();
    void search_invalidate();
    void show_search_count();
    void select_list_cell(const QModelIndex &index);

signals:
    void protocol_updated();

public slots:
    void update_model();

private slots:
    void on_add_protocol(); 
    void on_del_all_protocol();
    void decoded_progress(int progress);
    void set_model();   
    void export_table_view();
    void nav_table_view();
    void item_clicked(const QModelIndex &index);
    void column_resize(int index, int old_size, int new_size);
    void search_pre();
    void search_nxt();
    void search_changed();
    void show_protocol_select();

private:
    SigSession *_session;
    view::View &_view;

    std::vector<SearchMatch> _search_matches;
    QString     _search_text;       // text the matches were found for
    void        *_search_stack;     // decoder stack they were found in
    uint64_t    _search_list_size;  // annotations in the list at that time
    bool        _search_dirty;      // decoded data changed since then
    uint64_t    _search_generation; // counts search_invalidate() calls
    int         _search_pos;        // index of the selected match, or -1
    SearchMatch _list_cursor;       // annotation last selected in the list
    bool        _list_cursor_valid;
    int         _nav_column;        // list column of the navigate button

    QWidget     *_top_panel; 
    QTableView  *_table_view;
    QPushButton *_pre_button;
    QPushButton *_nxt_button;
    PopupLineEdit *_ann_search_edit; 
    QLabel *_matchs_label;
    QLabel *_matchs_title_label;
    QLabel *_bot_title_label;

    QPushButton *_pro_add_button;
    QPushButton *_del_all_button; 
    QVBoxLayout *_top_layout;
    std::vector <ProtocolItemLayer*> _protocol_lay_items; //protocol item layers

    QPushButton *_bot_set_button;
    QPushButton *_bot_save_button;
    QPushButton *_dn_nav_button;
    QPushButton *_ann_search_button;
    std::vector<DecoderInfoItem*> _decoderInfoList;
    KeywordLineEdit *_pro_keyword_edit;
    QString     _selected_protocol_id;
    XToolButton *_pro_search_button; 

};

} // namespace dock
} // namespace pv

#endif // DSVIEW_PV_PROTOCOLDOCK_H
