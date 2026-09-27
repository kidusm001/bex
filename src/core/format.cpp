#include "core/format.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace ps::fmt {

namespace {
constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
constexpr const char* kUnavailable = "n/a";
} // namespace

std::string number(double v, int decimals)
{
    if (!std::isfinite(v))
        return kUnavailable;
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, decimals);
    if (res.ec != std::errc())
        return kUnavailable;
    std::string s(buf, res.ptr);
    if (s == "-0" || s.rfind("-0.", 0) == 0) {
        // Avoid presenting tiny negative rounding noise as "-0.0".
        bool allZero = true;
        for (char c : s)
            if (c >= '1' && c <= '9')
                allZero = false;
        if (allZero)
            s.erase(0, 1);
    }
    return s;
}

IecScale iecScaleFor(double maxValue)
{
    IecScale s;
    double m = std::fabs(maxValue);
    int k = 0;
    while (m >= 1024.0 && k < 6) {
        m /= 1024.0;
        s.divisor *= 1024.0;
        ++k;
    }
    s.unit = kUnits[k];
    return s;
}

std::string bytes(double v)
{
    if (!std::isfinite(v))
        return kUnavailable;
    const IecScale s = iecScaleFor(v);
    const double scaled = v / s.divisor;
    if (s.divisor == 1)
        return number(scaled, 0) + " B";
    const double a = std::fabs(scaled);
    int decimals = a >= 99.95 ? 0 : (a >= 9.995 ? 1 : 2);
    std::string out = number(scaled, decimals);
    // Rounding may reach 1024 (e.g. 1023.9 KiB); present it in the next unit.
    if (decimals == 0 && std::fabs(std::round(scaled)) >= 1024.0 && s.divisor < 1024.0 * 1024 * 1024 * 1024 * 1024)
        return bytes(std::copysign(1024.0 * s.divisor, v));
    return out + " " + s.unit;
}

std::string rate(double bytesPerSecond)
{
    if (!std::isfinite(bytesPerSecond))
        return kUnavailable;
    return bytes(bytesPerSecond) + "/s";
}

std::string percent(double v, int decimals)
{
    if (!std::isfinite(v))
        return kUnavailable;
    return number(v, decimals) + " %";
}

std::string celsius(double v)
{
    if (!std::isfinite(v))
        return kUnavailable;
    return number(v, 1) + " \u00B0C";
}

std::string duration(double seconds)
{
    // Beyond ~317 years the value is a counter glitch, and far beyond it the
    // integer conversion below would be undefined.
    if (!std::isfinite(seconds) || seconds < 0 || seconds >= 1e10)
        return kUnavailable;
    const auto total = static_cast<long long>(seconds);
    const long long d = total / 86400, h = (total / 3600) % 24, m = (total / 60) % 60, s = total % 60;
    char buf[64];
    if (d > 0)
        std::snprintf(buf, sizeof buf, "%lldd %02lldh", d, h);
    else if (h > 0)
        std::snprintf(buf, sizeof buf, "%lldh %02lldm", h, m);
    else if (m > 0)
        std::snprintf(buf, sizeof buf, "%lldm %02llds", m, s);
    else
        std::snprintf(buf, sizeof buf, "%llds", s);
    return buf;
}

} // namespace ps::fmt
