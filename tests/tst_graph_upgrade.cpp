// Independent regression tests for the graph upgrade: smooth (frame-rate)
// chart motion between samples, colour-matched area fills, and their
// invariants (no fill across gaps or outside the plot, no stacking, no
// overshoot or extrapolation, stable readouts, palette changes).
//
// Oracles are measured pixels and known inputs, never the chart's own
// geometry code: vertical positions are calibrated from renders of known
// values, and fills are detected against a reference render of the same
// chart whose series lies far below the fixed range (so axes, grid, gap
// shading and background are identical and only the series paint differs).
#include "app/cli.h"
#include "app/mainwindow.h"
#include "app/metricbutton.h"
#include "app/processmodel.h"
#include "app/timechart.h"
#include "core/format.h"
#include "core/history.h"
#include "core/snapshot.h"

#include <QAbstractAnimation>
#include <QAction>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QProcess>
#include <QScreen>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QTreeView>
#include <QToolBar>
#include <QTreeWidget>
#include <QApplication>
#include <QBackingStore>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <vector>

namespace {

constexpr double kFar = -1e6; // reference value: far below every fixed range

QPalette lightPalette()
{
    QPalette p;
    for (auto role : {QPalette::Base, QPalette::Window, QPalette::ToolTipBase})
        p.setColor(role, Qt::white);
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ToolTipText, QPalette::Highlight})
        p.setColor(role, Qt::black);
    p.setColor(QPalette::PlaceholderText, QColor(110, 110, 110));
    return p;
}

QPalette darkPalette()
{
    QPalette p;
    for (auto role : {QPalette::Base, QPalette::Window, QPalette::ToolTipBase})
        p.setColor(role, QColor(30, 30, 46));
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ToolTipText, QPalette::Highlight})
        p.setColor(role, QColor(205, 214, 244));
    p.setColor(QPalette::PlaceholderText, QColor(150, 150, 170));
    return p;
}

QImage render(TimeChart& c)
{
    QImage img(c.size(), QImage::Format_RGB32);
    img.fill(Qt::magenta);
    c.render(&img);
    return img;
}

// p modelled as ref + a * (colour - ref): a is the blend weight toward the
// series colour, r the distance of p from that blend line.
struct Fit {
    double a = 0, r = 0;
};
Fit fit(QRgb p, QRgb ref, const QColor& c)
{
    const double d[3] = {double(c.red() - qRed(ref)), double(c.green() - qGreen(ref)), double(c.blue() - qBlue(ref))};
    const double q[3] = {double(qRed(p) - qRed(ref)), double(qGreen(p) - qGreen(ref)), double(qBlue(p) - qBlue(ref))};
    const double dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    Fit f;
    f.a = dd > 0 ? (q[0] * d[0] + q[1] * d[1] + q[2] * d[2]) / dd : 0;
    double r2 = 0;
    for (int k = 0; k < 3; ++k)
        r2 += (q[k] - f.a * d[k]) * (q[k] - f.a * d[k]);
    f.r = std::sqrt(r2);
    return f;
}
bool differs(QRgb p, QRgb ref)
{
    return std::abs(qRed(p) - qRed(ref)) + std::abs(qGreen(p) - qGreen(ref)) + std::abs(qBlue(p) - qBlue(ref)) > 6;
}
// A translucent wash of the series colour over whatever is underneath.
bool tinted(QRgb p, QRgb ref, const QColor& c)
{
    if (!differs(p, ref))
        return false;
    const Fit f = fit(p, ref, c);
    return f.a >= 0.025 && f.a <= 0.9 && f.r <= 14;
}
bool strokePixel(QRgb p, QRgb ref, const QColor& c)
{
    const Fit f = fit(p, ref, c);
    return f.a >= 0.55 && f.r <= 34;
}

// Share of pixels in [x0,x1]x[y0,y1] tinted with c (relative to ref).
double tintShare(const QImage& img, const QImage& ref, const QColor& c, int x0, int x1, int y0, int y1)
{
    int n = 0, t = 0;
    for (int y = std::max(0, y0); y <= std::min(img.height() - 1, y1); ++y)
        for (int x = std::max(0, x0); x <= std::min(img.width() - 1, x1); ++x) {
            ++n;
            t += tinted(img.pixel(x, y), ref.pixel(x, y), c);
        }
    return n ? double(t) / n : 0;
}

// Stroke centre in column x (weighted by blend), or NaN.
double strokeY(const QImage& img, const QImage& ref, const QColor& c, int x, int yMin = 0, int yMax = -1)
{
    if (yMax < 0)
        yMax = img.height() - 1;
    double sw = 0, sy = 0;
    for (int y = std::max(0, yMin); y <= std::min(img.height() - 1, yMax); ++y) {
        if (!strokePixel(img.pixel(x, y), ref.pixel(x, y), c))
            continue;
        const double w = fit(img.pixel(x, y), ref.pixel(x, y), c).a;
        sw += w;
        sy += w * y;
    }
    return sw > 0 ? sy / sw : ps::kNaN;
}

// Horizontal extent of the stroke of c.
std::pair<int, int> strokeExtent(const QImage& img, const QImage& ref, const QColor& c)
{
    int lo = -1, hi = -1;
    for (int x = 0; x < img.width(); ++x)
        if (!std::isnan(strokeY(img, ref, c, x))) {
            if (lo < 0)
                lo = x;
            hi = x;
        }
    return {lo, hi};
}

void addSample(ps::History& h, double t, std::vector<std::pair<std::string, double>> values, bool continuous = true,
               ps::Unit unit = ps::Unit::Percent)
{
    QVERIFY(h.beginSample(t, continuous));
    for (const auto& [k, v] : values)
        h.set(k, k, unit, v);
    h.endSample();
}

// A chart over history h with one series per key, fixed range, 60 s window
// ending at viewEnd.
struct ChartRig {
    TimeChart chart;
    ChartRig(ps::Unit unit, std::vector<ChartSeries> series, double lo, double hi, double viewEnd,
             const QPalette& pal = lightPalette(), QSize size = {640, 320})
        : chart(QStringLiteral("Test chart"), unit)
    {
        chart.setPalette(pal);
        chart.resize(size);
        chart.setSeries(std::move(series));
        chart.setFixedRange(lo, hi);
        chart.setWindowSeconds(60);
        chart.setViewEnd(viewEnd);
    }
    QImage shot(const ps::History& h)
    {
        chart.setHistory(&h);
        chart.refresh();
        return render(chart);
    }
};

// Constant-valued history over [t0, t1] for the given keys.
std::unique_ptr<ps::History> constant(std::vector<std::pair<std::string, double>> values, int t0 = 0, int t1 = 60)
{
    auto h = std::make_unique<ps::History>(512, 1e9);
    for (int t = t0; t <= t1; ++t)
        addSample(*h, t, values);
    return h;
}

// Linear value -> screen-y map calibrated from two rendered constant lines.
struct YMap {
    double a = ps::kNaN, b = ps::kNaN; // y = a + b * v
    double operator()(double v) const { return a + b * v; }
    bool valid() const { return std::isfinite(a) && std::isfinite(b) && b < 0; }
};
YMap calibrate(ChartRig& rig, const QColor& c, double v1, double v2, int x)
{
    auto h1 = constant({{"s", v1}});
    auto h2 = constant({{"s", v2}});
    auto hr = constant({{"s", kFar}});
    const QImage ref = rig.shot(*hr);
    const double y1 = strokeY(rig.shot(*h1), ref, c, x);
    const double y2 = strokeY(rig.shot(*h2), ref, c, x);
    YMap m;
    m.b = (y2 - y1) / (v2 - v1);
    m.a = y1 - m.b * v1;
    return m;
}

// ---------------------------------------------------------------------------
// MainWindow fixture

std::shared_ptr<ps::Snapshot> snap(uint64_t seq, double t, double cpu, bool continuous = true)
{
    auto s = std::make_shared<ps::Snapshot>();
    s->sequence = seq;
    s->generation = 1;
    s->time = t;
    s->interval = 1;
    s->continuous = continuous;
    s->collectMs = 1;
    s->logicalCpus = 2;
    s->cpu.total = cpu;
    s->cpu.user = cpu * 0.6;
    s->cpu.system = cpu * 0.4;
    s->cpu.iowait = 0;
    s->cpu.steal = 0;
    for (int i = 0; i < 2; ++i) {
        ps::CoreUsage c;
        c.index = i;
        c.online = true;
        c.usage = s->cpu;
        s->cores.push_back(c);
    }
    constexpr double GiB = 1024.0 * 1024 * 1024;
    s->memory.valid = true;
    s->memory.total = 8 * GiB;
    s->memory.used = 2 * GiB;
    s->memory.available = 6 * GiB;
    s->memory.free = 1 * GiB;
    s->memory.cache = 3 * GiB;
    s->memory.hasSwap = false;
    s->diskTotal = {"physical disks", 0, ps::kNaN, ps::kNaN};
    s->netTotal = {"all but loopback", 0, ps::kNaN, ps::kNaN};
    return s;
}

// Controlled clock: base + offset + real elapsed time, so animation sees time
// advance while every measured sample is placed exactly.
struct TestClock {
    QElapsedTimer et;
    double base = 5000;
    double offset = 0;
    TestClock() { et.start(); }
    double now() const { return base + offset + double(et.nsecsElapsed()) / 1e9; }
};

bool inTree(const QObject* o, const QObject* root)
{
    for (; o; o = o->parent())
        if (o == root)
            return true;
    return false;
}

// Counts, from the application's event stream, what the window actually does:
// paints of one chart (with the chart's viewEnd at each paint), and timer /
// update-request activity anywhere in the window's object tree.
class Activity : public QObject {
public:
    Activity(QWidget* window, TimeChart* chart) : m_window(window), m_chart(chart)
    {
        qApp->installEventFilter(this);
    }
    ~Activity() override { qApp->removeEventFilter(this); }
    void reset()
    {
        paints = timers = updates = 0;
        timerBy.clear();
        viewEnds.clear();
        otherChartPaints = 0;
    }
    int paints = 0, timers = 0, updates = 0, otherChartPaints = 0;
    std::vector<double> viewEnds;
    std::map<QString, int> timerBy; // receiver class/objectName -> timer events
    QString timerSummary() const
    {
        QStringList l;
        for (const auto& [k, v] : timerBy)
            l << QStringLiteral("%1 x%2").arg(k).arg(v);
        return l.join(QStringLiteral(", "));
    }
    TimeChart* other = nullptr; // a chart that must stay unpainted (hidden page)

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        switch (e->type()) {
        case QEvent::Paint:
            if (o == m_chart) {
                ++paints;
                viewEnds.push_back(m_chart->viewEnd());
            } else if (other && o == other) {
                ++otherChartPaints;
            }
            break;
        case QEvent::Timer:
            if (inTree(o, m_window)) {
                ++timers;
                ++timerBy[QString::fromLatin1(o->metaObject()->className()) + QLatin1Char('/') + o->objectName()];
            }
            break;
        case QEvent::UpdateRequest:
            if (inTree(o, m_window))
                ++updates;
            break;
        default: break;
        }
        return false;
    }

private:
    QPointer<QWidget> m_window;
    TimeChart* m_chart;
};

int runningAnimations(const QObject* root)
{
    int n = 0;
    for (const auto* a : root->findChildren<QAbstractAnimation*>())
        n += a->state() == QAbstractAnimation::Running;
    return n;
}

int differingPixels(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return -1;
    int n = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            n += differs(a.pixel(x, y), b.pixel(x, y));
    return n;
}

// Rightmost column holding a stroke of colour c on background base.
int rightmostStroke(const QImage& img, const QColor& base, const QColor& c, int yMin, int yMax)
{
    for (int x = img.width() - 1; x >= 0; --x)
        for (int y = std::max(0, yMin); y <= std::min(img.height() - 1, yMax); ++y)
            if (strokePixel(img.pixel(x, y), base.rgb(), c))
                return x;
    return -1;
}
int leftmostStroke(const QImage& img, const QColor& base, const QColor& c, int yMin, int yMax)
{
    for (int x = 0; x < img.width(); ++x)
        for (int y = std::max(0, yMin); y <= std::min(img.height() - 1, yMax); ++y)
            if (strokePixel(img.pixel(x, y), base.rgb(), c))
                return x;
    return -1;
}

} // namespace

class TestGraphUpgrade : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void oracleDetectsSyntheticFill_data();
    void oracleDetectsSyntheticFill();
    void chartFillTintsInteriorBelowConstantLine_data();
    void chartFillTintsInteriorBelowConstantLine();
    void chartFillFollowsStepSeriesWithoutOvershoot();
    void chartFillStopsAtUnavailableAndDiscontinuity();
    void chartOverlappingSeriesNotStacked();
    void chartSignedCelsiusAndZeroDrawnInsidePlot();
    void chartPaletteSwitchRecoloursLineFillAndText();
    void chartReadoutStableWhileViewAdvances();
    void chartNoExtrapolationPastLatestSample();
    void mainWindowVisibleChartAnimatesBetweenSamples();
    void mainWindowAnimationIdleWhenHiddenMinimizedOrPaused();
    void mainWindowResumeNoExtrapolationSingleDriver();
    void mainWindowDetailsDefaultSimpleActionsAndPersistence();
    void mainWindowDetailsToggleKeepsSelectionsHistoryAndWarnings();
    void mainWindowCsvAndReadoutSameAcrossDetailsAndSmooth();
    void mainWindowSmoothOffStepsPerSampleKeepsSampling();
    void chartRevealStaysOnMeasuredSegment();
    void chartPartialRepaintMatchesFullRender_data();
    void chartPartialRepaintMatchesFullRender();
    void chartPartialRepaintAtFractionalScales_data();
    void chartPartialRepaintAtFractionalScales();
    void metricButtonOffWindowSpikeKeepsSegmentGeometry_data();
    void metricButtonOffWindowSpikeKeepsSegmentGeometry();
    void chartLargeClockOffsetsSameAsSmall_data();
    void chartLargeClockOffsetsSameAsSmall();
    void mainWindowSensorVisualsFollowIdentity();
    void mainWindowNineSensorsKeepVisualsAcrossChoices();
    void cliThemeDecisionPrecedence_data();
    void cliThemeDecisionPrecedence();
    void cliThemeProbeReadsArgumentsAsQt_data();
    void cliThemeProbeReadsArgumentsAsQt();
    void appThemeBootstrapUsesDesktopConfiguration();
};

// ---------------------------------------------------------------------------
// Area fills (TimeChart, pixel oracles)

void TestGraphUpgrade::oracleDetectsSyntheticFill_data()
{
    QTest::addColumn<bool>("dark");
    QTest::addColumn<double>("alpha");
    for (bool d : {false, true})
        for (double a : {0.08, 0.18, 0.35})
            QTest::addRow("%s alpha %.2f", d ? "dark" : "light", a) << d << a;
}

void TestGraphUpgrade::oracleDetectsSyntheticFill()
{
    // Positive control for the fill oracle: the real chart render has no fill
    // (0%), and the same render with a synthetic translucent wash of the line
    // colour painted below the line is detected (>= 90%) without tinting the
    // area above. Proves "0% tinted" failures are not a blind oracle.
    QFETCH(bool, dark);
    QFETCH(double, alpha);
    const QPalette pal = dark ? darkPalette() : lightPalette();
    const QColor c = seriesColor(0, pal);
    ChartRig rig(ps::Unit::Percent, {{"s", QStringLiteral("S"), 0}}, 0, 100, 60, pal);
    auto hr = constant({{"s", kFar}});
    auto h = constant({{"s", 50}});
    const QImage ref = rig.shot(*hr);
    const QImage img = rig.shot(*h);
    const auto [xL, xR] = strokeExtent(img, ref, c);
    QVERIFY(xL >= 0 && xR - xL > 300);
    const int xm = (xL + xR) / 2;
    const YMap y = calibrate(rig, c, 25, 75, xm);
    QVERIFY(y.valid());
    QImage synth = img;
    {
        QPainter p(&synth);
        QColor wash = c;
        wash.setAlphaF(float(alpha));
        p.fillRect(QRect(QPoint(xL, int(y(50)) + 2), QPoint(xR, int(y(0)))), wash);
    }
    const int cx0 = xL + (xR - xL) / 5, cx1 = xR - (xR - xL) / 5;
    const double synthBelow = tintShare(synth, ref, c, cx0, cx1, int(y(50)) + 4, int(y(0)) - 3);
    const double synthAbove = tintShare(synth, ref, c, cx0, cx1, int(y(100)) + 3, int(y(50)) - 4);
    const double realAbove = tintShare(img, ref, c, cx0, cx1, int(y(100)) + 3, int(y(50)) - 4);
    qInfo("synthetic wash alpha %.2f: below %.1f%%, above %.1f%%; real render above %.1f%%", alpha, synthBelow * 100,
          synthAbove * 100, realAbove * 100);
    QVERIFY2(synthBelow >= 0.9, qPrintable(QStringLiteral("oracle missed a synthetic fill: %1%").arg(synthBelow * 100)));
    QVERIFY2(synthAbove <= 0.02, qPrintable(QStringLiteral("oracle saw a fill above the line: %1%").arg(synthAbove * 100)));
    QVERIFY2(realAbove <= 0.02, qPrintable(QStringLiteral("grid/background misread as fill: %1%").arg(realAbove * 100)));
}

void TestGraphUpgrade::chartFillTintsInteriorBelowConstantLine_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("light palette") << false;
    QTest::newRow("dark palette") << true;
}

void TestGraphUpgrade::chartFillTintsInteriorBelowConstantLine()
{
    // Task-Manager-style area: a translucent wash of the line's own colour
    // fills the plot between the line and the baseline; nothing above the
    // line and nothing outside the plot is tinted.
    QFETCH(bool, dark);
    const QPalette pal = dark ? darkPalette() : lightPalette();
    const QColor c = seriesColor(0, pal);
    ChartRig rig(ps::Unit::Percent, {{"s", QStringLiteral("S"), 0}}, 0, 100, 60, pal);
    auto hr = constant({{"s", kFar}});
    auto h = constant({{"s", 50}});
    const QImage ref = rig.shot(*hr);
    const QImage img = rig.shot(*h);
    const auto [xL, xR] = strokeExtent(img, ref, c);
    QVERIFY2(xL >= 0 && xR - xL > 300, qPrintable(QStringLiteral("stroke extent %1..%2").arg(xL).arg(xR)));
    const int xm = (xL + xR) / 2;
    const YMap y = calibrate(rig, c, 25, 75, xm);
    QVERIFY2(y.valid(), "could not calibrate the value axis from rendered lines");
    const double y50 = strokeY(img, ref, c, xm);
    QVERIFY2(std::fabs(y50 - y(50)) <= 1.5, qPrintable(QStringLiteral("line at y=%1, calibrated %2").arg(y50).arg(y(50))));

    const int cx0 = xL + (xR - xL) / 5, cx1 = xR - (xR - xL) / 5;
    const double below = tintShare(img, ref, c, cx0, cx1, int(y(50)) + 4, int(y(0)) - 3);
    const double above = tintShare(img, ref, c, cx0, cx1, int(y(100)) + 3, int(y(50)) - 4);
    const double underPlot = tintShare(img, ref, c, cx0, cx1, int(y(0)) + 4, int(y(0)) + 16);
    const double leftOfPlot = tintShare(img, ref, c, xL - 16, xL - 4, int(y(100)), int(y(0)));
    QStringList problems;
    if (below < 0.6)
        problems << QStringLiteral("interior below the line is not filled with the line colour: %1% of pixels tinted "
                                   "(expected >= 60%)")
                        .arg(below * 100, 0, 'f', 1);
    if (above > 0.02)
        problems << QStringLiteral("area above the line tinted: %1%").arg(above * 100, 0, 'f', 1);
    if (underPlot > 0.02)
        problems << QStringLiteral("fill leaks below the plot: %1%").arg(underPlot * 100, 0, 'f', 1);
    if (leftOfPlot > 0.02)
        problems << QStringLiteral("fill leaks left of the plot: %1%").arg(leftOfPlot * 100, 0, 'f', 1);
    // The stroke stays the full series colour on top of the fill.
    const QRgb onLine = img.pixel(xm, int(std::lround(y50)));
    if (fit(onLine, ref.pixel(xm, int(std::lround(y50))), c).a < 0.7)
        problems << QStringLiteral("stroke not drawn in the full series colour over the fill");
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
}

void TestGraphUpgrade::chartFillFollowsStepSeriesWithoutOvershoot()
{
    // Values alternate 20 / 80 every 10 s. The fill follows the measured line
    // (full below it, none above it), and smoothing never draws beyond the
    // measured extremes (no spline overshoot creating new peaks).
    const QPalette pal = lightPalette();
    const QColor c = seriesColor(0, pal);
    ChartRig rig(ps::Unit::Percent, {{"s", QStringLiteral("S"), 0}}, 0, 100, 60, pal);
    auto hr = constant({{"s", kFar}});
    const QImage ref = rig.shot(*hr);
    auto h = std::make_unique<ps::History>(512, 1e9);
    auto val = [](int t) { return (t / 10) % 2 ? 80.0 : 20.0; };
    for (int t = 0; t <= 60; ++t)
        addSample(*h, t, {{"s", val(t)}});
    const QImage img = rig.shot(*h);
    auto flat = constant({{"s", 50}});
    const auto [xL, xR] = strokeExtent(rig.shot(*flat), ref, c);
    QVERIFY(xL >= 0 && xR > xL);
    const YMap y = calibrate(rig, c, 25, 75, (xL + xR) / 2);
    QVERIFY(y.valid());
    auto xOf = [&](double t) { return xL + (xR - xL) * t / 60.0; }; // t=0 at the left edge, t=60 at the right

    QStringList problems;
    int overshoot = 0;
    QString example;
    for (int x = xL; x <= xR; ++x)
        for (int yy = int(y(100)) - 6; yy <= int(y(0)) + 6; ++yy)
            if (strokePixel(img.pixel(x, yy), ref.pixel(x, yy), c) && (yy < y(80) - 3 || yy > y(20) + 3)) {
                if (example.isEmpty())
                    example = QStringLiteral("(%1,%2)").arg(x).arg(yy);
                ++overshoot;
            }
    if (overshoot)
        problems << QStringLiteral("%1 stroke pixels outside the measured 20..80 band, e.g. %2 (80 at y=%3, 20 at y=%4)")
                        .arg(overshoot).arg(example).arg(y(80), 0, 'f', 1).arg(y(20), 0, 'f', 1);
    for (int k = 0; k < 6; ++k) {
        const double tc = 10 * k + 5;
        const double v = val(int(tc));
        const int x0 = int(xOf(tc - 2)), x1 = int(xOf(tc + 2));
        const double below = tintShare(img, ref, c, x0, x1, int(y(v)) + 4, int(y(0)) - 3);
        const double above = tintShare(img, ref, c, x0, x1, int(y(100)) + 3, int(y(v)) - 4);
        if (below < 0.6)
            problems << QStringLiteral("plateau t=%1 (%2%): %3% below the line tinted").arg(tc).arg(v).arg(below * 100, 0, 'f', 1);
        if (above > 0.02)
            problems << QStringLiteral("plateau t=%1 (%2%): %3% above the line tinted").arg(tc).arg(v).arg(above * 100, 0, 'f', 1);
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
}

void TestGraphUpgrade::chartFillStopsAtUnavailableAndDiscontinuity()
{
    // 50 % with unavailable samples at t = 20..29 and a pause/outage between
    // t = 40 and t = 46 (continuous = false at 46). Neither interval gets a
    // line or a fill; filled data on both sides proves the fill exists.
    const QPalette pal = lightPalette();
    const QColor c = seriesColor(0, pal);
    ChartRig rig(ps::Unit::Percent, {{"s", QStringLiteral("S"), 0}}, 0, 100, 60, pal);
    auto times = [] {
        std::vector<int> ts;
        for (int t = 0; t <= 40; ++t)
            ts.push_back(t);
        for (int t = 46; t <= 60; ++t)
            ts.push_back(t);
        return ts;
    }();
    ps::History hr(512, 1e9), h(512, 1e9);
    for (int t : times) {
        addSample(hr, t, {{"s", kFar}}, t != 46);
        addSample(h, t, {{"s", (t >= 20 && t <= 29) ? ps::kNaN : 50.0}}, t != 46);
    }
    auto flat = constant({{"s", 50}});
    auto far = constant({{"s", kFar}});
    const QImage flatRef = rig.shot(*far);
    const auto [xL, xR] = strokeExtent(rig.shot(*flat), flatRef, c);
    QVERIFY(xL >= 0 && xR > xL);
    const YMap y = calibrate(rig, c, 25, 75, (xL + xR) / 2);
    QVERIFY(y.valid());
    auto xOf = [&](double t) { return xL + (xR - xL) * t / 60.0; };
    const QImage ref = rig.shot(hr);
    const QImage img = rig.shot(h);
    const int top = int(y(100)) + 2, bottom = int(y(0)) - 2;

    QStringList problems;
    struct Band { const char* what; double t0, t1; };
    for (const Band& b : {Band {"unavailable 20..29 s", 20.5, 28.5}, Band {"discontinuity 40..46 s", 41, 45}}) {
        const int x0 = int(std::ceil(xOf(b.t0))), x1 = int(xOf(b.t1));
        const double share = tintShare(img, ref, c, x0, x1, top, bottom);
        int strokes = 0;
        for (int x = x0; x <= x1; ++x)
            strokes += !std::isnan(strokeY(img, ref, c, x, top, bottom));
        if (share > 0.02 || strokes)
            problems << QStringLiteral("%1: %2% tinted, %3 stroke columns (expected none)")
                            .arg(QLatin1String(b.what)).arg(share * 100, 0, 'f', 1).arg(strokes);
    }
    for (const Band& b : {Band {"data 3..17 s", 3, 17}, Band {"data 32..38 s", 32, 38}, Band {"data 49..58 s", 49, 58}}) {
        const double share = tintShare(img, ref, c, int(xOf(b.t0)), int(xOf(b.t1)), int(y(50)) + 4, bottom - 1);
        if (share < 0.6)
            problems << QStringLiteral("%1: only %2% below the line tinted (expected a fill)")
                            .arg(QLatin1String(b.what)).arg(share * 100, 0, 'f', 1);
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
}

void TestGraphUpgrade::chartOverlappingSeriesNotStacked()
{
    // A = 60 %, B = 30 %: two independent overlapping areas, not a stack.
    // B's line is at 30 % (a stack would put it at 90 %); only A's colour
    // washes the band between the lines; each identity keeps its colour when
    // the series order changes.
    const QPalette pal = lightPalette();
    const QColor ca = seriesColor(0, pal), cb = seriesColor(11, pal); // both solid pens
    QCOMPARE(seriesStyle(0), Qt::SolidLine);
    QCOMPARE(seriesStyle(11), Qt::SolidLine);
    ChartRig rig(ps::Unit::Percent, {{"a", QStringLiteral("A"), 0}, {"b", QStringLiteral("B"), 11}}, 0, 100, 60, pal);
    auto hr = constant({{"a", kFar}, {"b", kFar}});
    auto h = constant({{"a", 60}, {"b", 30}});
    const QImage ref = rig.shot(*hr);
    const QImage img = rig.shot(*h);

    ChartRig cal(ps::Unit::Percent, {{"s", QStringLiteral("S"), 0}}, 0, 100, 60, pal);
    auto flat = constant({{"s", 50}});
    auto far = constant({{"s", kFar}});
    const auto [xL, xR] = strokeExtent(cal.shot(*flat), cal.shot(*far), ca);
    QVERIFY(xL >= 0 && xR > xL);
    const int xm = (xL + xR) / 2;
    const YMap y = calibrate(cal, ca, 25, 75, xm);
    QVERIFY(y.valid());

    QStringList problems;
    const double ya = strokeY(img, ref, ca, xm), yb = strokeY(img, ref, cb, xm);
    if (!(std::fabs(ya - y(60)) <= 2))
        problems << QStringLiteral("A line at y=%1, expected %2 (60 %)").arg(ya).arg(y(60));
    if (!(std::fabs(yb - y(30)) <= 2))
        problems << QStringLiteral("B line at y=%1, expected %2 (30 %; stacked would be %3)").arg(yb).arg(y(30)).arg(y(90));
    const int cx0 = xL + (xR - xL) / 5, cx1 = xR - (xR - xL) / 5;
    const double between = tintShare(img, ref, ca, cx0, cx1, int(y(60)) + 4, int(y(30)) - 4);
    const double betweenB = tintShare(img, ref, cb, cx0, cx1, int(y(60)) + 4, int(y(30)) - 4);
    const double aboveA = tintShare(img, ref, ca, cx0, cx1, int(y(100)) + 3, int(y(60)) - 4)
                          + tintShare(img, ref, cb, cx0, cx1, int(y(100)) + 3, int(y(60)) - 4);
    int painted = 0, n = 0;
    for (int yy = int(y(30)) + 4; yy <= int(y(0)) - 3; ++yy)
        for (int x = cx0; x <= cx1; ++x, ++n)
            painted += differs(img.pixel(x, yy), ref.pixel(x, yy));
    if (between < 0.6)
        problems << QStringLiteral("band 30..60 %: %1% in A's colour (expected A's fill)").arg(between * 100, 0, 'f', 1);
    if (betweenB > 0.02)
        problems << QStringLiteral("band 30..60 %: %1% in B's colour (B's fill must stay below B)").arg(betweenB * 100, 0, 'f', 1);
    if (aboveA > 0.02)
        problems << QStringLiteral("above both lines tinted: %1%").arg(aboveA * 100, 0, 'f', 1);
    if (n && double(painted) / n < 0.6)
        problems << QStringLiteral("below both lines only %1% painted").arg(100.0 * painted / n, 0, 'f', 1);

    // Same identities in the other order: same colours at the same heights.
    rig.chart.setSeries({{"b", QStringLiteral("B"), 11}, {"a", QStringLiteral("A"), 0}});
    const QImage swapped = rig.shot(*h);
    const double ya2 = strokeY(swapped, ref, ca, xm), yb2 = strokeY(swapped, ref, cb, xm);
    if (!(std::fabs(ya2 - ya) <= 1 && std::fabs(yb2 - yb) <= 1))
        problems << QStringLiteral("series order changed identities: A %1->%2, B %3->%4").arg(ya).arg(ya2).arg(yb).arg(yb2);
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
}

void TestGraphUpgrade::chartSignedCelsiusAndZeroDrawnInsidePlot()
{
    // Temperatures: -10 °C and 0 °C are measured values (never "missing"),
    // drawn at their height; any fill stays inside the plot and next to the
    // line, whether it runs to the zero line or to the plot bottom.
    const QPalette pal = lightPalette();
    const QColor c = seriesColor(0, pal);
    ChartRig rig(ps::Unit::Celsius, {{"s", QStringLiteral("S"), 0}}, -20, 40, 60, pal);
    auto hr = constant({{"s", kFar}});
    const QImage ref = rig.shot(*hr);
    auto h30 = constant({{"s", 30}});
    const auto [xL, xR] = strokeExtent(rig.shot(*h30), ref, c);
    QVERIFY(xL >= 0 && xR > xL);
    const int xm = (xL + xR) / 2;
    const YMap y = calibrate(rig, c, 10, 30, xm);
    QVERIFY(y.valid());
    const int top = int(y(40)), bottom = int(y(-20));
    const int cx0 = xL + (xR - xL) / 5, cx1 = xR - (xR - xL) / 5;

    QStringList problems;
    for (double v : {-10.0, 0.0}) {
        auto h = constant({{"s", v}});
        const QImage img = rig.shot(*h);
        const double yl = strokeY(img, ref, c, xm);
        if (!(std::fabs(yl - y(v)) <= 2))
            problems << QStringLiteral("%1 °C line at y=%2, expected %3").arg(v).arg(yl).arg(y(v));
        if (rig.chart.lastPaintedPoints() < 50)
            problems << QStringLiteral("%1 °C: only %2 points drawn").arg(v).arg(rig.chart.lastPaintedPoints());
        const double outside = tintShare(img, ref, c, cx0, cx1, bottom + 4, bottom + 16)
                               + tintShare(img, ref, c, cx0, cx1, top - 16, top - 4);
        if (outside > 0.02)
            problems << QStringLiteral("%1 °C: fill outside the plot (%2%)").arg(v).arg(outside * 100, 0, 'f', 1);
        const double far = tintShare(img, ref, c, cx0, cx1, top + 3, int(std::min(y(v), y(0))) - 4);
        if (far > 0.02)
            problems << QStringLiteral("%1 °C: tint above both the line and the zero line (%2%)").arg(v).arg(far * 100, 0, 'f', 1);
        if (v < 0) {
            const double toZero = tintShare(img, ref, c, cx0, cx1, int(y(0)) + 4, int(y(v)) - 4);
            const double toBottom = tintShare(img, ref, c, cx0, cx1, int(y(v)) + 4, bottom - 3);
            if (std::max(toZero, toBottom) < 0.6)
                problems << QStringLiteral("-10 °C: no fill next to the line (to zero %1%, to bottom %2%)")
                                .arg(toZero * 100, 0, 'f', 1).arg(toBottom * 100, 0, 'f', 1);
        }
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
}

void TestGraphUpgrade::chartPaletteSwitchRecoloursLineFillAndText()
{
    // Switching light -> dark on a live chart (as the desktop theme does)
    // must not keep cached light-theme colours: background, stroke, fill and
    // title all follow the new palette, and text keeps readable contrast.
    ChartRig rig(ps::Unit::Percent, {{"s", QStringLiteral("S"), 0}}, 0, 100, 60, lightPalette());
    auto h = constant({{"s", 50}});
    auto hr = constant({{"s", kFar}});
    QStringList problems;
    auto luminance = [](QRgb p) {
        auto lin = [](int v) {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * lin(qRed(p)) + 0.7152 * lin(qGreen(p)) + 0.0722 * lin(qBlue(p));
    };
    auto contrast = [&](QRgb a, QRgb b) {
        const double la = luminance(a), lb = luminance(b);
        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    };
    for (bool dark : {false, true, false}) {
        const QPalette pal = dark ? darkPalette() : lightPalette();
        rig.chart.setPalette(pal);
        const QColor c = seriesColor(0, pal);
        const QColor base = pal.color(QPalette::Base);
        const QImage ref = rig.shot(*hr);
        const QImage img = rig.shot(*h);
        const char* name = dark ? "dark" : "light";
        const auto [xL, xR] = strokeExtent(img, ref, c);
        if (xL < 0) {
            problems << QStringLiteral("%1: no stroke in the palette's series colour").arg(QLatin1String(name));
            continue;
        }
        const int xm = (xL + xR) / 2;
        const double yl = strokeY(img, ref, c, xm);
        if (img.pixel(2, 2) != base.rgb())
            problems << QStringLiteral("%1: background %2 is not the palette base %3")
                            .arg(QLatin1String(name), QColor(img.pixel(2, 2)).name(), base.name());
        const double below = tintShare(img, ref, c, xL + 40, xR - 40, int(yl) + 4, int(yl) + 60);
        if (below < 0.6)
            problems << QStringLiteral("%1: fill below the line %2% in the palette's series colour")
                            .arg(QLatin1String(name)).arg(below * 100, 0, 'f', 1);
        double best = 1;
        for (int yy = 0; yy < 18; ++yy)
            for (int x = 0; x < img.width() / 2; ++x)
                best = std::max(best, contrast(img.pixel(x, yy), base.rgb()));
        if (best < 4.5)
            problems << QStringLiteral("%1: title contrast %2:1 (< 4.5:1)").arg(QLatin1String(name)).arg(best, 0, 'f', 2);
        const double strokeContrast = contrast(c.rgb(), base.rgb());
        if (strokeContrast < 3)
            problems << QStringLiteral("%1: series colour contrast %2:1 (< 3:1)").arg(QLatin1String(name)).arg(strokeContrast, 0, 'f', 2);
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
}

void TestGraphUpgrade::chartReadoutStableWhileViewAdvances()
{
    // Smooth motion moves the axis between samples; the readout at a measured
    // instant must keep that sample's raw value, whatever the view position.
    ps::History h(512, 1e9);
    for (int t = 0; t <= 60; ++t)
        addSample(h, t, {{"s", 10.0 + (t * 37 % 71)}});
    const double expected = 10.0 + (50 * 37 % 71); // value at t = 50: 10 + 4 = 14
    QCOMPARE(expected, 14.0);
    TimeChart c(QStringLiteral("CPU"), ps::Unit::Percent);
    c.setPalette(lightPalette());
    c.resize(640, 320);
    c.setHistory(&h);
    c.setSeries({{"s", QStringLiteral("S"), 0}});
    c.setFixedRange(0, 100);
    c.setWindowSeconds(60);
    const QString row = QStringLiteral("S: ") + QString::fromStdString(ps::fmt::percent(expected));
    QStringList problems;
    for (int k = 0; k <= 60; ++k) { // one second of 60 Hz view positions
        c.setViewEnd(60 + k / 60.0);
        c.refresh();
        c.setCursorTime(50);
        render(c);
        const QString ro = c.readoutText();
        const QString values = ro.section(QLatin1Char('\n'), 1);
        if (values != row) {
            problems << QStringLiteral("viewEnd %1: readout \"%2\" (expected \"%3\")").arg(60 + k / 60.0, 0, 'f', 3).arg(ro, row);
            break;
        }
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
}

void TestGraphUpgrade::chartNoExtrapolationPastLatestSample()
{
    // Between samples the axis runs ahead of the newest sample; the line (and
    // fill) must end at the newest measured point, never be extended to "now".
    const QPalette pal = lightPalette();
    const QColor c = seriesColor(0, pal);
    ChartRig rig(ps::Unit::Percent, {{"s", QStringLiteral("S"), 0}}, 0, 100, 100, pal);
    auto h = constant({{"s", 50}}, 30, 100);
    auto hr = constant({{"s", kFar}}, 30, 100);
    const QImage ref0 = rig.shot(*hr);
    const auto [xL, xR] = strokeExtent(rig.shot(*h), ref0, c);
    QVERIFY(xL >= 0 && xR - xL > 300);
    const double W = xR - xL; // 60 s
    QStringList problems;
    for (double ahead : {0.5, 1.0, 10.0}) {
        rig.chart.setViewEnd(100 + ahead);
        const QImage ref = rig.shot(*hr);
        const QImage img = rig.shot(*h);
        const double expectedRight = xR - W * ahead / 60.0;
        int lastPainted = -1;
        for (int x = img.width() - 1; x >= 0 && lastPainted < 0; --x)
            for (int yy = 0; yy < img.height(); ++yy)
                if (differs(img.pixel(x, yy), ref.pixel(x, yy)) && yy > 30) { // below the legend
                    lastPainted = x;
                    break;
                }
        if (lastPainted > expectedRight + 2.5)
            problems << QStringLiteral("%1 s ahead: series paint reaches x=%2, newest sample at x=%3")
                            .arg(ahead).arg(lastPainted).arg(expectedRight, 0, 'f', 1);
        const auto [l2, r2] = strokeExtent(img, ref, c);
        if (std::fabs(r2 - expectedRight) > 2.5)
            problems << QStringLiteral("%1 s ahead: stroke ends at x=%2, expected %3").arg(ahead).arg(r2).arg(expectedRight, 0, 'f', 1);
        Q_UNUSED(l2);
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
}

// ---------------------------------------------------------------------------
// MainWindow: the animation driver

namespace {
struct WindowRig {
    QTemporaryDir dir;
    TestClock clock;
    std::unique_ptr<MainWindow> w;
    TimeChart* cpu = nullptr;
    uint64_t seq = 0;
    WindowRig()
    {
        MainWindowOptions o;
        o.startCollector = false;
        o.settingsFile = dir.filePath(QStringLiteral("settings.ini"));
        o.initialIntervalMs = 1000;
        o.clock = [this] { return clock.now(); };
        w = std::make_unique<MainWindow>(o);
        w->setWindowSeconds(60);
        w->resize(1180, 780);
        w->showPage(MainWindow::CpuPage);
        cpu = w->findChild<TimeChart*>(QStringLiteral("cpuChart"));
    }
    // n one-second samples ending now.
    void feed(int n)
    {
        const double end = clock.now();
        for (int i = n - 1; i >= 0; --i)
        {
            ++seq;
            w->applySnapshot(snap(seq, end - i, 30 + 20 * std::sin(double(seq))));
        }
    }
    void feedOne() { w->applySnapshot(snap(++seq, clock.now(), 40)); }
};
} // namespace

void TestGraphUpgrade::mainWindowVisibleChartAnimatesBetweenSamples()
{
    // With 1 s sampling, a visible chart must move smoothly between samples:
    // many real paints per second, each at a later view position, while the
    // measured history does not change. The rate is measured, not assumed.
    WindowRig r;
    QVERIFY(r.cpu);
    r.w->show();
    QVERIFY(QTest::qWaitForWindowExposed(r.w.get()));
    r.feed(70);
    QTest::qWait(50);
    Activity act(r.w.get(), r.cpu);
    act.other = r.w->findChild<TimeChart*>(QStringLiteral("memoryChart"));
    const std::size_t samples = r.w->history().size();
    const uint64_t total = r.w->history().totalSamples();
    const int applied = r.w->snapshotsApplied();
    QElapsedTimer et;
    et.start();
    act.reset();
    QTest::qWait(1000);
    const double secs = et.nsecsElapsed() / 1e9;
    const auto ends = act.viewEnds;
    std::set<double> distinct(ends.begin(), ends.end());
    bool monotonic = std::is_sorted(ends.begin(), ends.end());
    const double span = ends.empty() ? 0 : ends.back() - ends.front();
    const int paints = act.paints;
    const double rate = paints / secs;
    qInfo("observed: %d chart paints in %.3f s (%.1f /s), %zu distinct view ends spanning %.3f s, %d window timer "
          "events, %d update requests, hidden-page chart paints %d",
          paints, secs, rate, distinct.size(), span, act.timers, act.updates, act.otherChartPaints);
    const int otherPaints = act.otherChartPaints;

    // Pixel evidence: two captures 250 ms apart differ (the plot moved).
    const QImage a = r.cpu->grab().toImage();
    const double endA = r.cpu->viewEnd();
    QTest::qWait(250);
    const QImage b = r.cpu->grab().toImage();
    const double endB = r.cpu->viewEnd();
    const int changed = differingPixels(a, b);

    QStringList problems;
    if (paints < 40)
        problems << QStringLiteral("only %1 chart paints in %2 s between samples (expected >= 40 for smooth motion)")
                        .arg(paints).arg(secs, 0, 'f', 2);
    if (distinct.size() < 40)
        problems << QStringLiteral("only %1 distinct view positions painted").arg(distinct.size());
    if (!monotonic)
        problems << QStringLiteral("view end moved backwards between paints");
    if (!(span >= 0.7 * secs && span <= secs + 0.1))
        problems << QStringLiteral("painted view ends span %1 s over %2 s of real time").arg(span, 0, 'f', 3).arg(secs, 0, 'f', 3);
    if (!(endB - endA >= 0.1))
        problems << QStringLiteral("view end advanced %1 s over 250 ms without a sample").arg(endB - endA, 0, 'f', 3);
    if (changed <= 50)
        problems << QStringLiteral("captures 250 ms apart differ in %1 pixels").arg(changed);
    if (otherPaints != 0)
        problems << QStringLiteral("chart on a hidden page painted %1 times").arg(otherPaints);
    if (r.w->history().size() != samples || r.w->history().totalSamples() != total || r.w->snapshotsApplied() != applied)
        problems << QStringLiteral("measured history changed while only the view should move");
    if (r.w->isStale())
        problems << QStringLiteral("window went stale during the measurement (fixture error)");
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
}

void TestGraphUpgrade::mainWindowAnimationIdleWhenHiddenMinimizedOrPaused()
{
    // The frame driver costs nothing when nobody can see it: hidden,
    // minimised or paused windows get no chart paints and (almost) no timer
    // activity, and a paused chart's axis stays put.
    WindowRig r;
    QVERIFY(r.cpu);
    Activity act(r.w.get(), r.cpu);
    QStringList problems;
    auto measure = [&](const char* state, int ms, bool expectFrozenAxis) {
        const double end0 = r.cpu->viewEnd();
        act.reset();
        QTest::qWait(ms);
        qInfo("%s: %d chart paints, %d timer events [%s], %d update requests, %d running animations in %d ms", state,
              act.paints, act.timers, qPrintable(act.timerSummary()), act.updates, runningAnimations(r.w.get()), ms);
        if (act.paints > 1)
            problems << QStringLiteral("%1: %2 chart paints").arg(QLatin1String(state)).arg(act.paints);
        // The 500 ms freshness check is allowed; a frame driver is not.
        if (act.timers > 4 || act.updates > 2)
            problems << QStringLiteral("%1: %2 timer events, %3 update requests in %4 ms")
                            .arg(QLatin1String(state)).arg(act.timers).arg(act.updates).arg(ms);
        if (runningAnimations(r.w.get()) > 0)
            problems << QStringLiteral("%1: %2 animations running").arg(QLatin1String(state)).arg(runningAnimations(r.w.get()));
        if (expectFrozenAxis && r.cpu->viewEnd() != end0)
            problems << QStringLiteral("%1: axis moved %2 s").arg(QLatin1String(state)).arg(r.cpu->viewEnd() - end0);
    };

    r.feed(70);
    QTest::qWait(300); // let one-shot item-view layout timers from construction fire
    measure("never shown", 600, false);

    r.w->show();
    QVERIFY(QTest::qWaitForWindowExposed(r.w.get()));
    r.feedOne();
    r.w->hide();
    QTest::qWait(50);
    measure("hidden", 600, false);

    r.w->show();
    QVERIFY(QTest::qWaitForWindowExposed(r.w.get()));
    r.feedOne();
    r.w->showMinimized();
    QTest::qWait(50);
    if (r.w->isMinimized())
        measure("minimised", 600, false);
    else
        qInfo("platform ignored minimise; minimised case not measurable here");

    r.w->showNormal();
    QVERIFY(QTest::qWaitForWindowExposed(r.w.get()));
    r.feedOne();
    r.w->setPaused(true);
    QTest::qWait(100);
    measure("paused", 600, true);
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
}

void TestGraphUpgrade::mainWindowResumeNoExtrapolationSingleDriver()
{
    // Pause, let 10 s pass, resume: before a new sample arrives the axis is
    // at the real time and the line still ends at the last measured sample
    // (the pause shows as empty time, never a line drawn across it). Page
    // switches, pause/resume and hide/show must not multiply frame drivers.
    WindowRig r;
    QVERIFY(r.cpu);
    r.w->show();
    QVERIFY(QTest::qWaitForWindowExposed(r.w.get()));
    r.feed(70);
    QTest::qWait(100);
    Activity act(r.w.get(), r.cpu);
    QStringList problems;

    act.reset();
    QTest::qWait(1000);
    const int timers1 = act.timers, paints1 = act.paints, anims1 = runningAnimations(r.w.get());
    qInfo("before toggles: %d timer events, %d paints, %d running animations per s", timers1, paints1, anims1);

    for (int k = 0; k < 10; ++k) {
        r.w->showPage(static_cast<MainWindow::Page>(k % MainWindow::PageCount));
        QTest::qWait(10);
    }
    for (int k = 0; k < 5; ++k) {
        r.w->setPaused(true);
        QTest::qWait(10);
        r.w->setPaused(false);
        QTest::qWait(10);
    }
    r.w->hide();
    QTest::qWait(20);
    r.w->show();
    QVERIFY(QTest::qWaitForWindowExposed(r.w.get()));
    r.w->showPage(MainWindow::CpuPage);
    r.feedOne();
    QTest::qWait(100);
    act.reset();
    QTest::qWait(1000);
    const int timers2 = act.timers, paints2 = act.paints, anims2 = runningAnimations(r.w.get());
    qInfo("after toggles: %d timer events, %d paints, %d running animations per s", timers2, paints2, anims2);
    if (timers2 > timers1 * 1.5 + 4)
        problems << QStringLiteral("timer events per second rose from %1 to %2 after page/pause/show toggles").arg(timers1).arg(timers2);
    if (anims2 > std::max(anims1, 1))
        problems << QStringLiteral("running animations rose from %1 to %2 after toggles").arg(anims1).arg(anims2);
    if (paints2 < 40)
        problems << QStringLiteral("after toggles only %1 chart paints per second (animation did not resume)").arg(paints2);

    // Geometry of the last sample before the pause (newest at the right edge),
    // taken once the newest segment is fully revealed: the reveal is part of
    // how a sample arrives, not a pause artefact.
    r.feedOne();
    QTRY_VERIFY_WITH_TIMEOUT(!r.cpu->isRevealing(), 2000);
    QTest::qWait(30);
    QVERIFY(!r.cpu->isRevealing());
    const QColor base = r.cpu->palette().color(QPalette::Base);
    const QColor c0 = seriesColor(0, r.cpu->palette());
    const int H = r.cpu->height();
    const QImage before = r.cpu->grab().toImage();
    const double endBefore = r.cpu->viewEnd();
    const double latest = r.w->history().latestTime();
    const int xR = rightmostStroke(before, base, c0, 40, H - 20);
    const int xL = leftmostStroke(before, base, c0, 40, H - 20);
    const double pxPerSecond = (xR - xL) / (60.0 - (endBefore - latest));
    const std::size_t samples = r.w->history().size();
    // Independent check of the scale: the plot is 60 s wide.
    const double plotPxPerSecond = r.cpu->plotArea().width() / 60.0;
    if (std::fabs(pxPerSecond - plotPxPerSecond) > 0.05 * plotPxPerSecond)
        problems << QStringLiteral("line scale %1 px/s disagrees with the plot width %2 px/s (fixture)")
                        .arg(pxPerSecond, 0, 'f', 2).arg(plotPxPerSecond, 0, 'f', 2);

    r.w->setPaused(true);
    // While paused nothing is recorded and the view is frozen.
    r.w->applySnapshot(snap(r.seq + 1, r.clock.now(), 90));
    if (r.w->history().size() != samples)
        problems << QStringLiteral("a snapshot delivered while paused was recorded");
    r.clock.offset += 10;
    QTest::qWait(100);
    r.w->setPaused(false);
    QTest::qWait(300);
    const QImage after = r.cpu->grab().toImage();
    const double endAfter = r.cpu->viewEnd();
    const int xR2 = rightmostStroke(after, base, c0, 40, H - 20);
    const double expected = xR - (endAfter - endBefore) * pxPerSecond;
    qInfo("resume: view end %.3f -> %.3f (clock %.3f), newest sample %.3f, stroke right edge %d -> %d (expected %.1f)",
          endBefore, endAfter, r.clock.now(), latest, xR, xR2, expected);
    if (xL < 0 || xR <= xL)
        problems << QStringLiteral("could not locate the CPU total line before the pause");
    else {
        if (!(endAfter >= endBefore + 10))
            problems << QStringLiteral("after resume the axis ends at %1, not at the current time (>= %2)")
                            .arg(endAfter, 0, 'f', 3).arg(endBefore + 10, 0, 'f', 3);
        if (std::fabs(xR2 - expected) > 3)
            problems << QStringLiteral("after resume the line ends at x=%1 but the newest sample is at x=%2 (%3)")
                            .arg(xR2).arg(expected, 0, 'f', 1)
                            .arg(xR2 > expected ? QStringLiteral("drawn across the pause") : QStringLiteral("cut short"));
    }
    if (r.w->history().size() != samples)
        problems << QStringLiteral("resume added %1 samples without a snapshot").arg(int(r.w->history().size() - samples));

    // The first sample after the pause is not continuous with the last one
    // before it (the collector restarts): it simply appears, with no reveal
    // and no line or area bridging the paused time.
    ++r.seq;
    r.w->applySnapshot(snap(r.seq, r.clock.now(), 40, false));
    const bool revealing = r.cpu->isRevealing();
    QTest::qWait(60);
    const QImage bridged = r.cpu->grab().toImage();
    const double endB = r.cpu->viewEnd();
    const double xOld = xR - (endB - endBefore) * pxPerSecond;
    const double xNew = xR + (endBefore - latest) * pxPerSecond - (endB - r.w->history().latestTime()) * pxPerSecond;
    // A fill pixel carries the series hue; neutral outage shading (grey over
    // the base) does not, even where its blend weight looks similar.
    auto spread = [](QRgb p) {
        const int hi = std::max({qRed(p), qGreen(p), qBlue(p)}), lo = std::min({qRed(p), qGreen(p), qBlue(p)});
        return hi - lo;
    };
    const double hueSpread = std::max({c0.red(), c0.green(), c0.blue()}) - std::min({c0.red(), c0.green(), c0.blue()});
    auto fillPixel = [&](QRgb p) {
        if (!tinted(p, base.rgb(), c0))
            return false;
        return spread(p) - spread(base.rgb()) >= std::max(4.0, 0.5 * fit(p, base.rgb(), c0).a * hueSpread);
    };
    const QRectF plot = r.cpu->plotArea();
    const int rowTop = int(plot.top()) + 2, rowBottom = int(plot.bottom()) - 2;
    int bridge = 0, filled = 0, n = 0;
    for (int x = int(std::ceil(xOld + 4)); x <= int(std::floor(xNew - 4)); ++x)
        for (int y = rowTop; y <= rowBottom; ++y) {
            ++n;
            bridge += strokePixel(bridged.pixel(x, y), base.rgb(), c0);
            filled += fillPixel(bridged.pixel(x, y));
        }
    // Positive control: just left of the gap, near the plot bottom (below
    // every measured value) the area is filled.
    int ctlFilled = 0, ctl = 0;
    for (int x = int(xOld) - 80; x <= int(xOld) - 10; ++x)
        for (int y = rowBottom - 12; y <= rowBottom - 2; ++y) {
            ++ctl;
            ctlFilled += fillPixel(bridged.pixel(x, y));
        }
    qInfo("after the pause: gap columns %.1f..%.1f, %d stroke pixels, %d of %d in the fill colour (control left of "
          "the gap %d of %d), reveal %d",
          xOld, xNew, bridge, filled, n, ctlFilled, ctl, int(revealing));
    if (!(ctl > 0 && ctlFilled >= ctl * 6 / 10))
        problems << QStringLiteral("fill oracle control failed: %1 of %2 pixels under the data").arg(ctlFilled).arg(ctl);
    if (n > 0 && filled > n / 50)
        problems << QStringLiteral("area fill drawn across the paused time: %1 of %2 pixels").arg(filled).arg(n);
    if (revealing)
        problems << QStringLiteral("the first sample after the pause was revealed along a segment from before it");
    if (!(xNew - xOld > 40))
        problems << QStringLiteral("gap after the pause only %1 px wide (fixture)").arg(xNew - xOld, 0, 'f', 1);
    if (bridge > 0)
        problems << QStringLiteral("%1 line pixels drawn across the paused time (x %2..%3)")
                        .arg(bridge).arg(xOld, 0, 'f', 1).arg(xNew, 0, 'f', 1);
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
}


// ---------------------------------------------------------------------------
// Details view, smooth graphs and CSV (MainWindow)

namespace {
ps::TemperatureReading tempReading(const std::string& key, const std::string& chip, const std::string& label, double c)
{
    ps::TemperatureReading t;
    t.key = key;
    t.chip = chip;
    t.label = label;
    t.displayName = chip + " " + label;
    t.path = "/sys/class/hwmon/" + chip + "/temp1_input";
    t.celsius = c;
    return t;
}

// A snapshot with every page populated: two disks, two interfaces, one
// filesystem, the given temperatures and three processes.
std::shared_ptr<ps::Snapshot> richSnap(uint64_t seq, double t, std::vector<ps::TemperatureReading> temps = {},
                                       bool continuous = true)
{
    auto s = snap(seq, t, 20 + double(seq % 7), continuous);
    for (const char* n : {"nvme0n1", "sda"}) {
        ps::DiskIo d;
        d.name = n;
        d.sizeBytes = 512e9;
        d.readBps = 1e6 * double(seq % 5);
        d.writeBps = 2e6;
        d.busyPercent = 3;
        s->disks.push_back(d);
    }
    s->diskTotal = {"physical disks", 2, 2e6, 4e6};
    for (const char* n : {"eth0", "wlan0"}) {
        ps::NetIo i;
        i.name = n;
        i.operState = "up";
        i.rxBps = 3e5;
        i.txBps = 1e5;
        s->interfaces.push_back(i);
    }
    s->netTotal = {"all but loopback", 2, 6e5, 2e5};
    ps::Filesystem f;
    f.source = "/dev/nvme0n1p2";
    f.fsType = "btrfs";
    f.mountPoints = {"/"};
    f.totalBytes = 500e9;
    f.usedBytes = 200e9;
    f.availBytes = 290e9;
    s->filesystems.push_back(f);
    s->temperatures = std::move(temps);
    s->processesScanned = true;
    int pid = 100;
    for (const char* n : {"ps-alpha", "ps-beta", "ps-gamma"}) {
        ps::ProcessInfo p;
        p.pid = ++pid;
        p.ppid = 1;
        p.startTicks = 1000 + uint64_t(pid);
        p.uidKnown = true;
        p.user = "tester";
        p.name = n;
        p.command = std::string("/usr/bin/") + n;
        p.threads = 2;
        p.state = 'S';
        p.cpuPercent = double(pid % 4);
        p.rssBytes = 1e6 * pid;
        p.ageSeconds = 100;
        s->processes.push_back(p);
    }
    return s;
}

std::vector<ps::TemperatureReading> threeSensors(double t)
{
    return {tempReading("hwmon:k10temp:temp1", "k10temp", "Tctl", 45 - t * 0.1),
            tempReading("hwmon:nvme:temp1", "nvme", "Composite", 60 + t * 0.5),
            tempReading("thermal:acpitz", "acpitz", "temp1", 30)};
}

MainWindowOptions windowOptions(const QString& settings, TestClock& clock)
{
    MainWindowOptions o;
    o.startCollector = false;
    o.settingsFile = settings;
    o.initialIntervalMs = 1000;
    o.clock = [&clock] { return clock.now(); };
    return o;
}

bool inMenuTitled(QWidget* w, QAction* a, const QString& title)
{
    for (auto* m : w->findChildren<QMenu*>()) {
        QString t = m->title();
        t.remove(QLatin1Char('&'));
        if (t == title && m->actions().contains(a))
            return true;
    }
    return false;
}

bool inToolbar(QWidget* w, QAction* a)
{
    for (auto* tb : w->findChildren<QToolBar*>())
        if (tb->actions().contains(a))
            return true;
    return false;
}

QWidget* factContainer(QWidget* w, const char* id)
{
    auto* l = w->findChild<QLabel*>(QString::fromLatin1(id));
    return l ? l->parentWidget() : nullptr;
}

QTreeWidgetItem* sensorItem(QTreeWidget* list, const QString& key)
{
    for (int i = 0; list && i < list->topLevelItemCount(); ++i)
        if (list->topLevelItem(i)->data(0, Qt::UserRole).toString() == key)
            return list->topLevelItem(i);
    return nullptr;
}

void settle(int ms = 30)
{
    for (int k = 0; k < 3; ++k)
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QTest::qWait(ms);
}

QString labelText(QWidget* w, const QString& name)
{
    auto* l = w->findChild<QLabel*>(name);
    return l ? l->text() : QStringLiteral("<missing %1>").arg(name);
}

struct DetailWidget {
    MainWindow::Page page;
    const char* what;
    std::function<QWidget*(QWidget*)> find;
};
std::vector<DetailWidget> detailWidgets()
{
    auto named = [](const char* n) {
        return [n](QWidget* w) { return w->findChild<QWidget*>(QString::fromLatin1(n)); };
    };
    auto fact = [](const char* id) { return [id](QWidget* w) { return factContainer(w, id); }; };
    return {{MainWindow::CpuPage, "CPU facts", fact("cpu_total")},
            {MainWindow::CpuPage, "per-core header", named("coreHeader")},
            {MainWindow::CpuPage, "per-core charts", named("coreScroll")},
            {MainWindow::MemoryPage, "memory facts", fact("mem_total")},
            {MainWindow::MemoryPage, "swap chart", named("swapChart")},
            {MainWindow::DiskPage, "disk facts", fact("disk_read")},
            {MainWindow::DiskPage, "filesystem table", named("filesystemTable")},
            {MainWindow::NetworkPage, "network facts", fact("net_rx")},
            {MainWindow::TemperaturePage, "sensor picker", named("sensorList")}};
}
struct Essential {
    MainWindow::Page page;
    const char* name;
};
const Essential kEssentials[] = {{MainWindow::CpuPage, "cpuChart"},          {MainWindow::MemoryPage, "memoryChart"},
                                 {MainWindow::DiskPage, "diskChart"},        {MainWindow::DiskPage, "diskSelector"},
                                 {MainWindow::NetworkPage, "netChart"},      {MainWindow::NetworkPage, "netSelector"},
                                 {MainWindow::TemperaturePage, "temperatureChart"},
                                 {MainWindow::ProcessPage, "processFilter"}, {MainWindow::ProcessPage, "processTable"},
                                 {MainWindow::CpuPage, "rangeCombo"}};

// What each page shows in the current Details state; problems for widgets
// on the wrong side.
QStringList checkDetailState(MainWindow& w, bool details)
{
    QStringList problems;
    const QString state = details ? QStringLiteral("Details") : QStringLiteral("Simple");
    for (const auto& d : detailWidgets()) {
        w.showPage(d.page);
        settle(10);
        QWidget* x = d.find(&w);
        if (!x) {
            problems << QStringLiteral("%1: %2 not found").arg(state, QLatin1String(d.what));
            continue;
        }
        const bool shown = x->isVisibleTo(&w) && x->isVisible();
        if (shown != details)
            problems << QStringLiteral("%1: %2 %3").arg(state, QLatin1String(d.what),
                                                      shown ? QStringLiteral("shown") : QStringLiteral("hidden"));
        if (details && shown && (x->width() <= 0 || x->height() <= 0))
            problems << QStringLiteral("%1: %2 has no size (%3x%4)").arg(state, QLatin1String(d.what)).arg(x->width()).arg(x->height());
    }
    for (const auto& e : kEssentials) {
        w.showPage(e.page);
        settle(10);
        auto* x = w.findChild<QWidget*>(QString::fromLatin1(e.name));
        if (!x || !x->isVisible() || x->width() <= 0 || x->height() <= 0)
            problems << QStringLiteral("%1: essential %2 not on screen").arg(state, QLatin1String(e.name));
    }
    // The update interval moved from the toolbar to View > Update Interval:
    // still reachable from the menu bar in both views, all four choices.
    {
        auto* menu = w.findChild<QMenu*>(QStringLiteral("intervalMenu"));
        bool inBar = false;
        if (menu)
            for (QAction* top : w.menuBar()->actions())
                if (top->menu())
                    for (QAction* a : top->menu()->actions())
                        inBar = inBar || a->menu() == menu;
        int enabled = 0;
        if (menu)
            for (QAction* a : menu->actions())
                enabled += a->isEnabled() && a->isVisible() && a->isCheckable();
        if (!menu || !inBar || enabled != 4 || !menu->menuAction()->isEnabled())
            problems << QStringLiteral("%1: update interval menu not reachable (menu %2, in menu bar %3, %4 choices)")
                            .arg(state).arg(menu != nullptr).arg(inBar).arg(enabled);
        if (w.findChild<QWidget*>(QStringLiteral("intervalCombo")))
            problems << QStringLiteral("%1: the removed interval combo box is back").arg(state);
    }
    w.showPage(MainWindow::ProcessPage);
    settle(10);
    if (auto* table = w.findChild<QTreeView*>(QStringLiteral("processTable"))) {
        for (int c = 0; c < ProcessModel::ColumnCount; ++c) {
            const bool essential = c == ProcessModel::Pid || c == ProcessModel::Name || c == ProcessModel::Cpu ||
                                   c == ProcessModel::Memory;
            const bool hidden = table->isColumnHidden(c);
            if (essential && hidden)
                problems << QStringLiteral("%1: essential process column %2 hidden").arg(state).arg(c);
            if (!essential && hidden == details)
                problems << QStringLiteral("%1: process column %2 %3").arg(state).arg(c)
                                .arg(hidden ? QStringLiteral("hidden") : QStringLiteral("shown"));
        }
    } else {
        problems << QStringLiteral("processTable missing");
    }
    auto* cpu = w.findChild<TimeChart*>(QStringLiteral("cpuChart"));
    auto* mem = w.findChild<TimeChart*>(QStringLiteral("memoryChart"));
    if (cpu && mem) {
        const std::size_t cpuSeries = details ? 4 : 1, memSeries = details ? 3 : 1;
        if (cpu->series().size() != cpuSeries || cpu->series().front().key != ps::keys::cpuTotal)
            problems << QStringLiteral("%1: CPU chart has %2 series (expected %3, total first)").arg(state)
                            .arg(cpu->series().size()).arg(cpuSeries);
        if (mem->series().size() != memSeries || mem->series().front().key != ps::keys::memUsed)
            problems << QStringLiteral("%1: memory chart has %2 series (expected %3, used first)").arg(state)
                            .arg(mem->series().size()).arg(memSeries);
    }
    return problems;
}

std::vector<std::pair<std::string, QString>> seriesKeys(const TimeChart* c)
{
    std::vector<std::pair<std::string, QString>> v;
    for (const auto& s : c->series())
        v.emplace_back(s.key, s.label);
    return v;
}

// Equal exports: every recorded column byte for byte. The second column is
// wall-clock time converted at export time from a millisecond clock, so it
// may differ by a millisecond between two exports of the same history.
bool sameCsv(const QByteArray& a, const QByteArray& b)
{
    const QList<QByteArray> x = a.split('\n'), y = b.split('\n');
    if (x.size() != y.size())
        return false;
    for (int i = 0; i < x.size(); ++i) {
        if (x[i] == y[i])
            continue;
        QList<QByteArray> p = x[i].split(','), q = y[i].split(',');
        if (i == 0 || p.size() < 2 || p.size() != q.size())
            return false;
        bool ok1 = false, ok2 = false;
        const double d = std::fabs(p[1].toDouble(&ok1) - q[1].toDouble(&ok2));
        if (!ok1 || !ok2 || d > 0.0025)
            return false;
        p.removeAt(1);
        q.removeAt(1);
        if (p != q)
            return false;
    }
    return true;
}

QByteArray exportCsv(MainWindow& w, const QString& path, QString* error)
{
    QSignalSpy done(&w, &MainWindow::csvExportFinished);
    if (!w.startCsvExport(path, error))
        return {};
    if (!done.wait(10000) || done.isEmpty() || !done.front().at(0).toBool()) {
        if (error)
            *error = done.isEmpty() ? QStringLiteral("export did not finish") : done.front().at(2).toString();
        return {};
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot read %1").arg(path);
        return {};
    }
    return f.readAll();
}
} // namespace

void TestGraphUpgrade::mainWindowDetailsDefaultSimpleActionsAndPersistence()
{
    // A fresh profile opens the glance view with smooth graphs; Details and
    // Smooth Graphs are checkable actions (Details: Ctrl+D, toolbar and View
    // menu) that change only what is shown, and both choices persist.
    QTemporaryDir dir;
    TestClock clock;
    const QString settings = dir.filePath(QStringLiteral("settings.ini"));
    QStringList problems;
    {
        auto w = std::make_unique<MainWindow>(windowOptions(settings, clock));
        QVERIFY(!w->detailsVisible());
        QVERIFY(w->smoothGraphs());
        auto* details = w->findChild<QAction*>(QStringLiteral("detailsAction"));
        auto* smooth = w->findChild<QAction*>(QStringLiteral("smoothAction"));
        QVERIFY(details && smooth);
        QVERIFY(details->isCheckable() && !details->isChecked() && details->isEnabled());
        QVERIFY(smooth->isCheckable() && smooth->isChecked() && smooth->isEnabled());
        QCOMPARE(details->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_D));
        QVERIFY2(!details->text().isEmpty() && !details->toolTip().isEmpty(), "Details action has no text/tooltip");
        QVERIFY2(inToolbar(w.get(), details), "Details is not on a toolbar");
        QVERIFY2(inMenuTitled(w.get(), details, QStringLiteral("View")), "Details is not in the View menu");
        QVERIFY2(inMenuTitled(w.get(), smooth, QStringLiteral("View")), "Smooth Graphs is not in the View menu");
        w->resize(1180, 780);
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w.get()));
        for (int i = 0; i < 3; ++i)
            w->applySnapshot(richSnap(uint64_t(i + 1), clock.now() - 2 + i, threeSensors(i)));
        const std::size_t samples = w->history().size();
        problems << checkDetailState(*w, false);

        // Toggle through the action (what the toolbar, menu and Ctrl+D do).
        details->trigger();
        if (!w->detailsVisible() || !details->isChecked())
            problems << QStringLiteral("triggering Details did not turn it on");
        problems << checkDetailState(*w, true);
        if (w->history().size() != samples)
            problems << QStringLiteral("toggling Details changed the history");

        // Ctrl+D on the active window.
        w->activateWindow();
        if (QTest::qWaitForWindowActive(w.get(), 2000)) {
            QTest::keyClick(w.get(), Qt::Key_D, Qt::ControlModifier);
            if (w->detailsVisible())
                problems << QStringLiteral("Ctrl+D did not turn Details off");
            QTest::keyClick(w.get(), Qt::Key_D, Qt::ControlModifier);
            if (!w->detailsVisible())
                problems << QStringLiteral("Ctrl+D did not turn Details back on");
        } else {
            qInfo("window never became active on this platform; Ctrl+D not exercised");
        }
        smooth->trigger();
        if (w->smoothGraphs() || smooth->isChecked())
            problems << QStringLiteral("triggering Smooth Graphs did not turn it off");
    }
    {
        QSettings raw(settings, QSettings::IniFormat);
        if (raw.value(QStringLiteral("details")).toString() != QStringLiteral("true"))
            problems << QStringLiteral("setting 'details' saved as '%1'").arg(raw.value(QStringLiteral("details")).toString());
        if (raw.value(QStringLiteral("smoothGraphs")).toString() != QStringLiteral("false"))
            problems << QStringLiteral("setting 'smoothGraphs' saved as '%1'").arg(raw.value(QStringLiteral("smoothGraphs")).toString());
    }
    {
        MainWindow w(windowOptions(settings, clock));
        auto* details = w.findChild<QAction*>(QStringLiteral("detailsAction"));
        auto* smooth = w.findChild<QAction*>(QStringLiteral("smoothAction"));
        if (!w.detailsVisible() || !details || !details->isChecked())
            problems << QStringLiteral("Details on was not restored");
        if (w.smoothGraphs() || !smooth || smooth->isChecked())
            problems << QStringLiteral("Smooth Graphs off was not restored");
        w.resize(1180, 780);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.applySnapshot(richSnap(1, clock.now(), threeSensors(0)));
        problems << checkDetailState(w, true);
        if (w.isAnimating())
            problems << QStringLiteral("restored Smooth Graphs off but the frame clock runs");
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
}

void TestGraphUpgrade::mainWindowDetailsToggleKeepsSelectionsHistoryAndWarnings()
{
    // Details changes visibility only: selected disk, interface, sensors,
    // process filter and sort, range, page and the recorded history survive
    // any number of toggles, and stale/paused warnings stay visible.
    QTemporaryDir dir;
    TestClock clock;
    MainWindow w(windowOptions(dir.filePath(QStringLiteral("s.ini")), clock));
    w.resize(1180, 780);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    for (int i = 0; i < 5; ++i)
        w.applySnapshot(richSnap(uint64_t(i + 1), clock.now() - 4 + i, threeSensors(i)));
    auto* disk = w.findChild<QComboBox*>(QStringLiteral("diskSelector"));
    auto* net = w.findChild<QComboBox*>(QStringLiteral("netSelector"));
    auto* list = w.findChild<QTreeWidget*>(QStringLiteral("sensorList"));
    auto* filter = w.findChild<QLineEdit*>(QStringLiteral("processFilter"));
    auto* table = w.findChild<QTreeView*>(QStringLiteral("processTable"));
    auto* range = w.findChild<QComboBox*>(QStringLiteral("rangeCombo"));
    auto* temp = w.findChild<TimeChart*>(QStringLiteral("temperatureChart"));
    auto* cpu = w.findChild<TimeChart*>(QStringLiteral("cpuChart"));
    QVERIFY(disk && net && list && filter && table && range && temp && cpu);
    QVERIFY(disk->count() >= 2 && net->count() >= 2 && range->count() >= 2);
    disk->setCurrentIndex(disk->count() - 1);
    net->setCurrentIndex(net->count() - 1);
    auto* a = sensorItem(list, QStringLiteral("hwmon:k10temp:temp1"));
    auto* b = sensorItem(list, QStringLiteral("hwmon:nvme:temp1"));
    auto* c = sensorItem(list, QStringLiteral("thermal:acpitz"));
    QVERIFY(a && b && c);
    a->setCheckState(0, Qt::Unchecked);
    b->setCheckState(0, Qt::Checked);
    c->setCheckState(0, Qt::Checked);
    filter->setText(QStringLiteral("ps-"));
    table->sortByColumn(ProcessModel::Cpu, Qt::AscendingOrder);
    range->setCurrentIndex(1);
    w.applySnapshot(richSnap(6, clock.now(), threeSensors(5)));
    w.showPage(MainWindow::ProcessPage); // the table is refreshed while its page shows
    settle();
    w.showPage(MainWindow::MemoryPage);
    settle();

    struct State {
        QVariant disk, net, range;
        QString filter;
        int sortColumn;
        Qt::SortOrder sortOrder;
        std::vector<std::pair<std::string, QString>> temps;
        std::vector<int> tempVisuals;
        std::size_t samples;
        uint64_t total;
        int rows;
        double window;
        MainWindow::Page page;
        bool legend;
    };
    auto capture = [&] {
        State s;
        s.disk = disk->currentData();
        s.net = net->currentData();
        s.range = range->currentData();
        s.filter = filter->text();
        s.sortColumn = table->header()->sortIndicatorSection();
        s.sortOrder = table->header()->sortIndicatorOrder();
        s.temps = seriesKeys(temp);
        for (const auto& x : temp->series())
            s.tempVisuals.push_back(x.visual);
        s.samples = w.history().size();
        s.total = w.history().totalSamples();
        // Process rows: the children of the three group rows.
        s.rows = 0;
        for (int g = 0; g < table->model()->rowCount(); ++g)
            s.rows += table->model()->rowCount(table->model()->index(g, 0));
        s.window = cpu->windowSeconds();
        s.page = w.currentPage();
        s.legend = temp->isLegendVisible();
        return s;
    };
    const State before = capture();
    QStringList problems;
    if (before.temps.size() != 2)
        problems << QStringLiteral("fixture: %1 temperature series").arg(before.temps.size());
    if (!before.legend)
        problems << QStringLiteral("temperature legend hidden in the glance view (it names the exact sensors)");
    if (before.rows != 3)
        problems << QStringLiteral("fixture: filter shows %1 rows").arg(before.rows);
    auto* details = w.findChild<QAction*>(QStringLiteral("detailsAction"));
    QVERIFY(details);
    for (int k = 0; k < 4; ++k) {
        details->trigger();
        settle(10);
        const State s = capture();
        const QString step = QStringLiteral("after toggle %1 (Details %2)").arg(k + 1).arg(w.detailsVisible() ? "on" : "off");
        if (s.disk != before.disk || s.net != before.net || s.range != before.range)
            problems << step + QStringLiteral(": disk/interface/range selection changed");
        if (s.filter != before.filter || s.rows != before.rows)
            problems << step + QStringLiteral(": process filter changed (%1 rows)").arg(s.rows);
        if (s.sortColumn != before.sortColumn || s.sortOrder != before.sortOrder)
            problems << step + QStringLiteral(": process sort changed to column %1").arg(s.sortColumn);
        if (s.temps != before.temps || s.tempVisuals != before.tempVisuals)
            problems << step + QStringLiteral(": plotted sensors or their colours changed");
        if (s.samples != before.samples || s.total != before.total)
            problems << step + QStringLiteral(": history changed");
        if (s.window != before.window || s.page != before.page)
            problems << step + QStringLiteral(": range or page changed");
        if (!s.legend)
            problems << step + QStringLiteral(": temperature legend hidden");
    }

    // Warnings: stale, then paused, stay on the chart and headline in both views.
    w.showPage(MainWindow::CpuPage);
    clock.offset += 5;
    QTRY_VERIFY_WITH_TIMEOUT(w.isStale(), 3000);
    for (int k = 0; k < 2; ++k) {
        details->trigger();
        settle(10);
        const QString head = labelText(&w, QStringLiteral("cpuHeadSummary"));
        if (!w.isStale() || cpu->notice().isEmpty() || !head.contains(QStringLiteral("No recent samples")))
            problems << QStringLiteral("stale, Details %1: stale=%2 notice=\"%3\" headline=\"%4\"")
                            .arg(w.detailsVisible() ? "on" : "off").arg(w.isStale()).arg(cpu->notice(), head);
    }
    w.setPaused(true);
    for (int k = 0; k < 2; ++k) {
        details->trigger();
        settle(10);
        const QString head = labelText(&w, QStringLiteral("cpuHeadSummary"));
        if (cpu->notice().isEmpty() || !head.contains(QStringLiteral("Paused")))
            problems << QStringLiteral("paused, Details %1: notice=\"%2\" headline=\"%3\"")
                            .arg(w.detailsVisible() ? "on" : "off").arg(cpu->notice(), head);
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
}

void TestGraphUpgrade::mainWindowCsvAndReadoutSameAcrossDetailsAndSmooth()
{
    // The export and the cursor readout are raw recorded values: identical
    // bytes and text whatever the view shows or how it moves.
    QTemporaryDir dir;
    TestClock clock;
    MainWindow w(windowOptions(dir.filePath(QStringLiteral("s.ini")), clock));
    w.resize(1180, 780);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    const double t0 = clock.now() - 20;
    for (int i = 0; i < 20; ++i)
        w.applySnapshot(richSnap(uint64_t(i + 1), t0 + i, threeSensors(i), i != 12));
    w.setPaused(true); // nothing new is recorded between the exports
    auto* cpu = w.findChild<TimeChart*>(QStringLiteral("cpuChart"));
    QVERIFY(cpu);
    QStringList problems;
    QByteArray first;
    QString firstReadout;
    int n = 0;
    for (bool details : {false, true})
        for (bool smooth : {true, false}) {
            w.setDetailsVisible(details);
            w.setSmoothGraphs(smooth);
            settle(40);
            QString err;
            const QByteArray csv = exportCsv(w, dir.filePath(QStringLiteral("e%1.csv").arg(n++)), &err);
            cpu->setCursorTime(t0 + 7);
            const QString ro = cpu->readoutText().section(QLatin1Char('\n'), 1);
            if (csv.isEmpty())
                problems << QStringLiteral("details %1 smooth %2: export failed: %3").arg(details).arg(smooth).arg(err);
            else if (first.isEmpty())
                first = csv;
            else if (!sameCsv(csv, first)) {
                const QList<QByteArray> x = first.split('\n'), y = csv.split('\n');
                int line = 0;
                while (line < std::min(x.size(), y.size()) && x[line] == y[line])
                    ++line;
                problems << QStringLiteral("details %1 smooth %2: CSV differs (%3 vs %4 bytes), line %5:\n    first \"%6\"\n    now   \"%7\"")
                                .arg(details).arg(smooth).arg(csv.size()).arg(first.size()).arg(line + 1)
                                .arg(QString::fromUtf8(x.value(line)), QString::fromUtf8(y.value(line)));
            }
            // Details adds rows for secondary series; the Total row must match.
            const QString total = ro.section(QLatin1Char('\n'), 0, 0);
            if (firstReadout.isEmpty())
                firstReadout = total;
            else if (total != firstReadout)
                problems << QStringLiteral("details %1 smooth %2: readout \"%3\" (first \"%4\")")
                                .arg(details).arg(smooth).arg(total, firstReadout);
        }
    const double expectedTotal = 20 + double(8 % 7); // sample 8 at t0 + 7
    if (!firstReadout.contains(QString::fromStdString(ps::fmt::percent(expectedTotal))))
        problems << QStringLiteral("readout \"%1\" does not show the recorded %2").arg(firstReadout).arg(expectedTotal);
    const int lines = first.count('\n');
    if (lines < 21)
        problems << QStringLiteral("CSV has %1 lines for 20 samples").arg(lines);
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
}

void TestGraphUpgrade::mainWindowSmoothOffStepsPerSampleKeepsSampling()
{
    // Smooth Graphs off: no frame clock; charts move once per sample and
    // every sample is still recorded. On again: the frame clock is back. It
    // also stops for pause and hide and restarts afterwards.
    WindowRig r;
    QVERIFY(r.cpu);
    r.w->show();
    QVERIFY(QTest::qWaitForWindowExposed(r.w.get()));
    r.feed(70);
    QTest::qWait(50);
    QStringList problems;
    if (!r.w->isAnimating())
        problems << QStringLiteral("smooth on, shown, live: frame clock not running");
    r.w->setSmoothGraphs(false);
    if (r.w->isAnimating())
        problems << QStringLiteral("smooth off: frame clock still running");
    QTest::qWait(50);
    Activity act(r.w.get(), r.cpu);
    act.reset();
    const double end0 = r.cpu->viewEnd();
    QTest::qWait(700);
    const int idlePaints = act.paints;
    if (idlePaints > 2)
        problems << QStringLiteral("smooth off: %1 chart paints in 700 ms without a sample").arg(idlePaints);
    if (r.cpu->viewEnd() != end0)
        problems << QStringLiteral("smooth off: axis moved %1 s without a sample").arg(r.cpu->viewEnd() - end0);
    const std::size_t n0 = r.w->history().size();
    const uint64_t total0 = r.w->history().totalSamples();
    act.reset();
    r.feedOne();
    QTest::qWait(60);
    if (r.w->history().totalSamples() != total0 + 1)
        problems << QStringLiteral("smooth off: sample not recorded");
    if (act.paints < 1)
        problems << QStringLiteral("smooth off: chart not repainted for a new sample");
    if (!(r.cpu->viewEnd() >= r.w->history().latestTime()))
        problems << QStringLiteral("smooth off: axis %1 behind the newest sample %2").arg(r.cpu->viewEnd(), 0, 'f', 3)
                        .arg(r.w->history().latestTime(), 0, 'f', 3);
    if (r.cpu->isRevealing())
        problems << QStringLiteral("smooth off: new sample is being revealed (should appear at once)");
    Q_UNUSED(n0);
    r.w->setSmoothGraphs(true);
    if (!r.w->isAnimating())
        problems << QStringLiteral("smooth back on: frame clock not running");
    act.reset();
    QTest::qWait(1000);
    if (act.paints < 40)
        problems << QStringLiteral("smooth back on: only %1 paints per second").arg(act.paints);
    r.w->setPaused(true);
    if (r.w->isAnimating())
        problems << QStringLiteral("paused: frame clock running");
    r.w->setPaused(false);
    if (!r.w->isAnimating())
        problems << QStringLiteral("resumed: frame clock not running");
    r.w->hide();
    QTest::qWait(30);
    if (r.w->isAnimating())
        problems << QStringLiteral("hidden: frame clock running");
    r.w->show();
    QVERIFY(QTest::qWaitForWindowExposed(r.w.get()));
    QTest::qWait(30);
    if (!r.w->isAnimating())
        problems << QStringLiteral("shown again: frame clock not running");
    qInfo("smooth off: %d paints in 700 ms idle", idlePaints);
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
}

// ---------------------------------------------------------------------------
// Reveal of a new sample and partial repaints (TimeChart)

void TestGraphUpgrade::chartRevealStaysOnMeasuredSegment()
{
    // A new continuous sample is revealed along the straight measured
    // segment from its predecessor: the drawn tip never passes the new
    // sample, never leaves the segment (no spline overshoot), the fill ends
    // at the tip, it completes within the reveal time, and text values use
    // the new sample at once. Progress follows the view clock only.
    const QPalette pal = lightPalette();
    const QColor c = seriesColor(0, pal);
    ChartRig rig(ps::Unit::Percent, {{"s", QStringLiteral("S"), 0}}, 0, 100, 60, pal);
    ChartRig full(ps::Unit::Percent, {{"s", QStringLiteral("S"), 0}}, 0, 100, 60, pal);
    ps::History h(512, 1e9), hr(512, 1e9);
    for (int t = 0; t <= 59; ++t) {
        addSample(h, t, {{"s", 20}});
        addSample(hr, t, {{"s", kFar}});
    }
    addSample(hr, 60, {{"s", kFar}});
    rig.chart.setRevealEnabled(true);
    rig.chart.setViewEnd(60);
    rig.shot(h); // knows sample 59
    addSample(h, 60, {{"s", 80}});
    rig.chart.refresh();
    QVERIFY2(rig.chart.isRevealing(), "a new continuous sample with reveal enabled is not revealed");
    rig.chart.setCursorTime(60);
    const QString ro = rig.chart.readoutText();
    rig.chart.clearCursor();
    QStringList problems;
    if (!ro.contains(QString::fromStdString(ps::fmt::percent(80))))
        problems << QStringLiteral("readout during the reveal \"%1\" (expected the new 80 %)").arg(ro);

    ChartRig refRig(ps::Unit::Percent, {{"s", QStringLiteral("S"), 0}}, 0, 100, 60, pal);
    int lastGap = 1 << 30, distinct = 0, prevTip = -1;
    double completedAt = ps::kNaN;
    for (int k = 0; k <= 45; ++k) {
        const double e = 60 + k * 0.01;
        rig.chart.setViewEnd(e);
        full.chart.setViewEnd(e);
        refRig.chart.setViewEnd(e);
        const QImage img = render(rig.chart);
        const QImage whole = full.shot(h);
        const QImage ref = refRig.shot(hr);
        const auto [l1, tip] = strokeExtent(img, ref, c);
        const auto [l2, end] = strokeExtent(whole, ref, c);
        Q_UNUSED(l1);
        Q_UNUSED(l2);
        if (tip < 0 || end < 0) {
            problems << QStringLiteral("e=%1: no line").arg(e);
            break;
        }
        if (tip > end + 1)
            problems << QStringLiteral("e=%1: tip x=%2 beyond the newest sample x=%3").arg(e).arg(tip).arg(end);
        // The whole plot scrolls left with the axis end, so progress is the
        // distance still to go to the new sample, which may only shrink.
        if (end - tip > lastGap + 1)
            problems << QStringLiteral("e=%1: reveal went backwards (%2 px to go, was %3)").arg(e).arg(end - tip).arg(lastGap);
        // Every revealed stroke pixel lies on the complete segment.
        int off = 0;
        for (int x = std::max(0, tip - 40); x <= tip; ++x)
            for (int y = 0; y < img.height(); ++y)
                if (strokePixel(img.pixel(x, y), ref.pixel(x, y), c)) {
                    bool near = false;
                    for (int dy = -1; dy <= 1 && !near; ++dy)
                        for (int dx = -1; dx <= 1 && !near; ++dx) {
                            const int xx = std::clamp(x + dx, 0, img.width() - 1),
                                      yy = std::clamp(y + dy, 0, img.height() - 1);
                            near = fit(whole.pixel(xx, yy), ref.pixel(xx, yy), c).a > 0.2;
                        }
                    off += !near;
                }
        if (off > 0)
            problems << QStringLiteral("e=%1: %2 revealed line pixels off the measured segment").arg(e).arg(off);
        // No fill to the right of the tip.
        const double beyond = tintShare(img, ref, c, tip + 3, end + 1, 30, img.height() - 30);
        if (tip + 3 <= end && beyond > 0.01)
            problems << QStringLiteral("e=%1: %2% fill beyond the tip").arg(e).arg(beyond * 100, 0, 'f', 1);
        if (end - tip != prevTip)
            ++distinct;
        prevTip = end - tip;
        lastGap = std::min(lastGap, end - tip);
        if (std::isnan(completedAt) && tip >= end - 1 && !rig.chart.isRevealing())
            completedAt = e - 60;
        if (!problems.isEmpty())
            break;
    }
    qInfo("reveal: %d distinct tip positions, complete after %.2f s of view time", distinct, completedAt);
    if (!(completedAt <= 0.35))
        problems << QStringLiteral("reveal not complete after 0.35 s of view time (%1)").arg(completedAt);
    if (distinct < 4)
        problems << QStringLiteral("reveal jumped: only %1 distinct tip positions").arg(distinct);

    // Discontinuous or unavailable samples appear at once.
    rig.chart.setViewEnd(61);
    addSample(h, 61, {{"s", 50}}, false);
    rig.chart.refresh();
    if (rig.chart.isRevealing())
        problems << QStringLiteral("a sample after a discontinuity is revealed from the one before it");
    rig.chart.setRevealEnabled(false);
    rig.chart.setViewEnd(62);
    addSample(h, 62, {{"s", 70}});
    rig.chart.refresh();
    if (rig.chart.isRevealing())
        problems << QStringLiteral("reveal disabled but a sample is revealed");
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
}

namespace {
// The pixels a shown top-level widget really has on screen (its backing
// store), in device pixels.
QImage onScreen(QWidget& w)
{
    // The raster backing store is exactly what was painted, in device pixels.
    if (QBackingStore* store = w.backingStore(); store && store->paintDevice() &&
                                                 store->paintDevice()->devType() == QInternal::Image) {
        const QImage& img = *static_cast<QImage*>(store->paintDevice());
        const QRect r(w.mapTo(w.window(), QPoint(0, 0)) * w.devicePixelRatioF(),
                      (QSizeF(w.size()) * w.devicePixelRatioF()).toSize());
        return img.copy(r).convertToFormat(QImage::Format_RGB32);
    }
    QScreen* screen = w.screen();
    if (!screen)
        return {};
    return screen->grabWindow(w.winId()).toImage().convertToFormat(QImage::Format_RGB32);
}
QImage fullRender(TimeChart& c, qreal dpr)
{
    QImage img((QSizeF(c.size()) * dpr).toSize(), QImage::Format_RGB32);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::magenta);
    c.render(&img);
    return img;
}
int countDiff(const QImage& a, const QImage& b, int* maxDiff)
{
    *maxDiff = 0;
    if (a.size() != b.size())
        return -1;
    int n = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            const int d = std::max({std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                                    std::abs(qBlue(p) - qBlue(q))});
            if (d > 0) {
                ++n;
                *maxDiff = std::max(*maxDiff, d);
            }
        }
    return n;
}
void flushPaints(QWidget& w)
{
    for (int k = 0; k < 3; ++k) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
    QTest::qWait(6);
    QCoreApplication::processEvents();
    Q_UNUSED(w);
}
} // namespace

void TestGraphUpgrade::chartPartialRepaintMatchesFullRender_data()
{
    QTest::addColumn<QString>("scenario");
    for (const char* s : {"scroll", "cursor ages out", "hover cursor snaps", "gaps", "sparse", "series change",
                          "palette change", "new sample reveal", "long window"})
        QTest::newRow(s) << QString::fromLatin1(s);
}

void TestGraphUpgrade::chartPartialRepaintMatchesFullRender()
{
    // Frames between samples repaint only parts of the plot from cached
    // images. After every frame the screen must equal a complete paint:
    //  - exactly, of a hidden twin that went through the same frames (any
    //    stale pixel left by a too-small repaint region shows here);
    //  - within antialiasing, of a fresh chart painted once at the same axis
    //    end (the cached images are drawn at 1/4 or 1/8 pixel phases, so an
    //    edge may differ by a few levels, but no line, fill, gap or colour
    //    may differ visibly).
    // The fresh comparison is made only while the screen shows no readout
    // cursor. The cursor, its markers and its box are composited over the
    // cached phase image; a fresh chart draws them over a cold paint at the
    // exact axis end, so the antialiased line pixels next to the markers are
    // blended from different backgrounds and the two are not equivalent
    // references. Cursor frames (hover, a set cursor, the old cursor being
    // erased, the frame the hovered sample switches) are held to the exact
    // same-frames paint instead, with the readout naming the same sample.
    // (A set cursor whose sample leaves the window moves to the oldest
    // visible sample, so it stays on screen.)
    // Cursor, gaps, sparse data, series and palette changes, a new sample's
    // reveal and a long window are covered; fractional scales run in
    // chartPartialRepaintAtFractionalScales.
    QFETCH(QString, scenario);
    ps::History h(1024, 1e9);
    auto value = [](double t) { return 40 + 4 * std::sin(t * 0.7); };
    const bool sparse = scenario == QLatin1String("sparse");
    const bool gaps = scenario == QLatin1String("gaps");
    for (int t = 0; t <= 100; t += sparse ? 7 : 1) {
        double v = value(t);
        if (gaps && (t == 60 || t == 61 || t == 85))
            v = ps::kNaN;
        addSample(h, t, {{"a", v}, {"b", v - 15}}, !(gaps && t == 75));
    }
    const std::vector<ChartSeries> one {{"a", QStringLiteral("A"), 0}};
    const std::vector<ChartSeries> two {{"a", QStringLiteral("A"), 0}, {"b", QStringLiteral("B"), 1}};
    const bool cursor = scenario == QLatin1String("cursor ages out");
    // The mouse rests over the plot while it scrolls: the readout jumps to
    // the next sample (about 10 px) whenever that one becomes the nearest,
    // so the old cursor must be erased where no other change repaints.
    const bool hover = scenario == QLatin1String("hover cursor snaps");
    auto hoverAt = [](TimeChart& c) {
        const QPointF pos(c.plotArea().left() + c.plotArea().width() * 0.3, c.plotArea().center().y() + 40);
        QMouseEvent move(QEvent::MouseMove, pos, c.mapToGlobal(pos), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&c, &move);
    };
    const bool seriesChange = scenario == QLatin1String("series change");
    const bool paletteChange = scenario == QLatin1String("palette change");
    const bool reveal = scenario == QLatin1String("new sample reveal");
    auto configure = [&](TimeChart& c) {
        c.setFocusPolicy(Qt::NoFocus); // no focus frame on the shown chart only
        c.setPalette(lightPalette());
        c.resize(640, 320);
        c.setSeries(one);
        c.setFixedRange(0, 100);
        c.setWindowSeconds(scenario == QLatin1String("long window") ? 900 : 60);
        c.setHistory(&h);
        c.setRevealEnabled(reveal);
        c.setViewEnd(100.2);
        c.refresh();
        if (cursor)
            c.setCursorTime(41.5); // leaves the 60 s window at axis end 101.5
    };
    TimeChart chart(QStringLiteral("Test chart"), ps::Unit::Percent);
    TimeChart twin(QStringLiteral("Test chart"), ps::Unit::Percent);
    configure(chart);
    configure(twin);
    chart.show();
    QVERIFY(QTest::qWaitForWindowExposed(&chart));
    flushPaints(chart);
    const qreal dpr = chart.devicePixelRatioF();
    if (hover) {
        fullRender(twin, dpr); // lays out the plot the mouse position refers to
        hoverAt(chart);
        hoverAt(twin);
        flushPaints(chart);
    }
    QStringList problems;
    int paintedFrames = 0, exactMismatch = 0, freshCompared = 0, freshMaxDiff = 0;
    int pendingLag = -1, pendingPixels = 0, oneFrameLags = 0, lagPixels = 0, edgeFlips = 0;
    double shownEnd = 100.2; // axis end of the chart's last actual paint
    // Whether the chart's last actual paint drew a readout cursor.
    bool cursorShown = !chart.readoutText().isEmpty();
    int cursorFrames = 0;
    QSet<QString> hoverSamples;
    for (int k = 0; k <= 90; ++k) {
        const double e = 100.2 + k / 60.0;
        for (TimeChart* c : {&chart, &twin}) {
            if (k == 30 && seriesChange)
                c->setSeries(two);
            if (k == 30 && paletteChange)
                c->setPalette(darkPalette());
            if (k == 20 && reveal && c == &chart)
                addSample(h, 100.5, {{"a", 70}, {"b", 55}}); // arrives at axis end 100.533
            if (k == 20 && reveal)
                c->refresh();
        }
        const int p0 = chart.paintCount();
        chart.advanceView(e);
        twin.advanceView(e);
        flushPaints(chart);
        const bool painted = chart.paintCount() != p0;
        paintedFrames += painted;
        if (hover || cursor) // the values under the "N s ago" line name the sample
            hoverSamples.insert(chart.readoutText().section(QLatin1Char('\n'), 1));
        if (painted) {
            shownEnd = e;
            cursorShown = !chart.readoutText().isEmpty();
        }
        cursorFrames += cursorShown;
        if (chart.readoutText() != twin.readoutText() && problems.size() < 8)
            problems << QStringLiteral("frame %1: the readout \"%2\" differs from the same-frames chart's \"%3\"")
                            .arg(k).arg(chart.readoutText(), twin.readoutText());
        const QImage screen = onScreen(chart);
        const QImage same = fullRender(twin, dpr).convertToFormat(QImage::Format_RGB32);
        int maxDiff = 0;
        const int n = countDiff(screen, same, &maxDiff);
        // A frame the chart skipped may lag by that one frame if the next one
        // repaints it exactly (checked when the next frame arrives).
        if (pendingLag >= 0) {
            if (painted && n == 0)
                ++oneFrameLags;
            else if (problems.size() < 8)
                problems << QStringLiteral("frame %1 was not repainted (%2 pixels stale) and frame %3 did not catch up (%4 "
                                           "pixels differ, %5)").arg(pendingLag).arg(pendingPixels).arg(k).arg(n)
                                .arg(painted ? "repainted" : "not repainted");
            pendingLag = -1;
        }
        if (n != 0 && !painted && k < 90) {
            pendingLag = k;
            pendingPixels = n;
            lagPixels = std::max(lagPixels, n);
        } else if (n != 0) {
            ++exactMismatch;
            if (!qEnvironmentVariableIsEmpty("PS_GU_DUMP")) {
                const QString base = qEnvironmentVariable("PS_GU_DUMP") + QStringLiteral("/%1-%2").arg(scenario).arg(k);
                screen.save(base + QStringLiteral("-screen.png"));
                same.save(base + QStringLiteral("-twin.png"));
            }
            if (problems.size() < 6)
                problems << QStringLiteral("frame %1 (axis end %2, %3): %4 pixels differ from a complete paint after "
                                           "the same frames, max channel difference %5 (screen %6x%7, paint %8x%9)")
                                .arg(k).arg(e, 0, 'f', 4).arg(painted ? "repainted" : "not repainted").arg(n).arg(maxDiff)
                                .arg(screen.width()).arg(screen.height()).arg(same.width()).arg(same.height());
        }
        // Fresh charts, for cursor-free frames (a reveal in progress has no
        // fresh equivalent either). The cached images are drawn at the
        // nearest 1/4 or 1/8 device pixel, so the screen must match a fresh
        // paint shifted by at most 1/8 pixel.
        const bool revealing = reveal && k >= 20 && k < 20 + 0.3 * 60 + 2;
        if (k % 3 == 0 && !revealing && !cursorShown) {
            const double pps = chart.plotArea().width() * dpr / chart.windowSeconds();
            // Per pixel: the smallest difference from any fresh paint within
            // 1/8 pixel (edges and text sit at slightly different phases).
            std::vector<int> least(std::size_t(screen.width()) * std::size_t(screen.height()), 256);
            QImage ref0;
            // PS_GU_SHIFTS widens the search for diagnosis only (default 4/32).
            const int maxShift = qEnvironmentVariableIsSet("PS_GU_SHIFTS") ? qEnvironmentVariableIntValue("PS_GU_SHIFTS") : 4;
            for (int shift = -maxShift; shift <= maxShift; ++shift) {
                TimeChart fresh(QStringLiteral("Test chart"), ps::Unit::Percent);
                configure(fresh);
                if (seriesChange && k >= 30)
                    fresh.setSeries(two);
                if (paletteChange && k >= 30)
                    fresh.setPalette(darkPalette());
                fresh.setRevealEnabled(false);
                // A skipped frame still shows its last painted axis end (the
                // lag itself is checked against the next frame above).
                fresh.setViewEnd(shownEnd + shift / (32.0 * pps));
                fresh.refresh();
                // A phase shift may move a cursor at the window's edge back
                // in: that paint is not a cursor-free reference.
                if (!fresh.readoutText().isEmpty()) {
                    if (shift == 0 && problems.size() < 8)
                        problems << QStringLiteral("frame %1: a fresh chart at the shown axis end has a cursor (\"%2\") "
                                                   "the screen does not").arg(k).arg(fresh.readoutText());
                    continue;
                }
                const QImage ref = fullRender(fresh, dpr).convertToFormat(QImage::Format_RGB32);
                if (ref.size() != screen.size())
                    continue;
                for (int y = 0; y < ref.height(); ++y)
                    for (int x = 0; x < ref.width(); ++x) {
                        const QRgb p = screen.pixel(x, y), q = ref.pixel(x, y);
                        int& m = least[std::size_t(y) * std::size_t(ref.width()) + std::size_t(x)];
                        m = std::min(m, std::max({std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                                                  std::abs(qBlue(p) - qBlue(q))}));
                    }
                if (shift == 0)
                    ref0 = ref;
                if (shift == 0 && !qEnvironmentVariableIsEmpty("PS_GU_DUMP"))
                    ref.save(qEnvironmentVariable("PS_GU_DUMP") + QStringLiteral("/%1-%2-fresh.png").arg(scenario).arg(k));
            }
            int best = 0, bestMax = 0;
            QRect where;
            // The columns at the plot's clip edges combine a fixed clip with a
            // shifted line, which no single fresh phase reproduces.
            const QRectF plot = chart.plotArea();
            const int clipL = int(std::floor(plot.left() * dpr)), clipR = int(std::ceil(plot.right() * dpr));
            for (std::size_t i = 0; i < least.size(); ++i) {
                const int x = int(i % std::size_t(screen.width()));
                const bool clipEdge = std::abs(x - clipL) <= 1 || std::abs(x - clipR) <= 1;
                int d = least[i];
                bool edgeSeam = false;
                // Unantialiased (snapped) vertical edges, such as gap shading,
                // may land one device column over: accept the fresh pixel
                // beside it, only where the fresh paint has a vertical edge.
                if (d > 12 && !ref0.isNull() && x > 0 && x + 1 < ref0.width()) {
                    const int y = int(i / std::size_t(screen.width()));
                    const QRgb l = ref0.pixel(x - 1, y), r = ref0.pixel(x + 1, y), p = screen.pixel(x, y);
                    auto dist = [](QRgb u, QRgb v) {
                        return std::max({std::abs(qRed(u) - qRed(v)), std::abs(qGreen(u) - qGreen(v)),
                                         std::abs(qBlue(u) - qBlue(v))});
                    };
                    // At such an edge the aliased band and the antialiased fill
                    // may also round apart, leaving a base-coloured seam in one
                    // column (15 levels at DPR 1.25 in the gaps row, identical
                    // in the complete paint): allow 16 levels in that column.
                    if (dist(l, r) > 12 && (std::min(dist(p, l), dist(p, r)) <= 4 || d <= 16)) {
                        d = std::min({d, dist(p, l), dist(p, r)});
                        edgeSeam = true;
                        ++edgeFlips;
                    }
                }
                bestMax = std::max(bestMax, d);
                if (d > (clipEdge ? 48 : edgeSeam ? 16 : 12)) {
                    ++best;
                    where |= QRect(int(i % std::size_t(screen.width())), int(i / std::size_t(screen.width())), 1, 1);
                }
            }
            if (best != 0 && !qEnvironmentVariableIsEmpty("PS_GU_DUMP"))
                screen.save(qEnvironmentVariable("PS_GU_DUMP") + QStringLiteral("/%1-%2-screen.png").arg(scenario).arg(k));
            ++freshCompared;
            freshMaxDiff = std::max(freshMaxDiff, bestMax);
            if (best != 0 && problems.size() < 8)
                problems << QStringLiteral("frame %1 (axis end %2): %3 pixels in %4,%5 %6x%7 differ by more than 12 "
                                           "levels (max %8) from every fresh paint within 1/8 pixel")
                                .arg(k).arg(e, 0, 'f', 4).arg(best).arg(where.x()).arg(where.y()).arg(where.width())
                                .arg(where.height()).arg(bestMax);
        }
    }
    if ((hover || cursor) && (hoverSamples.size() < 2 || hoverSamples.contains(QString())))
        problems << QStringLiteral("fixture: the cursor readout showed %1 samples (%2)")
                        .arg(hoverSamples.size()).arg(QStringList(hoverSamples.values()).join(QStringLiteral(" | ")));
    qInfo("%s at DPR %.2f: %d of 91 frames repainted, %d differ from the same-frames paint, %d skipped frames lagged "
          "one frame (at most %d pixels); %d frames showed a cursor (same-frames reference only); %d fresh comparisons "
          "of cursor-free frames, largest per-pixel difference from the nearest 1/8 px phase %d levels, %d "
          "snapped-edge pixels one column over",
          qPrintable(scenario), dpr, paintedFrames, exactMismatch, oneFrameLags, lagPixels, cursorFrames, freshCompared,
          freshMaxDiff, edgeFlips);
    // The fixture must exercise what each row is for.
    if ((hover || cursor) && cursorFrames < 60)
        problems << QStringLiteral("fixture: only %1 of 91 frames showed the cursor").arg(cursorFrames);
    if (!hover && !cursor && freshCompared < 20)
        problems << QStringLiteral("fixture: only %1 cursor-free frames were compared with fresh paints").arg(freshCompared);
    if (scenario != QLatin1String("long window") && paintedFrames < 60)
        problems << QStringLiteral("only %1 of 91 frames repainted while the view scrolled").arg(paintedFrames);
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
}

void TestGraphUpgrade::chartPartialRepaintAtFractionalScales_data()
{
    QTest::addColumn<QString>("scale");
    for (const char* s : {"1.25", "1.5", "2"})
        QTest::newRow(s) << QString::fromLatin1(s);
}

void TestGraphUpgrade::chartPartialRepaintAtFractionalScales()
{
    // The same pixel identity at fractional and integer device pixel ratios
    // (a child run of this test binary with QT_SCALE_FACTOR set).
    QFETCH(QString, scale);
    if (qEnvironmentVariableIsSet("PS_GU_CHILD"))
        QSKIP("child run");
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_SCALE_FACTOR"), scale);
    env.insert(QStringLiteral("PS_GU_CHILD"), QStringLiteral("1"));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::MergedChannels);
    // Measured child runs: 76/114/157 s (scale 1.25/1.5/2) in a normal
    // build, 270/615/951 s under AddressSanitizer. PS_GU_CHILD_TIMEOUT_S
    // overrides the limit.
#if defined(__SANITIZE_ADDRESS__)
    constexpr int defaultLimit = 1500;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    constexpr int defaultLimit = 1500;
#else
    constexpr int defaultLimit = 600;
#endif
#else
    constexpr int defaultLimit = 600;
#endif
    const int limit = qEnvironmentVariableIntValue("PS_GU_CHILD_TIMEOUT_S") > 0
                          ? qEnvironmentVariableIntValue("PS_GU_CHILD_TIMEOUT_S") : defaultLimit;
    QElapsedTimer clock;
    clock.start();
    p.start(QCoreApplication::applicationFilePath(), {QStringLiteral("chartPartialRepaintMatchesFullRender")});
    QVERIFY(p.waitForStarted(5000));
    if (!p.waitForFinished(limit * 1000)) {
        // Stop this child only; its output so far says where it was.
        p.kill();
        p.waitForFinished(10000);
        const QStringList lines = QString::fromLocal8Bit(p.readAll()).split(QLatin1Char('\n'));
        const QStringList tail = lines.sliced(std::max<qsizetype>(0, lines.size() - 8));
        QFAIL(qPrintable(QStringLiteral("child run at QT_SCALE_FACTOR=%1 did not finish in %2 s and was killed; last "
                                        "output:\n%3").arg(scale).arg(limit).arg(tail.join(QLatin1Char('\n')))));
    }
    const QString out = QString::fromLocal8Bit(p.readAll());
    QStringList relevant;
    for (const QString& line : out.split(QLatin1Char('\n')))
        if (line.startsWith(QLatin1String("FAIL")) || line.startsWith(QLatin1String("QINFO")) ||
            line.startsWith(QLatin1String("  frame")) || line.startsWith(QLatin1String("Totals")) ||
            line.contains(QLatin1String("pixels differ")))
            relevant << line;
    qInfo("QT_SCALE_FACTOR=%s (%.0f s of %d s allowed):\n%s", qPrintable(scale), clock.elapsed() / 1000.0, limit,
          qPrintable(relevant.join(QLatin1Char('\n'))));
    QVERIFY2(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0,
             qPrintable(QStringLiteral("child exit %1").arg(p.exitCode())));
    QVERIFY2(out.contains(QStringLiteral("DPR %1").arg(scale.toDouble(), 0, 'f', 2)), "child did not run at that scale");
}

// ---------------------------------------------------------------------------
// Review regressions (GRAPH-UPGRADE-3)

void TestGraphUpgrade::metricButtonOffWindowSpikeKeepsSegmentGeometry_data()
{
    QTest::addColumn<int>("unit");
    QTest::addColumn<double>("seconds");
    QTest::addColumn<double>("before"); // value of the sample just left of the window
    QTest::addColumn<double>("next");   // value of the first sample inside it
    QTest::addColumn<double>("later");  // level of every later sample (+ small ripple)
    QTest::addColumn<bool>("full");     // columns crossed by the segment: fully filled, else empty
    QTest::newRow("percent 60 s window, 1000 then 0") << int(ps::Unit::Percent) << 60.0 << 1000.0 << 0.0 << 0.6 << true;
    QTest::newRow("percent 10 s window, 1000 then 0") << int(ps::Unit::Percent) << 10.0 << 1000.0 << 0.0 << 0.6 << true;
    QTest::newRow("celsius 10 s window, -1000 then 20") << int(ps::Unit::Celsius) << 10.0 << -1000.0 << 20.0 << 20.0 << false;
    QTest::newRow("celsius 10 s window, 1000 then 20") << int(ps::Unit::Celsius) << 10.0 << 1000.0 << 20.0 << 20.0 << true;
}

void TestGraphUpgrade::metricButtonOffWindowSpikeKeepsSegmentGeometry()
{
    // The window (seconds long, ending at 65.1) starts just after a sample
    // far outside the autoscaled range; the next sample, 5 s later, is back
    // in range. The segment between them is the measured geometry: it
    // crosses the sparkline almost vertically next to the second sample.
    // Clamping the old sample to the visible range instead fabricates a
    // shallow 5 s ramp (and the area under it) that was never measured.
    QFETCH(int, unit);
    QFETCH(double, seconds);
    QFETCH(double, before);
    QFETCH(double, next);
    QFETCH(double, later);
    QFETCH(bool, full);
    const ps::Unit u = static_cast<ps::Unit>(unit);
    const double end = 65.1, start = end - seconds;
    const double tBefore = std::floor(start), tNext = tBefore + 5;
    const QPalette pal = lightPalette();
    const QColor c = seriesColor(0, pal);
    auto build = [&](ps::History& h, bool spike) {
        for (double t = tBefore - 5; t <= 65; t += 1) {
            double v = later + 0.4 * std::fabs(std::sin(t));
            if (spike && t < tBefore)
                v = later;
            if (spike && t == tBefore)
                v = before;
            if (spike && t > tBefore && t < tNext)
                continue; // no samples between the two
            if (spike && t == tNext)
                v = next;
            addSample(h, t, {{"s", v}}, true, u);
        }
    };
    auto shot = [&](const ps::History& h) {
        MetricButton b(QStringLiteral("Test"));
        b.setPalette(pal);
        b.setValueText(QStringLiteral("1.0"));
        b.setDetailText(QStringLiteral("detail"));
        b.resize(260, b.sizeHint().height());
        b.setSparkline(&h, {"s"}, 0, seconds);
        b.setSparklineVisuals({{0, Qt::SolidLine}});
        b.setViewEnd(end);
        QImage img(b.size(), QImage::Format_RGB32);
        img.fill(Qt::white);
        b.render(&img);
        return img;
    };
    ps::History ref(256, 1e9), h(256, 1e9);
    build(ref, false);
    build(h, true);
    const QImage refImg = shot(ref);
    const QImage img = shot(h);
    const QRgb white = QColor(Qt::white).rgb();
    const double hueSpread = std::max({c.red(), c.green(), c.blue()}) - std::min({c.red(), c.green(), c.blue()});
    auto hued = [&](QRgb p) {
        const Fit f = fit(p, white, c);
        if (!(f.a >= 0.025 && f.r <= 30))
            return false;
        const int hi = std::max({qRed(p), qGreen(p), qBlue(p)}), lo = std::min({qRed(p), qGreen(p), qBlue(p)});
        return hi - lo >= std::max(4.0, 0.5 * f.a * hueSpread);
    };
    // Sparkline extent from the reference (a line spanning the window).
    int xL = -1, xR = -1, yTop = img.height(), yBottom = -1;
    for (int x = 0; x < refImg.width(); ++x)
        for (int y = 0; y < refImg.height(); ++y)
            if (strokePixel(refImg.pixel(x, y), white, c)) {
                if (xL < 0)
                    xL = x;
                xR = x;
            }
    QVERIFY2(xL >= 0 && xR - xL > 40, qPrintable(QStringLiteral("reference sparkline %1..%2").arg(xL).arg(xR)));
    for (int y = 0; y < refImg.height(); ++y)
        if (hued(refImg.pixel((xL + xR) / 2, y))) {
            yTop = std::min(yTop, y);
            yBottom = std::max(yBottom, y);
        }
    QVERIFY(yBottom - yTop >= 8);
    const double pps = (xR - xL) / (65 - start); // xL is the window start, xR the sample at 65
    const int c0 = int(std::ceil(xL + (tBefore + 0.1 - start) * pps + 1.5));
    const int c1 = int(std::floor(xL + (tNext - start) * pps - 2.5));
    QVERIFY2(c1 >= c0, qPrintable(QStringLiteral("no columns between the samples (%1..%2)").arg(c0).arg(c1)));
    QStringList problems;
    for (int x = c0; x <= c1; ++x) {
        int strokes = 0, painted = 0, rows = 0;
        for (int y = yTop; y <= yBottom; ++y) {
            ++rows;
            strokes += strokePixel(img.pixel(x, y), white, c);
            painted += hued(img.pixel(x, y));
        }
        const double share = double(painted) / rows;
        if (strokes > 0)
            problems << QStringLiteral("column %1 (t=%2): %3 line pixels where the measured segment is outside the "
                                       "sparkline").arg(x).arg(start + (x - xL) / pps, 0, 'f', 2).arg(strokes);
        if (full && share < 0.9)
            problems << QStringLiteral("column %1 (t=%2): area covers %3% of the height (the segment is above the "
                                       "range: expected the full height)")
                            .arg(x).arg(start + (x - xL) / pps, 0, 'f', 2).arg(share * 100, 0, 'f', 0);
        if (!full && painted > 1)
            problems << QStringLiteral("column %1 (t=%2): %3 series pixels (the segment is below the range: expected "
                                       "none)").arg(x).arg(start + (x - xL) / pps, 0, 'f', 2).arg(painted);
    }
    qInfo("sparkline x %d..%d, rows %d..%d, checked columns %d..%d", xL, xR, yTop, yBottom, c0, c1);
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
}

void TestGraphUpgrade::chartLargeClockOffsetsSameAsSmall_data()
{
    QTest::addColumn<double>("offset");
    QTest::newRow("+2e7 s (231 days of uptime)") << 2e7;
    QTest::newRow("+1e8 s") << 1e8;
}

void TestGraphUpgrade::chartLargeClockOffsetsSameAsSmall()
{
    // Sample times are CLOCK_BOOTTIME seconds, beyond 1e7 after 116 days of
    // uptime. Shifting every time and the axis end by the same offset must
    // draw the same pixels and scroll with the same (cached, partial)
    // repaints: a large clock value is not a reason to repaint the whole
    // plot on every frame.
    QFETCH(double, offset);
    auto fill = [](ps::History& h, double base) {
        for (int t = 0; t <= 100; ++t) {
            const double v = t == 70 ? ps::kNaN : 42 + 2 * std::sin(t * 0.9);
            addSample(h, base + t, {{"a", v}}, t != 80);
        }
    };
    ps::History h0(512, 1e9), h1(512, 1e9);
    fill(h0, 0);
    fill(h1, offset);
    auto configure = [](TimeChart& c, const ps::History& h, double end) {
        c.setPalette(lightPalette());
        c.resize(640, 320);
        c.setSeries({{"a", QStringLiteral("A"), 0}});
        c.setFixedRange(0, 100);
        c.setWindowSeconds(60);
        c.setHistory(&h);
        c.setViewEnd(end);
        c.refresh();
    };
    QStringList problems;
    // Still images.
    for (double e : {100.3, 100.55, 131.0}) {
        TimeChart a(QStringLiteral("T"), ps::Unit::Percent), b(QStringLiteral("T"), ps::Unit::Percent);
        configure(a, h0, e);
        configure(b, h1, offset + e);
        int maxDiff = 0;
        const int n = countDiff(render(a), render(b), &maxDiff);
        // Double rounding of the shifted times may move an antialiased edge
        // by a fraction of a level on a few pixels; anything visible fails.
        if (n != 0)
            qInfo("axis end %.2f: %d pixels differ from the unshifted chart by at most %d levels", e, n, maxDiff);
        if (maxDiff > 2 || n > 20)
            problems << QStringLiteral("axis end %1: %2 pixels differ from the unshifted chart (max %3)").arg(e).arg(n).arg(maxDiff);
    }
    // Scrolling: paint regions per frame.
    struct Frames {
        int paints = 0;
        double area = 0, maxArea = 0;
        std::vector<QRect> firstRegions;
        double plotArea = 0;
    };
    auto scroll = [&](const ps::History& h, double base) {
        Frames f;
        TimeChart c(QStringLiteral("T"), ps::Unit::Percent);
        configure(c, h, base + 100.2);
        c.show();
        if (!QTest::qWaitForWindowExposed(&c))
            return f;
        flushPaints(c);
        struct Filter : QObject {
            Frames* f;
            bool eventFilter(QObject*, QEvent* e) override
            {
                if (e->type() == QEvent::Paint) {
                    double a = 0;
                    for (const QRect& r : static_cast<QPaintEvent*>(e)->region())
                        a += double(r.width()) * r.height();
                    ++f->paints;
                    if (f->paints == 30)
                        for (const QRect& r : static_cast<QPaintEvent*>(e)->region())
                            f->firstRegions.push_back(r);
                    f->area += a;
                    f->maxArea = std::max(f->maxArea, a);
                }
                return false;
            }
        } filter;
        filter.f = &f;
        c.installEventFilter(&filter);
        for (int k = 1; k <= 120; ++k) {
            c.advanceView(base + 100.2 + k / 60.0);
            flushPaints(c);
        }
        c.removeEventFilter(&filter);
        f.plotArea = c.plotArea().width() * c.plotArea().height();
        return f;
    };
    const Frames f0 = scroll(h0, 0), f1 = scroll(h1, offset);
    auto rects = [](const Frames& f) {
        QStringList l;
        for (const QRect& r : f.firstRegions)
            l << QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
        return l.join(QStringLiteral("; "));
    };
    qInfo("30th frame region: unshifted [%s]; shifted [%s]", qPrintable(rects(f0)), qPrintable(rects(f1)));
    qInfo("unshifted: %d paints, mean %.0f px, max %.0f px; shifted: %d paints, mean %.0f px, max %.0f px; plot %.0f px",
          f0.paints, f0.paints ? f0.area / f0.paints : 0, f0.maxArea, f1.paints, f1.paints ? f1.area / f1.paints : 0,
          f1.maxArea, f0.plotArea);
    if (f0.paints < 60)
        problems << QStringLiteral("fixture: unshifted chart painted only %1 of 120 frames").arg(f0.paints);
    if (std::abs(f1.paints - f0.paints) > std::max(6, f0.paints / 10))
        problems << QStringLiteral("shifted chart painted %1 frames, unshifted %2").arg(f1.paints).arg(f0.paints);
    const double m0 = f0.paints ? f0.area / f0.paints : 0, m1 = f1.paints ? f1.area / f1.paints : 0;
    if (m1 > 1.25 * m0 + 2000)
        problems << QStringLiteral("shifted chart repaints %1 px per frame, unshifted %2 px (full plot %3): cached "
                                   "partial repaint lost for a large clock value")
                        .arg(m1, 0, 'f', 0).arg(m0, 0, 'f', 0).arg(f0.plotArea, 0, 'f', 0);
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
}

namespace {
// Pixels of colour c (a strong stroke) in img over background bg.
int colourPixels(const QImage& img, QRgb bg, const QColor& c, QRect area = {})
{
    if (area.isNull())
        area = img.rect();
    int n = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x) {
            const Fit f = fit(img.pixel(x, y), bg, c);
            n += f.a >= 0.8 && f.r <= 20;
        }
    return n;
}
} // namespace

void TestGraphUpgrade::mainWindowSensorVisualsFollowIdentity()
{
    // A plotted sensor keeps its colour and pen style whatever else is
    // checked, unchecked, re-checked, hot-plugged or reordered: its retained
    // history is never recoloured as another sensor's. The navigation entry
    // draws the hottest sensor's trend in the colour the chart uses for that
    // same sensor. Colours follow palette changes.
    QTemporaryDir dir;
    TestClock clock;
    MainWindow w(windowOptions(dir.filePath(QStringLiteral("s.ini")), clock));
    w.setDetailsVisible(true);
    w.setPalette(lightPalette());
    w.resize(1180, 780);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* list = w.findChild<QTreeWidget*>(QStringLiteral("sensorList"));
    auto* chart = w.findChild<TimeChart*>(QStringLiteral("temperatureChart"));
    auto* nav = w.findChild<MetricButton*>(QStringLiteral("navTemperatures"));
    QVERIFY(list && chart && nav);
    const QString A = QStringLiteral("hwmon:k10temp:temp1"), B = QStringLiteral("hwmon:nvme:temp1"),
                  C = QStringLiteral("thermal:acpitz"), D = QStringLiteral("hwmon:acpi0:temp1");
    uint64_t seq = 0;
    bool withD = false, reversed = false;
    auto feed = [&](int n) {
        for (int i = 0; i < n; ++i) {
            clock.offset += 1; // one sample a second, each current when applied
            const double t = clock.now();
            auto temps = threeSensors(double(seq));
            if (withD)
                temps.push_back(tempReading(D.toStdString(), "acpi0", "aaa first", 35));
            if (reversed)
                std::reverse(temps.begin(), temps.end());
            w.applySnapshot(richSnap(++seq, t, temps));
        }
    };
    feed(1);
    auto check = [&](const QString& key, Qt::CheckState s) {
        auto* item = sensorItem(list, key);
        if (item)
            item->setCheckState(0, s);
        return item != nullptr;
    };
    QVERIFY(check(A, Qt::Checked) && check(B, Qt::Checked) && check(C, Qt::Unchecked));
    feed(10);
    auto visualOf = [&](const QString& key) -> std::pair<int, int> {
        const std::string k = ps::keys::temperature(key.toStdString());
        for (const auto& s : chart->series())
            if (s.key == k)
                return {s.visual, s.style ? int(*s.style) : -1};
        return {-2, -2};
    };
    const auto vA = visualOf(A), vB = visualOf(B);
    QStringList problems;
    if (vA.first < 0 || vB.first < 0)
        problems << QStringLiteral("fixture: A and B not both plotted");
    if (vA == vB)
        problems << QStringLiteral("A and B share colour and style");
    auto expect = [&](const QString& step, const QString& key, std::pair<int, int> v) {
        const auto now = visualOf(key);
        if (now != v)
            problems << QStringLiteral("%1: %2 colour/style changed from %3/%4 to %5/%6")
                            .arg(step, key).arg(v.first).arg(v.second).arg(now.first).arg(now.second);
    };
    // Navigation entry: names B (hottest) and draws it in B's chart colour.
    auto navColourCheck = [&](const QString& step, std::pair<int, int> vHot, std::pair<int, int> vOther) {
        QImage img(nav->size(), QImage::Format_RGB32);
        const QRgb bg = nav->palette().color(QPalette::Window).rgb();
        img.fill(bg);
        nav->render(&img);
        const QColor hot = seriesColor(vHot.first, nav->palette());
        const int nHot = colourPixels(img, bg, hot);
        int nOther = 0;
        if (vOther.first >= 0 && seriesColor(vOther.first, nav->palette()) != hot)
            nOther = colourPixels(img, bg, seriesColor(vOther.first, nav->palette()));
        if (!nav->detailText().contains(QStringLiteral("nvme Composite")))
            problems << QStringLiteral("%1: navigation entry names \"%2\"").arg(step, nav->detailText());
        if (nHot < 8 || nOther > nHot / 4)
            problems << QStringLiteral("%1: navigation trend of the hottest sensor B has %2 pixels in B's chart colour "
                                       "%3 and %4 in A's %5")
                            .arg(step).arg(nHot).arg(hot.name()).arg(nOther)
                            .arg(vOther.first >= 0 ? seriesColor(vOther.first, nav->palette()).name() : QString());
    };
    navColourCheck(QStringLiteral("A and B plotted"), vB, vA);

    QVERIFY(check(A, Qt::Unchecked));
    feed(2);
    expect(QStringLiteral("A unchecked"), B, vB);
    navColourCheck(QStringLiteral("A unchecked"), vB, {-1, -1});
    QVERIFY(check(A, Qt::Checked));
    feed(2);
    expect(QStringLiteral("A re-checked"), A, vA);
    expect(QStringLiteral("A re-checked"), B, vB);
    QVERIFY(check(C, Qt::Checked));
    feed(2);
    const auto vC = visualOf(C);
    expect(QStringLiteral("C checked"), A, vA);
    expect(QStringLiteral("C checked"), B, vB);
    if (vC == vA || vC == vB)
        problems << QStringLiteral("C shares a colour/style with A or B");
    withD = true;
    feed(2);
    if (auto* d = sensorItem(list, D))
        d->setCheckState(0, Qt::Checked);
    feed(1);
    expect(QStringLiteral("D hot-plugged"), A, vA);
    expect(QStringLiteral("D hot-plugged"), B, vB);
    expect(QStringLiteral("D hot-plugged"), C, vC);
    reversed = true;
    feed(2);
    expect(QStringLiteral("readings reordered"), A, vA);
    expect(QStringLiteral("readings reordered"), B, vB);
    expect(QStringLiteral("readings reordered"), C, vC);
    withD = false;
    feed(2);
    expect(QStringLiteral("D unplugged"), A, vA);
    expect(QStringLiteral("D unplugged"), B, vB);

    // Palette change: the chart draws B in the dark palette's colour for B.
    w.showPage(MainWindow::TemperaturePage);
    settle();
    auto chartHas = [&](const QPalette& pal, const QString& step) {
        QImage img(chart->size(), QImage::Format_RGB32);
        const QRgb bg = pal.color(QPalette::Base).rgb();
        img.fill(bg);
        chart->render(&img);
        const QColor mine = seriesColor(visualOf(B).first, pal);
        const int n = colourPixels(img, bg, mine);
        if (n < 20)
            problems << QStringLiteral("%1: chart has %2 pixels in B's colour %3").arg(step).arg(n).arg(mine.name());
        return n;
    };
    chartHas(lightPalette(), QStringLiteral("light palette"));
    w.setPalette(darkPalette());
    settle();
    chartHas(darkPalette(), QStringLiteral("dark palette"));
    const QColor lightB = seriesColor(vB.first, lightPalette());
    if (lightB != seriesColor(vB.first, darkPalette())) {
        QImage img(chart->size(), QImage::Format_RGB32);
        const QRgb bg = darkPalette().color(QPalette::Base).rgb();
        img.fill(bg);
        chart->render(&img);
        const int stale = colourPixels(img, bg, lightB);
        if (stale > 20)
            problems << QStringLiteral("dark palette: %1 pixels still in the light colour %2").arg(stale).arg(lightB.name());
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
}

void TestGraphUpgrade::mainWindowNineSensorsKeepVisualsAcrossChoices()
{
    // Nine listed sensors with retained history. A-H are plotted (eight
    // colours), I is not. Unchecking A (not the navigation sensor) keeps A's
    // identity reserved while it is listed: checking I then takes a new
    // colour/style, never A's, and re-checking A brings back A's own. No two
    // plotted sensors ever share colour and pen style, and nobody else is
    // restyled. The navigation trend keeps the hottest sensor's chart visual.
    QTemporaryDir dir;
    TestClock clock;
    MainWindow w(windowOptions(dir.filePath(QStringLiteral("s.ini")), clock));
    w.setDetailsVisible(true);
    w.resize(1180, 780);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* list = w.findChild<QTreeWidget*>(QStringLiteral("sensorList"));
    auto* chart = w.findChild<TimeChart*>(QStringLiteral("temperatureChart"));
    auto* nav = w.findChild<MetricButton*>(QStringLiteral("navTemperatures"));
    QVERIFY(list && chart && nav);
    QStringList keys;
    for (int n = 0; n < 9; ++n)
        keys << QStringLiteral("hwmon:chip%1:temp1").arg(n);
    const int H = 7; // hottest, stays the navigation sensor throughout
    uint64_t seq = 0;
    auto feed = [&](int count) {
        for (int i = 0; i < count; ++i) {
            clock.offset += 1;
            std::vector<ps::TemperatureReading> temps;
            for (int n = 0; n < 9; ++n)
                temps.push_back(tempReading(keys[n].toStdString(), "chip" + std::to_string(n), "S" + std::to_string(n),
                                            n == H ? 95.0 : 40.0 + n));
            w.applySnapshot(richSnap(++seq, clock.now(), temps));
        }
    };
    feed(2);
    QCOMPARE(list->topLevelItemCount(), 9);
    auto setChecked = [&](int n, bool on) {
        auto* item = sensorItem(list, keys[n]);
        if (item)
            item->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
        return item != nullptr;
    };
    QVERIFY(setChecked(8, false));
    for (int n = 0; n < 8; ++n)
        QVERIFY(setChecked(n, true));
    feed(3);
    using Visual = std::pair<int, int>;
    auto visualOf = [&](int n) -> Visual {
        const std::string k = ps::keys::temperature(keys[n].toStdString());
        for (const auto& cs : chart->series())
            if (cs.key == k)
                return {cs.visual, cs.style ? int(*cs.style) : int(Qt::SolidLine)};
        return {-1, -1};
    };
    QStringList problems;
    auto describe = [](Visual v) { return QStringLiteral("%1/%2").arg(v.first).arg(v.second); };
    auto distinctPlotted = [&](const QString& step) {
        std::map<Visual, int> seen;
        for (int n = 0; n < 9; ++n) {
            const Visual v = visualOf(n);
            if (v.first < 0)
                continue;
            if (auto it = seen.find(v); it != seen.end())
                problems << QStringLiteral("%1: S%2 and S%3 share colour/style %4")
                                .arg(step).arg(it->second).arg(n).arg(describe(v));
            seen[v] = n;
        }
    };
    auto navCheck = [&](const QString& step) {
        const auto vs = nav->sparklineVisuals();
        const Visual h = visualOf(H);
        if (vs.size() != 1 || Visual(vs[0].first, int(vs[0].second)) != h)
            problems << QStringLiteral("%1: navigation trend visual %2, the hottest sensor S7 is drawn as %3 (%4)")
                            .arg(step, vs.empty() ? QStringLiteral("none") : describe({vs[0].first, int(vs[0].second)}),
                                 describe(h), nav->detailText());
    };
    std::vector<Visual> v0(9);
    for (int n = 0; n < 8; ++n) {
        v0[n] = visualOf(n);
        if (v0[n].first < 0)
            problems << QStringLiteral("fixture: S%1 checked but not plotted").arg(n);
    }
    if (visualOf(8).first >= 0)
        problems << QStringLiteral("fixture: S8 plotted while unchecked");
    distinctPlotted(QStringLiteral("S0-S7 plotted"));
    navCheck(QStringLiteral("S0-S7 plotted"));
    auto keep = [&](const QString& step, int from, int to) {
        for (int n = from; n <= to; ++n)
            if (visualOf(n) != v0[n])
                problems << QStringLiteral("%1: S%2 restyled from %3 to %4").arg(step).arg(n).arg(describe(v0[n]),
                                                                                             describe(visualOf(n)));
    };

    QVERIFY(setChecked(0, false));
    feed(2);
    if (visualOf(0).first >= 0)
        problems << QStringLiteral("S0 unchecked but still plotted");
    keep(QStringLiteral("S0 unchecked"), 1, 7);
    navCheck(QStringLiteral("S0 unchecked"));

    QVERIFY(setChecked(8, true));
    feed(2);
    v0[8] = visualOf(8);
    if (v0[8].first < 0)
        problems << QStringLiteral("S8 checked but not plotted");
    else if (v0[8] == v0[0])
        problems << QStringLiteral("S8 (newly checked) took the colour/style %1 still reserved for listed, unchecked S0")
                        .arg(describe(v0[0]));
    keep(QStringLiteral("S8 checked"), 1, 7);
    distinctPlotted(QStringLiteral("S8 checked"));
    navCheck(QStringLiteral("S8 checked"));

    QVERIFY(setChecked(0, true));
    feed(2);
    keep(QStringLiteral("S0 re-checked"), 0, 8);
    distinctPlotted(QStringLiteral("S0 re-checked"));
    navCheck(QStringLiteral("S0 re-checked"));

    // The same after the chart is shown and rebuilt: nothing depends on
    // which page was visible when the choices changed.
    w.showPage(MainWindow::TemperaturePage);
    settle();
    QVERIFY(setChecked(3, false));
    feed(1);
    QVERIFY(setChecked(3, true));
    feed(1);
    keep(QStringLiteral("S3 unchecked and re-checked on its page"), 0, 8);
    distinctPlotted(QStringLiteral("S3 re-checked"));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
}

// ---------------------------------------------------------------------------
// Native Qt theme bootstrap

void TestGraphUpgrade::cliThemeDecisionPrecedence_data()
{
    QTest::addColumn<QString>("variable");
    QTest::addColumn<QString>("desktop");
    QTest::addColumn<QString>("argument"); // the -platformtheme value, empty for none
    QTest::addColumn<bool>("config");
    QTest::addColumn<bool>("plugin");
    QTest::addColumn<bool>("request");
    QTest::addColumn<bool>("missing");
    QTest::addColumn<QString>("noteMentions");
    QTest::newRow("Hyprland, qt6ct configured and installed") << "" << "Hyprland" << QString() << true << true << true << false << "qt6ct";
    QTest::newRow("Hyprland, configured, plugin missing") << "" << "Hyprland" << QString() << true << false << false << true << "libqt6ct";
    QTest::newRow("Hyprland, no qt6ct configuration") << "" << "Hyprland" << QString() << false << true << false << false << "default";
    QTest::newRow("variable set: kept") << "gtk3" << "Hyprland" << QString() << true << true << false << false << "QT_QPA_PLATFORMTHEME";
    QTest::newRow("argument given: kept") << "" << "Hyprland" << "ps-none" << true << true << false << false << "-platformtheme";
    QTest::newRow("argument and variable: the argument is what Qt uses")
        << "gtk3" << "Hyprland" << "qt6ct" << true << true << false << false << "-platformtheme";
    QTest::newRow("KDE desktop") << "" << "KDE" << QString() << true << true << false << false << "KDE";
    QTest::newRow("ubuntu:GNOME desktop") << "" << "ubuntu:GNOME" << QString() << true << true << false << false << "GNOME";
}

void TestGraphUpgrade::cliThemeDecisionPrecedence()
{
    QFETCH(QString, variable);
    QFETCH(QString, desktop);
    QFETCH(QString, argument);
    QFETCH(bool, config);
    QFETCH(bool, plugin);
    QFETCH(bool, request);
    QFETCH(bool, missing);
    QFETCH(QString, noteMentions);
    cli::ThemeEnvironment env;
    env.platformThemeVariable = variable;
    env.currentDesktop = desktop;
    env.themeArgument = !argument.isEmpty();
    env.themeArgumentValue = argument;
    env.qt6ctConfig = config ? QStringLiteral("/home/u/.config/qt6ct/qt6ct.conf") : QString();
    env.qt6ctPlugin = plugin ? QStringLiteral("/usr/lib/qt6/plugins/platformthemes/libqt6ct.so") : QString();
    const cli::ThemeDecision d = cli::chooseTheme(env);
    QCOMPARE(d.requestQt6ct, request);
    QCOMPARE(d.pluginMissing, missing);
    QVERIFY2(d.note.contains(noteMentions, Qt::CaseInsensitive),
             qPrintable(QStringLiteral("note \"%1\" does not mention %2").arg(d.note, noteMentions)));
}

namespace {
QString appPath()
{
    const QString env = qEnvironmentVariable("PS_GU_APP");
    if (!env.isEmpty())
        return env;
    const QString sibling = QCoreApplication::applicationDirPath() + QStringLiteral("/pulsescope");
    return QFileInfo(sibling).isExecutable() ? sibling : QString();
}
QByteArray fileHash(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return "<absent>";
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
}
struct ThemeInfo {
    int exitCode = -1;
    QString platform, theme, style, font, icons, palette, all, errors;
    QStringList plugins;
};
// Qt's gtk3 platform theme initialises GTK, GDK and Pango, which keep
// process-lifetime allocations that LeakSanitizer reports at exit (with
// fontconfig and glib frames below them). Only the runs meant to load that
// plugin get these library-specific suppressions; every other run, and this
// test process, keeps full leak checking.
QString gtkLeakSuppressions()
{
    static QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("gtk3-theme.lsan.supp"));
    if (dir.isValid() && !QFileInfo::exists(path)) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write("leak:libqgtk3.so\nleak:libgtk-3.so\nleak:libgdk-3.so\nleak:libpango-1.0.so\nleak:libpangoft2-1.0.so\n");
    }
    return QFileInfo::exists(path) ? path : QString();
}
ThemeInfo runThemeInfo(const QString& app, const QStringList& args, const std::map<QString, QString>& set,
                       const QStringList& unset, bool loadsGtk3 = false)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char* v : {"QT_QPA_PLATFORMTHEME", "QT_STYLE_OVERRIDE", "QT_QPA_PLATFORM", "QT_SCALE_FACTOR",
                          "QT_FONT_DPI", "PS_GU_CHILD"})
        env.remove(QString::fromLatin1(v));
    for (const QString& v : unset)
        env.remove(v);
    for (const auto& [k, v] : set)
        env.insert(k, v);
    if (loadsGtk3) {
        const QString supp = gtkLeakSuppressions();
        if (!supp.isEmpty()) {
            const QString before = env.value(QStringLiteral("LSAN_OPTIONS"));
            env.insert(QStringLiteral("LSAN_OPTIONS"),
                       (before.isEmpty() ? QString() : before + QLatin1Char(':')) + QStringLiteral("suppressions=") + supp);
            qInfo("gtk3 theme run: GTK/GDK/Pango leak reports suppressed in that child only (%s)", qPrintable(supp));
        }
    }
    QProcess p;
    p.setProcessEnvironment(env);
    p.start(app, QStringList(args) << QStringLiteral("--theme-info"));
    ThemeInfo info;
    if (!p.waitForStarted(5000) || !p.waitForFinished(30000)) {
        p.kill();
        p.waitForFinished(2000);
        info.all = QStringLiteral("did not finish");
        return info;
    }
    info.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
    info.all = QString::fromLocal8Bit(p.readAllStandardOutput());
    if (info.exitCode != 0) {
        const QStringList lines = QString::fromLocal8Bit(p.readAllStandardError()).trimmed().split(QLatin1Char('\n'));
        info.errors = lines.sliced(std::max<qsizetype>(0, lines.size() - 3)).join(QStringLiteral(" / "));
    }
    for (const QString& line : info.all.split(QLatin1Char('\n'))) {
        auto after = [&](const char* prefix) { return line.mid(int(std::strlen(prefix))).trimmed(); };
        if (line.startsWith(QLatin1String("platform:")))
            info.platform = after("platform:");
        else if (line.startsWith(QLatin1String("theme:")))
            info.theme = after("theme:");
        else if (line.startsWith(QLatin1String("style:")))
            info.style = after("style:");
        else if (line.startsWith(QLatin1String("font:")))
            info.font = after("font:");
        else if (line.startsWith(QLatin1String("icon theme:")))
            info.icons = after("icon theme:");
        else if (line.startsWith(QLatin1String("palette:")))
            info.palette = after("palette:");
        else if (line.startsWith(QLatin1String("loaded plugin:")))
            info.plugins << after("loaded plugin:");
    }
    return info;
}
bool loaded(const ThemeInfo& i, const QString& file)
{
    for (const QString& p : i.plugins)
        if (p.endsWith(QLatin1Char('/') + file))
            return true;
    return false;
}
} // namespace

// The theme a note says Qt uses: "Platform theme from <source> (<name>)..."
// or "Platform theme <name>, as configured...". Empty when the note names no
// theme in use (Qt chooses by itself, an argument is empty, and so on).
QString claimedTheme(const QString& note)
{
    static const QRegularExpression from(QStringLiteral("^Platform theme from [^(]*\\(([^)]*)\\)"));
    static const QRegularExpression named(QStringLiteral("^Platform theme ([^ ,(]+),"));
    if (const auto m = from.match(note); m.hasMatch())
        return m.captured(1);
    if (const auto m = named.match(note); m.hasMatch())
        return m.captured(1);
    return {};
}

void TestGraphUpgrade::cliThemeProbeReadsArgumentsAsQt_data()
{
    // Command lines and the platform theme Qt 6 really takes from them with
    // QT_QPA_PLATFORMTHEME=gtk3 (QGuiApplicationPrivate::createPlatformIntegration:
    // the last -platformtheme wins over the variable, even an empty one, which
    // leaves Qt to choose; options that take a value consume the next argument
    // even when it looks like an option; -title/-geometry/-icon take one only
    // on xcb; -qwindowtitle/-qwindowgeometry/-qwindowicon always). An empty
    // expected theme: Qt uses no named theme from the command line or variable.
    QTest::addColumn<QStringList>("args");
    QTest::addColumn<QString>("expected");
    const QString gtk3 = QStringLiteral("gtk3"), none;
    const QString pt = QStringLiteral("-platformtheme"), info = QStringLiteral("--theme-info");
    QTest::newRow("explicit empty") << QStringList {pt, QString(), info} << none;
    QTest::newRow("double-dash explicit empty") << QStringList {QStringLiteral("--platformtheme"), QString(), info} << none;
    QTest::newRow("empty after named") << QStringList {pt, QStringLiteral("qt6ct"), pt, QString(), info} << none;
    QTest::newRow("named after empty") << QStringList {pt, QString(), pt, QStringLiteral("qt6ct"), info}
                                       << QStringLiteral("qt6ct");
    for (const char* alias : {"-title", "-geometry", "-icon"})
        QTest::newRow(qPrintable(QStringLiteral("xcb %1 consumes -platformtheme").arg(QLatin1String(alias))))
            << QStringList {QStringLiteral("-platform"), QStringLiteral("xcb"), QString::fromLatin1(alias), pt, info} << gtk3;
    QTest::newRow("xcb -title with value, then theme")
        << QStringList {QStringLiteral("-platform"), QStringLiteral("xcb"), QStringLiteral("-title"), QStringLiteral("t"),
                        pt, QStringLiteral("qt6ct"), info}
        << QStringLiteral("qt6ct");
    QTest::newRow("wayland -title takes no value")
        << QStringList {QStringLiteral("-platform"), QStringLiteral("wayland"), QStringLiteral("-title"), pt, info} << info;
    QTest::newRow("-qwindowtitle consumes on any platform")
        << QStringList {QStringLiteral("-platform"), QStringLiteral("wayland"), QStringLiteral("-qwindowtitle"), pt, info}
        << gtk3;
    QTest::newRow("-platform consumes -platformtheme")
        << QStringList {QStringLiteral("-platform"), pt, info} << gtk3;
}

void TestGraphUpgrade::cliThemeProbeReadsArgumentsAsQt()
{
    // The probe reads the real argument vector (never this process's
    // environment variable: the variable is set on the probed value only).
    QFETCH(QStringList, args);
    QFETCH(QString, expected);
    std::vector<QByteArray> storage {QByteArrayLiteral("pulsescope")};
    for (const QString& a : args)
        storage.push_back(a.toLocal8Bit());
    std::vector<const char*> argv;
    for (const QByteArray& b : storage)
        argv.push_back(b.constData());
    argv.push_back(nullptr);
    cli::ThemeEnvironment env = cli::probeThemeEnvironment(int(storage.size()), argv.data());
    env.platformThemeVariable = QStringLiteral("gtk3");
    env.currentDesktop = QStringLiteral("Hyprland");
    env.qt6ctConfig = QStringLiteral("/nonexistent/qt6ct.conf");
    env.qt6ctPlugin = QStringLiteral("/nonexistent/libqt6ct.so");
    const cli::ThemeDecision d = cli::chooseTheme(env);
    const QString claimed = claimedTheme(d.note);
    qInfo("%s -> \"%s\"", qPrintable(args.join(QLatin1Char(' '))), qPrintable(d.note));
    QVERIFY2(!d.requestQt6ct, qPrintable(QStringLiteral("qt6ct requested over an explicit choice: %1").arg(d.note)));
    QVERIFY2(claimed == expected,
             qPrintable(QStringLiteral("Qt uses %1, the note claims %2: \"%3\"")
                            .arg(expected.isEmpty() ? QStringLiteral("no named theme") : expected,
                                 claimed.isEmpty() ? QStringLiteral("none") : claimed, d.note)));
    if (expected != QLatin1String("--theme-info"))
        QVERIFY2(!d.note.contains(QLatin1String("--theme-info")), qPrintable(d.note));
}

void TestGraphUpgrade::appThemeBootstrapUsesDesktopConfiguration()
{
    // The real startup path (a child process of the built application with
    // --theme-info): with qt6ct configured and QT_QPA_PLATFORMTHEME unset, the
    // app loads qt6ct for itself and gets the user's style, font, icons and
    // palette, read here independently from their configuration files. An
    // explicit variable, -platformtheme or style override wins and is named
    // truthfully. Configuration files are never written.
    const QString app = appPath();
    if (app.isEmpty())
        QSKIP("application binary not found (set PS_GU_APP)");
    const QString home = QDir::homePath();
    const QString configHome = qEnvironmentVariable("XDG_REAL_CONFIG_HOME", home + QStringLiteral("/.config"));
    const QString qt6ct = configHome + QStringLiteral("/qt6ct/qt6ct.conf");
    const QString kvantum = configHome + QStringLiteral("/Kvantum/kvantum.kvconfig");
    if (!QFileInfo(qt6ct).isFile())
        QSKIP("no qt6ct configuration on this machine");
    const QByteArray qt6ctHash = fileHash(qt6ct), kvantumHash = fileHash(kvantum);
    const QString envBefore = qEnvironmentVariable("QT_QPA_PLATFORMTHEME");

    QSettings conf(qt6ct, QSettings::IniFormat);
    const QString style = conf.value(QStringLiteral("Appearance/style")).toString();
    const QString icons = conf.value(QStringLiteral("Appearance/icon_theme")).toString();
    QVariant general = conf.value(QStringLiteral("Fonts/general"));
    const QString generalText = general.typeId() == QMetaType::QStringList ? general.toStringList().join(QLatin1Char(','))
                                                                           : general.toString();
    QFont expectedFont;
    const bool haveFont = !generalText.isEmpty() && expectedFont.fromString(generalText);
    QStringList expectedColours; // window, base, text from the Kvantum theme
    if (style.compare(QStringLiteral("kvantum"), Qt::CaseInsensitive) == 0 && QFileInfo(kvantum).isFile()) {
        const QString theme = QSettings(kvantum, QSettings::IniFormat).value(QStringLiteral("General/theme")).toString();
        for (const QString& dirName : {configHome + QStringLiteral("/Kvantum/"), QStringLiteral("/usr/share/Kvantum/")}) {
            const QString f = dirName + theme + QLatin1Char('/') + theme + QStringLiteral(".kvconfig");
            if (!QFileInfo(f).isFile())
                continue;
            QSettings kv(f, QSettings::IniFormat);
            for (const char* key : {"GeneralColors/window.color", "GeneralColors/base.color", "GeneralColors/text.color"})
                expectedColours << kv.value(QString::fromLatin1(key)).toString().left(7).toLower();
            break;
        }
    }
    const bool customScheme = conf.value(QStringLiteral("Appearance/custom_palette")).toBool() &&
                              !conf.value(QStringLiteral("Appearance/color_scheme_path")).toString().isEmpty();
    const std::map<QString, QString> base {{QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen")},
                                           {QStringLiteral("XDG_CURRENT_DESKTOP"), QStringLiteral("Hyprland")},
                                           {QStringLiteral("XDG_CONFIG_HOME"), configHome}};
    QStringList problems;
    auto describe = [](const ThemeInfo& i) {
        return QStringLiteral("[exit %1; platform %2; theme \"%3\"; style %4; font %5; icons %6; palette %7; plugins %8]")
            .arg(i.exitCode).arg(i.platform, i.theme, i.style, i.font, i.icons, i.palette, i.plugins.join(QLatin1Char(' '))) +
               (i.errors.isEmpty() ? QString() : QStringLiteral(" stderr: ") + i.errors);
    };

    // 1. Configured desktop, variable unset.
    const ThemeInfo user = runThemeInfo(app, {}, base, {});
    qInfo("user configuration: %s", qPrintable(describe(user)));
    if (user.exitCode != 0 || !loaded(user, QStringLiteral("libqt6ct.so")))
        problems << QStringLiteral("configured qt6ct not loaded: %1").arg(describe(user));
    if (!style.isEmpty() && style.compare(QStringLiteral("kvantum"), Qt::CaseInsensitive) == 0 &&
        !loaded(user, QStringLiteral("libkvantum.so")))
        problems << QStringLiteral("qt6ct style kvantum configured but libkvantum.so not loaded");
    if (haveFont && (!user.font.startsWith(expectedFont.family() + QLatin1Char(',')) ||
                     QFont().fromString(user.font) == false))
        problems << QStringLiteral("font %1, configured %2").arg(user.font, expectedFont.family());
    if (haveFont) {
        QFont got;
        got.fromString(user.font);
        if (got.pointSizeF() != expectedFont.pointSizeF())
            problems << QStringLiteral("font size %1, configured %2").arg(got.pointSizeF()).arg(expectedFont.pointSizeF());
    }
    if (!icons.isEmpty() && user.icons != icons)
        problems << QStringLiteral("icon theme %1, configured %2").arg(user.icons, icons);
    if (!customScheme && expectedColours.size() == 3) {
        const QStringList want {QStringLiteral("window ") + expectedColours[0], QStringLiteral("base ") + expectedColours[1],
                                QStringLiteral("text ") + expectedColours[2]};
        for (const QString& wnt : want)
            if (!user.palette.contains(wnt))
                problems << QStringLiteral("palette \"%1\" lacks %2 (from the Kvantum theme)").arg(user.palette, wnt);
    }

    // 2. Explicit variable: kept, and qt6ct not forced.
    const ThemeInfo var = runThemeInfo(app, {}, [&] { auto m = base; m[QStringLiteral("QT_QPA_PLATFORMTHEME")] = QStringLiteral("gtk3"); return m; }(), {},
                                       true);
    if (var.exitCode != 0 || loaded(var, QStringLiteral("libqt6ct.so")) || !var.theme.contains(QStringLiteral("QT_QPA_PLATFORMTHEME")))
        problems << QStringLiteral("QT_QPA_PLATFORMTHEME=gtk3 not respected or not named: %1").arg(describe(var));
    // 3. Explicit argument naming another theme.
    const ThemeInfo arg = runThemeInfo(app, {QStringLiteral("-platformtheme"), QStringLiteral("ps-none")}, base, {});
    if (arg.exitCode != 0 || loaded(arg, QStringLiteral("libqt6ct.so")) || !arg.theme.contains(QStringLiteral("-platformtheme")))
        problems << QStringLiteral("-platformtheme ps-none not respected or not named: %1").arg(describe(arg));
    // 4. Both: Qt uses the argument; the explanation must say so.
    const ThemeInfo both = runThemeInfo(app, {QStringLiteral("-platformtheme"), QStringLiteral("qt6ct")},
                                        [&] { auto m = base; m[QStringLiteral("QT_QPA_PLATFORMTHEME")] = QStringLiteral("gtk3"); return m; }(), {});
    if (both.exitCode != 0 || !loaded(both, QStringLiteral("libqt6ct.so")) || loaded(both, QStringLiteral("libqgtk3.so")))
        problems << QStringLiteral("-platformtheme qt6ct with QT_QPA_PLATFORMTHEME=gtk3: Qt did not use the argument: %1").arg(describe(both));
    else if (!both.theme.contains(QStringLiteral("-platformtheme")))
        problems << QStringLiteral("-platformtheme qt6ct with QT_QPA_PLATFORMTHEME=gtk3: qt6ct is loaded but the "
                                   "explanation says \"%1\"").arg(both.theme);
    // 5. A desktop with its own integration.
    const ThemeInfo kde = runThemeInfo(app, {}, [&] { auto m = base; m[QStringLiteral("XDG_CURRENT_DESKTOP")] = QStringLiteral("KDE"); return m; }(), {});
    if (kde.exitCode != 0 || loaded(kde, QStringLiteral("libqt6ct.so")))
        problems << QStringLiteral("KDE desktop: qt6ct forced: %1").arg(describe(kde));
    // 6. No qt6ct configuration.
    QTemporaryDir empty;
    const ThemeInfo none = runThemeInfo(app, {}, [&] { auto m = base; m[QStringLiteral("XDG_CONFIG_HOME")] = empty.path(); return m; }(), {});
    if (none.exitCode != 0 || loaded(none, QStringLiteral("libqt6ct.so")))
        problems << QStringLiteral("no qt6ct configuration: qt6ct loaded: %1").arg(describe(none));
    // 7. Style overrides win over the configured style.
    const ThemeInfo so = runThemeInfo(app, {}, [&] { auto m = base; m[QStringLiteral("QT_STYLE_OVERRIDE")] = QStringLiteral("Fusion"); return m; }(), {});
    if (so.exitCode != 0 || !so.style.contains(QStringLiteral("fusion"), Qt::CaseInsensitive))
        problems << QStringLiteral("QT_STYLE_OVERRIDE=Fusion: style %1").arg(so.style);
    const ThemeInfo sa = runThemeInfo(app, {QStringLiteral("-style"), QStringLiteral("Fusion")}, base, {});
    if (sa.exitCode != 0 || !sa.style.contains(QStringLiteral("fusion"), Qt::CaseInsensitive))
        problems << QStringLiteral("-style Fusion: style %1").arg(sa.style);
    // 8. The native platform, when there is a Wayland session.
    if (!qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()) {
        auto m = base;
        m.erase(QStringLiteral("QT_QPA_PLATFORM"));
        const ThemeInfo native = runThemeInfo(app, {}, m, {});
        qInfo("native: %s", qPrintable(describe(native)));
        if (native.exitCode != 0 || !loaded(native, QStringLiteral("libqt6ct.so")) || native.platform == QLatin1String("offscreen"))
            problems << QStringLiteral("native platform: %1").arg(describe(native));
    }
    // 9. An explicitly empty argument makes Qt ignore the variable.
    const auto withGtk3 = [&] { auto m = base; m[QStringLiteral("QT_QPA_PLATFORMTHEME")] = QStringLiteral("gtk3"); return m; }();
    const ThemeInfo cleared = runThemeInfo(app, {QStringLiteral("-platformtheme"), QString()}, withGtk3, {});
    qInfo("-platformtheme '' with QT_QPA_PLATFORMTHEME=gtk3: %s", qPrintable(describe(cleared)));
    if (cleared.exitCode != 0 || loaded(cleared, QStringLiteral("libqgtk3.so")) || loaded(cleared, QStringLiteral("libqt6ct.so")))
        problems << QStringLiteral("-platformtheme '' with QT_QPA_PLATFORMTHEME=gtk3: Qt did not clear the theme "
                                   "(fixture assumption): %1").arg(describe(cleared));
    // 10. On xcb, -title consumes "-platformtheme" as the window title: the
    // variable stays in force and --theme-info is still the app's option.
    ThemeInfo xcbTitle;
    const bool haveDisplay = !qEnvironmentVariable("DISPLAY").isEmpty();
    if (haveDisplay) {
        xcbTitle = runThemeInfo(app, {QStringLiteral("-platform"), QStringLiteral("xcb"), QStringLiteral("-title"),
                                      QStringLiteral("-platformtheme")}, withGtk3, {}, true);
        qInfo("-platform xcb -title -platformtheme with QT_QPA_PLATFORMTHEME=gtk3: %s", qPrintable(describe(xcbTitle)));
        if (xcbTitle.exitCode != 0 || xcbTitle.platform != QLatin1String("xcb") || !loaded(xcbTitle, QStringLiteral("libqgtk3.so")))
            problems << QStringLiteral("xcb -title -platformtheme: Qt did not keep gtk3 from the variable (fixture "
                                       "assumption): %1").arg(describe(xcbTitle));
    } else {
        qInfo("no DISPLAY: the xcb alias case is not run");
    }
    // Every explanation names the theme Qt really loaded, or none.
    const std::vector<std::pair<QString, const ThemeInfo*>> runs {
        {QStringLiteral("configured"), &user}, {QStringLiteral("variable gtk3"), &var}, {QStringLiteral("argument ps-none"), &arg},
        {QStringLiteral("argument qt6ct over variable"), &both}, {QStringLiteral("KDE"), &kde}, {QStringLiteral("no config"), &none},
        {QStringLiteral("empty argument over variable"), &cleared}, {QStringLiteral("xcb -title alias"), haveDisplay ? &xcbTitle : nullptr}};
    for (const auto& [name, run] : runs) {
        if (!run || run->exitCode != 0)
            continue;
        const QString claimed = claimedTheme(run->theme);
        QStringList themePlugins;
        for (const QString& plugin : run->plugins)
            if (plugin.contains(QLatin1String("/platformthemes/")))
                themePlugins << QFileInfo(plugin).fileName();
        const QString expectedPlugin = claimed == QLatin1String("gtk3") ? QStringLiteral("libqgtk3.so")
                                     : claimed.isEmpty()                ? QString()
                                                                        : QStringLiteral("libq%1.so").arg(claimed);
        const bool knownPlugin = claimed == QLatin1String("gtk3") || claimed == QLatin1String("qt6ct");
        if (knownPlugin && !themePlugins.contains(claimed == QLatin1String("qt6ct") ? QStringLiteral("libqt6ct.so") : expectedPlugin))
            problems << QStringLiteral("%1: the explanation claims %2 but Qt loaded %3: \"%4\"")
                            .arg(name, claimed, themePlugins.isEmpty() ? QStringLiteral("no theme plugin") : themePlugins.join(QLatin1Char(' ')),
                                 run->theme);
        if (run->theme.contains(QLatin1String("--theme-info")))
            problems << QStringLiteral("%1: the explanation takes --theme-info for a theme: \"%2\"").arg(name, run->theme);
    }
    if (fileHash(qt6ct) != qt6ctHash || fileHash(kvantum) != kvantumHash)
        problems << QStringLiteral("a theme configuration file changed during the runs");
    if (qEnvironmentVariable("QT_QPA_PLATFORMTHEME") != envBefore)
        problems << QStringLiteral("this process's QT_QPA_PLATFORMTHEME changed");
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
}

QTEST_MAIN(TestGraphUpgrade)
#include "tst_graph_upgrade.moc"
