// Read-only checks against this host's real /proc and /sys. Nothing here
// assumes a particular device, interface, sensor or process exists beyond
// this test process itself; values that can change between reads are
// compared with bounds rather than equality.
#include "app/collector.h"
#include "core/linuxparsers.h"
#include "core/sampler.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<uint64_t> meminfoKb(const std::string& key)
{
    std::istringstream in(readFile("/proc/meminfo"));
    std::string name;
    uint64_t value = 0;
    std::string unit;
    while (in >> name >> value) {
        std::getline(in, unit);
        if (name == key + ":")
            return value;
    }
    return std::nullopt;
}

int procStatCpuLines()
{
    std::istringstream in(readFile("/proc/stat"));
    std::string line;
    int n = 0;
    while (std::getline(in, line))
        if (line.rfind("cpu", 0) == 0 && line.size() > 3 && std::isdigit(static_cast<unsigned char>(line[3])))
            ++n;
    return n;
}

// Sectors read+written per disk from /proc/diskstats.
std::map<std::string, std::pair<uint64_t, uint64_t>> diskSectors()
{
    std::map<std::string, std::pair<uint64_t, uint64_t>> out;
    std::istringstream in(readFile("/proc/diskstats"));
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string major, minor, name, f1, f2, f3, f4, f5, f6, f7;
        uint64_t sectorsRead = 0, sectorsWritten = 0;
        if (ls >> major >> minor >> name >> f1 >> f2 >> sectorsRead >> f4 >> f5 >> f6 >> sectorsWritten)
            out[name] = {sectorsRead, sectorsWritten};
    }
    return out;
}

// Received/transmitted bytes per interface from /proc/net/dev.
std::map<std::string, std::pair<uint64_t, uint64_t>> netBytes()
{
    std::map<std::string, std::pair<uint64_t, uint64_t>> out;
    std::istringstream in(readFile("/proc/net/dev"));
    std::string line;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string name = line.substr(0, colon);
        name.erase(0, name.find_first_not_of(' '));
        std::istringstream ls(line.substr(colon + 1));
        uint64_t rx = 0, tx = 0, skip = 0;
        if (!(ls >> rx))
            continue;
        for (int i = 0; i < 7; ++i)
            ls >> skip;
        if (ls >> tx)
            out[name] = {rx, tx};
    }
    return out;
}

bool inPercentRange(double v) { return v >= 0.0 && v <= 100.0 + 1e-9; }

} // namespace

class TestLive : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void repeatedSamplesSatisfyInvariants();
    void countersBracketReportedRates();
    void ownProcessMatchesProcSelf();
    void rootCapacityMatchesStatvfs();
    void temperaturesAreRealOrExplained();
    void collectorDeliversAndStops();
};

void TestLive::repeatedSamplesSatisfyInvariants()
{
    ps::SamplerOptions o;
    o.fsProbeWaitMs = 2000;
    ps::Sampler sampler(o);
    const double interval = 0.3;
    const long cpuLines = procStatCpuLines();
    const auto memTotalKb = meminfoKb("MemTotal");
    QVERIFY(memTotalKb.has_value());

    for (int k = 0; k < 5; ++k) {
        if (k > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const ps::Snapshot s = sampler.sample(ps::monotonicSeconds(), interval);
        QVERIFY(std::isfinite(s.time));
        QVERIFY(ps::isValid(s.collectMs) && s.collectMs >= 0);

        // First sample: no baseline yet, so no rate is invented.
        QCOMPARE(s.continuous, k > 0);
        if (k == 0) {
            QVERIFY(std::isnan(s.cpu.total));
            QVERIFY(std::isnan(s.diskTotal.a) && std::isnan(s.netTotal.a));
            for (const auto& d : s.disks)
                QVERIFY(std::isnan(d.readBps));
            for (const auto& p : s.processes)
                QVERIFY(std::isnan(p.cpuPercent));
        } else {
            QVERIFY2(inPercentRange(s.cpu.total), qPrintable(QString::number(s.cpu.total)));
            QVERIFY(inPercentRange(s.cpu.user) && inPercentRange(s.cpu.system) && inPercentRange(s.cpu.iowait));
            QVERIFY(s.cpu.user + s.cpu.system <= s.cpu.total + 1e-6);
            for (const auto& c : s.cores)
                if (c.online)
                    QVERIFY(std::isnan(c.usage.total) || inPercentRange(c.usage.total));
        }

        // CPUs: /proc/stat lists the online CPUs.
        const auto online = std::count_if(s.cores.begin(), s.cores.end(), [](const auto& c) { return c.online; });
        QVERIFY(std::abs(online - cpuLines) <= 1); // allow a hotplug between reads
        QVERIFY(s.logicalCpus >= online);

        // Memory: exact total, used = total - available, parts within total.
        QVERIFY(s.memory.valid);
        QCOMPARE(s.memory.total, static_cast<double>(*memTotalKb) * 1024.0);
        QVERIFY(ps::isValid(s.memory.available));
        QCOMPARE(s.memory.used + s.memory.available, s.memory.total);
        if (const auto availKb = meminfoKb("MemAvailable"))
            QVERIFY(std::abs(s.memory.available - *availKb * 1024.0) < 0.1 * s.memory.total);
        if (ps::isValid(s.memory.cache))
            QVERIFY(s.memory.cache >= 0 && s.memory.cache <= s.memory.total);
        const auto swapTotalKb = meminfoKb("SwapTotal");
        QVERIFY(swapTotalKb.has_value());
        QCOMPARE(s.memory.hasSwap, *swapTotalKb > 0);
        if (s.memory.hasSwap && ps::isValid(s.memory.swapUsed))
            QCOMPARE(s.memory.swapUsed + s.memory.swapFree, s.memory.swapTotal);

        // Disks: whole disks only, rates non-negative, the aggregate sums the
        // physical disks it counts and never includes virtual devices.
        int physical = 0;
        double sumRead = 0;
        bool allValid = true;
        for (const auto& d : s.disks) {
            QVERIFY(QFileInfo::exists(QStringLiteral("/sys/block/") + QString::fromStdString(d.name)));
            QVERIFY(std::isnan(d.readBps) || d.readBps >= 0);
            QVERIFY(std::isnan(d.writeBps) || d.writeBps >= 0);
            if (d.kind == ps::DeviceKind::Physical) {
                ++physical;
                allValid = allValid && ps::isValid(d.readBps);
                sumRead += ps::isValid(d.readBps) ? d.readBps : 0;
            }
        }
        QCOMPARE(s.diskTotal.members, physical);
        if (k > 0 && physical > 0 && allValid)
            QVERIFY(std::abs(s.diskTotal.a - sumRead) <= 1e-6 * std::max(1.0, sumRead));

        // Network: loopback is listed as such and never in the aggregate.
        int members = 0;
        for (const auto& n : s.interfaces) {
            QVERIFY(std::isnan(n.rxBps) || n.rxBps >= 0);
            QVERIFY(std::isnan(n.txBps) || n.txBps >= 0);
            if (n.name == "lo")
                QCOMPARE(int(n.kind), int(ps::DeviceKind::Loopback));
            if (n.kind != ps::DeviceKind::Loopback)
                ++members;
        }
        QVERIFY(s.netTotal.members <= members);

        // Processes: a real scan that includes this process.
        QVERIFY(s.processesScanned);
        QVERIFY(s.threadCount >= static_cast<long>(s.processes.size()));
        for (const auto& p : s.processes) {
            QVERIFY(p.pid > 0);
            QVERIFY(std::isnan(p.cpuPercent) || (p.cpuPercent >= 0 && p.cpuPercent <= 125.0 * s.logicalCpus + 5));
            QVERIFY(std::isnan(p.rssBytes) || p.rssBytes >= 0);
        }

        // Filesystems: capacity is internally consistent when available.
        for (const auto& fs : s.filesystems) {
            QVERIFY(!fs.mountPoints.empty());
            if (!ps::isValid(fs.totalBytes))
                continue;
            QVERIFY(fs.usedBytes >= 0 && fs.availBytes >= 0);
            QVERIFY(fs.usedBytes <= fs.totalBytes);
            QVERIFY(fs.availBytes <= fs.totalBytes);
        }
    }
}

void TestLive::countersBracketReportedRates()
{
    // Rates times the measured interval must lie between zero and the counter
    // movement over a window that encloses the interval.
    ps::Sampler sampler;
    const auto disksBefore = diskSectors();
    const auto netBefore = netBytes();
    ps::Snapshot first = sampler.sample(ps::monotonicSeconds(), 0.5);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const ps::Snapshot second = sampler.sample(ps::monotonicSeconds(), 0.5);
    const auto disksAfter = diskSectors();
    const auto netAfter = netBytes();
    QVERIFY(!first.continuous);
    QVERIFY(second.continuous);
    QVERIFY(second.interval > 0.3 && second.interval < 5);

    int checked = 0;
    for (const auto& d : second.disks) {
        const auto b = disksBefore.find(d.name), a = disksAfter.find(d.name);
        if (b == disksBefore.end() || a == disksAfter.end() || !ps::isValid(d.readBps))
            continue;
        const double readMax = static_cast<double>(a->second.first - b->second.first) * 512.0;
        const double writeMax = static_cast<double>(a->second.second - b->second.second) * 512.0;
        QVERIFY2(d.readBps * second.interval <= readMax + 1, d.name.c_str());
        QVERIFY2(d.writeBps * second.interval <= writeMax + 1, d.name.c_str());
        ++checked;
    }
    for (const auto& n : second.interfaces) {
        const auto b = netBefore.find(n.name), a = netAfter.find(n.name);
        if (b == netBefore.end() || a == netAfter.end() || !ps::isValid(n.rxBps))
            continue;
        QVERIFY2(n.rxBps * second.interval <= static_cast<double>(a->second.first - b->second.first) + 1,
                 n.name.c_str());
        QVERIFY2(n.txBps * second.interval <= static_cast<double>(a->second.second - b->second.second) + 1,
                 n.name.c_str());
        ++checked;
    }
    qInfo("bracketed %d device/interface rates", checked);
}

void TestLive::ownProcessMatchesProcSelf()
{
    ps::Sampler sampler;
    sampler.sample(ps::monotonicSeconds(), 0.3);
    // Burn a little CPU so this process has something to measure.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    volatile double sink = 0;
    while (std::chrono::steady_clock::now() < until)
        sink = sink + std::sqrt(static_cast<double>(sink) + 1.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const ps::Snapshot s = sampler.sample(ps::monotonicSeconds(), 0.3);

    const int self = static_cast<int>(::getpid());
    const auto it = std::find_if(s.processes.begin(), s.processes.end(), [&](const auto& p) { return p.pid == self; });
    QVERIFY(it != s.processes.end());
    std::string comm = readFile("/proc/self/comm");
    while (!comm.empty() && comm.back() == '\n')
        comm.pop_back();
    QCOMPARE(it->name, ps::parse::sanitize(comm));
    const auto stat = ps::parse::parsePidStat(readFile("/proc/self/stat"));
    QVERIFY(stat.has_value());
    QCOMPARE(it->startTicks, stat->startTime);
    QCOMPARE(it->uid, static_cast<uint32_t>(::getuid()));
    QVERIFY(ps::isValid(it->cpuPercent) && it->cpuPercent > 0);
    // RSS within a factor of two of VmRSS read now (the heap may move).
    std::istringstream status(readFile("/proc/self/status"));
    std::string line;
    double vmRss = -1;
    while (std::getline(status, line))
        if (line.rfind("VmRSS:", 0) == 0)
            vmRss = std::stod(line.substr(6)) * 1024.0;
    QVERIFY(vmRss > 0);
    QVERIFY(ps::isValid(it->rssBytes));
    QVERIFY(it->rssBytes > vmRss / 2 && it->rssBytes < vmRss * 2);
    // Own /proc/PID/io is readable, so I/O rates are measured (not denied).
    QVERIFY(!it->ioDenied);
}

void TestLive::rootCapacityMatchesStatvfs()
{
    ps::SamplerOptions o;
    o.fsProbeWaitMs = 3000;
    ps::Sampler sampler(o);
    const ps::Snapshot s = sampler.sample(ps::monotonicSeconds(), 1.0);
    const ps::Filesystem* root = nullptr;
    for (const auto& fs : s.filesystems)
        if (std::find(fs.mountPoints.begin(), fs.mountPoints.end(), "/") != fs.mountPoints.end())
            root = &fs;
    if (!root)
        QSKIP("the root filesystem is not a local, capacity-probed filesystem here");
    QVERIFY2(root->error.empty(), root->error.c_str());
    QVERIFY(ps::isValid(root->totalBytes));
    struct statvfs st {};
    QCOMPARE(::statvfs("/", &st), 0);
    const double total = static_cast<double>(st.f_blocks) * static_cast<double>(st.f_frsize);
    const double avail = static_cast<double>(st.f_bavail) * static_cast<double>(st.f_frsize);
    QVERIFY(std::abs(root->totalBytes - total) <= 0.01 * total);
    QVERIFY(std::abs(root->availBytes - avail) <= 0.02 * total + 64.0 * 1024 * 1024);
}

void TestLive::temperaturesAreRealOrExplained()
{
    ps::Sampler sampler;
    const ps::Snapshot s = sampler.sample(ps::monotonicSeconds(), 1.0);
    if (s.temperatures.empty()) {
        QVERIFY2(!s.temperatureNote.empty(), "no sensors and no explanation");
        qInfo("no temperature sensors: %s", s.temperatureNote.c_str());
        return;
    }
    std::set<std::string> keys;
    int valid = 0;
    for (const auto& t : s.temperatures) {
        QVERIFY(keys.insert(t.key).second); // keys are unique
        QVERIFY(!t.displayName.empty());
        if (!ps::isValid(t.celsius))
            continue; // explicitly unavailable
        ++valid;
        QVERIFY(t.celsius >= -60 && t.celsius <= 200);
        // Compare with the kernel file read now (it may have changed a little).
        const auto raw = ps::parse::toI64(ps::parse::trim(readFile(t.path)));
        if (raw)
            QVERIFY2(std::abs(*raw / 1000.0 - t.celsius) < 15.0, t.path.c_str());
    }
    qInfo("%zu sensors, %d reading", s.temperatures.size(), valid);
}

void TestLive::collectorDeliversAndStops()
{
    Collector collector;
    QSignalSpy spy(&collector, &Collector::snapshotReady);
    collector.start(500);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 3, 8000);
    std::vector<ps::SnapshotPtr> got;
    for (const auto& args : spy)
        got.push_back(args.at(0).value<ps::SnapshotPtr>());
    QVERIFY(!got.front()->continuous);
    for (std::size_t i = 1; i < got.size(); ++i) {
        QVERIFY(got[i]->time > got[i - 1]->time);
        QVERIFY(got[i]->sequence > got[i - 1]->sequence);
    }
    QVERIFY(collector.stop(3000));
    QVERIFY(!collector.isRunning());
    const int after = static_cast<int>(spy.count());
    QTest::qWait(700);
    QCOMPARE(static_cast<int>(spy.count()), after);
}

QTEST_GUILESS_MAIN(TestLive)
#include "tst_live.moc"
