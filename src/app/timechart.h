// QPainter time-series chart reading directly from a ps::History. Lines break
// at unavailable samples and at discontinuities (pause, suspend, outages), so
// a gap is never drawn as data; area fills follow exactly the same runs.
#pragma once

#include "core/history.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QLineF>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QRegion>
#include <QString>
#include <QTransform>
#include <QVector>
#include <QWidget>
#include <cstdint>
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
    // Translucent area in the line colour between the line and the baseline
    // (zero, or the plot bottom for temperatures). Areas overlap; they are
    // never stacked into totals.
    bool fill = true;
    // Overrides the pen style of the visual (seriesStyle()).
    std::optional<Qt::PenStyle> style {};
};

// Colour-blind-friendly series colours tuned for the palette's lightness, and
// pen styles so series stay distinguishable without colour.
QColor seriesColor(int index, const QPalette& palette);
Qt::PenStyle seriesStyle(int index);
bool isDarkPalette(const QPalette& palette);
// Clips segment a-b to rect in place (Liang-Barsky); false if none of it is
// inside. Charts draw measured geometry through this instead of clamping.
bool clipSegment(QPointF& a, QPointF& b, const QRectF& rect);
// Sutherland-Hodgman clip of a polygon to rect, in double precision, so areas
// end exactly at the rectangle's edges whatever the (finite) coordinates.
QPolygonF clipPolygon(const QPolygonF& polygon, const QRectF& rect);
// Strokes a run of measured points as one antialiased batch of straight
// segments (Qt's fast path for thick lines), clipped to clip first so
// off-scale values cost nothing. A solid run ends exactly at its first and
// last points. Dash patterns restart at points flagged in anchors (empty: at
// the first point); anchors that move with the data keep dashes from
// crawling while the view scrolls.
void strokeTrace(QPainter& painter, const QPolygonF& run, const std::vector<char>& anchors, const QColor& color,
                 double width, Qt::PenStyle style, const QRectF& clip);

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
    // Frame-driven scrolling between samples: moves the axis end to time and
    // repaints only the plot, and only when the chart is visible and the data
    // lands on a new sub-pixel step (a quarter or an eighth of a device pixel,
    // whichever gives smooth motion at that speed); identical frames are not
    // repainted. Samples, values and the accessible text are not touched.
    void advanceView(double time);
    // While enabled, a new sample continuous with its predecessor is revealed
    // along the measured segment between the two over a short ease-out (never
    // beyond the newest sample); legend, readout and export use the sample at
    // once. Disabling completes a reveal in progress. Off by default.
    void setRevealEnabled(bool enabled);
    bool revealEnabled() const { return m_revealEnabled; }
    bool isRevealing() const;

    // Glance mode: fewer grid lines and time labels.
    void setSimple(bool simple);
    bool isSimple() const { return m_simple; }
    // Legend rows with current values under the title (Full mode).
    void setLegendVisible(bool visible);
    bool isLegendVisible() const { return m_legendVisible; }

    // Call after the history changed.
    void refresh();

    // Readout support (mouse hover or Left/Right keys when focused).
    void setCursorTime(double time);
    void clearCursor();
    QString readoutText() const;
    // Number of valid points drawn in the last paint, for tests.
    int lastPaintedPoints() const { return m_lastPainted; }
    // Paint events handled so far (frame-rate measurement).
    int paintCount() const { return m_paintCount; }
    // Plot rectangle of the last paint, for tests.
    QRectF plotArea() const { return m_lastPlot; }
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
    void hideEvent(QHideEvent* event) override;

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
        QFont axisFont;
        int fmHeight = 0;
        int afmHeight = 0;
        double afmAvgCharWidth = 0;
        int legendRows = 0;
        double valueReserve = 0;
        double tickLabelWidth = 0; // widest whole-minute relative-time label
        double compoundLabelWidth = 0; // widest "minutes and seconds" label
    };
    struct CachedColors {
        qint64 paletteKey = 0;
        int firstVisual = -2;
        bool valid = false;
        bool dark = false;
        QColor base, text, muted;
        QColor frame;       // translucent text
        QColor plotBg;
        QColor grid;        // translucent accent, for lines drawn over the data
        QColor gridOpaque;  // the same over the plot background
        QColor frameOpaque; // frame over the plot background
        QColor cardBorder;  // subtle opaque outer card perimeter
    };
    struct PreparedSeries {
        int visual = 0;
        Qt::PenStyle style = Qt::SolidLine;
        bool fill = false;
        std::vector<QPolygonF> areas;
        std::vector<QPointF> dots;
        QVector<QLineF> lines;
    };
    struct PreparedData {
        std::vector<QRectF> gaps;
        std::vector<PreparedSeries> series;
        int filledCount = 0;
        bool valid = false;
    };
    // The plot (background, horizontal grid, top and bottom frame lines and
    // the data: outage shading, areas and lines) rasterized on an opaque
    // image at a few sub-pixel phases whenever the data, value range,
    // geometry or palette change, so frames that only scroll the view copy
    // pixels. Phase j is drawn shifted right by j / phases device pixels
    // and copied at a whole-pixel offset: every frame shows the measured
    // geometry within half a phase step of its exact position.
    // Where what paintData draws inside the plot lies, logical, for the axis
    // ending at a given time or later.
    struct Extent {
        double top = 0; // top of the drawn data, or the plot top
        // Rows of the lines and dots, and the areas' baseline. Outside those
        // rows the data is uniform along the time axis except at the
        // vertical edges of areas and outage shading.
        double bandTop = 0, bandBottom = 0, base = 0;
        struct Edge {
            double time; // sample time the edge is at
            bool full;   // spans the plot height (shading), else the band to the baseline
        };
        std::vector<Edge> edges;
        bool banded = false; // band and edges are known and few
    };
    struct DataLayer {
        std::vector<QImage> images; // per phase, allocated when first used
        std::vector<char> drawn;
        // For incremental shift-and-patch of already-drawn phase images when
        // scrolling past the buffer margin or appending new samples without
        // changing geometry, scale, range, or palette.
        std::vector<char> canPatch;
        int patchShiftPx = 0;
        int patchDirtyLeft = 0; // device x relative to left (g.width if pure scroll of empty right)
        double patchPrevLatest = ps::kNaN;
        std::size_t patchFromIndex = 0;
        bool patchPureScroll = false;

        PreparedData prepared;
        PreparedData patchPrepared;
        int phases = 0;
        // Paint-device transform (scale and offset), logical plot and its
        // device-pixel bounds; the image adds a scrolling margin on the right.
        double sx = 0, sy = 0, ox = 0, oy = 0;
        QRectF plot;
        int left = 0, right = 0, top = 0, bottom = 0;
        int width = 0;
        double pxPerSecond = 0; // device pixels per second of history
        double end = 0;         // axis end the images are drawn for
        std::size_t start = 0;  // first history index drawn
        double lo = 0, hi = 0;  // value range drawn
        double window = 0;
        const ps::History* history = nullptr;
        uint64_t samples = 0;
        std::size_t size = 0;
        double latest = 0;
        std::vector<double> gridYs; // horizontal grid rows, logical
        Extent extent;
        std::size_t extentStart = 0; // first history index extent was computed from
        uint64_t content = 0; // serial of what the images show
        qint64 paletteKey = 0;
        bool valid = false;
    };

    const CachedColors& resolvedColors(const QPalette& pal) const;
    Layout computeLayout(const QFontMetrics& fm) const;
    const Layout& layout();
    // Draws outage shading, areas and lines for samples start..last with the
    // axis ending at tEnd. Areas are clipped to area, lines to lineClip.
    void paintData(QPainter& painter, const QRectF& plot, double lo, double hi, double tEnd, std::size_t start,
                   const QRectF& area, const QRectF& lineClip) const;
    void buildPreparedData(DataLayer& g, std::size_t fromIndex, const QRectF& area, const QRectF& lineClip,
                           PreparedData& out) const;
    void paintPreparedData(QPainter& painter, const PreparedData& prep, const QPalette& pal) const;
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
    void noteLatestSample();      // starts a reveal when a new continuous sample arrived
    double revealFraction() const; // eased 0..1 progress of the reveal, 1 when none
    // The layer is valid for the current plot and history.
    bool layerMatchesHistory() const;
    // What a frame that only scrolls the view repaints: the plot from the
    // drawn data down, or all of it when the layer is stale.
    QRect plotUpdateRect() const;
    // Less than that when the lines lie in a narrow band: that band, and the
    // columns the vertical edges below it swept since the layer's content
    // was first painted.
    QRegion plotUpdateRegion() const;
    // Where the readout cursor is drawn for the current axis end: its line
    // with the sample markers and its box. Empty when there is none.
    QRegion cursorFootprint() const;
    void invalidateLayer();
    // Makes m_layer match this device transform, plot, range, grid and data,
    // and cover axis end tEnd (drawing nothing yet); false if it cannot.
    bool prepareLayer(const QTransform& device, const QRectF& plot, const Range& range,
                      const std::vector<double>& gridYs, double tEnd, std::size_t start);
    void drawLayerPhase(int phase);
    // Where paintData draws inside the plot for samples start..last with the
    // axis ending at tEnd or later.
    Extent dataExtent(const QRectF& plot, double lo, double hi, double tEnd, std::size_t start) const;
    // Offset of the layer for axis end tEnd, in 1/phases device pixels.
    long long layerKey(double tEnd) const;
    bool layerCovers(long long key) const;

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
    int m_paintCount = 0;
    bool m_simple = false;
    bool m_legendVisible = true;

    // What the last paint showed, to decide whether a frame needs a repaint.
    double m_paintedEnd = ps::kNaN;
    std::size_t m_paintedFirst = 0;
    bool m_paintedInWindow = false;
    bool m_paintedMoving = false; // anything drawn scrolls with the view
    long long m_paintedKey = 0;
    bool m_paintedLayer = false;
    double m_paintedReveal = 1;
    DataLayer m_layer;
    uint64_t m_contentSerial = 0;
    // The axis ends painted with layer content m_sweepContent: the screen
    // shows that content at ends within them.
    uint64_t m_sweepContent = 0;
    double m_sweepFrom = ps::kNaN;
    double m_sweepTo = ps::kNaN;
    // Where the screen may still show a readout cursor.
    QRegion m_paintedCursor;
    Layout m_layoutCache;
    QSize m_layoutSize;
    bool m_layoutValid = false;

    mutable CachedColors m_colorsCache;
    mutable bool m_gridCacheValid = false;
    mutable double m_gridCacheLo = 0, m_gridCacheHi = 0, m_gridCacheStep = 0;
    mutable double m_gridCacheTop = 0, m_gridCacheHeight = 0;
    mutable bool m_gridCacheSimple = false;
    mutable int m_gridCacheMaxLines = 0;
    mutable std::vector<double> m_gridValsCache;
    mutable std::vector<double> m_gridYsCache;

    // Cached per-window sample stats and range to avoid re-scanning history on
    // every 60 Hz scroll frame when the history and visible window start are unchanged.
    bool m_statsValid = false;
    const ps::History* m_statsHistory = nullptr;
    uint64_t m_statsSamples = 0;
    std::size_t m_statsSize = 0;
    double m_statsLatest = ps::kNaN;
    std::size_t m_statsFirst = 0;
    int m_statsPainted = 0;
    bool m_statsMoving = false;
    Range m_statsRange;

    bool m_revealEnabled = false;
    double m_seenLatest = ps::kNaN; // newest sample time already known
    double m_revealFrom = ps::kNaN; // predecessor time of the sample being revealed
    double m_revealTo = ps::kNaN;   // time of that sample; NaN: no reveal
    double m_revealStart = ps::kNaN; // axis end when it arrived
    double m_revealDuration = 0;
};
