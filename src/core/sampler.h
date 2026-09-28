// Reads /proc and /sys, keeps the previous counters needed for deltas, and
// produces Snapshot values. Not thread-safe; owned by the collector thread.
#pragma once

#include "core/linuxparsers.h"
#include "core/snapshot.h"

#include <cstdint>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps {

struct FsStats {
    uint64_t blocks = 0; // f_blocks, in f_frsize units
    uint64_t bfree = 0;  // free blocks including root-reserved ones
    uint64_t bavail = 0; // free blocks available to unprivileged users
    uint64_t frsize = 0; // fundamental block size
    // st_dev of the probed directory. Informational only: btrfs subvolumes
    // legitimately report an anonymous device that differs from mountinfo.
    uint64_t device = 0;
    bool hasDevice = false;
    // Mount ID (mountinfo field 1) of the mount the numbers were read from.
    // When reported it must equal the ID the probe was requested for.
    int mountId = -1;
    bool hasMountId = false;
};

struct SamplerOptions {
    std::string procRoot = "/proc";
    std::string sysRoot = "/sys";
    long clockTicks = 0; // 0: sysconf(_SC_CLK_TCK)
    long pageSize = 0;   // 0: sysconf(_SC_PAGESIZE)
    int counterBits = 0; // width of kernel "unsigned long" counters; 0: native
    bool scanProcesses = true;
    double sensorRediscoverySeconds = 10.0;
    // Returns nullopt and sets errnoOut on failure. Runs on the capacity-probe
    // thread, never on the sampling path, so it may block (a failing disk)
    // without stalling samples. Default (empty): pin the mount point with
    // open(O_PATH), read the descriptor's mount ID (statx STATX_MNT_ID, or
    // /proc/self/fdinfo before Linux 5.8), confirm in mountinfo that this ID
    // still has the expected device, root and source, and only then call
    // fstatvfs on the same descriptor. A replacement that reports mountId
    // gets the same ID check.
    std::function<std::optional<FsStats>(const std::string& path, int& errnoOut)> statFs;
    // Default: getpwuid_r, falling back to the numeric uid.
    std::function<std::string(uint32_t uid)> userName;
    // Capacity results older than this are reported unavailable ("not
    // responding"); 0 means max(5 s, 3 x the expected interval).
    double fsStaleSeconds = 0;
    // Test hook: how long sample() may wait for the capacity probe. The
    // default 0 never waits; results are used once they are ready.
    int fsProbeWaitMs = 0;
    // Test hook: monotonic seconds used to time the reads inside one sample
    // (see sample()). Default (empty): std::chrono::steady_clock.
    std::function<double()> stopwatch;
};

// Seconds on CLOCK_BOOTTIME: monotonic, and it keeps counting during suspend
// so a suspend shows up as a real gap on time axes.
double monotonicSeconds();

class Sampler {
public:
    explicit Sampler(SamplerOptions options = {});
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    // Takes one sample at time `now` (monotonicSeconds() domain). A sample is
    // continuous with the previous one unless baselines were reset or the gap
    // exceeds metrics::continuityLimit(expectedInterval); non-continuous
    // samples report rates as unavailable and re-establish the baselines.
    // Rates divide by `now` minus the previous `now`, so they also need the
    // counters to be read as promptly after `now` as last time. When that
    // read offset moved by more than readSkewLimit(expectedInterval) (a
    // starved thread, a stalled kernel read), the sample's time-based rates
    // (transfer, disk busy, per-process CPU and I/O) are reported unavailable
    // instead of as a spike or dip; the next sample is judged the same way.
    // Each process's stat and io reads are also timed individually against
    // the same point in its previous sample, so a delay inside the process
    // scan withholds just the affected processes' rates.
    Snapshot sample(double now, double expectedInterval);
    static double readSkewLimit(double expectedInterval);
    void resetBaselines();

    const SamplerOptions& options() const { return m_opt; }

private:
    struct DiskState {
        std::string identity;
        uint64_t sectorsRead = 0, sectorsWritten = 0, ioTicks = 0;
        bool hasPrev = false;
    };
    struct NetState {
        std::string identity;
        DeviceKind kind = DeviceKind::Virtual;
        uint64_t rx = 0, tx = 0;
        bool hasPrev = false;
    };
    // When a read happened: the middle of the read call, in seconds after the
    // sample's start, and half the call's duration.
    struct ReadTime {
        double at = std::numeric_limits<double>::quiet_NaN();
        double spread = 0;
    };
    struct ProcState {
        uint64_t startTicks = 0;
        ReadTime statTime, ioTime; // of the readings in cpuTicks and ioRead/ioWrite
        double metaDueAt = 0;      // sample time at which uid/command are read again
        uint64_t cpuTicks = 0;
        bool hasCpu = false; // cpuTicks is a usable baseline
        uint64_t ioRead = 0, ioWrite = 0;
        bool hasIo = false;
        bool ioDenied = false;
        bool hasPrev = false;
        double ioRetryAt = 0; // when a denied /proc/PID/io is tried again
        uint32_t uid = 0;
        bool uidKnown = false;
        std::string comm, command, user;
        uint64_t seenScan = 0;
    };
    struct SensorInput {
        std::string key, chip, device, label;
        std::string displayPath; // sysfs input path, for tooltips
        std::string inputName;   // file inside the pinned directory
        std::string faultName;   // empty if the driver has no fault attribute
        int dir = -1;            // index into m_sensorDirs
    };
    // Which mount a capacity probe is for. Requests, results, errors and
    // cached answers all carry it, and an answer is only shown for a target
    // with an identical identity.
    struct FsIdentity {
        std::string path; // mount point that is probed
        int mountId = -1;
        unsigned major = 0, minor = 0;
        std::string root, source;
        bool operator==(const FsIdentity&) const = default;
    };
    struct FsTarget {
        std::size_t group = 0;
        FsIdentity id;
    };
    struct FsCached {
        std::optional<FsStats> stats;
        int err = 0;
        FsIdentity id; // identity the result was requested for
        double time = 0; // submit time of the batch that produced it
    };
    class FsProber;

    void sampleCpu(Snapshot& s, bool baseline);
    void sampleMemory(Snapshot& s);
    void sampleLoad(Snapshot& s);
    void sampleDisks(Snapshot& s, bool baseline, double elapsed);
    void sampleNetwork(Snapshot& s, bool baseline, double elapsed);
    void prepareFilesystems(Snapshot& s, double now);
    void finishFilesystems(Snapshot& s, double now, double expectedInterval);
    void sampleTemperatures(Snapshot& s, double now);
    void discoverSensors();
    void closeSensorDirs();
    void sampleProcesses(Snapshot& s, bool baseline, double elapsed, double uptime, double now);
    std::string userFor(uint32_t uid);
    double stopwatch() const;
    ReadTime readTime(double before, double after) const;
    bool sameTiming(const ReadTime& a, const ReadTime& b) const;

    SamplerOptions m_opt;
    long m_clockTicks = 100;
    long m_pageSize = 4096;
    int m_counterBits = 64;

    bool m_havePrev = false;
    bool m_forceReset = false;
    double m_prevTime = 0;
    double m_prevReadOffset = 0; // seconds from sample start to the last counter read
    double m_sampleStarted = 0;  // stopwatch() at the start of the current sample
    double m_skewLimit = 0.1;    // readSkewLimit() of the current sample
    uint64_t m_sequence = 0;

    std::optional<parse::CpuTimes> m_prevCpuAggregate;
    std::map<int, parse::CpuTimes> m_prevCores;
    std::set<int> m_knownCores;
    std::map<std::string, DiskState> m_disks;
    std::map<std::string, NetState> m_nets;
    std::unordered_map<int, ProcState> m_procs;
    uint64_t m_scanNo = 0;
    std::unordered_map<uint32_t, std::string> m_users;

    std::vector<Filesystem> m_fsGroups;
    std::vector<FsTarget> m_fsTargets;
    std::map<std::string, FsCached> m_fsCache;
    std::unique_ptr<FsProber> m_prober;

    std::vector<SensorInput> m_sensors;
    std::vector<int> m_sensorDirs; // pinned hwmon/thermal directory fds
    std::set<std::string> m_everValidSensors;
    bool m_hwmonPresent = false;
    bool m_thermalPresent = false;
    bool m_rediscover = true;
    double m_lastDiscovery = 0;
};

} // namespace ps
