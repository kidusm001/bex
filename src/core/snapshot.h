// Immutable value types describing one sample of the system. Produced by the
// Sampler on the collector thread and shared read-only with the UI.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace ps {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Every metric uses NaN for "unavailable" (not measured, warming up, stale or
// rejected as impossible). Zero always means a real measured zero.
inline bool isValid(double v) { return std::isfinite(v); }

struct CpuUsage {
    double total = kNaN;  // percent of the CPU(s) covered, 0..100
    double user = kNaN;   // user + nice (includes guest time, counted once)
    double system = kNaN; // system + irq + softirq
    double iowait = kNaN;
    double steal = kNaN;
};

struct CoreUsage {
    int index = 0;
    bool online = false;
    CpuUsage usage;
};

// Bytes. Conventions match procps-ng >= 4 `free`:
//   used      = MemTotal - MemAvailable
//   cache     = Buffers + Cached + SReclaimable (overlaps "available")
//   swap used = SwapTotal - SwapFree
struct MemoryInfo {
    bool valid = false;
    double total = kNaN;
    double used = kNaN;
    double available = kNaN;
    double free = kNaN;
    double cache = kNaN;
    double shared = kNaN;
    bool hasSwap = false;
    double swapTotal = kNaN;
    double swapUsed = kNaN;
    double swapFree = kNaN;
};

enum class DeviceKind { Physical, Virtual, Loopback };

struct DiskIo {
    std::string name;    // kernel name, e.g. nvme0n1
    std::string label;   // optional friendly name, e.g. device-mapper name
    DeviceKind kind = DeviceKind::Physical;
    double sizeBytes = kNaN;
    double readBps = kNaN;
    double writeBps = kNaN;
    double busyPercent = kNaN; // share of wall time with I/O in flight
};

struct NetIo {
    std::string name;
    DeviceKind kind = DeviceKind::Physical;
    std::string operState; // up, down, unknown, dormant, ...
    double rxBps = kNaN;
    double txBps = kNaN;
};

// Sum over a set of devices that do not overlap each other.
struct AggregateRate {
    std::string label;
    int members = 0;
    double a = kNaN; // read or receive
    double b = kNaN; // write or transmit
};

struct Filesystem {
    std::string source;                   // e.g. /dev/nvme0n1p3
    std::string fsType;
    std::vector<std::string> mountPoints; // all mount points sharing the source
    bool readOnly = false;
    double totalBytes = kNaN;
    double usedBytes = kNaN;
    double availBytes = kNaN; // available to unprivileged users (f_bavail)
    std::string error;        // set when capacity could not be read
};

struct TemperatureReading {
    std::string key;         // stable identity for history/selection
    std::string chip;        // hwmon name or "thermal zone"
    std::string label;       // sensor label
    std::string displayName; // chip + label, disambiguated
    std::string path;        // sysfs input file, for tooltips
    double celsius = kNaN;
};

struct ProcessInfo {
    int pid = 0;
    int ppid = 0;
    uint64_t startTicks = 0; // with pid, the process identity (survives PID reuse checks)
    uint32_t uid = 0;
    bool uidKnown = false;
    bool kernelThread = false;
    char state = '?';
    int threads = 0;
    int nice = 0;
    std::string name;    // kernel comm
    std::string command; // full command line (or [name] for kernel threads)
    std::string user;
    double cpuPercent = kNaN; // 100 = one logical CPU fully busy
    double rssBytes = kNaN;
    double ioReadBps = kNaN;  // storage-layer bytes, needs ptrace-level access
    double ioWriteBps = kNaN;
    bool ioDenied = false;
    double ageSeconds = kNaN;
};

struct Snapshot {
    uint64_t sequence = 0;
    uint64_t generation = 0; // bumped by the collector on every resume
    double time = kNaN;      // CLOCK_BOOTTIME seconds (monotonic, counts suspend)
    double interval = kNaN;  // seconds since the previous sample of the same run
    bool continuous = false; // false for the first sample after start/resume/gap
    double collectMs = kNaN;

    int logicalCpus = 0;
    CpuUsage cpu;
    std::vector<CoreUsage> cores;
    double load1 = kNaN, load5 = kNaN, load15 = kNaN;

    MemoryInfo memory;

    std::vector<DiskIo> disks;
    AggregateRate diskTotal;
    std::vector<NetIo> interfaces;
    AggregateRate netTotal;

    std::vector<Filesystem> filesystems;

    std::vector<TemperatureReading> temperatures;
    std::string temperatureNote; // why nothing is shown, when empty

    bool processesScanned = false;
    std::vector<ProcessInfo> processes;
    long threadCount = 0;

    std::vector<std::string> problems; // unreadable sources this sample
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

} // namespace ps
