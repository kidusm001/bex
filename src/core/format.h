// Locale-independent display formatting (IEC units, always '.' decimals).
#pragma once

#include <string>

namespace ps::fmt {

// Three significant digits: "512 B", "1.23 KiB", "12.3 MiB", "123 GiB".
std::string bytes(double v);
std::string rate(double bytesPerSecond); // bytes(v) + "/s"
std::string percent(double v, int decimals = 1);
std::string celsius(double v);
std::string duration(double seconds); // "45s", "12m 05s", "3h 04m", "2d 03h"
std::string number(double v, int decimals);

// Unit scaling used by chart axes: picks 1024^k so value/divisor < 1024.
struct IecScale {
    double divisor = 1;
    const char* unit = "B";
};
IecScale iecScaleFor(double maxValue);

} // namespace ps::fmt
