// Command-line value parsing that validates before converting, so huge,
// non-finite or malformed values are rejected instead of reaching an
// undefined floating-point to integer conversion.
#pragma once

#include <QSize>
#include <QString>

#include <optional>

namespace cli {

// A sampling interval in seconds that is exactly one of the supported
// choices (0.5, 1, 2, 5); returns it in milliseconds.
std::optional<int> parseIntervalMs(const QString& text);

// A finite, non-negative number of seconds no larger than maxSeconds,
// rounded to whole milliseconds.
inline constexpr double kMaxDelaySeconds = 86400.0;
std::optional<int> parseDelayMs(const QString& text, double maxSeconds = kMaxDelaySeconds);

// A window size "WIDTHxHEIGHT" in pixels, each from kMinWindowSide to
// kMaxWindowSide.
inline constexpr int kMinWindowSide = 320;
inline constexpr int kMaxWindowSide = 16384;
std::optional<QSize> parseWindowSize(const QString& text);

} // namespace cli
