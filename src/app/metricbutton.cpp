#include "app/metricbutton.h"

#include "app/timechart.h"

#include <QEvent>
#include <QImage>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionViewItem>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int kPadX = 10;
constexpr int kPadY = 7;
constexpr int kSparkW = 64;

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

void MetricButton::setValueText(const QString& value, const QString& accessibleValue)
{
    if (value == m_value && accessibleValue == m_accessibleValue)
        return;
    m_value = value;
    m_accessibleValue = accessibleValue;
    updateAccessibleText();
    update();
}

void MetricButton::setDetailText(const QString& detail)
{
    if (detail == m_detail)
        return;
    m_detail = detail;
    updateAccessibleText();
    update();
}

void MetricButton::setValueTemplate(const QString& widest)
{
    if (widest == m_valueTemplate)
        return;
    m_valueTemplate = widest;
    updateGeometry();
    update();
}

void MetricButton::setSparkline(const ps::History* history, std::vector<std::string> keys, double maxValue,
                                double seconds)
{
    m_history = history;
    m_keys = std::move(keys);
    if (m_keys.size() > 2)
        m_keys.resize(2);
    m_max = maxValue;
    m_seconds = seconds;
    update();
}

void MetricButton::setSparklineMax(double maxValue)
{
    if (maxValue == m_max)
        return;
    m_max = maxValue;
    update();
}

void MetricButton::setSparklineVisuals(std::vector<std::pair<int, Qt::PenStyle>> visuals)
{
    if (visuals == m_visuals)
        return;
    m_visuals = std::move(visuals);
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
    const double tEnd = std::max(m_viewEnd, m_history->latestTime());
    if (m_history->lowerBound(tEnd - m_seconds) != m_paintedFirst) {
        update(sparkUpdateRect());
        return;
    }
    // A fifth of a device pixel is the smallest step worth a frame.
    const double moved = std::fabs(tEnd - m_paintedEnd) / m_seconds * m_sparkRect.width() * devicePixelRatioF();
    if (moved >= 0.2)
        update(sparkUpdateRect());
}

void MetricButton::setDetailLineVisible(bool visible)
{
    if (visible == m_detailLine)
        return;
    m_detailLine = visible;
    updateGeometry();
    update();
}

void MetricButton::setStale(bool stale)
{
    if (stale == m_stale)
        return;
    m_stale = stale;
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
        updateGeometry();
        update();
    }
    QAbstractButton::changeEvent(event);
}

QColor MetricButton::selectedTextColor(const QStyleOptionViewItem& opt, QPalette::ColorGroup group) const
{
    const QPalette& pal = opt.palette;
    const QStyle* st = style();
    const bool down = isDown();
    if (m_contrastValid && m_contrastStyle == st && m_contrastPaletteKey == pal.cacheKey()
        && m_contrastGroup == group && m_contrastDown == down)
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
    m_contrastColor = (hiContrast < 3.0 && winContrast > hiContrast) ? winText : hiText;
    m_contrastStyle = st;
    m_contrastPaletteKey = pal.cacheKey();
    m_contrastGroup = group;
    m_contrastDown = down;
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

    // Selection and hover are drawn by the style as a selected list item, so
    // the entry follows the desktop's Qt theme like the rest of the window.
    QStyleOptionViewItem opt;
    opt.initFrom(this);
    opt.rect = rect();
    opt.showDecorationSelected = true;
    opt.viewItemPosition = QStyleOptionViewItem::OnlyOne;
    opt.features = QStyleOptionViewItem::HasDisplay;
    opt.state &= ~QStyle::State_HasFocus; // the focus ring below is stronger
    if (selected || isDown())
        opt.state |= QStyle::State_Selected;
    if (underMouse())
        opt.state |= QStyle::State_MouseOver;
    if (isActiveWindow())
        opt.state |= QStyle::State_Active;
    style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, &p, this);

    const QPalette::ColorGroup group = !isEnabled() ? QPalette::Disabled
                                                    : (isActiveWindow() ? QPalette::Active : QPalette::Inactive);
    const QColor text = selected ? selectedTextColor(opt, group) : pal.color(QPalette::WindowText);
    QColor muted = pal.color(QPalette::PlaceholderText);
    if (selected) {
        muted = text;
        muted.setAlphaF(0.78f);
    } else if (!muted.isValid() || muted == text) {
        muted = pal.color(QPalette::Disabled, QPalette::WindowText);
    }
    if (hasFocus() && !QRectF(rect()).adjusted(4, 4, -4, -4).contains(dirty)) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(selected ? text : pal.color(QPalette::Highlight), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 4, 4);
    }

    const QFont base = font();
    const QFont vf = valueFont(base);
    const QFont sf = smallFont(base);
    const QFontMetrics fm(base), vfm(vf), sfm(sf);
    const int x = kPadX;
    int y = kPadY;
    const int textW = width() - 2 * kPadX;
    const int besideW = textW - kSparkW - 6;

    // Layout: the sparkline spans the title and value rows when the widest
    // expected value fits beside it; otherwise it shrinks to the title row
    // and the value gets the full width, in a smaller font if it must.
    const bool hasSpark = m_history && !m_keys.empty();
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

    const QRect titleRect(x, y, hasSpark ? besideW : textW, fm.height());
    if (dirty.intersects(titleRect)) {
        p.setPen(text);
        p.setFont(base);
        p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(this->text(), Qt::ElideRight, titleRect.width()));
    }
    y += fm.height();

    const QRect valueRect(x, y, valueW, vfm.height());
    if (dirty.intersects(valueRect)) {
        p.setFont(valueDrawFont);
        p.setPen(m_stale ? muted : text);
        p.drawText(valueRect, Qt::AlignLeft | Qt::AlignVCenter, valueFm.elidedText(m_value, Qt::ElideRight, valueW));
    }
    y += vfm.height() + 2;

    const QRect detailRect(x, y, textW, sfm.height());
    if (m_detailLine && dirty.intersects(detailRect)) {
        p.setFont(sf);
        p.setPen(muted);
        p.drawText(detailRect, Qt::AlignLeft | Qt::AlignVCenter,
                   sfm.elidedText(m_stale ? QStringLiteral("No recent samples") : m_detail, Qt::ElideRight, textW));
    }

    // Sparkline in the right-hand corner of the title (and value) rows.
    m_sparkRect = QRectF(width() - kPadX - kSparkW, kPadY + 2, kSparkW,
                         sparkBesideValue ? fm.height() + vfm.height() - 6 : fm.height() - 3);
    m_paintedEnd = ps::kNaN;
    if (!hasSpark || m_history->empty())
        return;
    std::vector<const ps::History::Series*> series;
    for (const auto& key : m_keys)
        series.push_back(m_history->find(key));
    const double tEnd = std::isfinite(m_viewEnd) ? std::max(m_viewEnd, m_history->latestTime())
                                                  : m_history->latestTime();
    const double tStart = tEnd - m_seconds;
    const std::size_t first = m_history->lowerBound(tStart);
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
    for (const auto* s : series)
        if (s && s->unit == ps::Unit::Celsius)
            signedUnit = true;
    double lo = 0;
    double hi = m_max;
    if (hi <= 0 || signedUnit) {
        double vmin = std::numeric_limits<double>::infinity();
        double vmax = -std::numeric_limits<double>::infinity();
        for (const auto* s : series) {
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
    QColor baseline = muted;
    baseline.setAlphaF(0.35f);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(baseline, 1));
    p.drawLine(spark.bottomLeft(), spark.bottomRight());
    p.setClipRect(spark.adjusted(0, -2, 0, 1));
    const bool dark = isDarkPalette(pal);
    for (std::size_t si = 0; si < series.size(); ++si) {
        const auto* s = series[si];
        if (!s)
            continue;
        const auto [visual, style] = si < m_visuals.size()
            ? m_visuals[si]
            : std::pair {static_cast<int>(si), seriesStyle(static_cast<int>(si))};
        // On the selection the line takes the selected text colour so it
        // stays legible on any theme's highlight.
        const QColor line = m_stale ? muted : (selected ? text : seriesColor(visual, pal));
        // Runs of consecutive valid samples; areas under them first. Dashes
        // restart at fixed times (anchors) so they move with the data.
        const double quantum = m_seconds / 8;
        std::vector<std::pair<QPolygonF, std::vector<char>>> runs;
        std::pair<QPolygonF, std::vector<char>> run;
        auto endRun = [&] {
            if (!run.first.isEmpty())
                runs.push_back(std::move(run));
            run = {};
        };
        for (std::size_t i = from; i < n; ++i) {
            const double v = m_history->value(*s, i);
            if (!ps::isValid(v) || (i > from && !m_history->continuous(i)))
                endRun();
            if (!ps::isValid(v))
                continue;
            const double t = m_history->time(i);
            const double px = spark.left() + (t - tStart) / m_seconds * spark.width();
            run.first.append(QPointF(px, yOf(v)));
            run.second.push_back(i > 0 && std::floor(t / quantum) != std::floor(m_history->time(i - 1) / quantum));
        }
        endRun();
        if (!m_stale && si == 0) {
            QColor area = line;
            area.setAlphaF(selected ? 0.22f : (dark ? 0.24f : 0.16f));
            // Aliased: the line covers its top edge.
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(Qt::NoPen);
            p.setBrush(area);
            for (const auto& [r, anchors] : runs) {
                if (r.size() < 2)
                    continue;
                QPolygonF poly = r;
                poly.append(QPointF(r.back().x(), spark.bottom()));
                poly.append(QPointF(r.front().x(), spark.bottom()));
                // The area is clipped exactly like its line: above the scale
                // it fills the whole height, below it nothing.
                poly = clipPolygon(poly, spark);
                if (poly.size() >= 3)
                    p.drawPolygon(poly);
            }
        }
        const QRectF lineClip = spark.adjusted(-2, -3, 2, 2);
        for (const auto& [r, anchors] : runs)
            strokeTrace(p, r, anchors, line, 1.5, style, lineClip);
    }
}
