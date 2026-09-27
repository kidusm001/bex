// Pure parsers for Linux /proc and /sys text formats. No I/O happens here so
// every function can be exercised with synthetic input.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps::parse {

// Strict unsigned/signed decimal parsing of a whole token.
std::optional<uint64_t> toU64(std::string_view token);
std::optional<int64_t> toI64(std::string_view token);
std::optional<double> toDouble(std::string_view token);

std::string_view trim(std::string_view s);
std::vector<std::string_view> splitWhitespace(std::string_view line);
std::vector<std::string_view> splitLines(std::string_view text);

// Replaces control characters so kernel-provided free text (labels, command
// lines) is safe to display. Lossy: use escapeName for identifiers.
std::string sanitize(std::string_view s);
// Injective, printable form of a kernel identifier (device, interface, mount
// and sensor names): control characters (C0, DEL, C1) and bytes that are not
// well-formed UTF-8 become \xNN per byte and a backslash is doubled, so the
// result is valid UTF-8 and round-trips through QString. Ordinary names
// (including valid non-ASCII UTF-8) are unchanged. Two distinct names never
// map to the same string, so the result is safe as a history key and a
// selection id.
std::string escapeName(std::string_view s);

// ---- /proc/stat -----------------------------------------------------------
struct CpuTimes {
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
    uint64_t irq = 0, softirq = 0, steal = 0, guest = 0, guestNice = 0;
    // Counters present on the row (4..10). Missing columns read as 0 and are
    // only comparable with a sample that had the same columns, so a row that
    // lost fields (truncated read) invalidates the interval instead of turning
    // historical iowait/irq/steal totals into a fake burst of activity.
    int fields = 10;
    // guest and guest_nice are already included in user and nice by the
    // kernel, so they are deliberately left out to avoid double counting.
    // Sum of the accounted columns; nullopt if it does not fit in 64 bits.
    std::optional<uint64_t> checkedTotal() const
    {
        uint64_t sum = 0;
        for (uint64_t v : {user, nice, system, idle, iowait, irq, softirq, steal}) {
            if (v > UINT64_MAX - sum)
                return std::nullopt;
            sum += v;
        }
        return sum;
    }
    // Saturates at UINT64_MAX instead of wrapping to a small, plausible value.
    uint64_t total() const { return checkedTotal().value_or(UINT64_MAX); }
};

struct CpuLine {
    int index = -1;
    CpuTimes times;
};

struct ProcStat {
    std::optional<CpuTimes> aggregate;
    std::vector<CpuLine> cpus; // online logical CPUs, sorted by index
    std::optional<uint64_t> bootTime;
};

ProcStat parseProcStat(std::string_view text);

// ---- /proc/meminfo (values converted to bytes) -----------------------------
struct MemInfoRaw {
    std::optional<uint64_t> memTotal, memFree, memAvailable, buffers, cached;
    std::optional<uint64_t> sReclaimable, shmem, swapTotal, swapFree;
};

MemInfoRaw parseMeminfo(std::string_view text);

// ---- /proc/diskstats --------------------------------------------------------
struct DiskStatLine {
    unsigned major = 0, minor = 0;
    std::string name;
    uint64_t sectorsRead = 0;    // always 512-byte units
    uint64_t sectorsWritten = 0;
    uint64_t ioTicksMs = 0;      // milliseconds spent with I/O in flight
};

std::vector<DiskStatLine> parseDiskstats(std::string_view text);

// ---- /proc/net/dev ----------------------------------------------------------
struct NetDevLine {
    std::string name;
    uint64_t rxBytes = 0;
    uint64_t txBytes = 0;
};

std::vector<NetDevLine> parseNetDev(std::string_view text);

// ---- /proc/<pid>/stat, io, status, cmdline ---------------------------------
inline constexpr unsigned kPfKthread = 0x00200000;

struct PidStat {
    int pid = 0;
    std::string comm;
    char state = '?';
    int ppid = 0;
    unsigned flags = 0;
    uint64_t utime = 0, stime = 0;
    int64_t nice = 0;
    int64_t numThreads = 0;
    uint64_t startTime = 0; // clock ticks after boot
    int64_t rssPages = 0;
};

std::optional<PidStat> parsePidStat(std::string_view text);

struct PidIo {
    uint64_t readBytes = 0;
    uint64_t writeBytes = 0;
};

std::optional<PidIo> parsePidIo(std::string_view text);
std::optional<uint32_t> parseStatusRealUid(std::string_view text);
std::string parseCmdline(std::string_view raw);

// ---- /proc/mounts ----------------------------------------------------------
struct MountEntry {
    std::string source, mountPoint, fsType, options;
    // Only filled by parseMountInfo (/proc/self/mountinfo).
    int id = -1, parentId = -1;
    std::string root; // mountinfo field 4: the directory of the filesystem mounted here
    unsigned major = 0, minor = 0;
    bool hasDevice = false;
    bool readOnly() const;
};

std::string unescapeMountField(std::string_view field);
std::vector<MountEntry> parseMounts(std::string_view text);
// True for local, block-device backed filesystems whose capacity can be read
// with statvfs without risking a hang on network or FUSE daemons.
bool isLocalCapacityFs(const MountEntry& entry);

// /proc/self/mountinfo, which adds mount/parent ids and the device number.
std::vector<MountEntry> parseMountInfo(std::string_view text);
// True for in-kernel filesystems whose directory lookups never wait on a
// remote server or a userspace daemon (unlike NFS, FUSE or autofs).
bool isLookupSafeFs(std::string_view fsType);

// Simulates path resolution over a mountinfo table: which mount does `path`
// (absolute, canonical) reach, and did every mount crossed on the way (the
// ones looked up in, or stacked under the result) have a lookup-safe type?
// This catches entries hidden by a later mount on the same or a parent
// directory, and paths that would walk through a network, FUSE or autofs
// mount. index is -1 if the path cannot be resolved.
struct MountResolution {
    int index = -1;
    bool safe = false;
};
MountResolution resolveMountPath(const std::vector<MountEntry>& table, std::string_view path);

// ---- misc ------------------------------------------------------------------
// hwmon/thermal millidegree text to Celsius; rejects impossible readings.
std::optional<double> parseMilliCelsius(std::string_view text);

struct LoadAvg {
    double one = 0, five = 0, fifteen = 0;
};
std::optional<LoadAvg> parseLoadavg(std::string_view text);
std::optional<double> parseUptime(std::string_view text);

} // namespace ps::parse
