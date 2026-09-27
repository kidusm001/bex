#include "core/sampler.h"

#include "core/metrics.h"

#ifndef __linux__
#error "PulseScope's sampler reads Linux /proc and /sys and only supports Linux."
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace ps {

namespace {

// Reads a whole (small) file. Returns false with errOut set on failure.
bool readFile(const std::string& path, std::string& out, int* errOut = nullptr,
              std::size_t maxBytes = 8u << 20)
{
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        if (errOut)
            *errOut = errno;
        return false;
    }
    char buf[16384];
    while (out.size() < maxBytes) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errOut)
                *errOut = errno;
            ::close(fd);
            return false;
        }
        if (n == 0)
            break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return true;
}

std::string readTrimmed(const std::string& path, int* errOut = nullptr)
{
    std::string s;
    if (!readFile(path, s, errOut, 4096))
        return {};
    return std::string(parse::trim(s));
}

// Reads up to cap bytes of dirfd-relative `rel`; -1 with errOut on failure.
ssize_t readAt(int dirfd, const char* rel, char* buf, std::size_t cap, int* errOut)
{
    const int fd = ::openat(dirfd, rel, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        if (errOut)
            *errOut = errno;
        return -1;
    }
    std::size_t total = 0;
    while (total < cap) {
        const ssize_t n = ::read(fd, buf + total, cap - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errOut)
                *errOut = errno;
            ::close(fd);
            return -1;
        }
        if (n == 0)
            break;
        total += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return static_cast<ssize_t>(total);
}

bool exists(const std::string& path)
{
    return ::access(path.c_str(), F_OK) == 0;
}

std::vector<std::string> listDir(const std::string& path, bool* openedOut = nullptr)
{
    std::vector<std::string> names;
    DIR* dir = ::opendir(path.c_str());
    if (openedOut)
        *openedOut = dir != nullptr;
    if (!dir)
        return names;
    while (const dirent* e = ::readdir(dir)) {
        if (e->d_name[0] == '.')
            continue;
        names.emplace_back(e->d_name);
    }
    ::closedir(dir);
    return names;
}

// Lists a directory held open as dirfd (the descriptor stays owned by caller).
std::vector<std::string> listDirAt(int dirfd)
{
    std::vector<std::string> names;
    const int fd = ::openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return names;
    DIR* dir = ::fdopendir(fd);
    if (!dir) {
        ::close(fd);
        return names;
    }
    while (const dirent* e = ::readdir(dir)) {
        if (e->d_name[0] == '.')
            continue;
        names.emplace_back(e->d_name);
    }
    ::closedir(dir);
    return names;
}

std::string readTrimmedAt(int dirfd, const char* name)
{
    char buf[4096];
    const ssize_t n = readAt(dirfd, name, buf, sizeof buf, nullptr);
    if (n <= 0)
        return {};
    return std::string(parse::trim(std::string_view(buf, static_cast<std::size_t>(n))));
}

std::string realPath(const std::string& path)
{
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved)
        return {};
    std::string out(resolved);
    std::free(resolved);
    return out;
}

std::string baseName(const std::string& path)
{
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool startsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Natural ordering so hwmon10 sorts after hwmon9 and temp10 after temp9.
bool naturalLess(const std::string& a, const std::string& b)
{
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            std::size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei])))
                ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej])))
                ++ej;
            const auto na = std::string_view(a).substr(i, ei - i);
            const auto nb = std::string_view(b).substr(j, ej - j);
            if (na.size() != nb.size())
                return na.size() < nb.size();
            if (na != nb)
                return na < nb;
            i = ei;
            j = ej;
        } else {
            if (a[i] != b[j])
                return a[i] < b[j];
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

// Capacity probe outcomes that are not errno values.
constexpr int kErrMountChanged = -1001; // the path no longer names the requested mount
constexpr int kErrNoMountId = -1002;    // the kernel cannot tell which mount a descriptor is on

// Mount ID of the mount an open descriptor is on (mountinfo field 1).
std::optional<int> mountIdOf(int fd)
{
    struct statx sx {};
    if (::statx(fd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_MNT_ID, &sx) == 0 && (sx.stx_mask & STATX_MNT_ID)
        && sx.stx_mnt_id <= INT32_MAX)
        return static_cast<int>(sx.stx_mnt_id);
    // Kernels before 5.8 have no STATX_MNT_ID but report it in fdinfo (3.15+).
    std::string text;
    if (readFile("/proc/self/fdinfo/" + std::to_string(fd), text)) {
        for (auto line : parse::splitLines(text)) {
            if (line.substr(0, 7) == "mnt_id:") {
                const auto v = parse::toU64(parse::trim(line.substr(7)));
                if (v && *v <= INT32_MAX)
                    return static_cast<int>(*v);
            }
        }
    }
    return std::nullopt;
}

FsStats fsStatsFrom(const struct statvfs& st)
{
    FsStats out;
    out.blocks = st.f_blocks;
    out.bfree = st.f_bfree;
    out.bavail = st.f_bavail;
    out.frsize = st.f_frsize ? st.f_frsize : st.f_bsize;
    return out;
}

std::string defaultUserName(uint32_t uid)
{
    long size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (size <= 0)
        size = 16384;
    std::vector<char> buf(static_cast<std::size_t>(size));
    passwd pw {};
    passwd* result = nullptr;
    if (::getpwuid_r(uid, &pw, buf.data(), buf.size(), &result) == 0 && result && result->pw_name)
        return parse::sanitize(result->pw_name);
    return std::to_string(uid);
}

// One component of a colon-separated sensor key. escapeName is injective and
// never emits a raw backslash, so also escaping ":" keeps the joined key
// injective even though sysfs paths contain colons (PCI addresses).
std::string keyPart(std::string_view s)
{
    std::string out;
    for (char c : parse::escapeName(s)) {
        if (c == ':')
            out += "\\x3a";
        else
            out.push_back(c);
    }
    return out;
}

std::string errnoText(int err)
{
    if (err == kErrMountChanged)
        return "mount changed while measuring";
    if (err == kErrNoMountId)
        return "cannot verify which filesystem is mounted here; capacity not read";
    return err == EACCES || err == EPERM ? "permission denied" : std::strerror(err);
}

} // namespace

// Capacity probing on one long-lived thread with at most one outstanding
// batch. Sampling never waits for it. All state the thread touches lives in a
// shared block, so if a probe is stuck in an uninterruptible kernel call when
// the Sampler is destroyed, the thread is detached and later finishes against
// that block without referencing the Sampler.
class Sampler::FsProber {
public:
    using StatFn = std::function<std::optional<FsStats>(const std::string&, int&)>;
    // A request and its answer (value or error) both carry the full mount
    // identity the request was made for, so an answer can never be attributed
    // to a different filesystem mounted at the same path later.
    using Job = FsIdentity;
    struct Result {
        Job target;
        std::optional<FsStats> stats;
        int err = 0;
    };

    // An empty fn selects the production probe (probeBatch).
    explicit FsProber(StatFn fn)
        : m_state(std::make_shared<State>())
    {
        m_state->fn = std::move(fn);
    }

    ~FsProber()
    {
        if (!m_thread.joinable())
            return;
        std::unique_lock lock(m_state->mutex);
        m_state->stop = true;
        m_state->cv.notify_all();
        // A healthy probe finishes in microseconds; give it a moment, then
        // abandon a kernel-blocked one instead of hanging shutdown.
        const bool idle = m_state->cv.wait_for(lock, std::chrono::milliseconds(200),
                                               [this] { return !m_state->running; });
        lock.unlock();
        if (idle)
            m_thread.join();
        else
            m_thread.detach();
    }

    bool busy() const
    {
        std::lock_guard lock(m_state->mutex);
        return m_state->pending || m_state->running;
    }

    // Queues a batch unless one is outstanding. Returns false when busy.
    bool submit(std::vector<Job> jobs, double time)
    {
        {
            std::lock_guard lock(m_state->mutex);
            if (m_state->pending || m_state->running)
                return false;
            m_state->job = std::move(jobs);
            m_state->jobTime = time;
            m_state->pending = true;
        }
        if (!m_thread.joinable())
            m_thread = std::thread(&FsProber::run, m_state);
        m_state->cv.notify_all();
        return true;
    }

    void waitIdle(int ms)
    {
        std::unique_lock lock(m_state->mutex);
        m_state->cv.wait_for(lock, std::chrono::milliseconds(ms),
                             [this] { return !m_state->pending && !m_state->running; });
    }

    // Moves out a completed batch, if one finished since the last call.
    bool take(std::vector<Result>& out, double& batchTime)
    {
        std::lock_guard lock(m_state->mutex);
        if (!m_state->haveDone)
            return false;
        out = std::move(m_state->done);
        batchTime = m_state->doneTime;
        m_state->done.clear();
        m_state->haveDone = false;
        return true;
    }

    // Seconds the outstanding batch has been waiting, or 0 when idle.
    double outstandingSince() const
    {
        std::lock_guard lock(m_state->mutex);
        return (m_state->pending || m_state->running) ? m_state->jobTime : 0.0;
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        StatFn fn;
        std::atomic<bool> stop{false};
        bool pending = false;
        bool running = false;
        std::vector<Job> job;
        double jobTime = 0;
        bool haveDone = false;
        std::vector<Result> done;
        double doneTime = 0;
    };

    static void run(std::shared_ptr<State> st)
    {
        std::unique_lock lock(st->mutex);
        for (;;) {
            st->cv.wait(lock, [&] { return st->stop || st->pending; });
            if (st->stop)
                return;
            std::vector<Job> job = std::move(st->job);
            const double time = st->jobTime;
            st->job.clear();
            st->running = true;
            lock.unlock();

            std::vector<Result> results;
            if (st->fn) {
                results.reserve(job.size());
                for (auto& target : job) {
                    Result r;
                    r.target = std::move(target);
                    r.stats = st->fn(r.target.path, r.err);
                    if (r.stats && r.stats->hasMountId && r.stats->mountId != r.target.mountId) {
                        r.stats.reset();
                        r.err = kErrMountChanged;
                    }
                    results.push_back(std::move(r));
                    if (st->stop.load())
                        break;
                }
            } else {
                results = probeBatch(std::move(job), st->stop);
            }

            lock.lock();
            st->done = std::move(results);
            st->doneTime = time;
            st->haveDone = true;
            st->running = false;
            st->pending = false;
            st->cv.notify_all();
        }
    }

    // Production probe. Every mount point is pinned with an O_PATH descriptor
    // whose mount ID must be the requested one. A pinned mount cannot be
    // freed, so its ID cannot be reused while held; reading mountinfo after
    // pinning and finding that ID with the expected device, root and source
    // therefore proves which filesystem the descriptor is on. Only then is
    // fstatvfs() called on that same descriptor. st_dev is deliberately not
    // compared: btrfs subvolumes report an anonymous device number.
    static std::vector<Result> probeBatch(std::vector<Job> jobs, const std::atomic<bool>& stop)
    {
        std::vector<Result> results(jobs.size());
        std::vector<int> fds(jobs.size(), -1);
        bool anyPinned = false;
        for (std::size_t i = 0; i < jobs.size() && !stop.load(); ++i) {
            auto& r = results[i];
            r.target = std::move(jobs[i]);
            const int fd = ::open(r.target.path.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) {
                r.err = errno;
                continue;
            }
            const auto id = mountIdOf(fd);
            if (!id || *id != r.target.mountId) {
                r.err = id ? kErrMountChanged : kErrNoMountId;
                ::close(fd);
                continue;
            }
            fds[i] = fd;
            anyPinned = true;
        }
        if (anyPinned) {
            std::string text;
            int readErr = 0;
            const bool haveTable = readFile("/proc/self/mountinfo", text, &readErr);
            const auto table = haveTable ? parse::parseMountInfo(text) : std::vector<parse::MountEntry>{};
            for (std::size_t i = 0; i < fds.size(); ++i) {
                if (fds[i] < 0)
                    continue;
                auto& r = results[i];
                const auto& want = r.target;
                const auto e = std::find_if(table.begin(), table.end(),
                                            [&](const parse::MountEntry& m) { return m.id == want.mountId; });
                struct statvfs sv {};
                if (!haveTable) {
                    r.err = readErr ? readErr : EIO;
                } else if (e == table.end() || e->major != want.major || e->minor != want.minor
                           || e->root != want.root || e->source != want.source) {
                    r.err = kErrMountChanged;
                } else if (stop.load()) {
                    r.err = EINTR;
                } else if (::fstatvfs(fds[i], &sv) != 0) {
                    r.err = errno;
                } else {
                    r.stats = fsStatsFrom(sv);
                    r.stats->mountId = want.mountId;
                    r.stats->hasMountId = true;
                }
                ::close(fds[i]);
            }
        }
        return results;
    }

    std::shared_ptr<State> m_state;
    std::thread m_thread;
};

double monotonicSeconds()
{
    timespec ts {};
    ::clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

Sampler::Sampler(SamplerOptions options)
    : m_opt(std::move(options))
{
    m_clockTicks = m_opt.clockTicks > 0 ? m_opt.clockTicks : ::sysconf(_SC_CLK_TCK);
    if (m_clockTicks <= 0)
        m_clockTicks = 100;
    m_pageSize = m_opt.pageSize > 0 ? m_opt.pageSize : ::sysconf(_SC_PAGESIZE);
    if (m_pageSize <= 0)
        m_pageSize = 4096;
    m_counterBits = m_opt.counterBits > 0 ? m_opt.counterBits : static_cast<int>(sizeof(unsigned long) * 8);
    if (!m_opt.userName)
        m_opt.userName = defaultUserName;
    m_prober = std::make_unique<FsProber>(m_opt.statFs);
}

Sampler::~Sampler()
{
    closeSensorDirs();
}

void Sampler::resetBaselines()
{
    m_forceReset = true;
}

double Sampler::readSkewLimit(double expectedInterval)
{
    const double e = expectedInterval > 0 && std::isfinite(expectedInterval) ? expectedInterval : 1.0;
    return std::max(0.05, 0.1 * e);
}

Snapshot Sampler::sample(double now, double expectedInterval)
{
    const auto stopwatch = [this] {
        if (m_opt.stopwatch)
            return m_opt.stopwatch();
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const double started = stopwatch();
    Snapshot s;
    s.sequence = ++m_sequence;
    s.time = now;

    const double elapsed = now - m_prevTime;
    const bool baseline = m_havePrev && !m_forceReset && !metrics::isDiscontinuous(elapsed, expectedInterval);
    s.continuous = baseline;
    s.interval = baseline ? elapsed : kNaN;

    // The capacity batch is queued first and collected last, so a healthy
    // probe (microseconds) completes in parallel with the rest of the sample.
    prepareFilesystems(s, now);
    sampleCpu(s, baseline);
    sampleMemory(s);
    sampleLoad(s);
    sampleDisks(s, baseline, elapsed);
    sampleNetwork(s, baseline, elapsed);

    // The time-based rates are only right if the counters were read at the
    // same offset after `now` as last time (see sample() in the header). CPU
    // shares are ratios of the kernel's own tick counts and stay valid.
    const double readOffset = stopwatch() - started;
    const double skew = std::fabs(readOffset - m_prevReadOffset);
    m_prevReadOffset = readOffset;
    bool rateBaseline = baseline;
    if (baseline && !(skew <= readSkewLimit(expectedInterval))) {
        rateBaseline = false;
        for (auto& d : s.disks)
            d.readBps = d.writeBps = d.busyPercent = kNaN;
        for (auto& n : s.interfaces)
            n.rxBps = n.txBps = kNaN;
        s.diskTotal.a = s.diskTotal.b = kNaN;
        s.netTotal.a = s.netTotal.b = kNaN;
        s.problems.push_back("transfer and process rates (counter reads were "
                             + std::to_string(std::lround(std::min(skew, 1e6) * 1000.0)) + " ms off their usual timing)");
    }

    // Processes come before the sensors, whose reads can be slow on some
    // drivers, so their read offset stays as steady as the counters'.
    if (m_opt.scanProcesses) {
        std::string up;
        std::optional<double> uptime;
        if (readFile(m_opt.procRoot + "/uptime", up, nullptr, 4096))
            uptime = parse::parseUptime(up);
        sampleProcesses(s, rateBaseline, elapsed, uptime.value_or(kNaN), now);
    }
    sampleTemperatures(s, now);
    finishFilesystems(s, now, expectedInterval);

    m_prevTime = now;
    m_havePrev = true;
    m_forceReset = false;
    s.collectMs = (stopwatch() - started) * 1000.0;
    return s;
}

// ---------------------------------------------------------------------------

void Sampler::sampleCpu(Snapshot& s, bool baseline)
{
    std::string text;
    if (!readFile(m_opt.procRoot + "/stat", text)) {
        s.problems.emplace_back("/proc/stat unreadable");
        m_prevCpuAggregate.reset();
        m_prevCores.clear();
        for (int idx : m_knownCores)
            s.cores.push_back({idx, false, {}});
        return;
    }
    const auto st = parse::parseProcStat(text);
    if (st.cpus.empty() && !st.aggregate)
        s.problems.emplace_back("/proc/stat malformed");

    std::map<int, parse::CpuTimes> cur;
    for (const auto& c : st.cpus) {
        cur[c.index] = c.times;
        m_knownCores.insert(c.index);
    }
    s.logicalCpus = static_cast<int>(cur.size());

    for (int idx : m_knownCores) {
        CoreUsage cu;
        cu.index = idx;
        const auto it = cur.find(idx);
        cu.online = it != cur.end();
        if (cu.online && baseline) {
            if (const auto prev = m_prevCores.find(idx); prev != m_prevCores.end())
                cu.usage = metrics::cpuUsage(prev->second, it->second);
        }
        s.cores.push_back(cu);
    }

    if (baseline) {
        const bool sameSet = !cur.empty() && cur.size() == m_prevCores.size()
            && std::equal(cur.begin(), cur.end(), m_prevCores.begin(),
                          [](const auto& a, const auto& b) { return a.first == b.first; });
        if ((sameSet || cur.empty()) && m_prevCpuAggregate && st.aggregate) {
            s.cpu = metrics::cpuUsage(*m_prevCpuAggregate, *st.aggregate);
        } else if (!cur.empty()) {
            // CPUs were hot(un)plugged: sum the cores present in both samples.
            metrics::CpuDelta sum;
            bool ok = false;
            for (const auto& [idx, times] : cur) {
                const auto prev = m_prevCores.find(idx);
                if (prev == m_prevCores.end())
                    continue;
                const auto d = metrics::cpuDelta(prev->second, times);
                if (!d) {
                    ok = false;
                    break;
                }
                sum += *d;
                ok = true;
            }
            if (ok)
                s.cpu = metrics::usageFromDelta(sum);
        }
    }
    m_prevCores = std::move(cur);
    m_prevCpuAggregate = st.aggregate;
}

void Sampler::sampleMemory(Snapshot& s)
{
    std::string text;
    if (!readFile(m_opt.procRoot + "/meminfo", text)) {
        s.problems.emplace_back("/proc/meminfo unreadable");
        return;
    }
    s.memory = metrics::memoryFromRaw(parse::parseMeminfo(text));
    if (!s.memory.valid)
        s.problems.emplace_back("/proc/meminfo malformed");
}

void Sampler::sampleLoad(Snapshot& s)
{
    std::string text;
    if (!readFile(m_opt.procRoot + "/loadavg", text, nullptr, 4096))
        return;
    if (const auto l = parse::parseLoadavg(text)) {
        s.load1 = l->one;
        s.load5 = l->five;
        s.load15 = l->fifteen;
    }
}

// ---------------------------------------------------------------------------

void Sampler::sampleDisks(Snapshot& s, bool baseline, double elapsed)
{
    std::string text;
    if (!readFile(m_opt.procRoot + "/diskstats", text)) {
        s.problems.emplace_back("/proc/diskstats unreadable");
        m_disks.clear();
        return;
    }
    std::set<std::string> seen;
    for (const auto& line : parse::parseDiskstats(text)) {
        // Loop and RAM disks mirror other storage or memory; skipping them
        // before touching sysfs keeps hosts with many snap loops cheap.
        if (startsWith(line.name, "loop") || startsWith(line.name, "ram"))
            continue;
        std::string sysName = line.name;
        std::replace(sysName.begin(), sysName.end(), '/', '!');
        const std::string base = m_opt.sysRoot + "/block/" + sysName;
        // Only whole disks appear directly under /sys/block; partitions are
        // skipped so no byte is counted twice.
        if (!exists(base))
            continue;
        const auto sizeSectors = parse::toU64(readTrimmed(base + "/size"));
        if (sizeSectors && *sizeSectors == 0)
            continue; // no media (e.g. empty card reader or optical drive)

        std::string identity = readTrimmed(base + "/diskseq");
        if (identity.empty())
            identity = std::to_string(line.major) + ":" + std::to_string(line.minor);

        DiskIo d;
        d.name = parse::escapeName(line.name);
        d.kind = exists(base + "/device") ? DeviceKind::Physical : DeviceKind::Virtual;
        d.label = parse::sanitize(readTrimmed(base + "/dm/name"));
        if (sizeSectors && *sizeSectors <= UINT64_MAX / metrics::kDiskSectorBytes)
            d.sizeBytes = static_cast<double>(*sizeSectors * metrics::kDiskSectorBytes);

        auto& state = m_disks[line.name];
        if (state.identity != identity) {
            state = DiskState{};
            state.identity = identity;
        }
        if (baseline && state.hasPrev) {
            d.readBps = metrics::rate(metrics::counterDelta(state.sectorsRead, line.sectorsRead, m_counterBits),
                                      elapsed, metrics::kDiskSectorBytes);
            d.writeBps = metrics::rate(metrics::counterDelta(state.sectorsWritten, line.sectorsWritten, m_counterBits),
                                       elapsed, metrics::kDiskSectorBytes);
            const auto busyMs = metrics::counterDelta(state.ioTicks, line.ioTicksMs, m_counterBits);
            if (busyMs && elapsed > 0) {
                const double pct = 100.0 * static_cast<double>(*busyMs) / (elapsed * 1000.0);
                // io_ticks is sampled with jiffy granularity; tolerate slight overshoot.
                if (pct <= 110.0)
                    d.busyPercent = std::min(pct, 100.0);
            }
        }
        state.sectorsRead = line.sectorsRead;
        state.sectorsWritten = line.sectorsWritten;
        state.ioTicks = line.ioTicksMs;
        state.hasPrev = true;
        seen.insert(line.name);
        s.disks.push_back(std::move(d));
    }
    for (auto it = m_disks.begin(); it != m_disks.end();)
        it = seen.count(it->first) ? std::next(it) : m_disks.erase(it);

    // Physical whole disks never overlap each other, so their sum is honest.
    AggregateRate agg;
    agg.label = "all physical disks";
    double r = 0, w = 0;
    bool valid = true;
    for (const auto& d : s.disks) {
        if (d.kind != DeviceKind::Physical)
            continue;
        ++agg.members;
        if (isValid(d.readBps) && isValid(d.writeBps)) {
            r += d.readBps;
            w += d.writeBps;
        } else {
            valid = false;
        }
    }
    if (agg.members > 0 && valid) {
        agg.a = r;
        agg.b = w;
    }
    s.diskTotal = agg;
}

void Sampler::sampleNetwork(Snapshot& s, bool baseline, double elapsed)
{
    std::string text;
    if (!readFile(m_opt.procRoot + "/net/dev", text)) {
        s.problems.emplace_back("/proc/net/dev unreadable");
        m_nets.clear();
        return;
    }
    std::set<std::string> seen;
    for (const auto& line : parse::parseNetDev(text)) {
        const std::string base = m_opt.sysRoot + "/class/net/" + line.name;
        std::string identity = readTrimmed(base + "/ifindex");
        auto& state = m_nets[line.name];
        if (state.identity != identity || !state.hasPrev) {
            state = NetState{};
            state.identity = identity;
            const auto type = parse::toU64(readTrimmed(base + "/type"));
            if (line.name == "lo" || (type && *type == 772)) // ARPHRD_LOOPBACK
                state.kind = DeviceKind::Loopback;
            else if (exists(base + "/device"))
                state.kind = DeviceKind::Physical;
            else
                state.kind = DeviceKind::Virtual;
        }
        NetIo n;
        n.name = parse::escapeName(line.name);
        n.kind = state.kind;
        n.operState = parse::sanitize(readTrimmed(base + "/operstate"));
        if (baseline && state.hasPrev) {
            n.rxBps = metrics::rate(metrics::counterDelta(state.rx, line.rxBytes, m_counterBits), elapsed);
            n.txBps = metrics::rate(metrics::counterDelta(state.tx, line.txBytes, m_counterBits), elapsed);
        }
        state.rx = line.rxBytes;
        state.tx = line.txBytes;
        state.hasPrev = true;
        seen.insert(line.name);
        s.interfaces.push_back(std::move(n));
    }
    for (auto it = m_nets.begin(); it != m_nets.end();)
        it = seen.count(it->first) ? std::next(it) : m_nets.erase(it);

    // Virtual interfaces (bridges, veth, tunnels) re-carry traffic that also
    // crosses a physical NIC, so the default aggregate uses physical ones only.
    // Hosts without any (containers) fall back to all non-loopback interfaces.
    const bool anyPhysical = std::any_of(s.interfaces.begin(), s.interfaces.end(),
                                         [](const NetIo& n) { return n.kind == DeviceKind::Physical; });
    AggregateRate agg;
    agg.label = anyPhysical ? "all physical interfaces" : "all non-loopback interfaces";
    double rx = 0, tx = 0;
    bool valid = true;
    for (const auto& n : s.interfaces) {
        const bool member = anyPhysical ? n.kind == DeviceKind::Physical : n.kind != DeviceKind::Loopback;
        if (!member)
            continue;
        ++agg.members;
        if (isValid(n.rxBps) && isValid(n.txBps)) {
            rx += n.rxBps;
            tx += n.txBps;
        } else {
            valid = false;
        }
    }
    if (agg.members > 0 && valid) {
        agg.a = rx;
        agg.b = tx;
    }
    s.netTotal = agg;
}

void Sampler::prepareFilesystems(Snapshot& s, double now)
{
    m_fsGroups.clear();
    m_fsTargets.clear();
    std::string text;
    if (!readFile(m_opt.procRoot + "/self/mountinfo", text)) {
        s.problems.emplace_back("/proc/self/mountinfo unreadable");
        return;
    }
    const auto table = parse::parseMountInfo(text);
    // Classify by type first: network, FUSE and autofs mounts are never
    // probed. Then check the mount is what its path actually resolves to and
    // that reaching it never crosses a mount whose lookups could hang.
    struct Candidate {
        std::size_t group;
        std::size_t entry;
        std::string skipReason;
    };
    std::vector<Candidate> candidates;
    std::map<std::string, std::size_t> bySource;
    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto& m = table[i];
        if (!parse::isLocalCapacityFs(m))
            continue;
        std::string reason;
        const auto res = parse::resolveMountPath(table, m.mountPoint);
        if (res.index != static_cast<int>(i))
            reason = "hidden by another mount";
        else if (!res.safe)
            reason = "behind a network, FUSE or automount point; not probed";
        auto it = bySource.find(m.source);
        if (it == bySource.end()) {
            Filesystem fs;
            fs.source = parse::escapeName(m.source);
            fs.fsType = m.fsType;
            fs.readOnly = m.readOnly();
            it = bySource.emplace(m.source, m_fsGroups.size()).first;
            m_fsGroups.push_back(std::move(fs));
        }
        auto& mps = m_fsGroups[it->second].mountPoints;
        if (reason.empty() && std::find(mps.begin(), mps.end(), m.mountPoint) == mps.end())
            mps.push_back(m.mountPoint);
        candidates.push_back({it->second, i, std::move(reason)});
    }

    std::vector<FsProber::Job> jobs;
    for (std::size_t g = 0; g < m_fsGroups.size(); ++g) {
        auto& fs = m_fsGroups[g];
        std::sort(fs.mountPoints.begin(), fs.mountPoints.end(), [](const std::string& a, const std::string& b) {
            return a.size() != b.size() ? a.size() < b.size() : a < b;
        });
        if (fs.mountPoints.empty()) {
            // Every mount of this source was skipped; keep it listed with why.
            for (const auto& c : candidates) {
                if (c.group == g) {
                    fs.mountPoints.push_back(table[c.entry].mountPoint);
                    fs.error = c.skipReason;
                    break;
                }
            }
            continue;
        }
        FsTarget t;
        t.group = g;
        t.id.path = fs.mountPoints.front();
        for (const auto& c : candidates) {
            const auto& e = table[c.entry];
            if (c.group == g && e.mountPoint == t.id.path && c.skipReason.empty()) {
                t.id.mountId = e.id;
                t.id.major = e.major;
                t.id.minor = e.minor;
                t.id.root = e.root;
                t.id.source = e.source;
                break;
            }
        }
        jobs.push_back(t.id);
        m_fsTargets.push_back(std::move(t));
    }
    if (!jobs.empty())
        m_prober->submit(std::move(jobs), now); // no-op while a batch is outstanding
}

void Sampler::finishFilesystems(Snapshot& s, double now, double expectedInterval)
{
    if (m_opt.fsProbeWaitMs > 0)
        m_prober->waitIdle(m_opt.fsProbeWaitMs);

    std::vector<FsProber::Result> results;
    double batchTime = 0;
    if (m_prober->take(results, batchTime)) {
        for (auto& r : results) {
            // Only cache an answer (value or error) when the path still names
            // the exact mount it was requested for. A late answer for a path
            // that has since been remounted or removed is discarded.
            const auto target = std::find_if(m_fsTargets.begin(), m_fsTargets.end(),
                                             [&](const FsTarget& t) { return t.id == r.target; });
            if (target == m_fsTargets.end()) {
                m_fsCache.erase(r.target.path);
                continue;
            }
            FsCached c;
            c.stats = std::move(r.stats);
            c.err = r.err;
            c.id = std::move(r.target);
            c.time = batchTime;
            m_fsCache[c.id.path] = std::move(c);
        }
    }

    const double staleAfter = m_opt.fsStaleSeconds > 0
        ? m_opt.fsStaleSeconds
        : std::max(5.0, 3.0 * (expectedInterval > 0 ? expectedInterval : 1.0));
    const double outstanding = m_prober->outstandingSince();
    const bool stuck = outstanding > 0 && now - outstanding > staleAfter;

    std::set<std::string> live;
    for (const auto& t : m_fsTargets) {
        auto& fs = m_fsGroups[t.group];
        live.insert(t.id.path);
        const auto it = m_fsCache.find(t.id.path);
        const bool usable = it != m_fsCache.end() && it->second.id == t.id && now - it->second.time <= staleAfter
            && now >= it->second.time;
        if (!usable) {
            fs.error = stuck ? "not responding (capacity check blocked)" : "measuring";
            continue;
        }
        const auto& c = it->second;
        const auto& st = c.stats;
        if (!st) {
            fs.error = errnoText(c.err);
        } else if (st->frsize == 0 || st->blocks == 0 || st->bfree > st->blocks || st->bavail > st->bfree
                   || st->blocks > UINT64_MAX / st->frsize) {
            fs.error = "inconsistent statvfs values";
        } else {
            const uint64_t used = st->blocks - st->bfree;
            fs.totalBytes = static_cast<double>(st->blocks * st->frsize);
            fs.usedBytes = static_cast<double>(used * st->frsize);
            fs.availBytes = static_cast<double>(st->bavail * st->frsize);
        }
    }
    for (auto it = m_fsCache.begin(); it != m_fsCache.end();)
        it = live.count(it->first) ? std::next(it) : m_fsCache.erase(it);

    for (auto& fs : m_fsGroups)
        for (auto& mp : fs.mountPoints)
            mp = parse::escapeName(mp);
    s.filesystems = std::move(m_fsGroups);
    m_fsGroups.clear();
}

// ---------------------------------------------------------------------------

void Sampler::closeSensorDirs()
{
    for (int fd : m_sensorDirs)
        ::close(fd);
    m_sensorDirs.clear();
}

void Sampler::discoverSensors()
{
    // Each hwmon/thermal directory is opened once and inputs are read
    // relative to that descriptor. If the device goes away and its hwmonN
    // name is reused by another device, reads through the old descriptor fail
    // (ENOENT) instead of silently recording the new device under the old key.
    m_sensors.clear();
    closeSensorDirs();
    const std::string hwmonRoot = m_opt.sysRoot + "/class/hwmon";
    const std::string thermalRoot = m_opt.sysRoot + "/class/thermal";
    auto hwmons = listDir(hwmonRoot, &m_hwmonPresent);
    std::sort(hwmons.begin(), hwmons.end(), naturalLess);
    for (const auto& entry : hwmons) {
        const std::string base = hwmonRoot + "/" + entry;
        const int dirfd = ::open(base.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirfd < 0)
            continue;
        const int dirIndex = static_cast<int>(m_sensorDirs.size());
        m_sensorDirs.push_back(dirfd);
        std::string chip = parse::escapeName(readTrimmedAt(dirfd, "name"));
        if (chip.empty())
            chip = entry;
        const std::string devicePath = realPath(base + "/device");
        std::string identity = devicePath;
        if (identity.empty())
            identity = realPath(base);
        if (identity.empty())
            identity = base;
        const std::string device = devicePath.empty() ? entry : baseName(devicePath);

        auto files = listDirAt(dirfd);
        std::sort(files.begin(), files.end(), naturalLess);
        for (const auto& f : files) {
            if (!startsWith(f, "temp") || f.size() <= 10 || f.compare(f.size() - 6, 6, "_input") != 0)
                continue;
            const std::string num = f.substr(4, f.size() - 10);
            if (!parse::toU64(num))
                continue;
            SensorInput in;
            in.chip = chip;
            in.device = parse::escapeName(device);
            in.label = parse::sanitize(readTrimmedAt(dirfd, ("temp" + num + "_label").c_str()));
            if (in.label.empty())
                in.label = "temp" + num;
            in.dir = dirIndex;
            in.inputName = f;
            in.displayPath = base + "/" + f;
            const std::string fault = "temp" + num + "_fault";
            if (std::find(files.begin(), files.end(), fault) != files.end())
                in.faultName = fault;
            in.key = "hwmon:" + keyPart(identity) + ":" + keyPart(chip) + ":temp" + num;
            m_sensors.push_back(std::move(in));
        }
    }

    auto zones = listDir(thermalRoot, &m_thermalPresent);
    std::sort(zones.begin(), zones.end(), naturalLess);
    for (const auto& entry : zones) {
        if (!startsWith(entry, "thermal_zone"))
            continue;
        const std::string base = thermalRoot + "/" + entry;
        const int dirfd = ::open(base.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirfd < 0)
            continue;
        const auto children = listDirAt(dirfd);
        if (m_hwmonPresent && std::any_of(children.begin(), children.end(),
                                          [](const std::string& c) { return startsWith(c, "hwmon"); })) {
            ::close(dirfd); // zone registered an hwmon device, already listed above
            continue;
        }
        const int dirIndex = static_cast<int>(m_sensorDirs.size());
        m_sensorDirs.push_back(dirfd);
        SensorInput in;
        in.chip = "thermal zone";
        in.device = entry;
        in.label = parse::sanitize(readTrimmedAt(dirfd, "type"));
        if (in.label.empty())
            in.label = entry;
        in.dir = dirIndex;
        in.inputName = "temp";
        in.displayPath = base + "/temp";
        std::string identity = realPath(base);
        if (identity.empty())
            identity = base;
        in.key = "thermal:" + keyPart(identity);
        m_sensors.push_back(std::move(in));
    }

    // Only identities that still exist are remembered, so churning devices
    // cannot grow this set without bound.
    std::set<std::string> current;
    for (const auto& in : m_sensors)
        current.insert(in.key);
    for (auto it = m_everValidSensors.begin(); it != m_everValidSensors.end();)
        it = current.count(*it) ? std::next(it) : m_everValidSensors.erase(it);
}

void Sampler::sampleTemperatures(Snapshot& s, double now)
{
    if (m_rediscover || now - m_lastDiscovery >= m_opt.sensorRediscoverySeconds || now < m_lastDiscovery) {
        discoverSensors();
        m_lastDiscovery = now;
        m_rediscover = false;
    }
    std::vector<TemperatureReading> readings;
    char buf[256];
    auto gone = [](int err) { return err == ENOENT || err == ENODEV; };
    for (const auto& in : m_sensors) {
        TemperatureReading r;
        r.key = in.key;
        r.chip = in.chip;
        r.label = in.label;
        r.path = in.displayPath;
        const int dirfd = in.dir >= 0 ? m_sensorDirs[static_cast<std::size_t>(in.dir)] : -1;
        // An existing fault attribute that cannot be read or parsed leaves the
        // fault state unknown, so the reading is unavailable rather than
        // trusted; only a readable "0" clears it.
        bool trusted = true;
        if (!in.faultName.empty()) {
            int err = 0;
            const ssize_t n = readAt(dirfd, in.faultName.c_str(), buf, sizeof buf - 1, &err);
            const auto fault = n > 0 ? parse::toU64(parse::trim(std::string_view(buf, static_cast<std::size_t>(n))))
                                     : std::nullopt;
            trusted = fault && *fault == 0;
            if (n < 0 && gone(err))
                m_rediscover = true;
        }
        int err = 0;
        const ssize_t n = readAt(dirfd, in.inputName.c_str(), buf, sizeof buf - 1, &err);
        if (n > 0) {
            if (trusted)
                if (const auto c = parse::parseMilliCelsius(std::string_view(buf, static_cast<std::size_t>(n))))
                    r.celsius = *c;
        } else if (n < 0 && gone(err)) {
            m_rediscover = true; // sensor vanished (hot-unplug or driver unload)
        }
        if (isValid(r.celsius))
            m_everValidSensors.insert(r.key);
        if (m_everValidSensors.count(r.key))
            readings.push_back(std::move(r));
    }

    // Disambiguate identical chip/label pairs (e.g. two NVMe drives).
    std::map<std::string, int> counts;
    for (const auto& r : readings)
        ++counts[r.chip + "\x1f" + r.label];
    for (auto& r : readings) {
        r.displayName = r.chip + " \u00B7 " + r.label;
        if (counts[r.chip + "\x1f" + r.label] > 1) {
            const auto it = std::find_if(m_sensors.begin(), m_sensors.end(),
                                         [&](const SensorInput& in) { return in.key == r.key; });
            if (it != m_sensors.end())
                r.displayName += " (" + it->device + ")";
        }
    }

    if (readings.empty()) {
        if (!m_hwmonPresent && !m_thermalPresent)
            s.temperatureNote = "This system does not expose /sys/class/hwmon or /sys/class/thermal, "
                                "which is common inside containers.";
        else if (m_sensors.empty())
            s.temperatureNote = "No temperature sensors were found. Virtual machines usually have none; on "
                                "physical hardware the sensor driver may not be loaded (lm_sensors' "
                                "sensors-detect can identify it).";
        else
            s.temperatureNote = std::to_string(m_sensors.size())
                + " temperature inputs exist but none returned a valid reading (faulted, unsupported "
                  "or restricted).";
    }
    s.temperatures = std::move(readings);
}

// ---------------------------------------------------------------------------

std::string Sampler::userFor(uint32_t uid)
{
    auto it = m_users.find(uid);
    if (it != m_users.end())
        return it->second;
    if (m_users.size() > 4096)
        m_users.clear();
    return m_users.emplace(uid, m_opt.userName(uid)).first->second;
}

void Sampler::sampleProcesses(Snapshot& s, bool baseline, double elapsed, double uptime, double now)
{
    // A failed scan invalidates every process baseline: otherwise the next
    // successful scan would divide several intervals of CPU/IO by one.
    const int dirfd = ::open(m_opt.procRoot.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) {
        s.problems.emplace_back("/proc unreadable");
        m_procs.clear();
        return;
    }
    DIR* dir = ::fdopendir(dirfd);
    if (!dir) {
        ::close(dirfd);
        s.problems.emplace_back("/proc unreadable");
        m_procs.clear();
        return;
    }
    ++m_scanNo;
    s.processesScanned = true;
    s.processes.reserve(m_procs.size() + 32);

    char buf[4097];
    constexpr double kIoRetrySeconds = 15.0;
    const int cpus = std::max(1, s.logicalCpus);
    while (const dirent* e = ::readdir(dir)) {
        const char* name = e->d_name;
        bool numeric = name[0] != '\0';
        for (const char* c = name; *c; ++c)
            numeric = numeric && *c >= '0' && *c <= '9';
        if (!numeric)
            continue;

        // All per-process files are read through one descriptor for
        // /proc/PID. It stays bound to this process, so if the PID is reused
        // mid-scan the reads fail instead of mixing two processes' data.
        const int pidfd = ::openat(dirfd, name, O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (pidfd < 0)
            continue; // exited between readdir and open
        struct FdCloser {
            int fd;
            ~FdCloser() { ::close(fd); }
        } closer {pidfd};
        ssize_t len = readAt(pidfd, "stat", buf, sizeof buf - 1, nullptr);
        if (len <= 0)
            continue;
        const auto st = parse::parsePidStat(std::string_view(buf, static_cast<std::size_t>(len)));
        if (!st)
            continue;

        auto& ps = m_procs[st->pid];
        // PID + start time identifies a process; a reused PID starts fresh even
        // if its counters happen to be larger than the old process's.
        const bool fresh = ps.seenScan == 0 || ps.startTicks != st->startTime;
        const bool execed = !fresh && ps.comm != st->comm;
        if (fresh)
            ps = ProcState{};
        if (fresh || execed) {
            ps.startTicks = st->startTime;
            ps.comm = st->comm;
            ps.ioDenied = false;
            len = readAt(pidfd, "status", buf, sizeof buf - 1, nullptr);
            ps.uidKnown = false;
            if (len > 0) {
                if (const auto uid = parse::parseStatusRealUid(std::string_view(buf, static_cast<std::size_t>(len)))) {
                    ps.uid = *uid;
                    ps.uidKnown = true;
                }
            }
            ps.user = ps.uidKnown ? userFor(ps.uid) : std::string();
            len = readAt(pidfd, "cmdline", buf, sizeof buf - 1, nullptr);
            ps.command = len > 0 ? parse::parseCmdline(std::string_view(buf, static_cast<std::size_t>(len))) : std::string();
            if (ps.command.empty())
                ps.command = "[" + st->comm + "]";
        }
        ps.seenScan = m_scanNo;

        ProcessInfo p;
        p.pid = st->pid;
        p.ppid = st->ppid;
        p.startTicks = st->startTime;
        p.state = st->state;
        p.kernelThread = (st->flags & parse::kPfKthread) != 0;
        p.threads = static_cast<int>(std::min<int64_t>(st->numThreads, INT32_MAX));
        p.nice = static_cast<int>(std::clamp<int64_t>(st->nice, -100, 100));
        p.name = st->comm;
        p.command = ps.command;
        p.uid = ps.uid;
        p.uidKnown = ps.uidKnown;
        p.user = ps.user;
        if (static_cast<uint64_t>(st->rssPages) <= UINT64_MAX / static_cast<uint64_t>(m_pageSize))
            p.rssBytes = static_cast<double>(static_cast<uint64_t>(st->rssPages) * static_cast<uint64_t>(m_pageSize));
        if (isValid(uptime)) {
            const double age = uptime - static_cast<double>(st->startTime) / static_cast<double>(m_clockTicks);
            if (age >= -1.0)
                p.ageSeconds = std::max(0.0, age);
        }

        // utime + stime is checked: a wrapped sum would look like a small,
        // valid counter and yield an invented CPU share. An unusable sum also
        // clears the baseline so the next sample cannot delta against it.
        const bool ticksOk = st->utime <= UINT64_MAX - st->stime;
        const uint64_t ticks = ticksOk ? st->utime + st->stime : 0;
        if (baseline && ps.hasPrev && ps.hasCpu && ticksOk && !fresh)
            p.cpuPercent = metrics::processCpuPercent(ps.cpuTicks, ticks, elapsed, m_clockTicks, cpus);
        ps.cpuTicks = ticks;
        ps.hasCpu = ticksOk;

        // Access to /proc/PID/io can change (setuid, prctl dumpable), so a
        // denial is retried on a slow cadence; recovery starts a new baseline.
        if (!p.kernelThread && (!ps.ioDenied || now >= ps.ioRetryAt)) {
            int err = 0;
            len = readAt(pidfd, "io", buf, sizeof buf - 1, &err);
            if (len < 0 && (err == EACCES || err == EPERM)) {
                ps.ioDenied = true;
                ps.ioRetryAt = now + kIoRetrySeconds;
                ps.hasIo = false;
            } else if (len > 0) {
                ps.ioDenied = false;
                if (const auto io = parse::parsePidIo(std::string_view(buf, static_cast<std::size_t>(len)))) {
                    if (baseline && ps.hasPrev && ps.hasIo && !fresh) {
                        p.ioReadBps = metrics::rate(metrics::counterDelta(ps.ioRead, io->readBytes), elapsed);
                        p.ioWriteBps = metrics::rate(metrics::counterDelta(ps.ioWrite, io->writeBytes), elapsed);
                    }
                    ps.ioRead = io->readBytes;
                    ps.ioWrite = io->writeBytes;
                    ps.hasIo = true;
                } else {
                    ps.hasIo = false;
                }
            } else {
                ps.hasIo = false;
            }
        }
        p.ioDenied = ps.ioDenied;
        ps.hasPrev = true;

        s.threadCount += p.threads;
        s.processes.push_back(std::move(p));
    }
    ::closedir(dir);

    for (auto it = m_procs.begin(); it != m_procs.end();)
        it = it->second.seenScan == m_scanNo ? std::next(it) : m_procs.erase(it);
}

} // namespace ps
