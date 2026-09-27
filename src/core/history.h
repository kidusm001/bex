// Bounded, time-aligned metric history. All series share one timestamp ring so
// a sample index addresses the same instant in every series; values missing
// from a sample are stored as NaN (a gap, never zero).
#pragma once

#include "core/snapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ps {

enum class Unit { Percent, Bytes, BytesPerSecond, Celsius };
const char* unitSuffix(Unit unit); // "%", "bytes", "B/s", "°C"

// Categories for the per-group series quotas. Each group has its own quota and
// the quotas add up to History::kMaxSeries, so one flooded category (hundreds of
// CPU cores, churning virtual interfaces) can never crowd out the others: the
// summary series (CPU/memory/swap and the disk/network sums) always fit.
// Ungrouped series (None) are limited only by the overall cap.
enum class SeriesGroup : uint8_t { None, Summary, Core, Disk, Network, Temperature };
inline constexpr std::size_t kSeriesGroupCount = 6;

class History {
public:
    struct Series {
        std::string key;
        std::string label;
        Unit unit = Unit::Percent;
        std::vector<double> values; // ring, aligned with the timestamp ring
        uint64_t createdAt = 0;     // absolute sample number
        uint64_t lastValidAt = 0;   // absolute sample number of last finite value
        bool everValid = false;
        SeriesGroup group = SeriesGroup::None;
    };

    static constexpr std::size_t kDefaultCapacity = 1800; // 15 min at 0.5 s
    static constexpr double kDefaultMaxAge = 900.0;       // seconds
    static constexpr std::size_t kMaxSeries = 512;
    // Series quota of a group (the sum over groups is kMaxSeries); None: kMaxSeries.
    static constexpr std::size_t groupQuota(SeriesGroup group)
    {
        switch (group) {
        case SeriesGroup::Summary: return 16;
        case SeriesGroup::Core: return 256;
        case SeriesGroup::Disk: return 80;
        case SeriesGroup::Network: return 80;
        case SeriesGroup::Temperature: return 80;
        case SeriesGroup::None: break;
        }
        return kMaxSeries;
    }

    explicit History(std::size_t capacity = kDefaultCapacity, double maxAgeSeconds = kDefaultMaxAge);
    // Deep copies (the CSV exporter works on a private copy off the UI thread).
    History(const History& other);
    History& operator=(const History& other);
    History(History&&) noexcept = default;
    History& operator=(History&&) noexcept = default;
    ~History() = default;

    // Starts a sample; times must strictly increase (returns false otherwise).
    bool beginSample(double time, bool continuous);
    // Stores a value for the open sample; creates the series on first use.
    // Creation fails (returns false) when the overall cap or the group's quota
    // is full; the refusal is counted for refusedInLastSample().
    bool set(std::string_view key, std::string_view label, Unit unit, double value,
             SeriesGroup group = SeriesGroup::None);
    // Closes the sample and drops samples/series older than the retention window.
    void endSample();
    void clear();

    std::size_t size() const { return m_count; }
    std::size_t capacity() const { return m_capacity; }
    double maxAge() const { return m_maxAge; }
    uint64_t totalSamples() const { return m_total; }
    bool empty() const { return m_count == 0; }

    double time(std::size_t i) const { return m_times[slot(i)]; }
    bool continuous(std::size_t i) const { return m_continuous[slot(i)] != 0; }
    double latestTime() const;
    // Index of the first sample with time >= t (size() if none).
    std::size_t lowerBound(double t) const;

    const Series* find(std::string_view key) const;
    double value(const Series& s, std::size_t i) const { return s.values[slot(i)]; }
    double latestValue(std::string_view key) const;
    std::size_t seriesCount() const { return m_series.size(); }
    std::size_t seriesCount(SeriesGroup group) const { return m_groupCount[static_cast<std::size_t>(group)]; }
    // Series creations refused in the latest sample because the overall cap or
    // the group quota was full (recordSnapshot sets each key once per sample).
    std::size_t refusedInLastSample(SeriesGroup group) const
    {
        return m_refused[static_cast<std::size_t>(group)];
    }
    std::vector<const Series*> allSeries() const;

    // CSV with a header row carrying units; unavailable values are blank.
    // unixOffset converts the stored boot-time clock to Unix time.
    std::string toCsv(double unixOffset) const;

private:
    std::size_t slot(std::size_t i) const { return (m_head + i) % m_capacity; }
    void dropOldest();
    void pruneSeries();

    std::size_t m_capacity;
    double m_maxAge;
    std::vector<double> m_times;
    std::vector<uint8_t> m_continuous;
    std::size_t m_head = 0;
    std::size_t m_count = 0;
    uint64_t m_total = 0; // samples ever begun
    bool m_open = false;
    std::vector<std::unique_ptr<Series>> m_series;
    std::map<std::string, std::size_t, std::less<>> m_index;
    std::array<std::size_t, kSeriesGroupCount> m_groupCount {};
    std::array<std::size_t, kSeriesGroupCount> m_refused {};
};

// Series keys shared by the recorder and the UI.
namespace keys {
inline std::string core(int index) { return "cpu.core." + std::to_string(index); }
inline std::string diskRead(std::string_view dev) { return "disk.dev." + std::string(dev) + ".read"; }
inline std::string diskWrite(std::string_view dev) { return "disk.dev." + std::string(dev) + ".write"; }
inline std::string netRx(std::string_view dev) { return "net.if." + std::string(dev) + ".rx"; }
inline std::string netTx(std::string_view dev) { return "net.if." + std::string(dev) + ".tx"; }
inline std::string temperature(std::string_view key) { return "temp." + std::string(key); }
inline constexpr std::string_view cpuTotal = "cpu.total", cpuUser = "cpu.user", cpuSystem = "cpu.system",
                                  cpuIowait = "cpu.iowait", memUsed = "mem.used", memCache = "mem.cache",
                                  memAvailable = "mem.available", swapUsed = "swap.used",
                                  diskAllRead = "disk.all.read", diskAllWrite = "disk.all.write",
                                  netAllRx = "net.all.rx", netAllTx = "net.all.tx";
} // namespace keys

void recordSnapshot(History& history, const Snapshot& snapshot);

} // namespace ps
