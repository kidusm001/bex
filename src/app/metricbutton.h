// Navigation entry that doubles as a live metric summary: title, current
// value, one detail line and a small sparkline of the recent history.
#pragma once

#include "core/history.h"

#include <QAbstractButton>
#include <string>
#include <vector>

class MetricButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit MetricButton(const QString& title, QWidget* parent = nullptr);

    // accessibleValue replaces symbols (arrows) for screen readers.
    void setValueText(const QString& value, const QString& accessibleValue = {});
    void setDetailText(const QString& detail);
    // Widest value expected (e.g. "R 0000 MiB/s  W 0000 MiB/s"). The layout is
    // chosen from it rather than from the live value, so it never jumps as
    // values change: the sparkline moves to the title row, and then a smaller
    // value font is used, when the template does not fit beside it.
    void setValueTemplate(const QString& widest);
    QString valueText() const { return m_value; }
    QString detailText() const { return m_detail; }
    // Sparkline sources (up to two series, drawn in the chart colours 0 and 1);
    // no keys hides the sparkline. maxValue <= 0 scales to the visible data.
    void setSparkline(const ps::History* history, std::vector<std::string> keys, double maxValue = 0,
                      double seconds = 60);
    void setSparklineMax(double maxValue);
    // Right edge of the sparkline ("now"), as TimeChart::setViewEnd().
    void setViewEnd(double time);
    void setStale(bool stale);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void updateAccessibleText();

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
};
