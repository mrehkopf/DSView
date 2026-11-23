/*
 * This file is part of DSView, licensed under GPLv2 or later.
 */

#include "searchdialog.h"

#include <algorithm>
#include <functional>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QScreen>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

#include "../ui/dscombobox.h"
#include "../ui/langresource.h"

namespace pv {
namespace dialogs {

using namespace pv::search;

namespace {

QString T(const char *id, const char *fallback)
{
    return SearchDialog::text(id, fallback);
}

const char *const ErrorStyle = "QLineEdit { color: #e04848; }";

QString channel_hint()
{
    return T("IDS_DLG_SRCH_HINT_CHANNEL", "0, 1, X, R, F or C");
}

QString bus_hint()
{
    return T("IDS_DLG_SRCH_HINT_BUS", "0x21XX, 0b10XR, !=FF, 10..1F, changes");
}

QString time_hint()
{
    return T("IDS_DLG_SRCH_HINT_TIME", "e.g. 100ns");
}

void set_error(QLineEdit *edit, const QString &message)
{
    edit->setStyleSheet(message.isEmpty() ? QString() : QString::fromLatin1(ErrorStyle));
    edit->setToolTip(message);
}

// A monospaced family for example values. The system's fixed font is the
// first choice, but some platforms (Qt's VNC and offscreen backends) report
// a proportional one, and rich text resolves neither <code> nor aliases such
// as "monospace" reliably.
QString fixed_family()
{
    const QFontInfo info(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    if (info.fixedPitch())
        return info.family();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QStringList families = QFontDatabase::families();
#else
    const QStringList families = QFontDatabase().families();
#endif
    for (const char *name : {"DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono",
                             "Consolas", "Menlo", "Courier New"}){
        if (families.contains(QLatin1String(name)))
            return QLatin1String(name);
    }
    return info.family();
}

// Example values, as typed: "0x21XX", "$21XX".
QString codes(std::initializer_list<const char *> list)
{
    static const QString family = fixed_family().toHtmlEscaped();
    QStringList parts;
    for (const char *c : list)
        parts.append(QStringLiteral("<span style=\"font-family:'%1'\">%2</span>")
                         .arg(family, QString::fromUtf8(c).toHtmlEscaped()));
    // A space in the normal font gives the line the same height and
    // baseline as the description next to it; rich text has no baseline
    // alignment for table cells.
    return parts.join(QStringLiteral(", ")) + QStringLiteral("&nbsp;");
}

// A heading and a table of examples and what they mean.
QLabel *help_section(QWidget *parent, const QString &title,
                     const std::vector<std::pair<QString, QString>> &rows)
{
    QString html = QStringLiteral("<b>%1</b><table cellspacing=\"0\" cellpadding=\"1\">")
                       .arg(title.toHtmlEscaped());
    for (const auto &r : rows)
        html += QStringLiteral("<tr><td style=\"white-space:nowrap; padding-right:14px\">%1</td>"
                               "<td>%2</td></tr>").arg(r.first, r.second.toHtmlEscaped());
    html += QStringLiteral("</table>");
    QLabel *label = new QLabel(html, parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    return label;
}

// What can be typed into the fields, and how patterns match.
QWidget *help_box(QWidget *parent)
{
    QGroupBox *box = new QGroupBox(T("IDS_DLG_SRCH_HELP_TITLE", "Syntax"), parent);

    QLabel *channels = help_section(box, T("IDS_DLG_SRCH_HELP_CHANNELS", "Channel values"), {
        {codes({"0", "1"}), T("IDS_DLG_SRCH_HELP_LEVELS", "low, high")},
        {codes({"X"}), T("IDS_DLG_SRCH_HELP_DONTCARE", "don't care")},
        {codes({"R", "F"}), T("IDS_DLG_SRCH_HELP_EDGES", "rising, falling edge")},
        {codes({"C"}), T("IDS_DLG_SRCH_HELP_EITHER", "either edge")},
    });
    QLabel *times = help_section(box, T("IDS_DLG_SRCH_HELP_TIMES", "Times"), {
        {codes({"3ms", "2.5us", "100ns"}), T("IDS_DLG_SRCH_HELP_UNITS", "s, ms, us, ns, ps")},
        {codes({"-100ns"}), T("IDS_DLG_SRCH_HELP_NEGATIVE", "before the match")},
        {codes({"40sa"}), T("IDS_DLG_SRCH_HELP_SAMPLES", "samples")},
    });
    QLabel *buses = help_section(box, T("IDS_DLG_SRCH_HELP_BUSES", "Bus values (D0 is the LSB)"), {
        {codes({"0x21XX", "$21XX"}), T("IDS_DLG_SRCH_HELP_HEX", "hexadecimal; an X digit is four don't-care bits")},
        {codes({"0b0010_00XR"}), T("IDS_DLG_SRCH_HELP_BINARY", "binary: 0, 1, X, R, F or C per bit")},
        {codes({"!= 0xFF"}), T("IDS_DLG_SRCH_HELP_NOTEQUAL", "not this value")},
        {codes({"< 0x8000", "<=", ">", ">="}), T("IDS_DLG_SRCH_HELP_COMPARE", "compares the value")},
        {codes({"0x2100..0x213F"}), T("IDS_DLG_SRCH_HELP_RANGE", "range, both ends included")},
        {codes({"changes"}), T("IDS_DLG_SRCH_HELP_CHANGES", "any bit changes")},
    });

    QString rules = QStringLiteral("<b>%1</b>").arg(T("IDS_DLG_SRCH_HELP_MATCHING", "How patterns match").toHtmlEscaped());
    for (const QString &line : {
             T("IDS_DLG_SRCH_HELP_MATCH_LEVELS", "Levels alone match where they become true."),
             T("IDS_DLG_SRCH_HELP_MATCH_EDGES", "With edges, a pattern matches where its edges happen while its levels hold."),
             T("IDS_DLG_SRCH_HELP_MATCH_WINDOWS", "Time windows include both ends. Start one at 1sa to leave out the match itself.")})
        rules += QStringLiteral("<br>• %1").arg(line.toHtmlEscaped());
    QLabel *matching = new QLabel(rules, box);
    matching->setTextFormat(Qt::RichText);
    matching->setWordWrap(true);

    QGridLayout *g = new QGridLayout(box);
    QMargins m = g->contentsMargins();
    m.setTop(m.top() + 12);
    g->setContentsMargins(m);
    g->setHorizontalSpacing(28);
    g->setVerticalSpacing(10);
    // The short tables side by side, the bus table across the whole width.
    g->addWidget(channels, 0, 0);
    g->addWidget(times, 0, 1);
    g->addWidget(buses, 1, 0, 1, 2);
    g->addWidget(matching, 2, 0, 1, 2);
    g->setColumnStretch(0, 1);
    g->setColumnStretch(1, 1);
    return box;
}

} // namespace

//------------------------------------------------------------- pattern list

// Patterns, any of which may match, each a list of signal = value terms.
class SearchPatternList : public QWidget
{
public:
    SearchPatternList(QWidget *parent, const std::vector<SearchSignal> *sigs, bool state) :
        QWidget(parent), _signals(sigs), _state(state)
    {
        _layout = new QVBoxLayout(this);
        _layout->setContentsMargins(0, 0, 0, 0);
        set_patterns(Patterns());
    }

    void set_patterns(const Patterns &patterns)
    {
        _patterns = patterns;
        if (_patterns.empty())
            _patterns.push_back(Pattern(1, default_term(Pattern())));
        rebuild();
    }

    Patterns patterns() const
    {
        Patterns ps;
        for (const auto &rows : _rows){
            Pattern p;
            for (const Row &r : rows)
                p.push_back(read(r));
            ps.push_back(p);
        }
        return ps;
    }

    void set_state(bool state)
    {
        _state = state;
        for (auto &rows : _rows)
            for (Row &r : rows)
                validate(r);
    }

private:
    struct Row {
        DsComboBox *signal;
        QLineEdit *value;
        Term missing;
    };

    static bool matches(const SearchSignal &s, const Term &t)
    {
        if (!t.bits.empty())
            return false;
        if (t.is_channel())
            return s.bus.isEmpty() && s.channel == t.channel;
        return s.bus == t.bus;
    }

    int signal_of(const Term &t) const
    {
        for (size_t i = 0; i < _signals->size(); i++){
            if (matches((*_signals)[i], t))
                return (int)i;
        }
        return -1;
    }

    Term default_term(const Pattern &after) const
    {
        Term t;
        if (_signals->empty()){
            t.channel = 0;
            return t;
        }
        int next = 0;
        if (!after.empty()){
            const int last = signal_of(after.back());
            if (last >= 0)
                next = (last + 1) % (int)_signals->size();
        }
        const SearchSignal &s = (*_signals)[next];
        if (s.bus.isEmpty())
            t.channel = s.channel;
        else
            t.bus = s.bus;
        return t;
    }

    Term read(const Row &r) const
    {
        Term t;
        const int i = r.signal->currentData().toInt();
        if (i >= 0 && i < (int)_signals->size()){
            const SearchSignal &s = (*_signals)[i];
            if (s.bus.isEmpty())
                t.channel = s.channel;
            else
                t.bus = s.bus;
        }
        else
            t = r.missing;
        t.value = r.value->text();
        return t;
    }

    void validate(Row &r)
    {
        const Term t = read(r);
        const int i = r.signal->currentData().toInt();
        int width = 0;
        bool channel = t.is_channel();
        if (i >= 0 && i < (int)_signals->size())
            width = (*_signals)[i].bus.isEmpty() ? 1 : (*_signals)[i].width;
        else if (!t.bits.empty())
            width = (int)t.bits.size();

        r.value->setPlaceholderText(channel ? channel_hint() : bus_hint());

        Error e;
        e.detail = t.value;
        bool edge = false;
        if (channel){
            Req req;
            e.code = parse_channel_value(t.value, req);
            edge = e.code == ErrorCode::None && is_edge(req);
        }
        else if (width > 0){
            BusValue v;
            e.code = parse_bus_value(t.value, width, v);
            edge = e.code == ErrorCode::None && v.has_edge();
        }
        if (_state && edge)
            e.code = ErrorCode::EdgeInState;
        // A missing value is reported on OK; while typing it is not an error.
        if (e.code == ErrorCode::EmptyValue)
            e.code = ErrorCode::None;
        set_error(r.value, e.code == ErrorCode::None ? QString()
                                                    : SearchDialog::error_text(e, *_signals));
    }

    void sync()
    {
        _patterns = patterns();
    }

    void rebuild()
    {
        if (_body){
            _body->hide();
            _body->deleteLater();
        }
        _rows.clear();
        _body = new QWidget(this);
        QVBoxLayout *v = new QVBoxLayout(_body);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);

        for (size_t a = 0; a < _patterns.size(); a++){
            if (a > 0){
                QLabel *label = new QLabel(T("IDS_DLG_SRCH_OR", "or"), _body);
                label->setAlignment(Qt::AlignHCenter);
                v->addWidget(label);
            }
            QFrame *frame = new QFrame(_body);
            frame->setFrameShape(QFrame::StyledPanel);
            QGridLayout *g = new QGridLayout(frame);
            g->setContentsMargins(6, 6, 6, 6);
            g->setVerticalSpacing(4);

            std::vector<Row> rows;
            const Pattern &p = _patterns[a];
            for (size_t k = 0; k < p.size(); k++){
                const Term &t = p[k];
                Row r;
                r.signal = new DsComboBox(frame);
                r.signal->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                r.signal->setMinimumContentsLength(10);
                for (size_t i = 0; i < _signals->size(); i++)
                    r.signal->addItem(SearchDialog::signal_label((*_signals)[i]), (int)i);
                const int sel = signal_of(t);
                if (sel >= 0)
                    r.signal->setCurrentIndex(sel);
                else {
                    r.missing = t;
                    QString name;
                    if (!t.bits.empty()){
                        QStringList bits;
                        for (int b : t.bits)
                            bits.append(QString::number(b));
                        name = QStringLiteral("[%1]").arg(bits.join(QLatin1Char(',')));
                    }
                    else
                        name = t.is_channel() ? QString::number(t.channel) : t.bus;
                    r.signal->addItem(T("IDS_DLG_SRCH_MISSING", "%1 (missing)").arg(name), -1);
                    r.signal->setCurrentIndex(r.signal->count() - 1);
                }
                r.value = new QLineEdit(t.value, frame);
                QToolButton *remove = new QToolButton(frame);
                remove->setText(QStringLiteral("✕"));
                remove->setToolTip(T("IDS_DLG_SRCH_REMOVE_SIGNAL", "Remove this signal"));

                const size_t ra = a, rk = k;
                connect(r.value, &QLineEdit::textChanged, this, [this, ra, rk](){ validate(_rows[ra][rk]); });
                connect(r.signal, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                        [this, ra, rk](int){ validate(_rows[ra][rk]); });
                connect(remove, &QToolButton::clicked, this, [this, ra, rk](){ remove_term(ra, rk); });

                g->addWidget(r.signal, (int)k, 0);
                g->addWidget(r.value, (int)k, 1);
                g->addWidget(remove, (int)k, 2);
                rows.push_back(r);
            }
            g->setColumnStretch(1, 1);

            QHBoxLayout *buttons = new QHBoxLayout();
            QPushButton *add = new QPushButton(T("IDS_DLG_SRCH_ADD_SIGNAL", "Add signal"), frame);
            connect(add, &QPushButton::clicked, this, [this, a](){ add_term(a); });
            buttons->addWidget(add);
            if (_patterns.size() > 1){
                QPushButton *remove = new QPushButton(T("IDS_DLG_SRCH_REMOVE_OR", "Remove alternative"), frame);
                connect(remove, &QPushButton::clicked, this, [this, a](){ remove_alternative(a); });
                buttons->addWidget(remove);
            }
            buttons->addStretch(1);
            g->addLayout(buttons, (int)p.size(), 0, 1, 3);

            _rows.push_back(rows);
            v->addWidget(frame);
        }

        QHBoxLayout *more = new QHBoxLayout();
        QPushButton *alt = new QPushButton(T("IDS_DLG_SRCH_ADD_OR", "Add alternative (or)"), _body);
        connect(alt, &QPushButton::clicked, this, [this](){ add_alternative(); });
        more->addWidget(alt);
        more->addStretch(1);
        v->addLayout(more);

        _layout->addWidget(_body);
        for (auto &rows : _rows)
            for (Row &r : rows)
                validate(r);
    }

    void add_term(size_t a)
    {
        sync();
        _patterns[a].push_back(default_term(_patterns[a]));
        rebuild();
        if (!_rows[a].empty())
            _rows[a].back().value->setFocus();
    }

    void remove_term(size_t a, size_t k)
    {
        sync();
        _patterns[a].erase(_patterns[a].begin() + k);
        if (_patterns[a].empty() && _patterns.size() > 1)
            _patterns.erase(_patterns.begin() + a);
        rebuild();
    }

    void add_alternative()
    {
        sync();
        _patterns.push_back(Pattern(1, default_term(Pattern())));
        rebuild();
        if (!_rows.back().empty())
            _rows.back().back().value->setFocus();
    }

    void remove_alternative(size_t a)
    {
        sync();
        _patterns.erase(_patterns.begin() + a);
        if (_patterns.empty())
            _patterns.push_back(Pattern(1, default_term(Pattern())));
        rebuild();
    }

    const std::vector<SearchSignal> *_signals;
    bool _state;
    Patterns _patterns;
    std::vector<std::vector<Row>> _rows;
    QVBoxLayout *_layout;
    QWidget *_body = nullptr;
};

//------------------------------------------------------------- condition

class SearchConditionBox : public QFrame
{
public:
    SearchConditionBox(QWidget *parent, const std::vector<SearchSignal> *sigs,
                       std::function<void(SearchConditionBox *)> on_remove) :
        QFrame(parent), _find_state(false)
    {
        setFrameShape(QFrame::StyledPanel);
        _title = new QLabel(this);
        QFont bold = _title->font();
        bold.setBold(true);
        _title->setFont(bold);
        _subject = new DsComboBox(this);
        _subject->addItem(T("IDS_DLG_SRCH_AN_EVENT", "An event"));
        _subject->addItem(T("IDS_DLG_SRCH_A_STATE", "A state"));
        _quantifier = new DsComboBox(this);
        _count = new QLineEdit(this);
        _count->setMaximumWidth(80);
        _times = new QLabel(T("IDS_DLG_SRCH_TIMES", "times"), this);
        _patterns = new SearchPatternList(this, sigs, false);

        _window = new DsComboBox(this);
        _window->addItem(T("IDS_DLG_SRCH_W_START", "relative to the match start"), (int)WindowKind::Start);
        _window->addItem(T("IDS_DLG_SRCH_W_END", "relative to the match end"), (int)WindowKind::End);
        _window->addItem(T("IDS_DLG_SRCH_W_DURING", "during the match"), (int)WindowKind::During);
        _window->addItem(T("IDS_DLG_SRCH_W_UNTIL", "until the next boundary event"), (int)WindowKind::Until);
        _window->addItem(T("IDS_DLG_SRCH_W_SINCE", "since the previous boundary event"), (int)WindowKind::Since);
        _from_label = new QLabel(T("IDS_DLG_SRCH_FROM", "from"), this);
        _from = new QLineEdit(this);
        _from->setPlaceholderText(time_hint());
        _to_label = new QLabel(T("IDS_DLG_SRCH_TO", "to"), this);
        _to = new QLineEdit(this);
        _to->setPlaceholderText(time_hint());
        _boundary_label = new QLabel(T("IDS_DLG_SRCH_BOUNDARY", "Boundary event:"), this);
        _boundary = new SearchPatternList(this, sigs, false);
        QPushButton *remove = new QPushButton(T("IDS_DLG_SRCH_REMOVE_CONDITION", "Remove condition"), this);

        QHBoxLayout *what = new QHBoxLayout();
        what->addWidget(_subject);
        what->addWidget(_quantifier);
        what->addWidget(_count);
        what->addWidget(_times);
        what->addStretch(1);

        QHBoxLayout *when = new QHBoxLayout();
        when->addWidget(_window);
        when->addWidget(_from_label);
        when->addWidget(_from);
        when->addWidget(_to_label);
        when->addWidget(_to);
        when->addStretch(1);

        QHBoxLayout *header = new QHBoxLayout();
        header->addWidget(_title);
        header->addStretch(1);
        header->addWidget(remove);

        QVBoxLayout *v = new QVBoxLayout(this);
        v->addLayout(header);
        v->addLayout(what);
        v->addWidget(_patterns);
        v->addLayout(when);
        v->addWidget(_boundary_label);
        v->addWidget(_boundary);

        fill_quantifiers(false);
        connect(_subject, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){
            const Quantifier q = quantifier();
            const bool state = _subject->currentIndex() == 1;
            fill_quantifiers(state);
            // Keep the sense: present, absent.
            if (state)
                set_quantifier(q == Quantifier::None ? Quantifier::Never : Quantifier::Sometime);
            else
                set_quantifier(q == Quantifier::Never ? Quantifier::None : Quantifier::Occurs);
            _patterns->set_state(state);
            update_visibility();
        });
        connect(_quantifier, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int){ update_visibility(); });
        connect(_window, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int){ update_visibility(); });
        connect(_count, &QLineEdit::textChanged, this, [this](){
            uint64_t n;
            set_error(_count, parse_count(_count->text(), n) == ErrorCode::None || _count->text().isEmpty()
                          ? QString() : T("IDS_DLG_SRCH_E_COUNT", "\"%1\" is not a valid count.").arg(_count->text()));
        });
        for (QLineEdit *e : {_from, _to}){
            connect(e, &QLineEdit::textChanged, this, [e](){
                TimeValue t;
                const ErrorCode ec = parse_time(e->text(), t);
                Error err;
                err.code = e->text().isEmpty() ? ErrorCode::None : ec;
                err.detail = e->text();
                set_error(e, err.code == ErrorCode::None ? QString()
                                                         : SearchDialog::error_text(err, std::vector<SearchSignal>()));
            });
        }
        connect(remove, &QPushButton::clicked, this, [this, on_remove](){ on_remove(this); });

        Condition c;
        c.from = QStringLiteral("0");
        c.to = QStringLiteral("1us");
        set_condition(c);
    }

    void set_number(int n)
    {
        _title->setText(T("IDS_DLG_SRCH_CONDITION", "Condition %1").arg(n));
    }

    void set_find_state(bool state)
    {
        _find_state = state;
        QStandardItemModel *model = qobject_cast<QStandardItemModel *>(_window->model());
        if (model)
            model->item(2)->setEnabled(state);
        if (!state && window() == WindowKind::During)
            _window->setCurrentIndex(0);
    }

    void set_condition(const Condition &c)
    {
        _subject->setCurrentIndex(c.state ? 1 : 0);
        fill_quantifiers(c.state);
        set_quantifier(c.quantifier);
        _count->setText(c.count.isEmpty() ? QStringLiteral("1") : c.count);
        _patterns->set_state(c.state);
        _patterns->set_patterns(c.subject);
        for (int i = 0; i < _window->count(); i++){
            if (_window->itemData(i).toInt() == (int)c.window)
                _window->setCurrentIndex(i);
        }
        _from->setText(c.from);
        _to->setText(c.to);
        _boundary->set_patterns(c.boundary);
        update_visibility();
    }

    Condition condition() const
    {
        Condition c;
        c.state = _subject->currentIndex() == 1;
        c.subject = _patterns->patterns();
        c.quantifier = quantifier();
        c.count = _count->text();
        c.window = window();
        c.from = _from->text();
        c.to = _to->text();
        if (c.window == WindowKind::Until || c.window == WindowKind::Since)
            c.boundary = _boundary->patterns();
        return c;
    }

private:
    void fill_quantifiers(bool state)
    {
        _quantifier->blockSignals(true);
        _quantifier->clear();
        if (state){
            _quantifier->addItem(T("IDS_DLG_SRCH_Q_SOMETIME", "holds at some point"), (int)Quantifier::Sometime);
            _quantifier->addItem(T("IDS_DLG_SRCH_Q_NEVER", "never holds"), (int)Quantifier::Never);
            _quantifier->addItem(T("IDS_DLG_SRCH_Q_THROUGHOUT", "holds throughout"), (int)Quantifier::Throughout);
        }
        else {
            _quantifier->addItem(T("IDS_DLG_SRCH_Q_OCCURS", "occurs"), (int)Quantifier::Occurs);
            _quantifier->addItem(T("IDS_DLG_SRCH_Q_NONE", "does not occur"), (int)Quantifier::None);
            _quantifier->addItem(T("IDS_DLG_SRCH_Q_ATLEAST", "occurs at least"), (int)Quantifier::AtLeast);
            _quantifier->addItem(T("IDS_DLG_SRCH_Q_ATMOST", "occurs at most"), (int)Quantifier::AtMost);
            _quantifier->addItem(T("IDS_DLG_SRCH_Q_EXACTLY", "occurs exactly"), (int)Quantifier::Exactly);
        }
        _quantifier->blockSignals(false);
    }

    void set_quantifier(Quantifier q)
    {
        for (int i = 0; i < _quantifier->count(); i++){
            if (_quantifier->itemData(i).toInt() == (int)q)
                _quantifier->setCurrentIndex(i);
        }
    }

    Quantifier quantifier() const
    {
        return (Quantifier)_quantifier->currentData().toInt();
    }

    WindowKind window() const
    {
        return (WindowKind)_window->currentData().toInt();
    }

    void update_visibility()
    {
        const bool counted = has_count(quantifier());
        _count->setVisible(counted);
        _times->setVisible(counted);
        const WindowKind w = window();
        const bool times = w == WindowKind::Start || w == WindowKind::End;
        for (QWidget *x : {(QWidget *)_from_label, (QWidget *)_from, (QWidget *)_to_label, (QWidget *)_to})
            x->setVisible(times);
        const bool boundary = w == WindowKind::Until || w == WindowKind::Since;
        _boundary_label->setVisible(boundary);
        _boundary->setVisible(boundary);
    }

    bool _find_state;
    QLabel *_title;
    DsComboBox *_subject;
    DsComboBox *_quantifier;
    QLineEdit *_count;
    QLabel *_times;
    SearchPatternList *_patterns;
    DsComboBox *_window;
    QLabel *_from_label;
    QLineEdit *_from;
    QLabel *_to_label;
    QLineEdit *_to;
    QLabel *_boundary_label;
    SearchPatternList *_boundary;
};

//------------------------------------------------------------- dialog

SearchDialog::SearchDialog(QWidget *parent, const std::vector<SearchSignal> &sigs,
                           const BusResolver &buses, const Query &query) :
    DSDialog(parent, true, false),
    _signals(sigs),
    _buses(buses)
{
    setTitle(text("IDS_DLG_SRCH_TITLE", "Search"));
    // Tall enough for a condition or two, but within the screen.
    QSize size(780, 800);
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen)
        size = size.boundedTo(screen->availableGeometry().size() * 0.9);
    setMinimumSize(QSize(640, 480).boundedTo(size));
    resize(size);

    QWidget *content = new QWidget();
    QVBoxLayout *v = new QVBoxLayout(content);

    // What to find
    QGroupBox *find = new QGroupBox(text("IDS_DLG_SRCH_FIND", "Find"), content);
    QVBoxLayout *fv = new QVBoxLayout(find);
    QHBoxLayout *kind = new QHBoxLayout();
    _kind = new DsComboBox(find);
    _kind->addItem(text("IDS_DLG_SRCH_AN_EVENT", "An event"));
    _kind->addItem(text("IDS_DLG_SRCH_A_STATE", "A state"));
    kind->addWidget(_kind);
    kind->addStretch(1);
    fv->addLayout(kind);
    _find = new SearchPatternList(find, &_signals, query.state);
    fv->addWidget(_find);

    _duration_row = new QWidget(find);
    QHBoxLayout *dur = new QHBoxLayout(_duration_row);
    dur->setContentsMargins(0, 0, 0, 0);
    _min_duration = new QLineEdit(query.min_duration, _duration_row);
    _max_duration = new QLineEdit(query.max_duration, _duration_row);
    for (QLineEdit *e : {_min_duration, _max_duration}){
        e->setPlaceholderText(text("IDS_DLG_SRCH_HINT_ANY", "any"));
        e->setMaximumWidth(120);
        connect(e, &QLineEdit::textChanged, this, [this, e](){
            TimeValue t;
            Error err;
            err.code = e->text().isEmpty() ? ErrorCode::None : parse_time(e->text(), t);
            err.detail = e->text();
            set_error(e, err.code == ErrorCode::None ? QString() : error_text(err, _signals));
        });
    }
    dur->addWidget(new QLabel(text("IDS_DLG_SRCH_DURATION_MIN", "Lasting at least"), _duration_row));
    dur->addWidget(_min_duration);
    dur->addWidget(new QLabel(text("IDS_DLG_SRCH_DURATION_MAX", "and at most"), _duration_row));
    dur->addWidget(_max_duration);
    dur->addStretch(1);
    fv->addWidget(_duration_row);
    v->addWidget(find);

    // Conditions
    QGroupBox *conds = new QGroupBox(text("IDS_DLG_SRCH_CONDITIONS", "Conditions (all must hold)"), content);
    _conditions_layout = new QVBoxLayout(conds);
    // Room for the group titles, which framed children would cover.
    for (QVBoxLayout *l : {fv, _conditions_layout}){
        QMargins m = l->contentsMargins();
        m.setTop(m.top() + 12);
        l->setContentsMargins(m);
    }
    _add_condition = new QPushButton(text("IDS_DLG_SRCH_ADD_CONDITION", "Add condition"), conds);
    QHBoxLayout *add = new QHBoxLayout();
    add->addWidget(_add_condition);
    add->addStretch(1);
    _conditions_layout->addLayout(add);
    v->addWidget(conds);

    v->addWidget(help_box(content));
    v->addStretch(1);

    QScrollArea *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    layout()->addWidget(scroll, 1);

    _error = new QLabel();
    _error->setWordWrap(true);
    _error->setStyleSheet(QStringLiteral("QLabel { color: #e04848; }"));
    _error->hide();
    layout()->addWidget(_error);

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Ok)->setText(text("IDS_DLG_OK", "OK"));
    box->button(QDialogButtonBox::Cancel)->setText(text("IDS_DLG_CANCEL", "Cancel"));
    layout()->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, [this](){ on_ok(); });
    connect(box, &QDialogButtonBox::rejected, this, [this](){ reject(); });

    connect(_add_condition, &QPushButton::clicked, this, [this](){
        Condition c;
        c.from = QStringLiteral("0");
        c.to = QStringLiteral("1us");
        add_condition(c);
    });

    _kind->setCurrentIndex(query.state ? 1 : 0);
    _find->set_patterns(query.find);
    for (const Condition &c : query.conditions)
        add_condition(c);
    connect(_kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){ on_kind_changed(); });
    on_kind_changed();
}

void SearchDialog::keyPressEvent(QKeyEvent *event)
{
    // Enter picks list entries and finishes values; it doesn't close the
    // dialog (the button box makes OK the default when shown).
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter){
        event->accept();
        return;
    }
    DSDialog::keyPressEvent(event);
}

QString SearchDialog::text(const char *id, const char *fallback)
{
    return QString::fromUtf8(L_S(STR_PAGE_DLG, id, fallback));
}

QString SearchDialog::signal_label(const SearchSignal &s)
{
    if (s.bus.isEmpty())
        return QStringLiteral("%1 (%2)").arg(s.name).arg(s.channel);
    return text("IDS_DLG_SRCH_BUS_LABEL", "%1 (bus, %2 bits)").arg(s.name).arg(s.width);
}

Query SearchDialog::query() const
{
    Query q;
    q.state = _kind->currentIndex() == 1;
    q.find = _find->patterns();
    if (q.state){
        q.min_duration = _min_duration->text().trimmed();
        q.max_duration = _max_duration->text().trimmed();
    }
    for (SearchConditionBox *c : _conditions)
        q.conditions.push_back(c->condition());
    return q;
}

void SearchDialog::on_kind_changed()
{
    const bool state = _kind->currentIndex() == 1;
    _find->set_state(state);
    _duration_row->setVisible(state);
    for (SearchConditionBox *c : _conditions)
        c->set_find_state(state);
}

void SearchDialog::add_condition(const Condition &condition)
{
    SearchConditionBox *box = new SearchConditionBox(_add_condition->parentWidget(), &_signals,
        [this](SearchConditionBox *b){ remove_condition(b); });
    box->set_condition(condition);
    box->set_find_state(_kind->currentIndex() == 1);
    _conditions_layout->insertWidget((int)_conditions.size(), box);
    _conditions.push_back(box);
    renumber();
}

void SearchDialog::remove_condition(SearchConditionBox *box)
{
    _conditions.erase(std::remove(_conditions.begin(), _conditions.end(), box), _conditions.end());
    box->hide();
    box->deleteLater();
    renumber();
}

void SearchDialog::renumber()
{
    for (size_t i = 0; i < _conditions.size(); i++)
        _conditions[i]->set_number((int)i + 1);
    _add_condition->setEnabled(_conditions.size() < (size_t)MaxConditions);
}

void SearchDialog::on_ok()
{
    std::vector<int> channels;
    for (const SearchSignal &s : _signals){
        if (s.bus.isEmpty())
            channels.push_back(s.channel);
    }
    ChannelCheck has = [channels](int ch){
        return std::find(channels.begin(), channels.end(), ch) != channels.end();
    };

    Program program;
    Error error;
    _error->hide();
    if (!compile(query(), _buses, has, 0, program, error)){
        _error->setText(error_text(error, _signals));
        _error->show();
        return;
    }
    accept();
}

QString SearchDialog::error_text(const Error &e, const std::vector<SearchSignal> &sigs)
{
    QString d = e.detail;
    if (e.code == ErrorCode::UnknownChannel || e.code == ErrorCode::ConflictingTerms
        || e.code == ErrorCode::EmptyValue){
        bool ok;
        const int ch = d.toInt(&ok);
        for (const SearchSignal &s : sigs){
            if (ok && s.bus.isEmpty() && s.channel == ch)
                d = signal_label(s);
        }
    }

    QString m;
    switch (e.code){
    case ErrorCode::None:
        return QString();
    case ErrorCode::NoPattern:
        m = e.part == ErrorPart::Boundary ? text("IDS_DLG_SRCH_E_NOBOUNDARY", "No boundary event is given.")
                                          : text("IDS_DLG_SRCH_E_NOPATTERN", "Nothing to look for is given.");
        break;
    case ErrorCode::EmptyPattern:       m = text("IDS_DLG_SRCH_E_EMPTYPATTERN", "A pattern has no signals."); break;
    case ErrorCode::NoCondition:        m = text("IDS_DLG_SRCH_E_NOCONDITION", "A pattern has only don't-care values."); break;
    case ErrorCode::EmptyValue:         m = text("IDS_DLG_SRCH_E_EMPTYVALUE", "The value of %1 is missing.").arg(d); break;
    case ErrorCode::BadChannelValue:    m = text("IDS_DLG_SRCH_E_CHANNELVALUE", "\"%1\" is not a channel value. Use 0, 1, X, R, F or C.").arg(d); break;
    case ErrorCode::BadValue:           m = text("IDS_DLG_SRCH_E_BADVALUE", "\"%1\" is not a valid bus value.").arg(d); break;
    case ErrorCode::ValueTooWide:       m = text("IDS_DLG_SRCH_E_TOOWIDE", "\"%1\" does not fit the bus.").arg(d); break;
    case ErrorCode::EdgeNotAllowed:     m = text("IDS_DLG_SRCH_E_EDGECOMPARE", "\"%1\": edges can't be compared.").arg(d); break;
    case ErrorCode::DontCareNotAllowed: m = text("IDS_DLG_SRCH_E_XCOMPARE", "\"%1\": X can't be used in a comparison.").arg(d); break;
    case ErrorCode::BadRange:           m = text("IDS_DLG_SRCH_E_RANGE", "\"%1\": the range is reversed.").arg(d); break;
    case ErrorCode::EdgeInState:        m = text("IDS_DLG_SRCH_E_EDGESTATE", "A state can't contain edges (R, F, C or changes)."); break;
    case ErrorCode::UnknownChannel:     m = text("IDS_DLG_SRCH_E_CHANNEL", "Channel %1 has no data.").arg(d); break;
    case ErrorCode::UnknownBus:         m = text("IDS_DLG_SRCH_E_BUS", "There is no parallel decoder named \"%1\".").arg(d); break;
    case ErrorCode::BusTooWide:         m = text("IDS_DLG_SRCH_E_BUSWIDE", "Bus %1 has more than 64 bits.").arg(d); break;
    case ErrorCode::UnmappedBit:        m = text("IDS_DLG_SRCH_E_UNMAPPED", "\"%1\" needs a bus bit that has no channel.").arg(d); break;
    case ErrorCode::ConflictingTerms:   m = text("IDS_DLG_SRCH_E_CONFLICT", "Channel %1 has conflicting values.").arg(d); break;
    case ErrorCode::BadTime:            m = text("IDS_DLG_SRCH_E_TIME", "\"%1\" is not a valid time.").arg(d); break;
    case ErrorCode::MissingUnit:        m = text("IDS_DLG_SRCH_E_UNIT", "\"%1\" needs a unit: s, ms, us, ns or sa (samples).").arg(d); break;
    case ErrorCode::EmptyWindow:        m = text("IDS_DLG_SRCH_E_WINDOW", "The time window is empty."); break;
    case ErrorCode::BadCount:           m = text("IDS_DLG_SRCH_E_COUNT", "\"%1\" is not a valid count.").arg(d); break;
    case ErrorCode::BadDuration:        m = text("IDS_DLG_SRCH_E_DURATION", "The minimum duration is longer than the maximum."); break;
    case ErrorCode::DuringNeedsState:   m = text("IDS_DLG_SRCH_E_DURING", "\"During the match\" needs a state to find."); break;
    case ErrorCode::TooManyConditions:  m = text("IDS_DLG_SRCH_E_MANY", "There are more than 8 conditions."); break;
    case ErrorCode::NoSamplerate:       m = text("IDS_DLG_SRCH_E_RATE", "Times need a sample rate."); break;
    case ErrorCode::BadQuery:           m = text("IDS_DLG_SRCH_E_QUERY", "The search is malformed."); break;
    }

    if (e.condition < 0)
        return m;
    return text("IDS_DLG_SRCH_E_IN_CONDITION", "Condition %1: %2").arg(e.condition + 1).arg(m);
}

QString SearchDialog::summary(const Query &q, const std::vector<SearchSignal> &sigs)
{
    auto name_of = [&](const Term &t){
        for (const SearchSignal &s : sigs){
            if (t.bits.empty() && ((t.is_channel() && s.bus.isEmpty() && s.channel == t.channel)
                                   || (!t.is_channel() && s.bus == t.bus)))
                return s.name;
        }
        if (!t.bits.empty())
            return QStringLiteral("[bus]");
        return t.is_channel() ? QString::number(t.channel) : t.bus;
    };

    QStringList alts;
    for (const Pattern &p : q.find){
        QStringList terms;
        for (const Term &t : p){
            // "A=0x21XX", but "A != 0", "A <= 0xFF" and "D changes".
            const QString v = t.value.trimmed();
            const bool spaced = v.startsWith(QLatin1Char('!')) || v.startsWith(QLatin1Char('<'))
                || v.startsWith(QLatin1Char('>')) || v.startsWith(QLatin1Char('='))
                || v.toLower().startsWith(QLatin1String("change"));
            terms.append(QString::fromLatin1(spaced ? "%1 %2" : "%1=%2").arg(name_of(t), v));
        }
        alts.append(terms.join(QLatin1Char(' ')));
    }
    QString s = alts.join(QStringLiteral(" %1 ").arg(text("IDS_DLG_SRCH_OR", "or")));
    if (q.state){
        s = text("IDS_DLG_SRCH_SUMMARY_STATE", "state %1").arg(s);
        if (!q.min_duration.isEmpty() || !q.max_duration.isEmpty())
            s += QStringLiteral(" [%1..%2]").arg(q.min_duration, q.max_duration);
    }
    if (!q.conditions.empty())
        s += QStringLiteral(" · ") + text("IDS_DLG_SRCH_SUMMARY_CONDITIONS", "%1 condition(s)").arg(q.conditions.size());
    return s;
}

} // namespace dialogs
} // namespace pv
