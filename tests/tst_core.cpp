// Focused regression cases for identity, escaping and overflow guarantees
// that the broader adversarial suite does not pin down directly.
#include "core/history.h"
#include "core/linuxparsers.h"
#include "core/metrics.h"
#include "core/sampler.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace std::string_literals;
namespace fs = std::filesystem;

namespace {

class Fixture {
public:
    Fixture() { QVERIFY(m_dir.isValid()); }
    std::string path(const std::string& rel) const { return m_dir.path().toStdString() + "/" + rel; }
    void write(const std::string& rel, const std::string& content) const
    {
        const fs::path p = path(rel);
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << content;
    }
    void mkdir(const std::string& rel) const { fs::create_directories(path(rel)); }
    void symlink(const std::string& target, const std::string& rel) const
    {
        fs::create_directories(fs::path(path(rel)).parent_path());
        fs::create_directory_symlink(target, path(rel));
    }
    ps::SamplerOptions options(bool processes = false) const
    {
        ps::SamplerOptions o;
        o.procRoot = path("proc");
        o.sysRoot = path("sys");
        o.clockTicks = 100;
        o.pageSize = 4096;
        o.scanProcesses = processes;
        o.userName = [](uint32_t uid) { return std::to_string(uid); };
        o.fsProbeWaitMs = 3000;
        return o;
    }
    void writeBase() const
    {
        write("proc/stat", "cpu 0 0 0 100 0 0 0 0 0 0\ncpu0 0 0 0 100 0 0 0 0 0 0\n");
        write("proc/meminfo", "MemTotal: 1000 kB\nMemFree: 100 kB\nMemAvailable: 500 kB\n");
        write("proc/loadavg", "0.00 0.00 0.00 1/1 1\n");
        write("proc/uptime", "1000.00 0.00\n");
        write("proc/diskstats", "");
        write("proc/net/dev", "Inter-|\n face |\n");
        write("proc/self/mountinfo", "");
        mkdir("sys/block");
        mkdir("sys/class/net");
    }

private:
    QTemporaryDir m_dir;
};

std::string diskLine(const std::string& name, unsigned minor, uint64_t sectorsRead)
{
    return "   8 " + std::to_string(minor) + " " + name + " 1 0 " + std::to_string(sectorsRead)
        + " 0 1 0 0 0 0 0 0 0 0 0 0 0 0\n";
}

std::string pidStat(int pid, uint64_t utime, uint64_t stime)
{
    return std::to_string(pid) + " (app) S 1 1 1 0 -1 4194560 0 0 0 0 " + std::to_string(utime) + " "
        + std::to_string(stime) + " 0 0 20 0 1 0 5000 1000000 10 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 "
                                  "0 0 0 0 0\n";
}

std::string mountLine(int id, const std::string& dev, const std::string& root, const std::string& mp,
                      const std::string& type, const std::string& source)
{
    return std::to_string(id) + " 1 " + dev + " " + root + " " + mp + " rw - " + type + " " + source + " rw\n";
}

} // namespace

class TestCore : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void escapeNameIsInjectiveAndPrintable();
    void diskNamesWithControlBytesStayDistinct();
    void sensorKeysWithColonsStayDistinct();
    void csvHeadersStayUniqueForSharedLabels();
    void cacheNeedsEveryComponent();
    void processTickSumOverflowGivesNoCpu();
    void skewedCounterReadsGiveNoRates();
    void injectedProbeMountIdIsVerified();
    void productionProbeVerifiesMountIdentity();
};

void TestCore::escapeNameIsInjectiveAndPrintable()
{
    using ps::parse::escapeName;
    QCOMPARE(escapeName("eth0"), "eth0"s);
    QCOMPARE(escapeName("nvme0n1"), "nvme0n1"s);
    QCOMPARE(escapeName("my disk"), "my disk"s); // spaces are printable and kept
    QCOMPARE(escapeName("e\x01x"), "e\\x01x"s);
    QCOMPARE(escapeName("a\x7f"), "a\\x7f"s);
    QCOMPARE(escapeName("a\\b"), "a\\\\b"s);
    // Valid UTF-8 is kept; malformed bytes and C1 controls are escaped per byte.
    QCOMPARE(escapeName("caf\xc3\xa9"), "caf\xc3\xa9"s);
    QCOMPARE(escapeName("\xe2\x82\xac\xf0\x9f\x98\x80"), "\xe2\x82\xac\xf0\x9f\x98\x80"s);
    QCOMPARE(escapeName("a\xff"), "a\\xff"s);
    QCOMPARE(escapeName("a\xc3"), "a\\xc3"s);              // truncated sequence
    QCOMPARE(escapeName("\xc0\xaf"), "\\xc0\\xaf"s);       // overlong '/'
    QCOMPARE(escapeName("\xed\xa0\x80"), "\\xed\\xa0\\x80"s); // UTF-16 surrogate
    QCOMPARE(escapeName("\xf4\x90\x80\x80"), "\\xf4\\x90\\x80\\x80"s); // above U+10FFFF
    QCOMPARE(escapeName("\xc2\x85"), "\\xc2\\x85"s);       // C1 control NEL
    QCOMPARE(escapeName("\xc2\xa0"), "\xc2\xa0"s);         // no-break space is printable
    // Names that look like escapes differ from the bytes they would encode.
    const std::string inputs[] = {"e\x01x", "e\x02x", "e?x", "e\\x01x", "e\\\\x01x", "a\\", "a\\\\", "a", "",
                                  "\n", "\\n", "\x1f", "\x7f", "\\x7f", "d\xff", "d\xfe", "d\xef\xbf\xbd",
                                  "d\\xff", "\xc3", "\xc3\xa9", "\xc2\x85", "\\xc2\\x85"};
    std::set<std::string> outputs;
    std::set<QString> qstrings;
    for (const auto& in : inputs) {
        const std::string out = escapeName(in);
        for (char c : out)
            QVERIFY2(static_cast<unsigned char>(c) >= 0x20 && c != 0x7f, "escaped name contains a control byte");
        // The GUI stores keys as QString: the conversion must be lossless.
        const QString qs = QString::fromStdString(out);
        QCOMPARE(qs.toStdString(), out);
        outputs.insert(out);
        qstrings.insert(qs);
    }
    QCOMPARE(outputs.size(), std::size(inputs));
    QCOMPARE(qstrings.size(), std::size(inputs));
}

void TestCore::diskNamesWithControlBytesStayDistinct()
{
    Fixture f;
    f.writeBase();
    for (const std::string name : {"d\x01x", "d\x02x"}) {
        f.write("sys/block/" + name + "/size", "1000\n");
        f.mkdir("sys/block/" + name + "/device");
    }
    ps::Sampler smp(f.options());
    f.write("proc/diskstats", diskLine("d\x01x", 0, 0) + diskLine("d\x02x", 16, 0));
    smp.sample(1, 1);
    f.write("proc/diskstats", diskLine("d\x01x", 0, 2) + diskLine("d\x02x", 16, 8));
    const auto s = smp.sample(2, 1);
    QCOMPARE(s.disks.size(), std::size_t(2));
    QVERIFY(s.disks[0].name != s.disks[1].name);
    ps::History h(10, 1e9);
    ps::recordSnapshot(h, s);
    QCOMPARE(h.latestValue(ps::keys::diskRead(ps::parse::escapeName("d\x01x"))), 1024.0);
    QCOMPARE(h.latestValue(ps::keys::diskRead(ps::parse::escapeName("d\x02x"))), 4096.0);
    QCOMPARE(h.latestValue(ps::keys::diskAllRead), 5120.0);
}

void TestCore::sensorKeysWithColonsStayDistinct()
{
    // hwmon chip names may contain ':' and device paths always may (PCI
    // addresses). "dev/a:b" + chip "c" and "dev/a" + chip "b:c" must not
    // share a key, or two sensors would share one history series.
    Fixture f;
    f.writeBase();
    f.mkdir("devices/a:b");
    f.mkdir("devices/a");
    f.symlink(f.path("devices/a:b"), "sys/class/hwmon/hwmon0/device");
    f.write("sys/class/hwmon/hwmon0/name", "c\n");
    f.write("sys/class/hwmon/hwmon0/temp1_input", "41000\n");
    f.symlink(f.path("devices/a"), "sys/class/hwmon/hwmon1/device");
    f.write("sys/class/hwmon/hwmon1/name", "b:c\n");
    f.write("sys/class/hwmon/hwmon1/temp1_input", "42000\n");
    ps::Sampler smp(f.options());
    const auto s = smp.sample(1, 1);
    QCOMPARE(s.temperatures.size(), std::size_t(2));
    QVERIFY2(s.temperatures[0].key != s.temperatures[1].key, s.temperatures[0].key.c_str());
    ps::History h(10, 1e9);
    ps::recordSnapshot(h, s);
    int series = 0;
    for (const auto* sr : h.allSeries())
        series += sr->key.rfind("temp.", 0) == 0 ? 1 : 0;
    QCOMPARE(series, 2);
}

void TestCore::csvHeadersStayUniqueForSharedLabels()
{
    ps::History h(10, 1e9);
    QVERIFY(h.beginSample(1, true));
    h.set("temp.a", "Temperature: nvme \u00B7 Composite", ps::Unit::Celsius, 40);
    h.set("temp.b", "Temperature: nvme \u00B7 Composite", ps::Unit::Celsius, 50);
    h.set("cpu.total", "CPU total", ps::Unit::Percent, 5);
    h.endSample();
    const std::string csv = h.toCsv(0);
    const std::string header = csv.substr(0, csv.find("\r\n"));
    QVERIFY2(header.find("[temp.a]") != std::string::npos && header.find("[temp.b]") != std::string::npos,
             header.c_str());
    QVERIFY2(header.find("CPU total (%)") != std::string::npos && header.find("[cpu.total]") == std::string::npos,
             header.c_str()); // unique labels stay plain
    std::set<std::string> columns;
    std::stringstream ss(header);
    for (std::string col; std::getline(ss, col, ',');)
        QVERIFY2(columns.insert(col).second, col.c_str());
}

void TestCore::cacheNeedsEveryComponent()
{
    const std::string base = "MemTotal: 1000 kB\nMemFree: 100 kB\nMemAvailable: 500 kB\nBuffers: 10 kB\nCached: 20 kB\n";
    const auto full = ps::metrics::memoryFromRaw(ps::parse::parseMeminfo(base + "SReclaimable: 30 kB\n"));
    QCOMPARE(full.cache, 60.0 * 1024);
    // An invalid or missing part must not be counted as a measured zero.
    for (const std::string& tail : {"SReclaimable: -1 kB\n"s, "SReclaimable: 12oops kB\n"s, ""s}) {
        const auto mem = ps::metrics::memoryFromRaw(ps::parse::parseMeminfo(base + tail));
        QVERIFY2(!ps::isValid(mem.cache), qPrintable(QStringLiteral("cache %1 for '%2'")
                                                         .arg(mem.cache)
                                                         .arg(QString::fromStdString(tail))));
        QVERIFY(ps::isValid(mem.used)); // the rest of memory stays available
    }
}

void TestCore::processTickSumOverflowGivesNoCpu()
{
    Fixture f;
    f.writeBase();
    auto writeTicks = [&](uint64_t u, uint64_t s) {
        f.write("proc/42/stat", pidStat(42, u, s));
        f.write("proc/42/status", "Name:\tapp\nUid:\t0\t0\t0\t0\n");
        f.write("proc/42/cmdline", "app"s + '\0');
        f.write("proc/42/io", "read_bytes: 0\nwrite_bytes: 0\n");
    };
    ps::Sampler smp(f.options(true));
    auto cpuAt = [&](double t) {
        const auto snap = smp.sample(t, 1);
        for (const auto& p : snap.processes)
            if (p.pid == 42)
                return p.cpuPercent;
        return -1.0;
    };
    writeTicks(0, 0);
    QVERIFY(!ps::isValid(cpuAt(1))); // baseline
    writeTicks(UINT64_MAX, 11);      // utime + stime wraps to 10
    const double wrapped = cpuAt(2);
    QVERIFY2(!ps::isValid(wrapped), qPrintable(QStringLiteral("wrapped tick sum reported %1 %").arg(wrapped)));
    writeTicks(5, 5);
    const double afterBad = cpuAt(3);
    QVERIFY2(!ps::isValid(afterBad), qPrintable(QStringLiteral("delta against an unusable baseline: %1 %").arg(afterBad)));
    writeTicks(10, 10);
    QCOMPARE(cpuAt(4), 10.0); // recovers: 10 ticks at 100 Hz over 1 s
}

void TestCore::skewedCounterReadsGiveNoRates()
{
    QCOMPARE(ps::Sampler::readSkewLimit(0.5), 0.05);
    QCOMPARE(ps::Sampler::readSkewLimit(1), 0.1);
    QCOMPARE(ps::Sampler::readSkewLimit(5), 0.5);
    QCOMPARE(ps::Sampler::readSkewLimit(0), 0.1); // unusable interval: 1 s

    Fixture f;
    f.writeBase();
    f.write("sys/block/sda/size", "1000\n");
    f.mkdir("sys/block/sda/device");
    f.write("sys/class/net/eth0/ifindex", "2\n");
    f.write("sys/class/net/eth0/type", "1\n");
    f.write("sys/class/net/eth0/operstate", "up\n");
    f.mkdir("sys/class/net/eth0/device");
    f.write("proc/42/status", "Name:\tapp\nUid:\t0\t0\t0\t0\n");
    f.write("proc/42/cmdline", "app"s + '\0');

    // sample() reads the stopwatch at its start, right after the last
    // counter read and at its end; readOffset is the middle reading.
    int calls = 0;
    double readOffset = 0;
    auto o = f.options(true);
    o.stopwatch = [&] {
        const int k = calls++;
        const double base = 100.0 * (k / 3);
        switch (k % 3) {
        case 0: return base;
        case 1: return base + readOffset;
        default: return base + readOffset + 0.002;
        }
    };
    ps::Sampler smp(o);
    auto sampleAt = [&](int step, double offset) {
        readOffset = offset;
        const auto n = static_cast<uint64_t>(step);
        f.write("proc/stat", "cpu " + std::to_string(10 * n) + " 0 0 " + std::to_string(100 + 90 * n)
                                 + " 0 0 0 0 0 0\ncpu0 " + std::to_string(10 * n) + " 0 0 "
                                 + std::to_string(100 + 90 * n) + " 0 0 0 0 0 0\n");
        f.write("proc/diskstats", diskLine("sda", 0, 2 * n));
        f.write("proc/net/dev", "Inter-|\n face |\neth0: " + std::to_string(1000 * n)
                                    + " 1 0 0 0 0 0 0 " + std::to_string(500 * n) + " 1 0 0 0 0 0 0\n");
        f.write("proc/42/stat", pidStat(42, 10 * n, 0));
        f.write("proc/42/io", "read_bytes: " + std::to_string(4096 * n) + "\nwrite_bytes: 0\n");
        return smp.sample(step, 1);
    };
    auto cpuOf42 = [](const ps::Snapshot& s) {
        for (const auto& p : s.processes)
            if (p.pid == 42)
                return p.cpuPercent;
        return -1.0;
    };
    auto skewNoted = [](const ps::Snapshot& s) {
        for (const auto& p : s.problems)
            if (p.find("counter reads were") != std::string::npos)
                return true;
        return false;
    };
    auto expectRates = [&](const ps::Snapshot& s, bool valid, const char* what) {
        QVERIFY2(s.disks.size() == 1 && s.interfaces.size() == 1, what);
        QCOMPARE(ps::isValid(s.cpu.total), true); // tick ratios stay valid either way
        QCOMPARE(skewNoted(s), !valid);
        if (valid) {
            QCOMPARE(s.disks[0].readBps, 1024.0);
            QCOMPARE(s.interfaces[0].rxBps, 1000.0);
            QCOMPARE(s.interfaces[0].txBps, 500.0);
            QCOMPARE(s.diskTotal.a, 1024.0);
            QCOMPARE(s.netTotal.a, 1000.0);
            QCOMPARE(cpuOf42(s), 10.0);
        } else {
            QVERIFY2(!ps::isValid(s.disks[0].readBps) && !ps::isValid(s.disks[0].writeBps)
                         && !ps::isValid(s.disks[0].busyPercent),
                     what);
            QVERIFY2(!ps::isValid(s.interfaces[0].rxBps) && !ps::isValid(s.interfaces[0].txBps), what);
            QVERIFY2(!ps::isValid(s.diskTotal.a) && !ps::isValid(s.diskTotal.b), what);
            QVERIFY2(!ps::isValid(s.netTotal.a) && !ps::isValid(s.netTotal.b), what);
            QVERIFY2(!ps::isValid(cpuOf42(s)), what);
            QVERIFY(s.continuous); // CPU history stays connected
        }
    };

    const auto first = sampleAt(1, 0.010);
    QVERIFY(!first.continuous && !skewNoted(first)); // no baseline yet: nothing to judge
    expectRates(sampleAt(2, 0.030), true, "steady reads");           // 20 ms drift is tolerated
    const auto late = sampleAt(3, 0.330);                             // 300 ms late: rates would be inflated
    expectRates(late, false, "late reads");
    bool mentionsMs = false;
    for (const auto& p : late.problems)
        mentionsMs = mentionsMs || p.find("300 ms off") != std::string::npos;
    QVERIFY(mentionsMs);
    expectRates(sampleAt(4, 0.330), true, "consistently late reads"); // both ends shifted alike
    expectRates(sampleAt(5, 0.030), false, "early reads");
    expectRates(sampleAt(6, 0.030), true, "recovered");
    QCOMPARE(calls, 18);
}

void TestCore::injectedProbeMountIdIsVerified()
{
    Fixture f;
    f.writeBase();
    f.write("proc/self/mountinfo", mountLine(1, "8:2", "/", "/", "ext4", "/dev/sda2")
                                       + mountLine(7, "8:1", "/", "/data", "ext4", "/dev/sda1"));
    for (const int reported : {7, 99}) {
        auto o = f.options();
        o.statFs = [reported](const std::string& path, int&) -> std::optional<ps::FsStats> {
            ps::FsStats st{1000, 500, 400, 4096};
            st.hasMountId = true;
            st.mountId = path == "/data" ? reported : 1;
            return st;
        };
        ps::Sampler smp(o);
        const auto s = smp.sample(1, 1);
        const ps::Filesystem* data = nullptr;
        for (const auto& fsys : s.filesystems)
            if (fsys.source == "/dev/sda1")
                data = &fsys;
        QVERIFY(data);
        if (reported == 7) {
            QCOMPARE(data->totalBytes, 4096000.0);
        } else {
            QVERIFY(!ps::isValid(data->totalBytes));
            QCOMPARE(data->error, "mount changed while measuring"s);
        }
    }
}

void TestCore::productionProbeVerifiesMountIdentity()
{
    // Read-only host check with the default probe: the real root mount is
    // accepted with its true identity (this is a btrfs subvolume on many
    // hosts, whose st_dev differs from mountinfo), and rejected when any part
    // of the requested identity is wrong.
    std::ifstream in("/proc/self/mountinfo");
    std::string line, rootLine;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string field[5];
        for (auto& x : field)
            ss >> x;
        if (field[4] == "/")
            rootLine = line;
    }
    if (rootLine.empty())
        QSKIP("no root entry in /proc/self/mountinfo");
    const auto parsed = ps::parse::parseMountInfo(rootLine + "\n");
    if (parsed.empty() || !ps::parse::isLocalCapacityFs(parsed.front()))
        QSKIP("host root is not a local block filesystem");
    const auto& root = parsed.front();

    struct Variant {
        const char* name;
        std::string line;
        bool valid;
    };
    const std::string dev = std::to_string(root.major) + ":" + std::to_string(root.minor);
    const Variant variants[] = {
        {"true identity", rootLine, true},
        {"other mount id", mountLine(root.id + 100000, dev, root.root, "/", root.fsType, root.source), false},
        {"other device", mountLine(root.id, "250:250", root.root, "/", root.fsType, root.source), false},
        {"other root", mountLine(root.id, dev, root.root + "/elsewhere", "/", root.fsType, root.source), false},
        {"other source", mountLine(root.id, dev, root.root, "/", root.fsType, root.source + "9"), false},
    };
    for (const auto& v : variants) {
        Fixture f;
        f.writeBase();
        f.write("proc/self/mountinfo", v.line + "\n");
        ps::Sampler smp(f.options()); // statFs left empty: production probe
        const auto s = smp.sample(1, 1);
        QVERIFY2(s.filesystems.size() == 1, v.name);
        const auto& fsys = s.filesystems.front();
        if (v.valid) {
            QVERIFY2(fsys.error.empty() && ps::isValid(fsys.totalBytes) && fsys.totalBytes > 0,
                     qPrintable(QStringLiteral("%1: \"%2\"").arg(v.name, QString::fromStdString(fsys.error))));
        } else {
            QVERIFY2(!ps::isValid(fsys.totalBytes), v.name);
            QVERIFY2(fsys.error == "mount changed while measuring",
                     qPrintable(QStringLiteral("%1: \"%2\"").arg(v.name, QString::fromStdString(fsys.error))));
        }
    }
}

QTEST_APPLESS_MAIN(TestCore)
#include "tst_core.moc"
