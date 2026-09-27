#include "app/metricbutton.h"

#include "app/timechart.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionFocusRect>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int kPadX = 10;
constexpr int kPadY = 7;
constexpr int kSparkW = 64;

QFont valueFont(const QFont& base)
{
    QFont f = base;
    if (f.pointSizeF() > 0)
        f.setPointSizeF(f.pointSizeF() * 1.3);
    else
        f.setPixelSize(static_cast<int>(f.pixelSize() * 1.3));
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

void MetricButton::setViewEnd(double time)
{
    m_viewEnd = std::isfinite(time) ? time : ps::kNaN;
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
    const int h = kPadY * 2 + fm.height() + vfm.height() + sfm.height() + 2;
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
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange)
        updateGeometry();
    QAbstractButton::changeEvent(event);
}

void MetricButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPalette pal = palette();
    const QColor text = pal.color(QPalette::WindowText);
    QColor muted = pal.color(QPalette::PlaceholderText);
    if (!muted.isValid() || muted == text)
        muted = pal.color(QPalette::Disabled, QPalette::WindowText);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);

    // Selection is a tinted fill plus a full outline, hover a lighter tint.
    if (isChecked() || isDown() || underMouse()) {
        QColor fill = highlight;
        fill.setAlphaF(isChecked() ? 0.16f : 0.07f);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(r, 5, 5);
    }
    if (isChecked()) {
        QColor edge = highlight;
        edge.setAlphaF(0.55f);
        p.setPen(QPen(edge, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 5, 5);
    }
    if (hasFocus()) {
        p.setPen(QPen(highlight, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 6, 6);
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

    p.setPen(text);
    p.setFont(base);
    p.drawText(QRect(x, y, textW - kSparkW - 6, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(this->text(), Qt::ElideRight, textW - kSparkW - 6));
    y += fm.height();

    p.setFont(valueDrawFont);
    p.setPen(m_stale ? muted : text);
    p.drawText(QRect(x, y, valueW, vfm.height()), Qt::AlignLeft | Qt::AlignVCenter,
               valueFm.elidedText(m_value, Qt::ElideRight, valueW));
    y += vfm.height() + 2;

    p.setFont(sf);
    p.setPen(muted);
    p.drawText(QRect(x, y, textW, sfm.height()), Qt::AlignLeft | Qt::AlignVCenter,
               sfm.elidedText(m_stale ? QStringLiteral("No recent samples") : m_detail, Qt::ElideRight, textW));

    // Sparkline in the right-hand corner of the title (and value) rows.
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
    if (first >= n)
        return;
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
    const QRectF spark(width() - kPadX - kSparkW, kPadY + 2, kSparkW,
                       sparkBesideValue ? fm.height() + vfm.height() - 6 : fm.height() - 3);
    QColor baseline = muted;
    baseline.setAlphaF(0.35f);
    p.setPen(QPen(baseline, 1));
    p.drawLine(spark.bottomLeft(), spark.bottomRight());
    for (std::size_t si = 0; si < series.size(); ++si) {
        const auto* s = series[si];
        if (!s)
            continue;
        QPainterPath path;
        bool drawing = false;
        for (std::size_t i = first; i < n; ++i) {
            const double v = m_history->value(*s, i);
            if (!ps::isValid(v) || !m_history->continuous(i))
                drawing = false;
            if (!ps::isValid(v))
                continue;
            const double px = spark.left() + (m_history->time(i) - tStart) / m_seconds * spark.width();
            const double py = spark.bottom() - std::clamp((v - lo) / (hi - lo), 0.0, 1.0) * spark.height();
            if (drawing)
                path.lineTo(px, py);
            else
                path.moveTo(px, py);
            drawing = true;
        }
        const QColor line = m_stale ? muted : seriesColor(static_cast<int>(si), pal);
        QPen pen(line, 1.5);
        pen.setStyle(seriesStyle(static_cast<int>(si)));
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
}
