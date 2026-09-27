#include "app/cli.h"

#include "app/mainwindow.h"

#include <QStringList>

#include <cmath>

namespace cli {

namespace {

std::optional<double> parseSeconds(const QString& text)
{
    bool ok = false;
    const double v = text.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(v))
        return std::nullopt;
    return v;
}

} // namespace

std::optional<int> parseIntervalMs(const QString& text)
{
    const auto seconds = parseSeconds(text);
    if (!seconds)
        return std::nullopt;
    // Compare in floating point; only a matching choice is ever returned.
    for (int choice : MainWindow::intervalChoicesMs())
        if (std::fabs(*seconds * 1000.0 - choice) < 1e-6)
            return choice;
    return std::nullopt;
}

std::optional<int> parseDelayMs(const QString& text, double maxSeconds)
{
    const auto seconds = parseSeconds(text);
    if (!seconds || *seconds < 0 || !(maxSeconds >= 0) || maxSeconds > kMaxDelaySeconds || *seconds > maxSeconds)
        return std::nullopt;
    return static_cast<int>(std::llround(*seconds * 1000.0)); // at most 86 400 000
}

std::optional<QSize> parseWindowSize(const QString& text)
{
    const QStringList parts = text.trimmed().toLower().split(QLatin1Char('x'));
    if (parts.size() != 2)
        return std::nullopt;
    int side[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const QString& p = parts[i];
        // Plain digits only (no sign, spaces or exponent), short enough that
        // the conversion cannot overflow.
        if (p.isEmpty() || p.size() > 5)
            return std::nullopt;
        for (QChar c : p)
            if (c < QLatin1Char('0') || c > QLatin1Char('9'))
                return std::nullopt;
        side[i] = p.toInt();
        if (side[i] < kMinWindowSide || side[i] > kMaxWindowSide)
            return std::nullopt;
    }
    return QSize(side[0], side[1]);
}

} // namespace cli
