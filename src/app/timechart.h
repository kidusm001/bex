// QPainter time-series chart reading directly from a ps::History. Lines break
// at unavailable samples and at discontinuities (pause, suspend, outages), so
// a gap is never drawn as data.
#pragma once

#include "core/history.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct ChartSeries {
    std::string key;
    QString label;
    // Index into the colour/pen-style cycle; -1 uses the series position.
    // Colours are resolved at paint time so palette changes apply live.
    int visual = -1;
};

// Colour-blind-friendly series colours tuned for the palette's lightness, and
// pen styles so series stay distinguishable without colour.
QColor seriesColor(int index, const QPalette& palette);
Qt::PenStyle seriesStyle(int index);
bool isDarkPalette(const QPalette& palette);
// Clips segment a-b to rect in place (Liang-Barsky); false if none of it is
// inside. Charts draw measured geometry through this instead of clamping.
bool clipSegment(QPointF& a, QPointF& b, const QRectF& rect);

class TimeChart : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Full, Compact };

    TimeChart(const QString& title, ps::Unit unit, QWidget* parent = nullptr);

    void setHistory(const ps::History* history);
    void setSeries(std::vector<ChartSeries> series);
    const std::vector<ChartSeries>& series() const { return m_series; }
    void setUnit(ps::Unit unit);
    ps::Unit unit() const { return m_unit; }
    // Fixed vertical range for any unit; invalid bounds (non-finite, max <= min)
    // select automatic scaling.
    void setFixedRange(double min, double max);
    void setAutoRange();
    bool hasFixedRange() const;
    double fixedMin() const { return m_fixedMin; }
    double fixedMax() const { return m_fixedMax; }
    // Vertical axis bounds the next paint would use (lo, hi).
    std::pair<double, double> displayedRange() const;
    void setWindowSeconds(double seconds);
    double windowSeconds() const { return m_window; }
    void setMode(Mode mode);
    void setTitle(const QString& title);
    QString title() const { return m_title; }
    // Shown centred when no visible series has a valid value in the window.
    void setEmptyText(const QString& text);
    // Short status shown in the plot corner (for example "Paused").
    void setNotice(const QString& notice);
    QString notice() const { return m_notice; }

    // Right edge of the time axis on the history clock ("now"). The owner
    // sets it to the current time when it refreshes, so the axis never treats
    // an old sample as current; invalid (the default) follows the newest sample.
    void setViewEnd(double time);
    double viewEnd() const { return endTime(); }

    // Call after the history changed.
    void refresh();

    // Readout support (mouse hover or Left/Right keys when focused).
    void setCursorTime(double time);
    void clearCursor();
    QString readoutText() const;
    // Number of valid points drawn in the last paint, for tests.
    int lastPaintedPoints() const { return m_lastPainted; }
    bool hasVisibleData() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Range {
        double lo = 0, hi = 1;
        double step = 0.25;
        double divisor = 1;
        QString unitText;
    };
    struct Layout {
        QRectF plot;
        QRectF header;
        int legendRows = 0;
    };

    Layout computeLayout(const QFontMetrics& fm) const;
    Range computeRange(std::size_t first, std::size_t last) const;
    QString axisLabel(double value, const Range& range) const;
    QString formatValue(double value) const;
    double endTime() const;
    std::size_t firstVisibleIndex() const;
    // Whether any retained sample lies inside the window. With the axis
    // ending at the current time, all of them can be older than the window.
    bool hasVisibleSamples() const;
    // The visible sample nearest to time; none when the window is empty.
    std::optional<std::size_t> nearestIndex(double time) const;
    double cursorTime() const;
    void updateAccessibleDescription();
    QString emptyMessage() const; // what an empty plot says (and is announced as)

    QString m_title;
    ps::Unit m_unit;
    const ps::History* m_history = nullptr;
    std::vector<ChartSeries> m_series;
    double m_fixedMin = ps::kNaN;
    double m_fixedMax = ps::kNaN;
    double m_window = 300;
    double m_viewEnd = ps::kNaN;
    Mode m_mode = Mode::Full;
    QString m_emptyText;
    QString m_notice;

    double m_cursorTime = ps::kNaN;
    bool m_cursorFollowsMouse = false;
    double m_mouseX = 0;
    mutable QRectF m_lastPlot;
    int m_lastPainted = 0;
};
