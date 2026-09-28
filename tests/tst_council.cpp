// Independent regressions for the accepted council findings (SYS-001,
// ARCH-001, ARCH-002, ARCH-003). Everything runs on injected /proc and /sys
// trees in temporary directories; thread-start failure is provoked only in a
// child process of this test that lowers its own soft RLIMIT_NPROC.
#include "app/collector.h"
#include "app/windowowners.h"
#include "core/sampler.h"
#include "app/mainwindow.h"
#include "app/timechart.h"
#include "core/history.h"
#include "core/snapshot.h"

#include <QAccessible>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace std::string_literals;

namespace {

namespace fs = std::filesystem;

class FixtureRoot
{
public:
    std::string path(std::string_view rel) const { return m_dir.path().toStdString() + "/" + std::string(rel); }
    std::string proc() const { return path("proc"); }
    std::string sys() const { return path("sys"); }
    void write(std::string_view rel, std::string_view content) const
    {
        const fs::path p = path(rel);
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(content.data(), std::streamsize(content.size()));
    }
    void mkdir(std::string_view rel) const { fs::create_directories(path(rel)); }
    void remove(std::string_view rel) const { fs::remove_all(path(rel)); }

private:
    QTemporaryDir m_dir;
};

std::string u(uint64_t v) { return std::to_string(v); }

std::string cpuLine(std::string_view name, uint64_t user, uint64_t system, uint64_t idle)
{
    return std::string(name) + " " + u(user) + " 0 " + u(system) + " " + u(idle) + " 0 0 0 0 0 0\n";
}

// /proc/diskstats: major minor name, then 11 statistics.
std::string diskLine(unsigned major, unsigned minor, std::string_view name, uint64_t sectorsRead,
                     uint64_t sectorsWritten, uint64_t ioTicks)
{
    return u(major) + " " + u(minor) + " " + std::string(name) + " 1 0 " + u(sectorsRead) + " 0 1 0 "
        + u(sectorsWritten) + " 0 0 " + u(ioTicks) + " 0\n";
}

std::string pidStat(int pid, std::string_view comm, uint64_t utime, uint64_t start)
{
    return u(uint64_t(pid)) + " (" + std::string(comm) + ") S 1 " + u(uint64_t(pid)) + " " + u(uint64_t(pid))
        + " 0 -1 4194560 0 0 0 0 " + u(utime) + " 0 0 0 20 0 1 0 " + u(start)
        + " 1000000 456 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n";
}

std::string statusText(uint32_t uid)
{
    return "Name:\tx\nUmask:\t0022\nUid:\t" + u(uid) + "\t" + u(uid) + "\t" + u(uid) + "\t" + u(uid)
        + "\nGid:\t0\t0\t0\t0\n";
}

std::string ioText(uint64_t readBytes, uint64_t writeBytes)
{
    return "rchar: 1\nwchar: 2\nsyscr: 3\nsyscw: 4\nread_bytes: " + u(readBytes) + "\nwrite_bytes: " + u(writeBytes)
        + "\ncancelled_write_bytes: 0\n";
}

void writeProc(const FixtureRoot& r, int pid, const std::string& stat, const std::string& cmdline, uint32_t uid,
               const std::string& io = {})
{
    const std::string base = "proc/" + std::to_string(pid) + "/";
    r.write(base + "stat", stat);
    r.write(base + "status", statusText(uid));
    r.write(base + "cmdline", cmdline);
    if (!io.empty())
        r.write(base + "io", io);
}

void writeBaseTree(const FixtureRoot& r)
{
    r.write("proc/stat", cpuLine("cpu", 0, 0, 0) + cpuLine("cpu0", 0, 0, 0));
    r.write("proc/meminfo", "MemTotal: 1000 kB\nMemFree: 100 kB\nMemAvailable: 500 kB\n");
    r.write("proc/loadavg", "0.10 0.20 0.30 1/10 99\n");
    r.write("proc/uptime", "1000.00 0.00\n");
    r.write("proc/diskstats", "");
    r.write("proc/net/dev", "Inter-|   Receive  |  Transmit\n face |bytes    packets|bytes    packets\n");
    r.write("proc/mounts", "");
    r.write("proc/self/mountinfo", "");
    r.mkdir("sys/block");
    r.mkdir("sys/class/net");
}

void addPhysicalDisk(const FixtureRoot& r, std::string_view name)
{
    const std::string base = "sys/block/" + std::string(name) + "/";
    r.write(base + "size", "1000\n");
    r.mkdir(base + "device");
}

ps::SamplerOptions fixtureOptions(const FixtureRoot& r, bool scanProcesses = false)
{
    ps::SamplerOptions o;
    o.procRoot = r.proc();
    o.sysRoot = r.sys();
    o.clockTicks = 100;
    o.pageSize = 4096;
    o.counterBits = 64;
    o.scanProcesses = scanProcesses;
    o.statFs = [](const std::string&, int& err) -> std::optional<ps::FsStats> {
        err = ENOENT;
        return std::nullopt;
    };
    o.userName = [](uint32_t uid) { return "u" + std::to_string(uid); };
    return o;
}

std::string mi(int id, int parent, std::string_view dev, std::string_view root, std::string_view mp,
               std::string_view type, std::string_view source)
{
    return std::to_string(id) + " " + std::to_string(parent) + " " + std::string(dev) + " " + std::string(root) + " "
        + std::string(mp) + " rw - " + std::string(type) + " " + std::string(source) + " rw\n";
}

const ps::DiskIo* findDisk(const ps::Snapshot& s, std::string_view name)
{
    for (const auto& d : s.disks)
        if (d.name == name)
            return &d;
    return nullptr;
}

const ps::ProcessInfo* findProc(const ps::Snapshot& s, int pid)
{
    for (const auto& p : s.processes)
        if (p.pid == pid)
            return &p;
    return nullptr;
}

const ps::Filesystem* findFs(const ps::Snapshot& s, std::string_view source)
{
    for (const auto& f : s.filesystems)
        if (f.source == source)
            return &f;
    return nullptr;
}

bool hasProblem(const ps::Snapshot& s, std::string_view text)
{
    return std::any_of(s.problems.begin(), s.problems.end(),
                       [&](const std::string& p) { return p.find(text) != std::string::npos; });
}

QString problems(const ps::Snapshot& s)
{
    QStringList out;
    for (const auto& p : s.problems)
        out << QString::fromStdString(p);
    return out.join(QStringLiteral("; "));
}

QString num(double v) { return std::isnan(v) ? QStringLiteral("NaN") : QString::number(v, 'g', 12); }

// The whole-file read cap of the sampler's table sources (sampler.cpp readFile).
constexpr std::size_t kReadCap = std::size_t(8) << 20;

// `total` bytes of diskstats: loop-device rows (the sampler skips them before
// sysfs) and then `tail`, which ends exactly at `total`.
std::string paddedDiskstats(std::size_t total, const std::string& tail)
{
    const std::string head = "7 0 loop";
    const std::string rest = " 0 0 0 0 0 0 0 0 0 0 0\n";
    const std::size_t fillTo = total - tail.size();
    std::string out;
    out.reserve(total);
    const auto row = [&](std::size_t length) { out += head + std::string(length - head.size() - rest.size(), 'x') + rest; };
    while (fillTo - out.size() >= 128)
        row(64);
    row(fillTo - out.size());
    out += tail;
    return out;
}

// ---------------------------------------------------------------------------
// Child processes for thread-start failure. Each lowers only its own soft
// RLIMIT_NPROC, restores it before continuing, and prints FAIL:/INFO: lines.

int g_childFailures = 0;
void childFail(const std::string& m)
{
    std::printf("FAIL: %s\n", m.c_str());
    std::fflush(stdout);
    ++g_childFailures;
}
void childInfo(const std::string& m)
{
    std::printf("INFO: %s\n", m.c_str());
    std::fflush(stdout);
}

constexpr int kChildSkip = 77;

class ThreadLimit
{
public:
    // Lowers the soft limit so that no new thread can start, and checks it.
    bool lower()
    {
        if (::getrlimit(RLIMIT_NPROC, &m_saved) != 0)
            return false;
        rlimit l = m_saved;
        l.rlim_cur = 1;
        if (::setrlimit(RLIMIT_NPROC, &l) != 0)
            return false;
        m_lowered = true;
        try {
            std::thread t([] {});
            t.join();
            return false; // exempt (root or CAP_SYS_RESOURCE)
        } catch (const std::system_error&) {
            return true;
        }
    }
    void restore()
    {
        if (m_lowered)
            ::setrlimit(RLIMIT_NPROC, &m_saved);
        m_lowered = false;
    }
    ~ThreadLimit() { restore(); }

private:
    rlimit m_saved {};
    bool m_lowered = false;
};

void settle()
{
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

// Runs the event loop for up to ms, or until done() holds; returns the
// elapsed milliseconds.
qint64 pump(int ms, const std::function<bool()>& done = {})
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms && !(done && done()))
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return t.elapsed();
}

// Threads of this process whose name (15 characters at most) starts with
// prefix. QThread names its thread after its objectName.
int threadsNamed(const std::string& prefix)
{
    int n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/task", ec)) {
        std::ifstream in(e.path() / "comm");
        std::string comm;
        std::getline(in, comm);
        if (comm.rfind(prefix, 0) == 0)
            ++n;
    }
    return n;
}

void checkReason(const QString& reason, const std::string& what)
{
    if (reason.isEmpty())
        childFail(what + ": no startup reason although the system refused the thread");
    else if (!reason.contains(QLatin1String("thread"), Qt::CaseInsensitive))
        childFail(what + ": the startup reason does not say a thread could not be created: \""
                  + reason.toStdString() + "\"");
}

void checkIncreasing(const std::vector<uint64_t>& seqs, const std::string& what)
{
    for (size_t i = 1; i < seqs.size(); ++i)
        if (seqs[i] <= seqs[i - 1]) {
            childFail(what + ": sequence " + std::to_string(seqs[i]) + " after " + std::to_string(seqs[i - 1])
                      + " (a second worker or a duplicate delivery)");
            return;
        }
}

int childFsProber()
{
    FixtureRoot r;
    writeBaseTree(r);
    r.write("proc/self/mountinfo", mi(1, 0, "8:2", "/", "/", "ext4", "/dev/sda2"));
    auto o = fixtureOptions(r);
    o.fsProbeWaitMs = 1000;
    o.statFs = [](const std::string&, int&) -> std::optional<ps::FsStats> { return ps::FsStats {1000, 400, 300, 4096}; };
    ps::Sampler smp(o);
    ThreadLimit limit;
    if (!limit.lower()) {
        childInfo("RLIMIT_NPROC does not stop thread creation here");
        return kChildSkip;
    }
    try {
        const auto s = smp.sample(1, 1);
        const auto* f = findFs(s, "/dev/sda2");
        if (!f) {
            childFail("the root filesystem disappeared while its probe could not start");
        } else {
            childInfo("while limited: capacity " + num(f->totalBytes).toStdString() + ", error \"" + f->error + "\"");
            if (std::isfinite(f->totalBytes))
                childFail("capacity shown although no probe thread could start");
            if (f->error.empty())
                childFail("no explicit capacity error while the probe thread cannot start");
        }
    } catch (const std::exception& e) {
        childFail("sample() let the thread-start exception escape: "s + e.what());
    }
    limit.restore();
    bool recovered = false;
    for (int i = 2; i <= 5 && !recovered; ++i) {
        try {
            const auto s = smp.sample(i, 1);
            const auto* f = findFs(s, "/dev/sda2");
            recovered = f && f->totalBytes == 4096000.0 && f->error.empty();
            if (!recovered)
                childInfo("after restoring, sample " + std::to_string(i) + ": "
                          + (f ? "capacity " + num(f->totalBytes).toStdString() + ", error \"" + f->error + "\""
                               : "no filesystem"s));
        } catch (const std::exception& e) {
            childFail("sample() threw after the limit was restored: "s + e.what());
            break;
        }
    }
    if (!recovered)
        childFail("capacity never recovered after the thread limit was restored");
    return g_childFailures;
}

int childCollector()
{
    FixtureRoot r;
    writeBaseTree(r);
    auto sentinel = std::make_shared<int>(1);
    const std::weak_ptr<int> watch = sentinel;
    {
        auto o = fixtureOptions(r);
        o.userName = [sentinel](uint32_t uid) { return "u" + std::to_string(uid); };
        sentinel.reset();
        ThreadLimit limit;
        if (!limit.lower()) {
            childInfo("RLIMIT_NPROC does not stop thread creation here");
            return kChildSkip;
        }
        try {
            auto c = std::make_unique<Collector>(std::move(o));
            checkReason(c->startError(), "collector");
            int snapshots = 0;
            QObject::connect(c.get(), &Collector::snapshotReady, [&] { ++snapshots; });
            c->start(250);
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 400)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            childInfo("while limited: isRunning " + std::to_string(c->isRunning()) + ", snapshots "
                      + std::to_string(snapshots));
            if (c->isRunning())
                childFail("the collector claims to run although its thread could not start");
            checkReason(c->startError(), "collector after start()");
            t.restart();
            const bool stopped = c->stop(500);
            childInfo("stop() " + std::to_string(stopped) + " after " + std::to_string(t.elapsed()) + " ms");
            if (t.elapsed() > 1500)
                childFail("stop() of an unstarted collector took " + std::to_string(t.elapsed()) + " ms");
            // A stopped collector must not retry once threads can start again.
            limit.restore();
            pump(1500);
            if (threadsNamed("pulsescope-coll") != 0 || snapshots != 0)
                childFail("a stopped collector started its thread or delivered a snapshot after the limit was lifted");
            c.reset();
        } catch (const std::exception& e) {
            childFail("the collector let the thread-start failure escape: "s + e.what());
        }
        limit.restore();
    }
    settle();
    if (!watch.expired())
        childFail("the unstarted collector worker and its sampler options were never freed");
    // After the limit is restored a collector works again.
    auto c = std::make_unique<Collector>(fixtureOptions(r));
    int snapshots = 0;
    QObject::connect(c.get(), &Collector::snapshotReady, [&] { ++snapshots; });
    c->start(250);
    QElapsedTimer t;
    t.start();
    while (snapshots == 0 && t.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (snapshots == 0)
        childFail("a collector created after restoring the limit produced no snapshot within 3 s");
    c->stop();
    return g_childFailures;
}

int childWindowProbe()
{
    auto sentinel = std::make_shared<int>(1);
    const std::weak_ptr<int> watch = sentinel;
    const auto options = [](std::function<windows::Query(const windows::QueryContext&)> query) {
        windows::ProbeOptions o;
        windows::Environment e;
        e.hyprlandSignature = "sig";
        e.runtimeDir = "/nonexistent-run";
        e.waylandDisplay = "wayland-1";
        e.sessionType = "wayland";
        o.environment = e;
        o.clockTicks = 100;
        o.visibleIntervalMs = 100;
        o.backgroundIntervalMs = 100;
        o.query = std::move(query);
        return o;
    };
    {
        auto o = options([sentinel](const windows::QueryContext&) {
            windows::Query q;
            q.status = windows::Status::Ok;
            return q;
        });
        sentinel.reset();
        ThreadLimit limit;
        if (!limit.lower()) {
            childInfo("RLIMIT_NPROC does not stop thread creation here");
            return kChildSkip;
        }
        try {
            auto p = std::make_unique<windows::WindowProbe>(std::move(o));
            checkReason(p->startError(), "window probe");
            int results = 0;
            int failedWithReason = 0;
            QObject::connect(p.get(), &windows::WindowProbe::resultReady, [&](const windows::ResultPtr& res) {
                ++results;
                if (res && res->status == windows::Status::Failed && !res->detail.isEmpty()
                    && res->detail == p->startError())
                    ++failedWithReason;
            });
            p->start(true);
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 400)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            childInfo("while limited: isRunning " + std::to_string(p->isRunning()) + ", results "
                      + std::to_string(results) + " (" + std::to_string(failedWithReason) + " Failed with the reason)");
            if (p->isRunning())
                childFail("the window probe claims to run although its thread could not start");
            checkReason(p->startError(), "window probe after start()");
            if (failedWithReason == 0 || failedWithReason != results)
                childFail("a started, unpaused window probe did not report its start failure as a Failed result "
                          "carrying the reason (" + std::to_string(failedWithReason) + " of "
                          + std::to_string(results) + ")");
            t.restart();
            p->stop(500);
            if (t.elapsed() > 1500)
                childFail("stop() of an unstarted window probe took " + std::to_string(t.elapsed()) + " ms");
            const int before = results;
            limit.restore();
            pump(1500);
            if (threadsNamed("pulsescope-wind") != 0 || results != before)
                childFail("a stopped window probe started its thread or delivered a result after the limit was lifted");
            p.reset();
        } catch (const std::exception& e) {
            childFail("the window probe let the thread-start failure escape: "s + e.what());
        }
        limit.restore();
    }
    settle();
    if (!watch.expired())
        childFail("the unstarted window-probe worker and its query function were never freed");
    auto p = std::make_unique<windows::WindowProbe>(options([](const windows::QueryContext&) {
        windows::Query q;
        q.status = windows::Status::Ok;
        return q;
    }));
    int results = 0;
    QObject::connect(p.get(), &windows::WindowProbe::resultReady, [&] { ++results; });
    p->start(true);
    QElapsedTimer t;
    t.start();
    while (results == 0 && t.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (results == 0)
        childFail("a window probe created after restoring the limit produced no result within 3 s");
    p->stop();
    return g_childFailures;
}

windows::ProbeOptions okProbeOptions()
{
    windows::ProbeOptions o;
    windows::Environment e;
    e.hyprlandSignature = "sig";
    e.runtimeDir = "/nonexistent-run";
    e.waylandDisplay = "wayland-1";
    e.sessionType = "wayland";
    o.environment = e;
    o.clockTicks = 100;
    o.visibleIntervalMs = 100;
    o.backgroundIntervalMs = 100;
    o.query = [](const windows::QueryContext&) {
        windows::Query q;
        q.status = windows::Status::Ok;
        return q;
    };
    return o;
}

// Startup retries: none while paused or stopped, at once on resume, by
// back-off otherwise; the reason clears on success, and exactly one worker
// then runs.
int childCollectorRetry()
{
    FixtureRoot r;
    writeBaseTree(r);
    {
        ThreadLimit limit;
        if (!limit.lower()) {
            childInfo("RLIMIT_NPROC does not stop thread creation here");
            return kChildSkip;
        }
        Collector c(fixtureOptions(r));
        int snapshots = 0;
        int errorChanges = 0;
        std::vector<uint64_t> seqs;
        QObject::connect(&c, &Collector::snapshotReady, [&](const ps::SnapshotPtr& s) {
            ++snapshots;
            if (s)
                seqs.push_back(s->sequence);
        });
        QObject::connect(&c, &Collector::startErrorChanged, [&] { ++errorChanges; });
        checkReason(c.startError(), "collector");
        c.start(250);
        pump(300);
        // One resume while unstartable fails again and backs off to 2 s; a
        // retry timer left running while paused would fire within the wait.
        c.setPaused(true);
        c.setPaused(false);
        c.setPaused(true);
        pump(100);
        limit.restore();
        pump(2500);
        if (c.isRunning() || threadsNamed("pulsescope-coll") != 0)
            childFail("a paused collector retried its thread start");
        if (snapshots != 0)
            childFail("a paused collector delivered " + std::to_string(snapshots) + " snapshots");
        checkReason(c.startError(), "paused collector");
        const int changesBefore = errorChanges;
        c.setPaused(false);
        const qint64 resumedAt = pump(3000, [&] { return c.isRunning(); });
        childInfo("running " + std::to_string(resumedAt) + " ms after resume");
        if (!c.isRunning())
            childFail("resuming did not start the collector thread");
        else if (resumedAt > 700)
            childFail("resuming did not retry at once: running only after " + std::to_string(resumedAt) + " ms");
        if (!c.startError().isEmpty())
            childFail("the startup reason stayed after the thread started: \"" + c.startError().toStdString() + "\"");
        if (errorChanges == changesBefore)
            childFail("startErrorChanged() was not emitted when the reason cleared");
        const qint64 firstAt = pump(3000, [&] { return snapshots > 0; });
        if (snapshots == 0)
            childFail("no snapshot within 3 s of resuming");
        else
            childInfo("first snapshot " + std::to_string(firstAt) + " ms after the thread started");
        snapshots = 0;
        seqs.clear();
        pump(1500);
        childInfo("1.5 s at 250 ms: " + std::to_string(snapshots) + " snapshots");
        if (snapshots > 9)
            childFail(std::to_string(snapshots) + " snapshots in 1.5 s at 250 ms: more than one worker is collecting");
        if (snapshots < 3)
            childFail("only " + std::to_string(snapshots) + " snapshots in 1.5 s at 250 ms");
        checkIncreasing(seqs, "collector");
        const int threads = threadsNamed("pulsescope-coll");
        if (threads == 0)
            childInfo("thread names unavailable; worker count not checked");
        else if (threads != 1)
            childFail(std::to_string(threads) + " collector threads after recovery");
        c.stop();
    }
    {
        ThreadLimit limit;
        if (!limit.lower())
            return kChildSkip;
        Collector c(fixtureOptions(r));
        int snapshots = 0;
        QObject::connect(&c, &Collector::snapshotReady, [&] { ++snapshots; });
        c.start(250);
        pump(300);
        limit.restore();
        const qint64 at = pump(4000, [&] { return c.isRunning(); });
        childInfo("automatic retry started the collector " + std::to_string(at + 300) + " ms after start()");
        if (!c.isRunning())
            childFail("a started collector did not retry by itself within 4.3 s after threads became available");
        else if (!c.startError().isEmpty())
            childFail("the startup reason stayed after the automatic retry succeeded");
        pump(3000, [&] { return snapshots > 0; });
        if (snapshots == 0)
            childFail("no snapshot after the automatic retry");
        c.stop();
    }
    return g_childFailures;
}

int childWindowProbeRetry()
{
    {
        ThreadLimit limit;
        if (!limit.lower()) {
            childInfo("RLIMIT_NPROC does not stop thread creation here");
            return kChildSkip;
        }
        windows::WindowProbe p(okProbeOptions());
        std::vector<windows::ResultPtr> results;
        QObject::connect(&p, &windows::WindowProbe::resultReady,
                         [&](const windows::ResultPtr& res) { results.push_back(res); });
        p.start(true);
        pump(300);
        if (results.empty())
            childFail("no Failed result while the probe thread cannot start");
        for (const auto& res : results)
            if (!res || res->status != windows::Status::Failed || res->detail.isEmpty())
                childFail("a result other than Failed-with-reason while the thread cannot start");
        p.setPaused(true); // as for the collector: the back-off is then 2 s
        p.setPaused(false);
        p.setPaused(true);
        pump(100);
        const size_t pausedAt = results.size();
        limit.restore();
        pump(2500);
        if (p.isRunning() || threadsNamed("pulsescope-wind") != 0)
            childFail("a paused window probe retried its thread start");
        if (results.size() != pausedAt)
            childFail("a paused window probe delivered " + std::to_string(results.size() - pausedAt) + " results");
        checkReason(p.startError(), "paused window probe");
        p.setPaused(false);
        const size_t resumedIndex = results.size();
        const qint64 resumedAt = pump(3000, [&] { return p.isRunning(); });
        childInfo("running " + std::to_string(resumedAt) + " ms after resume");
        if (!p.isRunning())
            childFail("resuming did not start the window-probe thread");
        else if (resumedAt > 700)
            childFail("resuming did not retry at once: running only after " + std::to_string(resumedAt) + " ms");
        if (!p.startError().isEmpty())
            childFail("the startup reason stayed after the thread started: \"" + p.startError().toStdString() + "\"");
        pump(3000, [&] {
            for (size_t i = resumedIndex; i < results.size(); ++i)
                if (results[i] && results[i]->ok())
                    return true;
            return false;
        });
        size_t firstOk = results.size();
        for (size_t i = resumedIndex; i < results.size(); ++i) {
            if (!results[i])
                continue;
            if (results[i]->status == windows::Status::Failed && results[i]->detail.contains(QLatin1String("thread")))
                childFail("a stale start-failure result was delivered after the thread started");
            if (results[i]->ok() && firstOk == results.size())
                firstOk = i;
        }
        if (firstOk == results.size())
            childFail("no Ok result within 3 s of resuming");
        const size_t from = results.size();
        pump(1500);
        std::vector<uint64_t> seqs;
        for (size_t i = from; i < results.size(); ++i)
            if (results[i])
                seqs.push_back(results[i]->sequence);
        childInfo("1.5 s at 100 ms: " + std::to_string(seqs.size()) + " results");
        if (seqs.size() > 20)
            childFail(std::to_string(seqs.size()) + " results in 1.5 s at 100 ms: more than one worker is asking");
        if (seqs.size() < 3)
            childFail("only " + std::to_string(seqs.size()) + " results in 1.5 s at 100 ms");
        checkIncreasing(seqs, "window probe");
        const int threads = threadsNamed("pulsescope-wind");
        if (threads == 0)
            childInfo("thread names unavailable; worker count not checked");
        else if (threads != 1)
            childFail(std::to_string(threads) + " window-probe threads after recovery");
        p.stop();
    }
    {
        ThreadLimit limit;
        if (!limit.lower())
            return kChildSkip;
        windows::WindowProbe p(okProbeOptions());
        int failed = 0;
        int ok = 0;
        QObject::connect(&p, &windows::WindowProbe::resultReady, [&](const windows::ResultPtr& res) {
            if (res && res->status == windows::Status::Failed)
                ++failed;
            if (res && res->ok())
                ++ok;
        });
        p.start(true);
        pump(3200); // expect failures at 0 s, 1 s and 3 s
        childInfo("3.2 s limited: " + std::to_string(failed) + " start-failure results");
        if (failed < 2)
            childFail("no retry within 3.2 s while started (" + std::to_string(failed) + " failure results)");
        if (failed > 5)
            childFail(std::to_string(failed) + " start-failure results in 3.2 s: retries are not backing off");
        limit.restore();
        const qint64 at = pump(6000, [&] { return p.isRunning(); });
        childInfo("automatic retry started the probe " + std::to_string(at) + " ms after the limit was lifted");
        if (!p.isRunning())
            childFail("a started window probe did not retry by itself within 6 s after threads became available");
        pump(3000, [&] { return ok > 0; });
        if (ok == 0)
            childFail("no Ok result after the automatic retry");
        p.stop();
    }
    return g_childFailures;
}

} // namespace

class TestCouncil : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void readCapIsNotACompleteTable_data();
    void readCapIsNotACompleteTable();
    void processOnlyDelayDoesNotInventRates();
    void sameIdentityMetadataRefreshes_data();
    void sameIdentityMetadataRefreshes();
    void initialMetadataFailureRecovers();
    void compactChartAccessibleText();
    void compactChartAnnouncesAtDataCadenceOnly();
    void detailsCoreChartsStayHonest();
    void threadStartFailure_data();
    void threadStartFailure();
};

// ---------------------------------------------------------------------------
// ARCH-003: a table that reaches the read cap is not a complete table.

void TestCouncil::readCapIsNotACompleteTable_data()
{
    QTest::addColumn<qulonglong>("total");
    QTest::addColumn<int>("cutAt"); // >0: the sda row is cut after this many bytes by the cap
    QTest::addColumn<bool>("complete");
    const std::string sda = diskLine(8, 0, "sda", 123456, 654321, 777);
    const qulonglong cap = kReadCap;
    QTest::newRow("below cap") << cap - 1 << 0 << true;
    QTest::newRow("exactly the cap, then EOF") << cap << 0 << true;
    QTest::newRow("one byte over (a trailing newline)") << cap + 1 << 0 << false;
    QTest::newRow("whole sda row after the cap") << cap + qulonglong(sda.size()) << 0 << false;
    QTest::newRow("cap cuts sda's sector count") << cap - 14 + qulonglong(sda.size()) << 14 << false;
}

void TestCouncil::readCapIsNotACompleteTable()
{
    QFETCH(qulonglong, total);
    QFETCH(int, cutAt);
    QFETCH(bool, complete);
    const std::string sda = diskLine(8, 0, "sda", 123456, 654321, 777);
    std::string text;
    const QString tag = QString::fromLatin1(QTest::currentDataTag());
    if (tag.startsWith(QLatin1String("one byte over"))) {
        text = paddedDiskstats(kReadCap, sda) + "\n";
    } else if (tag.startsWith(QLatin1String("whole sda row"))) {
        text = paddedDiskstats(kReadCap, "") + sda;
    } else if (cutAt > 0) {
        // The first cutAt bytes of the row ("8 0 sda 1 0 12") fit under the cap.
        text = paddedDiskstats(kReadCap - std::size_t(cutAt), "") + sda;
        QCOMPARE(text.substr(kReadCap - std::size_t(cutAt), std::size_t(cutAt)), "8 0 sda 1 0 12"s);
    } else {
        text = paddedDiskstats(std::size_t(total), sda);
    }
    QCOMPARE(qulonglong(text.size()), total);
    FixtureRoot r;
    writeBaseTree(r);
    addPhysicalDisk(r, "sda");
    r.write("proc/diskstats", text);
    auto o = fixtureOptions(r);
    // Parsing 8 MiB takes real, varying time; a fixed read offset keeps the
    // counter-skew guard out of this test.
    o.stopwatch = [] { return 0.0; };
    ps::Sampler smp(o);
    for (int i = 1; i <= 2; ++i) {
        const auto s = smp.sample(i, 1);
        const auto* d = findDisk(s, "sda");
        if (complete) {
            QVERIFY2(d, qPrintable(problems(s)));
            QVERIFY2(!hasProblem(s, "diskstats"), qPrintable(problems(s)));
            QCOMPARE(s.diskTotal.members, 1);
            if (i == 2) {
                QCOMPARE(d->readBps, 0.0);
                QCOMPARE(s.diskTotal.a, 0.0);
            }
        } else {
            QVERIFY2(hasProblem(s, "diskstats"),
                     qPrintable(QStringLiteral("sample %1: a %2-byte diskstats (cap %3) read as complete; sda %4, "
                                               "all-disk members %5, problems [%6]")
                                    .arg(i)
                                    .arg(total)
                                    .arg(qulonglong(kReadCap))
                                    .arg(d ? QStringLiteral("present") : QStringLiteral("silently absent"))
                                    .arg(s.diskTotal.members)
                                    .arg(problems(s))));
            QVERIFY2(!d, "a device parsed from a capped prefix");
            QVERIFY(!std::isfinite(s.diskTotal.a) && !std::isfinite(s.diskTotal.b));
        }
    }
    if (complete)
        return;
    // The source becomes healthy again: a new baseline, then exact rates.
    r.write("proc/diskstats", diskLine(8, 0, "sda", 1000, 0, 0));
    auto s = smp.sample(3, 1);
    QVERIFY(findDisk(s, "sda"));
    QVERIFY(!hasProblem(s, "diskstats"));
    QVERIFY2(!std::isfinite(findDisk(s, "sda")->readBps), qPrintable(num(findDisk(s, "sda")->readBps)));
    r.write("proc/diskstats", diskLine(8, 0, "sda", 1200, 0, 0));
    s = smp.sample(4, 1);
    QCOMPARE(findDisk(s, "sda")->readBps, 102400.0); // 200 sectors x 512 B in 1 s
    QCOMPARE(s.diskTotal.a, 102400.0);
}

// ---------------------------------------------------------------------------
// ARCH-001: a delay inside the process scan only (after the counter-skew
// check) must not turn a steady 50 % / 1000 B/s process into 65 % / 1300 B/s,
// or into a dip on the next sample.
//
// Timeline (seconds): each sample k is requested at now = k and its counters
// are read 10 ms later. In sample 2 a user-name lookup for a newly seen
// process stalls 300 ms. The target process burns 0.5 CPU and reads 1000 B/s
// throughout, so at acquisition time a its counters are
//   ticks(a) = 100 + 50 (a - 1.01), read_bytes(a) = 1000 + 1000 (a - 1.01).
// The stall rewrites the target's files to their t = 2.31 values; if the scan
// reached the target first it read the t = 2.01 values instead. Either way an
// honest rate is 50 % / 1000 B/s or unavailable.

void TestCouncil::processOnlyDelayDoesNotInventRates()
{
    FixtureRoot r;
    writeBaseTree(r);
    addPhysicalDisk(r, "sda");
    const auto ticksAt = [](double a) { return uint64_t(std::llround(100 + 50 * (a - 1.01))); };
    const auto bytesAt = [](double a) { return uint64_t(std::llround(1000 + 1000 * (a - 1.01))); };
    const auto writeTarget = [&](double a) {
        writeProc(r, 300, pidStat(300, "worker", ticksAt(a), 7000), "worker\0"s, 1000, ioText(bytesAt(a), 0));
    };
    const auto writeDisk = [&](int k) { r.write("proc/diskstats", diskLine(8, 0, "sda", 1000 + 200 * uint64_t(k), 0, 0)); };
    // Directories for the processes that appear in sample 2 are made on both
    // sides of the target's, so one of them precedes it in any readdir order.
    r.mkdir("proc/290");
    writeTarget(1.01);
    r.mkdir("proc/310");
    auto clock = std::make_shared<double>(0);
    auto stalled = std::make_shared<bool>(false);
    auto o = fixtureOptions(r, true);
    o.stopwatch = [clock] { return *clock; };
    o.userName = [&, clock, stalled](uint32_t uid) {
        if (uid == 4321 && !*stalled) {
            *stalled = true;
            *clock += 0.3;
            writeTarget(2.31);
        }
        return "u" + std::to_string(uid);
    };
    ps::Sampler smp(o);
    const auto sampleAt = [&](int k) {
        *clock = k + 0.01;
        writeDisk(k);
        return smp.sample(k, 1);
    };
    const auto honest = [](double v, double expected) {
        return !std::isfinite(v) || std::fabs(v - expected) <= expected * 0.01;
    };

    sampleAt(1);
    writeTarget(2.01);
    for (int pid : {290, 310})
        writeProc(r, pid, pidStat(pid, "late", 0, 9000), "late\0"s, 4321, ioText(0, 0));
    const auto s2 = sampleAt(2);
    QVERIFY(*stalled);
    const auto* p = findProc(s2, 300);
    QVERIFY(p);
    qInfo("stalled sample: cpu %s %%, read %s B/s, collect %s ms, problems [%s]", qPrintable(num(p->cpuPercent)),
          qPrintable(num(p->ioReadBps)), qPrintable(num(s2.collectMs)), qPrintable(problems(s2)));
    QVERIFY2(honest(p->cpuPercent, 50.0),
             qPrintable(QStringLiteral("CPU %1 %, expected 50 or unavailable").arg(num(p->cpuPercent))));
    QVERIFY2(honest(p->ioReadBps, 1000.0),
             qPrintable(QStringLiteral("read %1 B/s, expected 1000 or unavailable").arg(num(p->ioReadBps))));
    // Counters read before the stall stay exact.
    QCOMPARE(findDisk(s2, "sda")->readBps, 102400.0);
    QCOMPARE(s2.diskTotal.a, 102400.0);

    writeTarget(3.01);
    const auto s3 = sampleAt(3);
    p = findProc(s3, 300);
    qInfo("prompt again: cpu %s %%, read %s B/s", qPrintable(num(p->cpuPercent)), qPrintable(num(p->ioReadBps)));
    QVERIFY2(honest(p->cpuPercent, 50.0),
             qPrintable(QStringLiteral("CPU %1 % after the stall, expected 50 or unavailable").arg(num(p->cpuPercent))));
    QVERIFY2(honest(p->ioReadBps, 1000.0),
             qPrintable(QStringLiteral("read %1 B/s after the stall, expected 1000 or unavailable").arg(num(p->ioReadBps))));
    QCOMPARE(findDisk(s3, "sda")->readBps, 102400.0);

    // Steady again: exact, available rates (one core = 100 %).
    writeTarget(4.01);
    const auto s4 = sampleAt(4);
    p = findProc(s4, 300);
    QCOMPARE(p->cpuPercent, 50.0);
    QCOMPARE(p->ioReadBps, 1000.0);
    QCOMPARE(findDisk(s4, "sda")->readBps, 102400.0);
}

// ---------------------------------------------------------------------------
// ARCH-002: the same PID, start time and comm can still change UID or argv.

// Agreed bound: a change is shown within 10 s, plus one 1 s interval.
constexpr int kMetadataBoundSamples = 11;

void TestCouncil::sameIdentityMetadataRefreshes_data()
{
    QTest::addColumn<int>("pid");
    QTest::newRow("pid 300") << 300;
    QTest::newRow("pid 303 (longest first stagger)") << 303;
}

void TestCouncil::sameIdentityMetadataRefreshes()
{
    QFETCH(int, pid);
    FixtureRoot r;
    writeBaseTree(r);
    const auto stat = [&](int k) { return pidStat(pid, "python3", 100 + 50 * uint64_t(k - 1), 7000); };
    const std::string base = "proc/" + std::to_string(pid) + "/";
    writeProc(r, pid, stat(1), "python3\0old.py\0"s, 1000);
    ps::Sampler smp(fixtureOptions(r, true));
    auto s = smp.sample(1, 1);
    QCOMPARE(findProc(s, pid)->user, "u1000"s);
    QCOMPARE(findProc(s, pid)->command, "python3 old.py"s);

    // Each phase changes the files before sample `from`; the change may have
    // happened just after sample from-1, so its age at sample k is k-from+1.
    struct Phase {
        const char* what;
        std::string cmdline; // empty: status and cmdline unreadable
        uint32_t uid;
        std::string shown;
        bool uidKnown;
    };
    const Phase phases[] = {
        {"setuid() plus setproctitle(), comm unchanged", "python3\0new.py\0"s, 1001, "python3 new.py", true},
        {"a second change right after a refresh", "python3\0newer.py\0"s, 1003, "python3 newer.py", true},
        {"status and cmdline unreadable at a refresh", {}, 0, "[python3]", false},
    };
    int k = 1;
    uint32_t previousUid = 1000;
    for (const Phase& ph : phases) {
        const int from = k + 1;
        int seenAt = 0;
        for (k = from; k < from + 20 && !seenAt; ++k) {
            if (ph.cmdline.empty()) {
                r.write(base + "stat", stat(k));
                r.remove(base + "status");
                r.remove(base + "cmdline");
            } else {
                writeProc(r, pid, stat(k), ph.cmdline, ph.uid);
            }
            s = smp.sample(k, 1);
            const auto* p = findProc(s, pid);
            QVERIFY(p);
            QCOMPARE(p->startTicks, uint64_t(7000));
            QCOMPARE(p->cpuPercent, 50.0); // the identity and its CPU baseline continue
            const bool shown = ph.uidKnown ? (p->uidKnown && p->uid == ph.uid && p->user == "u" + u(ph.uid)
                                              && p->command == ph.shown)
                                           : (!p->uidKnown && p->user.empty() && p->command == ph.shown);
            if (shown)
                seenAt = k;
            else
                QVERIFY2(p->uidKnown && p->uid == previousUid,
                         qPrintable(QStringLiteral("%1: sample %2 shows neither the old nor the new metadata "
                                                   "(uidKnown %3, uid %4, \"%5\")")
                                        .arg(QLatin1String(ph.what)).arg(k).arg(p->uidKnown).arg(p->uid)
                                        .arg(QString::fromStdString(p->command))));
        }
        --k;
        qInfo("%s: shown after %d s", ph.what, seenAt ? seenAt - from + 1 : -1);
        QVERIFY2(seenAt && seenAt - from + 1 <= kMetadataBoundSamples,
                 qPrintable(QStringLiteral("%1: not shown within %2 s (%3)")
                                .arg(QLatin1String(ph.what)).arg(kMetadataBoundSamples)
                                .arg(seenAt ? QStringLiteral("after %1 s").arg(seenAt - from + 1)
                                            : QStringLiteral("not in 20 s"))));
        previousUid = ph.uid;
    }
    // Readable again: a failed read is retried at the next scan.
    writeProc(r, pid, stat(k + 1), "python3\0back.py\0"s, 1004);
    s = smp.sample(k + 1, 1);
    const auto* p = findProc(s, pid);
    QVERIFY(p);
    QCOMPARE(p->cpuPercent, 50.0);
    QVERIFY2(p->uidKnown && p->uid == 1004 && p->user == "u1004" && p->command == "python3 back.py",
             qPrintable(QStringLiteral("the scan after a failed metadata read shows uidKnown %1 uid %2 \"%3\"")
                            .arg(p->uidKnown).arg(p->uid).arg(QString::fromStdString(p->command))));
}

void TestCouncil::initialMetadataFailureRecovers()
{
    FixtureRoot r;
    writeBaseTree(r);
    // Only stat is readable at first (status and cmdline failed transiently).
    r.write("proc/301/stat", pidStat(301, "tool", 100, 8000));
    ps::Sampler smp(fixtureOptions(r, true));
    auto s = smp.sample(1, 1);
    const auto* p = findProc(s, 301);
    QVERIFY(p);
    QVERIFY(!p->uidKnown);
    QCOMPARE(p->command, "[tool]"s);
    int recoveredAt = 0;
    for (int k = 2; k <= 31; ++k) {
        writeProc(r, 301, pidStat(301, "tool", 100 + 50 * uint64_t(k - 1), 8000), "tool\0--x\0"s, 1002);
        s = smp.sample(k, 1);
        p = findProc(s, 301);
        QVERIFY(p);
        QCOMPARE(p->cpuPercent, 50.0);
        const bool known = p->uidKnown && p->uid == 1002 && p->user == "u1002" && p->command == "tool --x";
        if (recoveredAt)
            QVERIFY2(known, "recovered metadata lost again");
        else if (known)
            recoveredAt = k;
    }
    qInfo("metadata recovered after %d s", recoveredAt ? recoveredAt - 1 : -1);
    QVERIFY2(recoveredAt, "after 30 s the UID is still unknown and the command still \"[tool]\"");
    // A failed read is retried at the next scan (one interval of tolerance).
    QVERIFY2(recoveredAt <= 3, qPrintable(QStringLiteral("recovered only at sample %1").arg(recoveredAt)));
}

// ---------------------------------------------------------------------------
// UX-001: compact (per-core) charts expose their current value to assistive
// technology through the QAccessible description, at data cadence only.
// This checks the Qt accessibility contract, not a screen reader end to end.

namespace {

// Records description/name/value change events while installed.
class AccessibleLog
{
public:
    AccessibleLog()
    {
        s_self = this;
        m_wasActive = QAccessible::isActive();
        m_previous = QAccessible::installUpdateHandler(&AccessibleLog::handle);
        QAccessible::setActive(true);
    }
    ~AccessibleLog()
    {
        QAccessible::installUpdateHandler(m_previous);
        QAccessible::setActive(m_wasActive);
        s_self = nullptr;
    }
    int count(const QObject* o) const { return int(std::count(m_events.begin(), m_events.end(), o)); }
    int total() const { return int(m_events.size()); }

private:
    static void handle(QAccessibleEvent* e)
    {
        if (!s_self || !e)
            return;
        const auto t = e->type();
        if (t == QAccessible::DescriptionChanged || t == QAccessible::NameChanged || t == QAccessible::ValueChanged)
            s_self->m_events.push_back(e->object());
    }
    static inline AccessibleLog* s_self = nullptr;
    QAccessible::UpdateHandler m_previous = nullptr;
    bool m_wasActive = false;
    std::vector<const QObject*> m_events;
};

void addCoreSample(ps::History& h, double time, double value)
{
    h.beginSample(time, true);
    h.set(ps::keys::core(0), "CPU 0", ps::Unit::Percent, value, ps::SeriesGroup::Core);
    h.endSample();
}

QString descriptionOf(QWidget* w)
{
    QAccessibleInterface* a = QAccessible::queryAccessibleInterface(w);
    return a ? a->text(QAccessible::Description) : QStringLiteral("<no accessible interface>");
}

QString nameOf(QWidget* w)
{
    QAccessibleInterface* a = QAccessible::queryAccessibleInterface(w);
    return a ? a->text(QAccessible::Name) : QString();
}

} // namespace

void TestCouncil::compactChartAccessibleText()
{
    ps::History h;
    TimeChart c(QStringLiteral("CPU 0"), ps::Unit::Percent);
    c.setMode(TimeChart::Mode::Compact);
    c.setFixedRange(0, 100);
    c.setWindowSeconds(60);
    c.setHistory(&h);
    c.setSeries({ChartSeries {ps::keys::core(0), QStringLiteral("CPU 0"), 0}});
    c.setEmptyText(QStringLiteral("Measuring\u2026"));
    QVERIFY(QAccessible::queryAccessibleInterface(&c));
    QCOMPARE(nameOf(&c), QStringLiteral("CPU 0"));
    QCOMPARE(c.focusPolicy(), Qt::NoFocus);
    const auto d = [&] { return descriptionOf(&c); };

    // No sample yet: an explanation, never a number.
    QVERIFY2(!d().isEmpty() && !d().contains(QLatin1Char('%')), qPrintable(d()));

    const auto step = [&](double t, double v) {
        addCoreSample(h, t, v);
        c.setViewEnd(t);
        c.refresh();
    };
    step(100, 12.5);
    QCOMPARE(d(), QStringLiteral("CPU 0 12.5 %"));
    step(101, 0.0); // a measured zero is a value, not "no data"
    QCOMPARE(d(), QStringLiteral("CPU 0 0.0 %"));
    step(102, ps::kNaN); // unavailable now: neither the old value nor a zero
    QVERIFY2(!d().contains(QLatin1String("12.5")) && !d().contains(QLatin1String("0.0")) && d().startsWith(QLatin1String("CPU 0")),
             qPrintable(d()));
    step(103, 100.0);
    QCOMPARE(d(), QStringLiteral("CPU 0 100.0 %"));

    // The frame path moves the axis but does not touch the text...
    c.advanceView(200);
    QCOMPARE(d(), QStringLiteral("CPU 0 100.0 %"));
    // ...the next refresh (data cadence) says the window has no samples.
    c.setViewEnd(200);
    c.refresh();
    QVERIFY2(!d().isEmpty() && !d().contains(QLatin1String("100.0")) && !d().contains(QLatin1Char('%')), qPrintable(d()));
    c.setViewEnd(103.5);
    c.refresh();
    QCOMPARE(d(), QStringLiteral("CPU 0 100.0 %"));

    // Full mode announces the cursor's sample; compact ignores the cursor.
    c.setMode(TimeChart::Mode::Full);
    QCOMPARE(c.focusPolicy(), Qt::TabFocus);
    c.setCursorTime(100);
    QVERIFY2(!c.readoutText().isEmpty() && c.readoutText().contains(QLatin1String("12.5")), qPrintable(c.readoutText()));
    QCOMPARE(d(), c.readoutText());
    c.setMode(TimeChart::Mode::Compact);
    QCOMPARE(c.focusPolicy(), Qt::NoFocus);
    QCOMPARE(d(), QStringLiteral("CPU 0 100.0 %"));
    c.setMode(TimeChart::Mode::Full);
    QCOMPARE(d(), c.readoutText().isEmpty() ? QStringLiteral("CPU 0 100.0 %") : c.readoutText());
    c.clearCursor();
    c.setMode(TimeChart::Mode::Compact);
    QCOMPARE(d(), QStringLiteral("CPU 0 100.0 %"));

    // A core over the history quota has no series: say why, with no number.
    c.setSeries({ChartSeries {ps::keys::core(9), QStringLiteral("CPU 9"), 0}});
    c.setEmptyText(QStringLiteral("Not recorded: history limit"));
    QCOMPARE(d(), QStringLiteral("Not recorded: history limit"));
}

void TestCouncil::compactChartAnnouncesAtDataCadenceOnly()
{
    ps::History h;
    for (int i = 0; i <= 10; ++i)
        addCoreSample(h, 100 + i, 10 + i);
    TimeChart c(QStringLiteral("CPU 0"), ps::Unit::Percent);
    c.setMode(TimeChart::Mode::Compact);
    c.setFixedRange(0, 100);
    c.setWindowSeconds(60);
    c.setHistory(&h);
    c.setSeries({ChartSeries {ps::keys::core(0), QStringLiteral("CPU 0"), 0}});
    c.setViewEnd(110);
    c.refresh();
    c.resize(220, 90);
    c.show();
    QVERIFY(QTest::qWaitForWindowExposed(&c));
    QTest::qWait(30);
    QCOMPARE(descriptionOf(&c), QStringLiteral("CPU 0 20.0 %"));

    AccessibleLog log;
    const int paintsBefore = c.paintCount();
    // Two seconds of 60 FPS frames between samples.
    for (int f = 1; f <= 120; ++f) {
        c.advanceView(110 + f / 60.0 * 0.45); // stays within one sample interval
        QCoreApplication::processEvents();
    }
    QTest::qWait(20);
    const int framePaints = c.paintCount() - paintsBefore;
    qInfo("frame path: %d paints, %d accessibility events", framePaints, log.count(&c));
    QVERIFY2(framePaints > 0, "the frame path painted nothing, so the check below would prove nothing");
    QCOMPARE(log.count(&c), 0);

    // A new, different value: exactly one announcement.
    c.setRevealEnabled(true);
    addCoreSample(h, 111, 55.0);
    c.setViewEnd(111);
    c.refresh();
    QCOMPARE(descriptionOf(&c), QStringLiteral("CPU 0 55.0 %"));
    QCOMPARE(log.count(&c), 1);
    // Refreshing without a change announces nothing.
    c.refresh();
    QCOMPARE(log.count(&c), 1);
    // Frames during the reveal of that sample announce nothing either.
    const int revealPaints = c.paintCount();
    for (int f = 1; f <= 30; ++f) {
        c.advanceView(111 + f / 60.0);
        QTest::qWait(5);
    }
    qInfo("reveal frames: %d paints", c.paintCount() - revealPaints);
    QCOMPARE(log.count(&c), 1);
    QCOMPARE(descriptionOf(&c), QStringLiteral("CPU 0 55.0 %"));
}

namespace {

ps::SnapshotPtr coreSnapshot(double time, double core0, double core1)
{
    static uint64_t seq = 0;
    auto s = std::make_shared<ps::Snapshot>();
    s->sequence = ++seq;
    s->time = time;
    s->interval = 1.0;
    s->continuous = true;
    s->logicalCpus = 3;
    s->cpu.total = (core0 + core1) / 2;
    for (int i = 0; i < 3; ++i) {
        ps::CoreUsage c;
        c.index = i;
        c.online = i < 2;
        c.usage.total = i == 0 ? core0 : i == 1 ? core1 : ps::kNaN;
        s->cores.push_back(c);
    }
    return s;
}

} // namespace

void TestCouncil::detailsCoreChartsStayHonest()
{
    QTemporaryDir dir;
    auto clock = std::make_shared<double>(100.0);
    MainWindowOptions o;
    o.startCollector = false;
    o.settingsFile = dir.filePath(QStringLiteral("pulsescope.ini"));
    o.windowProbe.enabled = false;
    o.initialIntervalMs = 1000;
    o.clock = [clock] { return *clock; };
    MainWindow w(o);
    w.setWindowSeconds(60);
    w.setDetailsVisible(true);
    w.showPage(MainWindow::CpuPage);
    w.resize(1180, 780);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    const auto apply = [&](double t, double c0, double c1) {
        *clock = t;
        w.applySnapshot(coreSnapshot(t, c0, c1));
    };
    apply(100, 12.5, 0.0);
    apply(101, 12.5, 0.0);
    apply(102, 12.5, 0.0);
    QTest::qWait(30);

    auto* grid = w.findChild<QWidget*>(QStringLiteral("coreGrid"));
    QVERIFY(grid);
    TimeChart* cores[3] = {};
    for (auto* c : grid->findChildren<TimeChart*>())
        for (int i = 0; i < 3; ++i)
            if (nameOf(c) == QStringLiteral("CPU %1").arg(i))
                cores[i] = c;
    for (int i = 0; i < 3; ++i) {
        QVERIFY2(cores[i], qPrintable(QStringLiteral("no chart named CPU %1").arg(i)));
        QCOMPARE(cores[i]->focusPolicy(), Qt::NoFocus);
    }
    TimeChart* full = nullptr;
    for (auto* c : w.findChildren<TimeChart*>())
        if (c->parentWidget() != grid && nameOf(c) == QLatin1String("CPU utilization"))
            full = c;
    QVERIFY2(full, "no full \"CPU utilization\" chart");
    QCOMPARE(full->focusPolicy(), Qt::TabFocus);
    QCOMPARE(descriptionOf(cores[0]), QStringLiteral("CPU 0 12.5 %"));
    QCOMPARE(descriptionOf(cores[1]), QStringLiteral("CPU 1 0.0 %"));
    QVERIFY2(!descriptionOf(cores[2]).contains(QLatin1Char('%')), qPrintable(descriptionOf(cores[2]))); // offline

    AccessibleLog log;
    // One second of frames between samples: no announcements.
    const int paintsBefore = cores[0]->paintCount();
    for (int f = 1; f <= 60; ++f) {
        *clock = 102 + f / 60.0 * 0.9;
        QTest::qWait(16);
    }
    qInfo("1 s of frames: CPU 0 painted %d times, %d accessibility events on core charts",
          cores[0]->paintCount() - paintsBefore, log.count(cores[0]) + log.count(cores[1]) + log.count(cores[2]));
    for (auto* c : cores)
        QCOMPARE(log.count(c), 0);
    QCOMPARE(log.count(full), 0);

    // A sample changing only CPU 0: one announcement, on CPU 0 only.
    apply(103.2, 40.0, 0.0);
    QCOMPARE(descriptionOf(cores[0]), QStringLiteral("CPU 0 40.0 %"));
    QCOMPARE(log.count(cores[0]), 1);
    QCOMPARE(log.count(cores[1]), 0);
    QCOMPARE(log.count(cores[2]), 0);
    QVERIFY2(descriptionOf(full).contains(QLatin1String("Total 20.0 %")), qPrintable(descriptionOf(full)));
    QCOMPARE(log.count(full), 1);

    // Samples stop. Once the window says "No recent samples", the value must
    // not still be announced as if current; and the 500 ms stale refreshes
    // must not repeat announcements.
    *clock = 103.2 + 6;
    QTest::qWait(700);
    const int staleEvents = log.count(cores[0]);
    const int fullStaleEvents = log.count(full);
    QTest::qWait(1600);
    const QString stale = descriptionOf(cores[0]);
    qInfo("stale: CPU 0 \"%s\", notice \"%s\"", qPrintable(stale), qPrintable(cores[0]->notice()));
    QVERIFY2(!cores[0]->notice().isEmpty(), "the stale state never reached the per-core charts");
    QVERIFY2(log.count(cores[0]) == staleEvents, qPrintable(QStringLiteral("%1 repeated announcements while stale")
                                                                .arg(log.count(cores[0]) - staleEvents)));
    const QString staleNotice = cores[0]->notice();
    QStringList problems;
    if (!(stale.contains(staleNotice) || !stale.contains(QLatin1String("40.0"))))
        problems << QStringLiteral("compact: while the chart shows \"%1\", assistive technology still hears \"%2\" "
                                   "as the current value").arg(staleNotice, stale);
    // The full CPU chart without a cursor announces its latest values: the
    // same rule applies.
    const QString fullStale = descriptionOf(full);
    qInfo("stale, full \"%s\": \"%s\" (readout \"%s\")", qPrintable(nameOf(full)), qPrintable(fullStale),
          qPrintable(full->readoutText()));
    QVERIFY2(full->readoutText().isEmpty(), "the full chart unexpectedly has a cursor");
    QCOMPARE(full->notice(), staleNotice);
    if (log.count(full) != fullStaleEvents)
        problems << QStringLiteral("full: %1 repeated announcements while stale").arg(log.count(full) - fullStaleEvents);
    if (!(fullStale.contains(staleNotice) || !fullStale.contains(QLatin1String("20.0"))))
        problems << QStringLiteral("full, no cursor: while the chart shows \"%1\", assistive technology still hears "
                                   "\"%2\" as the current value").arg(staleNotice, fullStale);
    // Expired: every sample left the window.
    *clock = 103.2 + 70;
    QTest::qWait(700);
    const QString expired = descriptionOf(cores[0]);
    qInfo("expired: CPU 0 \"%s\", full \"%s\"", qPrintable(expired), qPrintable(descriptionOf(full)));
    QVERIFY2(!expired.isEmpty() && !expired.contains(QLatin1Char('%')), qPrintable(expired));
    QVERIFY2(!descriptionOf(full).contains(QLatin1Char('%')), qPrintable(descriptionOf(full)));
    // Recovery: the next current sample is announced at once, without a notice.
    apply(*clock, 8.0, 0.0);
    QCOMPARE(descriptionOf(cores[0]), QStringLiteral("CPU 0 8.0 %"));
    QVERIFY2(cores[0]->notice().isEmpty(), qPrintable(cores[0]->notice()));
    QVERIFY2(descriptionOf(full).contains(QLatin1String("Total 4.0 %")) && !descriptionOf(full).contains(staleNotice),
             qPrintable(descriptionOf(full)));

    // Paused: the frozen values are announced as paused, once, in both modes.
    w.setPaused(true);
    QTest::qWait(50);
    const QString pausedNotice = full->notice();
    const QString pausedCompact = descriptionOf(cores[0]);
    const QString pausedFull = descriptionOf(full);
    qInfo("paused: notice \"%s\", CPU 0 \"%s\", full \"%s\"", qPrintable(pausedNotice), qPrintable(pausedCompact),
          qPrintable(pausedFull));
    QVERIFY2(!pausedNotice.isEmpty() && cores[0]->notice() == pausedNotice, qPrintable(pausedNotice));
    if (!(pausedCompact.contains(pausedNotice) || !pausedCompact.contains(QLatin1String("8.0"))))
        problems << QStringLiteral("compact: paused value \"%1\" does not say \"%2\"").arg(pausedCompact, pausedNotice);
    if (!(pausedFull.contains(pausedNotice) || !pausedFull.contains(QLatin1String("4.0"))))
        problems << QStringLiteral("full, no cursor: paused value \"%1\" does not say \"%2\"").arg(pausedFull, pausedNotice);
    const int pausedEvents = log.count(cores[0]) + log.count(full);
    for (int f = 1; f <= 60; ++f) {
        *clock += 0.015;
        QTest::qWait(16);
    }
    if (log.count(cores[0]) + log.count(full) != pausedEvents)
        problems << QStringLiteral("%1 announcements during 1 s paused").arg(log.count(cores[0]) + log.count(full) - pausedEvents);
    w.setPaused(false);
    apply(*clock, 30.0, 0.0);
    QCOMPARE(descriptionOf(cores[0]), QStringLiteral("CPU 0 30.0 %"));
    QVERIFY2(descriptionOf(full).contains(QLatin1String("Total 15.0 %")) && !descriptionOf(full).contains(pausedNotice),
             qPrintable(descriptionOf(full)));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
    w.close();
}

// ---------------------------------------------------------------------------
// SYS-001: a thread that cannot start. Runs in a child process.

void TestCouncil::threadStartFailure_data()
{
    QTest::addColumn<QString>("scenario");
    QTest::newRow("capacity prober") << QStringLiteral("fsprober");
    QTest::newRow("collector") << QStringLiteral("collector");
    QTest::newRow("window probe") << QStringLiteral("windowprobe");
    QTest::newRow("collector retry") << QStringLiteral("collector-retry");
    QTest::newRow("window probe retry") << QStringLiteral("windowprobe-retry");
}

void TestCouncil::threadStartFailure()
{
    QFETCH(QString, scenario);
    if (::geteuid() == 0)
        QSKIP("root is exempt from RLIMIT_NPROC");
    QProcess child;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PS_COUNCIL_CHILD"), scenario);
    child.setProcessEnvironment(env);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), {});
    QVERIFY(child.waitForStarted(5000));
    if (!child.waitForFinished(30000)) {
        child.kill();
        child.waitForFinished(5000);
        QFAIL(qPrintable(QStringLiteral("child did not finish within 30 s:\n") + QString::fromLocal8Bit(child.readAll())));
    }
    const QString out = QString::fromLocal8Bit(child.readAll());
    for (const QString& line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        if (line.startsWith(QLatin1String("INFO: ")))
            qInfo("%s", qPrintable(line.mid(6)));
    if (child.exitStatus() == QProcess::NormalExit && child.exitCode() == kChildSkip)
        QSKIP("RLIMIT_NPROC does not stop thread creation for this user");
    QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0 && !out.contains(QLatin1String("FAIL: ")),
             qPrintable(QStringLiteral("exit %1 (%2):\n%3")
                            .arg(child.exitCode())
                            .arg(child.exitStatus() == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crashed"))
                            .arg(out)));
}

int main(int argc, char** argv)
{
    if (const char* scenario = std::getenv("PS_COUNCIL_CHILD")) {
        QCoreApplication app(argc, argv);
        const std::string which = scenario;
        if (which == "fsprober")
            return childFsProber();
        if (which == "collector")
            return childCollector();
        if (which == "windowprobe")
            return childWindowProbe();
        if (which == "collector-retry")
            return childCollectorRetry();
        if (which == "windowprobe-retry")
            return childWindowProbeRetry();
        return 2;
    }
    QApplication app(argc, argv);
    TestCouncil t;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&t, argc, argv);
}

#include "tst_council.moc"
