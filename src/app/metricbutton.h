// Navigation entry that doubles as a live metric summary: title, current
// value, one detail line and a small sparkline of the recent history.
#pragma once

#include "core/history.h"

#include <QAbstractButton>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPalette>
#include <string>
#include <utility>
#include <vector>

class QStyle;
class QStyleOptionViewItem;

class MetricButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit MetricButton(const QString& title, QWidget* parent = nullptr);

    // accessibleValue replaces symbols (arrows) for screen readers.
    void setValueText(const QString& value, const QString& accessibleValue = {});
    void setDetailText(const QString& detail);
    // Whether the detail line is drawn (and has room); the detail stays in
    // the tooltip and accessible description either way. Default on.
    void setDetailLineVisible(bool visible);
    bool isDetailLineVisible() const { return m_detailLine; }
    // Widest value expected (e.g. "R 0000 MiB/s  W 0000 MiB/s"). The layout is
    // chosen from it rather than from the live value, so it never jumps as
    // values change: the sparkline moves to the title row, and then a smaller
    // value font is used, when the template does not fit beside it.
    void setValueTemplate(const QString& widest);
    QString valueText() const { return m_value; }
    QString detailText() const { return m_detail; }
    // Sparkline sources (up to two series, drawn by default in the chart
    // colours and pen styles 0 and 1); no keys hides the sparkline.
    // maxValue <= 0 scales to the visible data.
    void setSparkline(const ps::History* history, std::vector<std::string> keys, double maxValue = 0,
                      double seconds = 60);
    void setSparklineMax(double maxValue);
    // Colour index (seriesColor) and pen style per sparkline series, so the
    // entry uses the same colours as its page's chart.
    void setSparklineVisuals(std::vector<std::pair<int, Qt::PenStyle>> visuals);
    // Right edge of the sparkline ("now"), as TimeChart::setViewEnd().
    void setViewEnd(double time);
    // Frame-driven scrolling, as TimeChart::advanceView(): repaints only the
    // sparkline, and only when it moved by a visible amount.
    void advanceView(double time);
    int paintCount() const { return m_paintCount; }
    // Sparkline rectangle of the last paint, for tests.
    QRectF sparklineArea() const { return m_sparkRect; }
    std::vector<std::pair<int, Qt::PenStyle>> sparklineVisuals() const { return m_visuals; }
    void setStale(bool stale);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct SparkLayer {
        std::vector<QImage> images;
        std::vector<char> drawn;
        int phases = 4;
        double sx = 0, sy = 0, ox = 0, oy = 0;
        QRectF spark;
        int left = 0, right = 0, top = 0, bottom = 0;
        int width = 0;
        double pxPerSecond = 0;
        double end = 0;
        double lo = 0, hi = 1;
        double seconds = 0;
        const ps::History* history = nullptr;
        uint64_t samples = 0;
        std::size_t size = 0;
        double latest = 0;
        qint64 paletteKey = 0;
        bool selected = false;
        bool stale = false;
        bool dark = false;
        bool moving = false;
        bool valid = false;
    };
    struct LayoutCache {
        QSize size;
        QFont baseFont;
        QFont valueDrawFont;
        QFont smallFont;
        QRect titleRect, valueRect, detailRect;
        QRectF sparkRect;
        QString titleElided, valueElided, detailElided;
        bool sparkBesideValue = true;
        bool hasSpark = false;
        bool valid = false;
    };
    void updateAccessibleText();
    void invalidateLayout();
    void invalidateSparkLayer();
    void ensureLayout() const;
    long long sparkLayerKey(double tEnd) const;
    bool sparkLayerCovers(long long key) const;
    QRect sparkUpdateRect() const;
    QColor selectedTextColor(const QStyleOptionViewItem& opt, QPalette::ColorGroup group) const;

    QString m_value;
    QString m_accessibleValue;
    QString m_detail;
    QString m_valueTemplate;
    const ps::History* m_history = nullptr;
    std::vector<std::string> m_keys;
    double m_max = 0;
    double m_seconds = 60;
    double m_viewEnd = ps::kNaN;
    bool m_stale = false;
    bool m_detailLine = true;
    std::vector<std::pair<int, Qt::PenStyle>> m_visuals;
    // What the last paint showed.
    mutable QRectF m_sparkRect;
    mutable LayoutCache m_layout;
    mutable QImage m_panelCache;
    mutable QSize m_panelSize;
    mutable qreal m_panelDpr = 0;
    mutable const QStyle* m_panelStyle = nullptr;
    mutable qint64 m_panelPaletteKey = 0;
    mutable unsigned m_panelState = 0;
    mutable bool m_panelValid = false;
    mutable const QStyle* m_contrastStyle = nullptr;
    mutable qint64 m_contrastPaletteKey = 0;
    mutable QPalette::ColorGroup m_contrastGroup = QPalette::Active;
    mutable bool m_contrastDown = false;
    mutable bool m_contrastFocused = false;
    mutable bool m_contrastValid = false;
    mutable QColor m_contrastColor;
    mutable QColor m_contrastBg;
    mutable bool m_rangeValid = false;
    mutable const ps::History* m_rangeHistory = nullptr;
    mutable uint64_t m_rangeSamples = 0;
    mutable std::size_t m_rangeSize = 0;
    mutable double m_rangeLatest = ps::kNaN;
    mutable std::size_t m_rangeFirst = 0;
    mutable double m_rangeLo = 0;
    mutable double m_rangeHi = 1;
    mutable SparkLayer m_sparkLayer;
    double m_paintedEnd = ps::kNaN;
    std::size_t m_paintedFirst = 0;
    long long m_paintedSparkKey = 0;
    bool m_paintedSparkLayer = false;
    bool m_paintedMoving = false;
    int m_paintCount = 0;
};
