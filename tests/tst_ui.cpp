// Main window, process model and CSV export, offscreen. Most cases feed
// synthetic snapshots through MainWindow::applySnapshot (no collector), so
// they are deterministic; the collector cases use a synthetic /proc tree or
// only assert invariants that hold on any host.
#include "app/cli.h"
#include "app/mainwindow.h"
#include "app/metricbutton.h"
#include "app/processmodel.h"
#include "app/timechart.h"
#include "core/format.h"
#include "core/history.h"
#include "core/snapshot.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QStandardPaths>
#include <QTableView>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
const QString kDash = QStringLiteral("\u2014");

ps::ProcessInfo proc(int pid, const char* name, double cpu, double rss, bool kernel = false)
{
    ps::ProcessInfo p;
    p.pid = pid;
    p.ppid = 1;
    p.startTicks = 1000 + static_cast<uint64_t>(pid);
    p.uid = 1000;
    p.uidKnown = true;
    p.user = "dev";
    p.kernelThread = kernel;
    p.state = 'S';
    p.threads = 2;
    p.name = name;
    p.command = kernel ? "[" + std::string(name) + "]" : "/usr/bin/" + std::string(name) + " --flag";
    p.cpuPercent = cpu;
    p.rssBytes = rss;
    p.ageSeconds = 125;
    return p;
}

// A plausible two-device system. `continuous` false is a first sample
// (warming up): every rate and CPU value is unavailable, as the sampler
// reports it.
std::shared_ptr<ps::Snapshot> snapshot(double time, bool continuous, double load = 25)
{
    static uint64_t seq = 0;
    auto s = std::make_shared<ps::Snapshot>();
    s->sequence = ++seq;
    s->time = time;
    s->interval = 1.0;
    s->continuous = continuous;
    s->collectMs = 2.5;
    s->logicalCpus = 4;
    auto rate = [&](double v) { return continuous ? v : ps::kNaN; };
    s->cpu.total = rate(load);
    s->cpu.user = rate(load * 0.6);
    s->cpu.system = rate(load * 0.3);
    s->cpu.iowait = rate(load * 0.1);
    s->cpu.steal = rate(0);
    for (int i = 0; i < 4; ++i) {
        ps::CoreUsage c;
        c.index = i;
        c.online = true;
        c.usage.total = rate(10.0 * (i + 1));
        s->cores.push_back(c);
    }
    s->load1 = 0.5;
    s->load5 = 0.4;
    s->load15 = 0.3;

    auto& m = s->memory;
    m.valid = true;
    m.total = 16 * GiB;
    m.used = 6 * GiB;
    m.available = 10 * GiB;
    m.free = 4 * GiB;
    m.cache = 5 * GiB;
    m.shared = 0.5 * GiB;
    m.hasSwap = true;
    m.swapTotal = 8 * GiB;
    m.swapUsed = 1 * GiB;
    m.swapFree = 7 * GiB;

    ps::DiskIo nvme;
    nvme.name = "nvme0n1";
    nvme.sizeBytes = 512e9;
    nvme.readBps = rate(4 * 1024 * 1024);
    nvme.writeBps = rate(1024 * 1024);
    nvme.busyPercent = rate(3);
    ps::DiskIo dm;
    dm.name = "dm\\x2d0"; // escaped by the sampler; must stay an opaque key
    dm.label = "cryptroot";
    dm.kind = ps::DeviceKind::Virtual;
    dm.sizeBytes = 500e9;
    dm.readBps = rate(4 * 1024 * 1024);
    dm.writeBps = rate(1024 * 1024);
    s->disks = {nvme, dm};
    s->diskTotal = {"all physical disks", 1, nvme.readBps, nvme.writeBps};

    ps::NetIo lo;
    lo.name = "lo";
    lo.kind = ps::DeviceKind::Loopback;
    lo.operState = "unknown";
    lo.rxBps = rate(100);
    lo.txBps = rate(100);
    ps::NetIo wifi;
    wifi.name = "wlp0s20f3";
    wifi.operState = "up";
    wifi.rxBps = rate(123456789);
    wifi.txBps = rate(98765432);
    s->interfaces = {lo, wifi};
    s->netTotal = {"all physical interfaces", 1, wifi.rxBps, wifi.txBps};

    ps::Filesystem root;
    root.source = "/dev/nvme0n1p3";
    root.fsType = "btrfs";
    root.mountPoints = {"/", "/home"};
    root.totalBytes = 500 * GiB;
    root.usedBytes = 200 * GiB;
    root.availBytes = 290 * GiB;
    ps::Filesystem efi;
    efi.source = "/dev/nvme0n1p1";
    efi.fsType = "vfat";
    efi.mountPoints = {"/efi"};
    efi.error = "not responding";
    s->filesystems = {root, efi};

    s->temperatures = {
        {"hwmon.coretemp.temp1", "coretemp", "Package id 0", "coretemp Package id 0",
         "/sys/class/hwmon/hwmon5/temp1_input", 55.0},
        {"hwmon.nvme.temp1", "nvme", "Composite", "nvme Composite", "/sys/class/hwmon/hwmon2/temp1_input", 41.0},
    };

    s->processesScanned = true;
    s->processes = {
        proc(1, "systemd", rate(0.0), 12e6),
        proc(2, "kthreadd", rate(0.0), ps::kNaN, true),
        proc(500, "bash", rate(3.5), 5e6),
        proc(4242, "Web Content (x)", rate(150.0), 900e6),
        proc(777, "fire) fox", ps::kNaN, 300e6), // new process: no CPU baseline yet
    };
    s->threadCount = 42;
    return s;
}

// Isolated settings file per window.
struct Env {
    QTemporaryDir dir;
    QString settings() const { return dir.filePath(QStringLiteral("pulsescope.ini")); }
    MainWindowOptions options() const
    {
        MainWindowOptions o;
        o.startCollector = false;
        o.settingsFile = settings();
        return o;
    }
};

template <typename T>
T* child(QWidget& w, const char* name)
{
    return w.findChild<T*>(QString::fromLatin1(name));
}

QString factText(QWidget& w, const char* name)
{
    auto* l = child<QLabel>(w, name);
    return l ? l->text() : QStringLiteral("<missing %1>").arg(QLatin1String(name));
}

QStringList comboData(QComboBox* c)
{
    QStringList out;
    for (int i = 0; i < c->count(); ++i)
        out << c->itemData(i).toString();
    return out;
}

QList<int> pidOrder(QTableView* table)
{
    QList<int> out;
    auto* m = table->model();
    for (int r = 0; r < m->rowCount(); ++r)
        out << m->index(r, ProcessModel::Pid).data(ProcessModel::SortRole).toInt();
    return out;
}

void feed(MainWindow& w, double t0, int n, double step = 1.0)
{
    w.applySnapshot(snapshot(t0, false));
    for (int i = 1; i < n; ++i)
        w.applySnapshot(snapshot(t0 + i * step, true, 20 + i));
}

QString artifactDir()
{
    QString dir = qEnvironmentVariable("PULSESCOPE_ARTIFACT_DIR");
    if (dir.isEmpty())
        dir = QDir::temp().filePath(QStringLiteral("pulsescope-artifacts"));
    QDir().mkpath(dir);
    return dir;
}

// Writer that blocks until released; its state outlives an abandoned job.
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    void release()
    {
        std::lock_guard lock(m);
        released = true;
        cv.notify_all();
    }
    bool waitEntered(int ms)
    {
        std::unique_lock lock(m);
        return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return entered; });
    }
};

CsvExporter::Writer blockingWriter(const std::shared_ptr<Gate>& gate)
{
    return [gate](const std::string& data, const QString& path, QString* error) {
        {
            std::unique_lock lock(gate->m);
            gate->entered = true;
            gate->cv.notify_all();
            gate->cv.wait_for(lock, std::chrono::seconds(20), [&] { return gate->released; });
        }
        return CsvExporter::saveAtomically(data, path, error);
    };
}

// Minimal synthetic /proc + /sys tree for a real Collector.
class Tree {
public:
    std::string path(const std::string& rel) const { return m_dir.path().toStdString() + "/" + rel; }
    void write(const std::string& rel, const std::string& content) const
    {
        const std::filesystem::path p = path(rel);
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary | std::ios::trunc) << content;
    }
    void process(int pid, const std::string& comm, uint32_t uid) const
    {
        const std::string base = "proc/" + std::to_string(pid) + "/";
        write(base + "status", "Name:\t" + comm + "\nUid:\t" + std::to_string(uid) + "\t" + std::to_string(uid) + "\t"
                                   + std::to_string(uid) + "\t" + std::to_string(uid) + "\n");
        write(base + "cmdline", comm + std::string(1, '\0'));
        // stat last: the sampler skips a PID until its stat file parses.
        write(base + "stat", std::to_string(pid) + " (" + comm + ") S 1 " + std::to_string(pid) + " "
                                 + std::to_string(pid)
                                 + " 0 -1 4194560 0 0 0 0 10 5 0 0 20 0 1 0 100 1000000 456 "
                                   "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
    }
    void base() const
    {
        write("proc/stat", "cpu 100 0 50 1000 0 0 0 0 0 0\ncpu0 100 0 50 1000 0 0 0 0 0 0\n");
        write("proc/meminfo", "MemTotal: 1000000 kB\nMemFree: 100000 kB\nMemAvailable: 500000 kB\n"
                              "Buffers: 1000 kB\nCached: 2000 kB\nSReclaimable: 500 kB\nShmem: 100 kB\n"
                              "SwapTotal: 0 kB\nSwapFree: 0 kB\n");
        write("proc/loadavg", "0.10 0.20 0.30 1/10 99\n");
        write("proc/uptime", "1000.00 0.00\n");
        write("proc/diskstats", "");
        write("proc/net/dev", "Inter-| Receive | Transmit\n face |bytes packets|bytes packets\n");
        write("proc/mounts", "");
        write("proc/self/mountinfo", "");
        std::filesystem::create_directories(path("sys/block"));
        std::filesystem::create_directories(path("sys/class/net"));
        process(100, "init", 0);
    }

private:
    QTemporaryDir m_dir;
};

} // namespace

class TestUi : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void startsEmptyAndHonest();
    void pagesAndShortcuts();
    void warmupThenLiveValues();
    void deviceSelectorsUseOpaqueKeys();
    void networkNavKeepsFullTextInTooltip();
    void networkAggregateLabelsMatchSelector();
    void navSparklineNeverOverlapsValue();
    void narrowWindowReflowsWithoutClipping_data();
    void narrowWindowReflowsWithoutClipping();
    void factRowsGetTheirHeight_data();
    void factRowsGetTheirHeight();
    void sensorListNamesAndRemovedPlaceholder();
    void negativeTemperaturesRender();
    void swapUnknownNoneAndKnown();
    void pauseFreezesAndResumeLeavesGap();
    void intervalAndRangeChangesKeepHistory();
    void processTableSortsAndFilters();
    void processModelStaleKeepsRowsHidesValues();
    void csvExportWritesUnitsAndBlanks();
    void csvExportFailureIsReported();
    void csvExportBusyThenCompletes();
    void csvExportShutdownIsBounded();
    void historyLimitIsExplained();
    void settingsPersist();
    void blockedCollectorGoesStaleAndRecovers();
    void freshnessFollowsAcquisitionClock();
    void chartWithoutSamplesInWindow();
    void liveCollectorPopulatesAndShutsDown();
    void cliWindowSizeIsValidated();
};

void TestUi::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("PulseScopeTest"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_ui"));
}

void TestUi::startsEmptyAndHonest()
{
    Env env;
    MainWindow w(env.options());
    QVERIFY(w.history().empty());
    QCOMPARE(w.statusText(), QStringLiteral("Not collecting"));
    for (const char* id : {"navCpu", "navMemory", "navDisks", "navNetwork", "navTemperatures", "navProcesses"}) {
        auto* b = child<MetricButton>(w, id);
        QVERIFY2(b, id);
        QCOMPARE(b->valueText(), kDash);
    }
    QCOMPARE(factText(w, "cpu_total"), kDash);
    QCOMPARE(factText(w, "mem_used"), kDash);
    QString err;
    QVERIFY(!w.startCsvExport(QDir::temp().filePath(QStringLiteral("never.csv")), &err));
    QVERIFY(err.contains(QStringLiteral("No samples")));
    QCOMPARE(child<TimeChart>(w, "cpuChart")->accessibleDescription(), QStringLiteral("Waiting for the first sample"));
    QVERIFY(w.shutdown(1000));
}

void TestUi::pagesAndShortcuts()
{
    Env env;
    MainWindow w(env.options());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    feed(w, 100, 3);
    for (int p = 0; p < MainWindow::PageCount; ++p) {
        w.showPage(static_cast<MainWindow::Page>(p));
        QCOMPARE(int(w.currentPage()), p);
        QApplication::processEvents();
    }
    // Ctrl+1..6 switch pages; navigation buttons are checkable and exclusive.
    for (int p = 0; p < MainWindow::PageCount; ++p) {
        QTest::keyClick(&w, static_cast<Qt::Key>(Qt::Key_1 + p), Qt::ControlModifier);
        QCOMPARE(int(w.currentPage()), p);
    }
    auto* navCpu = child<MetricButton>(w, "navCpu");
    navCpu->click();
    QCOMPARE(w.currentPage(), MainWindow::CpuPage);
    QVERIFY(navCpu->isChecked());
    QVERIFY(!child<MetricButton>(w, "navProcesses")->isChecked());
    // Ctrl+F focuses the process filter from any page.
    QTest::keyClick(&w, Qt::Key_F, Qt::ControlModifier);
    QCOMPARE(w.currentPage(), MainWindow::ProcessPage);
    QVERIFY(child<QLineEdit>(w, "processFilter")->hasFocus());
    // Every navigation button has an accessible name and a descriptive tooltip.
    for (auto* b : w.findChildren<MetricButton*>()) {
        QVERIFY(!b->accessibleName().isEmpty());
        QVERIFY(b->toolTip().startsWith(b->accessibleName()));
    }
    w.close();
}

void TestUi::warmupThenLiveValues()
{
    Env env;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(10, false));
    QVERIFY(w.statusText().startsWith(QStringLiteral("Warming up")));
    QCOMPARE(child<MetricButton>(w, "navCpu")->valueText(), QString::fromUtf8("Measuring\u2026"));
    QCOMPARE(child<MetricButton>(w, "navDisks")->valueText(), QString::fromUtf8("Measuring\u2026"));
    QCOMPARE(factText(w, "cpu_total"), kDash);
    // Memory needs no baseline.
    QVERIFY(child<MetricButton>(w, "navMemory")->valueText().contains(QStringLiteral("6.00 GiB")));

    w.applySnapshot(snapshot(11, true, 25));
    QVERIFY(w.statusText().startsWith(QStringLiteral("Live, sampling every 1 s")));
    QVERIFY(factText(w, "cpu_total").contains(QStringLiteral("25")));
    QVERIFY(child<MetricButton>(w, "navCpu")->valueText().contains(QStringLiteral("25")));
    QVERIFY(factText(w, "mem_used").contains(QStringLiteral("6.00 GiB")));
    QVERIFY(factText(w, "mem_cache").contains(QStringLiteral("5.00 GiB")));
    QCOMPARE(factText(w, "cpu_count"), QStringLiteral("4 / 4"));
    QCOMPARE(factText(w, "cpu_tasks"), QStringLiteral("5 / 42"));
    QVERIFY(factText(w, "disk_read").contains(QStringLiteral("4.00 MiB/s")));
    QCOMPARE(w.history().size(), std::size_t(2));
    QVERIFY(!w.history().continuous(0));
    QVERIFY(w.history().continuous(1));
    QCOMPARE(w.snapshotsApplied(), 2);
    QVERIFY(child<QLabel>(w, "timingLabel")->text().contains(QStringLiteral("2.5 ms")));

    // Filesystems: capacity where readable, the reason where not.
    auto* fsTable = child<QTreeWidget>(w, "filesystemTable");
    QCOMPARE(fsTable->topLevelItemCount(), 2);
    QCOMPARE(fsTable->topLevelItem(0)->text(0), QStringLiteral("/, /home"));
    QCOMPARE(fsTable->topLevelItem(0)->text(3), QStringLiteral("500 GiB"));
    QCOMPARE(fsTable->topLevelItem(1)->text(3), kDash);
    QCOMPARE(fsTable->topLevelItem(1)->text(6), QStringLiteral("not responding"));

    // Charts paint real points.
    w.resize(1180, 780);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* cpu = child<TimeChart>(w, "cpuChart");
    QVERIFY(cpu->hasVisibleData());
    cpu->grab();
    QVERIFY(cpu->lastPaintedPoints() >= 1);
    w.close();
}

void TestUi::deviceSelectorsUseOpaqueKeys()
{
    Env env;
    MainWindow w(env.options());
    feed(w, 50, 3);
    auto* disks = child<QComboBox>(w, "diskSelector");
    QCOMPARE(comboData(disks), (QStringList {QString(), QStringLiteral("nvme0n1"), QStringLiteral("dm\\x2d0")}));
    QVERIFY(disks->itemText(0).startsWith(QStringLiteral("All physical disks (1)")));
    QVERIFY(disks->itemText(2).contains(QStringLiteral("virtual")));
    auto* diskChart = child<TimeChart>(w, "diskChart");
    QCOMPARE(diskChart->series().front().key, std::string(ps::keys::diskAllRead));

    // Choose the device-mapper device through the widget, as a user would.
    disks->setCurrentIndex(2);
    Q_EMIT disks->activated(2);
    QCOMPARE(diskChart->series().front().key, ps::keys::diskRead("dm\\x2d0"));
    QVERIFY(w.history().find(diskChart->series().front().key));
    QVERIFY(diskChart->hasVisibleData());
    QCOMPARE(child<MetricButton>(w, "navDisks")->detailText(), QStringLiteral("dm\\x2d0"));

    // The chosen device disappears: it stays selectable as "not present".
    auto gone = snapshot(53, true);
    gone->disks.pop_back();
    w.applySnapshot(gone);
    QCOMPARE(disks->currentData().toString(), QStringLiteral("dm\\x2d0"));
    QVERIFY(disks->currentText().contains(QStringLiteral("not present")));
    // Its past samples stay visible; the current value is unavailable.
    QCOMPARE(diskChart->accessibleDescription(), QStringLiteral("Read n/a, Write n/a"));
    // ...and returns to the same selection when it reappears.
    w.applySnapshot(snapshot(54, true));
    QVERIFY(!disks->currentText().contains(QStringLiteral("not present")));

    auto* nets = child<QComboBox>(w, "netSelector");
    QCOMPARE(comboData(nets), (QStringList {QString(), QStringLiteral("lo"), QStringLiteral("wlp0s20f3")}));
    QVERIFY(nets->itemText(0).contains(QStringLiteral("no loopback")));
    QVERIFY(nets->itemText(1).contains(QStringLiteral("loopback")));
    auto* netChart = child<TimeChart>(w, "netChart");
    QCOMPARE(netChart->series().front().key, std::string(ps::keys::netAllRx));
    nets->setCurrentIndex(1);
    Q_EMIT nets->activated(1);
    QCOMPARE(netChart->series().front().key, ps::keys::netRx("lo"));
    QCOMPARE(factText(w, "net_kind"), QStringLiteral("loopback"));
}

void TestUi::networkNavKeepsFullTextInTooltip()
{
    Env env;
    MainWindow w(env.options());
    feed(w, 10, 2);
    w.resize(852, 600);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* net = child<MetricButton>(w, "navNetwork");
    // The painted value may be elided in a narrow window; the tooltip and the
    // accessible description always carry both full rates.
    QVERIFY(net->valueText().contains(QString::fromUtf8("\u2193")));
    QVERIFY(net->valueText().contains(QString::fromUtf8("\u2191")));
    QVERIFY2(net->toolTip().contains(QStringLiteral("receive 118 MiB/s")), qPrintable(net->toolTip()));
    QVERIFY2(net->toolTip().contains(QStringLiteral("transmit 94.2 MiB/s")), qPrintable(net->toolTip()));
    QVERIFY(net->accessibleDescription().contains(QStringLiteral("transmit")));
    w.close();
}

void TestUi::networkAggregateLabelsMatchSelector()
{
    Env env;
    MainWindow w(env.options());
    feed(w, 10, 2);
    auto* chart = child<TimeChart>(w, "netChart");
    auto* nav = child<MetricButton>(w, "navNetwork");
    QCOMPARE(chart->title(), QStringLiteral("Network throughput: all physical interfaces"));
    QCOMPARE(nav->detailText(), QStringLiteral("All physical interfaces"));
    QCOMPARE(factText(w, "net_kind"), QStringLiteral("sum of 1 physical interface"));

    // A host without physical NICs (a container): the collector falls back
    // to all non-loopback interfaces, and every label follows it.
    auto s = snapshot(12, true);
    ps::NetIo veth;
    veth.name = "veth0";
    veth.kind = ps::DeviceKind::Virtual;
    veth.rxBps = 10;
    veth.txBps = 20;
    ps::NetIo tun;
    tun.name = "tun0";
    tun.kind = ps::DeviceKind::Virtual;
    tun.rxBps = 1;
    tun.txBps = 2;
    s->interfaces = {s->interfaces.front(), veth, tun};
    s->netTotal = {"all non-loopback interfaces", 2, 11, 22};
    w.applySnapshot(s);
    QCOMPARE(chart->title(), QStringLiteral("Network throughput: all non-loopback interfaces"));
    QCOMPARE(nav->detailText(), QStringLiteral("All non-loopback interfaces"));
    QCOMPARE(factText(w, "net_kind"), QStringLiteral("sum of 2 non-loopback interfaces"));
}

void TestUi::navSparklineNeverOverlapsValue()
{
    ps::History h;
    for (int i = 0; i < 30; ++i) {
        QVERIFY(h.beginSample(100.0 + i, i > 0));
        h.set("k", "K", ps::Unit::BytesPerSecond, 1000.0 * (i % 5 + 1), ps::SeriesGroup::Summary);
        h.endSample();
    }
    // Rows touched by the sparkline, found by its series colour.
    auto sparkRows = [](MetricButton& b) {
        const QImage img = b.grab().toImage().convertToFormat(QImage::Format_RGB32);
        const QColor c = seriesColor(0, b.palette());
        int top = img.height(), bottom = -1;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const QColor p = img.pixelColor(x, y);
                if (std::abs(p.red() - c.red()) + std::abs(p.green() - c.green()) + std::abs(p.blue() - c.blue()) < 30) {
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
        return std::pair {top, bottom};
    };
    for (const double scale : {1.0, 1.4}) {
        const QString wide = QStringLiteral("\u2193 1023 MiB/s  \u2191 1023 MiB/s");
        MetricButton b(QStringLiteral("Network"));
        QFont f = b.font();
        f.setPointSizeF(f.pointSizeF() * scale);
        b.setFont(f);
        b.setSparkline(&h, {"k"});
        b.setValueTemplate(QStringLiteral("\u2193 0000 MiB/s  \u2191 0000 MiB/s"));
        b.setValueText(wide);
        b.resize(b.sizeHint());
        // The hint fits the widest value in the compact (bold, base size) font.
        QFont bold = f;
        bold.setWeight(QFont::DemiBold);
        QVERIFY(b.sizeHint().width() >= QFontMetrics(bold).horizontalAdvance(wide) + 20);
        // Too wide to sit beside the value: the sparkline stays in the title row.
        const int valueTop = 7 + QFontMetrics(f).height();
        const auto [top, bottom] = sparkRows(b);
        QVERIFY2(bottom >= 0, "sparkline was not drawn");
        QVERIFY2(bottom < valueTop, qPrintable(QStringLiteral("scale %1: sparkline rows %2..%3, value row starts at %4")
                                                   .arg(scale).arg(top).arg(bottom).arg(valueTop)));

        // A short value leaves room, so the sparkline spans both rows.
        MetricButton c(QStringLiteral("CPU"));
        c.setFont(f);
        c.setSparkline(&h, {"k"});
        c.setValueTemplate(QStringLiteral("100.0 %"));
        c.setValueText(QStringLiteral("12.5 %"));
        c.resize(b.sizeHint());
        QVERIFY(sparkRows(c).second >= valueTop);
    }

    // Every navigation entry gets at least its preferred width.
    Env env;
    MainWindow w(env.options());
    feed(w, 10, 3);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    for (auto* nav : w.findChildren<MetricButton*>())
        QVERIFY2(nav->width() >= nav->sizeHint().width(), qPrintable(nav->objectName()));
    w.close();
}

void TestUi::narrowWindowReflowsWithoutClipping_data()
{
    QTest::addColumn<double>("fontScale");
    QTest::addColumn<int>("maxMinimumWidth");
    QTest::newRow("default font") << 1.0 << 800;
    QTest::newRow("large font") << 1.3 << 1000;
}

void TestUi::narrowWindowReflowsWithoutClipping()
{
    QFETCH(double, fontScale);
    QFETCH(int, maxMinimumWidth);
    const QFont appFont = QApplication::font();
    QFont big = appFont;
    big.setPointSizeF(appFont.pointSizeF() * fontScale);
    QApplication::setFont(big);
    struct Restore {
        QFont f;
        ~Restore() { QApplication::setFont(f); }
    } restore {appFont};

    Env env;
    MainWindow w(env.options());
    feed(w, 10, 3);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    // Tiling compositors and half-screen windows are the normal case for a
    // monitor beside an editor, so the minimum width must stay modest.
    const int minW = w.minimumSizeHint().width();
    qInfo("font %.0f%%: minimum window width %d px", fontScale * 100, minW);
    QVERIFY2(minW <= maxMinimumWidth, qPrintable(QStringLiteral("minimum width %1").arg(minW)));

    const QRegularExpression factName(QStringLiteral("^(cpu|mem|swap|disk|net)_"));
    auto clipped = [&] {
        QStringList out;
        for (auto* l : w.findChildren<QLabel*>()) {
            if (!l->isVisible() || l->wordWrap())
                continue;
            const bool fact = factName.match(l->objectName()).hasMatch();
            const bool name = l->buddy() != nullptr;
            const bool other = l->objectName() == QLatin1String("processCount");
            if ((fact || name || other) && l->width() < l->sizeHint().width())
                out << QStringLiteral("%1 '%2' %3<%4").arg(l->objectName(), l->text()).arg(l->width()).arg(l->sizeHint().width());
        }
        return out;
    };
    const auto y = [&](const char* name) { return child<QLabel>(w, name)->mapTo(&w, QPoint()).y(); };
    for (const int width : {minW, 1500}) {
        w.resize(width, 700);
        for (int p = 0; p < MainWindow::PageCount; ++p) {
            w.showPage(static_cast<MainWindow::Page>(p));
            QTest::qWait(20);
            const QStringList bad = clipped();
            QVERIFY2(bad.isEmpty(), qPrintable(QStringLiteral("width %1, page %2: ").arg(w.width()).arg(p) + bad.join(QStringLiteral("; "))));
        }
        w.showPage(MainWindow::MemoryPage);
        QTest::qWait(20);
        if (width == 1500) // wide: four pairs per row, as designed
            QCOMPARE(y("mem_total"), y("mem_cache"));
    }
    w.close();
}

void TestUi::factRowsGetTheirHeight_data()
{
    QTest::addColumn<double>("fontScale");
    QTest::newRow("default font") << 1.0;
    QTest::newRow("large font") << 1.3;
}

// The fact rows ("Total: 12 %") above the charts must get their full height
// in the real window, not just visible labels inside a squeezed parent.
void TestUi::factRowsGetTheirHeight()
{
    QFETCH(double, fontScale);
    const QFont appFont = QApplication::font();
    QFont big = appFont;
    big.setPointSizeF(appFont.pointSizeF() * fontScale);
    QApplication::setFont(big);
    struct Restore {
        QFont f;
        ~Restore() { QApplication::setFont(f); }
    } restore {appFont};

    Env env;
    MainWindow w(env.options());
    feed(w, 10, 3);
    w.resize(1180, 780);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    for (const int width : {1180, w.minimumSizeHint().width()}) {
        w.resize(width, 780);
        for (int p = 0; p < MainWindow::PageCount; ++p) {
            w.showPage(static_cast<MainWindow::Page>(p));
            QTest::qWait(20);
            const QString where = QStringLiteral("width %1, page %2: ").arg(w.width()).arg(p);
            int pairs = 0;
            for (auto* name : w.findChildren<QLabel*>()) {
                auto* value = qobject_cast<QLabel*>(name->buddy());
                if (!value || !name->isVisible())
                    continue;
                ++pairs;
                QWidget* grid = value->parentWidget();
                const QString what = where + value->objectName();
                QVERIFY2(grid->height() > 0, qPrintable(what + QStringLiteral(" grid height 0")));
                QVERIFY2(grid->height() >= grid->heightForWidth(grid->width()),
                         qPrintable(what + QStringLiteral(" grid %1 < %2").arg(grid->height()).arg(grid->heightForWidth(grid->width()))));
                for (QLabel* l : {name, value}) {
                    QVERIFY2(l->height() >= l->sizeHint().height(),
                             qPrintable(what + QStringLiteral(" '%1' height %2 < %3").arg(l->text()).arg(l->height()).arg(l->sizeHint().height())));
                    QVERIFY2(grid->rect().contains(l->geometry()), qPrintable(what + QStringLiteral(" '%1' outside its row").arg(l->text())));
                    QVERIFY2(!l->visibleRegion().isEmpty(), qPrintable(what + QStringLiteral(" '%1' not painted").arg(l->text())));
                }
            }
            if (p <= MainWindow::NetworkPage)
                QVERIFY2(pairs >= 4, qPrintable(where + QStringLiteral("%1 fact pairs").arg(pairs)));
        }
    }
    w.close();
}

void TestUi::sensorListNamesAndRemovedPlaceholder()
{
    Env env;
    MainWindow w(env.options());
    feed(w, 10, 2);
    auto* list = child<QTreeWidget>(w, "sensorList");
    QCOMPARE(list->topLevelItemCount(), 2);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("coretemp Package id 0"));
    QVERIFY(list->topLevelItem(0)->text(1).contains(QStringLiteral("55")));
    // Full name and sysfs path are recoverable even when the column elides.
    QVERIFY(list->topLevelItem(0)->toolTip(0).startsWith(QStringLiteral("coretemp Package id 0\n")));
    QVERIFY(list->topLevelItem(0)->toolTip(0).contains(QStringLiteral("/sys/class/hwmon/hwmon5/temp1_input")));
    QCOMPARE(list->textElideMode(), Qt::ElideMiddle);
    QVERIFY(qobject_cast<QSplitter*>(list->parentWidget()));
    // First run: readable sensors are plotted by default.
    auto* chart = child<TimeChart>(w, "temperatureChart");
    QCOMPARE(chart->series().size(), std::size_t(2));
    QCOMPARE(child<MetricButton>(w, "navTemperatures")->detailText(),
             QStringLiteral("Hottest of 2: coretemp Package id 0"));

    // Uncheck nvme, then unplug coretemp: it stays listed as removed, with
    // its check state, while its history is retained.
    list->topLevelItem(1)->setCheckState(0, Qt::Unchecked);
    QCOMPARE(chart->series().size(), std::size_t(1));
    auto unplugged = snapshot(12, true);
    unplugged->temperatures.erase(unplugged->temperatures.begin());
    w.applySnapshot(unplugged);
    QCOMPARE(list->topLevelItemCount(), 2);
    QTreeWidgetItem* removed = nullptr;
    for (int i = 0; i < list->topLevelItemCount(); ++i)
        if (list->topLevelItem(i)->data(0, Qt::UserRole).toString() == QStringLiteral("hwmon.coretemp.temp1"))
            removed = list->topLevelItem(i);
    QVERIFY(removed);
    QCOMPARE(removed->text(1), QStringLiteral("removed"));
    QCOMPARE(removed->text(0), QStringLiteral("coretemp Package id 0"));
    QCOMPARE(removed->checkState(0), Qt::Checked);
    QVERIFY(removed->toolTip(0).contains(QStringLiteral("no longer present")));
    QCOMPARE(chart->series().size(), std::size_t(1));
    QCOMPARE(chart->series().front().key, ps::keys::temperature("hwmon.coretemp.temp1"));
    // The navigation summary follows the remaining (hottest current) sensor.
    QCOMPARE(child<MetricButton>(w, "navTemperatures")->detailText(), QStringLiteral("Hottest of 1: nvme Composite"));

    // It comes back: same row identity, live value, still checked.
    w.applySnapshot(snapshot(13, true));
    QCOMPARE(list->topLevelItemCount(), 2);
    QVERIFY(list->topLevelItem(0)->text(1).contains(QStringLiteral("55")));
    QCOMPARE(list->topLevelItem(0)->checkState(0), Qt::Checked);
    QCOMPARE(list->topLevelItem(1)->checkState(0), Qt::Unchecked);

    // No sensors at all: an explanation, not an empty page.
    Env env2;
    MainWindow w2(env2.options());
    auto none = snapshot(20, false);
    none->temperatures.clear();
    none->temperatureNote = "No temperature sensors are exposed (no hwmon or thermal zones in /sys)";
    w2.applySnapshot(none);
    auto* note = child<QLabel>(w2, "temperatureNote");
    QVERIFY(!note->isHidden());
    QVERIFY(note->text().contains(QStringLiteral("hwmon")));
    QCOMPARE(child<MetricButton>(w2, "navTemperatures")->valueText(), QStringLiteral("Unavailable"));
    QCOMPARE(child<TimeChart>(w2, "temperatureChart")->accessibleDescription(), QStringLiteral("No temperature sensors"));
}

void TestUi::negativeTemperaturesRender()
{
    Env env;
    MainWindow w(env.options());
    for (int i = 0; i < 30; ++i) {
        auto s = snapshot(100 + i, i > 0);
        s->temperatures = {{"hwmon.probe.temp1", "probe", "Outdoor", "probe Outdoor", "/sys/x", -20.0 + i * 0.5}};
        w.applySnapshot(s);
    }
    auto* nav = child<MetricButton>(w, "navTemperatures");
    QVERIFY2(nav->valueText().contains(QStringLiteral("5.5")), qPrintable(nav->valueText()));
    QVERIFY(nav->valueText().startsWith(QLatin1Char('-')) || nav->valueText().startsWith(QChar(0x2212)));
    w.showPage(MainWindow::TemperaturePage);
    w.resize(1000, 700);
    const QImage img = w.grab().toImage();
    QVERIFY(!img.isNull());
    auto* chart = child<TimeChart>(w, "temperatureChart");
    QVERIFY(chart->hasVisibleData());
    const auto [lo, hi] = chart->displayedRange();
    QVERIFY2(lo <= -20 && hi >= -5.5, qPrintable(QStringLiteral("%1..%2").arg(lo).arg(hi)));
}

void TestUi::swapUnknownNoneAndKnown()
{
    Env env;
    MainWindow w(env.options());
    auto* swapChart = child<TimeChart>(w, "swapChart");
    auto* nav = child<MetricButton>(w, "navMemory");

    w.applySnapshot(snapshot(1, false));
    QVERIFY(factText(w, "swap_total").contains(QStringLiteral("8.00 GiB")));
    QVERIFY(factText(w, "swap_used").contains(QStringLiteral("1.00 GiB")));
    QVERIFY(nav->detailText().contains(QStringLiteral("swap 1.00 GiB")));

    auto none = snapshot(2, true);
    none->memory.hasSwap = false;
    none->memory.swapTotal = 0;
    none->memory.swapUsed = ps::kNaN;
    none->memory.swapFree = 0;
    w.applySnapshot(none);
    QCOMPARE(factText(w, "swap_total"), QStringLiteral("None configured"));
    QCOMPARE(factText(w, "swap_used"), QStringLiteral("None configured"));
    QVERIFY(nav->detailText().endsWith(QStringLiteral("no swap")));

    auto unknown = snapshot(3, true);
    unknown->memory.hasSwap = false;
    unknown->memory.swapTotal = ps::kNaN;
    unknown->memory.swapUsed = ps::kNaN;
    unknown->memory.swapFree = ps::kNaN;
    w.applySnapshot(unknown);
    QCOMPARE(factText(w, "swap_total"), kDash);
    QCOMPARE(factText(w, "swap_used"), kDash);
    QVERIFY(nav->detailText().endsWith(QStringLiteral("swap unknown")));

    // Fresh window with only "no swap" samples: the chart explains why it is empty.
    Env env2;
    MainWindow w2(env2.options());
    w2.applySnapshot(none);
    QCOMPARE(child<TimeChart>(w2, "swapChart")->accessibleDescription(), QStringLiteral("No swap is configured"));
    Env env3;
    MainWindow w3(env3.options());
    w3.applySnapshot(unknown);
    QVERIFY(child<TimeChart>(w3, "swapChart")->accessibleDescription().contains(QStringLiteral("SwapTotal")));
    Q_UNUSED(swapChart);
}

void TestUi::pauseFreezesAndResumeLeavesGap()
{
    Env env;
    MainWindow w(env.options());
    feed(w, 100, 3);
    auto* pause = w.findChild<QAction*>(QStringLiteral("pauseAction"));
    QVERIFY(pause);
    pause->trigger();
    QVERIFY(w.isPaused());
    QCOMPARE(pause->text(), QStringLiteral("Resume"));
    QVERIFY(w.statusText().startsWith(QStringLiteral("Paused")));
    for (auto* chart : w.findChildren<TimeChart*>())
        QCOMPARE(chart->notice(), QStringLiteral("Paused"));
    const QString frozen = factText(w, "cpu_total");
    // Paused: deliveries are ignored, nothing changes.
    w.applySnapshot(snapshot(103, true, 90));
    QCOMPARE(w.snapshotsApplied(), 3);
    QCOMPARE(w.history().size(), std::size_t(3));
    QCOMPARE(factText(w, "cpu_total"), frozen);

    // Keyboard shortcut P resumes.
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTest::keyClick(&w, Qt::Key_P);
    QVERIFY(!w.isPaused());
    QCOMPARE(pause->text(), QStringLiteral("Pause"));
    for (auto* chart : w.findChildren<TimeChart*>())
        QVERIFY(chart->notice().isEmpty());
    // The collector's first sample after resume is a new baseline: a gap.
    w.applySnapshot(snapshot(130, false));
    w.applySnapshot(snapshot(131, true, 40));
    QCOMPARE(w.history().size(), std::size_t(5));
    QVERIFY(!w.history().continuous(3));
    QVERIFY(w.history().continuous(4));
    QVERIFY(!ps::isValid(w.history().value(*w.history().find(ps::keys::cpuTotal), 3)));
    QVERIFY(factText(w, "cpu_total").contains(QStringLiteral("40")));
    w.close();
}

void TestUi::intervalAndRangeChangesKeepHistory()
{
    Env env;
    MainWindow w(env.options());
    feed(w, 1000, 10);
    auto* interval = child<QComboBox>(w, "intervalCombo");
    auto* range = child<QComboBox>(w, "rangeCombo");
    QCOMPARE(w.intervalMs(), 1000);
    QCOMPARE(w.windowSeconds(), 300);

    const int twoSec = interval->findData(2000);
    interval->setCurrentIndex(twoSec);
    Q_EMIT interval->activated(twoSec);
    QCOMPARE(w.intervalMs(), 2000);
    w.setIntervalMs(1234); // not a choice: ignored
    QCOMPARE(w.intervalMs(), 2000);
    w.setIntervalMs(500);
    QCOMPARE(interval->currentData().toInt(), 500);
    QVERIFY(w.statusText().contains(QStringLiteral("0.5 s")));

    const int oneMin = range->findData(60);
    range->setCurrentIndex(oneMin);
    Q_EMIT range->activated(oneMin);
    QCOMPARE(w.windowSeconds(), 60);
    for (auto* chart : w.findChildren<TimeChart*>())
        QCOMPARE(chart->windowSeconds(), 60.0);
    w.setWindowSeconds(61); // not a choice: ignored
    QCOMPARE(w.windowSeconds(), 60);
    w.setWindowSeconds(900);
    QCOMPARE(range->currentData().toInt(), 900);

    // Neither change touches the recorded samples; later samples at a new
    // cadence continue the same history.
    QCOMPARE(w.history().size(), std::size_t(10));
    w.applySnapshot(snapshot(1009.5, true));
    w.applySnapshot(snapshot(1010, true));
    QCOMPARE(w.history().size(), std::size_t(12));
    QVERIFY(w.history().continuous(11));
}

void TestUi::processTableSortsAndFilters()
{
    Env env;
    MainWindow w(env.options());
    feed(w, 10, 2);
    w.showPage(MainWindow::ProcessPage);
    auto* table = child<QTableView>(w, "processTable");
    auto* filter = child<QLineEdit>(w, "processFilter");
    auto* kernel = child<QCheckBox>(w, "kernelThreadsCheck");
    auto* count = child<QLabel>(w, "processCount");
    QVERIFY(!kernel->isChecked());
    QCOMPARE(table->model()->rowCount(), 4); // kthreadd hidden
    QCOMPARE(count->text(), QStringLiteral("Showing 4 of 5 processes, 42 threads"));

    // Default: CPU descending; numbers, not text; unavailable last.
    QCOMPARE(pidOrder(table), (QList<int> {4242, 500, 1, 777}));
    table->sortByColumn(ProcessModel::Cpu, Qt::AscendingOrder);
    QCOMPARE(pidOrder(table), (QList<int> {777, 1, 500, 4242}));
    table->sortByColumn(ProcessModel::Memory, Qt::DescendingOrder);
    QCOMPARE(pidOrder(table), (QList<int> {4242, 777, 1, 500}));
    table->sortByColumn(ProcessModel::Pid, Qt::AscendingOrder);
    QCOMPARE(pidOrder(table), (QList<int> {1, 500, 777, 4242}));
    // CPU above 100 % is displayed as measured (one core = 100 %).
    const QModelIndex hot = table->model()->index(3, ProcessModel::Cpu);
    QCOMPARE(hot.data().toString(), QStringLiteral("150.0"));
    QVERIFY(table->model()->headerData(ProcessModel::Cpu, Qt::Horizontal, Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("100")));
    // Names with spaces and parentheses are shown intact.
    QCOMPARE(table->model()->index(3, ProcessModel::Name).data().toString(), QStringLiteral("Web Content (x)"));
    QCOMPARE(table->model()->index(2, ProcessModel::Name).data().toString(), QStringLiteral("fire) fox"));

    QTest::keyClicks(filter, QStringLiteral("FOX"));
    QCOMPARE(pidOrder(table), (QList<int> {777}));
    QCOMPARE(count->text(), QStringLiteral("Showing 1 of 5 processes, 42 threads"));
    filter->clear();
    filter->setText(QStringLiteral("4242"));
    QCOMPARE(pidOrder(table), (QList<int> {4242}));
    filter->setText(QStringLiteral("(x)"));
    QCOMPARE(pidOrder(table), (QList<int> {4242}));
    filter->setText(QStringLiteral("/usr/bin/bash"));
    QCOMPARE(pidOrder(table), (QList<int> {500}));
    filter->clear();
    kernel->setChecked(true);
    QCOMPARE(table->model()->rowCount(), 5);
    QCOMPARE(count->text(), QStringLiteral("Showing 5 of 5 processes, 42 threads"));

    // Refresh with an exited and a reused PID: rows follow (PID, start time).
    auto next = snapshot(12, true);
    next->processes.erase(next->processes.begin() + 2); // bash exited
    next->processes[3].startTicks += 999;               // PID 777 reused
    next->processes[3].name = "newcomer";
    w.applySnapshot(next);
    QCOMPARE(table->model()->rowCount(), 4);
    filter->setText(QStringLiteral("777"));
    QCOMPARE(table->model()->index(0, ProcessModel::Name).data().toString(), QStringLiteral("newcomer"));
}

void TestUi::processModelStaleKeepsRowsHidesValues()
{
    ProcessModel model;
    model.setProcesses(snapshot(1, true)->processes);
    QCOMPARE(model.rowCount(), 5);
    const int row = 3; // 4242
    QCOMPARE(model.index(row, ProcessModel::Pid).data().toInt(), 4242);
    QCOMPARE(model.index(row, ProcessModel::Cpu).data().toString(), QStringLiteral("150.0"));
    model.setStale(true);
    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(model.index(row, ProcessModel::Pid).data().toInt(), 4242);
    QCOMPARE(model.index(row, ProcessModel::Name).data().toString(), QStringLiteral("Web Content (x)"));
    for (int c = 0; c < ProcessModel::ColumnCount; ++c) {
        const QString text = model.index(row, c).data().toString();
        if (ProcessModel::isSampledColumn(c)) {
            QCOMPARE(text, kDash);
            QVERIFY(model.index(row, c).data(Qt::ToolTipRole).toString().contains(QStringLiteral("No recent samples")));
        } else {
            QVERIFY2(text != kDash, qPrintable(QString::number(c)));
        }
    }
    model.setStale(false);
    QCOMPARE(model.index(row, ProcessModel::Cpu).data().toString(), QStringLiteral("150.0"));
    // Sorting role stays numeric (NaN for unavailable) regardless of display.
    QVERIFY(std::isnan(model.index(4, ProcessModel::Cpu).data(ProcessModel::SortRole).toDouble()));
}

void TestUi::csvExportWritesUnitsAndBlanks()
{
    Env env;
    MainWindow w(env.options());
    feed(w, 200, 3);
    QSignalSpy done(&w, &MainWindow::csvExportFinished);
    const QString path = env.dir.filePath(QStringLiteral("out.csv"));
    QString err;
    QVERIFY2(w.startCsvExport(path, &err), qPrintable(err));
    QVERIFY(done.wait(5000));
    QCOMPARE(done.count(), 1);
    QVERIFY2(done.at(0).at(0).toBool(), qPrintable(done.at(0).at(2).toString()));
    QCOMPARE(done.at(0).at(1).toString(), path);
    QVERIFY(!w.isExporting());
    QVERIFY(w.findChild<QAction*>(QStringLiteral("exportAction"))->isEnabled());

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QStringLiteral("\r\n"), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 4);
    const QStringList header = lines[0].split(QLatin1Char(','));
    QCOMPARE(header.mid(0, 3), (QStringList {QStringLiteral("time_since_boot_s"), QStringLiteral("unix_time_s"),
                                             QStringLiteral("continuous")}));
    const int cpu = header.indexOf(QStringLiteral("CPU total (%)"));
    const int mem = header.indexOf(QStringLiteral("Memory used (bytes)"));
    const int rx = header.indexOf(QStringLiteral("Receive: all physical interfaces (B/s)"));
    QVERIFY(cpu > 0 && mem > 0 && rx > 0);
    QVERIFY(header.contains(QString::fromUtf8("Temperature: coretemp Package id 0 (\u00b0C)")));
    const QStringList first = lines[1].split(QLatin1Char(','));
    const QStringList second = lines[2].split(QLatin1Char(','));
    QCOMPARE(first.at(2), QStringLiteral("0"));
    QCOMPARE(second.at(2), QStringLiteral("1"));
    QVERIFY(first.at(cpu).isEmpty()); // warming up: blank, not zero
    QVERIFY(first.at(rx).isEmpty());
    QVERIFY(!first.at(mem).isEmpty());
    QCOMPARE(second.at(cpu).toDouble(), 21.0);
    QCOMPARE(second.at(mem).toDouble(), 6 * GiB);
}

void TestUi::csvExportFailureIsReported()
{
    Env env;
    MainWindow w(env.options());
    feed(w, 1, 2);
    QSignalSpy done(&w, &MainWindow::csvExportFinished);
    const QString path = env.dir.filePath(QStringLiteral("missing-dir/out.csv"));
    QVERIFY(w.startCsvExport(path));
    QVERIFY(done.wait(5000));
    QVERIFY(!done.at(0).at(0).toBool());
    QVERIFY(!done.at(0).at(2).toString().isEmpty());
    QVERIFY(!QFile::exists(path));
    // A non-modal message box explains the failure; the window stays live.
    QMessageBox* box = nullptr;
    QTRY_VERIFY((box = w.findChild<QMessageBox*>(QStringLiteral("exportErrorBox"))) != nullptr);
    QVERIFY(box->text().contains(QStringLiteral("Could not save")));
    QVERIFY(!box->isModal() || box->windowModality() == Qt::WindowModal);
    w.applySnapshot(snapshot(3, true));
    QCOMPARE(w.history().size(), std::size_t(3));
    box->close();
    QVERIFY(w.findChild<QAction*>(QStringLiteral("exportAction"))->isEnabled());
}

void TestUi::csvExportBusyThenCompletes()
{
    Env env;
    auto gate = std::make_shared<Gate>();
    MainWindowOptions o = env.options();
    o.csvWriter = blockingWriter(gate);
    MainWindow w(std::move(o));
    feed(w, 1, 3);
    QSignalSpy done(&w, &MainWindow::csvExportFinished);
    const QString path = env.dir.filePath(QStringLiteral("slow.csv"));
    QVERIFY(w.startCsvExport(path));
    QVERIFY(gate->waitEntered(5000));
    QVERIFY(w.isExporting());
    auto* action = w.findChild<QAction*>(QStringLiteral("exportAction"));
    QVERIFY(!action->isEnabled());
    QCOMPARE(action->text(), QString::fromUtf8("Exporting\u2026"));
    QString err;
    QVERIFY(!w.startCsvExport(env.dir.filePath(QStringLiteral("second.csv")), &err));
    QVERIFY2(err.contains(QStringLiteral("still running")), qPrintable(err));
    // The UI keeps sampling while the writer is stuck; the export has its
    // own copy of the history.
    QElapsedTimer t;
    t.start();
    w.applySnapshot(snapshot(10, true));
    QVERIFY(t.elapsed() < 1000);
    QCOMPARE(w.history().size(), std::size_t(4));
    gate->release();
    QVERIFY(done.wait(5000));
    QVERIFY(done.at(0).at(0).toBool());
    QVERIFY(action->isEnabled());
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(f.readAll()).count(QStringLiteral("\r\n")), 4); // header + 3 samples at start
}

void TestUi::csvExportShutdownIsBounded()
{
    Env env;
    auto gate = std::make_shared<Gate>();
    MainWindowOptions o = env.options();
    o.csvWriter = blockingWriter(gate);
    auto w = std::make_unique<MainWindow>(std::move(o));
    feed(*w, 1, 2);
    QSignalSpy done(w.get(), &MainWindow::csvExportFinished);
    QVERIFY(w->startCsvExport(env.dir.filePath(QStringLiteral("stuck.csv"))));
    QVERIFY(gate->waitEntered(5000));
    QElapsedTimer t;
    t.start();
    QVERIFY(!w->shutdown(300)); // export abandoned, not waited for
    QVERIFY2(t.elapsed() < 1500, qPrintable(QString::number(t.elapsed())));
    w.reset(); // destroying the window after that must not hang either
    QVERIFY(t.elapsed() < 2500);
    gate->release(); // the detached worker finishes with its own data only
    QTest::qWait(200);
    QCOMPARE(done.count(), 0);
}

void TestUi::historyLimitIsExplained()
{
    Env env;
    MainWindow w(env.options());
    constexpr int cores = 300;
    constexpr int sensors = 90;
    for (int k = 0; k < 2; ++k) {
        auto s = snapshot(10 + k, k > 0);
        s->logicalCpus = cores;
        s->cores.clear();
        for (int i = 0; i < cores; ++i) {
            ps::CoreUsage c;
            c.index = i;
            c.online = true;
            c.usage.total = k > 0 ? 5 : ps::kNaN;
            s->cores.push_back(c);
        }
        s->temperatures.clear();
        for (int i = 0; i < sensors; ++i) {
            const std::string n = std::to_string(i);
            s->temperatures.push_back({"hwmon.chip.temp" + n, "chip", "temp" + n, "chip temp" + n, "/sys/t" + n, 40.0});
        }
        w.applySnapshot(s);
    }
    const auto& h = w.history();
    QCOMPARE(h.seriesCount(ps::SeriesGroup::Core), ps::History::groupQuota(ps::SeriesGroup::Core));
    QCOMPARE(h.seriesCount(ps::SeriesGroup::Temperature), ps::History::groupQuota(ps::SeriesGroup::Temperature));
    // Every summary series still fits.
    for (auto key : {ps::keys::cpuTotal, ps::keys::memUsed, ps::keys::swapUsed, ps::keys::diskAllRead,
                     ps::keys::netAllRx})
        QVERIFY2(h.find(key), std::string(key).c_str());
    QVERIFY2(w.statusText().contains(QStringLiteral("History limit reached, not recorded: 44 CPU cores")),
             qPrintable(w.statusText()));
    QVERIFY(w.statusText().contains(QStringLiteral("10 sensors")));

    // Omitted graphs say so instead of looking like they are still waiting.
    int notRecorded = 0;
    for (auto* chart : child<QWidget>(w, "coreGrid")->findChildren<TimeChart*>())
        if (chart->title().endsWith(QStringLiteral("(not recorded)")))
            ++notRecorded;
    QCOMPARE(notRecorded, cores - 256);
    auto* list = child<QTreeWidget>(w, "sensorList");
    QCOMPARE(list->topLevelItemCount(), sensors);
    QVERIFY(list->topLevelItem(sensors - 1)->toolTip(0).contains(QStringLiteral("Not recorded")));
    list->topLevelItem(sensors - 1)->setCheckState(0, Qt::Checked);
    auto* chart = child<TimeChart>(w, "temperatureChart");
    QVERIFY(chart->series().back().label.endsWith(QStringLiteral("(not recorded)")));
}

void TestUi::settingsPersist()
{
    Env env;
    {
        MainWindow w(env.options());
        feed(w, 1, 2);
        w.setIntervalMs(2000);
        w.setWindowSeconds(60);
        w.showPage(MainWindow::MemoryPage);
        child<QCheckBox>(w, "kernelThreadsCheck")->setChecked(true);
        auto* nets = child<QComboBox>(w, "netSelector");
        nets->setCurrentIndex(2);
        Q_EMIT nets->activated(2);
        w.close(); // saves settings
    }
    {
        MainWindow w(env.options());
        QCOMPARE(w.intervalMs(), 2000);
        QCOMPARE(w.windowSeconds(), 60);
        QCOMPARE(w.currentPage(), MainWindow::MemoryPage);
        QVERIFY(child<QCheckBox>(w, "kernelThreadsCheck")->isChecked());
        w.applySnapshot(snapshot(5, false));
        QCOMPARE(child<QComboBox>(w, "netSelector")->currentData().toString(), QStringLiteral("wlp0s20f3"));
    }
    {
        // --interval overrides the saved cadence; an invalid saved value falls back.
        MainWindowOptions o = env.options();
        o.initialIntervalMs = 500;
        MainWindow w(std::move(o));
        QCOMPARE(w.intervalMs(), 500);
    }
    {
        QSettings s(env.settings(), QSettings::IniFormat);
        s.setValue(QStringLiteral("intervalMs"), 7);
        s.setValue(QStringLiteral("windowSeconds"), -1);
        s.setValue(QStringLiteral("page"), 99);
        s.sync();
        MainWindow w(env.options());
        QCOMPARE(w.intervalMs(), 1000);
        QCOMPARE(w.windowSeconds(), 300);
        QVERIFY(w.currentPage() >= 0 && w.currentPage() < MainWindow::PageCount);
    }
}

void TestUi::blockedCollectorGoesStaleAndRecovers()
{
    // A real Collector over a synthetic /proc. After the first sample a new
    // process appears whose user lookup blocks the collector thread, so
    // samples stop arriving exactly as with a hung kernel read.
    Tree tree;
    tree.base();
    struct Block {
        std::mutex m;
        std::condition_variable cv;
        bool blocked = false;
        bool released = false;
    };
    auto block = std::make_shared<Block>();
    Env env;
    MainWindowOptions o = env.options();
    o.startCollector = true;
    o.initialIntervalMs = 500;
    o.sampler.procRoot = tree.path("proc");
    o.sampler.sysRoot = tree.path("sys");
    o.sampler.statFs = [](const std::string&, int& err) -> std::optional<ps::FsStats> {
        err = ENOENT;
        return std::nullopt;
    };
    o.sampler.userName = [block](uint32_t uid) {
        if (uid == 4242) {
            std::unique_lock lock(block->m);
            block->blocked = true;
            block->cv.notify_all();
            block->cv.wait_for(lock, std::chrono::seconds(20), [&] { return block->released; });
        }
        return "u" + std::to_string(uid);
    };
    auto release = [block] {
        std::lock_guard lock(block->m);
        block->released = true;
        block->cv.notify_all();
    };
    struct Guard {
        std::function<void()> f;
        ~Guard() { f(); }
    } guard {release};

    MainWindow w(std::move(o));
    QTRY_VERIFY_WITH_TIMEOUT(w.snapshotsApplied() >= 2, 5000);
    w.showPage(MainWindow::ProcessPage);
    QTRY_VERIFY_WITH_TIMEOUT(child<QTableView>(w, "processTable")->model()->rowCount() >= 1, 3000);
    QVERIFY(!w.isStale());
    QVERIFY(child<MetricButton>(w, "navMemory")->valueText() != kDash);

    tree.process(200, "stuck", 4242);
    {
        std::unique_lock lock(block->m);
        QVERIFY(block->cv.wait_for(lock, std::chrono::seconds(5), [&] { return block->blocked; }));
    }
    QTRY_VERIFY_WITH_TIMEOUT(w.isStale(), 5000);
    QVERIFY2(w.statusText().contains(QStringLiteral("blocked or starved")), qPrintable(w.statusText()));
    QCOMPARE(child<MetricButton>(w, "navMemory")->valueText(), kDash);
    QCOMPARE(factText(w, "mem_used"), kDash);
    auto* table = child<QTableView>(w, "processTable");
    QVERIFY(table->model()->rowCount() >= 1); // rows kept...
    QCOMPARE(table->model()->index(0, ProcessModel::Cpu).data().toString(), kDash); // ...values not
    QVERIFY(child<QLabel>(w, "processCount")->text().contains(QStringLiteral("No recent samples")));
    for (auto* chart : w.findChildren<TimeChart*>())
        QCOMPARE(chart->notice(), QStringLiteral("No recent samples"));

    // Pausing does not make the old values current again.
    w.setPaused(true);
    QVERIFY(w.isStale());
    QCOMPARE(child<TimeChart>(w, "cpuChart")->notice(), QStringLiteral("Paused, no recent samples"));
    QVERIFY(w.statusText().contains(QStringLiteral("stay unavailable")));
    QCOMPARE(factText(w, "mem_used"), kDash);

    // Recovery: release the thread and resume; the next snapshot clears it.
    release();
    w.setPaused(false);
    QTRY_VERIFY_WITH_TIMEOUT(!w.isStale(), 5000);
    QVERIFY(factText(w, "mem_used") != kDash);
    QTRY_COMPARE_WITH_TIMEOUT(child<TimeChart>(w, "cpuChart")->notice(), QString(), 2000);
    QVERIFY(w.shutdown(2000));
}

void TestUi::freshnessFollowsAcquisitionClock()
{
    // An injected clock stands in for CLOCK_BOOTTIME: freshness is judged by
    // when a sample was acquired, and charts end at the clock.
    Env env;
    auto clock = std::make_shared<double>(1000.0);
    auto o = env.options();
    o.clock = [clock] { return *clock; };
    MainWindow w(o);
    auto* cpuChart = child<TimeChart>(w, "cpuChart");
    QVERIFY(cpuChart);
    QCOMPARE(w.intervalMs(), 1000);
    QVERIFY(!w.isStale());

    // No sample at all: overdue after the limit (2.5 s at 1 s) plus 2 s.
    *clock = 1004.0;
    QTest::qWait(700);
    QVERIFY(!w.isStale());
    *clock = 1005.0;
    QTRY_VERIFY_WITH_TIMEOUT(w.isStale(), 3000);
    QVERIFY2(w.statusText().startsWith(QStringLiteral("Waiting 5 s")), qPrintable(w.statusText()));

    // A frame acquired 20 s ago is history, not current data.
    w.applySnapshot(snapshot(985, false));
    QVERIFY(w.isStale());
    QCOMPARE(factText(w, "mem_used"), kDash);
    QCOMPARE(w.history().latestTime(), 985.0);
    QCOMPARE(cpuChart->viewEnd(), 1005.0);
    cpuChart->setCursorTime(985);
    QVERIFY2(cpuChart->readoutText().startsWith(QStringLiteral("20 s ago")), qPrintable(cpuChart->readoutText()));
    cpuChart->clearCursor();
    QVERIFY2(w.statusText().startsWith(QStringLiteral("The newest sample is 20 s old")), qPrintable(w.statusText()));

    // A recent frame restores the live view; the chart still ends at the clock.
    *clock = 1005.4;
    w.applySnapshot(snapshot(1005, true, 40));
    QVERIFY2(!w.isStale(), qPrintable(w.statusText()));
    QVERIFY(w.statusText().startsWith(QStringLiteral("Live")));
    QVERIFY(factText(w, "mem_used").contains(QStringLiteral("6.00 GiB")));
    QVERIFY(factText(w, "cpu_total").contains(QStringLiteral("40")));
    QCOMPARE(cpuChart->viewEnd(), 1005.4);

    // A frame acquired before the one shown is ignored, not shown.
    const int applied = w.snapshotsApplied();
    auto older = snapshot(1004.5, true, 90);
    older->memory.used = 12 * GiB;
    w.applySnapshot(older);
    QCOMPARE(w.snapshotsApplied(), applied);
    QVERIFY(!w.isStale());
    QVERIFY(factText(w, "cpu_total").contains(QStringLiteral("40")));
    QVERIFY(factText(w, "mem_used").contains(QStringLiteral("6.00 GiB")));
    QCOMPARE(w.history().latestTime(), 1005.0);

    // Samples that stop arriving go stale once the newest acquisition is
    // older than the limit; the chart end keeps moving, so the gap grows.
    *clock = 1006;
    w.applySnapshot(snapshot(1006, true, 41));
    QVERIFY(!w.isStale());
    *clock = 1008.4;
    QTest::qWait(700);
    QVERIFY(!w.isStale());
    *clock = 1008.7;
    QTRY_VERIFY_WITH_TIMEOUT(w.isStale(), 3000);
    QVERIFY2(w.statusText().startsWith(QStringLiteral("The newest sample is 3 s old")), qPrintable(w.statusText()));
    *clock = 1012;
    QTRY_COMPARE_WITH_TIMEOUT(cpuChart->viewEnd(), 1012.0, 3000);
    QCOMPARE(w.history().latestTime(), 1006.0);
    // Delivered late (acquired 4 s before receipt): recorded, but not live.
    w.applySnapshot(snapshot(1008, true, 42));
    QVERIFY(w.isStale());
    QCOMPARE(w.history().latestTime(), 1008.0);

    // After an interval change the newest sample was taken on the old
    // schedule and is judged by the longer of the two until the next one.
    *clock = 1013;
    w.applySnapshot(snapshot(1013, true));
    QVERIFY(!w.isStale());
    w.setIntervalMs(5000); // limit 12.5 s
    *clock = 1020;
    QTest::qWait(700);
    QVERIFY(!w.isStale());
    w.applySnapshot(snapshot(1020, true));
    w.setIntervalMs(500); // still 12.5 s: this sample is on the 5 s schedule
    *clock = 1025;
    QTest::qWait(700);
    QVERIFY(!w.isStale());
    w.applySnapshot(snapshot(1025, true)); // on the new schedule: limit 1.5 s
    *clock = 1026.2;
    QTest::qWait(700);
    QVERIFY(!w.isStale());
    *clock = 1026.8;
    QTRY_VERIFY_WITH_TIMEOUT(w.isStale(), 3000);

    // Pausing never makes old values current, nor does resuming.
    w.setPaused(true);
    *clock = 1100;
    QTest::qWait(700);
    QVERIFY(w.isStale());
    QVERIFY(w.statusText().startsWith(QStringLiteral("Paused")));
    w.setPaused(false);
    QVERIFY(w.isStale());
    w.applySnapshot(snapshot(1100, false));
    QVERIFY(!w.isStale());

    // A time from the future is untrusted: stale, not recorded, and it does
    // not block the samples that follow.
    w.applySnapshot(snapshot(1200, true));
    QVERIFY(w.isStale());
    QVERIFY2(w.statusText().startsWith(QStringLiteral("The newest sample has an inconsistent time")),
             qPrintable(w.statusText()));
    QCOMPARE(w.history().latestTime(), 1100.0);
    *clock = 1101;
    w.applySnapshot(snapshot(1101, true, 43));
    QVERIFY2(!w.isStale(), qPrintable(w.statusText()));
    QCOMPARE(w.history().latestTime(), 1101.0);
    QVERIFY(factText(w, "cpu_total").contains(QStringLiteral("43")));
    w.close();
}

void TestUi::chartWithoutSamplesInWindow()
{
    const QString idle = QStringLiteral("No samples in this time window");
    auto pct = [](double v) { return QStringLiteral("Load ") + QString::fromStdString(ps::fmt::percent(v)); };
    auto line = [](double v) { return QStringLiteral("Load: ") + QString::fromStdString(ps::fmt::percent(v)); };
    auto add = [](ps::History& h, double t, double v) {
        QVERIFY(h.beginSample(t, !h.empty()));
        h.set("k", "Load", ps::Unit::Percent, v);
        h.endSample();
    };
    auto hover = [](TimeChart& c, QPointF pos) {
        QMouseEvent move(QEvent::MouseMove, pos, c.mapToGlobal(pos), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&c, &move);
    };
    auto keys = [](TimeChart& c, std::initializer_list<Qt::Key> list) {
        for (const Qt::Key k : list)
            QTest::keyClick(&c, k);
    };

    // Partial history: samples at 100 and 101, the axis ends at 162 with a
    // 60 s window, so both are older than the window's start (102).
    ps::History partial(10, 1e9);
    add(partial, 100, 10);
    add(partial, 101, 20);
    {
        TimeChart chart(QStringLiteral("Load"), ps::Unit::Percent);
        chart.setHistory(&partial);
        chart.setSeries({{"k", QStringLiteral("Load")}});
        chart.setEmptyText(QStringLiteral("Specific explanation"));
        chart.setWindowSeconds(60);
        chart.resize(480, 260);
        chart.setViewEnd(101);
        chart.refresh();
        (void)chart.grab();
        QCOMPARE(chart.lastPaintedPoints(), 2);
        chart.setCursorTime(101);
        QCOMPARE(chart.readoutText().section(QLatin1Char('\n'), 0, 0), QStringLiteral("Latest sample"));

        // The selected sample ages out: no readout, no line, the chart says why.
        chart.setViewEnd(162);
        chart.refresh();
        QVERIFY(!chart.hasVisibleData());
        QCOMPARE(chart.readoutText(), QString());
        QCOMPARE(chart.accessibleDescription(), idle);
        (void)chart.grab();
        QCOMPARE(chart.lastPaintedPoints(), 0);

        // Navigation keys neither select an expired sample nor move the cursor.
        keys(chart, {Qt::Key_Home, Qt::Key_End, Qt::Key_Left, Qt::Key_Right});
        QCOMPARE(chart.readoutText(), QString());
        QCOMPARE(chart.accessibleDescription(), idle);
        (void)chart.grab();

        // A wider range shows the samples again; the cursor is still at 101.
        chart.setWindowSeconds(900);
        chart.refresh();
        QVERIFY(chart.hasVisibleData());
        QCOMPARE(chart.readoutText().section(QLatin1Char('\n'), 0, 0), QStringLiteral("61 s ago"));
        QVERIFY(chart.accessibleDescription().startsWith(QStringLiteral("61 s ago")));

        // Escape clears the cursor even while no sample is visible.
        chart.setWindowSeconds(60);
        chart.refresh();
        QTest::keyClick(&chart, Qt::Key_Escape);
        chart.setWindowSeconds(900);
        chart.refresh();
        QCOMPARE(chart.readoutText(), QString());
        QCOMPARE(chart.accessibleDescription(), pct(20));

        // Without a cursor, keys cannot create one from the empty window.
        chart.setWindowSeconds(60);
        chart.refresh();
        keys(chart, {Qt::Key_End, Qt::Key_Right, Qt::Key_Home, Qt::Key_Left});
        chart.setWindowSeconds(900);
        chart.refresh();
        QCOMPARE(chart.readoutText(), QString());

        // Hovering the empty window shows no readout; widening shows one.
        chart.setWindowSeconds(60);
        chart.refresh();
        (void)chart.grab();
        const QPointF centre(chart.width() / 2.0, chart.height() / 2.0);
        hover(chart, centre);
        QCOMPARE(chart.readoutText(), QString());
        (void)chart.grab();
        QCOMPARE(chart.lastPaintedPoints(), 0);
        chart.setWindowSeconds(900);
        QCOMPARE(chart.readoutText(), QStringLiteral("62 s ago\n") + line(10));
        QTest::keyClick(&chart, Qt::Key_Escape);
        QCOMPARE(chart.readoutText(), QString());

        // A new sample inside the window brings back the line and readout.
        chart.setWindowSeconds(60);
        chart.setCursorTime(101);
        add(partial, 161, 30);
        chart.refresh();
        QCOMPARE(chart.readoutText(), QStringLiteral("1.0 s ago\n") + line(30));
        (void)chart.grab();
        QCOMPARE(chart.lastPaintedPoints(), 1);

        // The compact form shows no current value from an expired sample.
        chart.setViewEnd(400);
        chart.setMode(TimeChart::Mode::Compact);
        (void)chart.grab();
        QCOMPARE(chart.lastPaintedPoints(), 0);
    }

    // Wrapped history: capacity 4 after 7 samples keeps 103..106 with the
    // ring's head mid-buffer, so an index equal to size() would alias 103.
    ps::History wrapped(4, 1e9);
    for (int i = 0; i < 7; ++i)
        add(wrapped, 100 + i, 10.0 * i);
    QCOMPARE(wrapped.size(), std::size_t {4});
    QCOMPARE(wrapped.time(0), 103.0);
    {
        TimeChart chart(QStringLiteral("Load"), ps::Unit::Percent);
        chart.setHistory(&wrapped);
        chart.setSeries({{"k", QStringLiteral("Load")}});
        chart.setWindowSeconds(2);
        chart.resize(480, 260);
        chart.setViewEnd(106);
        chart.refresh();
        (void)chart.grab();

        // Valid window [104, 106]: Home, Left, End and Right stay inside it.
        QTest::keyClick(&chart, Qt::Key_Home);
        QCOMPARE(chart.readoutText(), QStringLiteral("2.0 s ago\n") + line(40));
        QTest::keyClick(&chart, Qt::Key_Left);
        QCOMPARE(chart.readoutText(), QStringLiteral("2.0 s ago\n") + line(40));
        QTest::keyClick(&chart, Qt::Key_Right);
        QCOMPARE(chart.readoutText(), QStringLiteral("1.0 s ago\n") + line(50));
        QTest::keyClick(&chart, Qt::Key_End);
        QTest::keyClick(&chart, Qt::Key_Right);
        QCOMPARE(chart.readoutText(), QStringLiteral("Latest sample\n") + line(60));

        // Everything ages out: the wrapped slots are never read.
        chart.setWindowSeconds(60);
        chart.setViewEnd(200);
        chart.refresh();
        QCOMPARE(chart.readoutText(), QString());
        QCOMPARE(chart.accessibleDescription(), idle);
        keys(chart, {Qt::Key_Home, Qt::Key_Left, Qt::Key_End, Qt::Key_Right});
        QCOMPARE(chart.readoutText(), QString());
        (void)chart.grab();
        QCOMPARE(chart.lastPaintedPoints(), 0);
        hover(chart, QPointF(chart.width() / 2.0, chart.height() / 2.0));
        QCOMPARE(chart.readoutText(), QString());
        (void)chart.grab();
        QTest::keyClick(&chart, Qt::Key_Escape);

        // After a gap a new sample arrives; Home then selects it, not 103.
        add(wrapped, 199, 70);
        chart.refresh();
        QCOMPARE(chart.accessibleDescription(), pct(70));
        QTest::keyClick(&chart, Qt::Key_Home);
        QCOMPARE(chart.readoutText(), QStringLiteral("1.0 s ago\n") + line(70));
        (void)chart.grab();
        QCOMPARE(chart.lastPaintedPoints(), 1);
    }
}

void TestUi::liveCollectorPopulatesAndShutsDown()
{
    Env env;
    MainWindowOptions o = env.options();
    o.startCollector = true;
    o.initialIntervalMs = 500;
    auto w = std::make_unique<MainWindow>(std::move(o));
    w->resize(1180, 780);
    w->show();
    QVERIFY(QTest::qWaitForWindowExposed(w.get()));
    QTRY_VERIFY_WITH_TIMEOUT(w->snapshotsApplied() >= 3, 10000);
    QVERIFY(w->collector() && w->collector()->isRunning());
    const auto s = w->latestSnapshot();
    QVERIFY(s && s->continuous);
    QVERIFY(ps::isValid(s->cpu.total));
    QVERIFY(s->memory.valid);
    QVERIFY(w->statusText().startsWith(QStringLiteral("Live")));
    for (int p = 0; p < MainWindow::PageCount; ++p) {
        w->showPage(static_cast<MainWindow::Page>(p));
        QApplication::processEvents();
    }
    // Every category is either populated or explicitly unavailable.
    QVERIFY(child<MetricButton>(*w, "navCpu")->valueText().contains(QLatin1Char('%')));
    QVERIFY(child<MetricButton>(*w, "navMemory")->valueText().contains(QStringLiteral("used")));
    const QString temp = child<MetricButton>(*w, "navTemperatures")->valueText();
    QVERIFY2(temp.contains(QString::fromUtf8("\u00b0C")) || temp == QStringLiteral("Unavailable"), qPrintable(temp));
    QVERIFY(child<MetricButton>(*w, "navProcesses")->valueText().contains(QStringLiteral("processes")));
    QVERIFY(child<QTableView>(*w, "processTable")->model()->rowCount() > 0);

    w->showPage(MainWindow::CpuPage);
    const QString shot = QDir(artifactDir()).filePath(QStringLiteral("tst_ui-live.png"));
    QVERIFY(w->grab().save(shot));

    QElapsedTimer t;
    t.start();
    QVERIFY(w->shutdown(2000));
    QVERIFY(!w->collector()->isRunning());
    QVERIFY2(t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));
    const int applied = w->snapshotsApplied();
    QTest::qWait(700);
    QCOMPARE(w->snapshotsApplied(), applied); // nothing arrives after shutdown
    w.reset();
}

void TestUi::cliWindowSizeIsValidated()
{
    QCOMPARE(cli::parseWindowSize(QStringLiteral("1180x780")), std::optional<QSize>(QSize(1180, 780)));
    QCOMPARE(cli::parseWindowSize(QStringLiteral(" 852X600 ")), std::optional<QSize>(QSize(852, 600)));
    for (const char* bad : {"", "x", "1180", "1180x", "x780", "1180x780x2", "-1180x780", "+852x600", "852 x600",
                            "852x6e2", "0x0", "319x600", "852x16385", "99999999999x600", "852.5x600", "nanxnan",
                            "\xd9\xa8\xd9\xa5\xd9\xa2x600" /* Arabic-Indic digits */})
        QVERIFY2(!cli::parseWindowSize(QString::fromUtf8(bad)), bad);
}

int main(int argc, char** argv)
{
    // Never open windows on the user's desktop unless explicitly asked to
    // (keyboard cases also need the focus the offscreen platform grants).
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestUi tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_ui.moc"
