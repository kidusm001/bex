#include "app/timechart.h"

#include "core/format.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr double kMargin = 8;
constexpr double kSwatch = 22;

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
    static const std::array<Qt::PenStyle, 4> styles = {Qt::SolidLine, Qt::DashLine, Qt::DotLine, Qt::DashDotLine};
    const int i = index < 0 ? 0 : index;
    return styles[static_cast<std::size_t>((i + i / 8) % 4)];
}

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
    refresh();
}

void TimeChart::setSeries(std::vector<ChartSeries> series)
{
    m_series = std::move(series);
    refresh();
}

void TimeChart::setUnit(ps::Unit unit)
{
    m_unit = unit;
    update();
}

void TimeChart::setFixedRange(double min, double max)
{
    // Invalid bounds (non-finite, empty or inverted) fall back to automatic.
    if (std::isfinite(min) && std::isfinite(max) && max > min) {
        m_fixedMin = min;
        m_fixedMax = max;
    } else {
        m_fixedMin = ps::kNaN;
        m_fixedMax = ps::kNaN;
    }
    update();
}

void TimeChart::setAutoRange()
{
    m_fixedMin = ps::kNaN;
    m_fixedMax = ps::kNaN;
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
    update();
}

void TimeChart::refresh()
{
    if (m_mode == Mode::Full)
        updateAccessibleDescription();
    update();
}

QSize TimeChart::sizeHint() const
{
    return m_mode == Mode::Full ? QSize(520, 240) : QSize(180, 84);
}

QSize TimeChart::minimumSizeHint() const
{
    return m_mode == Mode::Full ? QSize(260, 150) : QSize(110, 64);
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
    if (m_mode == Mode::Full)
        updateAccessibleDescription();
    update();
}

void TimeChart::clearCursor()
{
    m_cursorFollowsMouse = false;
    m_cursorTime = ps::kNaN;
    if (m_mode == Mode::Full)
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
    // sample, recomputed on every refresh, exactly as the readout shows it.
    QString d = readoutText();
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
    }
    if (d != accessibleDescription())
        setAccessibleDescription(d);
}

TimeChart::Range TimeChart::computeRange(std::size_t first, std::size_t last) const
{
    Range r;
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
    int rows = m_series.empty() ? 0 : 1;
    for (const auto& cs : m_series) {
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
    const double top = l.header.bottom() + 6;
    const double bottom = area.bottom() - fm.height() - 4;
    l.plot = QRectF(area.left() + axisW, top, std::max(10.0, area.width() - axisW), std::max(10.0, bottom - top));
    return l;
}

void TimeChart::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QPalette pal = palette();
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    QColor muted = pal.color(QPalette::PlaceholderText);
    if (!muted.isValid() || muted == text)
        muted = pal.color(QPalette::Disabled, QPalette::Text);
    QColor grid = text;
    grid.setAlphaF(isDarkPalette(pal) ? 0.13f : 0.10f);
    QColor frame = text;
    frame.setAlphaF(0.22f);

    p.fillRect(rect(), base);
    const QFontMetrics fm(font());
    const Layout l = computeLayout(fm);
    m_lastPlot = l.plot;
    m_lastPainted = 0;

    const bool haveData = m_history && !m_history->empty();
    const std::size_t first = haveData ? firstVisibleIndex() : 0;
    const std::size_t last = haveData ? m_history->size() - 1 : 0;
    const std::size_t start = (haveData && first > 0) ? first - 1 : first;
    const double tEnd = endTime();
    // No sample in the window: no line, no current value and no readout.
    const bool inWindow = haveData && first <= last;

    // Header: title, notice and legend with current values.
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

    if (m_mode == Mode::Full) {
        const double rowH = fm.height() + 2;
        const double valueReserve = fm.horizontalAdvance(QStringLiteral("000 MiB/s"));
        double x = 0;
        int row = 1;
        for (std::size_t si = 0; si < m_series.size(); ++si) {
            const auto& cs = m_series[si];
            const int visual = cs.visual >= 0 ? cs.visual : static_cast<int>(si);
            const QString label = cs.label + QStringLiteral(": ");
            const double w = kSwatch + 6 + fm.horizontalAdvance(label) + valueReserve + 16;
            if (x > 0 && x + w > l.header.width()) {
                ++row;
                x = 0;
            }
            const double y = l.header.top() + row * rowH;
            const double cy = y + rowH / 2;
            QPen pen(seriesColor(visual, pal), 2.2, seriesStyle(visual), Qt::FlatCap);
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

    const QRectF plot = l.plot;
    const Range range = computeRange(first, last);
    // Halved operands keep the fraction finite even for bounds near +-DBL_MAX.
    auto yOf = [&](double v) {
        return plot.bottom() - (v * 0.5 - range.lo * 0.5) / (range.hi * 0.5 - range.lo * 0.5) * plot.height();
    };
    auto xOf = [&](double t) { return plot.right() - (tEnd - t) / m_window * plot.width(); };

    // Value grid.
    p.setPen(QPen(grid, 1));
    const int maxLines = 12;
    int lines = 0;
    // Grid lines sit on multiples of the step, also for fixed ranges whose
    // bounds are not multiples of it.
    const double gridStart = std::ceil(range.lo / range.step - 1e-9) * range.step;
    for (double v = gridStart; v <= range.hi + range.step * 1e-6 && lines < maxLines; v += range.step, ++lines) {
        const double y = std::round(yOf(v)) + 0.5;
        p.setPen(QPen(grid, 1));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        if (m_mode == Mode::Full) {
            p.setPen(muted);
            p.drawText(QRectF(0, y - fm.height() / 2.0, plot.left() - 6, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                       axisLabel(v, range));
        }
    }

    // Time grid.
    if (m_mode == Mode::Full) {
        static const std::array<double, 10> steps = {5, 10, 15, 30, 60, 120, 180, 300, 600, 900};
        const double labelW = fm.horizontalAdvance(QStringLiteral("\u221215 min")) * 1.6;
        const int maxTicks = std::max(2, static_cast<int>(plot.width() / labelW));
        double step = steps.back();
        for (double s : steps) {
            if (m_window / s <= maxTicks) {
                step = s;
                break;
            }
        }
        for (double ago = 0; ago <= m_window + 1e-6; ago += step) {
            const double x = std::round(plot.right() - ago / m_window * plot.width()) + 0.5;
            p.setPen(QPen(grid, 1));
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            p.setPen(muted);
            const QString label = relTime(ago);
            const double w = fm.horizontalAdvance(label);
            double lx = x - w / 2;
            lx = std::clamp(lx, plot.left(), plot.right() - w);
            p.drawText(QRectF(lx, plot.bottom() + 3, w + 2, fm.height()), Qt::AlignLeft | Qt::AlignTop, label);
        }
    }

    p.setPen(QPen(frame, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(plot.adjusted(0.5, 0.5, -0.5, -0.5));

    if (inWindow) {
        p.save();
        p.setClipRect(plot.adjusted(0, 0, 0, 1));

        // Shade discontinuities (pause, suspend, collector outage) so the
        // missing interval reads as "no data", not as a flat line.
        QColor gapColor = text;
        gapColor.setAlphaF(0.06f);
        for (std::size_t i = std::max<std::size_t>(start, 1); i <= last; ++i) {
            if (m_history->continuous(i))
                continue;
            const double x0 = xOf(m_history->time(i - 1));
            const double x1 = xOf(m_history->time(i));
            if (x1 - x0 >= 2)
                p.fillRect(QRectF(x0, plot.top(), x1 - x0, plot.height()), gapColor);
        }

        p.setRenderHint(QPainter::Antialiasing, true);
        const double width = m_mode == Mode::Full ? 1.7 : 1.3;
        // Segments keep their measured geometry and are clipped to the plot
        // (plus a pen width), so an off-window predecessor or an off-scale
        // spike enters the view at its true slope instead of being bent by
        // clamping the value. Screen y is only bounded far outside the plot
        // to keep extreme values finite; that moves an edge crossing by a
        // negligible fraction of a pixel.
        const QRectF clipRect = plot.adjusted(-2, -2, 2, 2);
        const double yLimit = 1e7 * std::max(1.0, plot.height());
        QPolygonF run;
        run.reserve(static_cast<qsizetype>(last - start + 1));
        for (std::size_t si = 0; si < m_series.size(); ++si) {
            const auto& cs = m_series[si];
            const auto* s = m_history->find(cs.key);
            if (!s)
                continue;
            const int visual = cs.visual >= 0 ? cs.visual : static_cast<int>(si);
            const QColor c = seriesColor(visual, pal);
            QPen pen(c, width, seriesStyle(visual), Qt::FlatCap, Qt::RoundJoin);
            auto endRun = [&] {
                if (run.size() >= 2) {
                    p.setPen(pen);
                    p.setBrush(Qt::NoBrush);
                    p.drawPolyline(run);
                }
                run.clear();
            };
            bool havePrev = false, isolated = false;
            QPointF prev;
            // A sample with no drawable neighbour is shown as a dot.
            auto breakLine = [&] {
                endRun();
                if (havePrev && isolated && plot.contains(prev)) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(c);
                    p.drawEllipse(prev, 2.0, 2.0);
                }
                havePrev = false;
            };
            for (std::size_t i = start; i <= last; ++i) {
                if (i > start && !m_history->continuous(i))
                    breakLine();
                const double v = m_history->value(*s, i);
                if (!ps::isValid(v)) {
                    breakLine();
                    continue;
                }
                const double y = yOf(v);
                if (std::isnan(y)) {
                    breakLine();
                    continue;
                }
                const QPointF pt(xOf(m_history->time(i)), std::clamp(y, plot.top() - yLimit, plot.bottom() + yLimit));
                if (i >= first)
                    ++m_lastPainted;
                if (havePrev) {
                    QPointF a = prev, b = pt;
                    if (clipSegment(a, b, clipRect)) {
                        if (run.isEmpty() || QLineF(run.back(), a).length() > 0.01) {
                            endRun();
                            run.append(a);
                        }
                        run.append(b);
                    } else {
                        endRun();
                    }
                    isolated = false;
                } else {
                    isolated = true;
                }
                prev = pt;
                havePrev = true;
            }
            breakLine();
        }
        p.restore();
    }

    if (!hasVisibleData()) {
        p.setPen(muted);
        p.drawText(plot.adjusted(6, 4, -6, -4), Qt::AlignCenter | Qt::TextWordWrap, emptyMessage());
    }

    // Readout cursor.
    const double ct = cursorTime();
    const auto nearest = m_mode == Mode::Full ? nearestIndex(ct) : std::nullopt;
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
                const int visual = m_series[si].visual >= 0 ? m_series[si].visual : static_cast<int>(si);
                p.setPen(QPen(base, 1.5));
                p.setBrush(seriesColor(visual, pal));
                p.drawEllipse(QPointF(x, yOf(v)), 3.5, 3.5);
            }
            const QStringList rows = readoutText().split(QLatin1Char('\n'));
            double bw = 0;
            for (const auto& r : rows)
                bw = std::max<double>(bw, fm.horizontalAdvance(r));
            bw += 16;
            const double bh = rows.size() * fm.height() + 10;
            double bx = x + 10;
            if (bx + bw > plot.right())
                bx = x - 10 - bw;
            bx = std::max(bx, plot.left());
            const QRectF box(bx, plot.top() + 6, bw, bh);
            p.setPen(QPen(frame, 1));
            p.setBrush(pal.color(QPalette::ToolTipBase));
            p.drawRoundedRect(box, 4, 4);
            p.setPen(pal.color(QPalette::ToolTipText));
            double ty = box.top() + 5;
            for (const auto& r : rows) {
                p.drawText(QRectF(box.left() + 8, ty, bw, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, r);
                ty += fm.height();
            }
        }
    }

    if (hasFocus()) {
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(pal.color(QPalette::Highlight), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1));
    }
}

void TimeChart::mouseMoveEvent(QMouseEvent* event)
{
    if (m_mode != Mode::Full)
        return;
    const QPointF pos = event->position();
    if (m_lastPlot.contains(pos)) {
        m_cursorFollowsMouse = true;
        m_mouseX = pos.x();
        update();
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
        if (m_mode == Mode::Full)
            updateAccessibleDescription();
    }
    QWidget::focusOutEvent(event);
    update();
}

void TimeChart::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange ||
        event->type() == QEvent::StyleChange)
        update();
    QWidget::changeEvent(event);
}

std::pair<double, double> TimeChart::displayedRange() const
{
    const bool haveData = m_history && !m_history->empty();
    const std::size_t first = haveData ? firstVisibleIndex() : 0;
    const std::size_t last = haveData ? m_history->size() - 1 : 0;
    const Range r = haveData ? computeRange(first, last) : computeRange(1, 0);
    return {r.lo, r.hi};
}
