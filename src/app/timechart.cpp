#include "app/timechart.h"

#include "core/format.h"

#include <QFocusEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QVarLengthArray>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {

Qt::PenStyle cycleStyle(int index)
{
    static const std::array<Qt::PenStyle, 4> styles = {Qt::SolidLine, Qt::DashLine, Qt::DotLine, Qt::DashDotLine};
    const int i = index < 0 ? 0 : index;
    return styles[static_cast<std::size_t>((i + i / 8) % 4)];
}

constexpr double kMargin = 8;
constexpr double kSwatch = 22;
// Longest reveal of a new sample (never more than half its interval).
constexpr double kRevealSeconds = 0.3;
// Vertical edges below the lines beyond which frames repaint all data rows.
constexpr std::size_t kMaxEdges = 8;

long long floorDiv(long long a, long long b)
{
    const long long q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

// Qt's dash patterns, in pen widths.
struct DashPattern {
    const double* lengths;
    int count;
};
DashPattern dashPattern(Qt::PenStyle style)
{
    static constexpr double dash[] = {4, 2};
    static constexpr double dot[] = {1, 2};
    static constexpr double dashDot[] = {4, 2, 1, 2};
    static constexpr double dashDotDot[] = {4, 2, 1, 2, 1, 2};
    switch (style) {
    case Qt::DotLine: return {dot, 2};
    case Qt::DashDotLine: return {dashDot, 4};
    case Qt::DashDotDotLine: return {dashDotDot, 6};
    default: return {dash, 2};
    }
}

QColor blend(const QColor& a, const QColor& b, double t)
{
    return QColor::fromRgbF(static_cast<float>(a.redF() + (b.redF() - a.redF()) * t),
                            static_cast<float>(a.greenF() + (b.greenF() - a.greenF()) * t),
                            static_cast<float>(a.blueF() + (b.blueF() - a.blueF()) * t));
}

int visualOf(const ChartSeries& cs, std::size_t position)
{
    return cs.visual >= 0 ? cs.visual : static_cast<int>(position);
}

Qt::PenStyle styleOf(const ChartSeries& cs, int visual)
{
    return cs.style.value_or(cycleStyle(visual));
}

// Rounds v up to 1, 2, 2.5, 5 or 10 times a power of ten and reports how many
// grid intervals suit that value.
double niceCeil(double v, int* intervals)
{
    if (!(v > 0) || !std::isfinite(v)) {
        *intervals = 4;
        return 1;
    }
    const double mag = std::pow(10.0, std::floor(std::log10(v)));
    const double f = v / mag;
    double nf;
    if (f <= 1.0 + 1e-9) { nf = 1; *intervals = 5; }
    else if (f <= 2.0 + 1e-9) { nf = 2; *intervals = 4; }
    else if (f <= 2.5 + 1e-9) { nf = 2.5; *intervals = 5; }
    else if (f <= 5.0 + 1e-9) { nf = 5; *intervals = 5; }
    else { nf = 10; *intervals = 5; }
    return nf * mag;
}

QString trimNumber(double v)
{
    if (std::fabs(v) < 1e-12)
        return QStringLiteral("0");
    return QString::number(v, 'g', 4);
}

QString relTime(double secondsAgo)
{
    const long s = std::lround(secondsAgo);
    if (s == 0)
        return QStringLiteral("now");
    if (s % 60 == 0)
        return QStringLiteral("\u2212%1 min").arg(s / 60);
    if (s > 60)
        return QStringLiteral("\u2212%1 min %2 s").arg(s / 60).arg(s % 60);
    return QStringLiteral("\u2212%1 s").arg(s);
}

} // namespace

QPolygonF clipPolygon(const QPolygonF& polygon, const QRectF& r)
{
    QPolygonF out = polygon;
    QPolygonF in;
    auto edge = [&](auto inside, auto cross) {
        in.swap(out);
        out.clear();
        if (in.isEmpty())
            return;
        QPointF prev = in.back();
        bool prevIn = inside(prev);
        for (const QPointF& cur : std::as_const(in)) {
            const bool curIn = inside(cur);
            if (curIn != prevIn)
                out.append(cross(prev, cur));
            if (curIn)
                out.append(cur);
            prev = cur;
            prevIn = curIn;
        }
    };
    auto atX = [](const QPointF& a, const QPointF& b, double x) {
        return QPointF(x, a.y() + (x - a.x()) / (b.x() - a.x()) * (b.y() - a.y()));
    };
    auto atY = [](const QPointF& a, const QPointF& b, double y) {
        return QPointF(a.x() + (y - a.y()) / (b.y() - a.y()) * (b.x() - a.x()), y);
    };
    edge([&](const QPointF& q) { return q.x() >= r.left(); },
         [&](const QPointF& a, const QPointF& b) { return atX(a, b, r.left()); });
    edge([&](const QPointF& q) { return q.x() <= r.right(); },
         [&](const QPointF& a, const QPointF& b) { return atX(a, b, r.right()); });
    edge([&](const QPointF& q) { return q.y() >= r.top(); },
         [&](const QPointF& a, const QPointF& b) { return atY(a, b, r.top()); });
    edge([&](const QPointF& q) { return q.y() <= r.bottom(); },
         [&](const QPointF& a, const QPointF& b) { return atY(a, b, r.bottom()); });
    return out;
}

bool clipSegment(QPointF& a, QPointF& b, const QRectF& r)
{
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a.x() - r.left(), r.right() - a.x(), a.y() - r.top(), r.bottom() - a.y()};
    double t0 = 0, t1 = 1;
    for (int k = 0; k < 4; ++k) {
        if (p[k] == 0) {
            if (q[k] < 0)
                return false;
            continue;
        }
        const double t = q[k] / p[k];
        if (p[k] < 0) {
            if (t > t1)
                return false;
            t0 = std::max(t0, t);
        } else {
            if (t < t0)
                return false;
            t1 = std::min(t1, t);
        }
    }
    const QPointF origin = a;
    a = QPointF(origin.x() + t0 * dx, origin.y() + t0 * dy);
    b = QPointF(origin.x() + t1 * dx, origin.y() + t1 * dy);
    return true;
}

void strokeTrace(QPainter& p, const QPolygonF& run, const std::vector<char>& anchors, const QColor& color,
                 double width, Qt::PenStyle style, const QRectF& clip)
{
    const qsizetype n = run.size();
    if (n < 2 || style == Qt::NoPen || !(width > 0))
        return;
    QVarLengthArray<QLineF, 512> lines;
    const bool solid = style == Qt::SolidLine || style == Qt::CustomDashLine;
    if (solid) {
        // Square caps close the joins between segments; the run's own ends
        // are pulled in by half a width so that its caps end at the samples.
        const double half = width / 2;
        for (qsizetype k = 1; k < n; ++k) {
            QPointF a = run[k - 1], b = run[k];
            const double len = QLineF(a, b).length();
            if (!(len > 0))
                continue;
            const QPointF dir = (b - a) / len;
            if (k == 1)
                a += dir * std::min(half, len / 2);
            if (k == n - 1)
                b -= dir * std::min(half, len / 2);
            if (clipSegment(a, b, clip))
                lines.append(QLineF(a, b));
        }
    } else {
        const DashPattern pattern = dashPattern(style);
        const double unit = std::max(width, 0.5);
        double period = 0;
        for (int k = 0; k < pattern.count; ++k)
            period += pattern.lengths[k] * unit;
        // Dashes of segment a-b whose pattern position at a is offset; only
        // the part inside clip is walked.
        auto dashes = [&](const QPointF& a, const QPointF& b, double offset) {
            const double len = QLineF(a, b).length();
            QPointF ca = a, cb = b;
            if (!(len > 0) || !clipSegment(ca, cb, clip))
                return;
            const QPointF dir = (b - a) / len;
            double s = QLineF(a, ca).length();
            const double end = QLineF(a, cb).length();
            double m = std::fmod(offset + s, period);
            int k = 0;
            double edge = pattern.lengths[0] * unit;
            while (m >= edge && k < pattern.count - 1)
                edge += pattern.lengths[++k] * unit;
            while (s < end) {
                const double step = std::min(edge - m, end - s);
                if (k % 2 == 0 && step > 0)
                    lines.append(QLineF(a + dir * s, a + dir * (s + step)));
                s += std::max(step, 0.0);
                m += std::max(step, 0.0);
                if (m >= edge - 1e-9) {
                    k = (k + 1) % pattern.count;
                    if (k == 0) {
                        m = 0;
                        edge = pattern.lengths[0] * unit;
                    } else {
                        edge += pattern.lengths[k] * unit;
                    }
                }
            }
        };
        auto anchored = [&](qsizetype k) { return k < static_cast<qsizetype>(anchors.size()) && anchors[k]; };
        qsizetype first = 0;
        while (first < n && !anchored(first))
            ++first;
        if (first == n)
            first = 0;
        // Backwards from the first anchor, then forwards restarting at each.
        double d = 0;
        for (qsizetype k = first; k > 0; --k) {
            dashes(run[k], run[k - 1], d);
            d += QLineF(run[k], run[k - 1]).length();
        }
        d = 0;
        for (qsizetype k = first + 1; k < n; ++k) {
            if (anchored(k - 1))
                d = 0;
            dashes(run[k - 1], run[k], d);
            d += QLineF(run[k - 1], run[k]).length();
        }
    }
    if (lines.isEmpty())
        return;
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(color, width, Qt::SolidLine, solid ? Qt::SquareCap : Qt::FlatCap));
    p.setBrush(Qt::NoBrush);
    p.drawLines(lines.constData(), static_cast<int>(lines.size()));
}

bool isDarkPalette(const QPalette& palette)
{
    return palette.color(QPalette::Base).lightness() < 128;
}

QColor seriesColor(int index, const QPalette& palette)
{
    // Hues after Okabe-Ito, with separate lightness for light and dark bases.
    static const std::array<QColor, 8> light = {
        QColor(0x00, 0x5f, 0xa3), QColor(0xc2, 0x4a, 0x00), QColor(0x00, 0x7d, 0x5a), QColor(0xa2, 0x3f, 0x87),
        QColor(0x8a, 0x6d, 0x00), QColor(0x2b, 0x8c, 0xc4), QColor(0xb3, 0x1b, 0x1b), QColor(0x55, 0x5f, 0x6b),
    };
    static const std::array<QColor, 8> dark = {
        QColor(0x6c, 0xb4, 0xf0), QColor(0xf5, 0xa0, 0x5a), QColor(0x4f, 0xd1, 0xa3), QColor(0xe0, 0x8c, 0xcc),
        QColor(0xe8, 0xcc, 0x5a), QColor(0x8e, 0xd8, 0xff), QColor(0xff, 0x7a, 0x7a), QColor(0xb4, 0xbe, 0xcb),
    };
    const auto& set = isDarkPalette(palette) ? dark : light;
    const int n = static_cast<int>(set.size());
    return set[static_cast<std::size_t>(((index % n) + n) % n)];
}

Qt::PenStyle seriesStyle(int index)
{
    return cycleStyle(index);
}

namespace {

// Chart colours, all derived from the current palette. The plot is lightly
// tinted with the chart's first series colour, and so are its grid lines.
struct PlotColors {
    bool dark = false;
    QColor base, text, muted;
    QColor frame; // translucent text
    QColor plotBg;
    QColor grid;        // translucent accent, for lines drawn over the data
    QColor gridOpaque;  // the same over the plot background
    QColor frameOpaque; // frame over the plot background
};

PlotColors plotColors(const QPalette& pal, const std::vector<ChartSeries>& series)
{
    PlotColors c;
    c.dark = isDarkPalette(pal);
    c.base = pal.color(QPalette::Base);
    c.text = pal.color(QPalette::Text);
    c.muted = pal.color(QPalette::PlaceholderText);
    if (!c.muted.isValid() || c.muted == c.text)
        c.muted = pal.color(QPalette::Disabled, QPalette::Text);
    c.frame = c.text;
    c.frame.setAlphaF(0.22f);
    const QColor accent = series.empty() ? c.text : seriesColor(visualOf(series.front(), 0), pal);
    c.plotBg = blend(c.base, accent, c.dark ? 0.07 : 0.045);
    const double gridAlpha = c.dark ? 0.22 : 0.16;
    c.grid = accent;
    c.grid.setAlphaF(static_cast<float>(gridAlpha));
    c.gridOpaque = blend(c.plotBg, accent, gridAlpha);
    c.frameOpaque = blend(c.plotBg, c.text, 0.22);
    return c;
}

// Values of the horizontal grid lines: multiples of the step, also for fixed
// ranges whose bounds are not. The glance view keeps every other line, and a
// plot too short for maxLines labels (0: no limit) keeps every k-th one, zero
// included when it is shown, so labels never overlap.
std::vector<double> gridValues(double lo, double hi, double step, bool simple, int maxLines)
{
    std::vector<double> values;
    if (!(step > 0) || !std::isfinite(lo) || !std::isfinite(hi))
        return values;
    if (simple) {
        const long intervals = std::lround((hi - lo) / step);
        if (intervals >= 4 && intervals % 2 == 0)
            step *= 2;
    }
    const double start = std::ceil(lo / step - 1e-9) * step;
    for (double v = start; v <= hi + step * 1e-6 && values.size() < 12; v += step)
        values.push_back(v);
    const int n = static_cast<int>(values.size());
    if (maxLines >= 1 && n > maxLines) {
        const int k = (n + maxLines - 1) / maxLines;
        int anchor = 0;
        for (int i = 0; i < n; ++i)
            if (std::abs(values[static_cast<std::size_t>(i)]) < step * 1e-6)
                anchor = i;
        std::vector<double> kept;
        for (int i = anchor % k; i < n; i += k)
            kept.push_back(values[static_cast<std::size_t>(i)]);
        values = std::move(kept);
    }
    return values;
}

// The static plot background between x0 and x1: tint, horizontal grid lines
// (at rows gridYs, one of which can be the row below the plot) and the top
// and bottom frame lines, all opaque.
void paintPlotBase(QPainter& p, const QRectF& plot, double x0, double x1, const std::vector<double>& gridYs,
                   const PlotColors& c)
{
    if (!(x1 > x0))
        return;
    p.fillRect(QRectF(x0, plot.top(), x1 - x0, plot.height()), c.plotBg);
    for (double y : gridYs)
        p.fillRect(QRectF(x0, y, x1 - x0, 1), c.gridOpaque);
    p.fillRect(QRectF(x0, plot.top(), x1 - x0, 1), c.frameOpaque);
    p.fillRect(QRectF(x0, plot.bottom() - 1, x1 - x0, 1), c.frameOpaque);
}

// A translucent fill in bands of at most 90 device rows: taller ones make
// Qt's raster engine hand the blend to its thread pool, which for these
// one-pixel columns costs far more than it saves.
void fillTranslucent(QPainter& p, const QRectF& r, const QColor& color)
{
    const double sy = std::fabs(p.deviceTransform().m22());
    const double band = std::max(1.0, std::floor(90 / std::max(sy, 1e-3)));
    for (double y = r.top(); y < r.bottom(); y += band)
        p.fillRect(QRectF(r.left(), y, r.width(), std::min(band, r.bottom() - y)), color);
}

// The readout box beside the cursor line at x: right of it, or left of it
// where it would pass the plot's right edge.
QRectF readoutBox(const QFontMetrics& fm, const QStringList& rows, double x, const QRectF& plot)
{
    double bw = 0;
    for (const auto& r : rows)
        bw = std::max<double>(bw, fm.horizontalAdvance(r));
    bw += 16;
    const double bh = rows.size() * fm.height() + 10;
    double bx = x + 10;
    if (bx + bw > plot.right())
        bx = x - 10 - bw;
    bx = std::max(bx, plot.left());
    return {bx, plot.top() + 6, bw, bh};
}

// What the readout cursor at x covers: the dashed line with the sample
// markers on it, and the box.
QRegion cursorRegion(double x, const QRectF& box, const QRectF& plot)
{
    QRegion r(QRectF(x - 6, plot.top() - 5, 12, plot.height() + 10).toAlignedRect());
    r += box.adjusted(-2, -2, 2, 2).toAlignedRect();
    return r;
}

} // namespace

TimeChart::TimeChart(const QString& title, ps::Unit unit, QWidget* parent)
    : QWidget(parent)
    , m_title(title)
    , m_unit(unit)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName(title);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    if (unit == ps::Unit::Percent)
        setFixedRange(0, 100);
}

void TimeChart::setHistory(const ps::History* history)
{
    m_history = history;
    invalidateLayer();
    refresh();
}

void TimeChart::setSeries(std::vector<ChartSeries> series)
{
    const QSize minimum = minimumSizeHint();
    m_series = std::move(series);
    invalidateLayer();
    m_layoutValid = false;
    if (minimumSizeHint() != minimum)
        updateGeometry();
    refresh();
}

void TimeChart::setUnit(ps::Unit unit)
{
    m_unit = unit;
    invalidateLayer();
    m_layoutValid = false;
    update();
}

void TimeChart::setFixedRange(double min, double max)
{
    // Invalid bounds (non-finite, empty or inverted) fall back to automatic.
    const double nextMin = (std::isfinite(min) && std::isfinite(max) && max > min) ? min : ps::kNaN;
    const double nextMax = std::isfinite(nextMin) ? max : ps::kNaN;
    const bool same = (std::isnan(nextMin) && std::isnan(m_fixedMin))
        || (nextMin == m_fixedMin && nextMax == m_fixedMax);
    if (same)
        return;
    m_fixedMin = nextMin;
    m_fixedMax = nextMax;
    m_statsValid = false;
    update();
}

void TimeChart::setAutoRange()
{
    if (!hasFixedRange())
        return;
    m_fixedMin = ps::kNaN;
    m_fixedMax = ps::kNaN;
    m_statsValid = false;
    update();
}

bool TimeChart::hasFixedRange() const
{
    return std::isfinite(m_fixedMin) && std::isfinite(m_fixedMax) && m_fixedMax > m_fixedMin;
}

void TimeChart::setWindowSeconds(double seconds)
{
    if (seconds > 0 && std::isfinite(seconds))
        m_window = seconds;
    refresh(); // the visible window decides what is announced
}

void TimeChart::setMode(Mode mode)
{
    m_mode = mode;
    setFocusPolicy(mode == Mode::Full ? Qt::TabFocus : Qt::NoFocus);
    invalidateLayer();
    m_layoutValid = false;
    updateAccessibleDescription();
    updateGeometry();
    update();
}

void TimeChart::setTitle(const QString& title)
{
    if (title == m_title)
        return;
    m_title = title;
    setAccessibleName(title);
    update();
}

void TimeChart::setEmptyText(const QString& text)
{
    if (text == m_emptyText)
        return;
    m_emptyText = text;
    refresh();
}

void TimeChart::setNotice(const QString& notice)
{
    if (notice == m_notice)
        return;
    m_notice = notice;
    // Without a cursor the description announces the notice with the latest
    // value; a cursor readout gives the selected sample's own age instead.
    updateAccessibleDescription();
    update();
}

void TimeChart::refresh()
{
    noteLatestSample();
    updateAccessibleDescription();
    update();
}

void TimeChart::setSimple(bool simple)
{
    if (simple == m_simple)
        return;
    m_simple = simple;
    invalidateLayer(); // the layer holds the grid
    update();
}

void TimeChart::setLegendVisible(bool visible)
{
    if (visible == m_legendVisible)
        return;
    m_legendVisible = visible;
    m_layoutValid = false;
    updateGeometry();
    update();
}

void TimeChart::setRevealEnabled(bool enabled)
{
    if (enabled == m_revealEnabled)
        return;
    m_revealEnabled = enabled;
    if (!enabled && std::isfinite(m_revealTo)) {
        m_revealTo = ps::kNaN;
        update(plotUpdateRect());
    }
}

void TimeChart::noteLatestSample()
{
    const double latest = (m_history && !m_history->empty()) ? m_history->latestTime() : ps::kNaN;
    if (latest == m_seenLatest || (std::isnan(latest) && std::isnan(m_seenLatest)))
        return;
    const double previous = m_seenLatest;
    m_seenLatest = latest;
    m_revealTo = ps::kNaN;
    // Only a newer sample joined to its predecessor by a line is revealed;
    // one after a gap simply appears.
    if (!m_revealEnabled || !std::isfinite(latest) || !std::isfinite(previous) || !(latest > previous))
        return;
    const std::size_t n = m_history->size();
    if (n < 2 || !m_history->continuous(n - 1))
        return;
    const double from = m_history->time(n - 2);
    const double interval = latest - from;
    if (!(interval > 0) || !std::isfinite(interval))
        return;
    m_revealFrom = from;
    m_revealTo = latest;
    m_revealStart = endTime();
    m_revealDuration = std::min(kRevealSeconds, 0.5 * interval);
}

double TimeChart::revealFraction() const
{
    if (!std::isfinite(m_revealTo) || !(m_revealDuration > 0))
        return 1;
    const double f = (endTime() - m_revealStart) / m_revealDuration;
    if (!(f < 1))
        return 1;
    if (!(f > 0))
        return 0;
    const double rest = 1 - f; // ease-out quart
    return 1 - rest * rest * rest * rest;
}

bool TimeChart::isRevealing() const
{
    return revealFraction() < 1;
}

bool TimeChart::layerMatchesHistory() const
{
    const DataLayer& g = m_layer;
    return m_paintedLayer && g.valid && g.plot == m_lastPlot && m_history && g.history == m_history &&
           g.samples == m_history->totalSamples() && g.size == m_history->size() &&
           g.latest == m_history->latestTime();
}

QRect TimeChart::plotUpdateRect() const
{
    // The data layer never leaves the plot (the clip ends one pixel below
    // it). Above the drawn data, scrolling changes nothing: the background
    // there is uniform along the time axis and what is fixed on screen stays
    // put, so while the layer still shows the current history only the rows
    // from the data down are repainted.
    QRectF r = m_lastPlot;
    const double top = m_layer.extent.top;
    if (layerMatchesHistory() && top > r.top() && top < r.bottom())
        r.setTop(top);
    return r.adjusted(-1, -1, 1, 2).toAlignedRect().intersected(rect());
}

QRegion TimeChart::plotUpdateRegion() const
{
    const QRect rows = plotUpdateRect();
    const DataLayer& g = m_layer;
    const Extent& e = g.extent;
    if (!layerMatchesHistory() || !e.banded || m_sweepContent != g.content || !std::isfinite(m_sweepFrom) ||
        !std::isfinite(m_sweepTo) || !(m_window > 0))
        return rows;
    // Below the lines' rows the layer only changes along the time axis at
    // the vertical edges of areas and shading, and at the reveal cut. Each
    // may be on screen where it was for any axis end the content was painted
    // for, so the columns span from its position then to where it is now.
    const QRectF plot = m_lastPlot;
    const double end = endTime();
    const double early = std::min(m_sweepFrom, end), late = std::max(m_sweepTo, end);
    auto xAt = [&](double t, double axisEnd) { return plot.right() - (axisEnd - t) / m_window * plot.width(); };
    QRegion region(QRectF(plot.left() - 1, e.bandTop, plot.width() + 2, e.bandBottom - e.bandTop).toAlignedRect());
    const double hullTop = std::min(e.bandTop, e.base), hullBottom = std::max(e.bandBottom, e.base) + 2;
    auto column = [&](double from, double to, bool full) {
        const double top = full ? plot.top() - 1 : hullTop;
        const double bottom = full ? plot.bottom() + 2 : hullBottom;
        if (std::isfinite(from) && std::isfinite(to) && to - from < 1e6)
            region += QRectF(from - 3, top, to - from + 6, bottom - top).toAlignedRect();
        else
            region += rows;
    };
    for (const auto& edge : e.edges)
        column(xAt(edge.time, late), xAt(edge.time, early), edge.full);
    if (std::isfinite(m_revealTo))
        column(xAt(m_revealFrom, late), plot.right() + 1, true);
    region &= rows;
    // Each rectangle of a region costs about as much as copying this many
    // device pixels (clipping and blit setup), so a region only pays off
    // when it is much smaller than the rows.
    constexpr double perRect = 8000;
    const double dpr2 = devicePixelRatioF() * devicePixelRatioF();
    double cost = 0;
    for (const QRect& r : region)
        cost += static_cast<double>(r.width()) * r.height() * dpr2 + perRect;
    return cost < static_cast<double>(rows.width()) * rows.height() * dpr2 + perRect ? region : QRegion(rows);
}

void TimeChart::advanceView(double time)
{
    if (!std::isfinite(time))
        return;
    m_viewEnd = time;
    if (!isVisible())
        return;
    if (!(m_lastPlot.width() > 0) || !std::isfinite(m_paintedEnd)) {
        update();
        return;
    }
    const bool inWindow = hasVisibleSamples();
    const std::size_t first = inWindow ? firstVisibleIndex() : 0;
    // A sample entering or leaving the window can change the value range,
    // the legend values or the empty message: repaint everything then.
    if (inWindow != m_paintedInWindow || first != m_paintedFirst) {
        update();
        return;
    }
    if (!inWindow)
        return; // an empty window has nothing that moves
    const bool cursor = std::isfinite(cursorTime());
    // A reveal in progress, or one that ended since the last paint.
    const bool revealing = isRevealing() || m_paintedReveal < 1;
    if (!m_paintedMoving && !cursor && !revealing)
        return; // nothing drawn scrolls (for example no valid values)
    if (!revealing) {
        bool moved;
        if (m_paintedLayer && m_layer.valid)
            moved = layerKey(endTime()) != m_paintedKey;
        else
            moved = std::fabs(endTime() - m_paintedEnd) / m_window * m_lastPlot.width() * devicePixelRatioF() >= 0.125;
        if (!moved)
            return;
    }
    // The readout moves with its sample, over the plot and possibly past it.
    if (cursor) {
        QRegion region = plotUpdateRegion();
        region += m_paintedCursor;
        region += cursorFootprint();
        update(region);
    } else {
        update(plotUpdateRegion());
    }
}

QRegion TimeChart::cursorFootprint() const
{
    if (m_mode != Mode::Full || !(m_lastPlot.width() > 0) || !(m_window > 0))
        return {};
    const auto nearest = nearestIndex(cursorTime());
    if (!nearest)
        return {};
    const QRectF plot = m_lastPlot;
    const double x = plot.right() - (endTime() - m_history->time(*nearest)) / m_window * plot.width();
    if (!(x >= plot.left() - 1 && x <= plot.right() + 1))
        return {};
    const QFontMetrics fm(font());
    const QRectF box = readoutBox(fm, readoutText().split(QLatin1Char('\n')), x, plot);
    return cursorRegion(x, box, plot) & rect();
}

QSize TimeChart::sizeHint() const
{
    return m_mode == Mode::Full ? QSize(520, 240) : QSize(180, 84);
}

QSize TimeChart::minimumSizeHint() const
{
    if (m_mode == Mode::Compact)
        return QSize(110, 64);
    // Title, up to two legend rows, a plot with room for a few grid labels
    // and the time axis, in the current font.
    const QFontMetrics fm(font());
    const int legendRows = (m_series.empty() || !m_legendVisible) ? 0 : m_series.size() > 2 ? 2 : 1;
    const int h = static_cast<int>(kMargin * 1.5) + (fm.height() + 2) * (1 + legendRows) + 6 + 5 * fm.height() + 4;
    return QSize(260, std::max(150, h));
}

void TimeChart::setViewEnd(double time)
{
    m_viewEnd = std::isfinite(time) ? time : ps::kNaN;
}

double TimeChart::endTime() const
{
    const double latest = (m_history && !m_history->empty()) ? m_history->latestTime() : ps::kNaN;
    if (!std::isfinite(m_viewEnd))
        return std::isfinite(latest) ? latest : 0.0;
    return std::isfinite(latest) ? std::max(m_viewEnd, latest) : m_viewEnd;
}

std::size_t TimeChart::firstVisibleIndex() const
{
    if (!m_history || m_history->empty())
        return 0;
    return m_history->lowerBound(endTime() - m_window);
}

bool TimeChart::hasVisibleSamples() const
{
    return m_history && !m_history->empty() && firstVisibleIndex() < m_history->size();
}

std::optional<std::size_t> TimeChart::nearestIndex(double time) const
{
    if (!hasVisibleSamples() || !std::isfinite(time))
        return std::nullopt;
    const std::size_t n = m_history->size();
    const std::size_t first = firstVisibleIndex();
    std::size_t i = std::clamp(m_history->lowerBound(time), first, n - 1);
    if (i > first && std::fabs(m_history->time(i - 1) - time) <= std::fabs(m_history->time(i) - time))
        --i;
    return i;
}

double TimeChart::cursorTime() const
{
    if (m_cursorFollowsMouse && m_lastPlot.width() > 0)
        return endTime() - (m_lastPlot.right() - m_mouseX) / m_lastPlot.width() * m_window;
    return m_cursorTime;
}

void TimeChart::setCursorTime(double time)
{
    m_cursorFollowsMouse = false;
    m_cursorTime = time;
    updateAccessibleDescription();
    update();
}

void TimeChart::clearCursor()
{
    m_cursorFollowsMouse = false;
    m_cursorTime = ps::kNaN;
    updateAccessibleDescription();
    update();
}

QString TimeChart::formatValue(double v) const
{
    switch (m_unit) {
    case ps::Unit::Percent: return QString::fromStdString(ps::fmt::percent(v));
    case ps::Unit::Bytes: return QString::fromStdString(ps::fmt::bytes(v));
    case ps::Unit::BytesPerSecond: return QString::fromStdString(ps::fmt::rate(v));
    case ps::Unit::Celsius: return QString::fromStdString(ps::fmt::celsius(v));
    }
    return {};
}

QString TimeChart::emptyMessage() const
{
    // Before the first sample nothing is known yet, so the chart's specific
    // explanation ("No swap is configured") would be a claim.
    if (!m_history || m_history->empty())
        return QStringLiteral("Waiting for the first sample");
    // Every retained sample is older than the window (samples stopped
    // arriving), so the chart's specific explanation would not apply either.
    if (!hasVisibleSamples())
        return QStringLiteral("No samples in this time window");
    return m_emptyText.isEmpty() ? QStringLiteral("No data yet") : m_emptyText;
}

bool TimeChart::hasVisibleData() const
{
    if (!m_history || m_history->empty())
        return false;
    const std::size_t first = firstVisibleIndex(), n = m_history->size();
    for (const auto& cs : m_series) {
        const auto* s = m_history->find(cs.key);
        if (!s)
            continue;
        for (std::size_t i = first; i < n; ++i)
            if (ps::isValid(m_history->value(*s, i)))
                return true;
    }
    return false;
}

QString TimeChart::readoutText() const
{
    const auto nearest = nearestIndex(cursorTime());
    if (!nearest)
        return {};
    const std::size_t i = *nearest;
    const double ago = endTime() - m_history->time(i);
    QString text = ago < 0.05 ? QStringLiteral("Latest sample")
                              : QStringLiteral("%1 s ago").arg(QString::number(ago, 'f', ago < 10 ? 1 : 0));
    for (const auto& cs : m_series) {
        const auto* s = m_history->find(cs.key);
        const double v = s ? m_history->value(*s, i) : ps::kNaN;
        text += QStringLiteral("\n%1: %2").arg(cs.label, formatValue(v));
    }
    return text;
}

void TimeChart::updateAccessibleDescription()
{
    // While a cursor selects a sample, assistive technology hears that
    // sample, recomputed on every refresh, exactly as the readout shows it,
    // age included. Otherwise (no cursor, or a compact chart, which has
    // none) it hears the latest value, after the notice ("No recent
    // samples", "Paused") when there is one, so an old value is never heard
    // as current. This runs at sample cadence (refresh, cursor, notice and
    // mode changes), never from the frame loop.
    QString d = m_mode == Mode::Full ? readoutText() : QString();
    if (d.isEmpty() && !hasVisibleData()) {
        // Nothing is plotted: announce the same explanation the plot shows.
        d = emptyMessage();
    } else if (d.isEmpty()) {
        for (const auto& cs : m_series) {
            const double v = m_history ? m_history->latestValue(cs.key) : ps::kNaN;
            if (!d.isEmpty())
                d += QStringLiteral(", ");
            d += cs.label + QLatin1Char(' ') + formatValue(v);
        }
        if (d.isEmpty() && !m_emptyText.isEmpty())
            d = m_emptyText;
        else if (!m_notice.isEmpty())
            d = m_notice + QStringLiteral(", last ") + d;
    }
    if (d != accessibleDescription())
        setAccessibleDescription(d);
}

TimeChart::Range TimeChart::computeRange(std::size_t first, std::size_t last) const
{
    Range r;
    int intervals = 4;
    if (hasFixedRange()) {
        r.lo = m_fixedMin;
        r.hi = m_fixedMax;
        if (m_unit == ps::Unit::Bytes || m_unit == ps::Unit::BytesPerSecond) {
            const auto scale = ps::fmt::iecScaleFor(std::max(std::fabs(r.lo), std::fabs(r.hi)));
            r.divisor = scale.divisor;
            r.unitText = QString::fromLatin1(scale.unit)
                + (m_unit == ps::Unit::BytesPerSecond ? QStringLiteral("/s") : QString());
        } else {
            r.unitText = m_unit == ps::Unit::Percent ? QStringLiteral("%") : QStringLiteral("\u00b0C");
        }
        r.step = niceCeil((r.hi - r.lo) / r.divisor / 5.0, &intervals) * r.divisor;
        return r;
    }
    double lo = INFINITY, hi = -INFINITY;
    if (m_history) {
        for (const auto& cs : m_series) {
            const auto* s = m_history->find(cs.key);
            if (!s)
                continue;
            for (std::size_t i = first; i <= last && i < m_history->size(); ++i) {
                const double v = m_history->value(*s, i);
                if (ps::isValid(v)) {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            }
        }
    }
    switch (m_unit) {
    case ps::Unit::Percent: {
        // Utilisation normally tops out at 100%; grow in nice steps if a
        // series legitimately exceeds it.
        r.lo = 0;
        r.hi = (std::isfinite(hi) && hi > 100) ? niceCeil(hi, &intervals) : 100;
        r.step = r.hi / 4;
        r.unitText = QStringLiteral("%");
        break;
    }
    case ps::Unit::Bytes:
    case ps::Unit::BytesPerSecond: {
        const double floorTop = m_unit == ps::Unit::BytesPerSecond ? 1024.0 : 1024.0 * 1024.0;
        const double top = std::max(std::isfinite(hi) ? hi : 0.0, floorTop);
        const auto scale = ps::fmt::iecScaleFor(top);
        r.divisor = scale.divisor;
        r.unitText = QString::fromLatin1(scale.unit) + (m_unit == ps::Unit::BytesPerSecond ? QStringLiteral("/s") : QString());
        const double niceTop = niceCeil(top / r.divisor, &intervals);
        r.lo = 0;
        r.hi = niceTop * r.divisor;
        r.step = r.hi / intervals;
        break;
    }
    case ps::Unit::Celsius: {
        if (!std::isfinite(lo)) {
            lo = 30;
            hi = 60;
        }
        r.lo = std::floor((lo - 2) / 10.0) * 10.0;
        r.hi = std::ceil((hi + 2) / 10.0) * 10.0;
        if (r.lo > 0 && lo >= 0)
            r.lo = std::max(0.0, r.lo);
        if (r.hi - r.lo < 20)
            r.hi = r.lo + 20;
        const double span = r.hi - r.lo;
        r.step = span <= 40 ? 10 : span <= 100 ? 20 : 50;
        r.unitText = QStringLiteral("\u00b0C");
        break;
    }
    }
    if (!(r.hi > r.lo)) {
        r.lo = 0;
        r.hi = 1;
    }
    if (!(r.step > 0))
        r.step = (r.hi - r.lo) / 4;
    return r;
}

QString TimeChart::axisLabel(double v, const Range& r) const
{
    switch (m_unit) {
    case ps::Unit::Percent: return trimNumber(v) + QStringLiteral("%");
    case ps::Unit::Celsius: return trimNumber(v) + QStringLiteral(" \u00b0C");
    case ps::Unit::Bytes:
    case ps::Unit::BytesPerSecond: return trimNumber(v / r.divisor) + QLatin1Char(' ') + r.unitText;
    }
    return {};
}

TimeChart::Layout TimeChart::computeLayout(const QFontMetrics& fm) const
{
    Layout l;
    const QRectF area = QRectF(rect()).adjusted(kMargin, kMargin * 0.75, -kMargin, -kMargin * 0.75);
    if (m_mode == Mode::Compact) {
        l.header = QRectF(area.left(), area.top(), area.width(), fm.height());
        l.plot = QRectF(area.left(), l.header.bottom() + 3, area.width(), area.bottom() - l.header.bottom() - 3);
        return l;
    }
    // Title row, then legend rows that wrap to the widget width.
    const double rowH = fm.height() + 2;
    const double valueReserve = fm.horizontalAdvance(QStringLiteral("000 MiB/s"));
    double x = 0;
    int rows = (m_series.empty() || !m_legendVisible) ? 0 : 1;
    for (const auto& cs : m_series) {
        if (rows == 0)
            break;
        const double w = kSwatch + 6 + fm.horizontalAdvance(cs.label + QStringLiteral(": ")) + valueReserve + 16;
        if (x > 0 && x + w > area.width()) {
            ++rows;
            x = 0;
        }
        x += w;
    }
    l.legendRows = rows;
    l.header = QRectF(area.left(), area.top(), area.width(), rowH * (1 + rows));
    QString widest;
    switch (m_unit) {
    case ps::Unit::Percent: widest = QStringLiteral("100%"); break;
    case ps::Unit::Celsius: widest = QStringLiteral("\u2212100 \u00b0C"); break;
    case ps::Unit::Bytes: widest = QStringLiteral("1000 MiB"); break;
    case ps::Unit::BytesPerSecond: widest = QStringLiteral("1000 MiB/s"); break;
    }
    const double axisW = fm.horizontalAdvance(widest) + 8;
    l.tickLabelWidth = fm.horizontalAdvance(QStringLiteral("\u221215 min"));
    l.compoundLabelWidth = fm.horizontalAdvance(QStringLiteral("\u221214 min 45 s"));
    const double top = l.header.bottom() + 6;
    const double bottom = area.bottom() - fm.height() - 4;
    l.plot = QRectF(area.left() + axisW, top, std::max(10.0, area.width() - axisW), std::max(10.0, bottom - top));
    return l;
}

const TimeChart::Layout& TimeChart::layout(const QFontMetrics& fm)
{
    // Text measurement is not free (shaping), and frames between samples
    // would otherwise repeat it for an unchanged layout.
    if (!m_layoutValid || m_layoutSize != size()) {
        m_layoutCache = computeLayout(fm);
        m_layoutSize = size();
        m_layoutValid = true;
    }
    return m_layoutCache;
}

void TimeChart::paintEvent(QPaintEvent* event)
{
    QPainter p(this);
    const QPalette pal = palette();
    const PlotColors c = plotColors(pal, m_series);
    const bool dark = c.dark;
    const QColor& base = c.base;
    const QColor& text = c.text;
    const QColor& muted = c.muted;
    const QColor& frame = c.frame;

    // Frames between samples repaint only the plot; text outside it is
    // skipped when it is not in the repainted area.
    const QRect dirty = event->rect();
    const QRegion& dirtyRegion = event->region();
    auto needs = [&](const QRectF& r) { return dirtyRegion.intersects(r.toAlignedRect()); };

    const QFontMetrics fm(font());
    const Layout& l = layout(fm);
    m_lastPlot = l.plot;
    ++m_paintCount;
    {
        // Everything but the plot, which is painted opaque below.
        const QRectF pl = l.plot;
        const double w = width(), h = height();
        p.fillRect(QRectF(0, 0, w, pl.top()), base);
        p.fillRect(QRectF(0, pl.bottom(), w, h - pl.bottom()), base);
        p.fillRect(QRectF(0, pl.top(), pl.left(), pl.height()), base);
        p.fillRect(QRectF(pl.right(), pl.top(), w - pl.right(), pl.height()), base);
    }

    const bool haveData = m_history && !m_history->empty();
    const std::size_t first = haveData ? firstVisibleIndex() : 0;
    const std::size_t last = haveData ? m_history->size() - 1 : 0;
    const std::size_t start = (haveData && first > 0) ? first - 1 : first;
    const double tEnd = endTime();
    // No sample in the window: no line, no current value and no readout.
    const bool inWindow = haveData && first <= last;
    m_paintedEnd = tEnd;
    m_paintedFirst = inWindow ? first : 0;
    m_paintedInWindow = inWindow;

    // Valid points in the window, whether anything drawn (points or outage
    // shading) scrolls with the view, and the vertical range. Cached across
    // 60 Hz scroll frames while the history and first visible sample match.
    int painted = 0;
    bool moving = false;
    Range range;
    if (inWindow) {
        const bool statsHit = m_statsValid && m_statsHistory == m_history
            && m_statsSamples == m_history->totalSamples() && m_statsSize == m_history->size()
            && m_statsLatest == m_history->latestTime() && m_statsFirst == first;
        if (statsHit) {
            painted = m_statsPainted;
            moving = m_statsMoving;
            range = m_statsRange;
        } else {
            for (const auto& cs : m_series) {
                if (const auto* s = m_history->find(cs.key))
                    for (std::size_t i = first; i <= last; ++i)
                        painted += ps::isValid(m_history->value(*s, i));
            }
            moving = painted > 0;
            for (std::size_t i = std::max<std::size_t>(start, 1); !moving && i <= last; ++i)
                moving = !m_history->continuous(i);
            range = computeRange(first, last);
            m_statsValid = true;
            m_statsHistory = m_history;
            m_statsSamples = m_history->totalSamples();
            m_statsSize = m_history->size();
            m_statsLatest = m_history->latestTime();
            m_statsFirst = first;
            m_statsPainted = painted;
            m_statsMoving = moving;
            m_statsRange = range;
        }
    } else {
        range = computeRange(first, last);
    }
    m_lastPainted = painted;
    m_paintedMoving = moving;

    // Header: title, notice and legend with current values.
    if (needs(l.header)) {
        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        p.setPen(text);
        const QFontMetrics bfm(bold);
        QString title = m_title;
        if (m_mode == Mode::Compact && !m_series.empty()) {
            const double v = inWindow ? m_history->latestValue(m_series.front().key) : ps::kNaN;
            const QString value = formatValue(v);
            p.setFont(font());
            p.setPen(text);
            p.drawText(l.header, Qt::AlignRight | Qt::AlignVCenter, value);
            p.setFont(bold);
            const double avail = l.header.width() - fm.horizontalAdvance(value) - 6;
            title = bfm.elidedText(title, Qt::ElideRight, static_cast<int>(std::max(0.0, avail)));
        }
        p.drawText(QRectF(l.header.left(), l.header.top(), l.header.width(), fm.height() + 2),
                   Qt::AlignLeft | Qt::AlignVCenter, title);
        p.setFont(font());
        if (!m_notice.isEmpty()) {
            p.setPen(muted);
            p.drawText(QRectF(l.header.left(), l.header.top(), l.header.width(), fm.height() + 2),
                       Qt::AlignRight | Qt::AlignVCenter, m_notice);
        }

        if (m_mode == Mode::Full && l.legendRows > 0) {
            const double rowH = fm.height() + 2;
            const double valueReserve = fm.horizontalAdvance(QStringLiteral("000 MiB/s"));
            double x = 0;
            int row = 1;
            for (std::size_t si = 0; si < m_series.size(); ++si) {
                const auto& cs = m_series[si];
                const int visual = visualOf(cs, si);
                const QString label = cs.label + QStringLiteral(": ");
                const double w = kSwatch + 6 + fm.horizontalAdvance(label) + valueReserve + 16;
                if (x > 0 && x + w > l.header.width()) {
                    ++row;
                    x = 0;
                }
                const double y = l.header.top() + row * rowH;
                const double cy = y + rowH / 2;
                const QColor color = seriesColor(visual, pal);
                if (cs.fill) {
                    QColor swatchFill = color;
                    swatchFill.setAlphaF(dark ? 0.30f : 0.24f);
                    p.fillRect(QRectF(l.header.left() + x, cy, kSwatch, rowH / 2 - 1), swatchFill);
                }
                QPen pen(color, 2.2, styleOf(cs, visual), Qt::FlatCap);
                p.setPen(pen);
                p.drawLine(QPointF(l.header.left() + x, cy), QPointF(l.header.left() + x + kSwatch, cy));
                p.setPen(text);
                const double lx = l.header.left() + x + kSwatch + 6;
                p.drawText(QRectF(lx, y, w, rowH), Qt::AlignLeft | Qt::AlignVCenter, label);
                const double v = inWindow ? m_history->latestValue(cs.key) : ps::kNaN;
                QFont vf = font();
                vf.setBold(true);
                p.setFont(vf);
                p.drawText(QRectF(lx + fm.horizontalAdvance(label), y, valueReserve + 16, rowH),
                           Qt::AlignLeft | Qt::AlignVCenter, formatValue(v));
                p.setFont(font());
                x += w;
            }
        }
    }

    const QRectF plot = l.plot;
    // Halved operands keep the fraction finite even for bounds near +-DBL_MAX.
    auto yOf = [&](double v) {
        return plot.bottom() - (v * 0.5 - range.lo * 0.5) / (range.hi * 0.5 - range.lo * 0.5) * plot.height();
    };
    auto xOf = [&](double t) { return plot.right() - (tEnd - t) / m_window * plot.width(); };

    // Horizontal grid: its rows belong to the opaque plot background, which is
    // painted with the data below; the labels are outside the plot.
    const int maxLines = m_mode == Mode::Full ? static_cast<int>(plot.height() / (fm.height() * 1.2)) + 1 : 0;
    const std::vector<double> gridVals = gridValues(range.lo, range.hi, range.step, m_simple, maxLines);
    std::vector<double> gridYs;
    gridYs.reserve(gridVals.size());
    for (double v : gridVals) {
        const double y = std::round(yOf(v));
        gridYs.push_back(y);
        const QRectF labelRect(0, y + 0.5 - fm.height() / 2.0, plot.left() - 6, fm.height());
        if (m_mode == Mode::Full && needs(labelRect)) {
            p.setPen(muted);
            p.drawText(labelRect, Qt::AlignRight | Qt::AlignVCenter, axisLabel(v, range));
        }
    }

    m_paintedLayer = false;
    m_paintedReveal = revealFraction();
    bool plotPainted = false;
    if (inWindow) {
        // A sample being revealed is drawn up to a point on the measured
        // segment from its predecessor; nothing is drawn beyond it.
        double revealX = INFINITY;
        if (m_paintedReveal < 1)
            revealX = xOf(m_revealFrom + m_paintedReveal * (m_revealTo - m_revealFrom));
        const QTransform device = p.deviceTransform();
        const bool aligned = device.type() <= QTransform::TxScale && device.m11() > 0 && device.m22() > 0;
        if (aligned && prepareLayer(device, plot, range, gridYs, tEnd, start)) {
            DataLayer& g = m_layer;
            const long long key = layerKey(tEnd);
            const long long whole = floorDiv(key, g.phases);
            const int phase = static_cast<int>(key - whole * g.phases);
            if (!g.drawn[static_cast<std::size_t>(phase)])
                drawLayerPhase(phase);
            const QImage& image = g.images[static_cast<std::size_t>(phase)];
            int right = g.right;
            if (revealX < plot.right())
                right = std::min(right, static_cast<int>(std::lround(std::max(revealX * g.sx + g.ox, -1e9))));
            right = std::max(right, g.left);
            if (image.isNull())
                right = g.left;
            if (right > g.left) {
                // The plot with its data, copied in device pixels: a plain
                // opaque blit, no resampling or blending. A frame repainting
                // several rectangles copies each through a clip of just that
                // rectangle: through a clip of several, the copy is blended
                // row by row (and spread over threads).
                p.save();
                p.setWorldTransform(device.inverted() * p.worldTransform());
                const QRect target(g.left, g.top, right - g.left, g.bottom - g.top);
                auto blit = [&](const QRect& d) {
                    const QRect r = d.intersected(target);
                    if (!r.isEmpty())
                        p.drawImage(r.topLeft(), image, r.translated(static_cast<int>(-whole), -g.top));
                };
                if (event->region().rectCount() > 1) {
                    for (const QRect& d : event->region()) {
                        const QRect r = device.mapRect(QRectF(d)).toAlignedRect();
                        p.setClipRect(r);
                        blit(r);
                    }
                } else {
                    blit(target);
                }
                p.restore();
            }
            // Past the reveal point the plot shows only its background.
            if (right < g.right)
                paintPlotBase(p, plot, (right - g.ox) / g.sx, plot.right(), gridYs, c);
            m_paintedKey = key;
            m_paintedLayer = true;
            plotPainted = true;
            // The axis ends the screen may show this content for. Whatever an
            // earlier content left on screen outside this paint is repainted.
            if (m_sweepContent != g.content) {
                m_sweepContent = g.content;
                m_sweepFrom = m_sweepTo = tEnd;
                const QRect all = plot.adjusted(-1, -1, 1, 2).toAlignedRect().intersected(rect());
                if (!(QRegion(all) - event->region()).isEmpty())
                    update(all);
            } else {
                m_sweepFrom = std::min(m_sweepFrom, tEnd);
                m_sweepTo = std::max(m_sweepTo, tEnd);
            }
        } else {
            // Rotated or sheared paint devices: draw the data directly.
            paintPlotBase(p, plot, plot.left(), plot.right(), gridYs, c);
            plotPainted = true;
            QRectF clip = plot.adjusted(0, 0, 0, 1);
            QRectF area = plot;
            QRectF lineClip = plot.adjusted(-2, -2, 2, 2);
            for (QRectF* r : {&clip, &area, &lineClip})
                if (revealX < r->right())
                    r->setRight(std::max(r->left(), revealX));
            p.save();
            p.setClipRect(clip);
            paintData(p, plot, range.lo, range.hi, tEnd, start, area, lineClip);
            p.restore();
        }
    }
    if (!plotPainted)
        paintPlotBase(p, plot, plot.left(), plot.right(), gridYs, c);

    // Lines fixed on screen go over the data, translucent: the time grid (at
    // fixed ages relative to now, so the data scrolls under it) and the side
    // frame lines.
    p.setRenderHint(QPainter::Antialiasing, false);
    if (m_mode == Mode::Full) {
        static const std::array<double, 10> steps = {5, 10, 15, 30, 60, 120, 180, 300, 600, 900};
        const double gap = l.tickLabelWidth * (m_simple ? 1.6 : 0.6);
        double step = steps.back();
        for (double s : steps) {
            // Sub-minute steps in a window over a minute give "−1 min 45 s" labels.
            const double labelW = (s < 60 && m_window > 60 ? l.compoundLabelWidth : l.tickLabelWidth) + gap;
            if (m_window / s <= std::max(2, static_cast<int>(plot.width() / labelW))) {
                step = s;
                break;
            }
        }
        const bool labelRow = needs(QRectF(0, plot.bottom() + 3, width(), fm.height()));
        double nextLabelRight = std::numeric_limits<double>::infinity();
        for (double ago = 0; ago <= m_window + 1e-6; ago += step) {
            const double x = std::round(plot.right() - ago / m_window * plot.width());
            // Not over the side frame lines.
            if (std::fabs(x - plot.left()) >= 0.5 && std::fabs(x - (plot.right() - 1)) >= 0.5)
                fillTranslucent(p, QRectF(x, plot.top() + 1, 1, plot.height() - 2), c.grid);
            if (!labelRow)
                continue;
            const QString label = relTime(ago);
            const double w = fm.horizontalAdvance(label);
            double lx = x + 0.5 - w / 2;
            lx = std::clamp(lx, plot.left(), plot.right() - w);
            // Labels are laid out right to left; one that would touch the
            // label to its right (an edge label pushed inwards) is left out.
            if (lx + w + 2 * fm.averageCharWidth() > nextLabelRight)
                continue;
            nextLabelRight = lx;
            const QRectF labelRect(lx, plot.bottom() + 3, w + 2, fm.height());
            if (needs(labelRect)) {
                p.setPen(muted);
                p.drawText(labelRect, Qt::AlignLeft | Qt::AlignTop, label);
            }
        }
    }
    fillTranslucent(p, QRectF(plot.left(), plot.top() + 1, 1, plot.height() - 2), frame);
    fillTranslucent(p, QRectF(plot.right() - 1, plot.top() + 1, 1, plot.height() - 2), frame);

    if (painted == 0) {
        p.setPen(muted);
        p.drawText(plot.adjusted(6, 4, -6, -4), Qt::AlignCenter | Qt::TextWordWrap, emptyMessage());
    }

    // Readout cursor.
    const double ct = cursorTime();
    const auto nearest = m_mode == Mode::Full ? nearestIndex(ct) : std::nullopt;
    QRegion drawnCursor;
    if (nearest) {
        const std::size_t i = *nearest;
        const double x = xOf(m_history->time(i));
        if (x >= plot.left() - 1 && x <= plot.right() + 1) {
            QColor cursor = text;
            cursor.setAlphaF(0.55f);
            p.setPen(QPen(cursor, 1, Qt::DashLine));
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            p.setRenderHint(QPainter::Antialiasing, true);
            for (std::size_t si = 0; si < m_series.size(); ++si) {
                const auto* s = m_history->find(m_series[si].key);
                const double v = s ? m_history->value(*s, i) : ps::kNaN;
                if (!ps::isValid(v) || v < range.lo || v > range.hi)
                    continue;
                p.setPen(QPen(base, 1.5));
                p.setBrush(seriesColor(visualOf(m_series[si], si), pal));
                p.drawEllipse(QPointF(x, yOf(v)), 3.5, 3.5);
            }
            const QStringList rows = readoutText().split(QLatin1Char('\n'));
            const QRectF box = readoutBox(fm, rows, x, plot);
            p.setPen(QPen(frame, 1));
            p.setBrush(pal.color(QPalette::ToolTipBase));
            p.drawRoundedRect(box, 4, 4);
            p.setPen(pal.color(QPalette::ToolTipText));
            double ty = box.top() + 5;
            for (const auto& r : rows) {
                p.drawText(QRectF(box.left() + 8, ty, box.width(), fm.height()), Qt::AlignLeft | Qt::AlignVCenter, r);
                ty += fm.height();
            }
            drawnCursor = cursorRegion(x, box, plot) & rect();
        }
    }
    // The screen now shows the cursor where it was drawn, and wherever an
    // earlier one was that this paint did not cover.
    m_paintedCursor = (m_paintedCursor - event->region()) + drawnCursor;

    if (hasFocus() && !QRectF(rect()).adjusted(3, 3, -3, -3).contains(dirty)) {
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(pal.color(QPalette::Highlight), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1));
    }
}

void TimeChart::paintData(QPainter& p, const QRectF& plot, double lo, double hi, double tEnd, std::size_t start,
                          const QRectF& area, const QRectF& lineClip) const
{
    if (!m_history || m_history->empty())
        return;
    const std::size_t last = m_history->size() - 1;
    const QPalette pal = palette();
    const bool dark = isDarkPalette(pal);
    auto yOf = [&](double v) { return plot.bottom() - (v * 0.5 - lo * 0.5) / (hi * 0.5 - lo * 0.5) * plot.height(); };
    auto xOf = [&](double t) { return plot.right() - (tEnd - t) / m_window * plot.width(); };
    const QRectF band(area.left(), plot.top(), area.width(), plot.height());

    // Shade discontinuities (pause, suspend, collector outage) so the
    // missing interval reads as "no data", not as a flat line.
    p.setRenderHint(QPainter::Antialiasing, false);
    QColor gapColor = pal.color(QPalette::Text);
    gapColor.setAlphaF(0.06f);
    for (std::size_t i = std::max<std::size_t>(start, 1); i <= last; ++i) {
        if (m_history->continuous(i))
            continue;
        const double x0 = xOf(m_history->time(i - 1));
        const double x1 = xOf(m_history->time(i));
        if (x1 - x0 >= 2) {
            const QRectF shade = QRectF(x0, plot.top(), x1 - x0, plot.height()).intersected(band);
            if (!shade.isEmpty())
                p.fillRect(shade, gapColor);
        }
    }

    // Runs of consecutive valid samples in screen coordinates. Segments keep
    // their measured geometry and are clipped (the line to lineClip, areas to
    // area), so an off-window predecessor or an off-scale spike enters the
    // view at its true slope instead of being bent by clamping the value.
    // Screen y is only bounded far outside the plot to keep extreme values
    // finite; that moves an edge crossing by a negligible fraction of a pixel.
    struct Run {
        QPolygonF points;
        std::vector<char> anchors; // dash pattern restarts (dashed series)
    };
    const double yLimit = 1e7 * std::max(1.0, plot.height());
    // Dashes restart at fixed times, so they move with the data.
    const double quantum = m_window / 16;
    std::vector<std::vector<Run>> runs(m_series.size());
    int filled = 0;
    for (std::size_t si = 0; si < m_series.size(); ++si) {
        const auto& cs = m_series[si];
        const auto* s = m_history->find(cs.key);
        if (!s)
            continue;
        filled += cs.fill;
        const bool dashed = styleOf(cs, visualOf(cs, si)) != Qt::SolidLine;
        auto& out = runs[si];
        Run run;
        auto endRun = [&] {
            if (!run.points.isEmpty())
                out.push_back(std::move(run));
            run = Run();
        };
        for (std::size_t i = start; i <= last; ++i) {
            if (i > start && !m_history->continuous(i))
                endRun();
            const double v = m_history->value(*s, i);
            const double y = ps::isValid(v) ? yOf(v) : ps::kNaN;
            if (std::isnan(y)) {
                endRun();
                continue;
            }
            const double t = m_history->time(i);
            run.points.append(QPointF(xOf(t), std::clamp(y, plot.top() - yLimit, plot.bottom() + yLimit)));
            if (dashed)
                run.anchors.push_back(i > 0 && std::floor(t / quantum) != std::floor(m_history->time(i - 1) / quantum));
        }
        endRun();
    }

    // Areas first, all under every line: each runs from its own line to the
    // baseline (zero for non-negative units, the plot bottom for
    // temperatures), inside the plot and only where the line is drawn. The
    // line covers the area's (unsmoothed) top edge.
    const double baseValue = m_unit == ps::Unit::Celsius ? lo : std::clamp(0.0, lo, hi);
    const double yBase = yOf(baseValue);
    const double areaAlpha = (dark ? 0.26 : 0.20) / std::sqrt(static_cast<double>(std::clamp(filled, 1, 4)));
    p.setPen(Qt::NoPen);
    for (std::size_t si = 0; si < m_series.size(); ++si) {
        if (!m_series[si].fill || band.width() <= 0)
            continue;
        QColor c = seriesColor(visualOf(m_series[si], si), pal);
        c.setAlphaF(static_cast<float>(areaAlpha));
        p.setBrush(c);
        for (const auto& run : runs[si]) {
            if (run.points.size() < 2)
                continue;
            QPolygonF shape = run.points;
            shape.append(QPointF(run.points.back().x(), yBase));
            shape.append(QPointF(run.points.front().x(), yBase));
            shape = clipPolygon(shape, band);
            if (shape.size() >= 3)
                p.drawPolygon(shape);
        }
    }

    const double width = m_mode == Mode::Full ? 1.7 : 1.3;
    for (std::size_t si = 0; si < m_series.size(); ++si) {
        const auto& cs = m_series[si];
        const int visual = visualOf(cs, si);
        const QColor c = seriesColor(visual, pal);
        for (const auto& run : runs[si]) {
            if (run.points.size() == 1) {
                // A sample with no drawable neighbour is shown as a dot.
                if (band.contains(run.points.front())) {
                    p.setRenderHint(QPainter::Antialiasing, true);
                    p.setPen(Qt::NoPen);
                    p.setBrush(c);
                    p.drawEllipse(run.points.front(), 2.0, 2.0);
                }
                continue;
            }
            strokeTrace(p, run.points, run.anchors, c, width, styleOf(cs, visual), lineClip);
        }
    }
}

void TimeChart::invalidateLayer()
{
    m_layer.valid = false;
    m_statsValid = false;
}

long long TimeChart::layerKey(double tEnd) const
{
    const DataLayer& g = m_layer;
    const double key = std::round((g.left - (tEnd - g.end) * g.pxPerSecond) * g.phases);
    if (!std::isfinite(key) || std::fabs(key) > 1e15)
        return std::numeric_limits<long long>::min();
    return static_cast<long long>(key);
}

bool TimeChart::layerCovers(long long key) const
{
    const DataLayer& g = m_layer;
    if (!g.valid || g.phases <= 0 || key == std::numeric_limits<long long>::min())
        return false;
    // Phase images start at the plot's left edge for the axis end they were
    // drawn for and extend past its right edge: they cover later axis ends
    // until that margin is used up, never earlier ones.
    const long long whole = floorDiv(key, g.phases);
    return whole <= g.left && whole >= g.left - (g.width - (g.right - g.left));
}

bool TimeChart::prepareLayer(const QTransform& device, const QRectF& plot, const Range& range,
                             const std::vector<double>& gridYs, double tEnd, std::size_t start)
{
    const double sx = device.m11(), sy = device.m22(), ox = device.dx(), oy = device.dy();
    const double pxPerSecond = plot.width() * sx / m_window;
    const double l = plot.left() * sx + ox, r = plot.right() * sx + ox;
    const double t = plot.top() * sy + oy, b = (plot.bottom() + 1) * sy + oy;
    // Device coordinates must stay far from int overflow. The axis end is a
    // time on the history clock, which grows with uptime: the images only
    // use times relative to it, so it just has to be finite.
    for (double v : {pxPerSecond, l, r, t, b})
        if (!std::isfinite(v) || std::fabs(v) > 1e7)
            return false;
    if (!(pxPerSecond > 0) || !std::isfinite(tEnd))
        return false;
    DataLayer& g = m_layer;
    const bool same = g.valid && g.sx == sx && g.sy == sy && g.ox == ox && g.oy == oy && g.plot == plot &&
                      g.lo == range.lo && g.hi == range.hi && g.window == m_window && g.history == m_history &&
                      g.samples == m_history->totalSamples() && g.size == m_history->size() &&
                      g.latest == m_history->latestTime() && g.gridYs == gridYs;
    // The same content for another axis end or first sample: the rows it
    // draws from are recomputed (a sample that left the window no longer
    // reaches the plot), while the band and edges keep what the screen may
    // still show from the ends it was painted for.
    auto merge = [&g](const Extent& e) {
        Extent& m = g.extent;
        m.top = e.top;
        m.bandTop = std::min(m.bandTop, e.bandTop);
        m.bandBottom = std::max(m.bandBottom, e.bandBottom);
        for (const auto& edge : e.edges) {
            const bool known = std::any_of(m.edges.begin(), m.edges.end(), [&](const Extent::Edge& x) {
                return x.time == edge.time && x.full == edge.full;
            });
            if (!known)
                m.edges.push_back(edge);
        }
        m.banded = m.banded && e.banded && m.edges.size() <= kMaxEdges;
    };
    if (same && layerCovers(layerKey(tEnd))) {
        if (start != g.extentStart) {
            merge(dataExtent(plot, range.lo, range.hi, tEnd, start));
            g.extentStart = start;
        }
        return true;
    }
    const int left = static_cast<int>(std::lround(l)), right = static_cast<int>(std::lround(r));
    const int top = static_cast<int>(std::lround(t)), bottom = static_cast<int>(std::lround(b));
    if (right <= left || bottom <= top)
        return false;
    g.sx = sx;
    g.sy = sy;
    g.ox = ox;
    g.oy = oy;
    g.plot = plot;
    g.left = left;
    g.right = right;
    g.top = top;
    g.bottom = bottom;
    // Room to scroll for about three seconds before the images are redrawn
    // (a new sample redraws them anyway).
    g.width = right - left + std::clamp(static_cast<int>(std::ceil(pxPerSecond * 3)) + 8, 16, 1024);
    g.pxPerSecond = pxPerSecond;
    // Quarter-pixel steps when the plot moves fast enough for them to give
    // about 60 positions a second, eighths when it moves slower. Small
    // multiples step in quarter pixels.
    g.phases = (m_mode == Mode::Compact || pxPerSecond * 4 >= 50) ? 4 : 8;
    g.end = tEnd;
    g.start = start;
    g.lo = range.lo;
    g.hi = range.hi;
    g.window = m_window;
    g.history = m_history;
    g.samples = m_history->totalSamples();
    g.size = m_history->size();
    g.latest = m_history->latestTime();
    g.gridYs = gridYs;
    if (same) {
        merge(dataExtent(plot, range.lo, range.hi, tEnd, start));
    } else {
        g.extent = dataExtent(plot, range.lo, range.hi, tEnd, start);
        g.content = ++m_contentSerial;
    }
    g.extentStart = start;
    g.images.resize(static_cast<std::size_t>(g.phases));
    g.drawn.assign(static_cast<std::size_t>(g.phases), 0);
    g.valid = true;
    return true;
}

TimeChart::Extent TimeChart::dataExtent(const QRectF& plot, double lo, double hi, double tEnd,
                                        std::size_t start) const
{
    auto yOf = [&](double v) { return plot.bottom() - (v * 0.5 - lo * 0.5) / (hi * 0.5 - lo * 0.5) * plot.height(); };
    auto xOf = [&](double t) { return plot.right() - (tEnd - t) / m_window * plot.width(); };
    Extent e;
    // Unknown: everything.
    e.bandTop = plot.top() - 1;
    e.bandBottom = plot.bottom() + 2;
    e.base = yOf(m_unit == ps::Unit::Celsius ? lo : std::clamp(0.0, lo, hi));
    if (!m_history || m_history->empty()) {
        e.top = plot.bottom();
        return e;
    }
    const std::size_t last = m_history->size() - 1;
    // Samples more than a few pixels left of the plot, apart from the
    // predecessor of the first that is not, draw nothing inside it; they only
    // move further left as the view advances.
    std::size_t from = start;
    while (from + 1 <= last && xOf(m_history->time(from + 1)) < plot.left() - 3)
        ++from;
    // An edge left of the plot's left edge stays there.
    auto addEdge = [&](double t, bool full) {
        const bool known = std::any_of(e.edges.begin(), e.edges.end(),
                                       [&](const Extent::Edge& x) { return x.time == t && x.full == full; });
        if (!known && xOf(t) > plot.left() - 4)
            e.edges.push_back({t, full});
    };
    // Outage shading spans the whole plot.
    bool shaded = false;
    for (std::size_t i = std::max<std::size_t>(from, 1); i <= last; ++i) {
        if (!m_history->continuous(i)) {
            shaded = true;
            addEdge(m_history->time(i - 1), true);
            addEdge(m_history->time(i), true);
        }
    }
    double high = INFINITY, low = -INFINITY;
    for (const auto& cs : m_series) {
        const auto* s = m_history->find(cs.key);
        if (!s)
            continue;
        // An area's run of two or more points has vertical edges at its ends.
        std::size_t runLength = 0;
        double runStart = 0, runEnd = 0;
        auto endRun = [&] {
            if (cs.fill && runLength >= 2) {
                addEdge(runStart, false);
                addEdge(runEnd, false);
            }
            runLength = 0;
        };
        for (std::size_t i = from; i <= last; ++i) {
            if (i > from && !m_history->continuous(i))
                endRun();
            const double v = m_history->value(*s, i);
            if (!ps::isValid(v)) {
                endRun();
                continue;
            }
            const double y = yOf(v);
            high = std::min(high, y);
            low = std::max(low, y);
            if (runLength++ == 0)
                runStart = m_history->time(i);
            runEnd = m_history->time(i);
        }
        endRun();
    }
    // Lines, dots and their antialiasing reach a few pixels past a point.
    if (std::isfinite(high) && std::isfinite(low)) {
        e.bandTop = std::clamp(high - 4, plot.top() - 1, plot.bottom() + 2);
        e.bandBottom = std::clamp(low + 4, e.bandTop, plot.bottom() + 2);
        e.banded = e.edges.size() <= kMaxEdges;
    }
    if (shaded) {
        e.top = plot.top();
        return e;
    }
    // Areas reach from the lines to their baseline.
    if (m_unit != ps::Unit::Celsius && std::isfinite(high))
        high = std::min(high, yOf(std::clamp(0.0, lo, hi)));
    e.top = std::isfinite(high) ? std::max(plot.top(), high - 4) : plot.bottom();
    return e;
}

void TimeChart::drawLayerPhase(int phase)
{
    const PlotColors colors = plotColors(palette(), m_series);
    DataLayer& g = m_layer;
    QImage& image = g.images[static_cast<std::size_t>(phase)];
    const QSize size(g.width, g.bottom - g.top);
    if (image.size() != size)
        image = QImage(size, QImage::Format_RGB32);
    g.drawn[static_cast<std::size_t>(phase)] = 1;
    if (image.isNull())
        return;
    image.fill(colors.base);
    QPainter q(&image);
    const double shift = static_cast<double>(phase) / g.phases;
    q.setTransform(QTransform(g.sx, 0, 0, g.sy, g.ox - g.left + shift, g.oy - g.top));
    // The widget-coordinate span of the image, a little beyond its edges.
    const double x0 = (g.left - shift - g.ox) / g.sx - 1;
    const double x1 = (g.left + g.width - shift - g.ox) / g.sx + 1;
    const QRectF area(x0, g.plot.top(), x1 - x0, g.plot.height());
    paintPlotBase(q, g.plot, x0, x1, g.gridYs, colors);
    paintData(q, g.plot, g.lo, g.hi, g.end, g.start, area, area.adjusted(-2, -2, 2, 2));
}

void TimeChart::mouseMoveEvent(QMouseEvent* event)
{
    if (m_mode != Mode::Full)
        return;
    const QPointF pos = event->position();
    if (m_lastPlot.contains(pos)) {
        m_cursorFollowsMouse = true;
        m_mouseX = pos.x();
        const QRegion dirty = m_paintedCursor + cursorFootprint();
        if (!dirty.isEmpty())
            update(dirty);
    } else if (m_cursorFollowsMouse) {
        clearCursor();
    }
}

void TimeChart::leaveEvent(QEvent*)
{
    if (m_cursorFollowsMouse)
        clearCursor();
}

void TimeChart::keyPressEvent(QKeyEvent* event)
{
    const double ct = cursorTime();
    if (m_mode == Mode::Full && event->key() == Qt::Key_Escape && std::isfinite(ct)) {
        clearCursor();
        return;
    }
    const int key = event->key();
    const bool navigation = key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Home || key == Qt::Key_End;
    // Only samples inside the window can be inspected.
    if (m_mode != Mode::Full || !navigation || !hasVisibleSamples()) {
        QWidget::keyPressEvent(event);
        return;
    }
    const std::size_t first = firstVisibleIndex(), last = m_history->size() - 1;
    const auto nearest = nearestIndex(ct);
    std::size_t i = nearest.value_or(last);
    switch (key) {
    case Qt::Key_Left:
        if (nearest && i > first)
            --i;
        break;
    case Qt::Key_Right:
        if (i < last)
            ++i;
        break;
    case Qt::Key_Home: i = first; break;
    default: i = last; break; // End
    }
    setCursorTime(m_history->time(i));
}

void TimeChart::focusOutEvent(QFocusEvent* event)
{
    if (!m_cursorFollowsMouse) {
        m_cursorTime = ps::kNaN;
        updateAccessibleDescription();
    }
    QWidget::focusOutEvent(event);
    update();
}

void TimeChart::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange ||
        event->type() == QEvent::StyleChange) {
        // Cached colours (the data layer) and text metrics are stale.
        invalidateLayer();
        m_layoutValid = false;
        if (event->type() != QEvent::PaletteChange)
            updateGeometry();
        update();
    }
    QWidget::changeEvent(event);
}

void TimeChart::hideEvent(QHideEvent* event)
{
    // Hidden charts keep no image memory.
    m_layer = DataLayer{};
    QWidget::hideEvent(event);
}

std::pair<double, double> TimeChart::displayedRange() const
{
    const bool haveData = m_history && !m_history->empty();
    const std::size_t first = haveData ? firstVisibleIndex() : 0;
    const std::size_t last = haveData ? m_history->size() - 1 : 0;
    const Range r = haveData ? computeRange(first, last) : computeRange(1, 0);
    return {r.lo, r.hi};
}
