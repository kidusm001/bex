#include "app/metricbutton.h"

#include "app/timechart.h"

#include <QEvent>
#include <QHideEvent>
#include <QImage>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int kPadX = 10;
constexpr int kPadY = 7;
constexpr int kSparkW = 64;

long long floorDiv(long long a, long long b)
{
    const long long q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

double linearChannel(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor& c)
{
    return 0.2126 * linearChannel(c.redF()) + 0.7152 * linearChannel(c.greenF()) + 0.0722 * linearChannel(c.blueF());
}

double contrastRatio(const QColor& a, const QColor& b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QFont valueFont(const QFont& base)
{
    QFont f = base;
    if (f.pointSizeF() > 0)
        f.setPointSizeF(f.pointSizeF() * 1.3);
    else if (f.pixelSize() > 0)
        f.setPixelSize(std::max(1, static_cast<int>(std::lround(f.pixelSize() * 1.3))));
    f.setWeight(QFont::DemiBold);
    return f;
}

QFont compactValueFont(const QFont& base)
{
    QFont f = base;
    f.setWeight(QFont::DemiBold);
    return f;
}

QFont smallFont(const QFont& base)
{
    QFont f = base;
    if (f.pointSizeF() > 0)
        f.setPointSizeF(f.pointSizeF() * 0.9);
    else if (f.pixelSize() > 0)
        f.setPixelSize(std::max(1, static_cast<int>(std::lround(f.pixelSize() * 0.9))));
    return f;
}
} // namespace

MetricButton::MetricButton(const QString& title, QWidget* parent)
    : QAbstractButton(parent)
{
    setText(title);
    setCheckable(true);
    setAutoExclusive(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_value = QStringLiteral("\u2014");
    updateAccessibleText();
}

void MetricButton::invalidateLayout()
{
    m_layout.valid = false;
}

void MetricButton::invalidateSparkLayer()
{
    m_sparkLayer.valid = false;
}

long long MetricButton::sparkLayerKey(double tEnd) const
{
    const SparkLayer& g = m_sparkLayer;
    const double key = std::round((g.left - (tEnd - g.end) * g.pxPerSecond) * g.phases);
    if (!std::isfinite(key) || std::fabs(key) > 1e15)
        return std::numeric_limits<long long>::min();
    return static_cast<long long>(key);
}

bool MetricButton::sparkLayerCovers(long long key) const
{
    const SparkLayer& g = m_sparkLayer;
    if (!g.valid || g.phases <= 0 || key == std::numeric_limits<long long>::min())
        return false;
    const long long whole = floorDiv(key, g.phases);
    return whole <= g.left && whole >= g.left - (g.width - (g.right - g.left));
}

void MetricButton::setValueText(const QString& value, const QString& accessibleValue)
{
    if (value == m_value && accessibleValue == m_accessibleValue)
        return;
    m_value = value;
    m_accessibleValue = accessibleValue;
    invalidateLayout();
    updateAccessibleText();
    update();
}

void MetricButton::setDetailText(const QString& detail)
{
    if (detail == m_detail)
        return;
    m_detail = detail;
    invalidateLayout();
    updateAccessibleText();
    update();
}

void MetricButton::setValueTemplate(const QString& widest)
{
    if (widest == m_valueTemplate)
        return;
    m_valueTemplate = widest;
    invalidateLayout();
    updateGeometry();
    update();
}

void MetricButton::setSparkline(const ps::History* history, std::vector<std::string> keys, double maxValue,
                                double seconds)
{
    if (keys.size() > 2)
        keys.resize(2);
    if (history == m_history && keys == m_keys && maxValue == m_max && seconds == m_seconds)
        return;
    m_history = history;
    m_keys = std::move(keys);
    m_max = maxValue;
    m_seconds = seconds;
    m_rangeValid = false;
    invalidateSparkLayer();
    invalidateLayout();
    update();
}

void MetricButton::setSparklineMax(double maxValue)
{
    if (maxValue == m_max)
        return;
    m_max = maxValue;
    m_rangeValid = false;
    invalidateSparkLayer();
    update();
}

void MetricButton::setSparklineVisuals(std::vector<std::pair<int, Qt::PenStyle>> visuals)
{
    if (visuals == m_visuals)
        return;
    m_visuals = std::move(visuals);
    invalidateSparkLayer();
    update();
}

void MetricButton::setViewEnd(double time)
{
    m_viewEnd = std::isfinite(time) ? time : ps::kNaN;
}

QRect MetricButton::sparkUpdateRect() const
{
    return m_sparkRect.adjusted(-2, -2, 2, 2).toAlignedRect().intersected(rect());
}

double MetricButton::axisEndTime(double tNow) const
{
    if (m_history && !m_history->empty() && m_seconds > 0
        && m_history->totalSamples() == m_history->size()
        && tNow < m_history->time(0) + m_seconds) {
        return m_history->time(0) + m_seconds;
    }
    return tNow;
}

void MetricButton::advanceView(double time)
{
    if (!std::isfinite(time))
        return;
    m_viewEnd = time;
    if (!isVisible() || !m_history || m_keys.empty() || m_history->empty())
        return;
    if (!(m_sparkRect.width() > 0) || !std::isfinite(m_paintedEnd)) {
        update();
        return;
    }
    const double tNow = std::max(m_viewEnd, m_history->latestTime());
    const double tEnd = axisEndTime(tNow);
    const std::size_t first = m_history->lowerBound(tNow - m_seconds);
    if (first != m_paintedFirst) {
        update(sparkUpdateRect());
        return;
    }
    if (!m_paintedMoving)
        return;
    bool moved;
    if (m_paintedSparkLayer && m_sparkLayer.valid)
        moved = sparkLayerKey(tEnd) != m_paintedSparkKey;
    else
        moved = std::fabs(tEnd - m_paintedEnd) / m_seconds * m_sparkRect.width() * devicePixelRatioF() >= 0.2;
    if (moved)
        update(sparkUpdateRect());
}

void MetricButton::setDetailLineVisible(bool visible)
{
    if (visible == m_detailLine)
        return;
    m_detailLine = visible;
    invalidateLayout();
    invalidateSparkLayer();
    m_panelValid = false;
    updateGeometry();
    update();
}

void MetricButton::setStale(bool stale)
{
    if (stale == m_stale)
        return;
    m_stale = stale;
    invalidateLayout();
    invalidateSparkLayer();
    updateAccessibleText();
    update();
}

void MetricButton::updateAccessibleText()
{
    setAccessibleName(text());
    QString d = m_accessibleValue.isEmpty() ? m_value : m_accessibleValue;
    if (!m_detail.isEmpty())
        d += QStringLiteral(". ") + m_detail;
    if (m_stale)
        d += QStringLiteral(". No recent samples.");
    setAccessibleDescription(d);
    setToolTip(text() + QStringLiteral(": ") + d);
}

QSize MetricButton::sizeHint() const
{
    const QFontMetrics fm(font());
    const QFontMetrics vfm(valueFont(font()));
    const QFontMetrics sfm(smallFont(font()));
    const int h = kPadY * 2 + fm.height() + vfm.height() + (m_detailLine ? sfm.height() + 2 : 0);
    int w = std::max(fm.horizontalAdvance(QStringLiteral("Temperatures")) + kSparkW + 8,
                     sfm.horizontalAdvance(QStringLiteral("0000 MiB/s up  0000 MiB/s down")));
    // Wide enough for the template in the compact font at least.
    if (!m_valueTemplate.isEmpty())
        w = std::max(w, QFontMetrics(compactValueFont(font())).horizontalAdvance(m_valueTemplate) + 2);
    return {kPadX * 2 + w, h};
}

QSize MetricButton::minimumSizeHint() const
{
    return {kPadX * 2 + QFontMetrics(font()).horizontalAdvance(QStringLiteral("Temperatures")), sizeHint().height()};
}

void MetricButton::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange ||
        event->type() == QEvent::StyleChange) {
        m_contrastValid = false;
        m_panelValid = false;
        invalidateLayout();
        invalidateSparkLayer();
        updateGeometry();
        update();
    }
    QAbstractButton::changeEvent(event);
}

void MetricButton::resizeEvent(QResizeEvent* event)
{
    m_panelValid = false;
    invalidateLayout();
    invalidateSparkLayer();
    QAbstractButton::resizeEvent(event);
}

void MetricButton::hideEvent(QHideEvent* event)
{
    m_sparkLayer = SparkLayer{};
    m_panelCache = QImage();
    m_panelValid = false;
    QAbstractButton::hideEvent(event);
}

void MetricButton::ensureLayout() const
{
    const QFont base = font();
    const bool hasSpark = m_history && !m_keys.empty();
    if (m_layout.valid && m_layout.size == size() && m_layout.baseFont == base && m_layout.hasSpark == hasSpark)
        return;
    m_layout.size = size();
    m_layout.baseFont = base;
    m_layout.hasSpark = hasSpark;

    const QFont vf = valueFont(base);
    m_layout.smallFont = smallFont(base);
    const QFontMetrics fm(base), vfm(vf), sfm(m_layout.smallFont);
    const int x = kPadX;
    int y = kPadY;
    const int textW = width() - 2 * kPadX;
    const int besideW = textW - kSparkW - 6;

    // Layout: the sparkline spans the title and value rows when the widest
    // expected value fits beside it; otherwise it shrinks to the title row
    // and the value gets the full width, in a smaller font if it must.
    const QString widest = m_valueTemplate.isEmpty() ? m_value : m_valueTemplate;
    bool sparkBesideValue = true;
    QFont valueDrawFont = vf;
    if (hasSpark && vfm.horizontalAdvance(widest) > besideW) {
        sparkBesideValue = false;
        if (vfm.horizontalAdvance(widest) > textW)
            valueDrawFont = compactValueFont(base);
    } else if (!hasSpark && vfm.horizontalAdvance(widest) > textW) {
        valueDrawFont = compactValueFont(base);
    }
    const QFontMetrics valueFm(valueDrawFont);
    const int valueW = sparkBesideValue && hasSpark ? besideW : textW;

    m_layout.sparkBesideValue = sparkBesideValue;
    m_layout.valueDrawFont = valueDrawFont;
    m_layout.titleRect = QRect(x, y, hasSpark ? besideW : textW, fm.height());
    m_layout.titleElided = fm.elidedText(this->text(), Qt::ElideRight, m_layout.titleRect.width());
    y += fm.height();

    m_layout.valueRect = QRect(x, y, valueW, vfm.height());
    m_layout.valueElided = valueFm.elidedText(m_value, Qt::ElideRight, valueW);
    y += vfm.height() + 2;

    m_layout.detailRect = QRect(x, y, textW, sfm.height());
    m_layout.detailElided = sfm.elidedText(m_stale ? QStringLiteral("No recent samples") : m_detail, Qt::ElideRight,
                                           textW);
    m_layout.sparkRect = QRectF(width() - kPadX - kSparkW, kPadY + 2, kSparkW,
                                sparkBesideValue ? fm.height() + vfm.height() - 6 : fm.height() - 3);
    m_layout.valid = true;
}

QColor MetricButton::selectedTextColor(const QStyleOptionViewItem& opt, QPalette::ColorGroup group) const
{
    const QPalette& pal = opt.palette;
    const QStyle* st = style();
    const bool down = isDown();
    const bool focused = hasFocus();
    if (m_contrastValid && m_contrastStyle == st && m_contrastPaletteKey == pal.cacheKey()
        && m_contrastGroup == group && m_contrastDown == down && m_contrastFocused == focused)
        return m_contrastColor;
    // Some styles (e.g. Kvantum themes with translucent selection panels) paint
    // PE_PanelItemViewItem much darker or lighter than QPalette::Highlight,
    // leaving QPalette::HighlightedText with poor contrast. Probe the actual
    // rendered panel once per style/palette/state change and fall back to
    // WindowText when HighlightedText would be unreadable.
    QImage probe(16, 16, QImage::Format_ARGB32_Premultiplied);
    probe.fill(pal.color(QPalette::Window));
    QStyleOptionViewItem probeOpt = opt;
    probeOpt.rect = probe.rect();
    probeOpt.state &= ~QStyle::State_MouseOver;
    {
        QPainter qp(&probe);
        st->drawPrimitive(QStyle::PE_PanelItemViewItem, &probeOpt, &qp, this);
    }
    const QColor bg = probe.pixelColor(8, 8);
    const QColor hiText = pal.color(group, QPalette::HighlightedText);
    const QColor winText = pal.color(group, QPalette::WindowText);
    const double hiContrast = contrastRatio(hiText, bg);
    const double winContrast = contrastRatio(winText, bg);
    m_contrastBg = bg;
    m_contrastColor = (hiContrast < 3.0 && winContrast > hiContrast) ? winText : hiText;
    m_contrastStyle = st;
    m_contrastPaletteKey = pal.cacheKey();
    m_contrastGroup = group;
    m_contrastDown = down;
    m_contrastFocused = focused;
    m_contrastValid = true;
    return m_contrastColor;
}

void MetricButton::paintEvent(QPaintEvent* event)
{
    QPainter p(this);
    // Frames between samples repaint only the sparkline; text elsewhere is
    // skipped when it is not in the repainted area.
    const QRect dirty = event->rect();
    ++m_paintCount;
    const QPalette pal = palette();
    const bool selected = isChecked();
    const bool down = isDown();
    const bool hovered = underMouse();
    const bool active = isActiveWindow();

    // Selection and hover are drawn by the style as a selected list item, so
    // the entry follows the desktop's Qt theme like the rest of the window.
    // Cache the rendered panel per (size, dpr, style, palette, state) so 30 Hz
    // sparkline partial repaints blit the subrect instead of re-invoking the
    // style engine (e.g. Kvantum SVG rendering).
    QStyleOptionViewItem opt;
    opt.initFrom(this);
    opt.rect = rect();
    opt.showDecorationSelected = true;
    opt.viewItemPosition = QStyleOptionViewItem::OnlyOne;
    opt.features = QStyleOptionViewItem::HasDisplay;
    opt.state &= ~QStyle::State_HasFocus; // the focus ring below is stronger
    if (selected || down)
        opt.state |= QStyle::State_Selected;
    if (hovered)
        opt.state |= QStyle::State_MouseOver;
    if (active)
        opt.state |= QStyle::State_Active;

    const unsigned panelState = (selected ? 1u : 0u) | (down ? 2u : 0u) | (hovered ? 4u : 0u)
        | (active ? 8u : 0u) | (isEnabled() ? 16u : 0u) | (hasFocus() ? 32u : 0u);
    const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : devicePixelRatioF();
    const QStyle* st = style();
    if (!m_panelValid || m_panelSize != size() || m_panelDpr != dpr || m_panelStyle != st
        || m_panelPaletteKey != pal.cacheKey() || m_panelState != panelState) {
        const QSize pxSize((QSizeF(size()) * dpr).toSize());
        if (pxSize.width() > 0 && pxSize.height() > 0) {
            if (m_panelCache.size() != pxSize)
                m_panelCache = QImage(pxSize, QImage::Format_ARGB32_Premultiplied);
            m_panelCache.setDevicePixelRatio(dpr);
            m_panelCache.fill(Qt::transparent);
            QPainter pp(&m_panelCache);
            st->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, &pp, this);
        } else {
            m_panelCache = QImage();
        }
        m_panelSize = size();
        m_panelDpr = dpr;
        m_panelStyle = st;
        m_panelPaletteKey = pal.cacheKey();
        m_panelState = panelState;
        m_panelValid = true;
    }
    if (!m_panelCache.isNull()) {
        const QRect src(dirty.intersected(rect()));
        if (!src.isEmpty()) {
            const QRectF srcPx(src.x() * dpr, src.y() * dpr, src.width() * dpr, src.height() * dpr);
            p.drawImage(QRectF(src), m_panelCache, srcPx);
        }
    }

    const QPalette::ColorGroup group = !isEnabled() ? QPalette::Disabled
                                                    : (active ? QPalette::Active : QPalette::Inactive);
    const QColor text = selected ? selectedTextColor(opt, group) : pal.color(QPalette::WindowText);
    QColor muted = pal.color(QPalette::PlaceholderText);
    if (selected) {
        muted = text;
        muted.setAlphaF(0.90f);
    } else if (!muted.isValid() || muted == text) {
        muted = pal.color(QPalette::Disabled, QPalette::WindowText);
    }

    // Crisp left-edge vertical accent indicator on the selected navigation item.
    if (selected && dirty.left() <= 6 && height() > 12) {
        QColor accent = pal.color(QPalette::Highlight);
        if (!m_visuals.empty()) {
            const QColor sc = seriesColor(m_visuals.front().first, pal);
            if (contrastRatio(sc, m_contrastBg) >= 3.0)
                accent = sc;
        }
        if (contrastRatio(accent, m_contrastBg) < 3.0)
            accent = text;
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRoundedRect(QRectF(2.0, 5.0, 3.0, height() - 10.0), 1.5, 1.5);
    }

    if (hasFocus() && !QRectF(rect()).adjusted(4, 4, -4, -4).contains(dirty)) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(selected ? text : pal.color(QPalette::Highlight), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 4, 4);
    }

    ensureLayout();
    const LayoutCache& l = m_layout;
    if (dirty.intersects(l.titleRect)) {
        p.setPen(text);
        p.setFont(l.baseFont);
        p.drawText(l.titleRect, Qt::AlignLeft | Qt::AlignVCenter, l.titleElided);
    }
    if (dirty.intersects(l.valueRect)) {
        p.setFont(l.valueDrawFont);
        p.setPen(m_stale ? muted : text);
        p.drawText(l.valueRect, Qt::AlignLeft | Qt::AlignVCenter, l.valueElided);
    }
    if (m_detailLine && dirty.intersects(l.detailRect)) {
        p.setFont(l.smallFont);
        p.setPen(muted);
        p.drawText(l.detailRect, Qt::AlignLeft | Qt::AlignVCenter, l.detailElided);
    }

    // Sparkline in the right-hand corner of the title (and value) rows.
    m_sparkRect = l.sparkRect;
    m_paintedEnd = ps::kNaN;
    m_paintedMoving = false;
    if (!l.hasSpark || m_history->empty() || !dirty.intersects(sparkUpdateRect()))
        return;
    const ps::History::Series* series[2] = {nullptr, nullptr};
    const std::size_t nSeries = std::min<std::size_t>(m_keys.size(), 2);
    for (std::size_t i = 0; i < nSeries; ++i)
        series[i] = m_history->find(m_keys[i]);
    const double tNow = std::isfinite(m_viewEnd) ? std::max(m_viewEnd, m_history->latestTime())
                                                  : m_history->latestTime();
    const double tEnd = axisEndTime(tNow);
    const std::size_t first = m_history->lowerBound(tNow - m_seconds);
    const std::size_t n = m_history->size();
    m_paintedEnd = tEnd;
    m_paintedFirst = first;
    if (first >= n)
        return;
    // The sample before the window enters it along its measured segment.
    const std::size_t from = first > 0 ? first - 1 : first;
    // Non-negative units (percent, bytes, rates) keep a zero baseline.
    // Temperatures can be negative, so they use the visible signed range.
    bool signedUnit = false;
    for (std::size_t i = 0; i < nSeries; ++i)
        if (series[i] && series[i]->unit == ps::Unit::Celsius)
            signedUnit = true;
    double lo = 0;
    double hi = m_max;
    if (hi <= 0 || signedUnit) {
        const bool rangeHit = m_rangeValid && m_rangeHistory == m_history
            && m_rangeSamples == m_history->totalSamples() && m_rangeSize == n
            && m_rangeLatest == m_history->latestTime() && m_rangeFirst == first;
        if (rangeHit) {
            lo = m_rangeLo;
            hi = m_rangeHi;
        } else {
            double vmin = std::numeric_limits<double>::infinity();
            double vmax = -std::numeric_limits<double>::infinity();
            for (std::size_t si = 0; si < nSeries; ++si) {
                const auto* s = series[si];
                if (!s)
                    continue;
                for (std::size_t i = first; i < n; ++i) {
                    const double v = m_history->value(*s, i);
                    if (ps::isValid(v)) {
                        vmin = std::min(vmin, v);
                        vmax = std::max(vmax, v);
                    }
                }
            }
            if (signedUnit) {
                if (vmin > vmax) {
                    lo = 0;
                    hi = 1;
                } else {
                    const double pad = std::max((vmax - vmin) * 0.1, 0.5);
                    lo = vmin - pad;
                    hi = vmax + pad;
                }
            } else {
                hi = vmax > 0 ? vmax * 1.1 : 1;
            }
            m_rangeValid = true;
            m_rangeHistory = m_history;
            m_rangeSamples = m_history->totalSamples();
            m_rangeSize = n;
            m_rangeLatest = m_history->latestTime();
            m_rangeFirst = first;
            m_rangeLo = lo;
            m_rangeHi = hi;
        }
    }
    const QRectF spark = m_sparkRect;
    // Points keep their measured geometry: a value off the scale (such as the
    // off-window predecessor, which the scale does not include) is clipped
    // where its segment crosses the sparkline's edge, never pulled onto the
    // edge, which would bend the segment. Screen y is only bounded far
    // outside to keep extreme values finite.
    const double yLimit = 1e7 * std::max(1.0, spark.height());
    auto yOf = [&](double v) {
        // Halved operands keep the fraction finite for any finite bounds.
        const double py = spark.bottom() - (v * 0.5 - lo * 0.5) / (hi * 0.5 - lo * 0.5) * spark.height();
        return std::clamp(py, spark.top() - yLimit, spark.bottom() + yLimit);
    };
    p.setRenderHint(QPainter::Antialiasing, false);
    // Subtle neutral backdrop + baseline/frame (zero hue so series-color tests stay unaffected).
    QColor backdrop = text;
    backdrop.setAlphaF(0.035f);
    p.fillRect(spark, backdrop);
    QColor hairline = muted;
    hairline.setAlphaF(0.14f);
    p.setPen(QPen(hairline, 1));
    p.setBrush(Qt::NoBrush);
    p.drawLine(spark.topLeft(), spark.topRight());
    p.drawLine(spark.topLeft(), spark.bottomLeft());
    p.drawLine(spark.topRight(), spark.bottomRight());
    QColor baseline = muted;
    baseline.setAlphaF(0.35f);
    p.setPen(QPen(baseline, 1));
    p.drawLine(spark.bottomLeft(), spark.bottomRight());
    p.setClipRect(spark.adjusted(0, -2, 0, 1));
    const bool dark = isDarkPalette(pal);

    auto drawTraces = [&](QPainter& tp, double layerEnd, const QRectF& areaRect, const QRectF& lineClip, bool& movedOut) {
        const double layerStart = layerEnd - m_seconds;
        for (std::size_t si = 0; si < nSeries; ++si) {
            const auto* s = series[si];
            if (!s)
                continue;
            const auto [visual, style] = si < m_visuals.size()
                ? m_visuals[si]
                : std::pair {static_cast<int>(si), seriesStyle(static_cast<int>(si))};
            // Keep the series colour on the selection when it has clear contrast
            // against the probed selection background; otherwise use the selected
            // text colour (e.g. blue series on blue Fusion selection).
            const QColor sc = seriesColor(visual, pal);
            const QColor line = m_stale
                ? muted
                : (selected ? (contrastRatio(sc, m_contrastBg) >= 3.0 ? sc : text) : sc);
            // Runs of consecutive valid samples; areas under them first. Dashes
            // restart at fixed times (anchors) so they move with the data.
            const double quantum = m_seconds / 8;
            std::vector<std::pair<QPolygonF, std::vector<char>>> runs;
            std::pair<QPolygonF, std::vector<char>> run;
            auto endRun = [&] {
                if (!run.first.isEmpty()) {
                    movedOut = true;
                    runs.push_back(std::move(run));
                }
                run = {};
            };
            for (std::size_t i = from; i < n; ++i) {
                const double v = m_history->value(*s, i);
                if (!ps::isValid(v) || (i > from && !m_history->continuous(i)))
                    endRun();
                if (!ps::isValid(v))
                    continue;
                const double t = m_history->time(i);
                const double px = spark.left() + (t - layerStart) / m_seconds * spark.width();
                run.first.append(QPointF(px, yOf(v)));
                run.second.push_back(i > 0 && std::floor(t / quantum) != std::floor(m_history->time(i - 1) / quantum));
            }
            endRun();
            if (!m_stale && si == 0) {
                QColor area = line;
                area.setAlphaF(selected ? 0.22f : (dark ? 0.24f : 0.16f));
                // Aliased: the line covers its top edge.
                tp.setRenderHint(QPainter::Antialiasing, false);
                tp.setPen(Qt::NoPen);
                tp.setBrush(area);
                for (const auto& [r, anchors] : runs) {
                    if (r.size() < 2)
                        continue;
                    QPolygonF poly = r;
                    poly.append(QPointF(r.back().x(), spark.bottom()));
                    poly.append(QPointF(r.front().x(), spark.bottom()));
                    // The area is clipped exactly like its line: above the scale
                    // it fills the whole height, below it nothing.
                    poly = clipPolygon(poly, areaRect);
                    if (poly.size() >= 3)
                        tp.drawPolygon(poly);
                }
            }
            for (const auto& [r, anchors] : runs)
                strokeTrace(tp, r, anchors, line, 1.5, style, lineClip);
        }
    };

    m_paintedSparkLayer = false;
    const QTransform device = p.deviceTransform();
    const bool aligned = device.type() <= QTransform::TxScale && device.m11() > 0 && device.m22() > 0;
    bool usedLayer = false;
    if (aligned) {
        const double sx = device.m11(), sy = device.m22(), ox = device.dx(), oy = device.dy();
        const double pxPerSecond = spark.width() * sx / m_seconds;
        const double dl = spark.left() * sx + ox, dr = spark.right() * sx + ox;
        const double dt = (spark.top() - 2) * sy + oy, db = (spark.bottom() + 1) * sy + oy;
        const bool finite = std::isfinite(pxPerSecond) && pxPerSecond > 0 && std::isfinite(tEnd)
            && std::isfinite(dl) && std::isfinite(dr) && std::isfinite(dt) && std::isfinite(db)
            && std::fabs(dl) < 1e7 && std::fabs(dr) < 1e7 && std::fabs(dt) < 1e7 && std::fabs(db) < 1e7;
        const int left = finite ? static_cast<int>(std::lround(dl)) : 0;
        const int right = finite ? static_cast<int>(std::lround(dr)) : 0;
        const int top = finite ? static_cast<int>(std::lround(dt)) : 0;
        const int bottom = finite ? static_cast<int>(std::lround(db)) : 0;
        if (finite && right > left && bottom > top) {
            SparkLayer& g = m_sparkLayer;
            const qint64 palKey = pal.cacheKey();
            const bool same = g.valid && g.sx == sx && g.sy == sy && g.ox == ox && g.oy == oy && g.spark == spark
                && g.lo == lo && g.hi == hi && g.seconds == m_seconds && g.history == m_history
                && g.samples == m_history->totalSamples() && g.size == n && g.latest == m_history->latestTime()
                && g.paletteKey == palKey && g.selected == selected && g.stale == m_stale && g.dark == dark;
            if (!same || !sparkLayerCovers(sparkLayerKey(tEnd))) {
                g.sx = sx;
                g.sy = sy;
                g.ox = ox;
                g.oy = oy;
                g.spark = spark;
                g.left = left;
                g.right = right;
                g.top = top;
                g.bottom = bottom;
                g.pxPerSecond = pxPerSecond;
                g.phases = 4;
                g.width = right - left + std::clamp(static_cast<int>(std::ceil(pxPerSecond * 3.0)) + 4, 8, 64);
                g.end = tEnd;
                g.lo = lo;
                g.hi = hi;
                g.seconds = m_seconds;
                g.history = m_history;
                g.samples = m_history->totalSamples();
                g.size = n;
                g.latest = m_history->latestTime();
                g.paletteKey = palKey;
                g.selected = selected;
                g.stale = m_stale;
                g.dark = dark;
                g.moving = false;
                g.images.resize(static_cast<std::size_t>(g.phases));
                g.drawn.assign(static_cast<std::size_t>(g.phases), 0);
                g.valid = true;
            }
            const long long key = sparkLayerKey(tEnd);
            const long long whole = floorDiv(key, g.phases);
            const int phase = static_cast<int>(key - whole * g.phases);
            const auto pidx = static_cast<std::size_t>(phase);
            QImage& img = g.images[pidx];
            if (!g.drawn[pidx]) {
                const QSize imgSize(g.width, g.bottom - g.top);
                if (img.size() != imgSize)
                    img = QImage(imgSize, QImage::Format_ARGB32_Premultiplied);
                g.drawn[pidx] = 1;
                if (!img.isNull()) {
                    img.fill(Qt::transparent);
                    QPainter qp(&img);
                    const double shift = static_cast<double>(phase) / g.phases;
                    qp.setTransform(QTransform(g.sx, 0, 0, g.sy, g.ox - g.left + shift, g.oy - g.top));
                    const double x0 = (g.left - shift - g.ox) / g.sx - 1;
                    const double x1 = (g.left + g.width - shift - g.ox) / g.sx + 1;
                    const QRectF areaRect(x0, spark.top(), x1 - x0, spark.height());
                    const QRectF lineClip(x0 - 2, spark.top() - 3, (x1 - x0) + 4, spark.height() + 5);
                    bool moved = false;
                    drawTraces(qp, g.end, areaRect, lineClip, moved);
                    g.moving = g.moving || moved;
                }
            }
            if (!img.isNull()) {
                p.save();
                p.setWorldTransform(device.inverted() * p.worldTransform());
                const QRect target(g.left, g.top, g.right - g.left, g.bottom - g.top);
                p.drawImage(target.topLeft(), img, target.translated(static_cast<int>(-whole), -g.top));
                p.restore();
                m_paintedSparkKey = key;
                m_paintedSparkLayer = true;
                m_paintedMoving = g.moving;
                usedLayer = true;
            }
        }
    }
    if (!usedLayer) {
        const QRectF lineClip = spark.adjusted(-2, -3, 2, 2);
        drawTraces(p, tEnd, spark, lineClip, m_paintedMoving);
    }
}
