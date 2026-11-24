#include "app/mainwindow.h"

#include "app/appicon.h"
#include "app/metricbutton.h"
#include "app/processmodel.h"
#include "app/timechart.h"
#include "core/format.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <map>
#include <new>
#include <set>

#include <unistd.h>

namespace {

const QString kDash = QStringLiteral("\u2014");

QString q(const std::string& s)
{
    return QString::fromStdString(s);
}

QString capitalized(QString s)
{
    if (!s.isEmpty())
        s[0] = s[0].toUpper();
    return s;
}

// "sum of 1 physical interface" / "sum of 3 physical interfaces", from the
// collector's aggregate label ("all physical interfaces").
QString aggregateKind(const ps::AggregateRate& agg)
{
    QString kind = q(agg.label);
    if (kind.startsWith(QStringLiteral("all ")))
        kind.remove(0, 4);
    if (agg.members == 1 && kind.endsWith(QLatin1Char('s')))
        kind.chop(1);
    return QStringLiteral("sum of %1 %2").arg(agg.members).arg(kind);
}

QString netAggregateTitle(const ps::Snapshot* s)
{
    const QString what = s && !s->netTotal.label.empty() ? q(s->netTotal.label) : QStringLiteral("sum without loopback");
    return QStringLiteral("Network throughput: ") + what;
}

QString qRate(double v)
{
    return ps::isValid(v) ? q(ps::fmt::rate(v)) : kDash;
}

QString qBytes(double v)
{
    return ps::isValid(v) ? q(ps::fmt::bytes(v)) : kDash;
}

QString qPercent(double v, int decimals = 1)
{
    return ps::isValid(v) ? q(ps::fmt::percent(v, decimals)) : kDash;
}

QString qCelsius(double v)
{
    return ps::isValid(v) ? q(ps::fmt::celsius(v)) : kDash;
}

QString intervalText(int ms)
{
    return ms == 1000 ? QStringLiteral("1 second") : QStringLiteral("%1 seconds").arg(ms / 1000.0, 0, 'g', 3);
}

QFont scaledFont(const QFont& base, double factor, QFont::Weight weight)
{
    QFont f = base;
    if (f.pointSizeF() > 0)
        f.setPointSizeF(f.pointSizeF() * factor);
    else if (f.pixelSize() > 0)
        f.setPixelSize(std::max(1, static_cast<int>(std::lround(f.pixelSize() * factor))));
    f.setWeight(weight);
    return f;
}

// Sets a font relative to the window font and remembers how, so a later
// system font change can size it again (MainWindow::applyScaledFonts). With
// a template, the minimum width fits that text, so a changing value does not
// make the layout jitter.
void setRelativeFont(QWidget* w, const QFont& base, double factor, QFont::Weight weight, const QString& widest = {})
{
    w->setProperty("psFontScale", factor);
    w->setProperty("psFontWeight", static_cast<int>(weight));
    const QFont f = scaledFont(base, factor, weight);
    w->setFont(f);
    if (!widest.isEmpty()) {
        w->setProperty("psTemplate", widest);
        w->setMinimumWidth(QFontMetrics(f).horizontalAdvance(widest) + 2);
    }
}

QLabel* makeHeading(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    setRelativeFont(l, parent->font(), 1.3, QFont::DemiBold);
    l->setAccessibleName(text);
    return l;
}

QLabel* makeSubheading(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    setRelativeFont(l, parent->font(), 1.0, QFont::DemiBold);
    return l;
}

ChartSeries chartSeries(std::string key, QString label, int visual, Qt::PenStyle style, bool fill)
{
    ChartSeries s {std::move(key), std::move(label), visual};
    s.style = style;
    s.fill = fill;
    return s;
}

// Colour cycle for plotted sensors: warm hues first.
int temperatureVisual(int position)
{
    static constexpr int order[] = {6, 1, 4, 3, 5, 2, 0, 7};
    return order[position % 8];
}

QLabel* makeNote(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setForegroundRole(QPalette::PlaceholderText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setMaximumWidth(900);
    return l;
}

QString kindText(ps::DeviceKind kind)
{
    switch (kind) {
    case ps::DeviceKind::Physical: return QStringLiteral("physical");
    case ps::DeviceKind::Virtual: return QStringLiteral("virtual");
    case ps::DeviceKind::Loopback: return QStringLiteral("loopback");
    }
    return {};
}

// Small multiples of per-core charts that reflow to the available width.
} // namespace

class CoreGrid : public QWidget {
public:
    explicit CoreGrid(QWidget* parent)
        : QWidget(parent)
    {
        setAccessibleName(QStringLiteral("Per-core utilization"));
    }

    void setCores(const std::vector<std::pair<int, bool>>& cores, const ps::History* history, double window)
    {
        std::vector<int> indices;
        for (const auto& c : cores)
            indices.push_back(c.first);
        if (indices != m_indices) {
            qDeleteAll(m_charts);
            m_charts.clear();
            m_indices = indices;
            for (int index : indices) {
                const QString name = QStringLiteral("CPU %1").arg(index);
                auto* chart = new TimeChart(name, ps::Unit::Percent, this);
                chart->setMode(TimeChart::Mode::Compact);
                chart->setFixedRange(0, 100);
                chart->setHistory(history);
                chart->setWindowSeconds(window);
                chart->setSeries({ChartSeries {ps::keys::core(index), name, 0}});
                chart->show();
                m_charts.push_back(chart);
            }
            m_columns = 0;
            relayout();
        }
        for (std::size_t i = 0; i < cores.size() && i < m_charts.size(); ++i) {
            QString text = cores[i].second ? QStringLiteral("Measuring\u2026") : QStringLiteral("Offline");
            QString title = QStringLiteral("CPU %1").arg(cores[i].first);
            if (history && !history->find(ps::keys::core(cores[i].first))) {
                // Over the per-category history quota: say so on the chart
                // itself (title and accessible name), not only as empty text.
                text = QStringLiteral("Not recorded: history limit");
                title += QStringLiteral(" (not recorded)");
            }
            m_charts[i]->setEmptyText(text);
            m_charts[i]->setTitle(title);
        }
    }
    void refresh()
    {
        for (auto* c : m_charts)
            c->refresh();
    }
    void setWindowSeconds(double seconds)
    {
        for (auto* c : m_charts)
            c->setWindowSeconds(seconds);
    }
    void setNotice(const QString& notice)
    {
        for (auto* c : m_charts)
            c->setNotice(notice);
    }
    const std::vector<TimeChart*>& charts() const { return m_charts; }
    QSize sizeHint() const override { return {600, 2 * kCellH + kGap}; }

    static constexpr int kCellW = 176;
    static constexpr int kCellH = 78;
    static constexpr int kGap = 6;

protected:
    void resizeEvent(QResizeEvent*) override
    {
        m_columns = 0;
        relayout();
    }

private:
    void relayout()
    {
        const int n = static_cast<int>(m_charts.size());
        const int availW = std::max(1, width());
        const int cols = std::clamp((availW + kGap) / (kCellW + kGap), 1, std::max(1, std::min(n, 8)));
        if (cols == m_columns)
            return;
        m_columns = cols;
        const int rows = (n + cols - 1) / cols;
        setMinimumHeight(rows > 0 ? rows * kCellH + (rows - 1) * kGap : kCellH);
        const int totalGap = (cols - 1) * kGap;
        const int baseW = std::max(1, (availW - totalGap) / cols);
        const int extra = std::max(0, (availW - totalGap) - baseW * cols);
        for (int i = 0; i < n; ++i) {
            const int col = i % cols;
            const int row = i / cols;
            const int x = col * (baseW + kGap) + std::min(col, extra);
            const int w = baseW + (col < extra ? 1 : 0);
            m_charts[static_cast<std::size_t>(i)]->setGeometry(x, row * (kCellH + kGap), w, kCellH);
        }
    }

    std::vector<int> m_indices;
    std::vector<TimeChart*> m_charts;
    int m_columns = 0;
};

namespace {

// A metric page that scrolls vertically instead of squeezing its charts and
// facts below their minimum (short windows, large fonts, Details). It keeps
// the page's minimum width, so nothing is cut off sideways.
class PageScroll : public QScrollArea {
public:
    explicit PageScroll(QWidget* page)
    {
        setObjectName(page->accessibleName() + QStringLiteral(" page"));
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // Not a tab stop of its own; it still scrolls focused children into view.
        setFocusPolicy(Qt::NoFocus);
        setWidget(page);
    }

    QSize minimumSizeHint() const override
    {
        const int page = widget() ? widget()->minimumSizeHint().width() : 0;
        return {page + verticalScrollBar()->sizeHint().width(), QScrollArea::minimumSizeHint().height()};
    }
};

// Wide enough for a distinguishable sensor name and its reading.
int sensorListMinimumWidth(const QWidget* list)
{
    const QFontMetrics fm(list->font());
    const QStyle* style = list->style();
    return fm.horizontalAdvance(QStringLiteral("thinkpad \u00b7 temp5")) +
           fm.horizontalAdvance(QStringLiteral("100.0 \u00b0C")) + 2 * fm.averageCharWidth() +
           style->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, list) +
           style->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, list) + 16;
}

// Draws "used" as a proportional bar with the percentage beside it, so the
// fill level is readable without relying on the bar colour.
// Filesystem use from which the usage bar turns to the warning colour and
// the row's tooltip says "Nearly full".
constexpr double kNearlyFull = 0.9;

class UsageDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString text = opt.text;
        opt.text.clear();
        const QWidget* w = opt.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);

        const double fraction = index.data(Qt::UserRole).toDouble();
        const QFontMetrics fm(opt.font);
        const int textW = fm.horizontalAdvance(QStringLiteral("100 %")) + 8;
        const QRect r = opt.rect.adjusted(4, 0, -4, 0);
        const QRect textRect(r.right() - textW, r.top(), textW, r.height());
        const bool selected = (opt.state & QStyle::State_Selected) != 0;
        p->save();
        p->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        p->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, text);
        if (std::isfinite(fraction)) {
            const int barH = std::max(6, fm.height() / 3 + 2);
            const double radius = barH / 2.0;
            const QRectF track(r.left(), r.center().y() - barH / 2.0 + 0.5, std::max(0, r.width() - textW - 8), barH);
            QColor trackColor = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
            trackColor.setAlphaF(selected ? 0.24f : 0.14f);
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setPen(Qt::NoPen);
            p->setBrush(trackColor);
            p->drawRoundedRect(track, radius, radius);
            // The red of the series palette, in its light or dark variant.
            const bool nearlyFull = fraction >= kNearlyFull;
            QColor fill = nearlyFull
                ? seriesColor(6, opt.palette)
                : opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Highlight);
            p->setBrush(fill);
            QRectF bar = track;
            bar.setWidth(track.width() * std::clamp(fraction, 0.0, 1.0));
            if (bar.width() > 0.5)
                p->drawRoundedRect(bar, radius, radius);
        }
        p->restore();
    }
};

// Process cells other than the name keep a gutter of at least half an average
// character on the side their text is aligned to, so that a right-aligned
// number and the left-aligned text after it (PID and User, Threads and State,
// Age and Command) do not run together. The gutter is an empty decoration
// beside the text: the style lays it out and draws the whole cell itself, so
// text colours, selection, hover, focus and alternating rows are the theme's.
class ProcessCellDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static int gutter(const QFontMetrics& fm) { return std::max(2, (fm.averageCharWidth() + 1) / 2); }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        // The gutter widens the cell; the row keeps the height of its text
        // (styles add room around a decoration's height).
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        QStyleOptionViewItem plain = option;
        QStyledItemDelegate::initStyleOption(&plain, index);
        const QWidget* w = plain.widget;
        const QStyle* style = w ? w->style() : QApplication::style();
        size.setHeight(style->sizeFromContents(QStyle::CT_ItemViewItem, &plain, QSize(), w).height());
        return size;
    }

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (!index.parent().isValid() || index.column() == ProcessModel::Name || option->text.isEmpty()
            || (option->features & QStyleOptionViewItem::HasDecoration))
            return;
        const bool number = option->displayAlignment & Qt::AlignRight;
        QStyleOptionViewItem padded = *option;
        padded.features |= QStyleOptionViewItem::HasDecoration;
        padded.decorationPosition = number ? QStyleOptionViewItem::Right : QStyleOptionViewItem::Left;
        padded.decorationSize = QSize(1, 1);
        padded.icon = QIcon();
        int width = gutter(option->fontMetrics);
        if (option->rect.isValid()) {
            // What the style's own layout gives: the text's inset and the
            // text width a one pixel decoration costs, which grow with it.
            const QWidget* w = option->widget;
            const QStyle* style = w ? w->style() : QApplication::style();
            const QRect plain = style->subElementRect(QStyle::SE_ItemViewItemText, option, w);
            const QRect one = style->subElementRect(QStyle::SE_ItemViewItemText, &padded, w);
            const bool drawnRight = QStyle::visualAlignment(option->direction, option->displayAlignment) & Qt::AlignRight;
            const int inset = drawnRight ? plain.right() - one.right() : one.left() - plain.left();
            const int cost = plain.width() - one.width();
            const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, option, w) + 1;
            const int room = plain.width() - 2 * margin;
            const int text = qCeil(QFontMetricsF(option->font).horizontalAdvance(option->text));
            // Text that fits stays whole and gives up the gutter first; text
            // that is elided anyway keeps it while more than an ellipsis
            // remains. A number is never cut for it.
            int spare = room - text;
            if (spare < 0) {
                if (number)
                    return;
                spare = room - option->fontMetrics.horizontalAdvance(QChar(0x2026));
            }
            width = std::min(1 + std::max(0, width - inset), 1 + spare - cost);
            if (width < 1)
                return;
        }
        padded.decorationSize = QSize(width, 1);
        *option = padded;
    }
};

// "Name: value" pairs in up to `columns` pairs per row. Narrow windows (a
// tiled half screen, a large system font) get fewer pairs per row instead of
// clipped numbers: the minimum width is a single pair.
} // namespace

class FactGrid : public QWidget {
public:
    FactGrid(int columns, QWidget* parent)
        : QWidget(parent)
        , m_maxColumns(columns)
        , m_grid(new Layout(this))
    {
        m_grid->setContentsMargins(0, 2, 0, 4);
        m_grid->setHorizontalSpacing(kIntraGap);
        m_grid->setVerticalSpacing(4);
        // The widget's minimum is one pair, not the current arrangement.
        m_grid->setSizeConstraint(QLayout::SetNoConstraint);
        m_grid->setColumnStretch(3 * m_maxColumns, 1);
        QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
        sp.setHeightForWidth(true);
        setSizePolicy(sp);
    }

    void addPair(QLabel* name, QLabel* value)
    {
        m_pairs.push_back({name, value, 0});
        m_columns = 0;
        place(m_maxColumns);
    }

    void resetWidths()
    {
        for (auto& p : m_pairs)
            p.widest = 0;
        m_columns = 0;
        place(columnsFor(width()));
    }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return heightFor(columnsFor(w)); }
    QSize sizeHint() const override { return {widthFor(m_maxColumns), heightFor(m_maxColumns)}; }
    QSize minimumSizeHint() const override { return {widthFor(1), heightFor(columnsFor(width()))}; }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        place(columnsFor(event->size().width()));
    }

    bool event(QEvent* event) override
    {
        // A value got wider: rearrange before the layout is applied.
        if (event->type() == QEvent::LayoutRequest)
            place(columnsFor(width()));
        return QWidget::event(event);
    }

private:
    // A parent layout asks a widget's own layout, not the widget, for its
    // height at a width; a plain grid of labels answers "none" and the rows
    // were squeezed to zero height. This one answers with the reflowed height.
    class Layout : public QGridLayout {
    public:
        explicit Layout(FactGrid* owner)
            : QGridLayout(owner)
            , m_owner(owner)
        {
        }
        bool hasHeightForWidth() const override { return true; }
        int heightForWidth(int w) const override { return m_owner->heightFor(m_owner->columnsFor(w)); }
        int minimumHeightForWidth(int w) const override { return heightForWidth(w); }

    private:
        FactGrid* m_owner;
    };

    struct Pair {
        QLabel* name;
        QLabel* value;
        mutable int widest; // widest value seen, so the layout does not flicker
    };

    int valueWidth(const Pair& p) const
    {
        p.widest = std::max(p.widest, p.value->sizeHint().width());
        return p.widest;
    }

    int widthFor(int columns) const
    {
        columns = std::max(1, columns);
        std::vector<int> nameW(columns, 0), valueW(columns, 0);
        for (std::size_t i = 0; i < m_pairs.size(); ++i) {
            const auto c = i % static_cast<std::size_t>(columns);
            nameW[c] = std::max(nameW[c], m_pairs[i].name->sizeHint().width());
            valueW[c] = std::max(valueW[c], valueWidth(m_pairs[i]));
        }
        int w = 0;
        for (int c = 0; c < columns; ++c)
            w += nameW[c] + valueW[c];
        // Intra-pair spacing (kIntraGap) plus inter-pair gutter column (kPairGap + 2 * kIntraGap).
        return w + columns * kIntraGap + (columns - 1) * (kPairGap + 2 * kIntraGap);
    }

    int heightFor(int columns) const
    {
        if (m_pairs.empty())
            return 0;
        int rowH = 0;
        for (const auto& p : m_pairs)
            rowH = std::max({rowH, p.name->sizeHint().height(), p.value->sizeHint().height()});
        const int n = static_cast<int>(m_pairs.size());
        const int rows = (n + columns - 1) / columns;
        const QMargins m = m_grid->contentsMargins();
        return m.top() + m.bottom() + rows * rowH + (rows - 1) * std::max(0, m_grid->verticalSpacing());
    }

    int columnsFor(int width) const
    {
        const int most = std::min(m_maxColumns, std::max(1, static_cast<int>(m_pairs.size())));
        for (int c = most; c > 1; --c)
            if (widthFor(c) <= width)
                return c;
        return 1;
    }

    void place(int columns)
    {
        if (columns == m_columns)
            return;
        m_columns = columns;
        for (const auto& p : m_pairs) {
            m_grid->removeWidget(p.name);
            m_grid->removeWidget(p.value);
        }
        for (int c = 0; c < m_maxColumns; ++c)
            m_grid->setColumnMinimumWidth(c * 3 + 2, c + 1 < columns ? kPairGap : 0);
        for (std::size_t i = 0; i < m_pairs.size(); ++i) {
            const int row = static_cast<int>(i) / columns;
            const int col = static_cast<int>(i) % columns;
            m_grid->addWidget(m_pairs[i].name, row, col * 3, Qt::AlignLeft | Qt::AlignVCenter);
            m_grid->addWidget(m_pairs[i].value, row, col * 3 + 1, Qt::AlignLeft | Qt::AlignVCenter);
        }
        updateGeometry();
    }

    static constexpr int kIntraGap = 6;
    static constexpr int kPairGap = 14;
    int m_maxColumns;
    int m_columns = 0;
    QGridLayout* m_grid;
    std::vector<Pair> m_pairs;
};

namespace {

void addFactPair(FactGrid* grid, const QString& name, QLabel* value, const QString& tip)
{
    auto* label = new QLabel(name + QLatin1Char(':'), grid);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setBuddy(value);
    value->setParent(grid);
    setRelativeFont(value, grid->font(), 1.0, QFont::DemiBold);
    grid->addPair(label, value);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value->setAccessibleName(name);
    if (!tip.isEmpty()) {
        label->setToolTip(tip);
        value->setToolTip(tip);
    }
}

// A short stroke in a series' colour and dash pattern over its area tint:
// the key between a headline value and its trace. Colours are resolved at
// paint time, so palette changes apply.
class Swatch : public QWidget {
public:
    Swatch(int visual, Qt::PenStyle style, QWidget* parent)
        : QWidget(parent)
        , m_visual(visual)
        , m_style(style)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    QSize sizeHint() const override
    {
        const int h = fontMetrics().height();
        return {std::max(14, static_cast<int>(std::lround(h * 1.35))), h};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor c = seriesColor(m_visual, palette());
        const QRectF r = QRectF(rect()).adjusted(1, 2, -1, -2);
        const double mid = r.center().y();
        QColor tint = c;
        tint.setAlphaF(isDarkPalette(palette()) ? 0.30f : 0.24f);
        p.fillRect(QRectF(r.left(), mid, r.width(), std::min(4.0, r.bottom() - mid)), tint);
        QPen pen(c, 2.0);
        pen.setStyle(m_style);
        pen.setCapStyle(Qt::FlatCap);
        p.setPen(pen);
        p.drawLine(QPointF(r.left(), mid), QPointF(r.right(), mid));
    }

private:
    int m_visual;
    Qt::PenStyle m_style;
};

} // namespace

// The main reading of a page at a glance: one or two large values, each
// under its name and the key of its trace, and a one-line summary. Values
// sit side by side when they fit and stack in a narrow window.
class HeadRow : public QWidget {
public:
    explicit HeadRow(QWidget* parent)
        : QWidget(parent)
        , m_items(new QBoxLayout(QBoxLayout::LeftToRight))
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 4);
        v->setSpacing(4);
        // The minimum is one value wide, not the current arrangement.
        v->setSizeConstraint(QLayout::SetNoConstraint);
        m_items->setContentsMargins(0, 0, 0, 0);
        m_items->setSpacing(kGap);
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->addLayout(m_items);
        row->addStretch(1);
        v->addLayout(row);
        m_summary = new QLabel(this);
        m_summary->setWordWrap(true);
        m_summary->setForegroundRole(QPalette::PlaceholderText);
        m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(m_summary);
    }

    QLabel* addItem(const QString& name, const QString& id, const QString& accessibleName, const QString& widest,
                    int visual, Qt::PenStyle style, const QString& tip)
    {
        auto* cell = new QWidget(this);
        cell->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
        auto* g = new QGridLayout(cell);
        g->setContentsMargins(0, 0, 0, 0);
        g->setHorizontalSpacing(6);
        g->setVerticalSpacing(2);
        int col = 0;
        if (visual >= 0) {
            g->addWidget(new Swatch(visual, style, cell), 0, 0, Qt::AlignVCenter);
            col = 1;
        }
        auto* label = new QLabel(name, cell);
        label->setForegroundRole(QPalette::PlaceholderText);
        g->addWidget(label, 0, col, Qt::AlignLeft | Qt::AlignVCenter);
        auto* value = new QLabel(QStringLiteral("\u2014"), cell);
        value->setObjectName(id);
        value->setAccessibleName(accessibleName);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        setRelativeFont(value, font(), 1.6, QFont::DemiBold, widest);
        g->addWidget(value, 1, 0, 1, col + 1, Qt::AlignLeft | Qt::AlignVCenter);
        label->setBuddy(value);
        if (!tip.isEmpty())
            cell->setToolTip(tip);
        m_items->addWidget(cell);
        m_cells.push_back(cell);
        m_values.push_back(value);
        return value;
    }

    QLabel* summary() const { return m_summary; }
    QLabel* value(std::size_t i) const { return i < m_values.size() ? m_values[i] : nullptr; }
    void setValue(std::size_t i, const QString& text)
    {
        if (QLabel* l = value(i); l && l->text() != text)
            l->setText(text);
    }
    void setSummary(const QString& text)
    {
        if (m_summary->text() != text)
            m_summary->setText(text);
        m_summary->setHidden(text.isEmpty());
    }
    bool isStacked() const { return m_items->direction() == QBoxLayout::TopToBottom; }

    void reflow()
    {
        int wide = 0;
        for (const auto* c : m_cells)
            wide += c->sizeHint().width();
        wide += kGap * std::max(0, static_cast<int>(m_cells.size()) - 1);
        const auto dir = width() >= wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom;
        if (m_items->direction() == dir)
            return;
        m_items->setDirection(dir);
        m_items->setSpacing(dir == QBoxLayout::LeftToRight ? kGap : 4);
        updateGeometry();
    }

    QSize minimumSizeHint() const override
    {
        int w = 0;
        for (const auto* c : m_cells)
            w = std::max(w, c->minimumSizeHint().width());
        return {w, QWidget::minimumSizeHint().height()};
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        reflow();
    }

private:
    static constexpr int kGap = 32;
    QBoxLayout* m_items;
    QLabel* m_summary = nullptr;
    std::vector<QWidget*> m_cells;
    std::vector<QLabel*> m_values;
};

// ---------------------------------------------------------------------------

const std::vector<int>& MainWindow::intervalChoicesMs()
{
    static const std::vector<int> v {500, 1000, 2000, 5000};
    return v;
}

const std::vector<int>& MainWindow::windowChoices()
{
    static const std::vector<int> v {60, 300, 900};
    return v;
}

MainWindow::MainWindow(MainWindowOptions options, QWidget* parent)
    : QMainWindow(parent)
    , m_options(std::move(options))
{
    setWindowTitle(QStringLiteral("bex"));
    setWindowIcon(bexIcon());
    setObjectName(QStringLiteral("mainWindow"));
    if (m_options.settingsFile.isEmpty())
        m_settings = std::make_unique<QSettings>();
    else
        m_settings = std::make_unique<QSettings>(m_options.settingsFile, QSettings::IniFormat);
    loadSettings();

    // End task; see MainWindowOptions::endTask. Only the live /proc may
    // authorize a signal: numbers from injected snapshots or another tree
    // could name an unrelated process here.
    EndTaskRunner::Backend endTask = m_options.endTask;
    if (!endTask && m_options.startCollector && m_options.sampler.procRoot == "/proc")
        endTask = [](const ps::endtask::Target& t, ps::endtask::Signal s) { return ps::endtask::send(t, s); };
    if (endTask)
        m_endTaskRunner = new EndTaskRunner(
            std::move(endTask),
            [this](const ps::endtask::Target& t, ps::endtask::Signal s, const ps::endtask::Result& r) {
                onEndTaskDone(t, s, r);
            },
            this);
    else
        m_endTaskUnavailable = QStringLiteral("Ending tasks needs the live process list of this system's /proc");

    // One clock drives all scrolling between samples; see updateFrameDriver().
    m_frameTimer = new QTimer(this);
    m_frameTimer->setObjectName(QStringLiteral("frameTimer"));
    m_frameTimer->setTimerType(Qt::PreciseTimer);
    connect(m_frameTimer, &QTimer::timeout, this, &MainWindow::advanceFrame);

    buildActions();
    buildToolbar();
    buildMenus();

    auto* central = new QWidget(this);
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(10);
    layout->addWidget(buildNavigation());
    auto* navDivider = new QFrame(central);
    navDivider->setObjectName(QStringLiteral("navDivider"));
    navDivider->setFrameShape(QFrame::VLine);
    navDivider->setFrameShadow(QFrame::Plain);
    navDivider->setForegroundRole(QPalette::Mid);
    layout->addWidget(navDivider);
    m_stack = new QStackedWidget(central);
    m_stack->addWidget(new PageScroll(buildCpuPage()));
    m_stack->addWidget(new PageScroll(buildMemoryPage()));
    m_stack->addWidget(new PageScroll(buildDiskPage()));
    m_stack->addWidget(new PageScroll(buildNetworkPage()));
    m_stack->addWidget(new PageScroll(buildTemperaturePage()));
    m_stack->addWidget(buildProcessPage());
    layout->addWidget(m_stack, 1);
    setCentralWidget(central);

    // Shown only while something is wrong or a message (export) is up.
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("statusLabel"));
    m_statusLabel->setAccessibleName(QStringLiteral("Status"));
    statusBar()->addWidget(m_statusLabel, 1);
    connect(statusBar(), &QStatusBar::messageChanged, this, &MainWindow::updateStatusBarVisibility);

    for (auto* chart : allCharts())
        chart->setWindowSeconds(m_windowSeconds);
    applyDetails();

    const int page = std::clamp(m_settings->value(QStringLiteral("page"), 0).toInt(), 0, PageCount - 1);
    showPage(static_cast<Page>(page));
    // Give initial focus to the central container (ClickFocus) so neither the
    // toolbar's rangeCombo nor a navigation button opens with a stray keyboard
    // focus rectangle on startup, while Tab still navigates normally.
    central->setFocusPolicy(Qt::ClickFocus);
    central->setFocus();

    if (!restoreGeometry(m_settings->value(QStringLiteral("geometry")).toByteArray()))
        resize(1180, 780);

    m_staleTimer = new QTimer(this);
    m_staleTimer->setInterval(500);
    connect(m_staleTimer, &QTimer::timeout, this, &MainWindow::checkStale);
    if (!m_options.clock && m_options.startCollector)
        m_options.clock = &ps::monotonicSeconds;
    m_waitingSince = now();
    m_graceIntervalMs = m_intervalMs;

    m_exporter = new CsvExporter(m_options.csvWriter, this);
    connect(m_exporter, &CsvExporter::finished, this, &MainWindow::onCsvExportFinished);

    if (m_options.startCollector) {
        m_collector = new Collector(m_options.sampler, this);
        connect(m_collector, &Collector::snapshotReady, this, &MainWindow::applySnapshot);
        connect(m_collector, &Collector::startErrorChanged, this, &MainWindow::updateStatus);
        m_collector->start(m_intervalMs);
    }
    if (m_options.windowProbe.enabled && (m_options.startCollector || m_options.windowProbe.query)) {
        windows::ProbeOptions probe = m_options.windowProbe;
        if (!probe.clock && m_options.clock)
            probe.clock = m_options.clock;
        m_probe = new windows::WindowProbe(std::move(probe), this);
        connect(m_probe, &windows::WindowProbe::resultReady, this, &MainWindow::applyWindowOwners);
        m_ownersAwaiting = true;
        m_ownersAwaitingSince = now();
        m_probe->start(processListShown());
    }
    updateGrouping();
    if (m_options.clock)
        m_staleTimer->start();
    updateStatus();
    updateSummaries();
    updateFacts();
}

MainWindow::~MainWindow()
{
    shutdown();
}

bool MainWindow::shutdown(int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    m_shutdown = true;
    if (m_staleTimer)
        m_staleTimer->stop();
    if (m_frameTimer)
        m_frameTimer->stop();
    bool clean = true;
    if (m_collector)
        clean = m_collector->stop(timeoutMs);
    if (m_probe) {
        const int remaining = static_cast<int>(std::max<qint64>(0, timeoutMs - clock.elapsed()));
        clean = m_probe->stop(std::min(remaining, 1000)) && clean;
    }
    if (m_exporter && m_exporter->isBusy()) {
        // A local export normally finishes in milliseconds; one stuck on a
        // stalled mount is abandoned like the capacity probe (atomic save:
        // the destination is either complete or untouched).
        const int remaining = static_cast<int>(std::max<qint64>(0, timeoutMs - clock.elapsed()));
        const QString path = m_exporter->currentPath();
        if (!m_exporter->shutdown(std::min(remaining, 1000))) {
            clean = false;
            // The worker cannot be cancelled inside a blocked write; say so
            // rather than let the export vanish silently.
            qWarning("bex: the CSV export to \"%s\" was still blocked at exit and was abandoned. That file is "
                     "either complete or unchanged; on a filesystem without unnamed temporary files a leftover "
                     "\"%s.XXXXXX\" temporary may remain beside it.",
                     qUtf8Printable(path), qUtf8Printable(QFileInfo(path).fileName()));
        }
    }
    // Any open confirmation or About dialog is closed.
    if (m_aboutBox) {
        m_aboutBox->hide();
        m_aboutBox->deleteLater();
        m_aboutBox = nullptr;
    }
    if (m_endTaskBox) {
        m_endTaskBox->hide();
        m_endTaskBox->deleteLater();
        m_endTaskBox = nullptr;
    }
    if (m_endTaskRunner && m_endTaskRunner->isBusy()) {
        const int remaining = static_cast<int>(std::max<qint64>(0, timeoutMs - clock.elapsed()));
        clean = m_endTaskRunner->shutdown(std::min(remaining, 1000)) && clean;
    }
    return clean;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();
    shutdown();
    event->accept();
}

bool MainWindow::event(QEvent* event)
{
    const bool result = QMainWindow::event(event);
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide)
        updateFrameDriver();
    return result;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (m_procTable && event->type() == QEvent::ContextMenu
        && (watched == m_procTable || watched == m_procTable->viewport()))
        return processContextMenu(watched, static_cast<QContextMenuEvent*>(event));
    // The platform turns the Menu key into a context menu event; Shift+F10,
    // the other usual key for it, is left to applications.
    if (m_procTable && watched == m_procTable && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_F10 && key->modifiers() == Qt::ShiftModifier) {
            QContextMenuEvent menu(QContextMenuEvent::Keyboard, QPoint(),
                                   m_procTable->viewport()->mapToGlobal(QPoint()));
            return processContextMenu(watched, &menu);
        }
    }
    // Exposure of the native window: covered, on another workspace, or (on
    // Wayland) no longer given frames by the compositor.
    if (watched == m_watchedWindow && event->type() == QEvent::Expose)
        updateFrameDriver();
    else if (m_procTable && watched == m_procTable->viewport() && event->type() == QEvent::Resize)
        fitProcessNameColumn();
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
        updateFrameDriver();
    else if (event->type() == QEvent::FontChange)
        applyScaledFonts();
}

// ---------------------------------------------------------------------------
// Settings

void MainWindow::loadSettings()
{
    const auto& intervals = intervalChoicesMs();
    const int interval = m_options.initialIntervalMs > 0 ? m_options.initialIntervalMs
                                                         : m_settings->value(QStringLiteral("intervalMs"), 1000).toInt();
    m_intervalMs = std::find(intervals.begin(), intervals.end(), interval) != intervals.end() ? interval : 1000;
    const auto& windows = windowChoices();
    const int window = m_settings->value(QStringLiteral("windowSeconds"), 300).toInt();
    m_windowSeconds = std::find(windows.begin(), windows.end(), window) != windows.end() ? window : 300;
    m_wantedDisk = m_settings->value(QStringLiteral("disk")).toString();
    m_wantedNet = m_settings->value(QStringLiteral("interface")).toString();
    m_sensorsChosen = m_settings->contains(QStringLiteral("sensors"));
    m_wantedSensors = m_settings->value(QStringLiteral("sensors")).toStringList();
    m_details = m_settings->value(QStringLiteral("details"), false).toBool();
    m_smooth = m_settings->value(QStringLiteral("smoothGraphs"), true).toBool();
}

void MainWindow::saveSettings()
{
    m_settings->setValue(QStringLiteral("intervalMs"), m_intervalMs);
    m_settings->setValue(QStringLiteral("windowSeconds"), m_windowSeconds);
    m_settings->setValue(QStringLiteral("page"), static_cast<int>(currentPage()));
    m_settings->setValue(QStringLiteral("disk"), m_wantedDisk);
    m_settings->setValue(QStringLiteral("interface"), m_wantedNet);
    if (m_sensorsChosen)
        m_settings->setValue(QStringLiteral("sensors"), m_wantedSensors);
    m_settings->setValue(QStringLiteral("processView/sortColumn"), m_procProxy->sortColumn());
    m_settings->setValue(QStringLiteral("processView/sortOrder"), static_cast<int>(m_procProxy->sortOrder()));
    saveGroupExpansion();
    m_settings->setValue(QStringLiteral("details"), m_details);
    m_settings->setValue(QStringLiteral("smoothGraphs"), m_smooth);
    m_settings->setValue(QStringLiteral("geometry"), saveGeometry());
    m_settings->sync();
}

// ---------------------------------------------------------------------------
// Construction

void MainWindow::buildActions()
{
    m_pauseAction = new QAction(QStringLiteral("Pause"), this);
    m_pauseAction->setObjectName(QStringLiteral("pauseAction"));
    m_pauseAction->setCheckable(true);
    m_pauseAction->setShortcuts({QKeySequence(Qt::Key_P), QKeySequence(Qt::Key_Pause)});
    m_pauseAction->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    m_pauseAction->setToolTip(QStringLiteral("Pause collection and freeze the view (P)"));
    connect(m_pauseAction, &QAction::toggled, this, &MainWindow::setPaused);
    addAction(m_pauseAction); // the shortcut works whatever the menu bar does

    m_exportAction = new QAction(QStringLiteral("Export History as CSV\u2026"), this);
    m_exportAction->setObjectName(QStringLiteral("exportAction"));
    m_exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    m_exportAction->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_exportAction->setToolTip(QStringLiteral("Save the retained history (up to 15 minutes) as CSV (Ctrl+E)"));
    connect(m_exportAction, &QAction::triggered, this, &MainWindow::exportCsvInteractive);
    addAction(m_exportAction);

    // View > Update Interval: one checked choice, the same saved setting
    // and cadence the collector runs at.
    m_intervalGroup = new QActionGroup(this);
    m_intervalGroup->setExclusive(true);
    for (int ms : intervalChoicesMs()) {
        auto* a = new QAction(intervalText(ms), m_intervalGroup);
        a->setObjectName(QStringLiteral("intervalAction%1").arg(ms));
        a->setCheckable(true);
        a->setChecked(ms == m_intervalMs);
        a->setData(ms);
    }
    connect(m_intervalGroup, &QActionGroup::triggered, this,
            [this](QAction* a) { setIntervalMs(a->data().toInt()); });

    m_quitAction = new QAction(QStringLiteral("Quit"), this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    m_quitAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_quitAction, &QAction::triggered, this, &QWidget::close);

    m_aboutAction = new QAction(QStringLiteral("About bex"), this);
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::showAbout);

    m_detailsAction = new QAction(QStringLiteral("Details"), this);
    m_detailsAction->setObjectName(QStringLiteral("detailsAction"));
    m_detailsAction->setCheckable(true);
    m_detailsAction->setChecked(m_details);
    m_detailsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    m_detailsAction->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    m_detailsAction->setToolTip(QStringLiteral("Show every reading: secondary series, per-CPU charts, swap, "
                                               "filesystems, sensor list, all process columns and the navigation's "
                                               "detail lines (Ctrl+D)"));
    connect(m_detailsAction, &QAction::toggled, this, &MainWindow::setDetailsVisible);

    m_smoothAction = new QAction(QStringLiteral("Smooth Graphs"), this);
    m_smoothAction->setObjectName(QStringLiteral("smoothAction"));
    m_smoothAction->setCheckable(true);
    m_smoothAction->setChecked(m_smooth);
    m_smoothAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    m_smoothAction->setToolTip(QStringLiteral("Scroll graphs continuously between samples, up to 60 frames per "
                                              "second. Off, graphs move once per sample: less power, less motion "
                                              "(%1)")
                                   .arg(m_smoothAction->shortcut().toString(QKeySequence::NativeText)));
    connect(m_smoothAction, &QAction::toggled, this, &MainWindow::setSmoothGraphs);
    addAction(m_smoothAction);

    const QStringList names {QStringLiteral("CPU"),          QStringLiteral("Memory"),
                             QStringLiteral("Disks"),        QStringLiteral("Network"),
                             QStringLiteral("Temperatures"), QStringLiteral("Processes")};
    for (int i = 0; i < PageCount; ++i) {
        auto* a = new QAction(names[i], this);
        a->setShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)));
        connect(a, &QAction::triggered, this, [this, i] { showPage(static_cast<Page>(i)); });
        addAction(a);
        m_pageActions.push_back(a);
    }
}

void MainWindow::buildToolbar()
{
    auto* tb = addToolBar(QStringLiteral("Controls"));
    tb->setObjectName(QStringLiteral("controlsToolbar"));
    tb->setMovable(false);
    tb->setFloatable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto* rangeLabel = new QLabel(QStringLiteral("History"), tb);
    rangeLabel->setObjectName(QStringLiteral("rangeLabel"));
    const int pad = fontMetrics().averageCharWidth();
    rangeLabel->setContentsMargins(pad / 2, 0, pad, 0);
    m_rangeCombo = new QComboBox(tb);
    m_rangeCombo->setObjectName(QStringLiteral("rangeCombo"));
    m_rangeCombo->setAccessibleName(QStringLiteral("Chart time range"));
    m_rangeCombo->setToolTip(QStringLiteral("Time span shown by the charts; 15 minutes of history is kept"));
    for (int s : windowChoices())
        m_rangeCombo->addItem(QStringLiteral("%1 min").arg(s / 60), s);
    m_rangeCombo->setCurrentIndex(m_rangeCombo->findData(m_windowSeconds));
    rangeLabel->setBuddy(m_rangeCombo);
    connect(m_rangeCombo, &QComboBox::activated, this,
            [this](int index) { setWindowSeconds(m_rangeCombo->itemData(index).toInt()); });
    tb->addWidget(rangeLabel);
    tb->addWidget(m_rangeCombo);
    tb->addSeparator();
    tb->addAction(m_detailsAction);
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(m_exportAction);
    file->addSeparator();
    file->addAction(m_quitAction);
    auto* view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(m_pauseAction);
    auto* interval = view->addMenu(QStringLiteral("Update Interval"));
    interval->setObjectName(QStringLiteral("intervalMenu"));
    interval->addActions(m_intervalGroup->actions());
    view->addSeparator();
    view->addAction(m_detailsAction);
    view->addAction(m_smoothAction);
    view->addSeparator();
    for (auto* a : m_pageActions)
        view->addAction(a);
    auto* help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(m_aboutAction);
}

QWidget* MainWindow::buildNavigation()
{
    auto* nav = new QWidget(this);
    m_navPanel = nav;
    nav->setObjectName(QStringLiteral("navigation"));
    nav->setAccessibleName(QStringLiteral("Metric categories"));
    auto* v = new QVBoxLayout(nav);
    v->setContentsMargins(0, 2, 0, 0);
    v->setSpacing(4);
    const QStringList names {QStringLiteral("CPU"),          QStringLiteral("Memory"),
                             QStringLiteral("Disks"),        QStringLiteral("Network"),
                             QStringLiteral("Temperatures"), QStringLiteral("Processes")};
    const QStringList ids {QStringLiteral("navCpu"),          QStringLiteral("navMemory"),
                           QStringLiteral("navDisks"),        QStringLiteral("navNetwork"),
                           QStringLiteral("navTemperatures"), QStringLiteral("navProcesses")};
    auto* group = new QButtonGroup(nav);
    group->setExclusive(true);
    for (int i = 0; i < PageCount; ++i) {
        auto* b = new MetricButton(names[i], nav);
        b->setObjectName(ids[i]);
        group->addButton(b, i);
        connect(b, &QAbstractButton::toggled, this, [this, i](bool on) {
            if (on)
                showPage(static_cast<Page>(i));
        });
        v->addWidget(b);
        m_nav.push_back(b);
    }
    m_nav[CpuPage]->setSparkline(&m_history, {std::string(ps::keys::cpuTotal)}, 100);
    m_nav[MemoryPage]->setSparkline(&m_history, {std::string(ps::keys::memUsed)});
    m_nav[DiskPage]->setSparkline(&m_history, {std::string(ps::keys::diskAllRead), std::string(ps::keys::diskAllWrite)});
    m_nav[NetworkPage]->setSparkline(&m_history, {std::string(ps::keys::netAllRx), std::string(ps::keys::netAllTx)});
    // The same colour and pen as each page's main trace.
    m_nav[CpuPage]->setSparklineVisuals({{0, Qt::SolidLine}});
    m_nav[MemoryPage]->setSparklineVisuals({{3, Qt::SolidLine}});
    m_nav[DiskPage]->setSparklineVisuals({{2, Qt::SolidLine}, {4, Qt::DashLine}});
    m_nav[NetworkPage]->setSparklineVisuals({{1, Qt::SolidLine}, {5, Qt::DashLine}});
    m_nav[TemperaturePage]->setSparklineVisuals({{temperatureVisual(0), Qt::SolidLine}});
    // Widest values each entry is expected to show; see MetricButton.
    m_nav[CpuPage]->setValueTemplate(QStringLiteral("100.0 %"));
    m_nav[MemoryPage]->setValueTemplate(QStringLiteral("0000 MiB used"));
    m_nav[DiskPage]->setValueTemplate(QStringLiteral("R 0000 MiB/s  W 0000 MiB/s"));
    m_nav[NetworkPage]->setValueTemplate(QStringLiteral("\u2193 0000 MiB/s  \u2191 0000 MiB/s"));
    m_nav[TemperaturePage]->setValueTemplate(QStringLiteral("100.0 \u00B0C"));
    m_nav[ProcessPage]->setValueTemplate(QStringLiteral("00000 processes"));
    v->addStretch(1);
    int navWidth = 228;
    for (const auto* b : m_nav)
        navWidth = std::max(navWidth, b->sizeHint().width());
    nav->setFixedWidth(navWidth);
    return nav;
}

QWidget* MainWindow::buildCpuPage()
{
    auto* page = new QWidget;
    page->setAccessibleName(QStringLiteral("CPU"));
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(10, 6, 10, 8);
    v->setSpacing(8);
    v->addWidget(makeHeading(QStringLiteral("CPU"), page));
    v->addWidget(buildHeadline(CpuPage, QStringLiteral("cpu"), page));

    auto* grid = new FactGrid(4, page);
    m_factGrids.push_back(grid);
    const QString convention = QStringLiteral("Percent of all logical CPUs together: 100% means every CPU is busy.");
    struct F { const char* id; const char* name; QString tip; };
    const F fs[] = {
        {"cpu.total", "Total", convention},
        {"cpu.user", "User", QStringLiteral("User and nice time; virtual machine guest time is included once")},
        {"cpu.system", "System", QStringLiteral("Kernel, IRQ and soft-IRQ time")},
        {"cpu.iowait", "I/O wait", QStringLiteral("Idle time with I/O outstanding (a hint, not busy time)")},
        {"cpu.load", "Load average", QStringLiteral("Runnable and uninterruptible tasks, 1 / 5 / 15 minute averages")},
        {"cpu.count", "Logical CPUs", QStringLiteral("Online / known logical CPUs")},
        {"cpu.steal", "Steal", QStringLiteral("Time a hypervisor ran other guests while this one wanted CPU")},
        {"cpu.tasks", "Tasks", QStringLiteral("Processes and threads")},
    };
    for (const auto& f : fs) {
        auto* value = new QLabel(kDash, page);
        value->setObjectName(QString::fromLatin1(f.id).replace(QLatin1Char('.'), QLatin1Char('_')));
        addFactPair(grid, QString::fromLatin1(f.name), value, f.tip);
        m_facts.emplace_back(f.id, value);
        m_factMap.emplace(f.id, value);
    }
    v->addWidget(detailOnly(grid));

    m_cpuChart = new TimeChart(QStringLiteral("CPU utilization"), ps::Unit::Percent, page);
    m_cpuChart->setObjectName(QStringLiteral("cpuChart"));
    m_cpuChart->setFixedRange(0, 100);
    m_cpuChart->setHistory(&m_history);
    m_cpuChart->setEmptyText(QStringLiteral("Measuring: CPU use needs two samples"));
    v->addWidget(m_cpuChart, 3);

    auto* coreHeaderRow = new QWidget(page);
    coreHeaderRow->setObjectName(QStringLiteral("coreHeader"));
    auto* coreHeader = new QHBoxLayout(coreHeaderRow);
    coreHeader->setContentsMargins(0, 0, 0, 0);
    coreHeader->setSpacing(12); // some styles (Kvantum themes) have near-zero default layout spacing
    auto* coreHeading = makeSubheading(QStringLiteral("Per logical CPU"), coreHeaderRow);
    coreHeading->setToolTip(QStringLiteral("Each chart is 0\u2013100% of one logical CPU"));
    coreHeading->setAccessibleDescription(coreHeading->toolTip());
    coreHeader->addWidget(coreHeading);
    coreHeader->addStretch(1);
    v->addWidget(detailOnly(coreHeaderRow));

    auto* scroll = new QScrollArea(page);
    scroll->setObjectName(QStringLiteral("coreScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // At least one row of per-core charts stays in view.
    scroll->setMinimumHeight(CoreGrid::kCellH);
    m_coreGrid = new CoreGrid(scroll);
    m_coreGrid->setObjectName(QStringLiteral("coreGrid"));
    scroll->setWidget(m_coreGrid);
    m_coreGrid->setAutoFillBackground(false);
    v->addWidget(detailOnly(scroll), 2);
    return page;
}

QWidget* MainWindow::buildMemoryPage()
{
    auto* page = new QWidget;
    page->setAccessibleName(QStringLiteral("Memory"));
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(10, 6, 10, 8);
    v->setSpacing(8);
    v->addWidget(makeHeading(QStringLiteral("Memory"), page));
    v->addWidget(buildHeadline(MemoryPage, QStringLiteral("memory"), page));

    auto* grid = new FactGrid(4, page);
    m_factGrids.push_back(grid);
    struct F { const char* id; const char* name; QString tip; };
    const F fs[] = {
        {"mem.total", "Total", QStringLiteral("MemTotal: usable RAM")},
        {"mem.used", "Used", QStringLiteral("Total minus available, the same as free(1) from procps-ng 4")},
        {"mem.available", "Available",
         QStringLiteral("MemAvailable: the kernel's estimate of memory that can be given to programs without swapping")},
        {"mem.cache", "Buff/cache",
         QStringLiteral("Buffers + page cache + reclaimable slab. Overlaps Available: most of it is freed on demand")},
        {"mem.free", "Free", QStringLiteral("MemFree: completely unused RAM")},
        {"mem.shared", "Shared", QStringLiteral("Shmem: tmpfs and shared memory (part of buff/cache)")},
        {"swap.used", "Swap used", QStringLiteral("SwapTotal minus SwapFree")},
        {"swap.total", "Swap total", QStringLiteral("All active swap devices and files")},
    };
    for (const auto& f : fs) {
        auto* value = new QLabel(kDash, page);
        value->setObjectName(QString::fromLatin1(f.id).replace(QLatin1Char('.'), QLatin1Char('_')));
        addFactPair(grid, QString::fromLatin1(f.name), value, f.tip);
        m_facts.emplace_back(f.id, value);
        m_factMap.emplace(f.id, value);
    }
    v->addWidget(detailOnly(grid));

    m_memChart = new TimeChart(QStringLiteral("Physical memory"), ps::Unit::Bytes, page);
    m_memChart->setObjectName(QStringLiteral("memoryChart"));
    m_memChart->setHistory(&m_history);
    m_memChart->setEmptyText(QStringLiteral("/proc/meminfo is unavailable"));
    v->addWidget(m_memChart, 3);

    m_swapChart = new TimeChart(QStringLiteral("Swap"), ps::Unit::Bytes, page);
    m_swapChart->setObjectName(QStringLiteral("swapChart"));
    m_swapChart->setHistory(&m_history);
    m_swapChart->setSeries({chartSeries(std::string(ps::keys::swapUsed), QStringLiteral("Swap used"), 3,
                                        Qt::SolidLine, true)});
    m_swapChart->setEmptyText(QStringLiteral("No swap is configured"));
    v->addWidget(detailOnly(m_swapChart), 2);
    return page;
}

QWidget* MainWindow::buildDiskPage()
{
    auto* page = new QWidget;
    page->setAccessibleName(QStringLiteral("Disks"));
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(10, 6, 10, 8);
    v->setSpacing(8);
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    header->addWidget(makeHeading(QStringLiteral("Disks"), page));
    header->addStretch(1);
    auto* devLabel = new QLabel(QStringLiteral("Device:"), page);
    m_diskCombo = new QComboBox(page);
    m_diskCombo->setObjectName(QStringLiteral("diskSelector"));
    m_diskCombo->setAccessibleName(QStringLiteral("Disk device"));
    m_diskCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_diskCombo->setToolTip(QStringLiteral("Whole disks only, never partitions, so no byte is counted twice. The "
                                           "sum covers physical disks; device-mapper, RAID and zram devices carry "
                                           "that traffic again and are listed separately"));
    devLabel->setBuddy(m_diskCombo);
    connect(m_diskCombo, &QComboBox::activated, this, [this] {
        m_wantedDisk = m_diskCombo->currentData().toString();
        applyDiskSelection();
        updateFacts();
        updateSummaries();
    });
    header->addWidget(devLabel);
    header->addWidget(m_diskCombo);
    v->addLayout(header);
    v->addWidget(buildHeadline(DiskPage, QStringLiteral("disk"), page));

    auto* grid = new FactGrid(4, page);
    m_factGrids.push_back(grid);
    struct F { const char* id; const char* name; QString tip; };
    const F fs[] = {
        {"disk.read", "Read", {}},
        {"disk.write", "Write", {}},
        {"disk.busy", "Busy", QStringLiteral("Share of time with at least one request in flight")},
        {"disk.size", "Size", {}},
    };
    for (const auto& f : fs) {
        auto* value = new QLabel(kDash, page);
        value->setObjectName(QString::fromLatin1(f.id).replace(QLatin1Char('.'), QLatin1Char('_')));
        addFactPair(grid, QString::fromLatin1(f.name), value, f.tip);
        m_facts.emplace_back(f.id, value);
        m_factMap.emplace(f.id, value);
    }
    v->addWidget(detailOnly(grid));

    m_diskChart = new TimeChart(QStringLiteral("Disk throughput"), ps::Unit::BytesPerSecond, page);
    m_diskChart->setObjectName(QStringLiteral("diskChart"));
    m_diskChart->setHistory(&m_history);
    m_diskChart->setEmptyText(QStringLiteral("Measuring: throughput needs two samples"));
    v->addWidget(m_diskChart, 3);

    auto* fsHeading = makeSubheading(QStringLiteral("Local filesystems"), page);
    fsHeading->setToolTip(QStringLiteral("Capacity from statvfs on local filesystems only; network, FUSE and "
                                         "automount points are never touched"));
    v->addWidget(detailOnly(fsHeading));
    m_fsTable = new QTreeWidget(page);
    m_fsTable->setObjectName(QStringLiteral("filesystemTable"));
    m_fsTable->setAccessibleName(QStringLiteral("Local filesystems"));
    m_fsTable->setRootIsDecorated(false);
    m_fsTable->setUniformRowHeights(true);
    m_fsTable->setAlternatingRowColors(true);
    m_fsTable->setHeaderLabels({QStringLiteral("Mounted at"), QStringLiteral("Device"), QStringLiteral("Type"),
                                QStringLiteral("Size"), QStringLiteral("Used"), QStringLiteral("Available"),
                                QStringLiteral("Use")});
    m_fsTable->headerItem()->setToolTip(5, QStringLiteral("Space unprivileged users can still write (excludes "
                                                          "blocks reserved for root)"));
    m_fsTable->headerItem()->setToolTip(6, QStringLiteral("Used / (used + available), like df"));
    m_fsTable->setItemDelegateForColumn(6, new UsageDelegate(m_fsTable));
    const QFontMetrics fm(font());
    m_fsTable->setColumnWidth(0, fm.horizontalAdvance(QStringLiteral("/home/user/mount")) + 24);
    m_fsTable->setColumnWidth(1, fm.horizontalAdvance(QStringLiteral("/dev/nvme0n1p3")) + 24);
    m_fsTable->setColumnWidth(2, fm.horizontalAdvance(QStringLiteral("btrfs")) + 24);
    for (int c = 3; c <= 5; ++c)
        m_fsTable->setColumnWidth(c, fm.horizontalAdvance(QStringLiteral("1000 GiB")) + 24);
    v->addWidget(detailOnly(m_fsTable), 1);
    return page;
}

QWidget* MainWindow::buildNetworkPage()
{
    auto* page = new QWidget;
    page->setAccessibleName(QStringLiteral("Network"));
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(10, 6, 10, 8);
    v->setSpacing(8);
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    header->addWidget(makeHeading(QStringLiteral("Network"), page));
    header->addStretch(1);
    auto* ifLabel = new QLabel(QStringLiteral("Interface:"), page);
    m_netCombo = new QComboBox(page);
    m_netCombo->setObjectName(QStringLiteral("netSelector"));
    m_netCombo->setAccessibleName(QStringLiteral("Network interface"));
    m_netCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_netCombo->setToolTip(QStringLiteral("The sum covers physical interfaces and never includes loopback; bridges, "
                                          "VPN tunnels and container links carry traffic already counted on a "
                                          "physical interface, so they are listed separately"));
    ifLabel->setBuddy(m_netCombo);
    connect(m_netCombo, &QComboBox::activated, this, [this] {
        m_wantedNet = m_netCombo->currentData().toString();
        applyNetSelection();
        updateFacts();
        updateSummaries();
    });
    header->addWidget(ifLabel);
    header->addWidget(m_netCombo);
    v->addLayout(header);
    v->addWidget(buildHeadline(NetworkPage, QStringLiteral("net"), page));

    auto* grid = new FactGrid(4, page);
    m_factGrids.push_back(grid);
    struct F { const char* id; const char* name; };
    const F fs[] = {{"net.rx", "Receive"}, {"net.tx", "Transmit"}, {"net.state", "State"}, {"net.kind", "Type"}};
    for (const auto& f : fs) {
        auto* value = new QLabel(kDash, page);
        value->setObjectName(QString::fromLatin1(f.id).replace(QLatin1Char('.'), QLatin1Char('_')));
        addFactPair(grid, QString::fromLatin1(f.name), value, {});
        m_facts.emplace_back(f.id, value);
        m_factMap.emplace(f.id, value);
    }
    v->addWidget(detailOnly(grid));

    m_netChart = new TimeChart(QStringLiteral("Network throughput"), ps::Unit::BytesPerSecond, page);
    m_netChart->setObjectName(QStringLiteral("netChart"));
    m_netChart->setHistory(&m_history);
    m_netChart->setEmptyText(QStringLiteral("Measuring: throughput needs two samples"));
    v->addWidget(m_netChart, 1);
    return page;
}

QWidget* MainWindow::buildTemperaturePage()
{
    auto* page = new QWidget;
    page->setAccessibleName(QStringLiteral("Temperatures"));
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(10, 6, 10, 8);
    v->setSpacing(8);
    v->addWidget(makeHeading(QStringLiteral("Temperatures"), page));
    v->addWidget(buildHeadline(TemperaturePage, QStringLiteral("temperature"), page));
    m_tempNote = makeNote({}, page);
    m_tempNote->setObjectName(QStringLiteral("temperatureNote"));
    v->addWidget(m_tempNote);

    auto* split = new QSplitter(Qt::Horizontal, page);
    split->setObjectName(QStringLiteral("temperatureSplitter"));
    split->setChildrenCollapsible(false);
    m_tempSplit = split;
    m_sensorList = new QTreeWidget(split);
    m_sensorList->setObjectName(QStringLiteral("sensorList"));
    m_sensorList->setAccessibleName(QStringLiteral("Temperature sensors"));
    m_sensorList->setRootIsDecorated(false);
    m_sensorList->setUniformRowHeights(true);
    m_sensorList->setAlternatingRowColors(true);
    // Sensor names often share a long prefix ("coretemp Core 0", "coretemp
    // Core 1"); eliding in the middle keeps the distinguishing end visible.
    m_sensorList->setTextElideMode(Qt::ElideMiddle);
    m_sensorList->setHeaderLabels({QStringLiteral("Sensor (check to plot)"), QStringLiteral("Now")});
    m_sensorList->headerItem()->setToolTip(0, QStringLiteral("Degrees Celsius from /sys/class/hwmon and "
                                                             "/sys/class/thermal. A sensor that reports a fault or "
                                                             "an impossible value is shown as unavailable"));
    m_sensorList->header()->setStretchLastSection(false);
    m_sensorList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_sensorList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(m_sensorList, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*, int column) {
        if (column != 0)
            return;
        m_sensorsChosen = true;
        // Checked sensors in the list, plus earlier choices that are not
        // listed right now (so they are checked again when they reappear).
        QStringList wanted;
        QSet<QString> listed;
        for (int i = 0; i < m_sensorList->topLevelItemCount(); ++i) {
            auto* item = m_sensorList->topLevelItem(i);
            const QString key = item->data(0, Qt::UserRole).toString();
            listed.insert(key);
            if (item->checkState(0) == Qt::Checked)
                wanted << key;
        }
        for (const QString& key : std::as_const(m_wantedSensors))
            if (!listed.contains(key) && wanted.size() < 64)
                wanted << key;
        m_wantedSensors = wanted;
        applySensorSelection();
        updateSummaries();
    });
    m_tempChart = new TimeChart(QStringLiteral("Temperature"), ps::Unit::Celsius, split);
    m_tempChart->setObjectName(QStringLiteral("temperatureChart"));
    m_tempChart->setHistory(&m_history);
    m_tempChart->setEmptyText(QStringLiteral("Check a sensor to plot it"));
    split->addWidget(m_sensorList);
    split->addWidget(m_tempChart);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    m_tempSplitSizes = {340, 660};
    split->setSizes(m_tempSplitSizes);
    m_sensorList->setMinimumWidth(sensorListMinimumWidth(m_sensorList));
    detailOnly(m_sensorList);
    v->addWidget(split, 1);
    return page;
}

QWidget* MainWindow::buildProcessPage()
{
    auto* page = new QWidget;
    page->setAccessibleName(QStringLiteral("Processes"));
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(10, 6, 10, 8);
    v->setSpacing(6);
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    header->addWidget(makeHeading(QStringLiteral("Processes"), page));
    header->addSpacing(12);
    m_procFilter = new QLineEdit(page);
    m_procFilter->setObjectName(QStringLiteral("processFilter"));
    m_procFilter->setPlaceholderText(QStringLiteral("Filter by name, user, command or PID (Ctrl+F)"));
    m_procFilter->setAccessibleName(QStringLiteral("Process filter"));
    m_procFilter->setClearButtonEnabled(true);
    m_procFilter->setMinimumWidth(200);
    header->addWidget(m_procFilter, 1);
    // End task acts on the selected process; Force end task is only in the
    // row's context menu.
    m_endTaskButton = new QPushButton(QStringLiteral("End task\u2026"), page);
    m_endTaskButton->setObjectName(QStringLiteral("endTaskButton"));
    header->addWidget(m_endTaskButton);
    m_procCount = new QLabel(page);
    m_procCount->setObjectName(QStringLiteral("processCount"));
    m_procCount->setForegroundRole(QPalette::PlaceholderText);
    v->addLayout(header);
    // One line: the count, and only when app detection fails, a short
    // notice whose reason is in its tooltip and accessible description.
    auto* countRow = new QHBoxLayout;
    countRow->setSpacing(fontMetrics().averageCharWidth() * 3);
    countRow->addWidget(m_procCount);
    m_groupNote = makeNote(QString(), page);
    m_groupNote->setObjectName(QStringLiteral("processGroupNote"));
    m_groupNote->setAccessibleName(QStringLiteral("App detection"));
    m_groupNote->setWordWrap(false);
    m_groupNote->hide();
    countRow->addWidget(m_groupNote);
    countRow->addStretch(1);
    v->addLayout(countRow);

    auto* find = new QAction(this);
    find->setShortcut(QKeySequence::Find);
    connect(find, &QAction::triggered, this, [this] {
        showPage(ProcessPage);
        m_procFilter->setFocus(Qt::ShortcutFocusReason);
        m_procFilter->selectAll();
    });
    addAction(find);

    m_procModel = new ProcessModel(this);
    m_procProxy = new ProcessFilterModel(this);
    m_procProxy->setSourceModel(m_procModel);
    connect(m_procFilter, &QLineEdit::textChanged, this, [this](const QString& text) {
        const bool was = m_procProxy->isFiltering();
        m_procProxy->setFilterString(text);
        // Every filter starts with all groups open; clearing it brings back
        // the groups as the user left them.
        if (was && !m_procProxy->isFiltering())
            m_filterExpanded = {true, true, true};
        syncGroupExpansion();
        updateProcessCount();
        updateEndTaskActions();
    });

    // Groups: saved expansion; a showKernelThreads=true from before groups
    // existed opens the kernel group.
    const auto expandKey = [](int g) {
        switch (g) {
        case ProcessModel::Apps: return QStringLiteral("processView/expandApps");
        case ProcessModel::Background: return QStringLiteral("processView/expandBackground");
        default: return QStringLiteral("processView/expandKernel");
        }
    };
    for (int g = 0; g < ProcessModel::GroupCount; ++g) {
        const QString key = expandKey(g);
        if (m_settings->contains(key))
            m_groupExpanded[static_cast<std::size_t>(g)] = m_settings->value(key).toBool();
        else if (g == ProcessModel::Kernel)
            m_groupExpanded[static_cast<std::size_t>(g)] =
                m_settings->value(QStringLiteral("showKernelThreads"), false).toBool();
    }

    m_procTable = new QTreeView(page);
    m_procTable->setObjectName(QStringLiteral("processTable"));
    m_procTable->setAccessibleName(QStringLiteral("Processes"));
    m_procTable->setAccessibleDescription(QStringLiteral("Grouped as apps, background processes and kernel "
                                                         "processes. CPU % is of one logical CPU, so it can exceed "
                                                         "100%. Delete or the context menu ends the selected "
                                                         "process, after confirmation."));
    m_procTable->setModel(m_procProxy);
    m_procTable->setItemDelegate(new ProcessCellDelegate(m_procTable));
    m_procTable->setSortingEnabled(true);
    m_procTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_procTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_procTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_procTable->setAlternatingRowColors(true);
    m_procTable->setWordWrap(false);
    m_procTable->setTextElideMode(Qt::ElideRight);
    m_procTable->setUniformRowHeights(true);
    m_procTable->setAllColumnsShowFocus(true);
    m_procTable->header()->setStretchLastSection(true);
    m_procTable->header()->setSectionsMovable(false);
    const QFontMetrics fm(font());
    const auto width = [&](const char* sample) { return fm.horizontalAdvance(QString::fromLatin1(sample)) + 28; };
    // Cells beside the name also hold the delegate's gutter on both sides.
    const auto cellWidth = [&](const char* sample) { return width(sample) + 2 * ProcessCellDelegate::gutter(fm); };
    m_procTable->setColumnWidth(ProcessModel::Pid, cellWidth("4194304"));
    // The name column also holds the group branch indicator and indent.
    m_procTable->setColumnWidth(ProcessModel::Name, width("gnome-shell-calen") + m_procTable->indentation());
    m_procTable->setColumnWidth(ProcessModel::User, cellWidth("username"));
    m_procTable->setColumnWidth(ProcessModel::Cpu, cellWidth("1000.0 %") + 16);
    m_procTable->setColumnWidth(ProcessModel::Memory, cellWidth("Memory (RSS)"));
    m_procTable->setColumnWidth(ProcessModel::Threads, cellWidth("Threads"));
    m_procTable->setColumnWidth(ProcessModel::State, cellWidth("Disk wait"));
    m_procTable->setColumnWidth(ProcessModel::Nice, cellWidth("Nice"));
    m_procTable->setColumnWidth(ProcessModel::IoRead, cellWidth("1000 KiB/s"));
    m_procTable->setColumnWidth(ProcessModel::IoWrite, cellWidth("1000 KiB/s"));
    m_procTable->setColumnWidth(ProcessModel::Age, cellWidth("12m 05s"));
    // In Simple, the name fills what the numbers leave, but never shrinks
    // below a readable name under a group (two indentation steps): a narrow
    // window scrolls sideways instead.
    m_procNameMinWidth = 2 * m_procTable->indentation() + width("gnome-shell");
    m_procTable->viewport()->installEventFilter(this);
    connect(m_procTable->header(), &QHeaderView::sectionResized, this, [this](int section) {
        if (section != ProcessModel::Name)
            fitProcessNameColumn();
    });
    // Sorting: saved under new keys since PID and Name swapped places; the
    // old keys are read once.
    int sortColumn = ProcessModel::Cpu;
    if (m_settings->contains(QStringLiteral("processView/sortColumn"))) {
        sortColumn = m_settings->value(QStringLiteral("processView/sortColumn")).toInt();
    } else if (m_settings->contains(QStringLiteral("processSortColumn"))) {
        sortColumn = m_settings->value(QStringLiteral("processSortColumn")).toInt();
        if (sortColumn == 0 || sortColumn == 1)
            sortColumn = 1 - sortColumn;
    }
    sortColumn = std::clamp(sortColumn, 0, ProcessModel::ColumnCount - 1);
    const QVariant order = m_settings->contains(QStringLiteral("processView/sortOrder"))
        ? m_settings->value(QStringLiteral("processView/sortOrder"))
        : m_settings->value(QStringLiteral("processSortOrder"), int(Qt::DescendingOrder));
    const auto sortOrder = order.toInt() == 0 ? Qt::AscendingOrder : Qt::DescendingOrder;
    m_procTable->sortByColumn(sortColumn, sortOrder);
    const auto spanGroups = [this] {
        for (int row = 0; row < m_procProxy->rowCount(); ++row)
            m_procTable->setFirstColumnSpanned(row, QModelIndex(), true);
    };
    spanGroups();
    connect(m_procProxy, &QAbstractItemModel::modelReset, this, [this, spanGroups] {
        spanGroups();
        syncGroupExpansion();
    });
    connect(m_procTable, &QTreeView::expanded, this, [this](const QModelIndex& index) { onGroupExpansion(index, true); });
    connect(m_procTable, &QTreeView::collapsed, this,
            [this](const QModelIndex& index) { onGroupExpansion(index, false); });
    syncGroupExpansion();
    v->addWidget(m_procTable, 1);

    m_endTaskAction = new QAction(QStringLiteral("End task\u2026"), m_procTable);
    m_endTaskAction->setObjectName(QStringLiteral("endTaskAction"));
    // Only while the list has focus: Delete keeps editing the filter.
    m_endTaskAction->setShortcut(QKeySequence::Delete);
    m_endTaskAction->setShortcutContext(Qt::WidgetShortcut);
    m_procTable->addAction(m_endTaskAction);
    connect(m_endTaskAction, &QAction::triggered, this, [this] {
        if (const auto target = processTargetAt(selectedProcessIndex()))
            confirmEndTask(*target, ps::endtask::Signal::Terminate);
    });
    connect(m_endTaskButton, &QPushButton::clicked, m_endTaskAction, &QAction::trigger);
    m_procTable->installEventFilter(this); // context menus; see processContextMenu()
    QItemSelectionModel* selection = m_procTable->selectionModel();
    connect(selection, &QItemSelectionModel::selectionChanged, this, &MainWindow::updateEndTaskActions);
    connect(selection, &QItemSelectionModel::currentChanged, this, &MainWindow::updateEndTaskActions);
    connect(m_procProxy, &QAbstractItemModel::rowsRemoved, this, &MainWindow::updateEndTaskActions);
    connect(m_procProxy, &QAbstractItemModel::layoutChanged, this, &MainWindow::updateEndTaskActions);
    connect(m_procProxy, &QAbstractItemModel::modelReset, this, &MainWindow::updateEndTaskActions);
    updateEndTaskActions();
    return page;
}

// ---------------------------------------------------------------------------
// Controls

void MainWindow::showPage(Page page)
{
    if (page < 0 || page >= PageCount || !m_stack)
        return;
    m_stack->setCurrentIndex(page);
    if (!m_nav[page]->isChecked())
        m_nav[page]->setChecked(true);
    if (page == ProcessPage)
        updateProcesses();
    updateFrameDriver();
}

MainWindow::Page MainWindow::currentPage() const
{
    return m_stack ? static_cast<Page>(m_stack->currentIndex()) : CpuPage;
}

void MainWindow::setPaused(bool paused)
{
    if (paused == m_paused)
        return;
    m_paused = paused;
    if (m_pauseAction->isChecked() != paused) {
        const QSignalBlocker block(m_pauseAction);
        m_pauseAction->setChecked(paused);
    }
    m_pauseAction->setText(paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    m_pauseAction->setIcon(style()->standardIcon(paused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
    m_pauseAction->setToolTip(paused ? QStringLiteral("Resume live collection (P)")
                                     : QStringLiteral("Pause collection and freeze the view (P)"));
    if (m_collector)
        m_collector->setPaused(paused);
    // The window list is frozen with the view. On resume anything asked
    // before the pause is dropped and a fresh list is asked for at once;
    // meanwhile a list that expired during the pause no longer counts.
    if (m_probe) {
        m_probe->setPaused(paused);
        if (!paused) {
            m_ownersAwaiting = true;
            m_ownersAwaitingSince = now();
        }
    }
    if (!paused)
        updateGrouping();
    // Pausing freezes the view but does not make old values fresh: staleness
    // is cleared only by a new, current snapshot. On resume the overdue clock
    // restarts with the collector.
    if (!paused) {
        m_lastAcquired = ps::kNaN;
        m_waitingSince = now();
        m_graceIntervalMs = m_intervalMs;
    }
    updateChartNotices();
    updateProcessCount();
    updateHeadlines();
    updateStatus();
    updateFrameDriver();
}

void MainWindow::setIntervalMs(int ms)
{
    const auto& choices = intervalChoicesMs();
    if (std::find(choices.begin(), choices.end(), ms) == choices.end())
        return;
    for (QAction* a : m_intervalGroup->actions())
        if (a->data().toInt() == ms && !a->isChecked())
            a->setChecked(true);
    if (ms == m_intervalMs)
        return;
    m_intervalMs = ms;
    // The newest sample was taken on the old schedule; judge it by that
    // until one arrives on the new one.
    m_graceIntervalMs = std::max(m_graceIntervalMs, ms);
    if (m_collector)
        m_collector->setIntervalMs(ms);
    m_settings->setValue(QStringLiteral("intervalMs"), ms);
    updateStatus();
}

void MainWindow::setWindowSeconds(int seconds)
{
    const auto& choices = windowChoices();
    if (std::find(choices.begin(), choices.end(), seconds) == choices.end())
        return;
    if (m_rangeCombo->currentData().toInt() != seconds)
        m_rangeCombo->setCurrentIndex(m_rangeCombo->findData(seconds));
    m_windowSeconds = seconds;
    for (auto* chart : allCharts())
        chart->setWindowSeconds(seconds);
    m_settings->setValue(QStringLiteral("windowSeconds"), seconds);
}

void MainWindow::setDetailsVisible(bool on)
{
    if (m_detailsAction->isChecked() != on) {
        const QSignalBlocker block(m_detailsAction);
        m_detailsAction->setChecked(on);
    }
    if (on == m_details)
        return;
    m_details = on;
    m_settings->setValue(QStringLiteral("details"), on);
    applyDetails();
}

void MainWindow::setSmoothGraphs(bool on)
{
    if (m_smoothAction->isChecked() != on) {
        const QSignalBlocker block(m_smoothAction);
        m_smoothAction->setChecked(on);
    }
    if (on == m_smooth)
        return;
    m_smooth = on;
    m_settings->setValue(QStringLiteral("smoothGraphs"), on);
    updateFrameDriver();
}

QWidget* MainWindow::detailOnly(QWidget* widget)
{
    m_detailWidgets.push_back(widget);
    return widget;
}

HeadRow* MainWindow::buildHeadline(Page page, const QString& prefix, QWidget* parent)
{
    auto* head = new HeadRow(parent);
    head->setObjectName(prefix + QStringLiteral("Headline"));
    struct Item {
        QString name, accessible, widest;
        int visual;
        Qt::PenStyle style;
        QString tip;
    };
    const QString rate = QStringLiteral("0000 KiB/s");
    std::vector<Item> items;
    switch (page) {
    case CpuPage:
        items = {{QStringLiteral("Utilization"), QStringLiteral("CPU utilization"), QStringLiteral("100.0 %"), 0,
                  Qt::SolidLine,
                  QStringLiteral("Percent of all logical CPUs together: 100% means every CPU is busy")}};
        break;
    case MemoryPage:
        items = {{QStringLiteral("Used"), QStringLiteral("Memory used"), QStringLiteral("0000 MiB"), 3, Qt::SolidLine,
                  QStringLiteral("Used = total \u2212 available, the same as free(1). IEC units: 1 GiB = 1024\u00b3 "
                                 "bytes")}};
        break;
    case DiskPage:
        items = {{QStringLiteral("Read"), QStringLiteral("Disk read"), rate, 2, Qt::SolidLine, {}},
                 {QStringLiteral("Write"), QStringLiteral("Disk write"), rate, 4, Qt::DashLine, {}}};
        break;
    case NetworkPage:
        items = {{QStringLiteral("Receive"), QStringLiteral("Network receive"), rate, 1, Qt::SolidLine, {}},
                 {QStringLiteral("Transmit"), QStringLiteral("Network transmit"), rate, 5, Qt::DashLine, {}}};
        break;
    case TemperaturePage:
        items = {{QStringLiteral("Hottest sensor"), QStringLiteral("Hottest temperature"),
                  QStringLiteral("100.0 \u00b0C"), -1, Qt::SolidLine, {}}};
        break;
    default:
        break;
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        head->addItem(it.name, prefix + QStringLiteral("HeadValue%1").arg(i), it.accessible, it.widest, it.visual,
                      it.style, it.tip);
    }
    head->summary()->setObjectName(prefix + QStringLiteral("HeadSummary"));
    head->summary()->setAccessibleName(items.empty() ? QString() : items.front().accessible + QStringLiteral(" summary"));
    m_head[page] = head;
    return head;
}

void MainWindow::applyCpuSeries()
{
    std::vector<ChartSeries> series {
        chartSeries(std::string(ps::keys::cpuTotal), QStringLiteral("Total"), 0, Qt::SolidLine, true)};
    if (m_details) {
        series.push_back(chartSeries(std::string(ps::keys::cpuUser), QStringLiteral("User"), 1, Qt::DashLine, false));
        series.push_back(
            chartSeries(std::string(ps::keys::cpuSystem), QStringLiteral("System"), 2, Qt::DotLine, false));
        series.push_back(
            chartSeries(std::string(ps::keys::cpuIowait), QStringLiteral("I/O wait"), 3, Qt::DashDotLine, false));
    }
    if (m_cpuChart->series().size() != series.size())
        m_cpuChart->setSeries(std::move(series));
}

void MainWindow::applyMemorySeries()
{
    std::vector<ChartSeries> series {
        chartSeries(std::string(ps::keys::memUsed), QStringLiteral("Used"), 3, Qt::SolidLine, true)};
    if (m_details) {
        series.push_back(
            chartSeries(std::string(ps::keys::memCache), QStringLiteral("Buff/cache"), 5, Qt::DashLine, false));
        series.push_back(
            chartSeries(std::string(ps::keys::memAvailable), QStringLiteral("Available"), 2, Qt::DotLine, false));
    }
    if (m_memChart->series().size() != series.size())
        m_memChart->setSeries(std::move(series));
}

void MainWindow::applyDetails()
{
    const int state = m_details ? 1 : 0;
    if (state == m_appliedDetails)
        return;
    // Keep the sensor list width while the list is hidden.
    if (m_appliedDetails == 1 && m_tempSplit && !m_sensorList->isHidden()) {
        const QList<int> sizes = m_tempSplit->sizes();
        if (sizes.size() == 2 && sizes[0] > 0 && sizes[1] > 0)
            m_tempSplitSizes = sizes;
    }
    m_appliedDetails = state;
    for (auto* w : m_detailWidgets)
        w->setVisible(m_details);
    // The navigation's third line (load, swap, device, threads...) is a
    // Details reading; its text stays in each entry's tooltip and
    // accessible description.
    for (auto* b : m_nav)
        b->setDetailLineVisible(m_details);
    if (m_details && m_tempSplit && m_tempSplitSizes.size() == 2)
        m_tempSplit->setSizes(m_tempSplitSizes);

    for (auto* chart : {m_cpuChart, m_memChart, m_swapChart, m_diskChart, m_netChart, m_tempChart})
        chart->setSimple(!m_details);
    // Without details the headline above each chart names its traces; the
    // temperature legend stays, as it names the exact sensors plotted.
    for (auto* chart : {m_cpuChart, m_memChart, m_diskChart, m_netChart})
        chart->setLegendVisible(m_details);
    applyCpuSeries();
    applyMemorySeries();
    applySensorSelection();

    for (int c = 0; c < ProcessModel::ColumnCount; ++c) {
        const bool essential =
            c == ProcessModel::Pid || c == ProcessModel::Name || c == ProcessModel::Cpu || c == ProcessModel::Memory;
        m_procTable->setColumnHidden(c, !essential && !m_details);
    }
    // Simple: the name takes the spare width and the numbers stay compact.
    // Details: the command line does, and the name gets back the width it had.
    QHeaderView* procHeader = m_procTable->header();
    if (m_details) {
        if (m_procNameFitted)
            m_procTable->setColumnWidth(ProcessModel::Name, m_procNameWidth);
        m_procNameFitted = false;
        procHeader->setStretchLastSection(true);
    } else {
        if (!m_procNameFitted)
            m_procNameWidth = procHeader->sectionSize(ProcessModel::Name);
        m_procNameFitted = true;
        procHeader->setStretchLastSection(false);
        fitProcessNameColumn();
    }
    if (m_detailsAction && m_detailsAction->isChecked() != m_details) {
        const QSignalBlocker block(m_detailsAction);
        m_detailsAction->setChecked(m_details);
    }
}

bool MainWindow::isAnimating() const
{
    return m_frameTimer && m_frameTimer->isActive();
}

void MainWindow::updateFrameDriver()
{
    if (!m_frameTimer)
        return;
    QWindow* handle = windowHandle();
    if (handle != m_watchedWindow) {
        if (m_watchedWindow)
            m_watchedWindow->removeEventFilter(this);
        m_watchedWindow = handle;
        if (handle)
            handle->installEventFilter(this);
    }
    // Frames are drawn only while someone can see them move: never for a
    // hidden, minimized or covered window, a paused view or a stopped window.
    const bool run = m_smooth && !m_paused && !m_shutdown && m_options.clock && isVisible() && !isMinimized()
        && handle && handle->isExposed();
    if (!run) {
        m_frameTimer->stop();
    } else {
        // The process table has no chart; its sparklines need far fewer frames.
        const int interval = currentPage() == ProcessPage ? 66 : 16;
        if (!m_frameTimer->isActive() || m_frameTimer->interval() != interval)
            m_frameTimer->start(interval);
    }
    for (auto* chart : allCharts())
        chart->setRevealEnabled(run);
    updateProbeVisibility();
}

void MainWindow::advanceFrame()
{
    const double t = now();
    if (!std::isfinite(t))
        return;
    // Only the view clock moves: no sampling, sorting, text or layout here.
    // Charts that are hidden or scrolled out of view just take the time.
    for (auto* chart : {m_cpuChart, m_memChart, m_swapChart, m_diskChart, m_netChart, m_tempChart})
        chart->advanceView(t);
    const QRect coresShown = m_coreGrid->isVisible() ? m_coreGrid->visibleRegion().boundingRect() : QRect();
    for (auto* chart : m_coreGrid->charts()) {
        if (chart->geometry().intersects(coresShown))
            chart->advanceView(t);
        else
            chart->setViewEnd(t);
    }
    for (auto* b : m_nav)
        b->advanceView(t);
}

void MainWindow::applyScaledFonts()
{
    if (!m_stack)
        return;
    const QFont base = font();
    for (auto* w : findChildren<QWidget*>()) {
        const QVariant factor = w->property("psFontScale");
        if (!factor.isValid())
            continue;
        const auto weight = static_cast<QFont::Weight>(w->property("psFontWeight").toInt());
        const QFont f = scaledFont(base, factor.toDouble(), weight);
        if (w->font() != f)
            w->setFont(f);
        const QString widest = w->property("psTemplate").toString();
        if (!widest.isEmpty())
            w->setMinimumWidth(QFontMetrics(f).horizontalAdvance(widest) + 2);
    }
    int navWidth = 228;
    for (const auto* b : m_nav)
        navWidth = std::max(navWidth, b->sizeHint().width());
    if (m_navPanel)
        m_navPanel->setFixedWidth(navWidth);
    m_sensorList->setMinimumWidth(sensorListMinimumWidth(m_sensorList));
    const QFontMetrics fm(base);
    if (m_fsTable) {
        m_fsTable->setColumnWidth(0, fm.horizontalAdvance(QStringLiteral("/home/user/mount")) + 24);
        m_fsTable->setColumnWidth(1, fm.horizontalAdvance(QStringLiteral("/dev/nvme0n1p3")) + 24);
        m_fsTable->setColumnWidth(2, fm.horizontalAdvance(QStringLiteral("btrfs")) + 24);
        for (int c = 3; c <= 5; ++c)
            m_fsTable->setColumnWidth(c, fm.horizontalAdvance(QStringLiteral("1000 GiB")) + 24);
    }
    if (m_procTable) {
        m_procNameMinWidth = 2 * m_procTable->indentation() + fm.horizontalAdvance(QStringLiteral("gnome-shell")) + 28;
        fitProcessNameColumn();
    }
    for (auto* grid : m_factGrids)
        grid->resetWidths();
    for (auto* head : m_head)
        if (head)
            head->reflow();
}

std::vector<TimeChart*> MainWindow::allCharts() const
{
    std::vector<TimeChart*> charts {m_cpuChart, m_memChart, m_swapChart, m_diskChart, m_netChart, m_tempChart};
    if (m_coreGrid)
        for (auto* c : m_coreGrid->charts())
            charts.push_back(c);
    return charts;
}

QLabel* MainWindow::fact(const std::string& id) const
{
    const auto it = m_factMap.find(id);
    return it != m_factMap.end() ? it->second : nullptr;
}

// ---------------------------------------------------------------------------
// Snapshot handling

void MainWindow::applySnapshot(ps::SnapshotPtr snapshot)
{
    if (!snapshot || m_paused)
        return;
    // The collector delivers frames in acquisition order; one acquired no
    // later than the sample already shown (only possible when injected) must
    // not replace it.
    if (ps::isValid(m_lastAcquired) && ps::isValid(snapshot->time) && snapshot->time <= m_lastAcquired)
        return;
    m_latest = snapshot;
    ++m_applied;
    // Freshness follows when the sample was acquired, not when it arrived: a
    // late one is kept as history at its own time but does not make the
    // current values live again. A time from the future is not trusted at
    // all: it is neither recorded nor used as the ordering reference.
    const bool current = isCurrent(*snapshot);
    if (!isFromFuture(*snapshot)) {
        ps::recordSnapshot(m_history, *snapshot);
        if (ps::isValid(snapshot->time))
            m_lastAcquired = snapshot->time;
    }
    m_graceIntervalMs = m_intervalMs;
    setStale(!current);
    updateSelectors(*snapshot);
    updateSummaries();
    updateFacts();
    updateFilesystems(*snapshot);
    if (currentPage() == ProcessPage)
        updateProcesses();
    refreshCharts();
    updateStatus();
    Q_EMIT snapshotApplied();
}

void MainWindow::refreshCharts()
{
    // Time axes end at the current time, so samples show their real age.
    const double end = now();
    const bool reveal = isAnimating();
    for (auto* chart : allCharts()) {
        chart->setViewEnd(end);
        chart->setRevealEnabled(reveal);
        chart->refresh();
    }
    for (auto* b : m_nav) {
        b->setViewEnd(end);
        b->update();
    }
}

void MainWindow::updateSelectors(const ps::Snapshot& s)
{
    std::vector<std::pair<int, bool>> cores;
    for (const auto& c : s.cores)
        cores.emplace_back(c.index, c.online);
    m_coreGrid->setCores(cores, &m_history, m_windowSeconds);
    if (m_paused || m_stale)
        m_coreGrid->setNotice(m_cpuChart->notice());
    updateDiskSelector(s);
    updateNetSelector(s);
    updateSensorList(s);
}

void MainWindow::updateDiskSelector(const ps::Snapshot& s)
{
    // Items: the physical-disk sum, every whole disk, and a previously chosen
    // disk that is currently absent (so the selection is not silently lost).
    std::vector<std::pair<QString, QString>> items; // label, data
    if (s.diskTotal.members > 0)
        items.emplace_back(QStringLiteral("All physical disks (%1)").arg(s.diskTotal.members), QString());
    bool wantedPresent = m_wantedDisk.isEmpty();
    for (const auto& d : s.disks) {
        QString label = q(d.name);
        if (!d.label.empty())
            label += QStringLiteral(" (%1)").arg(q(d.label));
        if (ps::isValid(d.sizeBytes))
            label += QStringLiteral(", ") + q(ps::fmt::bytes(d.sizeBytes));
        if (d.kind != ps::DeviceKind::Physical)
            label += QStringLiteral(", virtual");
        items.emplace_back(label, q(d.name));
        if (q(d.name) == m_wantedDisk)
            wantedPresent = true;
    }
    if (!wantedPresent)
        items.emplace_back(m_wantedDisk + QStringLiteral(" (not present)"), m_wantedDisk);
    if (items.empty())
        items.emplace_back(QStringLiteral("No disks found"), QString());

    bool same = m_diskCombo->count() == static_cast<int>(items.size());
    for (int i = 0; same && i < m_diskCombo->count(); ++i)
        same = m_diskCombo->itemText(i) == items[static_cast<std::size_t>(i)].first
            && m_diskCombo->itemData(i).toString() == items[static_cast<std::size_t>(i)].second;
    if (!same) {
        const QSignalBlocker block(m_diskCombo);
        m_diskCombo->clear();
        for (const auto& [label, key] : items)
            m_diskCombo->addItem(label, key);
    }
    // No aggregate (for example only virtual disks): fall back to the first device.
    int index = m_diskCombo->findData(m_wantedDisk);
    if (index < 0)
        index = 0;
    if (m_diskCombo->currentIndex() != index) {
        const QSignalBlocker block(m_diskCombo);
        m_diskCombo->setCurrentIndex(index);
    }
    applyDiskSelection();
}

void MainWindow::updateNetSelector(const ps::Snapshot& s)
{
    std::vector<std::pair<QString, QString>> items;
    if (s.netTotal.members > 0)
        items.emplace_back(QStringLiteral("All %1 (%2, no loopback)").arg(q(s.netTotal.label).mid(4)).arg(s.netTotal.members),
                           QString());
    bool wantedPresent = m_wantedNet.isEmpty();
    for (const auto& n : s.interfaces) {
        QString label = q(n.name);
        QStringList tags;
        if (n.kind != ps::DeviceKind::Physical)
            tags << kindText(n.kind);
        if (!n.operState.empty())
            tags << q(n.operState);
        if (!tags.isEmpty())
            label += QStringLiteral(" (") + tags.join(QStringLiteral(", ")) + QLatin1Char(')');
        items.emplace_back(label, q(n.name));
        if (q(n.name) == m_wantedNet)
            wantedPresent = true;
    }
    if (!wantedPresent)
        items.emplace_back(m_wantedNet + QStringLiteral(" (not present)"), m_wantedNet);
    if (items.empty())
        items.emplace_back(QStringLiteral("No interfaces found"), QString());

    bool same = m_netCombo->count() == static_cast<int>(items.size());
    for (int i = 0; same && i < m_netCombo->count(); ++i)
        same = m_netCombo->itemText(i) == items[static_cast<std::size_t>(i)].first
            && m_netCombo->itemData(i).toString() == items[static_cast<std::size_t>(i)].second;
    if (!same) {
        const QSignalBlocker block(m_netCombo);
        m_netCombo->clear();
        for (const auto& [label, key] : items)
            m_netCombo->addItem(label, key);
    }
    int index = m_netCombo->findData(m_wantedNet);
    if (index < 0)
        index = 0;
    if (m_netCombo->currentIndex() != index) {
        const QSignalBlocker block(m_netCombo);
        m_netCombo->setCurrentIndex(index);
    }
    applyNetSelection();
}

QString MainWindow::selectedDisk() const
{
    return m_diskCombo->currentData().toString();
}

QString MainWindow::selectedNet() const
{
    return m_netCombo->currentData().toString();
}

void MainWindow::applyDiskSelection()
{
    const QString dev = selectedDisk();
    std::vector<ChartSeries> series;
    if (dev.isEmpty()) {
        series = {chartSeries(std::string(ps::keys::diskAllRead), QStringLiteral("Read"), 2, Qt::SolidLine, true),
                  chartSeries(std::string(ps::keys::diskAllWrite), QStringLiteral("Write"), 4, Qt::DashLine, true)};
        m_diskChart->setTitle(QStringLiteral("Disk throughput: all physical disks"));
    } else {
        series = {chartSeries(ps::keys::diskRead(dev.toStdString()), QStringLiteral("Read"), 2, Qt::SolidLine, true),
                  chartSeries(ps::keys::diskWrite(dev.toStdString()), QStringLiteral("Write"), 4, Qt::DashLine, true)};
        m_diskChart->setTitle(QStringLiteral("Disk throughput: ") + dev);
    }
    if (!dev.isEmpty()) {
        bool present = false;
        if (m_latest)
            for (const auto& d : m_latest->disks)
                present = present || q(d.name) == dev;
        if (!present) {
            m_diskChart->setEmptyText(QStringLiteral("%1 is not present").arg(dev));
        } else if (!m_history.empty() && !m_history.find(series[0].key)) {
            m_diskChart->setTitle(m_diskChart->title() + QStringLiteral(" (not recorded)"));
            m_diskChart->setEmptyText(QStringLiteral("Not recorded: the history limit for disk series is reached"));
        } else {
            m_diskChart->setEmptyText(QStringLiteral("Measuring: throughput needs two samples"));
        }
    } else {
        m_diskChart->setEmptyText(QStringLiteral("Measuring: throughput needs two samples"));
    }
    std::vector<std::string> keys {series[0].key, series[1].key};
    if (m_diskChart->series().size() != 2 || m_diskChart->series()[0].key != series[0].key)
        m_diskChart->setSeries(std::move(series));
    m_nav[DiskPage]->setSparkline(&m_history, keys);
}

void MainWindow::applyNetSelection()
{
    const QString dev = selectedNet();
    std::vector<ChartSeries> series;
    if (dev.isEmpty()) {
        series = {chartSeries(std::string(ps::keys::netAllRx), QStringLiteral("Receive"), 1, Qt::SolidLine, true),
                  chartSeries(std::string(ps::keys::netAllTx), QStringLiteral("Transmit"), 5, Qt::DashLine, true)};
        m_netChart->setTitle(netAggregateTitle(m_latest.get()));
    } else {
        series = {chartSeries(ps::keys::netRx(dev.toStdString()), QStringLiteral("Receive"), 1, Qt::SolidLine, true),
                  chartSeries(ps::keys::netTx(dev.toStdString()), QStringLiteral("Transmit"), 5, Qt::DashLine, true)};
        m_netChart->setTitle(QStringLiteral("Network throughput: ") + dev);
    }
    if (!dev.isEmpty()) {
        bool present = false;
        if (m_latest)
            for (const auto& n : m_latest->interfaces)
                present = present || q(n.name) == dev;
        if (!present) {
            m_netChart->setEmptyText(QStringLiteral("%1 is not present").arg(dev));
        } else if (!m_history.empty() && !m_history.find(series[0].key)) {
            m_netChart->setTitle(m_netChart->title() + QStringLiteral(" (not recorded)"));
            m_netChart->setEmptyText(QStringLiteral("Not recorded: the history limit for interface series is reached"));
        } else {
            m_netChart->setEmptyText(QStringLiteral("Measuring: throughput needs two samples"));
        }
    } else {
        m_netChart->setEmptyText(QStringLiteral("Measuring: throughput needs two samples"));
    }
    std::vector<std::string> keys {series[0].key, series[1].key};
    if (m_netChart->series().size() != 2 || m_netChart->series()[0].key != series[0].key)
        m_netChart->setSeries(std::move(series));
    m_nav[NetworkPage]->setSparkline(&m_history, keys);
}

void MainWindow::updateSensorList(const ps::Snapshot& s)
{
    struct Entry {
        QString key, name, value, valueTip, tip;
    };
    std::vector<Entry> entries;
    QSet<QString> present;
    for (const auto& t : s.temperatures) {
        Entry e;
        e.key = q(t.key);
        e.name = q(t.displayName);
        e.tip = e.name + QLatin1Char('\n') + q(t.path);
        if (!m_history.empty() && !m_history.find(ps::keys::temperature(t.key)))
            e.tip += QStringLiteral("\nNot recorded in history: the limit for sensor series is reached");
        if (m_stale) {
            e.value = kDash;
            e.valueTip = QStringLiteral("No recent samples: ") + e.tip;
        } else if (ps::isValid(t.celsius)) {
            e.value = qCelsius(t.celsius);
            e.valueTip = e.tip;
        } else {
            e.value = QStringLiteral("unavailable");
            e.valueTip = QStringLiteral("No valid reading: ") + e.tip;
        }
        present.insert(e.key);
        entries.push_back(std::move(e));
    }
    // A sensor that disappeared (hot-unplug, driver unload) stays listed,
    // with its check state, while the bounded history still holds its series,
    // so its past values and the gap remain visible instead of vanishing.
    // The history's own retention bounds these placeholders.
    const QString removedPrefix = QStringLiteral("Temperature: ");
    for (int i = 0; i < m_sensorList->topLevelItemCount(); ++i) {
        const QString key = m_sensorList->topLevelItem(i)->data(0, Qt::UserRole).toString();
        if (present.contains(key))
            continue;
        const auto* series = m_history.find(ps::keys::temperature(key.toStdString()));
        if (!series)
            continue;
        Entry e;
        e.key = key;
        e.name = q(series->label);
        if (e.name.startsWith(removedPrefix))
            e.name.remove(0, removedPrefix.size());
        e.value = QStringLiteral("removed");
        e.tip = e.name + QStringLiteral("\nThis sensor is no longer present; its retained history is still shown");
        e.valueTip = e.tip;
        present.insert(key);
        entries.push_back(std::move(e));
    }

    const QSignalBlocker block(m_sensorList);
    bool same = m_sensorList->topLevelItemCount() == static_cast<int>(entries.size());
    for (int i = 0; same && i < m_sensorList->topLevelItemCount(); ++i)
        same = m_sensorList->topLevelItem(i)->data(0, Qt::UserRole).toString()
            == entries[static_cast<std::size_t>(i)].key;
    if (!same) {
        // First run without a saved choice: plot up to six sensors that read,
        // hottest first so the headline sensor is always plotted.
        if (!m_sensorsChosen && m_wantedSensors.isEmpty()) {
            std::vector<const ps::TemperatureReading*> valid;
            valid.reserve(s.temperatures.size());
            for (const auto& t : s.temperatures)
                if (ps::isValid(t.celsius))
                    valid.push_back(&t);
            std::stable_sort(valid.begin(), valid.end(), [](const ps::TemperatureReading* a, const ps::TemperatureReading* b) {
                return a->celsius > b->celsius;
            });
            for (std::size_t i = 0; i < valid.size() && i < 6; ++i)
                m_wantedSensors << q(valid[i]->key);
        }
        m_sensorList->clear();
        for (const auto& e : entries) {
            auto* item = new QTreeWidgetItem(m_sensorList);
            item->setData(0, Qt::UserRole, e.key);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
            item->setCheckState(0, m_wantedSensors.contains(e.key) ? Qt::Checked : Qt::Unchecked);
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    for (int i = 0; i < m_sensorList->topLevelItemCount(); ++i) {
        const auto& e = entries[static_cast<std::size_t>(i)];
        auto* item = m_sensorList->topLevelItem(i);
        if (item->text(0) != e.name)
            item->setText(0, e.name);
        if (item->text(1) != e.value)
            item->setText(1, e.value);
        if (item->toolTip(0) != e.tip)
            item->setToolTip(0, e.tip);
        if (item->toolTip(1) != e.valueTip)
            item->setToolTip(1, e.valueTip);
    }
    // Why nothing reads, in one line; the whole explanation in the tooltip.
    const QString note = q(s.temperatureNote);
    const qsizetype end = note.indexOf(QStringLiteral(". "));
    const QString line = end < 0 ? note : note.left(end + 1);
    if (m_tempNote->text() != line)
        m_tempNote->setText(line);
    if (m_tempNote->toolTip() != note) {
        m_tempNote->setToolTip(note);
        m_tempNote->setAccessibleDescription(note);
    }
    m_tempNote->setVisible(!note.isEmpty());
    applySensorSelection();
}

void MainWindow::applySensorSelection()
{
    // Checked sensors in list order. Each keeps its slot (colour and line
    // style) for as long as it is listed, whatever happens to the others;
    // slots of sensors no longer listed (their history expired) are freed.
    QStringList listed;
    for (int i = 0; i < m_sensorList->topLevelItemCount(); ++i)
        listed << m_sensorList->topLevelItem(i)->data(0, Qt::UserRole).toString();
    std::erase_if(m_sensorSlots,
                  [&](const auto& entry) { return entry.first != m_navSensor && !listed.contains(entry.first); });
    std::vector<ChartSeries> series;
    int omitted = 0;
    for (int i = 0; i < m_sensorList->topLevelItemCount(); ++i) {
        auto* item = m_sensorList->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        const QString sensor = item->data(0, Qt::UserRole).toString();
        std::string key = ps::keys::temperature(sensor.toStdString());
        QString label = item->text(0);
        // Listed but without a series: refused by the history's sensor quota.
        if (!m_history.empty() && !m_history.find(key)) {
            label += QStringLiteral(" (not recorded)");
            ++omitted;
        }
        const int slot = sensorSlot(sensor);
        series.push_back(chartSeries(std::move(key), label, temperatureVisual(slot), seriesStyle(slot), true));
    }
    // Areas help one or two sensors read at a glance; more would only tint
    // each other.
    if (series.size() > 2)
        for (auto& cs : series)
            cs.fill = false;
    bool same = series.size() == m_tempChart->series().size();
    for (std::size_t i = 0; same && i < series.size(); ++i) {
        const auto& old = m_tempChart->series()[i];
        same = series[i].key == old.key && series[i].label == old.label && series[i].visual == old.visual
            && series[i].style == old.style && series[i].fill == old.fill;
    }
    if (!same)
        m_tempChart->setSeries(series);
    QString empty;
    if (m_sensorList->topLevelItemCount() == 0)
        empty = QStringLiteral("No temperature sensors");
    else if (series.empty())
        empty = m_details ? QStringLiteral("Check a sensor to plot it")
                          : QStringLiteral("No sensor is chosen: turn on Details (Ctrl+D) to choose");
    else if (omitted == static_cast<int>(series.size()))
        empty = QStringLiteral("Not recorded: the history limit for sensors is reached");
    else
        empty = QStringLiteral("Waiting for a valid reading");
    m_tempChart->setEmptyText(empty);
}

int MainWindow::sensorSlot(const QString& sensor)
{
    if (const auto it = m_sensorSlots.find(sensor); it != m_sensorSlots.end())
        return it->second;
    // The lowest slot no listed sensor holds, so none ever takes another's.
    // Past eight, colours repeat in other line styles (see seriesStyle()).
    std::set<int> held;
    for (const auto& entry : m_sensorSlots)
        held.insert(entry.second);
    int slot = 0;
    while (held.count(slot))
        ++slot;
    m_sensorSlots[sensor] = slot;
    return slot;
}

void MainWindow::updateHeadlines()
{
    if (!m_head[CpuPage])
        return;
    const ps::Snapshot* s = m_latest.get();
    // Each summary adds only what the values, the picker and the chart do
    // not already say; a warning comes first. Nothing while starting: the
    // charts say they are measuring.
    const auto summary = [this](const QString& text) {
        QStringList parts;
        if (m_paused)
            parts << QStringLiteral("Paused");
        if (!text.isEmpty())
            parts << text;
        return parts.join(QStringLiteral(". "));
    };
    if (!s || m_stale) {
        QString why;
        if (s)
            why = QStringLiteral("No recent samples");
        else if (!m_options.startCollector)
            why = QStringLiteral("Not collecting");
        for (auto* h : m_head) {
            if (!h)
                continue;
            h->setValue(0, kDash);
            h->setValue(1, kDash);
            h->setSummary(summary(why));
        }
        return;
    }
    const bool warming = !s->continuous;
    const QString measuring = QStringLiteral("Measuring\u2026");
    auto measured = [&](double v, const QString& text) { return ps::isValid(v) ? text : (warming ? measuring : kDash); };

    // CPU
    {
        auto* h = m_head[CpuPage];
        h->setValue(0, measured(s->cpu.total, qPercent(s->cpu.total)));
        h->setSummary(summary({}));
    }

    // Memory
    {
        auto* h = m_head[MemoryPage];
        const auto& m = s->memory;
        if (m.valid) {
            h->setValue(0, qBytes(m.used));
            QString text = QStringLiteral("%1 of %2, %3 available")
                               .arg(qPercent(100.0 * m.used / m.total, 0), qBytes(m.total), qBytes(m.available));
            if (m.hasSwap)
                text += QStringLiteral(", swap %1 of %2").arg(qBytes(m.swapUsed), qBytes(m.swapTotal));
            else
                text += ps::isValid(m.swapTotal) ? QStringLiteral(", no swap") : QStringLiteral(", swap unknown");
            h->setSummary(summary(text));
        } else {
            h->setValue(0, kDash);
            h->setSummary(summary(QStringLiteral("/proc/meminfo is unavailable")));
        }
    }

    // Disks: the selected device (named by the picker), and the root
    // filesystem's free space.
    {
        auto* h = m_head[DiskPage];
        const QString dev = selectedDisk();
        double r = ps::kNaN, w = ps::kNaN;
        bool present = true;
        QStringList text;
        if (dev.isEmpty()) {
            r = s->diskTotal.a;
            w = s->diskTotal.b;
        } else {
            const ps::DiskIo* found = nullptr;
            for (const auto& d : s->disks)
                if (q(d.name) == dev)
                    found = &d;
            present = found != nullptr;
            if (found) {
                r = found->readBps;
                w = found->writeBps;
            } else {
                text << QStringLiteral("%1 is not present").arg(dev);
            }
        }
        for (const auto& fs : s->filesystems) {
            const bool root = std::find(fs.mountPoints.begin(), fs.mountPoints.end(), "/") != fs.mountPoints.end();
            if (root && ps::isValid(fs.availBytes) && ps::isValid(fs.totalBytes)) {
                text << QStringLiteral("/ has %1 free of %2").arg(qBytes(fs.availBytes), qBytes(fs.totalBytes));
                break;
            }
        }
        h->setValue(0, present ? measured(r, qRate(r)) : kDash);
        h->setValue(1, present ? measured(w, qRate(w)) : kDash);
        h->setSummary(summary(text.join(QStringLiteral(". "))));
    }

    // Network: the selected interface, named by the picker.
    {
        auto* h = m_head[NetworkPage];
        const QString dev = selectedNet();
        double rx = ps::kNaN, tx = ps::kNaN;
        bool present = true;
        QString text;
        if (dev.isEmpty()) {
            rx = s->netTotal.a;
            tx = s->netTotal.b;
        } else {
            const ps::NetIo* found = nullptr;
            for (const auto& n : s->interfaces)
                if (q(n.name) == dev)
                    found = &n;
            present = found != nullptr;
            if (found) {
                rx = found->rxBps;
                tx = found->txBps;
            } else {
                text = QStringLiteral("%1 is not present").arg(dev);
            }
        }
        h->setValue(0, present ? measured(rx, qRate(rx)) : kDash);
        h->setValue(1, present ? measured(tx, qRate(tx)) : kDash);
        h->setSummary(summary(text));
    }

    // Temperatures: the hottest valid sensor, named exactly.
    {
        auto* h = m_head[TemperaturePage];
        const ps::TemperatureReading* hottest = nullptr;
        for (const auto& t : s->temperatures) {
            if (!ps::isValid(t.celsius))
                continue;
            if (!hottest || t.celsius > hottest->celsius)
                hottest = &t;
        }
        if (hottest) {
            h->setValue(0, qCelsius(hottest->celsius));
            h->setSummary(summary(q(hottest->displayName)));
        } else {
            // The temperature note, when there is one, says why.
            h->setValue(0, kDash);
            h->setSummary(summary(!s->temperatureNote.empty() ? QString()
                                      : s->temperatures.empty() ? QStringLiteral("No temperature sensors found")
                                                                : QStringLiteral("No sensor is reading")));
        }
    }
}

void MainWindow::updateSummaries()
{
    updateHeadlines();
    const ps::Snapshot* s = m_latest.get();
    for (auto* b : m_nav)
        b->setStale(m_stale);
    if (!s || m_stale) {
        for (auto* b : m_nav) {
            b->setValueText(kDash);
            if (!s)
                b->setDetailText(m_options.startCollector ? QStringLiteral("Starting\u2026") : QString());
        }
        return;
    }
    const bool warming = !s->continuous;

    // CPU
    m_nav[CpuPage]->setValueText(ps::isValid(s->cpu.total) ? qPercent(s->cpu.total)
                                                           : (warming ? QStringLiteral("Measuring\u2026") : kDash));
    QString cpuDetail = QStringLiteral("%1 logical CPUs").arg(s->logicalCpus);
    if (ps::isValid(s->load1))
        cpuDetail += QStringLiteral(", load %1").arg(s->load1, 0, 'f', 2);
    m_nav[CpuPage]->setDetailText(cpuDetail);

    // Memory
    const auto& m = s->memory;
    if (m.valid) {
        m_nav[MemoryPage]->setValueText(QStringLiteral("%1 used").arg(qBytes(m.used)));
        QString detail = QStringLiteral("%1 of %2").arg(qPercent(100.0 * m.used / m.total, 0), qBytes(m.total));
        if (m.hasSwap)
            detail += QStringLiteral(", swap %1").arg(qBytes(m.swapUsed));
        else
            detail += ps::isValid(m.swapTotal) ? QStringLiteral(", no swap") : QStringLiteral(", swap unknown");
        m_nav[MemoryPage]->setDetailText(detail);
        m_nav[MemoryPage]->setSparklineMax(m.total);
    } else {
        m_nav[MemoryPage]->setValueText(kDash);
        m_nav[MemoryPage]->setDetailText(QStringLiteral("/proc/meminfo unavailable"));
    }

    // Disks: follows the selected device.
    {
        const QString dev = selectedDisk();
        double r = ps::kNaN, w = ps::kNaN;
        QString what;
        if (dev.isEmpty()) {
            r = s->diskTotal.a;
            w = s->diskTotal.b;
            what = s->diskTotal.members > 0 ? QStringLiteral("All physical disks") : QStringLiteral("No disks");
        } else {
            what = dev;
            for (const auto& d : s->disks)
                if (q(d.name) == dev) {
                    r = d.readBps;
                    w = d.writeBps;
                }
        }
        if (ps::isValid(r) && ps::isValid(w))
            m_nav[DiskPage]->setValueText(QStringLiteral("R %1  W %2").arg(qRate(r), qRate(w)),
                                          QStringLiteral("read %1, write %2").arg(qRate(r), qRate(w)));
        else
            m_nav[DiskPage]->setValueText(warming ? QStringLiteral("Measuring\u2026") : kDash);
        m_nav[DiskPage]->setDetailText(what);
    }

    // Network
    {
        const QString dev = selectedNet();
        double rx = ps::kNaN, tx = ps::kNaN;
        QString what;
        if (dev.isEmpty()) {
            rx = s->netTotal.a;
            tx = s->netTotal.b;
            what = s->netTotal.members > 0 ? capitalized(q(s->netTotal.label))
                                           : QStringLiteral("No interfaces besides loopback");
        } else {
            what = dev;
            for (const auto& n : s->interfaces)
                if (q(n.name) == dev) {
                    rx = n.rxBps;
                    tx = n.txBps;
                }
        }
        if (ps::isValid(rx) && ps::isValid(tx))
            m_nav[NetworkPage]->setValueText(QStringLiteral("\u2193 %1  \u2191 %2").arg(qRate(rx), qRate(tx)),
                                             QStringLiteral("receive %1, transmit %2").arg(qRate(rx), qRate(tx)));
        else
            m_nav[NetworkPage]->setValueText(warming ? QStringLiteral("Measuring\u2026") : kDash);
        m_nav[NetworkPage]->setDetailText(what);
    }

    // Temperatures: hottest valid sensor.
    {
        const ps::TemperatureReading* hottest = nullptr;
        int valid = 0;
        for (const auto& t : s->temperatures) {
            if (!ps::isValid(t.celsius))
                continue;
            ++valid;
            if (!hottest || t.celsius > hottest->celsius)
                hottest = &t;
        }
        // Value, name and sparkline all describe the same sensor: the one
        // that is hottest right now, drawn from its own history in the
        // colour and line style it has on the chart.
        std::vector<std::string> keys;
        if (hottest) {
            keys.push_back(ps::keys::temperature(hottest->key));
            m_navSensor = q(hottest->key);
            const int slot = sensorSlot(m_navSensor);
            m_nav[TemperaturePage]->setSparklineVisuals({{temperatureVisual(slot), seriesStyle(slot)}});
        }
        m_nav[TemperaturePage]->setSparkline(&m_history, std::move(keys));
        if (hottest) {
            m_nav[TemperaturePage]->setValueText(qCelsius(hottest->celsius));
            m_nav[TemperaturePage]->setDetailText(QStringLiteral("Hottest of %1: %2").arg(valid).arg(q(hottest->displayName)));
        } else {
            m_nav[TemperaturePage]->setValueText(QStringLiteral("Unavailable"));
            m_nav[TemperaturePage]->setDetailText(s->temperatures.empty() ? QStringLiteral("No sensors found")
                                                                          : QStringLiteral("No sensor is reading"));
        }
    }

    // Processes
    if (s->processesScanned) {
        m_nav[ProcessPage]->setValueText(QStringLiteral("%1 processes").arg(s->processes.size()));
        const ps::ProcessInfo* top = nullptr;
        for (const auto& p : s->processes)
            if (ps::isValid(p.cpuPercent) && (!top || p.cpuPercent > top->cpuPercent))
                top = &p;
        QString detail = QStringLiteral("%1 threads").arg(s->threadCount);
        if (top)
            detail += QStringLiteral(", top: %1 %2").arg(q(top->name), qPercent(top->cpuPercent));
        m_nav[ProcessPage]->setDetailText(detail);
    } else {
        m_nav[ProcessPage]->setValueText(kDash);
        m_nav[ProcessPage]->setDetailText(QStringLiteral("Process list unavailable"));
    }
}

void MainWindow::updateFacts()
{
    const ps::Snapshot* s = (m_latest && !m_stale) ? m_latest.get() : nullptr;
    auto set = [this](const std::string& id, const QString& text) {
        if (QLabel* l = fact(id); l && l->text() != text)
            l->setText(text);
    };
    if (!s) {
        for (const auto& [id, label] : m_facts)
            if (label->text() != kDash)
                label->setText(kDash);
        return;
    }
    set("cpu.total", qPercent(s->cpu.total));
    set("cpu.user", qPercent(s->cpu.user));
    set("cpu.system", qPercent(s->cpu.system));
    set("cpu.iowait", qPercent(s->cpu.iowait));
    set("cpu.steal", qPercent(s->cpu.steal));
    set("cpu.load", ps::isValid(s->load1) ? QStringLiteral("%1 / %2 / %3")
                                                .arg(s->load1, 0, 'f', 2)
                                                .arg(s->load5, 0, 'f', 2)
                                                .arg(s->load15, 0, 'f', 2)
                                          : kDash);
    const auto online = std::count_if(s->cores.begin(), s->cores.end(), [](const ps::CoreUsage& c) { return c.online; });
    set("cpu.count", QStringLiteral("%1 / %2").arg(online).arg(s->cores.size()));
    set("cpu.tasks", s->processesScanned ? QStringLiteral("%1 / %2").arg(s->processes.size()).arg(s->threadCount)
                                         : kDash);

    const auto& m = s->memory;
    auto withPercent = [&](double v) {
        if (!m.valid || !ps::isValid(v))
            return kDash;
        return QStringLiteral("%1 (%2)").arg(qBytes(v), qPercent(100.0 * v / m.total, 0));
    };
    set("mem.total", m.valid ? qBytes(m.total) : kDash);
    set("mem.used", withPercent(m.used));
    set("mem.available", withPercent(m.available));
    set("mem.cache", withPercent(m.cache));
    set("mem.free", withPercent(m.free));
    set("mem.shared", m.valid ? qBytes(m.shared) : kDash);
    // SwapTotal 0 is a measured "no swap"; a missing or unreadable SwapTotal
    // is unknown and must not read as "none".
    const bool swapKnown = ps::isValid(m.swapTotal);
    const QString noSwap = swapKnown ? QStringLiteral("None configured") : kDash;
    set("swap.total", m.hasSwap ? qBytes(m.swapTotal) : noSwap);
    set("swap.used", m.hasSwap && ps::isValid(m.swapUsed)
                         ? QStringLiteral("%1 (%2)").arg(qBytes(m.swapUsed), qPercent(100.0 * m.swapUsed / m.swapTotal, 0))
                         : (m.hasSwap ? kDash : noSwap));
    if (m_swapChart) {
        QString empty;
        if (m.hasSwap)
            empty = QStringLiteral("Swap use is unavailable: /proc/meminfo has no readable SwapFree");
        else if (swapKnown)
            empty = QStringLiteral("No swap is configured");
        else
            empty = QStringLiteral("Swap size is unavailable: /proc/meminfo has no readable SwapTotal");
        m_swapChart->setEmptyText(empty);
    }
    if (m.valid && m_memChart)
        m_memChart->setFixedRange(0, m.total);
    if (m.hasSwap && ps::isValid(m.swapTotal))
        m_swapChart->setFixedRange(0, m.swapTotal);
    else
        m_swapChart->setAutoRange();

    const QString dev = selectedDisk();
    if (dev.isEmpty()) {
        set("disk.read", qRate(s->diskTotal.a));
        set("disk.write", qRate(s->diskTotal.b));
        set("disk.busy", kDash);
        double total = 0;
        bool any = false;
        for (const auto& d : s->disks)
            if (d.kind == ps::DeviceKind::Physical && ps::isValid(d.sizeBytes)) {
                total += d.sizeBytes;
                any = true;
            }
        set("disk.size", any ? qBytes(total) : kDash);
    } else {
        const ps::DiskIo* found = nullptr;
        for (const auto& d : s->disks)
            if (q(d.name) == dev)
                found = &d;
        set("disk.read", found ? qRate(found->readBps) : kDash);
        set("disk.write", found ? qRate(found->writeBps) : kDash);
        set("disk.busy", found ? qPercent(found->busyPercent, 0) : kDash);
        set("disk.size", found ? qBytes(found->sizeBytes) : QStringLiteral("not present"));
    }

    const QString nif = selectedNet();
    if (nif.isEmpty()) {
        set("net.rx", qRate(s->netTotal.a));
        set("net.tx", qRate(s->netTotal.b));
        set("net.state", kDash);
        set("net.kind", aggregateKind(s->netTotal));
        m_netChart->setTitle(netAggregateTitle(s));
    } else {
        const ps::NetIo* found = nullptr;
        for (const auto& n : s->interfaces)
            if (q(n.name) == nif)
                found = &n;
        set("net.rx", found ? qRate(found->rxBps) : kDash);
        set("net.tx", found ? qRate(found->txBps) : kDash);
        set("net.state", found ? (found->operState.empty() ? kDash : q(found->operState)) : QStringLiteral("not present"));
        set("net.kind", found ? kindText(found->kind) : kDash);
    }
}

void MainWindow::updateFilesystems(const ps::Snapshot& s)
{
    auto sameRow = [&](int i) {
        const auto& fs = s.filesystems[static_cast<std::size_t>(i)];
        const auto* item = m_fsTable->topLevelItem(i);
        return item->data(0, Qt::UserRole).toString() == q(fs.source) + QLatin1Char('\n') + q(fs.mountPoints.front());
    };
    bool same = m_fsTable->topLevelItemCount() == static_cast<int>(s.filesystems.size());
    for (int i = 0; same && i < m_fsTable->topLevelItemCount(); ++i)
        same = !s.filesystems[static_cast<std::size_t>(i)].mountPoints.empty() && sameRow(i);
    if (!same) {
        m_fsTable->clear();
        for (const auto& fs : s.filesystems) {
            auto* item = new QTreeWidgetItem(m_fsTable);
            const QString mount = fs.mountPoints.empty() ? QString() : q(fs.mountPoints.front());
            item->setData(0, Qt::UserRole, q(fs.source) + QLatin1Char('\n') + mount);
            for (int c = 3; c <= 5; ++c)
                item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    for (int i = 0; i < m_fsTable->topLevelItemCount(); ++i) {
        const auto& fs = s.filesystems[static_cast<std::size_t>(i)];
        auto* item = m_fsTable->topLevelItem(i);
        QStringList mounts;
        for (const auto& mp : fs.mountPoints)
            mounts << q(mp);
        const QString mountText = mounts.join(QStringLiteral(", "));
        // Stale: keep the rows (identity) but not numbers that are out of date.
        const bool ok = !m_stale && fs.error.empty() && ps::isValid(fs.totalBytes);
        const double denom = fs.usedBytes + fs.availBytes;
        const double fraction = ok && denom > 0 ? fs.usedBytes / denom : ps::kNaN;
        const QString cols[] = {
            mountText,
            q(fs.source),
            q(fs.fsType) + (fs.readOnly ? QStringLiteral(", read-only") : QString()),
            ok ? qBytes(fs.totalBytes) : kDash,
            ok ? qBytes(fs.usedBytes) : kDash,
            ok ? qBytes(fs.availBytes) : kDash,
            ok ? qPercent(100.0 * fraction, 0) : (m_stale ? QStringLiteral("no recent samples") : q(fs.error)),
        };
        for (int c = 0; c < 7; ++c)
            if (item->text(c) != cols[c])
                item->setText(c, cols[c]);
        item->setData(6, Qt::UserRole, fraction);
        // Mount points and device names are often longer than their columns;
        // the tooltip carries the full identity on every column.
        QString tip = mounts.join(QLatin1Char('\n')) + QStringLiteral("\nDevice: ") + q(fs.source)
            + QStringLiteral("\nType: ") + cols[2];
        if (m_stale)
            tip += QStringLiteral("\nNo recent samples: capacity values are out of date");
        else if (!fs.error.empty())
            tip += QStringLiteral("\nCapacity unavailable: ") + q(fs.error);
        else if (fraction >= kNearlyFull)
            tip += QStringLiteral("\nNearly full: %1 used").arg(cols[6]);
        if (item->toolTip(0) != tip)
            for (int c = 0; c < 7; ++c)
                item->setToolTip(c, tip);
    }
}

void MainWindow::updateProcesses()
{
    updateGrouping(); // judged now, not at the last stale check
    m_procModel->setGrouping(m_grouping);
    if (const ps::Snapshot* s = m_latest.get()) {
        if (s->processesScanned)
            m_procModel->setProcesses(s->processes);
        else
            m_procModel->clear();
    }
    syncGroupExpansion();
    updateProcessCount();
    updateEndTaskActions();
}

void MainWindow::updateProcessCount()
{
    const ps::Snapshot* s = m_latest.get();
    QString text, tip;
    if (!s) {
        text = QStringLiteral("Waiting for the first scan");
    } else if (!s->processesScanned) {
        text = QStringLiteral("Process list unavailable: /proc could not be read");
    } else {
        // Processes only; group rows are not counted.
        const int total = m_procModel->processCount();
        text = m_procProxy->isFiltering()
            ? QStringLiteral("%1 of %2 processes match").arg(m_procProxy->visibleProcessCount()).arg(total)
            : QStringLiteral("%1 processes").arg(total);
        tip = QStringLiteral("%1 threads").arg(s->threadCount);
        if (m_stale) {
            text += QStringLiteral(". No recent samples");
            tip += QStringLiteral(". Values are out of date and shown as unavailable");
        } else if (m_paused) {
            text += QStringLiteral(". Paused");
            tip += QStringLiteral(". Frozen at the last sample");
        }
    }
    if (m_procCount->text() != text)
        m_procCount->setText(text);
    if (m_procCount->toolTip() != tip) {
        m_procCount->setToolTip(tip);
        m_procCount->setAccessibleDescription(tip);
    }
}

bool MainWindow::isEndTaskPending() const
{
    return m_endTaskBox || (m_endTaskRunner && m_endTaskRunner->isBusy());
}

QModelIndex MainWindow::selectedProcessIndex() const
{
    const QItemSelectionModel* selection = m_procTable ? m_procTable->selectionModel() : nullptr;
    if (!selection)
        return {};
    const QModelIndexList rows = selection->selectedRows();
    return rows.size() == 1 ? rows.first() : QModelIndex();
}

std::optional<ps::endtask::Target> MainWindow::processTargetAt(const QModelIndex& index) const
{
    if (!index.isValid() || index.model() != m_procProxy)
        return std::nullopt;
    const ps::ProcessInfo* process = m_procModel->process(m_procProxy->mapToSource(index.siblingAtColumn(0)));
    if (!process)
        return std::nullopt; // a group row
    return ps::endtask::targetOf(*process);
}

// Why the target cannot be ended now; empty when it may be tried.
QString MainWindow::endTaskBlocked(const std::optional<ps::endtask::Target>& target) const
{
    using ps::endtask::Outcome;
    if (!m_endTaskRunner)
        return m_endTaskUnavailable;
    if (m_shutdown)
        return QStringLiteral("bex is closing");
    if (isEndTaskPending())
        return QStringLiteral("An end task request is already open");
    if (!target)
        return QStringLiteral("Select a process to end it");
    switch (ps::endtask::precheck(*target, static_cast<uint32_t>(::getuid()), static_cast<int>(::getpid()))) {
    case Outcome::Sent:
        return {};
    case Outcome::Protected:
        if (target->pid == 1)
            return QStringLiteral("PID 1, the system's init process, is not ended from here");
        if (target->kernelThread)
            return QStringLiteral("Kernel threads cannot be ended");
        return QStringLiteral("bex does not end itself; quit it instead");
    case Outcome::NotOwned:
        return target->uidKnown ? QStringLiteral("Only your own processes can be ended")
                                : QStringLiteral("Its owner is unknown, so it cannot be ended");
    default:
        return QStringLiteral("This row has no usable process identity");
    }
}

void MainWindow::updateEndTaskActions()
{
    if (!m_endTaskAction)
        return;
    const QString blocked = endTaskBlocked(processTargetAt(selectedProcessIndex()));
    const bool enabled = blocked.isEmpty();
    const QString tip = enabled
        ? QStringLiteral("Ask the selected process to quit (Delete). Force end task is in its context menu.")
        : blocked;
    m_endTaskAction->setEnabled(enabled);
    m_endTaskButton->setEnabled(enabled);
    if (m_endTaskButton->toolTip() != tip) {
        m_endTaskButton->setToolTip(tip);
        m_endTaskButton->setAccessibleDescription(tip);
    }
}

// The menu is for the row it was opened on: under the pointer, or the
// selected one from the keyboard. Nothing opens on group rows or empty space.
bool MainWindow::processContextMenu(QObject* watched, QContextMenuEvent* event)
{
    QWidget* viewport = m_procTable->viewport();
    QModelIndex index;
    QPoint at = event->globalPos();
    if (event->reason() == QContextMenuEvent::Keyboard) {
        index = selectedProcessIndex();
        const QRect rect = m_procTable->visualRect(index).intersected(viewport->rect());
        if (!rect.isEmpty())
            at = viewport->mapToGlobal(QPoint(rect.left(), rect.bottom()));
    } else {
        const QPoint pos = watched == viewport ? event->pos() : viewport->mapFrom(m_procTable, event->pos());
        if (viewport->rect().contains(pos))
            index = m_procTable->indexAt(pos);
    }
    event->accept();
    const auto target = processTargetAt(index);
    if (!target)
        return true;

    auto* menu = new QMenu(m_procTable);
    menu->setObjectName(QStringLiteral("processContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setToolTipsVisible(true);
    const QString blocked = endTaskBlocked(target);
    const auto add = [&](const QString& text, const QString& name, ps::endtask::Signal signal) {
        QAction* action = menu->addAction(text);
        action->setObjectName(name);
        action->setEnabled(blocked.isEmpty());
        action->setToolTip(blocked);
        connect(action, &QAction::triggered, this, [this, t = *target, signal] { confirmEndTask(t, signal); });
    };
    add(QStringLiteral("End task\u2026"), QStringLiteral("endTaskMenuAction"), ps::endtask::Signal::Terminate);
    menu->addSeparator();
    add(QStringLiteral("Force end task\u2026"), QStringLiteral("forceEndTaskMenuAction"), ps::endtask::Signal::Kill);
    menu->popup(at);
    return true;
}

void MainWindow::confirmEndTask(const ps::endtask::Target& target, ps::endtask::Signal signal)
{
    if (const QString blocked = endTaskBlocked(target); !blocked.isEmpty()) {
        statusBar()->showMessage(blocked, 5000);
        return;
    }
    const bool force = signal == ps::endtask::Signal::Kill;
    const QString name = q(target.name);
    const QString pid = QString::number(target.pid);
    // Plain text: the name is whatever the process chose to call itself.
    auto* box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("endTaskConfirmBox"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIcon(QMessageBox::Warning);
    box->setTextFormat(Qt::PlainText);
    box->setWindowTitle(force ? QStringLiteral("Force End Task") : QStringLiteral("End Task"));
    box->setText(force ? QStringLiteral("Force end \u201c%1\u201d (PID %2)?").arg(name, pid)
                       : QStringLiteral("End \u201c%1\u201d (PID %2)?").arg(name, pid));
    box->setInformativeText(force ? QStringLiteral("SIGKILL cannot be caught or ignored, so it cannot save or "
                                                   "clean up. Unsaved work in it will be lost.")
                                  : QStringLiteral("It is asked to quit (SIGTERM) and may save or clean up first. "
                                                   "Unsaved work in it may be lost."));
    box->setProperty("psForce", force);
    QPushButton* ok = box->addButton(force ? QStringLiteral("Force end task") : QStringLiteral("End task"),
                                     QMessageBox::AcceptRole);
    ok->setObjectName(QStringLiteral("endTaskConfirmButton"));
    QPushButton* cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    connect(box, &QMessageBox::finished, this, [this, box, ok, target, signal] {
        const bool confirmed = box->clickedButton() == ok;
        m_endTaskBox = nullptr;
        if (confirmed && !m_shutdown && m_endTaskRunner) {
            QString why;
            if (!m_endTaskRunner->start(target, signal, &why)) {
                const QString who = QStringLiteral("\u201c%1\u201d (PID %2)").arg(q(target.name)).arg(target.pid);
                onEndTaskDone(target, signal, {},
                              QStringLiteral("Could not end %1: %2 Nothing was sent.").arg(who, why));
                return;
            }
        }
        updateEndTaskActions();
    });
    m_endTaskBox = box;
    updateEndTaskActions();
    box->open();
}

void MainWindow::onEndTaskDone(const ps::endtask::Target& target, ps::endtask::Signal signal,
                               const ps::endtask::Result& result, const QString& message)
{
    if (m_shutdown)
        return;
    const bool sent = result.outcome == ps::endtask::Outcome::Sent;
    QString shown = message.isEmpty() ? q(ps::endtask::describe(result, target, signal)) : message;
    // The row stays until a new sample no longer lists the process.
    if (sent && m_paused)
        shown += QStringLiteral(" The list is paused.");
    else if (sent && m_stale)
        shown += QStringLiteral(" The list is not updating.");
    statusBar()->showMessage(shown, sent ? 8000 : 15000);
    if (!sent) {
        const bool force = signal == ps::endtask::Signal::Kill;
        auto* box = new QMessageBox(this);
        box->setObjectName(QStringLiteral("endTaskErrorBox"));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setIcon(QMessageBox::Warning);
        box->setTextFormat(Qt::PlainText);
        box->setWindowTitle(force ? QStringLiteral("Could Not Force End Task") : QStringLiteral("Could Not End Task"));
        box->setText(shown);
        box->setStandardButtons(QMessageBox::Ok);
        box->open();
    }
    updateEndTaskActions();
    Q_EMIT endTaskFinished(target.pid, static_cast<qulonglong>(target.startTicks), static_cast<int>(signal),
                           static_cast<int>(result.outcome), shown);
}

void MainWindow::applyWindowOwners(windows::ResultPtr result)
{
    if (!result || m_paused || m_shutdown)
        return;
    if (m_options.clock) {
        // A result that cannot be placed on the clock, or claims to come
        // from the future, cannot be judged for freshness.
        if (!std::isfinite(result->time) || result->time - now() > 1.0)
            return;
    }
    if (ps::isValid(m_ownersNewest) && std::isfinite(result->time) && result->time <= m_ownersNewest)
        return; // no newer than one already applied
    if (std::isfinite(result->time))
        m_ownersNewest = result->time;
    m_ownersAwaiting = false;
    // An incomplete list cannot tell which processes own no window, so it
    // counts as a failure: it neither replaces a complete list nor extends
    // its validity.
    if (result->complete())
        m_ownersOk = std::move(result);
    else
        m_ownersFailure = std::move(result);
    updateGrouping();
    Q_EMIT windowOwnersApplied();
}

bool MainWindow::updateGrouping()
{
    if (m_paused)
        return false; // frozen with the view
    using State = ProcessGrouping::State;
    ProcessGrouping g;
    const double t = now();
    const auto fresh = [&](const windows::Result& r) {
        return !m_options.clock || (std::isfinite(r.time) && t <= r.time + r.validFor);
    };
    // No answer for this long after starting or resuming: stop saying
    // "detecting".
    constexpr double kPendingLimit = 5.0;
    const bool awaiting = m_ownersAwaiting
        && (!m_options.clock || !std::isfinite(m_ownersAwaitingSince) || t - m_ownersAwaitingSince <= kPendingLimit);
    if (m_ownersOk && fresh(*m_ownersOk)) {
        // A failure after a success does not cancel it while it is valid:
        // one missed request should not reshuffle the list.
        g.state = State::Detected;
        g.source = windows::backendName(m_ownersOk->backend);
        g.apps.reserve(m_ownersOk->owners.size());
        for (const auto& o : m_ownersOk->owners)
            g.apps.insert({o.pid, o.startTicks});
    } else if (awaiting) {
        g.state = State::Pending;
    } else if (m_ownersFailure && (!m_ownersOk || !(m_ownersFailure->time <= m_ownersOk->time))) {
        g.state = State::Unavailable;
        g.reason = windows::describe(*m_ownersFailure);
    } else if (m_ownersOk) {
        g.state = State::Unavailable;
        g.reason = QStringLiteral("The window list from %1 is out of date").arg(windows::backendName(m_ownersOk->backend));
    } else {
        g.state = State::Unavailable;
        g.reason = m_probe ? QStringLiteral("The desktop has not answered") : QStringLiteral("App detection is off");
    }
    if (g == m_grouping)
        return false;
    m_grouping = std::move(g);
    if (m_procModel && currentPage() == ProcessPage) {
        m_procModel->setGrouping(m_grouping);
        syncGroupExpansion();
        updateEndTaskActions();
    }
    updateGroupNote();
    if (m_procCount)
        updateProcessCount();
    return true;
}

void MainWindow::updateGroupNote()
{
    if (!m_groupNote)
        return;
    const bool show = m_grouping.state == ProcessGrouping::State::Unavailable;
    if (show) {
        const QString text = QStringLiteral("App detection unavailable");
        const QString why = QStringLiteral("App detection unavailable: %1. User processes are listed together "
                                           "without deciding which are apps; kernel processes are still separate.")
                                .arg(m_grouping.reason.isEmpty() ? QStringLiteral("no window list") : m_grouping.reason);
        if (m_groupNote->text() != text)
            m_groupNote->setText(text);
        if (m_groupNote->toolTip() != why) {
            m_groupNote->setToolTip(why);
            m_groupNote->setAccessibleDescription(why);
        }
    }
    if (m_groupNote->isVisibleTo(m_groupNote->parentWidget()) != show)
        m_groupNote->setVisible(show);
}

void MainWindow::syncGroupExpansion()
{
    if (!m_procTable || !m_procProxy)
        return;
    const auto& want = m_procProxy->isFiltering() ? m_filterExpanded : m_groupExpanded;
    m_syncingExpansion = true;
    for (int g = 0; g < ProcessModel::GroupCount; ++g) {
        const QModelIndex index = m_procProxy->groupIndex(static_cast<ProcessModel::Group>(g));
        const bool expand = want[static_cast<std::size_t>(g)];
        if (index.isValid() && m_procTable->isExpanded(index) != expand)
            m_procTable->setExpanded(index, expand);
    }
    m_syncingExpansion = false;
}

void MainWindow::onGroupExpansion(const QModelIndex& index, bool expanded)
{
    if (m_syncingExpansion || !index.isValid() || index.parent().isValid()
        || !index.data(ProcessModel::IsGroupRole).toBool())
        return;
    const int g = index.data(ProcessModel::GroupRole).toInt();
    if (g < 0 || g >= ProcessModel::GroupCount)
        return;
    // While filtering, opening or closing a group only lasts until the
    // filter is cleared.
    if (m_procProxy->isFiltering()) {
        m_filterExpanded[static_cast<std::size_t>(g)] = expanded;
    } else {
        m_groupExpanded[static_cast<std::size_t>(g)] = expanded;
        saveGroupExpansion();
    }
}

void MainWindow::saveGroupExpansion()
{
    m_settings->setValue(QStringLiteral("processView/expandApps"), m_groupExpanded[ProcessModel::Apps]);
    m_settings->setValue(QStringLiteral("processView/expandBackground"), m_groupExpanded[ProcessModel::Background]);
    m_settings->setValue(QStringLiteral("processView/expandKernel"), m_groupExpanded[ProcessModel::Kernel]);
}

void MainWindow::fitProcessNameColumn()
{
    if (!m_procTable || !m_procNameFitted)
        return;
    const QHeaderView* header = m_procTable->header();
    int others = 0;
    for (int c = 0; c < ProcessModel::ColumnCount; ++c)
        if (c != ProcessModel::Name && !m_procTable->isColumnHidden(c))
            others += header->sectionSize(c);
    const int width = std::max(m_procNameMinWidth, m_procTable->viewport()->width() - others);
    if (header->sectionSize(ProcessModel::Name) != width)
        m_procTable->setColumnWidth(ProcessModel::Name, width);
}

bool MainWindow::processListShown() const
{
    const QWindow* handle = windowHandle();
    return currentPage() == ProcessPage && isVisible() && !isMinimized() && handle && handle->isExposed();
}

void MainWindow::updateProbeVisibility()
{
    if (m_probe)
        m_probe->setVisible(processListShown());
}

// ---------------------------------------------------------------------------
// Status and staleness

double MainWindow::now() const
{
    return m_options.clock ? m_options.clock() : ps::kNaN;
}

double MainWindow::staleLimitSeconds() const
{
    // Sample-to-sample jitter is small; no sample acquired within 2.5
    // intervals (at least one second extra) means the collector is blocked or
    // starved.
    const double ms = std::max(m_intervalMs, m_graceIntervalMs);
    return std::max(ms * 2.5, ms + 1000.0) / 1000.0;
}

bool MainWindow::isCurrent(const ps::Snapshot& s) const
{
    if (!m_options.clock)
        return true;
    const double age = now() - s.time;
    return ps::isValid(age) && !isFromFuture(s) && age <= staleLimitSeconds();
}

bool MainWindow::isFromFuture(const ps::Snapshot& s) const
{
    // Beyond scheduling noise; the collector's boot-time clock never does this.
    return m_options.clock && s.time - now() > 1.0;
}

void MainWindow::checkStale()
{
    if (m_paused || !m_options.clock)
        return;
    const double t = now();
    const bool overdue = ps::isValid(m_lastAcquired) ? t - m_lastAcquired > staleLimitSeconds()
                                                     : t - m_waitingSince > staleLimitSeconds() + 2.0;
    // Only a new, current snapshot (applySnapshot) clears staleness.
    if (overdue && !m_stale)
        setStale(true);
    updateGrouping(); // a window list expires like a sample
    if (m_stale) {
        refreshCharts(); // the time axes keep moving, so the outage shows as a growing gap
        updateStatus();
    }
}

void MainWindow::setStale(bool stale)
{
    if (stale == m_stale)
        return;
    m_stale = stale;
    updateChartNotices();
    updateSummaries();
    updateFacts();
    // Tables keep their rows (identity and selection) but stop showing
    // numbers that are no longer current.
    m_procModel->setStale(stale);
    if (m_latest) {
        updateFilesystems(*m_latest);
        updateSensorList(*m_latest);
    }
    updateProcessCount();
    updateStatus();
}

void MainWindow::updateChartNotices()
{
    QString notice;
    if (m_paused)
        notice = m_stale ? QStringLiteral("Paused, no recent samples") : QStringLiteral("Paused");
    else if (m_stale)
        notice = QStringLiteral("No recent samples");
    for (auto* chart : allCharts())
        chart->setNotice(notice);
}

QString MainWindow::statusText() const
{
    return m_statusLabel ? m_statusLabel->text() : QString();
}

QString MainWindow::statusDetail() const
{
    return m_statusLabel ? m_statusLabel->toolTip() : QString();
}

void MainWindow::updateStatus()
{
    // A few words for what is wrong; why, how old and what happens next go
    // to the tooltip and the accessible description.
    QStringList text;
    QStringList detail;
    if (m_paused) {
        text << QStringLiteral("Paused (press %1 to resume)")
                    .arg(m_pauseAction->shortcut().toString(QKeySequence::NativeText));
        detail << QStringLiteral("The view is frozen. Resuming starts new rate baselines and leaves a gap.");
        if (m_stale)
            detail << QStringLiteral("Samples had already stopped arriving before the pause, so current values stay "
                                     "unavailable.");
    } else if (m_collector && !m_collector->startError().isEmpty()) {
        const QString error = m_collector->startError();
        const qsizetype colon = error.indexOf(QStringLiteral(": "));
        text << (colon > 0 ? error.left(colon) : error) + QStringLiteral("; retrying");
        detail << error + QStringLiteral(". Nothing is being collected; retrying automatically.");
    } else if (m_stale) {
        text << QStringLiteral("Readings unavailable");
        const double age = now() - m_lastAcquired;
        const double waited = now() - m_waitingSince;
        const bool futureShown = m_latest && isFromFuture(*m_latest);
        if (futureShown)
            detail << QStringLiteral("The newest sample has an inconsistent time. Values are shown as unavailable "
                                     "until a current sample arrives.");
        else if (ps::isValid(m_lastAcquired) && ps::isValid(age) && age >= 0)
            detail << QStringLiteral("The newest sample is %1 s old: the collector is blocked or starved. Values are "
                                     "shown as unavailable until it recovers.")
                          .arg(age, 0, 'f', 0);
        else if (ps::isValid(m_lastAcquired))
            detail << QStringLiteral("The newest sample has an inconsistent time. Values are shown as unavailable "
                                     "until a current sample arrives.");
        else
            detail << QStringLiteral("Waiting %1 s for a sample: the collector is blocked or starved. Values are "
                                     "shown as unavailable until it recovers.")
                          .arg(ps::isValid(waited) ? waited : 0.0, 0, 'f', 0);
    } else if (!m_latest && !m_options.startCollector) {
        text << QStringLiteral("Not collecting");
    }
    if (m_latest && !m_paused && !m_latest->problems.empty()) {
        QStringList problems;
        for (const auto& p : m_latest->problems)
            problems << q(p);
        if (text.isEmpty())
            text << QStringLiteral("Some readings unavailable");
        detail << QStringLiteral("Unavailable: ") + problems.join(QStringLiteral("; ")) + QLatin1Char('.');
    }
    if (m_latest && !m_paused) {
        // Each category has a fixed share of the bounded history; say so when a
        // very large system exceeds it instead of silently dropping sources.
        const std::pair<ps::SeriesGroup, const char*> groups[] = {
            {ps::SeriesGroup::Core, "CPU cores"},       {ps::SeriesGroup::Disk, "disk series"},
            {ps::SeriesGroup::Network, "interface series"}, {ps::SeriesGroup::Temperature, "sensors"},
            {ps::SeriesGroup::Summary, "summary series"}};
        QStringList limited;
        for (const auto& [group, name] : groups)
            if (const std::size_t n = m_history.refusedInLastSample(group))
                limited << QStringLiteral("%1 %2").arg(n).arg(QLatin1String(name));
        if (!limited.isEmpty()) {
            text << QStringLiteral("History limit reached");
            detail << QStringLiteral("History limit reached, not recorded: ") + limited.join(QStringLiteral(", "))
                    + QLatin1Char('.');
        }
    }
    const QString shown = text.join(QStringLiteral(". "));
    const QString tip = detail.join(QLatin1Char('\n'));
    if (m_statusLabel->text() != shown)
        m_statusLabel->setText(shown);
    // The name replaces the label's text for assistive technology, so it
    // carries the cue itself.
    const QString name = shown.isEmpty() ? QStringLiteral("Status") : QStringLiteral("Status: ") + shown;
    if (m_statusLabel->accessibleName() != name)
        m_statusLabel->setAccessibleName(name);
    if (m_statusLabel->toolTip() != tip) {
        m_statusLabel->setToolTip(tip);
        m_statusLabel->setAccessibleDescription(tip);
    }
    updateStatusBarVisibility();
}

void MainWindow::updateStatusBarVisibility()
{
    // A message (export progress or result) shows the bar too, and it hides
    // again when both are gone. Always an explicit show or hide: a bar left
    // implicitly hidden would appear with the window.
    QStatusBar* bar = statusBar();
    bar->setVisible(!m_statusLabel->text().isEmpty() || !bar->currentMessage().isEmpty());
}

// ---------------------------------------------------------------------------
// Export and About

bool MainWindow::startCsvExport(const QString& path, QString* error)
{
    auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (m_history.empty())
        return fail(QStringLiteral("No samples have been collected yet."));
    if (m_exporter->isBusy())
        return fail(QStringLiteral("An export to %1 is still running.")
                        .arg(QDir::toNativeSeparators(m_exporter->currentPath())));
    // History times are CLOCK_BOOTTIME; convert with the current offset. The
    // worker gets a private copy, so later samples never race with it.
    const double offset = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0 - ps::monotonicSeconds();
    QString why;
    bool started = false;
    try {
        started = m_exporter->start(m_history, offset, path, &why); // copying the history may run out of memory
    } catch (const std::bad_alloc&) {
        why = QStringLiteral("Not enough memory to copy the history for export.");
    }
    if (!started)
        return fail(why);
    m_exportAction->setEnabled(false);
    m_exportAction->setText(QStringLiteral("Exporting\u2026"));
    statusBar()->showMessage(QStringLiteral("Exporting %1 samples to %2\u2026")
                                 .arg(m_history.size())
                                 .arg(QDir::toNativeSeparators(path)));
    return true;
}

void MainWindow::onCsvExportFinished(bool ok, const QString& path, const QString& error, qulonglong samples)
{
    m_exportAction->setEnabled(true);
    m_exportAction->setText(QStringLiteral("Export History as CSV\u2026"));
    if (ok) {
        statusBar()->showMessage(
            QStringLiteral("Exported %1 samples to %2").arg(samples).arg(QDir::toNativeSeparators(path)), 8000);
    } else {
        const QString message = QStringLiteral("Could not save %1: %2").arg(QDir::toNativeSeparators(path), error);
        statusBar()->showMessage(message, 15000);
        // Non-blocking: the window stays live while the message is shown.
        auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Export failed"), message, QMessageBox::Ok,
                                    this);
        box->setObjectName(QStringLiteral("exportErrorBox"));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
    }
    Q_EMIT csvExportFinished(ok, path, error);
}

void MainWindow::exportCsvInteractive()
{
    QString error;
    if (m_history.empty() || m_exporter->isBusy()) {
        startCsvExport(QString(), &error);
        statusBar()->showMessage(error, 5000);
        return;
    }
    // Only strings here: probing the folder could hang on a stalled network
    // or FUSE mount before the chooser even opens. The chooser copes with a
    // missing folder.
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();
    const QString name = QStringLiteral("bex-%1.csv")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export History as CSV"),
                                                      QDir(dir).filePath(name),
                                                      QStringLiteral("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty() || m_shutdown)
        return;
    if (!startCsvExport(path, &error))
        statusBar()->showMessage(error, 8000);
}

void MainWindow::showAbout()
{
    if (m_aboutBox) {
        m_aboutBox->raise();
        m_aboutBox->activateWindow();
        return;
    }
    auto* box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("aboutBox"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowTitle(QStringLiteral("About bex"));
    box->setIconPixmap(bexIcon().pixmap(64, 64));
    box->setTextFormat(Qt::RichText);
    box->setText(
        QStringLiteral("<p><b>bex %1</b></p>"
                       "<p>A native Linux performance monitor. It reads /proc and /sys locally; it needs no "
                       "privileges and sends nothing over the network. It changes nothing on the system, except "
                       "that End task signals one of your own processes after you confirm.</p>"
                       "<p>History is kept in memory for 15 minutes and discarded on exit unless exported.</p>"
                       "<p>Qt style: %2%3. %4</p>")
            .arg(QStringLiteral(BEX_VERSION),
                 (m_options.styleName.isEmpty() ? style()->name() : m_options.styleName).toHtmlEscaped(),
                 qApp->styleSheet().isEmpty() ? QString() : QStringLiteral(", with a style sheet"),
                 m_options.themeNote.toHtmlEscaped()));
    box->setStandardButtons(QMessageBox::Ok);
    m_aboutBox = box;
    box->open();
}
