#include "core/history.h"

#include "core/format.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace ps {

const char* unitSuffix(Unit unit)
{
    switch (unit) {
    case Unit::Percent: return "%";
    case Unit::Bytes: return "bytes";
    case Unit::BytesPerSecond: return "B/s";
    case Unit::Celsius: return "\u00B0C";
    }
    return "";
}

History::History(std::size_t capacity, double maxAgeSeconds)
    : m_capacity(std::max<std::size_t>(capacity, 2))
    , m_maxAge(maxAgeSeconds > 0 ? maxAgeSeconds : kDefaultMaxAge)
    , m_times(m_capacity, kNaN)
    , m_continuous(m_capacity, 0)
{
}

History::History(const History& other)
    : m_capacity(other.m_capacity)
    , m_maxAge(other.m_maxAge)
    , m_times(other.m_times)
    , m_continuous(other.m_continuous)
    , m_head(other.m_head)
    , m_count(other.m_count)
    , m_total(other.m_total)
    , m_open(other.m_open)
    , m_index(other.m_index)
    , m_groupCount(other.m_groupCount)
    , m_refused(other.m_refused)
{
    m_series.reserve(other.m_series.size());
    for (const auto& s : other.m_series)
        m_series.push_back(std::make_unique<Series>(*s));
}

History& History::operator=(const History& other)
{
    if (this != &other) {
        History copy(other);
        *this = std::move(copy);
    }
    return *this;
}

void History::clear()
{
    m_head = 0;
    m_count = 0;
    m_open = false;
    m_series.clear();
    m_index.clear();
    m_groupCount.fill(0);
    m_refused.fill(0);
}

double History::latestTime() const
{
    return m_count ? time(m_count - 1) : kNaN;
}

void History::dropOldest()
{
    if (m_count == 0)
        return;
    m_head = (m_head + 1) % m_capacity;
    --m_count;
}

bool History::beginSample(double t, bool continuous)
{
    if (!std::isfinite(t))
        return false;
    if (m_open)
        endSample();
    if (m_count > 0 && !(t > latestTime()))
        return false;
    if (m_count == m_capacity)
        dropOldest();
    const std::size_t s = (m_head + m_count) % m_capacity;
    m_times[s] = t;
    m_continuous[s] = continuous ? 1 : 0;
    for (auto& series : m_series)
        series->values[s] = kNaN;
    ++m_count;
    ++m_total;
    m_open = true;
    m_refused.fill(0);
    return true;
}

bool History::set(std::string_view key, std::string_view label, Unit unit, double value, SeriesGroup group)
{
    if (!m_open)
        return false;
    Series* series = nullptr;
    if (auto it = m_index.find(key); it != m_index.end()) {
        series = m_series[it->second].get();
    } else {
        const auto g = static_cast<std::size_t>(group);
        if (m_series.size() >= kMaxSeries || m_groupCount[g] >= groupQuota(group)) {
            ++m_refused[g];
            return false;
        }
        auto created = std::make_unique<Series>();
        created->key = std::string(key);
        created->label = std::string(label);
        created->unit = unit;
        created->values.assign(m_capacity, kNaN);
        created->createdAt = m_total;
        created->group = group;
        ++m_groupCount[g];
        series = created.get();
        m_index.emplace(created->key, m_series.size());
        m_series.push_back(std::move(created));
    }
    const double v = std::isfinite(value) ? value : kNaN;
    series->values[slot(m_count - 1)] = v;
    if (std::isfinite(v)) {
        series->lastValidAt = m_total;
        series->everValid = true;
    }
    if (series->label != label)
        series->label = std::string(label);
    return true;
}

void History::endSample()
{
    if (!m_open)
        return;
    m_open = false;
    const double newest = latestTime();
    while (m_count > 1 && time(0) < newest - m_maxAge)
        dropOldest();
    pruneSeries();
}

void History::pruneSeries()
{
    // Absolute sample number of the oldest retained sample (samples are
    // numbered from 1 as they are begun).
    const uint64_t oldest = m_total - m_count + 1;
    bool removed = false;
    for (auto& s : m_series) {
        const uint64_t lastRelevant = std::max(s->createdAt, s->everValid ? s->lastValidAt : 0);
        if (lastRelevant < oldest) {
            --m_groupCount[static_cast<std::size_t>(s->group)];
            s.reset();
            removed = true;
        }
    }
    if (!removed)
        return;
    m_series.erase(std::remove(m_series.begin(), m_series.end(), nullptr), m_series.end());
    m_index.clear();
    for (std::size_t i = 0; i < m_series.size(); ++i)
        m_index.emplace(m_series[i]->key, i);
}

std::size_t History::lowerBound(double t) const
{
    std::size_t lo = 0, hi = m_count;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (time(mid) < t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

const History::Series* History::find(std::string_view key) const
{
    auto it = m_index.find(key);
    return it == m_index.end() ? nullptr : m_series[it->second].get();
}

double History::latestValue(std::string_view key) const
{
    const Series* s = find(key);
    return (s && m_count) ? value(*s, m_count - 1) : kNaN;
}

std::vector<const History::Series*> History::allSeries() const
{
    std::vector<const Series*> out;
    out.reserve(m_series.size());
    for (const auto& s : m_series)
        out.push_back(s.get());
    return out;
}

namespace {

void appendCsvField(std::string& out, std::string_view field)
{
    const bool quote = field.find_first_of(",\"\r\n") != std::string_view::npos;
    if (!quote) {
        out.append(field);
        return;
    }
    out.push_back('"');
    for (char c : field) {
        if (c == '"')
            out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
}

int csvDecimals(Unit unit)
{
    switch (unit) {
    case Unit::Percent: return 2;
    case Unit::Bytes: return 0;
    case Unit::BytesPerSecond: return 1;
    case Unit::Celsius: return 3;
    }
    return 3;
}

} // namespace

std::string History::toCsv(double unixOffset) const
{
    std::string out;
    out.reserve(64 + m_count * (24 + m_series.size() * 10));
    out.append("time_since_boot_s,unix_time_s,continuous");
    // Column names are labels; distinct series can share one (for example two
    // sensors with identical names), so repeated names also carry the key.
    std::vector<std::string> headers;
    std::map<std::string, int> seen;
    headers.reserve(m_series.size());
    for (const auto& s : m_series) {
        headers.push_back(s->label + " (" + unitSuffix(s->unit) + ")");
        ++seen[headers.back()];
    }
    for (std::size_t i = 0; i < m_series.size(); ++i) {
        out.push_back(',');
        appendCsvField(out, seen[headers[i]] > 1 ? headers[i] + " [" + m_series[i]->key + "]" : headers[i]);
    }
    out.append("\r\n");
    for (std::size_t i = 0; i < m_count; ++i) {
        const double t = time(i);
        out.append(fmt::number(t, 3));
        out.push_back(',');
        out.append(std::isfinite(unixOffset) ? fmt::number(t + unixOffset, 3) : std::string());
        out.push_back(',');
        out.push_back(continuous(i) ? '1' : '0');
        for (const auto& s : m_series) {
            out.push_back(',');
            const double v = value(*s, i);
            if (std::isfinite(v))
                out.append(fmt::number(v, csvDecimals(s->unit)));
        }
        out.append("\r\n");
    }
    return out;
}

// ---------------------------------------------------------------------------

void recordSnapshot(History& h, const Snapshot& s)
{
    if (!h.beginSample(s.time, s.continuous))
        return;
    using namespace keys;
    // Summary series first; every group has its own quota (see SeriesGroup).
    constexpr auto S = SeriesGroup::Summary;
    h.set(cpuTotal, "CPU total", Unit::Percent, s.cpu.total, S);
    h.set(cpuUser, "CPU user", Unit::Percent, s.cpu.user, S);
    h.set(cpuSystem, "CPU system", Unit::Percent, s.cpu.system, S);
    h.set(cpuIowait, "CPU I/O wait", Unit::Percent, s.cpu.iowait, S);
    if (s.memory.valid) {
        h.set(memUsed, "Memory used", Unit::Bytes, s.memory.used, S);
        h.set(memCache, "Memory buff/cache", Unit::Bytes, s.memory.cache, S);
        h.set(memAvailable, "Memory available", Unit::Bytes, s.memory.available, S);
    }
    if (s.memory.hasSwap)
        h.set(swapUsed, "Swap used", Unit::Bytes, s.memory.swapUsed, S);
    if (s.diskTotal.members > 0) {
        h.set(diskAllRead, "Disk read: " + s.diskTotal.label, Unit::BytesPerSecond, s.diskTotal.a, S);
        h.set(diskAllWrite, "Disk write: " + s.diskTotal.label, Unit::BytesPerSecond, s.diskTotal.b, S);
    }
    if (s.netTotal.members > 0) {
        h.set(netAllRx, "Receive: " + s.netTotal.label, Unit::BytesPerSecond, s.netTotal.a, S);
        h.set(netAllTx, "Transmit: " + s.netTotal.label, Unit::BytesPerSecond, s.netTotal.b, S);
    }

    for (const auto& c : s.cores)
        h.set(core(c.index), "CPU " + std::to_string(c.index), Unit::Percent, c.usage.total, SeriesGroup::Core);
    for (const auto& d : s.disks) {
        h.set(diskRead(d.name), "Disk read: " + d.name, Unit::BytesPerSecond, d.readBps, SeriesGroup::Disk);
        h.set(diskWrite(d.name), "Disk write: " + d.name, Unit::BytesPerSecond, d.writeBps, SeriesGroup::Disk);
    }
    for (const auto& n : s.interfaces) {
        h.set(netRx(n.name), "Receive: " + n.name, Unit::BytesPerSecond, n.rxBps, SeriesGroup::Network);
        h.set(netTx(n.name), "Transmit: " + n.name, Unit::BytesPerSecond, n.txBps, SeriesGroup::Network);
    }
    for (const auto& t : s.temperatures)
        h.set(temperature(t.key), "Temperature: " + t.displayName, Unit::Celsius, t.celsius,
              SeriesGroup::Temperature);

    h.endSample();
}

} // namespace ps
