/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#ifndef DSVIEW_PV_DIALOGS_SEARCHDIALOG_H
#define DSVIEW_PV_DIALOGS_SEARCHDIALOG_H

#include <vector>
#include <QString>

#include "dsdialog.h"
#include "../search/searchengine.h"

class QComboBox;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class QWidget;

namespace pv {
namespace dialogs {

// A signal a search term can test: a logic channel or a parallel
// decoder's bus.
struct SearchSignal
{
    QString name;
    int channel = -1;
    QString bus;
    int width = 1;
};

class SearchPatternList;
class SearchConditionBox;

// Edits a search query (doc/search.md).
class SearchDialog : public DSDialog
{
public:
    SearchDialog(QWidget *parent, const std::vector<SearchSignal> &sigs,
                 const search::BusResolver &buses, const search::Query &query);

    search::Query query() const;

    // Translated texts, shared with the search dock.
    static QString text(const char *id, const char *fallback);
    static QString error_text(const search::Error &error, const std::vector<SearchSignal> &sigs);
    static QString summary(const search::Query &query, const std::vector<SearchSignal> &sigs);
    static QString signal_label(const SearchSignal &signal);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void on_ok();
    void on_kind_changed();
    void add_condition(const search::Condition &condition);
    void remove_condition(SearchConditionBox *box);
    void renumber();

    std::vector<SearchSignal> _signals;
    search::BusResolver _buses;

    QComboBox *_kind;
    SearchPatternList *_find;
    QWidget *_duration_row;
    QLineEdit *_min_duration;
    QLineEdit *_max_duration;
    QVBoxLayout *_conditions_layout;
    std::vector<SearchConditionBox *> _conditions;
    QPushButton *_add_condition;
    QLabel *_error;
};

} // namespace dialogs
} // namespace pv

#endif
