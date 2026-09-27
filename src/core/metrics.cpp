#include "core/metrics.h"

#include <algorithm>
#include <cmath>

namespace ps::metrics {

namespace {

// a + b, clearing ok instead of wrapping.
uint64_t addChecked(uint64_t a, uint64_t b, bool& ok)
{
    if (b > UINT64_MAX - a) {
        ok = false;
        return UINT64_MAX;
    }
    return a + b;
}

} // namespace

CpuDelta& CpuDelta::operator+=(const CpuDelta& o)
{
    bool ok = !overflow && !o.overflow;
    user = addChecked(user, o.user, ok);
    system = addChecked(system, o.system, ok);
    idle = addChecked(idle, o.idle, ok);
    iowait = addChecked(iowait, o.iowait, ok);
    steal = addChecked(steal, o.steal, ok);
    total = addChecked(total, o.total, ok);
    overflow = !ok;
    return *this;
}

std::optional<CpuDelta> cpuDelta(const parse::CpuTimes& prev, const parse::CpuTimes& cur)
{
    if (prev.fields != cur.fields)
        return std::nullopt; // columns appeared or vanished: not comparable
    const uint64_t p[] = {prev.user, prev.nice, prev.system, prev.idle, prev.iowait, prev.irq, prev.softirq, prev.steal};
    const uint64_t c[] = {cur.user, cur.nice, cur.system, cur.idle, cur.iowait, cur.irq, cur.softirq, cur.steal};
    uint64_t d[8];
    for (int i = 0; i < 8; ++i) {
        if (c[i] < p[i])
            return std::nullopt;
        d[i] = c[i] - p[i];
    }
    bool ok = true;
    CpuDelta out;
    out.user = addChecked(d[0], d[1], ok);
    out.system = addChecked(addChecked(d[2], d[5], ok), d[6], ok);
    out.iowait = d[4];
    out.idle = addChecked(d[3], d[4], ok);
    out.steal = d[7];
    out.total = addChecked(addChecked(addChecked(out.user, out.system, ok), out.idle, ok), out.steal, ok);
    if (!ok)
        return std::nullopt; // absurd counters: never report a wrapped interval
    return out;
}

CpuUsage usageFromDelta(const CpuDelta& d)
{
    CpuUsage u;
    if (d.overflow || d.total == 0)
        return u;
    // The parts must add up to the total; anything else (a wrapped or
    // hand-built delta) is unavailable rather than a clamped percentage.
    bool ok = true;
    const uint64_t sum = addChecked(addChecked(addChecked(d.user, d.system, ok), d.idle, ok), d.steal, ok);
    if (!ok || sum != d.total || d.iowait > d.idle)
        return u;
    const double total = static_cast<double>(d.total);
    const uint64_t busy = d.total - d.idle;
    u.total = 100.0 * static_cast<double>(busy) / total;
    u.user = 100.0 * static_cast<double>(d.user) / total;
    u.system = 100.0 * static_cast<double>(d.system) / total;
    u.iowait = 100.0 * static_cast<double>(d.iowait) / total;
    u.steal = 100.0 * static_cast<double>(d.steal) / total;
    return u;
}

CpuUsage cpuUsage(const parse::CpuTimes& prev, const parse::CpuTimes& cur)
{
    const auto d = cpuDelta(prev, cur);
    return d ? usageFromDelta(*d) : CpuUsage{};
}

std::optional<uint64_t> counterDelta(uint64_t prev, uint64_t cur, int counterBits)
{
    if (cur >= prev)
        return cur - prev;
    if (counterBits == 32) {
        constexpr uint64_t kWrap = uint64_t{1} << 32;
        if (prev < kWrap && cur < kWrap && prev >= kWrap / 2)
            return (kWrap - prev) + cur;
    }
    return std::nullopt;
}

double rate(std::optional<uint64_t> delta, double seconds, uint64_t scale, double maxRate)
{
    if (!delta || !(seconds > 0) || !std::isfinite(seconds) || scale == 0)
        return kNaN;
    if (*delta > UINT64_MAX / scale)
        return kNaN;
    const double r = static_cast<double>(*delta * scale) / seconds;
    if (!std::isfinite(r) || r > maxRate)
        return kNaN;
    return r;
}

double processCpuPercent(uint64_t prevTicks, uint64_t curTicks, double seconds,
                         long clockTicks, int logicalCpus)
{
    if (curTicks < prevTicks || !(seconds > 0) || !std::isfinite(seconds) || clockTicks <= 0)
        return kNaN;
    const double pct = 100.0 * static_cast<double>(curTicks - prevTicks)
        / static_cast<double>(clockTicks) / seconds;
    // Tick accounting is quantised, so allow some slack above the theoretical
    // maximum before declaring the value impossible.
    const double limit = 100.0 * std::max(1, logicalCpus) * 1.25 + 5.0;
    if (!std::isfinite(pct) || pct > limit)
        return kNaN;
    return pct;
}

MemoryInfo memoryFromRaw(const parse::MemInfoRaw& raw)
{
    MemoryInfo m;
    if (raw.memTotal && *raw.memTotal > 0) {
        const uint64_t total = *raw.memTotal;
        m.valid = true;
        m.total = static_cast<double>(total);
        if (raw.memAvailable && *raw.memAvailable <= total) {
            m.available = static_cast<double>(*raw.memAvailable);
            m.used = static_cast<double>(total - *raw.memAvailable);
        }
        if (raw.memFree && *raw.memFree <= total)
            m.free = static_cast<double>(*raw.memFree);
        if (raw.buffers && raw.cached && raw.sReclaimable) {
            // buff/cache is Buffers + Cached + SReclaimable, like free(1). All
            // three exist on every supported kernel (2.6.19+), so a missing or
            // invalid part leaves the total unavailable instead of silently
            // counting it as 0. Each part must fit in MemTotal and the sum is
            // checked, so a wrapped sum can never pass as a plausible size.
            const uint64_t parts[] = {*raw.buffers, *raw.cached, *raw.sReclaimable};
            uint64_t cache = 0;
            bool ok = true;
            for (uint64_t part : parts) {
                if (part > total || part > total - cache) {
                    ok = false;
                    break;
                }
                cache += part;
            }
            if (ok)
                m.cache = static_cast<double>(cache);
        }
        if (raw.shmem && *raw.shmem <= total)
            m.shared = static_cast<double>(*raw.shmem);
    }
    if (raw.swapTotal) {
        const uint64_t swapTotal = *raw.swapTotal;
        m.swapTotal = static_cast<double>(swapTotal);
        m.hasSwap = swapTotal > 0;
        if (raw.swapFree && *raw.swapFree <= swapTotal) {
            m.swapFree = static_cast<double>(*raw.swapFree);
            m.swapUsed = static_cast<double>(swapTotal - *raw.swapFree);
        }
    }
    return m;
}

double continuityLimit(double expectedInterval)
{
    const double e = expectedInterval > 0 ? expectedInterval : 1.0;
    return std::max(2.5 * e, e + 1.0);
}

bool isDiscontinuous(double elapsed, double expectedInterval)
{
    return !(elapsed > 0) || !std::isfinite(elapsed) || elapsed > continuityLimit(expectedInterval);
}

} // namespace ps::metrics
