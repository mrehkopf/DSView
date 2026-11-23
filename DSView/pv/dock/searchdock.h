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


#ifndef DSVIEW_PV_SEARCHDOCK_H
#define DSVIEW_PV_SEARCHDOCK_H

#include <mutex>
#include <vector>
#include <QJsonObject>
#include <QTimer>
#include <QWidget>

#include "../interface/icallbacks.h"
#include "../ui/uimanager.h"
#include "../search/searchengine.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QTableView;
class QModelIndex;

namespace pv {

class SigSession;

namespace data {
class LogicSnapshot;
}

namespace view {
class View;
}

namespace search {
class SearchJob;
}

namespace dialogs {
struct SearchSignal;
}

namespace dock {

class SearchResultModel;

// A value shown per match: a condition's count, or its delta.
struct SearchColumn
{
    int condition;
    bool count;
};

// Matches, compactly. values has one entry per column per match; NoValue
// for a delta with no occurrence.
struct SearchRows
{
    static const int64_t NoValue = INT64_MIN;
    enum { StartUnknown = 1, EndUnknown = 2 };

    std::vector<uint64_t> start;
    std::vector<uint64_t> end;
    std::vector<uint8_t> flags;
    std::vector<int64_t> values;

    inline size_t size() const { return start.size(); }
    void append(const search::Match &m, const std::vector<SearchColumn> &columns);
    void clear();
};

// The search bar under the waveforms: what is searched for, Previous and
// Next, and the list of matches (doc/search.md).
class SearchDock : public QWidget, public IUiWindow, public IMessageListener
{
    Q_OBJECT

public:
    SearchDock(QWidget *parent, pv::view::View &view, SigSession *session);
    ~SearchDock();

    // The query, for the session file.
    QJsonObject get_session();
    void set_session(const QJsonObject &obj);

    // IMessageListener
    void OnMessage(int msg) override;

private:
    void retranslateUi();
    void reStyle();

    //IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

    std::vector<dialogs::SearchSignal> search_signals();
    search::BusResolver bus_resolver();
    data::LogicSnapshot *logic_snapshot();
    bool results_current();
    bool start_search();
    void clear_results();
    void jump(int direction);
    bool jump_to(size_t index);
    void update_summary();
    void update_status();
    void set_status(const QString &text);

private slots:
    void on_edit();
    void on_previous();
    void on_next();
    void on_run();
    void on_list(bool show);
    void on_poll();
    void on_finished();
    void on_row_activated(const QModelIndex &index);

private:
    SigSession *_session;
    view::View &_view;

    search::Query _query;
    search::Program _program;
    search::SearchJob *_job;
    SearchResultModel *_model;

    // Matches from the search thread, waiting for on_poll().
    std::vector<SearchColumn> _columns;
    std::mutex _pending_lock;
    SearchRows _pending;

    // What the results were found in.
    data::LogicSnapshot *_results_snapshot;
    uint64_t _results_version;
    bool _have_results;
    int _pending_jump;          // -1 previous, 1 next, 0 none
    int64_t _current;           // the match last jumped to, in time order
    uint64_t _polled_progress;  // where the search was when on_poll() last took matches
    QString _message;           // the last outcome, shown next to the count

    QTimer _poll;
    QPushButton *_edit_button;
    QPushButton *_pre_button;
    QPushButton *_nxt_button;
    QPushButton *_run_button;
    QPushButton *_list_button;
    QLabel *_status;
    QProgressBar *_progress;
    QTableView *_table;
    QLabel *_stats;
};

} // namespace dock
} // namespace pv

#endif // DSVIEW_PV_SEARCHDOCK_H
