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

#include "searchdock.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <QAbstractTableModel>
#include <QDockWidget>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include "../sigsession.h"
#include "../view/view.h"
#include "../view/logicsignal.h"
#include "../data/logicsnapshot.h"
#include "../dialogs/searchdialog.h"
#include "../search/searchjob.h"
#include "../search/snapshotsource.h"
#include "../trigger/decoderbus.h"
#include "../config/appconfig.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"
#include "../log.h"

namespace pv {
namespace dock {

using namespace pv::search;
using dialogs::SearchDialog;
using dialogs::SearchSignal;

namespace {

// The most matches the list keeps; the search stops there.
const uint64_t MaxListed = 1000000;

QString T(const char *id, const char *fallback)
{
    return SearchDialog::text(id, fallback);
}

// A time with an SI prefix, to the sample period's resolution.
QString format_seconds(double seconds, double period, bool sign)
{
    static const char *const units[] = {"s", "ms", "µs", "ns", "ps"};
    if (seconds == 0)
        return QStringLiteral("0");
    const double a = std::fabs(seconds);
    int u = 0;
    double scale = 1;
    while (u < 4 && a * scale < 1){
        scale *= 1000;
        u++;
    }
    int decimals = 3;
    if (period > 0)
        decimals = (int)std::ceil(-std::log10(period * scale) - 1e-9);
    decimals = std::max(0, std::min(9, decimals));
    QString s = QString::number(seconds * scale, 'f', decimals);
    if (sign && seconds > 0)
        s.prepend(QLatin1Char('+'));
    return s + QLatin1Char(' ') + QString::fromUtf8(units[u]);
}

} // namespace

//------------------------------------------------------------- rows

const int64_t SearchRows::NoValue;

void SearchRows::append(const Match &m, const std::vector<SearchColumn> &columns)
{
    start.push_back(m.start);
    end.push_back(m.end);
    flags.push_back((m.start_unknown ? StartUnknown : 0) | (m.end_unknown ? EndUnknown : 0));
    for (const SearchColumn &c : columns){
        const ConditionValue &v = m.values[c.condition];
        if (c.count)
            values.push_back((int64_t)v.count);
        else
            values.push_back(v.has_delta ? v.delta : NoValue);
    }
}

void SearchRows::clear()
{
    std::vector<uint64_t>().swap(start);
    std::vector<uint64_t>().swap(end);
    std::vector<uint8_t>().swap(flags);
    std::vector<int64_t>().swap(values);
}

//------------------------------------------------------------- model

class SearchResultModel : public QAbstractTableModel
{
public:
    explicit SearchResultModel(QObject *parent) : QAbstractTableModel(parent) {}

    void reset(const std::vector<SearchColumn> &columns, double rate, bool state)
    {
        beginResetModel();
        _rows.clear();
        _columns = columns;
        _rate = rate;
        _state = state;
        _order.clear();
        _inverse.clear();
        _stats.assign(columns.size() + 1, Stat());
        endResetModel();
    }

    void clear()
    {
        reset(std::vector<SearchColumn>(), _rate, false);
    }

    void append(const SearchRows &rows)
    {
        if (rows.size() == 0)
            return;
        const size_t first = _rows.size();
        const size_t stride = _columns.size();
        beginInsertRows(QModelIndex(), (int)first, (int)(first + rows.size() - 1));
        _rows.start.insert(_rows.start.end(), rows.start.begin(), rows.start.end());
        _rows.end.insert(_rows.end.end(), rows.end.begin(), rows.end.end());
        _rows.flags.insert(_rows.flags.end(), rows.flags.begin(), rows.flags.end());
        _rows.values.insert(_rows.values.end(), rows.values.begin(), rows.values.end());
        if (!_order.empty()){
            for (size_t i = first; i < _rows.size(); i++){
                _inverse.push_back((uint32_t)_order.size());
                _order.push_back((uint32_t)i);
            }
        }
        endInsertRows();

        for (size_t i = 0; i < rows.size(); i++){
            if (_state && !(rows.flags[i] & (SearchRows::StartUnknown | SearchRows::EndUnknown)))
                _stats[0].add((double)(rows.end[i] - rows.start[i]));
            for (size_t c = 0; c < stride; c++){
                const int64_t v = rows.values[i * stride + c];
                if (v != SearchRows::NoValue)
                    _stats[c + 1].add((double)v);
            }
        }
    }

    inline size_t size() const { return _rows.size(); }
    inline const std::vector<uint64_t> &starts() const { return _rows.start; }

    int display_row(size_t index) const
    {
        return _order.empty() ? (int)index : (int)_inverse[index];
    }

    size_t time_index(int row) const
    {
        return _order.empty() ? (size_t)row : _order[row];
    }

    QString stats_text() const
    {
        QStringList parts;
        const double period = _rate > 0 ? 1.0 / _rate : 0;
        auto add = [&](const QString &name, const Stat &s, bool count){
            if (s.n == 0)
                return;
            auto f = [&](double v){
                return count ? QString::number(v, 'f', v == std::floor(v) ? 0 : 1)
                             : format_seconds(v * period, period, false);
            };
            parts.append(T("IDS_DLG_SRCH_STATS", "%1: min %2, mean %3, max %4")
                         .arg(name, f(s.min), f(s.sum / s.n), f(s.max)));
        };
        if (_state)
            add(T("IDS_DLG_SRCH_COL_DURATION", "Duration"), _stats[0], false);
        for (size_t c = 0; c < _columns.size(); c++)
            add(column_name(c), _stats[c + 1], _columns[c].count);
        return parts.join(QStringLiteral("  ·  "));
    }

    int rowCount(const QModelIndex &parent) const override
    {
        return parent.isValid() ? 0 : (int)_rows.size();
    }

    int columnCount(const QModelIndex &parent) const override
    {
        return parent.isValid() ? 0 : 2 + (_state ? 1 : 0) + (int)_columns.size();
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return QVariant();
        if (section == 0)
            return QStringLiteral("#");
        if (section == 1)
            return T("IDS_DLG_SRCH_COL_TIME", "Time");
        if (_state && section == 2)
            return T("IDS_DLG_SRCH_COL_DURATION", "Duration");
        return column_name(section - 2 - (_state ? 1 : 0));
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid())
            return QVariant();
        if (role == Qt::TextAlignmentRole)
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        if (role != Qt::DisplayRole)
            return QVariant();

        const size_t i = time_index(index.row());
        const double period = _rate > 0 ? 1.0 / _rate : 0;
        const int col = index.column();
        if (col == 0)
            return QString::number(i + 1);
        if (col == 1){
            QString s = format_seconds(_rows.start[i] * period, period, false);
            if (_rows.flags[i] & SearchRows::StartUnknown)
                s.prepend(QStringLiteral("≤ "));
            return s;
        }
        if (_state && col == 2){
            QString s = format_seconds((_rows.end[i] - _rows.start[i]) * period, period, false);
            if (_rows.flags[i] & (SearchRows::StartUnknown | SearchRows::EndUnknown))
                s.prepend(QStringLiteral("≥ "));
            return s;
        }
        const size_t c = col - 2 - (_state ? 1 : 0);
        const int64_t v = _rows.values[i * _columns.size() + c];
        if (_columns[c].count)
            return QString::number(v);
        if (v == SearchRows::NoValue)
            return QString();
        return format_seconds(v * period, period, true);
    }

    void sort(int column, Qt::SortOrder order) override
    {
        const size_t n = _rows.size();
        beginResetModel();
        _order.clear();
        _inverse.clear();
        const bool by_time = column <= 1;
        if (!(by_time && order == Qt::AscendingOrder) && n > 0){
            _order.resize(n);
            std::iota(_order.begin(), _order.end(), 0);
            if (by_time)
                std::reverse(_order.begin(), _order.end());
            else {
                const bool duration = _state && column == 2;
                const size_t c = duration ? 0 : column - 2 - (_state ? 1 : 0);
                const size_t stride = _columns.size();
                auto key = [&](uint32_t i) -> int64_t {
                    if (duration)
                        return (int64_t)(_rows.end[i] - _rows.start[i]);
                    return _rows.values[i * stride + c];
                };
                const bool asc = order == Qt::AscendingOrder;
                std::stable_sort(_order.begin(), _order.end(), [&](uint32_t a, uint32_t b){
                    const int64_t ka = key(a), kb = key(b);
                    // Empty values last either way.
                    if (ka == SearchRows::NoValue || kb == SearchRows::NoValue)
                        return kb == SearchRows::NoValue && ka != SearchRows::NoValue;
                    return asc ? ka < kb : ka > kb;
                });
            }
            _inverse.resize(n);
            for (size_t r = 0; r < n; r++)
                _inverse[_order[r]] = (uint32_t)r;
        }
        endResetModel();
    }

private:
    struct Stat {
        uint64_t n = 0;
        double min = 0, max = 0, sum = 0;
        void add(double v)
        {
            if (n == 0 || v < min)
                min = v;
            if (n == 0 || v > max)
                max = v;
            sum += v;
            n++;
        }
    };

    QString column_name(size_t c) const
    {
        const int n = _columns[c].condition + 1;
        return _columns[c].count ? T("IDS_DLG_SRCH_COL_COUNT", "Count %1").arg(n)
                                 : T("IDS_DLG_SRCH_COL_DELTA", "Δ %1").arg(n);
    }

    SearchRows _rows;
    std::vector<SearchColumn> _columns;
    double _rate = 0;
    bool _state = false;
    std::vector<uint32_t> _order;       // display row -> match, when sorted
    std::vector<uint32_t> _inverse;
    std::vector<Stat> _stats;           // duration, then the columns
};

//------------------------------------------------------------- dock

SearchDock::SearchDock(QWidget *parent, view::View &view, SigSession *session) :
    QWidget(parent),
    _session(session),
    _view(view),
    _results_snapshot(nullptr),
    _results_version(0),
    _have_results(false),
    _pending_jump(0),
    _current(-1),
    _polled_progress(0)
{
    _job = new SearchJob(this);
    connect(_job, &SearchJob::finished, this, &SearchDock::on_finished);
    _model = new SearchResultModel(this);

    _edit_button = new QPushButton(this);
    _edit_button->setFlat(true);
    _edit_button->setMinimumWidth(240);
    _pre_button = new QPushButton(this);
    _nxt_button = new QPushButton(this);
    _run_button = new QPushButton(this);
    _list_button = new QPushButton(this);
    _list_button->setCheckable(true);
    _status = new QLabel(this);
    _progress = new QProgressBar(this);
    _progress->setRange(0, 1000);
    _progress->setTextVisible(false);
    _progress->setFixedWidth(100);
    _progress->setMaximumHeight(10);
    _progress->hide();

    _table = new QTableView(this);
    _table->setModel(_model);
    _table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _table->setSelectionMode(QAbstractItemView::SingleSelection);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _table->verticalHeader()->hide();
    _table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    _table->horizontalHeader()->setStretchLastSection(true);
    _table->setMinimumHeight(160);
    _table->hide();
    _stats = new QLabel(this);
    _stats->setWordWrap(true);
    _stats->hide();

    QHBoxLayout *strip = new QHBoxLayout();
    strip->setContentsMargins(0, 0, 0, 0);
    strip->addWidget(_pre_button);
    strip->addWidget(_edit_button, 1);
    strip->addWidget(_nxt_button);
    strip->addWidget(_run_button);
    strip->addWidget(_progress);
    strip->addWidget(_status);
    strip->addStretch(1);
    strip->addWidget(_list_button);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignTop);
    layout->addLayout(strip);
    layout->addWidget(_table, 1);
    layout->addWidget(_stats);

    connect(_edit_button, &QPushButton::clicked, this, &SearchDock::on_edit);
    connect(_pre_button, &QPushButton::clicked, this, &SearchDock::on_previous);
    connect(_nxt_button, &QPushButton::clicked, this, &SearchDock::on_next);
    connect(_run_button, &QPushButton::clicked, this, &SearchDock::on_run);
    connect(_list_button, &QPushButton::toggled, this, &SearchDock::on_list);
    connect(_table, &QTableView::clicked, this, &SearchDock::on_row_activated);
    connect(_table, &QTableView::activated, this, &SearchDock::on_row_activated);
    _poll.setInterval(100);
    connect(&_poll, &QTimer::timeout, this, &SearchDock::on_poll);

    _session->add_msg_listener(this);
    ADD_UI(this);
}

SearchDock::~SearchDock()
{
    // The search thread calls back into this object.
    _job->stop();
    _session->remove_msg_listener(this);
    REMOVE_UI(this);
}

void SearchDock::retranslateUi()
{
    _pre_button->setToolTip(T("IDS_DLG_SRCH_PREVIOUS", "Previous match"));
    _nxt_button->setToolTip(T("IDS_DLG_SRCH_NEXT", "Next match"));
    _edit_button->setToolTip(T("IDS_DLG_SRCH_EDIT", "Edit the search"));
    _run_button->setText(_job->is_running() ? T("IDS_DLG_SRCH_STOP", "Stop")
                                            : T("IDS_DLG_SRCH_RUN", "Search"));
    _list_button->setText(T("IDS_DLG_SRCH_LIST", "Results"));
    _model->headerDataChanged(Qt::Horizontal, 0, std::max(0, _model->columnCount(QModelIndex()) - 1));
    update_summary();
    update_status();
}

void SearchDock::reStyle()
{
    const QString iconPath = GetIconPath();
    _pre_button->setIcon(QIcon(iconPath + "/pre.svg"));
    _nxt_button->setIcon(QIcon(iconPath + "/next.svg"));
    _edit_button->setIcon(QIcon(iconPath + "/search.svg"));
}

void SearchDock::UpdateLanguage()
{
    retranslateUi();
}

void SearchDock::UpdateTheme()
{
    reStyle();
}

void SearchDock::UpdateFont()
{
    QFont font = this->font();
    font.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    for (QWidget *w : {(QWidget *)_edit_button, (QWidget *)_status, (QWidget *)_table,
                       (QWidget *)_stats, (QWidget *)_run_button, (QWidget *)_list_button})
        w->setFont(font);
    update_summary();
}

QJsonObject SearchDock::get_session()
{
    return to_json(_query);
}

void SearchDock::set_session(const QJsonObject &obj)
{
    Query q;
    QString error;
    if (!obj.isEmpty() && !from_json(obj, q, error)){
        dsv_warn("Ignoring the saved search: %s", error.toUtf8().data());
        return;
    }
    if (obj.isEmpty())
        return;
    clear_results();
    _query = q;
    update_summary();
}

void SearchDock::OnMessage(int msg)
{
    switch (msg){
    case DSV_MSG_START_COLLECT_WORK_PREV:
    case DSV_MSG_CURRENT_DEVICE_CHANGE_PREV:
    case DSV_MSG_DEVICE_MODE_CHANGED:
    case DSV_MSG_DEVICE_OPTIONS_UPDATED:
    case DSV_MSG_DATA_POOL_CHANGED:
        _message.clear();
        clear_results();
        update_summary();
        break;
    default:
        break;
    }
}

//------------------------------------------------------------- sigs

namespace {

struct BusList
{
    std::vector<SearchSignal> sigs;
    std::map<QString, std::vector<int>> bits;
};

BusList collect_buses(SigSession *session)
{
    BusList list;
    for (const trigger::DecoderBus &bus : trigger::parallel_decoder_buses(*session)){
        if (bus.mapping.isEmpty())
            continue;
        QString name = bus.name;
        for (int n = 2; list.bits.count(name); n++)
            name = QStringLiteral("%1 (%2)").arg(bus.name).arg(n);
        list.bits[name] = std::vector<int>(bus.mapping.begin(), bus.mapping.end());
        SearchSignal s;
        s.name = name;
        s.bus = name;
        s.width = bus.mapping.size();
        list.sigs.push_back(s);
    }
    return list;
}

} // namespace

std::vector<SearchSignal> SearchDock::search_signals()
{
    // Buses first: there are few of them, and many channels.
    std::vector<SearchSignal> list = collect_buses(_session).sigs;
    for (auto s : _session->get_signals()){
        if (s->signal_type() != SR_CHANNEL_LOGIC)
            continue;
        SearchSignal ss;
        ss.name = s->get_name();
        ss.channel = s->get_index();
        list.push_back(ss);
    }
    return list;
}

BusResolver SearchDock::bus_resolver()
{
    const BusList buses = collect_buses(_session);
    const std::map<QString, std::vector<int>> bits = buses.bits;
    return [bits](const QString &name, std::vector<int> &out){
        auto it = bits.find(name);
        if (it == bits.end())
            return false;
        out = it->second;
        return true;
    };
}

data::LogicSnapshot *SearchDock::logic_snapshot()
{
    if (_session->get_device()->get_work_mode() != LOGIC)
        return nullptr;
    return dynamic_cast<data::LogicSnapshot *>(_session->get_snapshot(SR_CHANNEL_LOGIC));
}

//------------------------------------------------------------- searching

bool SearchDock::results_current()
{
    if (!_have_results || _session->is_working())
        return false;
    data::LogicSnapshot *snapshot = logic_snapshot();
    return snapshot != nullptr && snapshot == _results_snapshot
        && snapshot->data_version() == _results_version;
}

bool SearchDock::start_search()
{
    _job->stop();
    _pending_jump = 0;

    if (_session->is_working()){
        set_status(T("IDS_DLG_SRCH_BUSY", "Stop the capture to search."));
        return false;
    }
    data::LogicSnapshot *snapshot = logic_snapshot();
    if (snapshot == nullptr || snapshot->get_ring_sample_count() == 0){
        MsgBox::Show(QString(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NO_SAMPLE_DATA), "No Sample data!")));
        return false;
    }
    if (_query.find.empty()){
        on_edit();
        return false;
    }

    std::unique_ptr<SnapshotSource> source(new SnapshotSource(snapshot));
    SnapshotSource *src = source.get();
    const double rate = src->samplerate();
    Program program;
    Error error;
    if (rate <= 0)
        error.code = ErrorCode::NoSamplerate;
    else
        compile(_query, bus_resolver(), [src](int ch){ return src->has_channel(ch); },
                rate, program, error);
    if (error){
        MsgBox::Show(SearchDialog::error_text(error, search_signals()));
        return false;
    }

    clear_results();
    _program = program;
    _columns.clear();
    for (size_t i = 0; i < program.conditions.size(); i++){
        const ConditionSpec &c = program.conditions[i];
        if (reports_value(c))
            _columns.push_back(SearchColumn{(int)i, has_count(c.quantifier)});
    }
    _model->reset(_columns, rate, program.state);
    _table->setSortingEnabled(false);
    // In time order until a column is clicked (Qt's default is descending).
    _table->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    _results_snapshot = snapshot;
    _results_version = src->version();
    _have_results = true;
    _polled_progress = 0;
    _message.clear();

    _job->start(program, std::move(source), 0, UINT64_MAX, MaxListed, [this](const Match &m){
        std::lock_guard<std::mutex> lock(_pending_lock);
        _pending.append(m, _columns);
    });
    _poll.start();
    _progress->setValue(0);
    _progress->show();
    _run_button->setText(T("IDS_DLG_SRCH_STOP", "Stop"));
    update_status();
    return true;
}

void SearchDock::clear_results()
{
    _job->stop();
    _poll.stop();
    {
        std::lock_guard<std::mutex> lock(_pending_lock);
        _pending.clear();
    }
    _model->clear();
    _have_results = false;
    _results_snapshot = nullptr;
    _current = -1;
    _pending_jump = 0;
    _progress->hide();
    _run_button->setText(T("IDS_DLG_SRCH_RUN", "Search"));
    _stats->clear();
    update_status();
}

void SearchDock::on_poll()
{
    // Matches before this point are in what is taken below.
    const uint64_t progress = _job->progress();
    SearchRows rows;
    {
        std::lock_guard<std::mutex> lock(_pending_lock);
        std::swap(rows, _pending);
    }
    _model->append(rows);
    _polled_progress = progress;

    if (_job->is_running() && _job->total() > 0)
        _progress->setValue((int)(1000.0 * _job->progress() / _job->total()));
    if (_pending_jump != 0)
        jump(_pending_jump);
    _stats->setText(_model->stats_text());
    update_status();
}

void SearchDock::on_finished()
{
    _poll.stop();
    on_poll();

    const RunResult &r = _job->result();
    switch (r.status){
    case RunStatus::Done:
        break;
    case RunStatus::Limit:
        _message = T("IDS_DLG_SRCH_LIMIT", "Stopped after %1 matches.").arg(r.matches);
        break;
    case RunStatus::Cancelled:
        _message = T("IDS_DLG_SRCH_CANCELLED", "Search stopped.");
        break;
    case RunStatus::DataChanged:
        _message = T("IDS_DLG_SRCH_CHANGED", "The data changed during the search.");
        break;
    case RunStatus::TooMuchData:
        _message = T("IDS_DLG_SRCH_TOOMUCH", "Stopped: a window holds too many events. Narrow the search.");
        break;
    }
    _progress->hide();
    _run_button->setText(T("IDS_DLG_SRCH_RUN", "Search"));
    _table->setSortingEnabled(true);
    if (_pending_jump != 0)
        jump(_pending_jump);
    _stats->setText(_model->stats_text());
    update_status();
}

void SearchDock::jump(int direction)
{
    if (!results_current()){
        // On another search, or none yet: search, then go.
        if (!_job->is_running() || !_have_results){
            if (!start_search())
                return;
        }
        else
            return;
        _pending_jump = direction;
        return;
    }

    const std::vector<uint64_t> &starts = _model->starts();
    const uint64_t pos = _view.get_search_pos();
    const bool running = _job->is_running();

    if (direction > 0){
        // At a match, the next one; otherwise the first at or after the cursor.
        auto it = _view.get_search_hit() ? std::upper_bound(starts.begin(), starts.end(), pos)
                                         : std::lower_bound(starts.begin(), starts.end(), pos);
        if (it != starts.end()){
            _pending_jump = 0;
            jump_to(it - starts.begin());
            return;
        }
    }
    else {
        // Once the search is past the cursor, later matches can't change
        // the one before it. Only what on_poll() took counts.
        const bool settled = !running || _polled_progress >= pos;
        auto it = std::lower_bound(starts.begin(), starts.end(), pos);
        if (settled && it != starts.begin()){
            _pending_jump = 0;
            jump_to(it - starts.begin() - 1);
            return;
        }
        if (settled && !running){
            _pending_jump = 0;
            _message = T("IDS_DLG_SRCH_NOPREV", "No matches before the cursor.");
            update_status();
            return;
        }
    }

    if (running){
        _pending_jump = direction;
        return;
    }
    _pending_jump = 0;
    _message = direction > 0 ? T("IDS_DLG_SRCH_NONEXT", "No matches after the cursor.")
                             : T("IDS_DLG_SRCH_NOPREV", "No matches before the cursor.");
    update_status();
}

bool SearchDock::jump_to(size_t index)
{
    if (index >= _model->size())
        return false;
    _current = (int64_t)index;
    _message.clear();
    _view.set_search_pos(_model->starts()[index], true);
    const int row = _model->display_row(index);
    if (_table->isVisible()){
        _table->selectRow(row);
        _table->scrollTo(_model->index(row, 0));
    }
    update_status();
    return true;
}

//------------------------------------------------------------- slots

void SearchDock::on_edit()
{
    const std::vector<SearchSignal> sigs = search_signals();
    dialogs::SearchDialog dlg(this, sigs, bus_resolver(), _query);
    connect(_session->device_event_object(), SIGNAL(device_updated()), &dlg, SLOT(reject()));

    if (!dlg.exec())
        return;
    _query = dlg.query();
    update_summary();
    clear_results();
    _message.clear();

    data::LogicSnapshot *snapshot = logic_snapshot();
    if (!_session->is_working() && snapshot != nullptr && snapshot->get_ring_sample_count() > 0)
        start_search();
}

void SearchDock::on_previous()
{
    jump(-1);
}

void SearchDock::on_next()
{
    jump(1);
}

void SearchDock::on_run()
{
    if (_job->is_running()){
        _job->stop();
        _poll.stop();
        on_poll();
        _pending_jump = 0;
        _progress->hide();
        _run_button->setText(T("IDS_DLG_SRCH_RUN", "Search"));
        _table->setSortingEnabled(true);
        _message = T("IDS_DLG_SRCH_CANCELLED", "Search stopped.");
        update_status();
        return;
    }
    start_search();
}

void SearchDock::on_list(bool show)
{
    _table->setVisible(show);
    _stats->setVisible(show);

    // The bottom dock keeps its height on its own; fit it to what shows.
    QDockWidget *dock = qobject_cast<QDockWidget *>(parentWidget());
    QMainWindow *window = dock ? qobject_cast<QMainWindow *>(dock->parentWidget()) : nullptr;
    if (window){
        layout()->activate();
        const int height = show ? std::max(dock->minimumSizeHint().height(), 320)
                                : dock->minimumSizeHint().height();
        window->resizeDocks({dock}, {height}, Qt::Vertical);
    }
    if (show && _current >= 0 && (size_t)_current < _model->size()){
        const int row = _model->display_row((size_t)_current);
        _table->selectRow(row);
        _table->scrollTo(_model->index(row, 0));
    }
}

void SearchDock::on_row_activated(const QModelIndex &index)
{
    if (!index.isValid() || !results_current())
        return;
    jump_to(_model->time_index(index.row()));
}

//------------------------------------------------------------- texts

void SearchDock::update_summary()
{
    QString text;
    if (_query.find.empty())
        text = T("IDS_DLG_SRCH_SETUP", "Set up a search...");
    else
        text = SearchDialog::summary(_query, search_signals());
    _edit_button->setToolTip(text);
    const QFontMetrics fm(_edit_button->font());
    _edit_button->setText(fm.elidedText(text, Qt::ElideRight, 420));
}

void SearchDock::set_status(const QString &text)
{
    _message = text;
    update_status();
}

void SearchDock::update_status()
{
    QString s;
    const size_t n = _model->size();
    if (_have_results){
        if (n == 1)
            s = T("IDS_DLG_SRCH_ONE", "1 match");
        else if (n == 0 && !_job->is_running())
            s = T("IDS_DLG_SRCH_NONE", "No matches");
        else
            s = T("IDS_DLG_SRCH_COUNT", "%1 matches").arg(n);
        if (_job->is_running())
            s = T("IDS_DLG_SRCH_RUNNING", "Searching... %1").arg(s);
        else if (_current >= 0 && (size_t)_current < n)
            s = T("IDS_DLG_SRCH_POSITION", "%1 of %2").arg(_current + 1).arg(s);
        if (!results_current() && !_job->is_running())
            s += QStringLiteral(" (%1)").arg(T("IDS_DLG_SRCH_STALE", "old data"));
    }
    if (!_message.isEmpty())
        s = s.isEmpty() ? _message : s + QStringLiteral(" · ") + _message;
    _status->setText(s);
}

} // namespace dock
} // namespace pv
