// Delta math turning cumulative kernel counters into rates and percentages.
#pragma once

#include "core/linuxparsers.h"
#include "core/snapshot.h"

#include <cstdint>
#include <optional>

namespace ps::metrics {

// No storage or network device on hardware we can run on exceeds this; faster
// "rates" come from counter glitches and are rejected.
inline constexpr double kMaxPlausibleRate = 1024.0 * 1024.0 * 1024.0 * 1024.0; // 1 TiB/s
inline constexpr uint64_t kDiskSectorBytes = 512; // diskstats always uses 512-byte sectors

struct CpuDelta {
    uint64_t user = 0;   // user + nice
    uint64_t system = 0; // system + irq + softirq
    uint64_t idle = 0;   // idle + iowait
    uint64_t iowait = 0;
    uint64_t steal = 0;
    uint64_t total = 0;
    bool overflow = false; // set by operator+= if a sum did not fit
    CpuDelta& operator+=(const CpuDelta& o);
};

// Returns nullopt when any accounted field went backwards. Linux documents that
// iowait may decrease; such intervals are invalidated and the caller rebases.
std::optional<CpuDelta> cpuDelta(const parse::CpuTimes& prev, const parse::CpuTimes& cur);
CpuUsage usageFromDelta(const CpuDelta& d);
CpuUsage cpuUsage(const parse::CpuTimes& prev, const parse::CpuTimes& cur);

// Difference of a monotonically increasing counter. A decrease is treated as a
// reset (nullopt) unless counterBits is 32 and the previous value sat in the
// upper half of the range, which indicates a 32-bit wrap.
std::optional<uint64_t> counterDelta(uint64_t prev, uint64_t cur, int counterBits = 64);

// delta * scale / seconds, or NaN if any input is unusable or the result is
// implausible. The subtraction happens in integers before this is called.
double rate(std::optional<uint64_t> delta, double seconds, uint64_t scale = 1,
            double maxRate = kMaxPlausibleRate);

// Share of one logical CPU (100 = one core busy). May exceed 100 for
// multithreaded processes; NaN if impossible for the machine.
double processCpuPercent(uint64_t prevTicks, uint64_t curTicks, double seconds,
                         long clockTicks, int logicalCpus);

MemoryInfo memoryFromRaw(const parse::MemInfoRaw& raw);

// Largest gap between samples that is still treated as continuous.
double continuityLimit(double expectedInterval);
bool isDiscontinuous(double elapsed, double expectedInterval);

} // namespace ps::metrics
