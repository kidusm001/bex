#include "app/mainwindow.h"

#include "app/appicon.h"
#include "app/metricbutton.h"
#include "app/processmodel.h"
#include "app/timechart.h"
#include "core/format.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QResizeEvent>
#include <QSet>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <new>
#include <set>

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
    return ms % 1000 == 0 ? QStringLiteral("%1 s").arg(ms / 1000) : QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 1);
}

QFont scaledFont(const QFont& base, double factor, QFont::Weight weight)
{
    QFont f = base;
    if (f.pointSizeF() > 0)
        f.setPointSizeF(f.pointSizeF() * factor);
    f.setWeight(weight);
    return f;
}

QLabel* makeHeading(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setFont(scaledFont(parent->font(), 1.3, QFont::DemiBold));
    l->setAccessibleName(text);
    return l;
}

QLabel* makeSubheading(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setFont(scaledFont(parent->font(), 1.0, QFont::DemiBold));
    return l;
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
    QSize sizeHint() const override { return {600, 2 * kCellH}; }

protected:
    void resizeEvent(QResizeEvent*) override
    {
        m_columns = 0;
        relayout();
    }

private:
    static constexpr int kCellW = 190;
    static constexpr int kCellH = 86;

    void relayout()
    {
        const int n = static_cast<int>(m_charts.size());
        const int cols = std::clamp(width() / kCellW, 1, std::max(1, std::min(n, 8)));
        if (cols == m_columns)
            return;
        m_columns = cols;
        const int rows = (n + cols - 1) / cols;
        setMinimumHeight(rows * kCellH);
        const int cellW = width() / cols;
        for (int i = 0; i < n; ++i)
            m_charts[static_cast<std::size_t>(i)]->setGeometry((i % cols) * cellW, (i / cols) * kCellH, cellW - 4,
                                                               kCellH - 4);
    }

    std::vector<int> m_indices;
    std::vector<TimeChart*> m_charts;
    int m_columns = 0;
};

namespace {

// Draws "used" as a proportional bar with the percentage beside it, so the
// fill level is readable without relying on the bar colour.
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
        const int textW = fm.horizontalAdvance(QStringLiteral("100%")) + 6;
        const QRect r = opt.rect.adjusted(4, 0, -4, 0);
        const QRect textRect(r.right() - textW, r.top(), textW, r.height());
        p->save();
        p->setPen(opt.palette.color(opt.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
        p->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, text);
        if (std::isfinite(fraction)) {
            const int barH = std::max(6, fm.height() / 2);
            const QRectF track(r.left(), r.center().y() - barH / 2.0 + 0.5, std::max(0, r.width() - textW - 6), barH);
            QColor trackColor = opt.palette.color(QPalette::Text);
            trackColor.setAlphaF(0.12f);
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setPen(Qt::NoPen);
            p->setBrush(trackColor);
            p->drawRoundedRect(track, 2, 2);
            const bool nearlyFull = fraction >= 0.9;
            QColor fill = nearlyFull ? seriesColor(5, opt.palette) : opt.palette.color(QPalette::Highlight);
            p->setBrush(fill);
            QRectF bar = track;
            bar.setWidth(track.width() * std::clamp(fraction, 0.0, 1.0));
            p->drawRoundedRect(bar, 2, 2);
        }
        p->restore();
    }
};

// "Name: value" pairs in up to `columns` pairs per row. Narrow windows (a
// tiled half screen, a large system font) get fewer pairs per row instead of
// clipped numbers: the minimum width is a single pair.
class FactGrid : public QWidget {
public:
    FactGrid(int columns, QWidget* parent)
        : QWidget(parent)
        , m_maxColumns(columns)
        , m_grid(new Layout(this))
    {
        m_grid->setContentsMargins(0, 0, 0, 0);
        m_grid->setHorizontalSpacing(10);
        // The widget's minimum is one pair, not the current arrangement.
        m_grid->setSizeConstraint(QLayout::SetNoConstraint);
        m_grid->setColumnStretch(2 * m_maxColumns, 1);
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
        return w + m_grid->horizontalSpacing() * (2 * columns - 1);
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
        return rows * rowH + (rows - 1) * std::max(0, m_grid->verticalSpacing());
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
        for (std::size_t i = 0; i < m_pairs.size(); ++i) {
            const int row = static_cast<int>(i) / columns;
            const int col = static_cast<int>(i) % columns;
            m_grid->addWidget(m_pairs[i].name, row, col * 2, Qt::AlignLeft | Qt::AlignVCenter);
            m_grid->addWidget(m_pairs[i].value, row, col * 2 + 1, Qt::AlignLeft | Qt::AlignVCenter);
        }
        updateGeometry();
    }

    int m_maxColumns;
    int m_columns = 0;
    QGridLayout* m_grid;
    std::vector<Pair> m_pairs;
};

void addFactPair(FactGrid* grid, const QString& name, QLabel* value, const QString& tip)
{
    auto* label = new QLabel(name + QLatin1Char(':'), grid);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setBuddy(value);
    value->setParent(grid);
    grid->addPair(label, value);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value->setAccessibleName(name);
    if (!tip.isEmpty()) {
        label->setToolTip(tip);
        value->setToolTip(tip);
    }
}

} // namespace

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
    setWindowTitle(QStringLiteral("PulseScope"));
    setWindowIcon(pulseScopeIcon());
    setObjectName(QStringLiteral("mainWindow"));
    if (m_options.settingsFile.isEmpty())
        m_settings = std::make_unique<QSettings>();
    else
        m_settings = std::make_unique<QSettings>(m_options.settingsFile, QSettings::IniFormat);
    loadSettings();

    buildActions();
    buildToolbar();
    buildMenus();

    auto* central = new QWidget(this);
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(8, 6, 8, 4);
    layout->setSpacing(10);
    layout->addWidget(buildNavigation());
    m_stack = new QStackedWidget(central);
    m_stack->addWidget(buildCpuPage());
    m_stack->addWidget(buildMemoryPage());
    m_stack->addWidget(buildDiskPage());
    m_stack->addWidget(buildNetworkPage());
    m_stack->addWidget(buildTemperaturePage());
    m_stack->addWidget(buildProcessPage());
    layout->addWidget(m_stack, 1);
    setCentralWidget(central);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("statusLabel"));
    m_statusLabel->setAccessibleName(QStringLiteral("Status"));
    m_timingLabel = new QLabel(this);
    m_timingLabel->setObjectName(QStringLiteral("timingLabel"));
    m_timingLabel->setToolTip(QStringLiteral("Time the collector thread spent reading /proc and /sys for the "
                                             "latest sample"));
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_timingLabel);

    for (auto* chart : allCharts())
        chart->setWindowSeconds(m_windowSeconds);

    const int page = std::clamp(m_settings->value(QStringLiteral("page"), 0).toInt(), 0, PageCount - 1);
    showPage(static_cast<Page>(page));

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
        m_collector->start(m_intervalMs);
    }
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
    if (m_staleTimer)
        m_staleTimer->stop();
    bool clean = true;
    if (m_collector)
        clean = m_collector->stop(timeoutMs);
    if (m_exporter && m_exporter->isBusy()) {
        // A local export normally finishes in milliseconds; one stuck on a
        // stalled mount is abandoned like the capacity probe (atomic save:
        // the destination is either complete or untouched).
        const int remaining = static_cast<int>(std::max<qint64>(0, timeoutMs - clock.elapsed()));
        clean = m_exporter->shutdown(std::min(remaining, 1000)) && clean;
    }
    return clean;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();
    shutdown();
    event->accept();
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
    m_settings->setValue(QStringLiteral("showKernelThreads"), m_kernelCheck->isChecked());
    m_settings->setValue(QStringLiteral("processSortColumn"), m_procProxy->sortColumn());
    m_settings->setValue(QStringLiteral("processSortOrder"), static_cast<int>(m_procProxy->sortOrder()));
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

    m_exportAction = new QAction(QStringLiteral("Export History as CSV\u2026"), this);
    m_exportAction->setObjectName(QStringLiteral("exportAction"));
    m_exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    m_exportAction->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_exportAction->setToolTip(QStringLiteral("Save the retained history (up to 15 minutes) as CSV (Ctrl+E)"));
    connect(m_exportAction, &QAction::triggered, this, &MainWindow::exportCsvInteractive);

    m_quitAction = new QAction(QStringLiteral("Quit"), this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    m_quitAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_quitAction, &QAction::triggered, this, &QWidget::close);

    m_aboutAction = new QAction(QStringLiteral("About PulseScope"), this);
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::showAbout);

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
    tb->addAction(m_pauseAction);
    tb->addSeparator();

    auto* intervalLabel = new QLabel(QStringLiteral(" Sample every "), tb);
    m_intervalCombo = new QComboBox(tb);
    m_intervalCombo->setObjectName(QStringLiteral("intervalCombo"));
    m_intervalCombo->setAccessibleName(QStringLiteral("Sampling interval"));
    m_intervalCombo->setToolTip(QStringLiteral("How often the system is sampled"));
    for (int ms : intervalChoicesMs())
        m_intervalCombo->addItem(intervalText(ms), ms);
    m_intervalCombo->setCurrentIndex(m_intervalCombo->findData(m_intervalMs));
    intervalLabel->setBuddy(m_intervalCombo);
    connect(m_intervalCombo, &QComboBox::activated, this,
            [this](int index) { setIntervalMs(m_intervalCombo->itemData(index).toInt()); });
    tb->addWidget(intervalLabel);
    tb->addWidget(m_intervalCombo);

    auto* rangeLabel = new QLabel(QStringLiteral("  Show last "), tb);
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
    tb->addAction(m_exportAction);
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(m_exportAction);
    file->addSeparator();
    file->addAction(m_quitAction);
    auto* view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(m_pauseAction);
    view->addSeparator();
    for (auto* a : m_pageActions)
        view->addAction(a);
    auto* help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(m_aboutAction);
}

QWidget* MainWindow::buildNavigation()
{
    auto* nav = new QWidget(this);
    nav->setObjectName(QStringLiteral("navigation"));
    nav->setAccessibleName(QStringLiteral("Metric categories"));
    auto* v = new QVBoxLayout(nav);
    v->setContentsMargins(0, 4, 0, 0);
    v->setSpacing(2);
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
    // Widest values each entry is expected to show; see MetricButton.
    m_nav[CpuPage]->setValueTemplate(QStringLiteral("100.0 %"));
    m_nav[MemoryPage]->setValueTemplate(QStringLiteral("0000 MiB used"));
    m_nav[DiskPage]->setValueTemplate(QStringLiteral("R 0000 MiB/s  W 0000 MiB/s"));
    m_nav[NetworkPage]->setValueTemplate(QStringLiteral("\u2193 0000 MiB/s  \u2191 0000 MiB/s"));
    m_nav[TemperaturePage]->setValueTemplate(QStringLiteral("100.0 \u00B0C"));
    m_nav[ProcessPage]->setValueTemplate(QStringLiteral("00000 processes"));
    v->addStretch(1);
    int navWidth = 210;
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
    v->setContentsMargins(4, 4, 4, 4);
    v->addWidget(makeHeading(QStringLiteral("CPU"), page));

    auto* grid = new FactGrid(4, page);
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
    }
    v->addWidget(grid);

    m_cpuChart = new TimeChart(QStringLiteral("CPU utilization"), ps::Unit::Percent, page);
    m_cpuChart->setObjectName(QStringLiteral("cpuChart"));
    m_cpuChart->setFixedRange(0, 100);
    m_cpuChart->setHistory(&m_history);
    m_cpuChart->setSeries({{std::string(ps::keys::cpuTotal), QStringLiteral("Total"), 0},
                           {std::string(ps::keys::cpuUser), QStringLiteral("User"), 1},
                           {std::string(ps::keys::cpuSystem), QStringLiteral("System"), 2},
                           {std::string(ps::keys::cpuIowait), QStringLiteral("I/O wait"), 3}});
    m_cpuChart->setEmptyText(QStringLiteral("Measuring: CPU use needs two samples"));
    v->addWidget(m_cpuChart, 3);

    auto* coreHeader = new QHBoxLayout;
    coreHeader->addWidget(makeSubheading(QStringLiteral("Per logical CPU"), page));
    m_coreNote = makeNote(QStringLiteral("Each chart is 0\u2013100% of one logical CPU."), page);
    coreHeader->addWidget(m_coreNote, 1);
    v->addLayout(coreHeader);

    auto* scroll = new QScrollArea(page);
    scroll->setObjectName(QStringLiteral("coreScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_coreGrid = new CoreGrid(scroll);
    m_coreGrid->setObjectName(QStringLiteral("coreGrid"));
    scroll->setWidget(m_coreGrid);
    v->addWidget(scroll, 2);
    v->addWidget(makeNote(convention + QStringLiteral(" Read from /proc/stat; guest time is counted once."), page));
    return page;
}

QWidget* MainWindow::buildMemoryPage()
{
    auto* page = new QWidget;
    page->setAccessibleName(QStringLiteral("Memory"));
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);
    v->addWidget(makeHeading(QStringLiteral("Memory"), page));

    auto* grid = new FactGrid(4, page);
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
    }
    v->addWidget(grid);

    m_memChart = new TimeChart(QStringLiteral("Physical memory"), ps::Unit::Bytes, page);
    m_memChart->setObjectName(QStringLiteral("memoryChart"));
    m_memChart->setHistory(&m_history);
    m_memChart->setSeries({{std::string(ps::keys::memUsed), QStringLiteral("Used"), 0},
                           {std::string(ps::keys::memCache), QStringLiteral("Buff/cache"), 1},
                           {std::string(ps::keys::memAvailable), QStringLiteral("Available"), 2}});
    m_memChart->setEmptyText(QStringLiteral("/proc/meminfo is unavailable"));
    v->addWidget(m_memChart, 3);

    m_swapChart = new TimeChart(QStringLiteral("Swap"), ps::Unit::Bytes, page);
    m_swapChart->setObjectName(QStringLiteral("swapChart"));
    m_swapChart->setHistory(&m_history);
    m_swapChart->setSeries({{std::string(ps::keys::swapUsed), QStringLiteral("Swap used"), 3}});
    m_swapChart->setEmptyText(QStringLiteral("No swap is configured"));
    v->addWidget(m_swapChart, 2);

    v->addWidget(makeNote(
        QStringLiteral("Used = total \u2212 available, matching free(1). Buff/cache overlaps available because the kernel "
                       "reclaims most cache when programs need memory, so used + buff/cache + available exceeds total. "
                       "Sizes are IEC units (1 GiB = 1024\u00b3 bytes). Source: /proc/meminfo."),
        page));
    return page;
}

QWidget* MainWindow::buildDiskPage()
{
    auto* page = new QWidget;
    page->setAccessibleName(QStringLiteral("Disks"));
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);
    auto* header = new QHBoxLayout;
    header->addWidget(makeHeading(QStringLiteral("Disks"), page));
    header->addStretch(1);
    auto* devLabel = new QLabel(QStringLiteral("Device:"), page);
    m_diskCombo = new QComboBox(page);
    m_diskCombo->setObjectName(QStringLiteral("diskSelector"));
    m_diskCombo->setAccessibleName(QStringLiteral("Disk device"));
    m_diskCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_diskCombo->setToolTip(QStringLiteral("Whole disks only; partitions are never added to their disk"));
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

    auto* grid = new FactGrid(4, page);
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
    }
    v->addWidget(grid);

    m_diskChart = new TimeChart(QStringLiteral("Disk throughput"), ps::Unit::BytesPerSecond, page);
    m_diskChart->setObjectName(QStringLiteral("diskChart"));
    m_diskChart->setHistory(&m_history);
    m_diskChart->setEmptyText(QStringLiteral("Measuring: throughput needs two samples"));
    v->addWidget(m_diskChart, 3);

    v->addWidget(makeSubheading(QStringLiteral("Local filesystems"), page));
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
    v->addWidget(m_fsTable, 2);

    v->addWidget(makeNote(
        QStringLiteral("Throughput is measured per whole disk (/proc/diskstats), never per partition, so no byte is "
                       "counted twice. The sum covers physical disks; device-mapper, RAID and zram devices re-carry that "
                       "traffic and are listed separately. Capacity comes from statvfs on local filesystems only; "
                       "network, FUSE and automount points are never touched."),
        page));
    return page;
}

QWidget* MainWindow::buildNetworkPage()
{
    auto* page = new QWidget;
    page->setAccessibleName(QStringLiteral("Network"));
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);
    auto* header = new QHBoxLayout;
    header->addWidget(makeHeading(QStringLiteral("Network"), page));
    header->addStretch(1);
    auto* ifLabel = new QLabel(QStringLiteral("Interface:"), page);
    m_netCombo = new QComboBox(page);
    m_netCombo->setObjectName(QStringLiteral("netSelector"));
    m_netCombo->setAccessibleName(QStringLiteral("Network interface"));
    m_netCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
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

    auto* grid = new FactGrid(4, page);
    struct F { const char* id; const char* name; };
    const F fs[] = {{"net.rx", "Receive"}, {"net.tx", "Transmit"}, {"net.state", "State"}, {"net.kind", "Type"}};
    for (const auto& f : fs) {
        auto* value = new QLabel(kDash, page);
        value->setObjectName(QString::fromLatin1(f.id).replace(QLatin1Char('.'), QLatin1Char('_')));
        addFactPair(grid, QString::fromLatin1(f.name), value, {});
        m_facts.emplace_back(f.id, value);
    }
    v->addWidget(grid);

    m_netChart = new TimeChart(QStringLiteral("Network throughput"), ps::Unit::BytesPerSecond, page);
    m_netChart->setObjectName(QStringLiteral("netChart"));
    m_netChart->setHistory(&m_history);
    m_netChart->setEmptyText(QStringLiteral("Measuring: throughput needs two samples"));
    v->addWidget(m_netChart, 1);
    v->addWidget(makeNote(
        QStringLiteral("Bytes per second from /proc/net/dev (1 KiB/s = 1024 B/s). The default sum covers physical "
                       "interfaces and never includes loopback; bridges, VPN tunnels and container links re-carry "
                       "traffic already counted on a physical interface, so they are listed individually."),
        page));
    return page;
}

QWidget* MainWindow::buildTemperaturePage()
{
    auto* page = new QWidget;
    page->setAccessibleName(QStringLiteral("Temperatures"));
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);
    v->addWidget(makeHeading(QStringLiteral("Temperatures"), page));
    m_tempNote = makeNote({}, page);
    m_tempNote->setObjectName(QStringLiteral("temperatureNote"));
    v->addWidget(m_tempNote);

    auto* split = new QSplitter(Qt::Horizontal, page);
    split->setChildrenCollapsible(false);
    m_sensorList = new QTreeWidget(split);
    m_sensorList->setObjectName(QStringLiteral("sensorList"));
    m_sensorList->setAccessibleName(QStringLiteral("Temperature sensors"));
    m_sensorList->setRootIsDecorated(false);
    m_sensorList->setUniformRowHeights(true);
    // Sensor names often share a long prefix ("coretemp Core 0", "coretemp
    // Core 1"); eliding in the middle keeps the distinguishing end visible.
    m_sensorList->setTextElideMode(Qt::ElideMiddle);
    m_sensorList->setHeaderLabels({QStringLiteral("Sensor (check to plot)"), QStringLiteral("Now")});
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
    split->setSizes({300, 700});
    v->addWidget(split, 1);
    v->addWidget(makeNote(QStringLiteral("Degrees Celsius from /sys/class/hwmon and /sys/class/thermal. Sensors "
                                         "that report a fault or an impossible value are shown as unavailable."),
                          page));
    return page;
}

QWidget* MainWindow::buildProcessPage()
{
    auto* page = new QWidget;
    page->setAccessibleName(QStringLiteral("Processes"));
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);
    auto* header = new QHBoxLayout;
    header->addWidget(makeHeading(QStringLiteral("Processes"), page));
    header->addSpacing(12);
    m_procFilter = new QLineEdit(page);
    m_procFilter->setObjectName(QStringLiteral("processFilter"));
    m_procFilter->setPlaceholderText(QStringLiteral("Filter by name, user, command or PID (Ctrl+F)"));
    m_procFilter->setAccessibleName(QStringLiteral("Process filter"));
    m_procFilter->setClearButtonEnabled(true);
    m_procFilter->setMinimumWidth(200);
    header->addWidget(m_procFilter, 1);
    m_kernelCheck = new QCheckBox(QStringLiteral("Show kernel threads"), page);
    m_kernelCheck->setObjectName(QStringLiteral("kernelThreadsCheck"));
    m_kernelCheck->setChecked(m_settings->value(QStringLiteral("showKernelThreads"), false).toBool());
    header->addWidget(m_kernelCheck);
    m_procCount = new QLabel(page);
    m_procCount->setObjectName(QStringLiteral("processCount"));
    m_procCount->setForegroundRole(QPalette::PlaceholderText);
    v->addLayout(header);
    v->addWidget(m_procCount);

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
    m_procProxy->setShowKernelThreads(m_kernelCheck->isChecked());
    connect(m_procFilter, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_procProxy->setFilterString(text);
        updateProcessCount();
    });
    connect(m_kernelCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_procProxy->setShowKernelThreads(on);
        updateProcessCount();
    });

    m_procTable = new QTableView(page);
    m_procTable->setObjectName(QStringLiteral("processTable"));
    m_procTable->setAccessibleName(QStringLiteral("Processes"));
    m_procTable->setModel(m_procProxy);
    m_procTable->setSortingEnabled(true);
    m_procTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_procTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_procTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_procTable->setAlternatingRowColors(true);
    m_procTable->setWordWrap(false);
    m_procTable->setTextElideMode(Qt::ElideRight);
    m_procTable->verticalHeader()->hide();
    m_procTable->verticalHeader()->setDefaultSectionSize(QFontMetrics(font()).height() + 6);
    m_procTable->horizontalHeader()->setStretchLastSection(true);
    m_procTable->horizontalHeader()->setSectionsMovable(false);
    const QFontMetrics fm(font());
    const auto width = [&](const char* sample) { return fm.horizontalAdvance(QString::fromLatin1(sample)) + 28; };
    m_procTable->setColumnWidth(ProcessModel::Pid, width("4194304"));
    m_procTable->setColumnWidth(ProcessModel::Name, width("gnome-shell-calen"));
    m_procTable->setColumnWidth(ProcessModel::User, width("username"));
    m_procTable->setColumnWidth(ProcessModel::Cpu, width("1000.0 %"));
    m_procTable->setColumnWidth(ProcessModel::Memory, width("Memory (RSS)"));
    m_procTable->setColumnWidth(ProcessModel::Threads, width("Threads"));
    m_procTable->setColumnWidth(ProcessModel::State, width("Disk wait"));
    m_procTable->setColumnWidth(ProcessModel::Nice, width("Nice"));
    m_procTable->setColumnWidth(ProcessModel::IoRead, width("1000 KiB/s"));
    m_procTable->setColumnWidth(ProcessModel::IoWrite, width("1000 KiB/s"));
    m_procTable->setColumnWidth(ProcessModel::Age, width("12m 05s"));
    const int sortColumn =
        std::clamp(m_settings->value(QStringLiteral("processSortColumn"), int(ProcessModel::Cpu)).toInt(), 0,
                   ProcessModel::ColumnCount - 1);
    const auto sortOrder = m_settings->value(QStringLiteral("processSortOrder"), int(Qt::DescendingOrder)).toInt() == 0
        ? Qt::AscendingOrder
        : Qt::DescendingOrder;
    m_procTable->sortByColumn(sortColumn, sortOrder);
    v->addWidget(m_procTable, 1);
    v->addWidget(makeNote(
        QStringLiteral("CPU %: 100% is one logical CPU fully busy, so a multithreaded process can exceed 100% "
                       "(up to 100% \u00d7 the number of logical CPUs). Memory is the resident set size, which includes "
                       "shared pages. Disk I/O is only readable for your own processes. This view is read-only."),
        page));
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
    updateStatus();
}

void MainWindow::setIntervalMs(int ms)
{
    const auto& choices = intervalChoicesMs();
    if (std::find(choices.begin(), choices.end(), ms) == choices.end())
        return;
    if (m_intervalCombo->currentData().toInt() != ms)
        m_intervalCombo->setCurrentIndex(m_intervalCombo->findData(ms));
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
    for (const auto& [key, label] : m_facts)
        if (key == id)
            return label;
    return nullptr;
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
    for (auto* chart : allCharts()) {
        chart->setViewEnd(end);
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
        series = {{std::string(ps::keys::diskAllRead), QStringLiteral("Read"), 0},
                  {std::string(ps::keys::diskAllWrite), QStringLiteral("Write"), 1}};
        m_diskChart->setTitle(QStringLiteral("Disk throughput: all physical disks"));
    } else {
        series = {{ps::keys::diskRead(dev.toStdString()), QStringLiteral("Read"), 0},
                  {ps::keys::diskWrite(dev.toStdString()), QStringLiteral("Write"), 1}};
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
        series = {{std::string(ps::keys::netAllRx), QStringLiteral("Receive"), 0},
                  {std::string(ps::keys::netAllTx), QStringLiteral("Transmit"), 1}};
        m_netChart->setTitle(netAggregateTitle(m_latest.get()));
    } else {
        series = {{ps::keys::netRx(dev.toStdString()), QStringLiteral("Receive"), 0},
                  {ps::keys::netTx(dev.toStdString()), QStringLiteral("Transmit"), 1}};
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
        // First run without a saved choice: plot up to six sensors that read.
        if (!m_sensorsChosen && m_wantedSensors.isEmpty()) {
            for (const auto& t : s.temperatures)
                if (ps::isValid(t.celsius) && m_wantedSensors.size() < 6)
                    m_wantedSensors << q(t.key);
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
    m_tempNote->setText(q(s.temperatureNote));
    m_tempNote->setVisible(!s.temperatureNote.empty());
    applySensorSelection();
}

void MainWindow::applySensorSelection()
{
    std::vector<ChartSeries> series;
    int visual = 0;
    int omitted = 0;
    for (int i = 0; i < m_sensorList->topLevelItemCount(); ++i) {
        auto* item = m_sensorList->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        std::string key = ps::keys::temperature(item->data(0, Qt::UserRole).toString().toStdString());
        QString label = item->text(0);
        // Listed but without a series: refused by the history's sensor quota.
        if (!m_history.empty() && !m_history.find(key)) {
            label += QStringLiteral(" (not recorded)");
            ++omitted;
        }
        series.push_back({std::move(key), label, visual++});
    }
    bool same = series.size() == m_tempChart->series().size();
    for (std::size_t i = 0; same && i < series.size(); ++i)
        same = series[i].key == m_tempChart->series()[i].key && series[i].label == m_tempChart->series()[i].label;
    if (!same)
        m_tempChart->setSeries(series);
    QString empty;
    if (m_sensorList->topLevelItemCount() == 0)
        empty = QStringLiteral("No temperature sensors");
    else if (series.empty())
        empty = QStringLiteral("Check a sensor to plot it");
    else if (omitted == static_cast<int>(series.size()))
        empty = QStringLiteral("Not recorded: the history limit for sensors is reached");
    else
        empty = QStringLiteral("Waiting for a valid reading");
    m_tempChart->setEmptyText(empty);
}

void MainWindow::updateSummaries()
{
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
        // that is hottest right now, drawn from its own history.
        std::vector<std::string> keys;
        if (hottest)
            keys.push_back(ps::keys::temperature(hottest->key));
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
        if (item->toolTip(0) != tip)
            for (int c = 0; c < 7; ++c)
                item->setToolTip(c, tip);
    }
}

void MainWindow::updateProcesses()
{
    if (const ps::Snapshot* s = m_latest.get()) {
        if (s->processesScanned)
            m_procModel->setProcesses(s->processes);
        else
            m_procModel->clear();
    }
    updateProcessCount();
}

void MainWindow::updateProcessCount()
{
    const ps::Snapshot* s = m_latest.get();
    QString text;
    if (!s) {
        text = QStringLiteral("Waiting for the first scan");
    } else if (!s->processesScanned) {
        text = QStringLiteral("Process list unavailable: /proc could not be read");
    } else {
        text = QStringLiteral("Showing %1 of %2 processes, %3 threads")
                   .arg(m_procProxy->rowCount())
                   .arg(s->processes.size())
                   .arg(s->threadCount);
        if (m_stale)
            text += QStringLiteral(". No recent samples: values are out of date and shown as unavailable");
        else if (m_paused)
            text += QStringLiteral(". Paused: frozen at the last sample");
    }
    if (m_procCount->text() != text)
        m_procCount->setText(text);
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

void MainWindow::updateStatus()
{
    QString text;
    if (m_paused) {
        text = QStringLiteral("Paused: the view is frozen. Resuming starts new rate baselines and leaves a gap.");
        if (m_stale)
            text += QStringLiteral(" Samples had already stopped arriving before the pause, so current values stay "
                                   "unavailable.");
    } else if (m_stale) {
        const double age = now() - m_lastAcquired;
        const double waited = now() - m_waitingSince;
        const bool futureShown = m_latest && isFromFuture(*m_latest);
        if (futureShown)
            text = QStringLiteral("The newest sample has an inconsistent time. Values are shown as unavailable "
                                  "until a current sample arrives.");
        else if (ps::isValid(m_lastAcquired) && ps::isValid(age) && age >= 0)
            text = QStringLiteral("The newest sample is %1 s old: the collector is blocked or starved. Values are "
                                  "shown as unavailable until it recovers.")
                       .arg(age, 0, 'f', 0);
        else if (ps::isValid(m_lastAcquired))
            text = QStringLiteral("The newest sample has an inconsistent time. Values are shown as unavailable "
                                  "until a current sample arrives.");
        else
            text = QStringLiteral("Waiting %1 s for a sample: the collector is blocked or starved. Values are shown "
                                  "as unavailable until it recovers.")
                       .arg(ps::isValid(waited) ? waited : 0.0, 0, 'f', 0);
    } else if (!m_latest) {
        text = m_options.startCollector ? QStringLiteral("Starting\u2026") : QStringLiteral("Not collecting");
    } else if (!m_latest->continuous) {
        text = QStringLiteral("Warming up: CPU use and transfer rates appear after the next sample.");
    } else {
        text = QStringLiteral("Live, sampling every %1.").arg(intervalText(m_intervalMs));
    }
    if (m_latest && !m_paused && !m_latest->problems.empty()) {
        QStringList problems;
        for (const auto& p : m_latest->problems)
            problems << q(p);
        text += QStringLiteral(" Unavailable: ") + problems.join(QStringLiteral("; ")) + QLatin1Char('.');
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
        if (!limited.isEmpty())
            text += QStringLiteral(" History limit reached, not recorded: ") + limited.join(QStringLiteral(", "))
                + QLatin1Char('.');
    }
    if (m_statusLabel->text() != text)
        m_statusLabel->setText(text);
    const QString timing = (m_latest && ps::isValid(m_latest->collectMs))
        ? QStringLiteral("Sample took %1 ms").arg(m_latest->collectMs, 0, 'f', 1)
        : QString();
    if (m_timingLabel->text() != timing)
        m_timingLabel->setText(timing);
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
    const QString name = QStringLiteral("pulsescope-%1.csv")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export History as CSV"),
                                                      QDir(dir).filePath(name),
                                                      QStringLiteral("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty())
        return;
    if (!startCsvExport(path, &error))
        statusBar()->showMessage(error, 8000);
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this, QStringLiteral("About PulseScope"),
        QStringLiteral("<p><b>PulseScope %1</b></p>"
                       "<p>A native Linux performance monitor. It reads /proc and /sys locally; it needs no "
                       "privileges, never changes the system and sends nothing over the network.</p>"
                       "<p>History is kept in memory for 15 minutes and discarded on exit unless exported.</p>")
            .arg(QStringLiteral(PULSESCOPE_VERSION)));
}
