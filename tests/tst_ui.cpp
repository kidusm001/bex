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
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPaintEvent>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <map>
#include <set>
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
    void simpleByDefaultDetailsRestoresEverything();
    void detailsAndSmoothPreferencesPersist();
    void headlinesFollowDataAndState();
    void seriesKeepColoursStylesAndFills();
    void frameDriverRunsOnlyWhenSomeoneCanSee();
    void animationNeverTouchesMeasurements();
    void themeChoiceKeepsExplicitSettings();
    void themeProbeFindsConfigAndPlugin();
    void partialFramesMatchFullRepaint();
    void bandFramesMatchFullRepaint();
    void cursorFramesMatchFullRepaint();
    void layerCacheIgnoresClockMagnitude();
    void navSparklineClipsMeasuredGeometry();
    void temperatureLooksFollowSensors();
    void temperatureLooksOutlastEightSensors();
    void detailsScrollInsteadOfSqueezing();
    void shortChartsKeepAxisLabelsApart();
    void timeAxisLabelsNeverTouch();
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
    QTest::addColumn<bool>("details");
    QTest::newRow("default font") << 1.0 << 800 << false;
    QTest::newRow("large font") << 1.3 << 1000 << false;
    QTest::newRow("default font, details") << 1.0 << 800 << true;
    QTest::newRow("large font, details") << 1.3 << 1000 << true;
}

void TestUi::narrowWindowReflowsWithoutClipping()
{
    QFETCH(double, fontScale);
    QFETCH(int, maxMinimumWidth);
    QFETCH(bool, details);
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
    w.setDetailsVisible(details);
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
            const bool other = l->objectName() == QLatin1String("processCount")
                || l->objectName().contains(QLatin1String("HeadValue"));
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
        if (width == 1500 && details) // wide: four pairs per row, as designed
            QCOMPARE(y("mem_total"), y("mem_cache"));
        if (width == 1500) { // wide: read and write side by side
            w.showPage(MainWindow::DiskPage);
            QTest::qWait(20);
            QCOMPARE(y("diskHeadValue0"), y("diskHeadValue1"));
        }
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
    w.setDetailsVisible(true); // the fact rows are details
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

namespace {

// Visible with real size, and painted somewhere: itself, or (for containers
// such as scroll areas, covered by their children) a descendant.
bool shownWithArea(QWidget* w)
{
    if (!w || !w->isVisible() || w->width() <= 0 || w->height() <= 0)
        return false;
    if (!w->visibleRegion().isEmpty())
        return true;
    const auto kids = w->findChildren<QWidget*>();
    return std::any_of(kids.begin(), kids.end(), [](QWidget* c) { return c->isVisible() && !c->visibleRegion().isEmpty(); });
}

QString headText(QWidget& w, const char* name)
{
    auto* l = child<QLabel>(w, name);
    return l ? l->text() : QStringLiteral("<missing %1>").arg(QLatin1String(name));
}

} // namespace

void TestUi::simpleByDefaultDetailsRestoresEverything()
{
    // New and existing users start with the simple view: one headline per
    // page, the main chart, and no fact grids, per-core charts, tables or
    // extra columns. Details (Ctrl+D) brings every one of them back with
    // real geometry, and toggling never touches data, selections or order.
    Env env;
    MainWindow w(env.options());
    QVERIFY(!w.detailsVisible());
    auto* details = child<QAction>(w, "detailsAction");
    QVERIFY(details && details->isCheckable() && !details->isChecked());
    QCOMPARE(details->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_D));
    QVERIFY(!details->toolTip().isEmpty());
    feed(w, 100, 6);
    w.resize(1180, 780);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto* cpuChart = child<TimeChart>(w, "cpuChart");
    auto* memChart = child<TimeChart>(w, "memoryChart");
    auto* table = child<QTableView>(w, "processTable");
    auto* disks = child<QComboBox>(w, "diskSelector");
    auto* nets = child<QComboBox>(w, "netSelector");
    auto* sensors = child<QTreeWidget>(w, "sensorList");
    QVERIFY(cpuChart && memChart && table && disks && nets && sensors);

    struct PageWidgets {
        MainWindow::Page page;
        std::vector<const char*> simple;  // shown in both views
        std::vector<const char*> details; // shown only with details
    };
    const std::vector<PageWidgets> pages {
        {MainWindow::CpuPage, {"cpuHeadValue0", "cpuHeadSummary", "cpuChart"}, {"cpu_total", "cpu_user", "coreHeader", "coreScroll"}},
        {MainWindow::MemoryPage, {"memoryHeadValue0", "memoryHeadSummary", "memoryChart"}, {"mem_cache", "swap_used", "swapChart"}},
        {MainWindow::DiskPage, {"diskHeadValue0", "diskHeadValue1", "diskSelector", "diskChart"}, {"disk_read", "filesystemTable"}},
        {MainWindow::NetworkPage, {"netHeadValue0", "netHeadValue1", "netSelector", "netChart"}, {"net_rx"}},
        {MainWindow::TemperaturePage, {"temperatureHeadValue0", "temperatureChart"}, {"sensorList"}},
        {MainWindow::ProcessPage, {"processFilter", "processTable", "processShortNote"}, {}},
    };
    auto checkView = [&](bool detailed) {
        QStringList problems;
        for (const auto& p : pages) {
            w.showPage(p.page);
            QTest::qWait(30);
            for (const char* name : p.simple) {
                auto* c = w.findChild<QWidget*>(QString::fromLatin1(name));
                const bool wantShown = !(detailed && QLatin1String(name) == QLatin1String("processShortNote"));
                if (wantShown != shownWithArea(c))
                    problems << QStringLiteral("%1 %2").arg(QLatin1String(name), wantShown ? QStringLiteral("hidden") : QStringLiteral("shown"));
            }
            for (const char* name : p.details) {
                auto* c = w.findChild<QWidget*>(QString::fromLatin1(name));
                if (!c)
                    problems << QStringLiteral("%1 missing").arg(QLatin1String(name));
                else if (detailed != shownWithArea(c))
                    problems << QStringLiteral("%1 %2 (%3x%4)")
                                    .arg(QLatin1String(name), detailed ? QStringLiteral("not shown") : QStringLiteral("shown"))
                                    .arg(c->width())
                                    .arg(c->height());
            }
        }
        for (int c = 0; c < ProcessModel::ColumnCount; ++c) {
            const bool essential =
                c == ProcessModel::Pid || c == ProcessModel::Name || c == ProcessModel::Cpu || c == ProcessModel::Memory;
            if (table->isColumnHidden(c) == (essential || detailed))
                problems << QStringLiteral("process column %1 %2").arg(c).arg(table->isColumnHidden(c) ? "hidden" : "shown");
        }
        return problems;
    };

    QStringList bad = checkView(false);
    QVERIFY2(bad.isEmpty(), qPrintable(QStringLiteral("simple: ") + bad.join(QStringLiteral("; "))));
    QCOMPARE(cpuChart->series().size(), std::size_t(1));
    QCOMPARE(memChart->series().size(), std::size_t(1));

    // Choices made in either view survive the toggle.
    disks->setCurrentIndex(1);
    Q_EMIT disks->activated(1);
    nets->setCurrentIndex(2);
    Q_EMIT nets->activated(2);
    w.showPage(MainWindow::ProcessPage);
    table->sortByColumn(ProcessModel::Memory, Qt::AscendingOrder);
    const QList<int> order = pidOrder(table);
    const std::size_t samples = w.history().size();
    const std::string csv = w.history().toCsv(0);
    const int applied = w.snapshotsApplied();

    QTest::keyClick(&w, Qt::Key_D, Qt::ControlModifier);
    QVERIFY(w.detailsVisible());
    QVERIFY(details->isChecked());
    bad = checkView(true);
    QVERIFY2(bad.isEmpty(), qPrintable(QStringLiteral("details: ") + bad.join(QStringLiteral("; "))));
    QCOMPARE(cpuChart->series().size(), std::size_t(4));
    QCOMPARE(memChart->series().size(), std::size_t(3));
    // The per-core charts and the fact values are live, not placeholders.
    w.showPage(MainWindow::CpuPage);
    QTest::qWait(30);
    QVERIFY(factText(w, "cpu_user").contains(QStringLiteral("15")));
    int coresShown = 0;
    for (auto* c : child<QWidget>(w, "coreGrid")->findChildren<TimeChart*>())
        coresShown += shownWithArea(c) ? 1 : 0;
    QCOMPARE(coresShown, 4);

    details->trigger();
    QVERIFY(!w.detailsVisible());
    bad = checkView(false);
    QVERIFY2(bad.isEmpty(), qPrintable(QStringLiteral("simple again: ") + bad.join(QStringLiteral("; "))));

    QCOMPARE(w.history().size(), samples);
    QCOMPARE(w.history().toCsv(0), csv);
    QCOMPARE(w.snapshotsApplied(), applied);
    QCOMPARE(disks->currentData().toString(), QStringLiteral("nvme0n1"));
    QCOMPARE(nets->currentData().toString(), QStringLiteral("wlp0s20f3"));
    QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), int(ProcessModel::Memory));
    QCOMPARE(pidOrder(table), order);
    QCOMPARE(sensors->topLevelItemCount(), 2);
    w.close();
}

void TestUi::detailsAndSmoothPreferencesPersist()
{
    Env env;
    {
        MainWindow w(env.options());
        QVERIFY(!w.detailsVisible());
        QVERIFY(w.smoothGraphs());
        QVERIFY(child<QAction>(w, "smoothAction")->isChecked());
        w.setDetailsVisible(true);
        child<QAction>(w, "smoothAction")->trigger();
        QVERIFY(!w.smoothGraphs());
        w.close();
    }
    {
        MainWindow w(env.options());
        QVERIFY(w.detailsVisible());
        QVERIFY(child<QAction>(w, "detailsAction")->isChecked());
        QVERIFY(!w.smoothGraphs());
        QVERIFY(!child<QAction>(w, "smoothAction")->isChecked());
        w.resize(1180, 780);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(shownWithArea(child<QWidget>(w, "cpu_total")));
        QVERIFY(!w.isAnimating());
        w.close();
    }
    {
        // A settings file from before the toggle existed: simple, smooth.
        Env old;
        {
            QSettings s(old.settings(), QSettings::IniFormat);
            s.setValue(QStringLiteral("intervalMs"), 2000);
            s.setValue(QStringLiteral("windowSeconds"), 60);
            s.sync();
        }
        MainWindow w(old.options());
        QCOMPARE(w.intervalMs(), 2000);
        QVERIFY(!w.detailsVisible());
        QVERIFY(w.smoothGraphs());
    }
}

void TestUi::headlinesFollowDataAndState()
{
    Env env;
    auto clock = std::make_shared<double>(100.0);
    auto o = env.options();
    o.clock = [clock] { return *clock; };
    MainWindow w(o);
    QCOMPARE(headText(w, "cpuHeadValue0"), kDash);
    QCOMPARE(headText(w, "diskHeadValue1"), kDash);
    QVERIFY(!child<QLabel>(w, "cpuHeadValue0")->accessibleName().isEmpty());
    QVERIFY(!child<QLabel>(w, "cpuHeadSummary")->accessibleName().isEmpty());

    // Warming up: rates need a second sample; memory and sensors do not.
    w.applySnapshot(snapshot(100, false));
    QCOMPARE(headText(w, "cpuHeadValue0"), QString::fromUtf8("Measuring\u2026"));
    QCOMPARE(headText(w, "diskHeadValue0"), QString::fromUtf8("Measuring\u2026"));
    QCOMPARE(headText(w, "netHeadValue1"), QString::fromUtf8("Measuring\u2026"));
    QCOMPARE(headText(w, "memoryHeadValue0"), QStringLiteral("6.00 GiB"));
    const QString mem = headText(w, "memoryHeadSummary");
    QVERIFY2(mem.contains(QStringLiteral("of 16.0 GiB")) && mem.contains(QStringLiteral("10.0 GiB available"))
                 && mem.contains(QStringLiteral("swap 1.00 GiB of 8.00 GiB")),
             qPrintable(mem));
    QVERIFY(headText(w, "temperatureHeadValue0").startsWith(QStringLiteral("55")));
    QCOMPARE(headText(w, "temperatureHeadSummary"), QStringLiteral("coretemp Package id 0, the hottest of 2 sensors reading"));
    const QString disk = headText(w, "diskHeadSummary");
    QVERIFY2(disk.contains(QStringLiteral("One physical disk")) && disk.contains(QStringLiteral("/ has 290 GiB free of 500 GiB")),
             qPrintable(disk));
    QVERIFY2(headText(w, "netHeadSummary").contains(QStringLiteral("loopback excluded")), qPrintable(headText(w, "netHeadSummary")));

    *clock = 101;
    w.applySnapshot(snapshot(101, true, 25));
    QVERIFY2(headText(w, "cpuHeadValue0").startsWith(QStringLiteral("25")), qPrintable(headText(w, "cpuHeadValue0")));
    QCOMPARE(headText(w, "cpuHeadSummary"), QStringLiteral("Average of 4 logical CPUs, load average 0.50"));
    QCOMPARE(headText(w, "diskHeadValue0"), QStringLiteral("4.00 MiB/s"));
    QCOMPARE(headText(w, "diskHeadValue1"), QStringLiteral("1.00 MiB/s"));
    QVERIFY(headText(w, "netHeadValue0").endsWith(QStringLiteral("MiB/s")));

    // A chosen device is named; a missing one is said to be missing.
    auto* disks = child<QComboBox>(w, "diskSelector");
    disks->setCurrentIndex(1);
    Q_EMIT disks->activated(1);
    QVERIFY2(headText(w, "diskHeadSummary").startsWith(QStringLiteral("nvme0n1, ")), qPrintable(headText(w, "diskHeadSummary")));
    auto noNvme = snapshot(102, true);
    noNvme->disks.erase(noNvme->disks.begin());
    *clock = 102;
    w.applySnapshot(noNvme);
    QCOMPARE(headText(w, "diskHeadValue0"), kDash);
    QVERIFY(headText(w, "diskHeadSummary").startsWith(QStringLiteral("nvme0n1 is not present")));

    // No swap, no sensors reading.
    auto bare = snapshot(103, true);
    bare->memory.hasSwap = false;
    bare->memory.swapTotal = 0;
    for (auto& t : bare->temperatures)
        t.celsius = ps::kNaN;
    *clock = 103;
    w.applySnapshot(bare);
    QVERIFY(headText(w, "memoryHeadSummary").endsWith(QStringLiteral(", no swap")));
    QCOMPARE(headText(w, "temperatureHeadValue0"), kDash);
    QCOMPARE(headText(w, "temperatureHeadSummary"), QStringLiteral("No sensor is reading"));

    // Paused and stale states are said, not hidden.
    w.setPaused(true);
    QVERIFY2(headText(w, "cpuHeadSummary").startsWith(QStringLiteral("Paused. ")), qPrintable(headText(w, "cpuHeadSummary")));
    QVERIFY(headText(w, "cpuHeadValue0").startsWith(QStringLiteral("25")) || headText(w, "cpuHeadValue0").contains(QLatin1Char('%')));
    w.setPaused(false);
    *clock = 120;
    w.applySnapshot(snapshot(104, true, 50)); // acquired 16 s ago: history, not current
    QVERIFY(w.isStale());
    QCOMPARE(headText(w, "cpuHeadValue0"), kDash);
    QCOMPARE(headText(w, "memoryHeadValue0"), kDash);
    QVERIFY2(headText(w, "cpuHeadSummary").startsWith(QStringLiteral("No recent samples")), qPrintable(headText(w, "cpuHeadSummary")));
    *clock = 121;
    w.applySnapshot(snapshot(121, true, 30));
    QVERIFY(!w.isStale());
    QVERIFY(headText(w, "cpuHeadValue0").startsWith(QStringLiteral("30")));
}

void TestUi::seriesKeepColoursStylesAndFills()
{
    // Traces are told apart by colour, pen style and legend or headline
    // label; the main measure of each chart gets an area, extra detail lines
    // do not, and overlapping areas are never stacked into totals.
    Env env;
    MainWindow w(env.options());
    feed(w, 10, 3);
    auto series = [&](const char* name) { return child<TimeChart>(w, name)->series(); };
    auto style = [](const ChartSeries& s) { return s.style.value_or(seriesStyle(s.visual)); };
    for (const char* name : {"diskChart", "netChart"}) {
        const auto s = series(name);
        QCOMPARE(s.size(), std::size_t(2));
        QVERIFY2(s[0].fill && s[1].fill, name);
        QVERIFY2(style(s[0]) == Qt::SolidLine && style(s[1]) == Qt::DashLine, name);
        QVERIFY2(s[0].visual != s[1].visual, name);
    }
    QVERIFY(series("cpuChart")[0].fill);
    QVERIFY(series("memoryChart")[0].fill);
    w.setDetailsVisible(true);
    const auto cpu = series("cpuChart");
    QCOMPARE(cpu.size(), std::size_t(4));
    QVERIFY(cpu[0].fill);
    std::set<Qt::PenStyle> styles;
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        styles.insert(style(cpu[i]));
        if (i > 0)
            QVERIFY(!cpu[i].fill);
    }
    QCOMPARE(styles.size(), std::size_t(4));
    // Headline swatches match their traces: read solid, write dashed.
    QCOMPARE(child<TimeChart>(w, "diskChart")->series()[0].label, QStringLiteral("Read"));
    QCOMPARE(child<TimeChart>(w, "netChart")->series()[1].label, QStringLiteral("Transmit"));
}

void TestUi::frameDriverRunsOnlyWhenSomeoneCanSee()
{
    Env env;
    auto clock = std::make_shared<double>(1000.0);
    auto o = env.options();
    o.clock = [clock] { return *clock; };
    MainWindow w(o);
    auto* timer = child<QTimer>(w, "frameTimer");
    auto* cpu = child<TimeChart>(w, "cpuChart");
    QVERIFY(timer && cpu);
    for (int i = 0; i < 10; ++i)
        w.applySnapshot(snapshot(991 + i, i > 0));
    QVERIFY(!w.isAnimating()); // never shown
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTRY_VERIFY(w.isAnimating());
    QCOMPARE(timer->interval(), 16);

    // The view clock follows real time between samples; data does not.
    const std::size_t samples = w.history().size();
    *clock = 1000.5;
    QTRY_COMPARE(cpu->viewEnd(), 1000.5);
    QCOMPARE(w.history().size(), samples);

    // The process page has no chart: a slower clock for its sparklines.
    w.showPage(MainWindow::ProcessPage);
    QCOMPARE(timer->interval(), 66);
    w.showPage(MainWindow::CpuPage);
    QCOMPARE(timer->interval(), 16);

    // Details changes neither the driver nor its timer.
    const int id = timer->timerId();
    w.setDetailsVisible(true);
    w.setDetailsVisible(false);
    QVERIFY(timer->isActive());
    QCOMPARE(timer->timerId(), id);

    w.setPaused(true);
    QVERIFY(!w.isAnimating());
    *clock = 1002;
    QTest::qWait(60);
    QCOMPARE(cpu->viewEnd(), 1000.5); // frozen while paused
    w.setPaused(false);
    QVERIFY(w.isAnimating());

    child<QAction>(w, "smoothAction")->trigger();
    QVERIFY(!w.smoothGraphs());
    QVERIFY(!w.isAnimating());
    const double end = cpu->viewEnd();
    *clock = 1002.2;
    QTest::qWait(60);
    QCOMPARE(cpu->viewEnd(), end); // without smooth graphs the view moves with samples only
    w.setSmoothGraphs(true);
    QVERIFY(w.isAnimating());

    w.hide();
    QVERIFY(!w.isAnimating());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTRY_VERIFY(w.isAnimating());
    w.showMinimized();
    QTest::qWait(50);
    if (w.isMinimized())
        QVERIFY(!w.isAnimating());
    w.showNormal();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTRY_VERIFY(w.isAnimating());
    QVERIFY(w.shutdown(1000));
    QVERIFY(!w.isAnimating());
}

void TestUi::animationNeverTouchesMeasurements()
{
    // Two windows get the same samples; one animates for a while, the other
    // does not animate at all. What was measured, and what export writes,
    // is identical.
    Env envA, envB;
    auto clock = std::make_shared<double>(2000.0);
    auto oa = envA.options();
    oa.clock = [clock] { return *clock; };
    auto ob = envB.options();
    ob.clock = oa.clock;
    MainWindow a(oa), b(ob);
    b.setSmoothGraphs(false);
    a.resize(1000, 700);
    a.show();
    QVERIFY(QTest::qWaitForWindowExposed(&a));
    QTRY_VERIFY(a.isAnimating());
    QVERIFY(!b.isAnimating());
    for (int i = 0; i < 6; ++i) {
        *clock = 2000 + i;
        auto s = snapshot(2000 + i, i > 0, 10 + 7 * i);
        a.applySnapshot(s);
        b.applySnapshot(s);
        for (int f = 0; f < 5; ++f) {
            *clock += 0.15;
            QTest::qWait(20);
        }
    }
    QCOMPARE(a.history().size(), b.history().size());
    QCOMPARE(a.history().totalSamples(), b.history().totalSamples());
    QCOMPARE(a.history().toCsv(0), b.history().toCsv(0));
    QCOMPARE(a.snapshotsApplied(), b.snapshotsApplied());
    // The readout names the measured sample, not an animated position.
    auto* cpu = child<TimeChart>(a, "cpuChart");
    cpu->setCursorTime(2004.1);
    QVERIFY2(cpu->readoutText().contains(QStringLiteral("38.0")), qPrintable(cpu->readoutText()));
    cpu->clearCursor();
}

void TestUi::themeChoiceKeepsExplicitSettings()
{
    cli::ThemeEnvironment hypr;
    hypr.currentDesktop = QStringLiteral("Hyprland");
    hypr.qt6ctConfig = QStringLiteral("/home/u/.config/qt6ct/qt6ct.conf");
    hypr.qt6ctPlugin = QStringLiteral("/usr/lib/qt6/plugins/platformthemes/libqt6ct.so");
    auto d = cli::chooseTheme(hypr);
    QVERIFY(d.requestQt6ct);
    QVERIFY(!d.pluginMissing);
    QVERIFY(d.note.contains(QStringLiteral("qt6ct")));

    auto e = hypr;
    e.platformThemeVariable = QStringLiteral("gtk3");
    d = cli::chooseTheme(e);
    QVERIFY(!d.requestQt6ct);
    QVERIFY(d.note.contains(QStringLiteral("gtk3")));

    e = hypr;
    e.themeArgument = true;
    QVERIFY(!cli::chooseTheme(e).requestQt6ct);

    // Conflicting overrides: Qt takes the command line over the variable,
    // and so does the explanation.
    e = hypr;
    e.platformThemeVariable = QStringLiteral("gtk3");
    e.themeArgument = true;
    e.themeArgumentValue = QStringLiteral("qt6ct");
    d = cli::chooseTheme(e);
    QVERIFY(!d.requestQt6ct);
    QVERIFY2(d.note.startsWith(QStringLiteral("Platform theme from the -platformtheme argument (qt6ct).")),
             qPrintable(d.note));
    QVERIFY2(d.note.contains(QStringLiteral("overrides QT_QPA_PLATFORMTHEME (gtk3)")), qPrintable(d.note));
    e.themeArgumentValue = QStringLiteral("gtk3");
    d = cli::chooseTheme(e);
    QVERIFY2(d.note.contains(QStringLiteral("argument (gtk3)")) && !d.note.contains(QStringLiteral("overrides")),
             qPrintable(d.note));
    // An argument Qt does not read ("-platformtheme=name") leaves the
    // variable in charge.
    e.themeArgumentValue.clear();
    d = cli::chooseTheme(e);
    QVERIFY(!d.requestQt6ct);
    QVERIFY2(d.note.startsWith(QStringLiteral("Platform theme from QT_QPA_PLATFORMTHEME (gtk3)")), qPrintable(d.note));
    e.platformThemeVariable.clear();
    d = cli::chooseTheme(e);
    QVERIFY(!d.requestQt6ct);
    QVERIFY2(d.note.contains(QStringLiteral("Qt does not read it")), qPrintable(d.note));

    // An empty name is still an argument: Qt then ignores the variable.
    e = hypr;
    e.platformThemeVariable = QStringLiteral("gtk3");
    e.themeArgument = true;
    e.themeArgumentCleared = true;
    d = cli::chooseTheme(e);
    QVERIFY(!d.requestQt6ct);
    QVERIFY2(d.note.contains(QStringLiteral("argument is empty"))
                 && d.note.contains(QStringLiteral("ignores QT_QPA_PLATFORMTHEME (gtk3)")),
             qPrintable(d.note));
    e.platformThemeVariable.clear();
    d = cli::chooseTheme(e);
    QVERIFY(!d.requestQt6ct);
    QVERIFY2(d.note.contains(QStringLiteral("argument is empty")) && !d.note.contains(QStringLiteral("gtk3")),
             qPrintable(d.note));

    // Arguments whose reading depends on an unknown platform: nothing is
    // claimed and nothing is requested.
    e = hypr;
    e.themeArgument = true;
    e.themeArgumentUncertain = true;
    d = cli::chooseTheme(e);
    QVERIFY(!d.requestQt6ct);
    QVERIFY2(d.note.startsWith(QStringLiteral("Not known")) && d.note.contains(QStringLiteral("xcb")),
             qPrintable(d.note));
    e.platformThemeVariable = QStringLiteral("gtk3");
    QVERIFY(!cli::chooseTheme(e).note.contains(QStringLiteral("gtk3")));

    for (const char* desktop : {"KDE", "ubuntu:GNOME", "X-Cinnamon", "XFCE", "LXQt"}) {
        e = hypr;
        e.currentDesktop = QString::fromLatin1(desktop);
        d = cli::chooseTheme(e);
        QVERIFY2(!d.requestQt6ct, desktop);
    }

    e = hypr;
    e.qt6ctConfig.clear();
    d = cli::chooseTheme(e);
    QVERIFY(!d.requestQt6ct);
    QVERIFY(!d.pluginMissing);

    e = hypr;
    e.qt6ctPlugin.clear();
    d = cli::chooseTheme(e);
    QVERIFY(!d.requestQt6ct);
    QVERIFY(d.pluginMissing);
    QVERIFY(d.note.contains(QStringLiteral("not installed")));

    e = hypr;
    e.currentDesktop.clear(); // a bare X or Wayland session with qt6ct set up
    QVERIFY(cli::chooseTheme(e).requestQt6ct);
}

void TestUi::themeProbeFindsConfigAndPlugin()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("config/qt6ct")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("plugins/platformthemes")));
    struct Var {
        const char* name;
        QByteArray old;
        bool had;
        explicit Var(const char* n) : name(n), old(qgetenv(n)), had(qEnvironmentVariableIsSet(n)) {}
        ~Var() { had ? qputenv(name, old) : qunsetenv(name); }
    } config("XDG_CONFIG_HOME"), plugins("QT_PLUGIN_PATH"), theme("QT_QPA_PLATFORMTHEME"),
        platform("QT_QPA_PLATFORM"), wayland("WAYLAND_DISPLAY"), display("DISPLAY"), session("XDG_SESSION_TYPE");
    qputenv("XDG_CONFIG_HOME", dir.filePath(QStringLiteral("config")).toLocal8Bit());
    qputenv("QT_PLUGIN_PATH", dir.filePath(QStringLiteral("plugins")).toLocal8Bit());
    qunsetenv("QT_QPA_PLATFORMTHEME");

    const char* plain[] = {"pulsescope", "--page", "cpu"};
    auto env = cli::probeThemeEnvironment(3, plain);
    QVERIFY(!env.themeArgument);
    QVERIFY(env.platformThemeVariable.isEmpty());
    QVERIFY(env.qt6ctConfig.isEmpty());

    QFile conf(dir.filePath(QStringLiteral("config/qt6ct/qt6ct.conf")));
    QVERIFY(conf.open(QIODevice::WriteOnly));
    conf.close();
    QFile lib(dir.filePath(QStringLiteral("plugins/platformthemes/libqt6ct.so")));
    QVERIFY(lib.open(QIODevice::WriteOnly));
    lib.close();
    env = cli::probeThemeEnvironment(3, plain);
    QCOMPARE(env.qt6ctConfig, conf.fileName());
    QCOMPARE(env.qt6ctPlugin, lib.fileName());

    const char* explicitTheme[] = {"pulsescope", "-platformtheme", "xdgdesktopportal"};
    QVERIFY(cli::probeThemeEnvironment(3, explicitTheme).themeArgument);
    const char* joined[] = {"pulsescope", "--platformtheme=gtk3"};
    QVERIFY(cli::probeThemeEnvironment(2, joined).themeArgument);
    qputenv("QT_QPA_PLATFORMTHEME", "gtk3");
    QCOMPARE(cli::probeThemeEnvironment(3, plain).platformThemeVariable, QStringLiteral("gtk3"));

    // The command line as Qt reads it: the value after the last
    // -platformtheme, options with a value consume theirs, and a joined
    // form carries no value.
    env = cli::probeThemeEnvironment(3, explicitTheme);
    QCOMPARE(env.platformThemeVariable, QStringLiteral("gtk3"));
    QCOMPARE(env.themeArgumentValue, QStringLiteral("xdgdesktopportal"));
    QVERIFY(cli::chooseTheme(env).note.contains(QStringLiteral("overrides QT_QPA_PLATFORMTHEME (gtk3)")));
    const char* twice[] = {"pulsescope", "--platformtheme", "gtk3", "--page", "cpu", "-platformtheme", "qt6ct"};
    env = cli::probeThemeEnvironment(7, twice);
    QVERIFY(env.themeArgument);
    QCOMPARE(env.themeArgumentValue, QStringLiteral("qt6ct"));
    QVERIFY(cli::probeThemeEnvironment(2, joined).themeArgumentValue.isEmpty());
    const char* consumed[] = {"pulsescope", "-qwindowtitle", "-platformtheme", "--platform", "-platformtheme"};
    env = cli::probeThemeEnvironment(5, consumed);
    QVERIFY(!env.themeArgument);
    QVERIFY(env.themeArgumentValue.isEmpty());
    const char* trailing[] = {"pulsescope", "-platformtheme"};
    env = cli::probeThemeEnvironment(2, trailing);
    QVERIFY(env.themeArgument);
    QVERIFY(env.themeArgumentValue.isEmpty());
    QVERIFY(!env.themeArgumentCleared);
    QVERIFY(cli::chooseTheme(env).note.startsWith(QStringLiteral("Platform theme from QT_QPA_PLATFORMTHEME (gtk3)")));

    // An explicit empty name replaces the variable's.
    const char* empty[] = {"pulsescope", "-platformtheme", "", "--theme-info"};
    env = cli::probeThemeEnvironment(4, empty);
    QVERIFY(env.themeArgument && env.themeArgumentCleared && env.themeArgumentValue.isEmpty());
    QVERIFY2(!cli::chooseTheme(env).note.contains(QStringLiteral("from QT_QPA_PLATFORMTHEME")),
             qPrintable(cli::chooseTheme(env).note));
    const char* emptyThenName[] = {"pulsescope", "-platformtheme", "", "-platformtheme", "qt6ct"};
    env = cli::probeThemeEnvironment(5, emptyThenName);
    QVERIFY(!env.themeArgumentCleared);
    QCOMPARE(env.themeArgumentValue, QStringLiteral("qt6ct"));
    const char* nameThenEmpty[] = {"pulsescope", "--platformtheme", "qt6ct", "--platformtheme", ""};
    env = cli::probeThemeEnvironment(5, nameThenEmpty);
    QVERIFY(env.themeArgumentCleared && env.themeArgumentValue.isEmpty());

    // -title, -geometry and -icon take a value only on xcb, decided per
    // argument from the platform Qt starts with and any -platform before.
    struct Session {
        const char* platform; // QT_QPA_PLATFORM
        const char* wayland;  // WAYLAND_DISPLAY
        const char* display;  // DISPLAY
        const char* type;     // XDG_SESSION_TYPE
    };
    auto use = [](const Session& s) {
        auto set = [](const char* name, const char* value) { value ? qputenv(name, value) : qunsetenv(name); };
        set("QT_QPA_PLATFORM", s.platform);
        set("WAYLAND_DISPLAY", s.wayland);
        set("DISPLAY", s.display);
        set("XDG_SESSION_TYPE", s.type);
    };
    struct Reading {
        bool argument;
        const char* value;
        bool uncertain;
    };
    auto expect = [](std::initializer_list<const char*> args, Reading want, const char* what) {
        std::vector<const char*> argv {"pulsescope"};
        argv.insert(argv.end(), args.begin(), args.end());
        const auto got = cli::probeThemeEnvironment(static_cast<int>(argv.size()), argv.data());
        const QString problem = QStringLiteral("%1: argument %2, value \"%3\", uncertain %4")
                                    .arg(QLatin1String(what))
                                    .arg(got.themeArgument)
                                    .arg(got.themeArgumentValue)
                                    .arg(got.themeArgumentUncertain);
        QVERIFY2(got.themeArgument == want.argument && got.themeArgumentValue == QLatin1String(want.value)
                     && got.themeArgumentUncertain == want.uncertain,
                 qPrintable(problem));
    };
    const Session hyprland {nullptr, "wayland-1", ":0", "wayland"};
    use(hyprland);
    expect({"-platform", "xcb", "-title", "-platformtheme", "--theme-info"}, {false, "", false},
           "-title after -platform xcb");
    expect({"-title", "-platformtheme", "--theme-info"}, {true, "--theme-info", false}, "-title on Wayland");
    expect({"-title", "-platformtheme", "gtk3", "-platform", "xcb"}, {true, "gtk3", false}, "-platform xcb after");
    expect({"-platform", "xcb", "--platform", "wayland", "-icon", "-platformtheme", "gtk3"}, {true, "gtk3", false},
           "last -platform wayland");
    expect({"--platform", "xcb:nomitshm", "--geometry", "-platformtheme", "qt6ct"}, {false, "", false},
           "--geometry on xcb with options");
    use({"xcb", "wayland-1", ":0", "wayland"});
    expect({"-icon", "-platformtheme", "gtk3"}, {false, "", false}, "QT_QPA_PLATFORM=xcb");
    use({nullptr, nullptr, ":0", "x11"});
    expect({"-title", "-platformtheme", "gtk3"}, {false, "", false}, "X11 session");
    expect({"-qwindowtitle", "x", "-platformtheme", "gtk3"}, {true, "gtk3", false}, "X11 session, -qwindowtitle");
    use({nullptr, nullptr, ":0", "wayland"});
    expect({"-title", "-platformtheme", "gtk3"}, {true, "gtk3", false}, "Wayland session type");
    // Nothing tells which platform Qt starts with.
    use({nullptr, nullptr, nullptr, "tty"});
    expect({"-title", "-platformtheme", "gtk3"}, {true, "", true}, "unknown platform");
    expect({"-title", "Monitor", "--page", "cpu"}, {false, "", false}, "unknown platform, no theme either way");
    expect({"-platformtheme", "gtk3", "-geometry", "640x480"}, {true, "gtk3", false},
           "unknown platform, same theme either way");
    expect({"-platform", "wayland", "-title", "-platformtheme", "gtk3"}, {true, "gtk3", false},
           "unknown start, -platform wayland first");
    use({"offscreen", nullptr, nullptr, nullptr});
    expect({"-title", "-platformtheme", "gtk3"}, {true, "gtk3", false}, "QT_QPA_PLATFORM=offscreen");
    const char* unknown[] = {"pulsescope", "-title", "-platformtheme", "gtk3"};
    use({nullptr, nullptr, nullptr, nullptr});
    const auto d = cli::chooseTheme(cli::probeThemeEnvironment(4, unknown));
    QVERIFY(!d.requestQt6ct);
    QVERIFY2(d.note.startsWith(QStringLiteral("Not known")), qPrintable(d.note));
}

void TestUi::partialFramesMatchFullRepaint()
{
    // Frames between samples copy cached layers and repaint only the rows
    // from the drawn data down. What is on screen after any such sequence
    // must equal a complete repaint of the same state, also in the rows the
    // frames skipped: a spike leaving the window, a sample arriving without
    // a refresh, a gap and a reveal all pass through.
    auto add = [](ps::History& h, double t, double v, bool continuous = true) {
        QVERIFY(h.beginSample(t, continuous && !h.empty()));
        h.set("k", "Load", ps::Unit::Percent, v);
        h.endSample();
    };
    ps::History history(200, 1e9);
    for (int i = 0; i <= 40; ++i)
        add(history, 1000 + i, i == 11 ? 90 : 5 + (i % 3));

    TimeChart chart(QStringLiteral("Load"), ps::Unit::Percent);
    chart.setHistory(&history);
    chart.setSeries({{"k", QStringLiteral("Load")}});
    chart.setWindowSeconds(30);
    chart.resize(600, 320);
    chart.setViewEnd(1040);
    chart.show();
    QVERIFY(QTest::qWaitForWindowExposed(&chart));
    QCoreApplication::processEvents();

    struct PaintRects : QObject {
        std::vector<QRect> rects;
        bool eventFilter(QObject*, QEvent* e) override
        {
            if (e->type() == QEvent::Paint)
                rects.push_back(static_cast<QPaintEvent*>(e)->rect());
            return false;
        }
    } paints;
    chart.installEventFilter(&paints);
    QScreen* screen = chart.screen();
    QVERIFY(screen);
    auto check = [&](const char* when, int fromRow = 0) {
        QCoreApplication::processEvents();
        const QImage shown = screen->grabWindow(chart.winId()).toImage().convertToFormat(QImage::Format_RGB32);
        const QImage full = chart.grab().toImage().convertToFormat(QImage::Format_RGB32);
        QVERIFY2(!shown.isNull() && shown.size() == full.size(), when);
        int differ = 0;
        QRect box;
        for (int y = fromRow; y < full.height(); ++y) {
            const auto* a = reinterpret_cast<const QRgb*>(shown.constScanLine(y));
            const auto* b = reinterpret_cast<const QRgb*>(full.constScanLine(y));
            for (int x = 0; x < full.width(); ++x) {
                if (a[x] != b[x]) {
                    ++differ;
                    box |= QRect(x, y, 1, 1);
                }
            }
        }
        const QString problem = QStringLiteral("%1: %2 pixels on screen differ from a full repaint, in %3,%4 %5x%6")
                                    .arg(QLatin1String(when))
                                    .arg(differ)
                                    .arg(box.x())
                                    .arg(box.y())
                                    .arg(box.width())
                                    .arg(box.height());
        QVERIFY2(differ == 0, qPrintable(problem));
    };

    double end = 1040;
    auto frames = [&](int n) {
        for (int f = 0; f < n; ++f) {
            end += 1.0 / 60;
            chart.advanceView(end);
            QCoreApplication::processEvents();
        }
    };
    // The spike (t 1011) enters the window's left edge, leaves it, and its
    // segment scrolls out.
    frames(40);
    check("spike at the left edge");
    frames(80);
    check("spike left the window");
    frames(20);
    check("spike's segment scrolled out");
    paints.rects.clear();
    frames(60);
    check("low load scrolling");
    const auto partial = std::count_if(paints.rects.begin(), paints.rects.end(),
                                       [&](const QRect& r) { return r.height() < chart.height() / 3; });
    QVERIFY2(partial >= 50, qPrintable(QStringLiteral("only %1 of %2 frames repainted just the data rows")
                                           .arg(partial).arg(paints.rects.size())));

    // A higher sample arrives without refresh(): the next frame repaints the
    // whole plot, not only the old data rows. (The legend value above the
    // plot waits for the refresh.)
    add(history, 1041, 70);
    frames(10);
    check("sample arrived without a refresh", chart.height() / 5);
    chart.refresh();
    check("refreshed");

    // A gap (outage shading spans the plot) and a revealed sample.
    add(history, 1046, 8, false);
    chart.setRevealEnabled(true);
    add(history, 1047, 40);
    chart.refresh();
    end = 1047;
    chart.advanceView(end);
    frames(6);
    QVERIFY(chart.isRevealing());
    check("mid-reveal");
    frames(24);
    QVERIFY(!chart.isRevealing());
    check("gap and reveal");
    frames(3 * 60);
    check("gap scrolling");
}

void TestUi::bandFramesMatchFullRepaint()
{
    // Lines that stay in a narrow band high in the plot: frames repaint that
    // band and the columns the areas' vertical edges (their ends, holes and
    // outage shading) and the reveal cut cross, not all the rows below the
    // lines. The screen must still equal a complete repaint after each step.
    struct Paints : QObject {
        int frames = 0;
        int banded = 0; // frames repainting a region of several rectangles
        double area = 0;
        bool eventFilter(QObject*, QEvent* e) override
        {
            if (e->type() == QEvent::Paint) {
                const QRegion& r = static_cast<QPaintEvent*>(e)->region();
                ++frames;
                banded += r.rectCount() > 1;
                for (const QRect& x : r)
                    area += static_cast<double>(x.width()) * x.height();
            }
            return false;
        }
        void reset() { frames = banded = 0, area = 0; }
    };
    auto check = [](TimeChart& chart, const QString& when, int fromRow = 0) {
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QScreen* screen = chart.screen();
        QVERIFY(screen);
        const QImage shown = screen->grabWindow(chart.winId()).toImage().convertToFormat(QImage::Format_RGB32);
        const QImage full = chart.grab().toImage().convertToFormat(QImage::Format_RGB32);
        QVERIFY2(!shown.isNull() && shown.size() == full.size(), qPrintable(when));
        int differ = 0;
        QRect box;
        for (int y = fromRow; y < full.height(); ++y) {
            const auto* a = reinterpret_cast<const QRgb*>(shown.constScanLine(y));
            const auto* b = reinterpret_cast<const QRgb*>(full.constScanLine(y));
            for (int x = 0; x < full.width(); ++x) {
                if (a[x] != b[x]) {
                    ++differ;
                    box |= QRect(x, y, 1, 1);
                }
            }
        }
        QVERIFY2(differ == 0, qPrintable(QStringLiteral("%1: %2 pixels on screen differ from a full repaint, in %3,%4 %5x%6")
                                              .arg(when)
                                              .arg(differ)
                                              .arg(box.x())
                                              .arg(box.y())
                                              .arg(box.width())
                                              .arg(box.height())));
    };
    auto run = [](TimeChart& chart, double& end, int n) {
        for (int f = 0; f < n; ++f) {
            end += 1.0 / 60;
            chart.advanceView(end);
            QCoreApplication::processEvents();
        }
    };

    // A young history (its first samples are inside the window) of two
    // overlapping filled series near 85 %; the lower one has a hole.
    ps::History history(400, 1e9);
    auto add = [&](int i, bool continuous = true) {
        QVERIFY(history.beginSample(1000 + i, continuous && !history.empty()));
        const bool hole = i == 20 || i == 21;
        history.set("a", "A", ps::Unit::Percent, 86 + (i * 7) % 3);
        history.set("b", "B", ps::Unit::Percent, hole ? ps::kNaN : 81 + (i % 2));
        history.endSample();
    };
    for (int i = 0; i <= 40; ++i)
        add(i);
    TimeChart chart(QStringLiteral("Load"), ps::Unit::Percent);
    chart.setHistory(&history);
    chart.setSeries({{"a", QStringLiteral("A"), 0, true}, {"b", QStringLiteral("B"), 1, true}});
    chart.setWindowSeconds(60);
    chart.resize(700, 360);
    double end = 1040;
    chart.setViewEnd(end);
    chart.show();
    QVERIFY(QTest::qWaitForWindowExposed(&chart));
    QCoreApplication::processEvents();

    Paints paints;
    chart.installEventFilter(&paints);
    run(chart, end, 50);
    check(chart, QStringLiteral("band scrolling"));
    if (QTest::currentTestFailed())
        return;
    const QRectF plot = chart.plotArea();
    const double plotArea = plot.width() * plot.height();
    QVERIFY2(paints.frames >= 40 && paints.banded * 10 >= paints.frames * 9,
             qPrintable(QStringLiteral("%1 of %2 frames repainted the band").arg(paints.banded).arg(paints.frames)));
    // The rows from the lines down are nearly the whole plot.
    QVERIFY2(paints.area / paints.frames < 0.3 * plotArea,
             qPrintable(QStringLiteral("frames repainted %1 px on average, the plot has %2")
                            .arg(paints.area / paints.frames)
                            .arg(plotArea)));

    // Past the images' scrolling margin they are redrawn for a later end.
    run(chart, end, 260);
    check(chart, QStringLiteral("layer redrawn for a later end"));

    // A paint the chart did not ask for, at a later end, moves only part of
    // the plot; the next frame brings the rest along.
    end += 0.4;
    chart.setViewEnd(end);
    chart.update(QRectF(plot.center(), QSizeF(40, 40)).toAlignedRect());
    QCoreApplication::processEvents();
    run(chart, end, 1);
    check(chart, QStringLiteral("partial paint from outside"));
    run(chart, end, 20);
    check(chart, QStringLiteral("scrolling after it"));

    // The view moved back.
    end -= 2.5;
    chart.advanceView(end);
    QCoreApplication::processEvents();
    check(chart, QStringLiteral("view moved back"));
    run(chart, end, 30);
    check(chart, QStringLiteral("scrolling after moving back"));

    // A sample arrives without refresh() and the first paint of the new
    // history is one the chart did not ask for: the rest of the plot still
    // shows the old history and is repainted too. (The legend value above
    // the plot waits for the refresh.)
    add(41);
    chart.update(QRectF(plot.center(), QSizeF(40, 40)).toAlignedRect());
    QCoreApplication::processEvents();
    run(chart, end, 3);
    check(chart, QStringLiteral("new history first painted from outside"), static_cast<int>(plot.top()) - 1);
    chart.refresh();

    // A revealed sample.
    chart.setRevealEnabled(true);
    add(42);
    chart.refresh();
    end = std::max(end, 1042.0);
    chart.advanceView(end);
    run(chart, end, 6);
    QVERIFY(chart.isRevealing());
    check(chart, QStringLiteral("mid-reveal"));
    paints.reset();
    run(chart, end, 30);
    QVERIFY(!chart.isRevealing());
    check(chart, QStringLiteral("after the reveal"));
    QVERIFY2(paints.banded * 2 >= paints.frames,
             qPrintable(QStringLiteral("%1 of %2 frames repainted the band").arg(paints.banded).arg(paints.frames)));

    // An outage: its shading spans the plot height and scrolls.
    add(46, false);
    add(47);
    chart.refresh();
    end = std::max(end, 1047.0);
    chart.advanceView(end);
    run(chart, end, 30);
    check(chart, QStringLiteral("outage revealed"));
    paints.reset();
    run(chart, end, 90);
    check(chart, QStringLiteral("outage scrolling"));
    QVERIFY2(paints.banded * 2 >= paints.frames,
             qPrintable(QStringLiteral("%1 of %2 frames repainted the band").arg(paints.banded).arg(paints.frames)));

    // Temperatures below zero, filled down to the plot bottom (-10 °C).
    ps::History temps(400, 1e9);
    for (int i = 0; i <= 50; ++i) {
        QVERIFY(temps.beginSample(1000 + i, i > 0));
        temps.set("t", "Probe", ps::Unit::Celsius, -2 + 0.3 * (i % 3));
        temps.endSample();
    }
    TimeChart celsius(QStringLiteral("Probe"), ps::Unit::Celsius);
    celsius.setHistory(&temps);
    celsius.setSeries({{"t", QStringLiteral("Probe"), 6, true}});
    celsius.setWindowSeconds(60);
    celsius.resize(520, 300);
    double cend = 1050;
    celsius.setViewEnd(cend);
    celsius.show();
    QVERIFY(QTest::qWaitForWindowExposed(&celsius));
    QVERIFY(celsius.displayedRange().first < 0);
    Paints cpaints;
    celsius.installEventFilter(&cpaints);
    run(celsius, cend, 90);
    check(celsius, QStringLiteral("signed temperature"));
    QVERIFY2(cpaints.banded * 2 >= cpaints.frames,
             qPrintable(QStringLiteral("%1 of %2 frames repainted the band").arg(cpaints.banded).arg(cpaints.frames)));

    // A small multiple.
    TimeChart core(QStringLiteral("CPU 3"), ps::Unit::Percent);
    core.setMode(TimeChart::Mode::Compact);
    core.setHistory(&history);
    core.setSeries({{"a", QStringLiteral("A"), 0, true}});
    core.setWindowSeconds(60);
    core.resize(220, 90);
    double kend = end;
    core.setViewEnd(kend);
    core.show();
    QVERIFY(QTest::qWaitForWindowExposed(&core));
    run(core, kend, 90);
    check(core, QStringLiteral("small multiple"));
}

void TestUi::cursorFramesMatchFullRepaint()
{
    // A readout cursor scrolls with its sample: frames repaint its old and
    // new place with the band (here low in the plot, far below the box),
    // not the whole chart, and the screen equals a
    // complete repaint, also when the box flips sides, the sample under the
    // mouse changes, a paint the chart did not ask for moves only part of
    // it, and the cursor goes away.
    struct Paints : QObject {
        int frames = 0;
        int whole = 0; // frames repainting the whole chart
        QRect all;
        bool eventFilter(QObject*, QEvent* e) override
        {
            if (e->type() == QEvent::Paint) {
                ++frames;
                whole += static_cast<QPaintEvent*>(e)->rect() == all;
            }
            return false;
        }
        void reset() { frames = whole = 0; }
    };
    auto check = [](TimeChart& chart, const QString& when) {
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QScreen* screen = chart.screen();
        QVERIFY(screen);
        const QImage shown = screen->grabWindow(chart.winId()).toImage().convertToFormat(QImage::Format_RGB32);
        const QImage full = chart.grab().toImage().convertToFormat(QImage::Format_RGB32);
        QVERIFY2(!shown.isNull() && shown.size() == full.size(), qPrintable(when));
        int differ = 0;
        QRect box;
        for (int y = 0; y < full.height(); ++y) {
            const auto* a = reinterpret_cast<const QRgb*>(shown.constScanLine(y));
            const auto* b = reinterpret_cast<const QRgb*>(full.constScanLine(y));
            for (int x = 0; x < full.width(); ++x) {
                if (a[x] != b[x]) {
                    ++differ;
                    box |= QRect(x, y, 1, 1);
                }
            }
        }
        QVERIFY2(differ == 0, qPrintable(QStringLiteral("%1: %2 pixels on screen differ from a full repaint, in %3,%4 %5x%6")
                                              .arg(when)
                                              .arg(differ)
                                              .arg(box.x())
                                              .arg(box.y())
                                              .arg(box.width())
                                              .arg(box.height())));
    };
    auto run = [](TimeChart& chart, double& end, int n, double step = 1.0 / 60) {
        for (int f = 0; f < n; ++f) {
            end += step;
            chart.advanceView(end);
            QCoreApplication::processEvents();
        }
    };

    ps::History history(400, 1e9);
    for (int i = 0; i <= 60; ++i) {
        QVERIFY(history.beginSample(1000 + i, i > 0));
        history.set("a", "A", ps::Unit::Percent, 5 + (i * 7) % 10);
        history.set("b", "B", ps::Unit::Percent, i == 30 ? ps::kNaN : 16 - (i % 2));
        history.endSample();
    }
    TimeChart chart(QStringLiteral("Load"), ps::Unit::Percent);
    chart.setHistory(&history);
    chart.setSeries({{"a", QStringLiteral("A"), 0, true}, {"b", QStringLiteral("B"), 1, false}});
    chart.setFixedRange(0, 100);
    chart.setWindowSeconds(30);
    chart.resize(640, 320);
    double end = 1060;
    chart.setViewEnd(end);
    chart.show();
    QVERIFY(QTest::qWaitForWindowExposed(&chart));
    QCoreApplication::processEvents();
    Paints paints;
    paints.all = chart.rect();
    chart.installEventFilter(&paints);

    // A sample picked from the keyboard, its box right of the line.
    chart.setCursorTime(1040);
    QCoreApplication::processEvents();
    paints.reset();
    run(chart, end, 90);
    check(chart, QStringLiteral("keyboard cursor scrolling"));
    if (QTest::currentTestFailed())
        return;
    QVERIFY2(paints.frames >= 80 && paints.whole * 10 <= paints.frames,
             qPrintable(QStringLiteral("%1 of %2 frames repainted the whole chart").arg(paints.whole).arg(paints.frames)));

    // The latest sample: the box starts left of the line and flips right of
    // it as the sample scrolls away from the plot's right edge.
    chart.setCursorTime(1060);
    QCoreApplication::processEvents();
    run(chart, end, 6);
    check(chart, QStringLiteral("box left of the line"));
    run(chart, end, 90, 0.1);
    check(chart, QStringLiteral("box flipped right"));

    // A paint the chart did not ask for, at a later end, moves only part of
    // the cursor; the next frames bring the rest along.
    const QRectF plot = chart.plotArea();
    end += 0.3;
    chart.setViewEnd(end);
    chart.update(QRectF(plot.center(), QSizeF(30, 30)).toAlignedRect());
    QCoreApplication::processEvents();
    run(chart, end, 2);
    check(chart, QStringLiteral("partial paint from outside"));

    // The mouse: the line stays under it and snaps from sample to sample.
    const QPointF pos(plot.left() + plot.width() * 0.6, plot.center().y());
    QMouseEvent move(QEvent::MouseMove, pos, chart.mapToGlobal(pos), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&chart, &move);
    QCoreApplication::processEvents();
    const QString before = chart.readoutText();
    paints.reset();
    run(chart, end, 150);
    QVERIFY(chart.readoutText() != before);
    check(chart, QStringLiteral("mouse cursor over scrolling samples"));
    QVERIFY2(paints.whole * 10 <= paints.frames,
             qPrintable(QStringLiteral("%1 of %2 frames repainted the whole chart").arg(paints.whole).arg(paints.frames)));

    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&chart, &leave);
    QVERIFY(chart.readoutText().isEmpty());
    run(chart, end, 3);
    check(chart, QStringLiteral("cursor gone"));

    // Markers at the plot's top edge reach above it.
    ps::History high(100, 1e9);
    for (int i = 0; i <= 60; ++i) {
        QVERIFY(high.beginSample(1000 + i, i > 0));
        high.set("a", "A", ps::Unit::Percent, 30 + (i * 7) % 20);
        high.set("b", "B", ps::Unit::Percent, 100 - (i % 2));
        high.endSample();
    }
    TimeChart top(QStringLiteral("Load"), ps::Unit::Percent);
    top.setHistory(&high);
    top.setSeries({{"a", QStringLiteral("A"), 0, true}, {"b", QStringLiteral("B"), 1, false}});
    top.setFixedRange(0, 100);
    top.setWindowSeconds(30);
    top.resize(640, 320);
    double tend = 1060;
    top.setViewEnd(tend);
    top.show();
    QVERIFY(QTest::qWaitForWindowExposed(&top));
    top.setCursorTime(1040);
    QCoreApplication::processEvents();
    run(top, tend, 90);
    check(top, QStringLiteral("markers at the top edge"));
}

void TestUi::layerCacheIgnoresClockMagnitude()
{
    // History times are on CLOCK_BOOTTIME, past 1e7 s after about 116 days
    // of uptime. The same data 2e7 s later must still be drawn from the
    // cached layers with partial frames, and look exactly the same.
    auto build = [](ps::History& h, double base) {
        for (int i = 0; i <= 40; ++i) {
            QVERIFY(h.beginSample(base + 1000 + i, i > 0 && i != 5));
            h.set("k", "Load", ps::Unit::Percent, i == 11 ? 90 : (i == 25 ? 40 : 5 + (i % 3)));
            h.endSample();
        }
    };
    constexpr double kLater = 2e7;
    ps::History near(200, 1e9), far(200, 1e9);
    build(near, 0);
    build(far, kLater);
    TimeChart a(QStringLiteral("Load"), ps::Unit::Percent), b(QStringLiteral("Load"), ps::Unit::Percent);
    for (auto [chart, history] : {std::pair {&a, &near}, std::pair {&b, &far}}) {
        chart->setHistory(history);
        chart->setSeries({{"k", QStringLiteral("Load")}});
        chart->setWindowSeconds(30);
        chart->resize(600, 320);
        chart->show();
        QVERIFY(QTest::qWaitForWindowExposed(chart));
    }
    // Exact binary steps, so both clocks land on the same relative times.
    double offset = 1040;
    a.setViewEnd(offset);
    b.setViewEnd(kLater + offset);
    QCoreApplication::processEvents();

    struct PaintRects : QObject {
        std::vector<QRect> rects;
        bool eventFilter(QObject*, QEvent* e) override
        {
            if (e->type() == QEvent::Paint)
                rects.push_back(static_cast<QPaintEvent*>(e)->rect());
            return false;
        }
    } paints, control;
    b.installEventFilter(&paints);
    a.installEventFilter(&control);
    auto shown = [](TimeChart& c) {
        return c.screen()->grabWindow(c.winId()).toImage().convertToFormat(QImage::Format_RGB32);
    };
    for (int round = 0; round < 4; ++round) {
        for (int f = 0; f < 60; ++f) {
            offset += 1.0 / 64;
            a.advanceView(offset);
            b.advanceView(kLater + offset);
            QCoreApplication::processEvents();
        }
        const QImage na = shown(a), nb = shown(b);
        QVERIFY(!na.isNull() && na.size() == nb.size());
        QVERIFY2(na == nb, qPrintable(QStringLiteral("round %1: a clock 2e7 s later draws differently").arg(round)));
        QVERIFY2(nb == b.grab().toImage().convertToFormat(QImage::Format_RGB32),
                 qPrintable(QStringLiteral("round %1: frames differ from a full repaint").arg(round)));
    }
    auto partial = [&](const PaintRects& p) {
        return std::count_if(p.rects.begin(), p.rects.end(), [&](const QRect& r) { return r.height() < b.height() / 3; });
    };
    // (grab() adds a full paint of the far chart per round.)
    QVERIFY2(partial(paints) >= 90 && partial(paints) == partial(control),
             qPrintable(QStringLiteral("%1 of %2 frames at 2e7 s repainted just the data rows, %3 of %4 at 1e3 s")
                            .arg(partial(paints))
                            .arg(paints.rects.size())
                            .arg(partial(control))
                            .arg(control.rects.size())));
}

void TestUi::navSparklineClipsMeasuredGeometry()
{
    // The sample before the window enters along its measured segment. The
    // scale covers the visible samples only, so that sample may be off it:
    // its segment is clipped where it crosses the edge (line and area
    // alike), never drawn from a point pulled onto the edge.
    struct Case {
        const char* name;
        ps::Unit unit;
        double before;
        double first;
        bool continuous;  // first continues from before
        bool fullColumns; // left of first the area covers the whole height
    };
    const Case cases[] = {
        {"peak before the window", ps::Unit::BytesPerSecond, 1000, 0, true, true},
        {"cold reading before the window", ps::Unit::Celsius, -40, 50, true, false},
        {"hot reading before the window", ps::Unit::Celsius, 400, 50, true, true},
        {"peak before a gap", ps::Unit::BytesPerSecond, 1000, 0, false, false},
    };
    for (const Case& c : cases) {
        const bool celsius = c.unit == ps::Unit::Celsius;
        auto build = [&](ps::History& h, bool withBefore) {
            auto add = [&](double t, double v, bool continuous) {
                QVERIFY(h.beginSample(t, continuous && !h.empty()));
                h.set("k", "K", c.unit, v);
                h.endSample();
            };
            if (withBefore)
                add(5, c.before, true);
            add(10, c.first, c.continuous);
            for (int i = 0; i < 5; ++i)
                add(11 + i, (celsius ? 50 : 0) + (i % 2 ? 1 : 0.5), true);
        };
        ps::History with, without;
        build(with, true);
        build(without, false);
        // A 10 s sparkline ending at 15.1: the sample at 5 is just outside.
        auto render = [&](const ps::History& h, QRectF* area) {
            MetricButton b(QStringLiteral("Sensor"));
            b.setSparkline(&h, {"k"}, 0, 10);
            b.setViewEnd(15.1);
            b.setValueText(QStringLiteral("1"));
            b.resize(b.sizeHint());
            const QImage img = b.grab().toImage().convertToFormat(QImage::Format_RGB32);
            *area = b.sparklineArea();
            return img;
        };
        QRectF area, area2;
        const QImage a = render(with, &area);
        const QImage ref = render(without, &area2);
        QVERIFY2(!a.isNull() && a.size() == ref.size() && area == area2, c.name);
        QVERIFY2(area.width() > 40 && area.height() > 10, c.name);
        const double dpr = a.devicePixelRatio();
        const double x10 = (area.left() + 0.49 * area.width()) * dpr;
        const int left = static_cast<int>(std::ceil(area.left() * dpr)) + 2;
        const int right = static_cast<int>(std::floor(x10)) - 3;
        const int top = static_cast<int>(std::ceil(area.top() * dpr)) + 1;
        const int bottom = static_cast<int>(std::floor(area.bottom() * dpr)) - 2;
        QVERIFY2(right - left > 10 && bottom - top > 8, c.name);
        // Above the sparkline nothing is drawn there: the area stops at its
        // top edge, and the line crosses it only near the first sample.
        int above = 0;
        for (int y = 0; y < top - 1; ++y)
            for (int x = left; x <= right; ++x)
                above += a.pixel(x, y) != ref.pixel(x, y);
        QVERIFY2(above == 0, qPrintable(QStringLiteral("%1: %2 pixels above the sparkline differ")
                                            .arg(QLatin1String(c.name))
                                            .arg(above)));
        int differ = 0, uneven = 0, unfilled = 0;
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                differ += a.pixel(x, y) != ref.pixel(x, y);
                uneven += a.pixel(x, y) != a.pixel(left, top);
                unfilled += a.pixel(x, y) == ref.pixel(x, y);
            }
        }
        const QString where = QStringLiteral("%1: %2 pixels differ, %3 uneven, %4 unfilled left of the first sample")
                                  .arg(QLatin1String(c.name))
                                  .arg(differ)
                                  .arg(uneven)
                                  .arg(unfilled);
        if (c.fullColumns) {
            // The true segment stays above the scale until just before the
            // first sample: a full-height area, no line.
            QVERIFY2(uneven == 0 && unfilled == 0, qPrintable(where));
        } else {
            QVERIFY2(differ == 0, qPrintable(where));
        }
        // From just after the first sample on, both are the same drawing (a
        // line continuing from an earlier segment may antialias a level
        // apart from one starting there).
        int after = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = static_cast<int>(std::ceil(x10)) + 3; x < a.width(); ++x) {
                const QRgb pa = a.pixel(x, y), pr = ref.pixel(x, y);
                after += std::abs(qRed(pa) - qRed(pr)) > 2 || std::abs(qGreen(pa) - qGreen(pr)) > 2
                    || std::abs(qBlue(pa) - qBlue(pr)) > 2;
            }
        QVERIFY2(after == 0, qPrintable(QStringLiteral("%1: %2 pixels differ after the first sample")
                                            .arg(QLatin1String(c.name))
                                            .arg(after)));
    }
}

void TestUi::temperatureLooksOutlastEightSensors()
{
    // More listed sensors than colours: a sensor that stays listed keeps its
    // look while unchecked, and a newly plotted one gets a look of its own
    // (a repeated colour in another line style) instead of taking it.
    Env env;
    MainWindow w(env.options());
    const std::vector<std::string> ids {"a", "b", "c", "d", "e", "f", "g", "h", "i"};
    std::map<std::string, double> readings;
    for (std::size_t n = 0; n < ids.size(); ++n)
        readings[ids[n]] = 40 + static_cast<double>(n);
    readings["b"] = 90; // the navigation shows another sensor than A
    double t = 100;
    auto apply = [&] {
        auto s = snapshot(t, t > 100);
        s->temperatures.clear();
        for (const auto& [id, celsius] : readings)
            s->temperatures.push_back({"hwmon.chip." + id, "chip", id, "chip " + id, "/sys/" + id, celsius});
        w.applySnapshot(s);
        t += 1;
    };
    apply();
    apply();
    auto* list = child<QTreeWidget>(w, "sensorList");
    auto* chart = child<TimeChart>(w, "temperatureChart");
    auto* nav = child<MetricButton>(w, "navTemperatures");
    QCOMPARE(list->topLevelItemCount(), 9);
    using Look = std::pair<int, Qt::PenStyle>;
    auto looks = [&] {
        std::map<std::string, Look> out;
        for (const auto& cs : chart->series())
            out[cs.key] = {cs.visual, cs.style.value_or(seriesStyle(cs.visual))};
        return out;
    };
    auto key = [](const std::string& id) { return ps::keys::temperature("hwmon.chip." + id); };
    auto check = [&](const std::string& id, Qt::CheckState state) {
        for (int i = 0; i < list->topLevelItemCount(); ++i)
            if (list->topLevelItem(i)->data(0, Qt::UserRole).toString() == QString::fromStdString("hwmon.chip." + id))
                list->topLevelItem(i)->setCheckState(0, state);
    };
    auto distinct = [](const std::map<std::string, Look>& m) {
        std::set<Look> seen;
        for (const auto& entry : m)
            if (!seen.insert(entry.second).second)
                return false;
        return true;
    };
    for (const auto& id : ids)
        check(id, id == "i" ? Qt::Unchecked : Qt::Checked);
    const auto eight = looks();
    QCOMPARE(eight.size(), std::size_t(8));
    QVERIFY(distinct(eight));
    const Look a = eight.at(key("a"));

    check("a", Qt::Unchecked);
    check("i", Qt::Checked);
    auto now = looks();
    QCOMPARE(now.size(), std::size_t(8));
    QVERIFY(distinct(now));
    QVERIFY2(now.at(key("i")) != a, "the new sensor took the look of a listed one");
    for (const auto& id : ids)
        if (id != "a" && id != "i")
            QVERIFY2(now.at(key(id)) == eight.at(key(id)), id.c_str());
    const Look i = now.at(key("i"));

    check("a", Qt::Checked);
    now = looks();
    QCOMPARE(now.size(), std::size_t(9));
    QVERIFY(distinct(now));
    QVERIFY(now.at(key("a")) == a);
    QVERIFY(now.at(key("i")) == i);

    // Samples, a palette change, Details and more toggling change no look;
    // the navigation draws the hottest sensor in its chart look.
    const auto nine = now;
    QPalette dark = w.palette();
    dark.setColor(QPalette::Window, QColor(30, 30, 46));
    dark.setColor(QPalette::Base, QColor(24, 24, 37));
    dark.setColor(QPalette::WindowText, QColor(205, 214, 244));
    dark.setColor(QPalette::Text, QColor(205, 214, 244));
    w.setPalette(dark);
    apply();
    w.setDetailsVisible(true);
    check("a", Qt::Unchecked);
    check("c", Qt::Unchecked);
    readings["a"] = 95;
    apply();
    QVERIFY(nav->sparklineVisuals().size() == 1 && nav->sparklineVisuals().front() == a);
    check("c", Qt::Checked);
    check("a", Qt::Checked);
    w.setDetailsVisible(false);
    w.setPalette(QPalette());
    apply();
    QVERIFY(looks() == nine);
    QVERIFY(nav->sparklineVisuals().front() == a);
    QVERIFY(w.shutdown(1000));
}

void TestUi::temperatureLooksFollowSensors()
{
    // A plotted sensor keeps its colour and line style whatever happens to
    // the others, and the navigation sparkline draws the hottest sensor in
    // the look it has on the chart.
    Env env;
    MainWindow w(env.options());
    std::map<std::string, double> readings {{"a", 70}, {"b", 50}, {"c", 40}};
    double t = 100;
    auto apply = [&] {
        auto s = snapshot(t, t > 100);
        s->temperatures.clear();
        for (const auto& [id, celsius] : readings)
            s->temperatures.push_back({"hwmon.chip." + id, "chip", id, "chip " + id, "/sys/" + id, celsius});
        w.applySnapshot(s);
        t += 1;
    };
    apply();
    apply();
    auto* list = child<QTreeWidget>(w, "sensorList");
    auto* chart = child<TimeChart>(w, "temperatureChart");
    auto* nav = child<MetricButton>(w, "navTemperatures");
    QCOMPARE(list->topLevelItemCount(), 3);
    using Look = std::pair<int, Qt::PenStyle>;
    auto looks = [&] {
        std::map<std::string, Look> out;
        for (std::size_t i = 0; i < chart->series().size(); ++i) {
            const auto& cs = chart->series()[i];
            out[cs.key] = {cs.visual, cs.style.value_or(seriesStyle(cs.visual))};
        }
        return out;
    };
    auto key = [](const char* id) { return ps::keys::temperature(std::string("hwmon.chip.") + id); };
    auto item = [&](const char* id) {
        for (int i = 0; i < list->topLevelItemCount(); ++i)
            if (list->topLevelItem(i)->data(0, Qt::UserRole).toString() == QStringLiteral("hwmon.chip.") + id)
                return list->topLevelItem(i);
        return static_cast<QTreeWidgetItem*>(nullptr);
    };
    auto navLook = [&] {
        const auto v = nav->sparklineVisuals();
        return v.size() == 1 ? v.front() : Look {-99, Qt::NoPen};
    };
    auto distinct = [&](const std::map<std::string, Look>& m) {
        std::set<Look> seen;
        for (const auto& [k, look] : m)
            if (!seen.insert(look).second)
                return false;
        return true;
    };

    const auto first = looks();
    QCOMPARE(first.size(), std::size_t(3));
    QVERIFY(distinct(first));
    QVERIFY(navLook() == first.at(key("a"))); // hottest, as on the chart

    // Unchecking the first sensor restyles none of the others; the
    // navigation still shows it (it is the hottest) in its own look.
    item("a")->setCheckState(0, Qt::Unchecked);
    auto now = looks();
    QCOMPARE(now.size(), std::size_t(2));
    QVERIFY(now.at(key("b")) == first.at(key("b")));
    QVERIFY(now.at(key("c")) == first.at(key("c")));
    apply();
    QVERIFY(navLook() == first.at(key("a")));
    for (const auto& [k, look] : now)
        QVERIFY2(look != navLook(), k.c_str());

    // A new sensor gets a look nobody on show has; checking the first one
    // again brings back its own.
    readings["d"] = 45;
    apply();
    item("d")->setCheckState(0, Qt::Checked);
    now = looks();
    QCOMPARE(now.size(), std::size_t(3));
    QVERIFY(distinct(now));
    QVERIFY(now.at(key("d")) != first.at(key("a")));
    item("a")->setCheckState(0, Qt::Checked);
    now = looks();
    QCOMPARE(now.size(), std::size_t(4));
    QVERIFY(distinct(now));
    QVERIFY(now.at(key("a")) == first.at(key("a")));
    QVERIFY(now.at(key("b")) == first.at(key("b")));

    // Another sensor becomes the hottest: the navigation follows it, in its
    // chart look.
    readings["c"] = 90;
    apply();
    QVERIFY(nav->detailText().contains(QStringLiteral("chip c")));
    QVERIFY(navLook() == now.at(key("c")));
    QVERIFY(looks() == now);

    // Details on and off and the chart's page change nothing.
    w.setDetailsVisible(true);
    w.showPage(MainWindow::TemperaturePage);
    w.setDetailsVisible(false);
    QVERIFY(looks() == now);

    // Looks are indices into the palette's colours, so a palette change
    // recolours the sparkline instead of keeping old colours. (A page other
    // than the sensors' keeps the entry unselected, in its series colour.)
    readings["a"] = 95;
    apply();
    w.showPage(MainWindow::CpuPage);
    QVERIFY(navLook() == now.at(key("a")));
    QCOMPARE(navLook().second, Qt::SolidLine);
    nav->resize(nav->sizeHint());
    QPalette pals[2] = {nav->palette(), nav->palette()};
    for (int dark = 0; dark < 2; ++dark) {
        pals[dark].setColor(QPalette::Window, dark ? QColor(30, 30, 46) : QColor(239, 239, 239));
        pals[dark].setColor(QPalette::Base, dark ? QColor(24, 24, 37) : QColor(255, 255, 255));
        pals[dark].setColor(QPalette::WindowText, dark ? QColor(205, 214, 244) : QColor(20, 20, 20));
        pals[dark].setColor(QPalette::Text, dark ? QColor(205, 214, 244) : QColor(20, 20, 20));
    }
    const QColor wants[2] = {seriesColor(navLook().first, pals[0]), seriesColor(navLook().first, pals[1])};
    QVERIFY(wants[0] != wants[1]);
    for (int dark = 0; dark < 2; ++dark) {
        nav->setPalette(pals[dark]);
        const QImage img = nav->grab().toImage().convertToFormat(QImage::Format_RGB32);
        // The pixel closest to each palette's colour, in the sparkline.
        const QRect spark = QRectF(nav->sparklineArea().topLeft() * img.devicePixelRatio(),
                                   nav->sparklineArea().size() * img.devicePixelRatio())
                                .toAlignedRect()
                                .intersected(img.rect());
        QVERIFY(!spark.isEmpty());
        int best[2] = {1000, 1000};
        for (int y = spark.top(); y <= spark.bottom(); ++y)
            for (int x = spark.left(); x <= spark.right(); ++x) {
                const QColor p = img.pixelColor(x, y);
                for (int k = 0; k < 2; ++k)
                    best[k] = std::min(best[k], std::abs(p.red() - wants[k].red()) + std::abs(p.green() - wants[k].green())
                                                    + std::abs(p.blue() - wants[k].blue()));
            }
        QVERIFY2(best[dark] < 45 && best[dark] < best[1 - dark],
                 qPrintable(QStringLiteral("%1 palette: line colour %2 away, other palette's %3")
                                .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                                .arg(best[dark])
                                .arg(best[1 - dark])));
    }
    QVERIFY(w.shutdown(1000));
}

// Details pages in a short window with a large font scroll; they never
// squeeze their charts, fact rows, per-core charts or sensor list.
void TestUi::detailsScrollInsteadOfSqueezing()
{
    const QFont appFont = QApplication::font();
    QFont big = appFont;
    big.setPointSizeF(appFont.pointSizeF() * 1.6);
    QApplication::setFont(big);
    struct Restore {
        QFont f;
        ~Restore() { QApplication::setFont(f); }
    } restore {appFont};

    Env env;
    MainWindow w(env.options());
    w.setDetailsVisible(true);
    feed(w, 10, 3);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.resize(w.minimumSizeHint().width(), 500);
    QTest::qWait(20);
    auto* stack = w.findChild<QStackedWidget*>();
    QVERIFY(stack);
    int scrolling = 0;
    for (int p = 0; p < MainWindow::ProcessPage; ++p) {
        w.showPage(static_cast<MainWindow::Page>(p));
        QTest::qWait(20);
        const QString where = QStringLiteral("page %1 at %2x%3: ").arg(p).arg(w.width()).arg(w.height());
        auto* scroll = qobject_cast<QScrollArea*>(stack->currentWidget());
        QWidget* content = scroll && scroll->widget() ? scroll->widget() : stack->currentWidget();
        QVERIFY2(content->width() >= content->minimumSizeHint().width(),
                 qPrintable(where + QStringLiteral("page narrower than its minimum")));
        int charts = 0;
        for (auto* chart : content->findChildren<TimeChart*>()) {
            if (!chart->isVisible() || chart->parentWidget()->objectName() == QLatin1String("coreGrid"))
                continue;
            ++charts;
            QVERIFY2(chart->height() >= chart->minimumSizeHint().height(),
                     qPrintable(where + QStringLiteral("%1 height %2 < %3")
                                            .arg(chart->objectName())
                                            .arg(chart->height())
                                            .arg(chart->minimumSizeHint().height())));
        }
        QVERIFY2(charts >= 1, qPrintable(where + QStringLiteral("no chart")));
        int pairs = 0;
        for (auto* name : content->findChildren<QLabel*>()) {
            auto* value = qobject_cast<QLabel*>(name->buddy());
            if (!value || !name->isVisible())
                continue;
            ++pairs;
            for (QLabel* l : {name, value})
                QVERIFY2(l->height() >= l->sizeHint().height(),
                         qPrintable(where + QStringLiteral("'%1' height %2 < %3")
                                                .arg(l->text())
                                                .arg(l->height())
                                                .arg(l->sizeHint().height())));
        }
        if (p <= MainWindow::NetworkPage)
            QVERIFY2(pairs >= 4, qPrintable(where + QStringLiteral("%1 fact pairs").arg(pairs)));
        QVERIFY2(scroll, qPrintable(where + QStringLiteral("not in a scroll area")));
        scrolling += scroll->verticalScrollBar()->maximum() > 0;
    }
    // Otherwise this window is tall enough and nothing above was at stake.
    QVERIFY2(scrolling >= 2, qPrintable(QStringLiteral("%1 pages scroll at %2x%3").arg(scrolling).arg(w.width()).arg(w.height())));

    w.showPage(MainWindow::CpuPage);
    QTest::qWait(20);
    // At least one whole row of per-core charts is in view.
    auto* cores = child<QScrollArea>(w, "coreScroll");
    auto* coreGrid = child<QWidget>(w, "coreGrid");
    const auto coreCharts = coreGrid->findChildren<TimeChart*>();
    QVERIFY(!coreCharts.isEmpty());
    QVERIFY2(cores->viewport()->height() >= coreCharts.front()->geometry().bottom(),
             qPrintable(QStringLiteral("per-core view %1 px, first row ends at %2")
                            .arg(cores->viewport()->height())
                            .arg(coreCharts.front()->geometry().bottom())));
    // Scrolling brings a chart below the fold into view.
    auto* page = qobject_cast<QScrollArea*>(stack->currentWidget());
    QVERIFY(page);
    page->ensureWidgetVisible(cores);
    QTest::qWait(20);
    QVERIFY(!cores->visibleRegion().isEmpty());

    w.showPage(MainWindow::TemperaturePage);
    QTest::qWait(20);
    auto* list = child<QTreeWidget>(w, "sensorList");
    QVERIFY(list->isVisible());
    const int nameWidth = QFontMetrics(list->font()).horizontalAdvance(QStringLiteral("thinkpad \u00b7 temp5"));
    QVERIFY2(list->width() >= list->minimumWidth() && list->minimumWidth() > nameWidth,
             qPrintable(QStringLiteral("sensor list %1 px, minimum %2, name %3")
                            .arg(list->width())
                            .arg(list->minimumWidth())
                            .arg(nameWidth)));
    w.close();
}

// A chart too short for all its grid labels keeps every k-th one instead of
// drawing them over each other.
void TestUi::shortChartsKeepAxisLabelsApart()
{
    ps::History h;
    for (int i = 0; i < 10; ++i) {
        h.beginSample(100 + i, i > 0);
        h.set("v", "V", ps::Unit::Percent, 40 + i, ps::SeriesGroup::Summary);
        h.endSample();
    }
    TimeChart chart(QString(), ps::Unit::Percent); // no title ink beside the labels
    QFont big = chart.font();
    big.setPointSizeF(big.pointSizeF() * 1.6);
    chart.setFont(big);
    chart.setHistory(&h);
    chart.setSeries({{"v", QStringLiteral("V")}});
    chart.setLegendVisible(false);
    chart.setFixedRange(0, 100);
    chart.setWindowSeconds(60);
    const QFontMetrics fm(big);
    // Left of the plot only the value labels are drawn (time labels stay
    // inside the plot's width).
    const int stripWidth = 8 + fm.horizontalAdvance(QStringLiteral("100%")) + 3;
    for (const int height : {chart.minimumSizeHint().height(), 4 * fm.height() + 24, 3 * fm.height() + 24}) {
        chart.resize(420, height);
        const QImage img = chart.grab().toImage().convertToFormat(QImage::Format_RGB32);
        const QRgb base = img.pixel(1, 1);
        std::vector<std::pair<int, int>> runs;
        int start = -1;
        for (int y = 0; y <= img.height(); ++y) {
            bool ink = false;
            for (int x = 0; y < img.height() && x < stripWidth && !ink; ++x)
                ink = img.pixel(x, y) != base;
            if (ink && start < 0)
                start = y;
            if (!ink && start >= 0) {
                runs.emplace_back(start, y);
                start = -1;
            }
        }
        const QString where = QStringLiteral("height %1 (font height %2): ").arg(height).arg(fm.height());
        QVERIFY2(!runs.empty(), qPrintable(where + QStringLiteral("no axis labels")));
        for (const auto& [a, b] : runs)
            QVERIFY2(b - a <= fm.height(), qPrintable(where + QStringLiteral("labels overlap in rows %1-%2").arg(a).arg(b)));
    }
    QVERIFY(chart.minimumSizeHint().height() >= 6 * fm.height());
}

void TestUi::timeAxisLabelsNeverTouch()
{
    ps::History h;
    for (int i = 0; i <= 900; i += 3) {
        h.beginSample(1000 + i, i > 0);
        h.set("v", "V", ps::Unit::Percent, 40, ps::SeriesGroup::Summary);
        h.endSample();
    }
    TimeChart chart(QString(), ps::Unit::Percent);
    chart.setHistory(&h);
    chart.setSeries({{"v", QStringLiteral("V")}});
    chart.setLegendVisible(false);
    chart.setFixedRange(0, 100);
    chart.setRevealEnabled(false);
    struct Case {
        double window;
        int width;
    };
    // 120 s gives "−1 min 45 s" labels at 15 s steps; 300 s on a wide plot
    // gives "−1 min 30 s" at 30 s steps.
    const Case cases[] = {{60, 320}, {120, 900}, {300, 1900}, {300, 2600}, {900, 1000}};
    for (const double scale : {1.0, 1.25}) {
        QFont font(QStringLiteral("monospace"));
        font.setStyleHint(QFont::Monospace);
        font.setPointSizeF(11 * scale);
        chart.setFont(font);
        const QFontMetrics fm(font);
        const int widest = fm.horizontalAdvance(QStringLiteral("\u221214 min 45 s"));
        for (const bool simple : {false, true}) {
            chart.setSimple(simple);
            for (const Case& c : cases) {
                chart.setWindowSeconds(c.window);
                chart.resize(c.width, 260);
                chart.setViewEnd(1900);
                chart.refresh();
                const QImage img = chart.grab().toImage().convertToFormat(QImage::Format_RGB32);
                const QRgb base = img.pixel(1, 1);
                const int y0 = img.height() - 6 - fm.height(), y1 = img.height() - 8;
                const int plotLeft = 8 + fm.horizontalAdvance(QStringLiteral("100%")) + 8;
                // Ink columns grouped into labels: a word space stays inside a
                // label, a wider blank separates two labels.
                std::vector<std::pair<int, int>> labels;
                int blank = 0;
                for (int x = plotLeft; x < img.width() - 6; ++x) {
                    bool ink = false;
                    for (int y = y0; y <= y1 && !ink; ++y)
                        ink = img.pixel(x, y) != base;
                    if (!ink) {
                        ++blank;
                        continue;
                    }
                    if (labels.empty() || blank > 1.5 * fm.averageCharWidth())
                        labels.emplace_back(x, x + 1);
                    else
                        labels.back().second = x + 1;
                    blank = 0;
                }
                const QString where = QStringLiteral("%1 s window, %2 px, font x%3, %4: ")
                                          .arg(c.window)
                                          .arg(c.width)
                                          .arg(scale)
                                          .arg(simple ? "simple" : "details");
                QVERIFY2(labels.size() >= 3, qPrintable(where + QStringLiteral("only %1 time labels").arg(labels.size())));
                for (const auto& [a, b] : labels)
                    QVERIFY2(b - a <= widest + 2,
                             qPrintable(where + QStringLiteral("labels run together at x %1-%2").arg(a).arg(b)));
            }
        }
    }
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
