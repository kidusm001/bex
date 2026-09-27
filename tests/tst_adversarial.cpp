// Adversarial regression tests for the pure /proc and /sys parsers and the
// delta math built directly on them. Expected values are derived by hand from
// the documented kernel formats (proc(5), Documentation/admin-guide/iostats.rst,
// Documentation/filesystems/proc.rst), never by re-running production logic.
//
// Every test states the kernel convention it relies on. A failing assertion is
// a finding; assertions must not be relaxed just to make the suite pass.

#include "app/collector.h"
#include "core/format.h"
#include "core/history.h"
#include "core/linuxparsers.h"
#include "core/metrics.h"
#include "core/sampler.h"
#include "core/snapshot.h"

#include <QCoreApplication>
#if defined(QT_WIDGETS_LIB) && __has_include("app/timechart.h")
#define PS_ADV_WIDGETS 1
#include "app/timechart.h"
#if __has_include("app/metricbutton.h")
#define PS_ADV_METRICBUTTON 1
#include "app/metricbutton.h"
#endif
#if __has_include("app/mainwindow.h") && __has_include("app/cli.h")
#define PS_ADV_MAINWINDOW 1
#include "app/cli.h"
#include "app/csvexporter.h"
#include "app/mainwindow.h"
#include "app/processmodel.h"
#include <QAction>
#include <QFileDialog>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStatusBar>
#include <QLayout>
#include <QLineEdit>
#include <QTimer>
#include <QTextStream>
#include <sys/resource.h>
#include <system_error>
#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QTableView>
#include <QTreeWidget>
#endif
#include <QApplication>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#endif
#include <QPointer>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QThread>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

using namespace std::string_literals;
using namespace std::string_view_literals;
namespace parse = ps::parse;
namespace metrics = ps::metrics;

namespace {

constexpr uint64_t kU64Max = std::numeric_limits<uint64_t>::max();

bool near(double actual, double expected, double tolerance = 1e-9)
{
    return std::fabs(actual - expected) <= tolerance;
}

// Fields 3..52 of /proc/<pid>/stat with every value distinct so a column
// mix-up is visible:
//   state=S ppid=7 pgrp=1234 session=1235 tty_nr=0 tpgid=-1 flags=4194560
//   minflt=100 cminflt=101 majflt=102 cmajflt=103 utime=250 stime=75
//   cutime=-3 cstime=-4 priority=15 nice=-5 num_threads=3 itrealvalue=0
//   starttime=98765 vsize=12345678 rss=456 rsslim=18446744073709551615 ...
constexpr std::string_view kStatTail =
    "S 7 1234 1235 0 -1 4194560 100 101 102 103 250 75 -3 -4 15 -5 3 0 98765 12345678 456 "
    "18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 2 0 0 0 0 0 0 0 0 0 0 0 0 0\n";

std::string statLine(std::string_view pid, std::string_view comm, std::string_view tail = kStatTail)
{
    std::string s(pid);
    s += " (";
    s += comm;
    s += ") ";
    s += tail;
    return s;
}

// One /proc/net/dev data line with distinct values in all 16 columns.
std::string netLine(std::string_view name, std::string_view rx = "1111", std::string_view tx = "9999")
{
    std::string s(name);
    s += ": ";
    s += rx;
    s += " 12 13 14 15 16 17 18 ";
    s += tx;
    s += " 22 23 24 25 26 27 28\n";
    return s;
}

parse::MemInfoRaw rawMem(uint64_t totalKb, std::optional<uint64_t> availKb, std::optional<uint64_t> freeKb)
{
    parse::MemInfoRaw r;
    r.memTotal = totalKb * 1024;
    if (availKb)
        r.memAvailable = *availKb * 1024;
    if (freeKb)
        r.memFree = *freeKb * 1024;
    return r;
}

// ---- synthetic /proc + /sys trees for the Sampler (procRoot/sysRoot injection)

namespace fs = std::filesystem;

class FixtureRoot
{
public:
    bool ok() const { return m_dir.isValid(); }
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
    void rename(std::string_view from, std::string_view to) const { fs::rename(path(from), path(to)); }
    void symlinkDir(std::string_view targetRel, std::string_view linkRel) const
    {
        mkdir(targetRel);
        fs::create_directories(fs::path(path(linkRel)).parent_path());
        fs::create_directory_symlink(path(targetRel), path(linkRel));
    }

private:
    QTemporaryDir m_dir;
};

std::string u(uint64_t v) { return std::to_string(v); }

// /proc/stat cpu line: user nice system idle iowait irq softirq steal guest guest_nice
std::string cpuLine(std::string_view name, uint64_t user, uint64_t system, uint64_t idle,
                    uint64_t iowait = 0, uint64_t guest = 0)
{
    return std::string(name) + " " + u(user) + " 0 " + u(system) + " " + u(idle) + " " + u(iowait)
        + " 0 0 0 " + u(guest) + " 0\n";
}

// One /proc/diskstats line (14 tokens; extra statistics are not needed).
std::string diskLine(unsigned major, unsigned minor, std::string_view name, uint64_t sectorsRead,
                     uint64_t sectorsWritten, uint64_t ioTicks)
{
    return u(major) + " " + u(minor) + " " + std::string(name) + " 1 0 " + u(sectorsRead) + " 0 1 0 "
        + u(sectorsWritten) + " 0 0 " + u(ioTicks) + " 0\n";
}

constexpr std::string_view kNetHeader =
    "Inter-|   Receive                                                |  Transmit\n"
    " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n";

std::string netDevLine(std::string_view name, uint64_t rx, uint64_t tx)
{
    return std::string(name) + ": " + u(rx) + " 1 0 0 0 0 0 0 " + u(tx) + " 1 0 0 0 0 0 0\n";
}

std::string pidStat(int pid, std::string_view comm, uint64_t utime, uint64_t stime, uint64_t start,
                    unsigned flags = 4194560, int threads = 1, int64_t rss = 456)
{
    return u(uint64_t(pid)) + " (" + std::string(comm) + ") S 1 " + u(uint64_t(pid)) + " " + u(uint64_t(pid))
        + " 0 -1 " + u(flags) + " 0 0 0 0 " + u(utime) + " " + u(stime) + " 0 0 20 0 " + std::to_string(threads)
        + " 0 " + u(start) + " 1000000 " + std::to_string(rss)
        + " 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n";
}

void writeBaseTree(const FixtureRoot& r)
{
    r.write("proc/stat", cpuLine("cpu", 0, 0, 0) + cpuLine("cpu0", 0, 0, 0));
    r.write("proc/meminfo", "MemTotal: 1000 kB\nMemFree: 100 kB\nMemAvailable: 500 kB\n");
    r.write("proc/loadavg", "0.10 0.20 0.30 1/10 99\n");
    r.write("proc/uptime", "1000.00 0.00\n");
    r.write("proc/diskstats", "");
    r.write("proc/net/dev", kNetHeader);
    r.write("proc/mounts", "");
    r.write("proc/self/mountinfo", "");
    r.mkdir("sys/block");
    r.mkdir("sys/class/net");
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

const ps::DiskIo* findDisk(const ps::Snapshot& s, std::string_view name)
{
    for (const auto& d : s.disks)
        if (d.name == name)
            return &d;
    return nullptr;
}

const ps::NetIo* findNet(const ps::Snapshot& s, std::string_view name)
{
    for (const auto& n : s.interfaces)
        if (n.name == name)
            return &n;
    return nullptr;
}

const ps::ProcessInfo* findProc(const ps::Snapshot& s, int pid)
{
    for (const auto& p : s.processes)
        if (p.pid == pid)
            return &p;
    return nullptr;
}

const ps::CoreUsage* findCore(const ps::Snapshot& s, int index)
{
    for (const auto& c : s.cores)
        if (c.index == index)
            return &c;
    return nullptr;
}

bool hasProblem(const ps::Snapshot& s, std::string_view text)
{
    return std::any_of(s.problems.begin(), s.problems.end(),
                       [&](const std::string& p) { return p.find(text) != std::string::npos; });
}

std::set<std::string> temperatureKeys(const ps::Snapshot& s)
{
    std::set<std::string> out;
    for (const auto& t : s.temperatures)
        out.insert(t.key);
    return out;
}

const ps::TemperatureReading* findTemp(const ps::Snapshot& s, std::string_view label)
{
    for (const auto& t : s.temperatures)
        if (t.label == label)
            return &t;
    return nullptr;
}

} // namespace

class TestAdversarial : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ---- numeric primitives ------------------------------------------------
    void toU64Strictness_data();
    void toU64Strictness();
    void toI64Strictness_data();
    void toI64Strictness();
    void toDoubleStrictness_data();
    void toDoubleStrictness();
    void splitAndSanitize();

    // ---- /proc/stat --------------------------------------------------------
    void procStatModernLineKeepsGuestSeparate();
    void procStatHotplugGapsAndOrdering();
    void procStatRejectsMalformedLines_data();
    void procStatRejectsMalformedLines();
    void procStatShortLegacyLineDefaultsMissingFieldsToZero();
    void procStatBtime();
    void procStatCrlfAndTabs();
    void cpuTimesTotalDoesNotSilentlyWrap();
    void cpuDeltaGuestCountedOnce();
    void cpuDeltaIowaitRegressionInvalidates();
    void cpuDeltaOtherRegressionInvalidates();
    void cpuDeltaZeroElapsedIsUnavailable();
    void cpuUsageNeverOutsidePercentRange();

    // ---- /proc/meminfo -----------------------------------------------------
    void meminfoRealisticKbToBytes();
    void meminfoRejectsBadValues_data();
    void meminfoRejectsBadValues();
    void meminfoMissingUnitIsNotBytes();
    void meminfoOverflowBoundary();
    void memoryUsedIsTotalMinusAvailable();
    void memoryMissingAvailableNoMemFreeSubstitution();
    void memoryImpossibleValuesRejected();
    void memoryZeroAvailableIsRealZero();
    void memoryZeroTotalInvalid();
    void memoryCacheSumCannotWrap();
    void swapConventions();

    // ---- /proc/diskstats ---------------------------------------------------
    void diskstatsExtendedFieldColumns();
    void diskstatsLegacy14Tokens();
    void diskstatsRejectsMalformed_data();
    void diskstatsRejectsMalformed();
    void diskstatsSectorsAre512ByteUnits();
    void diskstatsNamesKeptVerbatim();

    // ---- /proc/net/dev -----------------------------------------------------
    void netDevRealisticWithHeaders();
    void netDevLegacyNoSpaceAfterColon();
    void netDevMaxLengthName();
    void netDevLegalPipeInName();
    void netDevRejectsMalformed_data();
    void netDevRejectsMalformed();
    void netDevOddButLegalNamesPreserved();

    // ---- /proc/<pid>/stat --------------------------------------------------
    void pidStatAllColumns();
    void pidStatHostileComm_data();
    void pidStatHostileComm();
    void pidStatKernelThreadFlag();
    void pidStatStartTimePreservedAsU64();
    void pidStatRejectsMalformed_data();
    void pidStatRejectsMalformed();

    // ---- /proc/<pid>/io, status, cmdline -----------------------------------
    void pidIo_data();
    void pidIo();
    void statusRealUid_data();
    void statusRealUid();
    void cmdline_data();
    void cmdline();

    // ---- /proc/mounts ------------------------------------------------------
    void mountUnescape_data();
    void mountUnescape();
    void mountUnescapeNeverProducesNul();
    void mountsParsing();
    void mountInfoParsing();
    void mountResolution();
    void mountReadOnly_data();
    void mountReadOnly();
    void localCapacityFs_data();
    void localCapacityFs();

    // ---- temperature, loadavg, uptime --------------------------------------
    void milliCelsius_data();
    void milliCelsius();
    void loadavg_data();
    void loadavg();
    void uptime_data();
    void uptime();

    // ---- counter / rate / process math built on parser output --------------
    void counterDeltaSemantics();
    void rateSemantics();
    void processCpuPercentOneCoreIs100();
    void continuity();

    // ---- display boundary for adversarial values ---------------------------
    void formatUnavailableAndHuge();

    // ---- History: bounded, time-aligned ring ---------------------------------
    void historyCapacityBound();
    void historyRealTimeCutoffAndGaps();
    void historyRejectsBadTimesAndValues();
    void historyMissingValuesAreGapsNotZero();
    void historyDeviceChurnStaysBounded();
    void historySeriesCapAndRecovery();
    void historyLowerBound();
    void historyCsvQuotingAndBlanks();
    void historyRecordSnapshotConventions();

    // ---- Sampler over injected /proc and /sys trees --------------------------
    void samplerFirstSampleIsBaselineOnly();
    void samplerCpuCountersAbove2p53();
    void samplerCpuGuestAndIdleAccounting();
    void samplerCpuIowaitRegressionInvalidatesThenRebases();
    void samplerCpuHotplug();
    void samplerCpuSourceLossAndRecovery();
    void samplerGapResetAndBackwardsTime();
    void samplerIrregularElapsedUsesActualInterval();
    void samplerMemoryOverlapZeroSwapAndLoss();
    void samplerDiskUnitsPartitionsAndAggregate();
    void samplerDiskLargeCountersResetAndIdentity();
    void samplerDiskChurnAndBusyClamp();
    void samplerNetKindsAggregateAndIdentity();
    void samplerNetContainerFallbackAndLoss();
    void samplerNetSanitizedNamesStayDistinct();
    void samplerProcessCpuAgeRssIo();
    void samplerProcessPidReuseWithHigherTicks();
    void samplerProcessExecKeepsCpuHistory();
    void samplerProcessKernelThreadAndVanish();
    void samplerProcessIoDenied();
    void samplerTemperaturesSignedZeroFault();
    void samplerTemperatureIdentitySurvivesRenumbering();
    void samplerTemperatureDuplicateChipsDisambiguated();
    void samplerThermalZoneFallbackAndNotes();
    void samplerFilesystemsGroupingSafetyAndErrors();
    void samplerFailedProcessScanInvalidatesBaselines();
    void samplerOvermountedLocalNotProbedThroughRemote();
    void samplerOvermountedLocalByLocal();
    void samplerCpuTruncatedRowDoesNotSpike();
    void samplerProcessIoDeniedRecovers();
    void samplerTemperatureMalformedFaultIsUnavailable();
    void samplerTemperatureUnreadableFaultIsUnavailable();
    void samplerTemperatureCachedPathNotSplicedToNewDevice();
    void samplerBtrfsSubvolumeDeviceIdentity();
    void samplerHostRootCapacityWithDefaultProbe();
    void samplerLateCapacityResultCannotReviveOldMount();
    void samplerLateCapacityErrorCannotStampNewMount();
    void samplerBlockedCapacityProbeDoesNotStallCoreMetrics();

    // ---- Collector thread facade (QtCore event loop) -------------------------
    void collectorPausedBeforeStartThenResumeSamples();
    void collectorStopDropsInFlightSnapshots();
    void collectorBlockedGuiDeliveryBounded();
    void collectorStopBoundedWhenWorkerBlocked();
    void collectorOldGenerationAcksDoNotReleaseCurrentPermit();

    // ---- Spec pass 3: numeric fidelity and pinned process identity -----------
    void memoryMissingOrInvalidSReclaimableIsNotZero();
    void samplerProcessTickSumOverflowIsNotPlausibleCpu();
    void samplerProcessDirReplacedMidReadDoesNotMixFields();

    void historyHighCardinalityKeepsEssentialSeries_data();
    void historyHighCardinalityKeepsEssentialSeries();

    // ---- TimeChart (QtWidgets, offscreen) ------------------------------------
    void chartFixedRangeHonouredForEveryUnit();
    void chartInvalidFixedRangeFallsBackToAuto();
    void chartOffWindowAndOutOfRangeLinesClipped();
    void chartGapsAndUnavailableSamplesNotBridged();
    void chartCursorReadoutKeyboardAndAccessibility();
    void chartCursorNeverReadsOffWindowSample();
    void chartNoVisibleSampleAfterClockAdvance_data();
    void chartNoVisibleSampleAfterClockAdvance();
    void metricButtonSparklineTracksNegativeCelsius_data();
    void metricButtonSparklineTracksNegativeCelsius();
    void cliRejectsMalformedNumbers_data();
    void cliRejectsMalformedNumbers();
    void hostRootFilesystemHasCapacity();
    void mainWindowStaleThenPauseDoesNotResurrect();
    void mainWindowKnownSwapTotalUnknownFree_data();
    void mainWindowKnownSwapTotalUnknownFree();
    void mainWindowUnpluggedSensorKeepsBoundedPlaceholder();
    void mainWindowTemperatureNavSingleSource();
    void mainWindowElidedIdentitiesHaveFullTooltips();
    void mainWindowFactGridsVisibleAboveCharts_data();
    void mainWindowFactGridsVisibleAboveCharts();
    void mainWindowReportsHistoryTruncation();
    void mainWindowCsvExportNeverBlocksUi();
    // RC-3: snapshot age, interactive export entry, exporter thread start failure
    void mainWindowOldFrameKeepsStaleAndChartClock();
    void mainWindowDelayedGuiDeliveryKeepsStale();
    void mainWindowSlowCollectionKeepsStaleWithoutRateSpike();
    void mainWindowInteractiveExportUsesChooser();
    void csvExportThreadStartFailureIsRetryable();
};

// ===========================================================================
// numeric primitives

void TestAdversarial::toU64Strictness_data()
{
    QTest::addColumn<QByteArray>("in");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<quint64>("value");
    QTest::newRow("zero") << QByteArray("0") << true << quint64(0);
    QTest::newRow("leading zeros are decimal") << QByteArray("007") << true << quint64(7);
    QTest::newRow("max") << QByteArray("18446744073709551615") << true << quint64(kU64Max);
    QTest::newRow("max+1") << QByteArray("18446744073709551616") << false << quint64(0);
    QTest::newRow("huge") << QByteArray("99999999999999999999999") << false << quint64(0);
    QTest::newRow("negative") << QByteArray("-1") << false << quint64(0);
    QTest::newRow("negative zero") << QByteArray("-0") << false << quint64(0);
    QTest::newRow("plus") << QByteArray("+1") << false << quint64(0);
    QTest::newRow("empty") << QByteArray("") << false << quint64(0);
    QTest::newRow("leading space") << QByteArray(" 1") << false << quint64(0);
    QTest::newRow("trailing space") << QByteArray("1 ") << false << quint64(0);
    QTest::newRow("trailing junk") << QByteArray("12abc") << false << quint64(0);
    QTest::newRow("hex") << QByteArray("0x10") << false << quint64(0);
    QTest::newRow("decimal point") << QByteArray("1.0") << false << quint64(0);
    QTest::newRow("exponent") << QByteArray("1e3") << false << quint64(0);
    QTest::newRow("embedded NUL") << QByteArray("1\0" "2", 3) << false << quint64(0);
    QTest::newRow("arabic-indic digit") << QByteArray("\xd9\xa1") << false << quint64(0);
}

void TestAdversarial::toU64Strictness()
{
    QFETCH(QByteArray, in);
    QFETCH(bool, ok);
    QFETCH(quint64, value);
    const auto r = parse::toU64(std::string_view(in.constData(), size_t(in.size())));
    QCOMPARE(r.has_value(), ok);
    if (ok)
        QCOMPARE(quint64(*r), value);
}

void TestAdversarial::toI64Strictness_data()
{
    QTest::addColumn<QByteArray>("in");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<qint64>("value");
    QTest::newRow("min") << QByteArray("-9223372036854775808") << true << std::numeric_limits<qint64>::min();
    QTest::newRow("max") << QByteArray("9223372036854775807") << true << std::numeric_limits<qint64>::max();
    QTest::newRow("min-1") << QByteArray("-9223372036854775809") << false << qint64(0);
    QTest::newRow("max+1") << QByteArray("9223372036854775808") << false << qint64(0);
    QTest::newRow("minus one") << QByteArray("-1") << true << qint64(-1);
    QTest::newRow("bare minus") << QByteArray("-") << false << qint64(0);
    QTest::newRow("double minus") << QByteArray("--1") << false << qint64(0);
    QTest::newRow("plus") << QByteArray("+1") << false << qint64(0);
    QTest::newRow("trailing junk") << QByteArray("-5x") << false << qint64(0);
    QTest::newRow("empty") << QByteArray("") << false << qint64(0);
}

void TestAdversarial::toI64Strictness()
{
    QFETCH(QByteArray, in);
    QFETCH(bool, ok);
    QFETCH(qint64, value);
    const auto r = parse::toI64(std::string_view(in.constData(), size_t(in.size())));
    QCOMPARE(r.has_value(), ok);
    if (ok)
        QCOMPARE(qint64(*r), value);
}

void TestAdversarial::toDoubleStrictness_data()
{
    QTest::addColumn<QByteArray>("in");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<double>("value");
    QTest::newRow("fixed") << QByteArray("0.52") << true << 0.52;
    QTest::newRow("negative") << QByteArray("-1.5") << true << -1.5;
    QTest::newRow("integer") << QByteArray("3") << true << 3.0;
    QTest::newRow("nan") << QByteArray("nan") << false << 0.0;
    QTest::newRow("NAN") << QByteArray("NAN") << false << 0.0;
    QTest::newRow("inf") << QByteArray("inf") << false << 0.0;
    QTest::newRow("-infinity") << QByteArray("-infinity") << false << 0.0;
    QTest::newRow("exponent") << QByteArray("1e3") << false << 0.0;
    QTest::newRow("hexfloat") << QByteArray("0x1p3") << false << 0.0;
    QTest::newRow("plus") << QByteArray("+1.0") << false << 0.0;
    QTest::newRow("comma decimal") << QByteArray("0,52") << false << 0.0;
    QTest::newRow("trailing junk") << QByteArray("0.52x") << false << 0.0;
    QTest::newRow("empty") << QByteArray("") << false << 0.0;
}

void TestAdversarial::toDoubleStrictness()
{
    QFETCH(QByteArray, in);
    QFETCH(bool, ok);
    QFETCH(double, value);
    const auto r = parse::toDouble(std::string_view(in.constData(), size_t(in.size())));
    QCOMPARE(r.has_value(), ok);
    if (ok)
        QVERIFY(near(*r, value));
}

void TestAdversarial::splitAndSanitize()
{
    const auto lines = parse::splitLines("a\n\nb\n");
    QCOMPARE(lines.size(), size_t(3));
    QCOMPARE(lines[0], "a"sv);
    QCOMPARE(lines[1], ""sv);
    QCOMPARE(lines[2], "b"sv);
    QVERIFY(parse::splitLines("").empty());
    QCOMPARE(parse::splitLines("no-newline").size(), size_t(1));

    const auto t = parse::splitWhitespace(" \t a\t\tb \r\n c\f\v");
    QCOMPARE(t.size(), size_t(3));
    QCOMPARE(t[0], "a"sv);
    QCOMPARE(t[1], "b"sv);
    QCOMPARE(t[2], "c"sv);

    // C0 controls and DEL become '?'; bytes >= 0x80 (UTF-8) are left for the UI.
    QCOMPARE(parse::sanitize("a\x1b[31mb\x7f\tc\n"s), "a?[31mb??c?"s);
    QCOMPARE(parse::sanitize("\0x"s), "?x"s);
    QCOMPARE(parse::sanitize("caf\xc3\xa9"s), "caf\xc3\xa9"s);
}

// ===========================================================================
// /proc/stat

void TestAdversarial::procStatModernLineKeepsGuestSeparate()
{
    // Linux >= 2.6.33: user nice system idle iowait irq softirq steal guest guest_nice.
    const auto s = parse::parseProcStat(
        "cpu  10132153 290696 3084719 46828483 16683 0 25195 0 175628 0\n"
        "cpu0 1393280 32966 572056 13343292 6130 0 17875 0 23933 0\n"
        "intr 199292 0 0\n"
        "ctxt 1990473\n"
        "btime 1062191376\n"
        "processes 2915\n"
        "procs_running 1\n"
        "procs_blocked 0\n"
        "softirq 12 1 2 3\n");
    QVERIFY(s.aggregate.has_value());
    const auto& a = *s.aggregate;
    QCOMPARE(a.user, uint64_t(10132153));
    QCOMPARE(a.nice, uint64_t(290696));
    QCOMPARE(a.system, uint64_t(3084719));
    QCOMPARE(a.idle, uint64_t(46828483));
    QCOMPARE(a.iowait, uint64_t(16683));
    QCOMPARE(a.irq, uint64_t(0));
    QCOMPARE(a.softirq, uint64_t(25195));
    QCOMPARE(a.steal, uint64_t(0));
    QCOMPARE(a.guest, uint64_t(175628));
    QCOMPARE(a.guestNice, uint64_t(0));
    // Hand sum of the eight non-guest columns (guest already inside user).
    QCOMPARE(a.total(), uint64_t(60377929));
    QCOMPARE(s.cpus.size(), size_t(1));
    QCOMPARE(s.cpus[0].index, 0);
    QCOMPARE(s.cpus[0].times.guest, uint64_t(23933));
    QCOMPARE(s.bootTime, std::optional<uint64_t>(1062191376));
}

void TestAdversarial::procStatHotplugGapsAndOrdering()
{
    // Offline CPUs are simply absent; order in the file must not matter.
    const auto s = parse::parseProcStat(
        "cpu 4 0 4 8 0 0 0 0 0 0\n"
        "cpu7 1 0 1 2 0 0 0 0 0 0\n"
        "cpu2 3 0 3 6 0 0 0 0 0 0\n"
        "cpu0 5 0 5 10 0 0 0 0 0 0\n"
        "cpufreq 1 2 3 4 5\n"      // not a cpu line
        "cpu2 99 99 99 99 99\n");  // duplicate index: first occurrence kept
    QCOMPARE(s.cpus.size(), size_t(3));
    QCOMPARE(s.cpus[0].index, 0);
    QCOMPARE(s.cpus[1].index, 2);
    QCOMPARE(s.cpus[2].index, 7);
    QCOMPARE(s.cpus[0].times.user, uint64_t(5));
    QCOMPARE(s.cpus[1].times.user, uint64_t(3));
    QCOMPARE(s.cpus[2].times.idle, uint64_t(2));
}

void TestAdversarial::procStatRejectsMalformedLines_data()
{
    QTest::addColumn<QByteArray>("line");
    QTest::newRow("truncated to 3 values") << QByteArray("cpu0 1 2 3\n");
    QTest::newRow("name only") << QByteArray("cpu0\n");
    QTest::newRow("negative value") << QByteArray("cpu0 1 2 -3 4 5 6 7 8 0 0\n");
    QTest::newRow("trailing junk in value") << QByteArray("cpu0 1 2 3 4 5x 6 7 8 0 0\n");
    QTest::newRow("overflowing value") << QByteArray("cpu0 1 2 3 18446744073709551616 5 6 7 8 0 0\n");
    QTest::newRow("float value") << QByteArray("cpu0 1 2 3.5 4 5 6 7 8 0 0\n");
    QTest::newRow("hex value") << QByteArray("cpu0 1 2 0x3 4 5 6 7 8 0 0\n");
    QTest::newRow("junk index") << QByteArray("cpuX 1 2 3 4 5 6 7 8 0 0\n");
    QTest::newRow("negative index") << QByteArray("cpu-1 1 2 3 4 5 6 7 8 0 0\n");
    QTest::newRow("signed index") << QByteArray("cpu+1 1 2 3 4 5 6 7 8 0 0\n");
    QTest::newRow("absurd index") << QByteArray("cpu99999999999 1 2 3 4 5 6 7 8 0 0\n");
    QTest::newRow("NUL in value") << QByteArray("cpu0 1 2\0 3 4 5 6 7 8 0 0\n", 27);
}

void TestAdversarial::procStatRejectsMalformedLines()
{
    QFETCH(QByteArray, line);
    const auto s = parse::parseProcStat(std::string_view(line.constData(), size_t(line.size())));
    QVERIFY(s.cpus.empty());
    QVERIFY(!s.aggregate.has_value());
}

void TestAdversarial::procStatShortLegacyLineDefaultsMissingFieldsToZero()
{
    // Pre-2.5.41 kernels had only four columns. Missing columns are real zeros.
    const auto s = parse::parseProcStat("cpu 10 20 30 40\n");
    QVERIFY(s.aggregate.has_value());
    QCOMPARE(s.aggregate->user, uint64_t(10));
    QCOMPARE(s.aggregate->idle, uint64_t(40));
    QCOMPARE(s.aggregate->iowait, uint64_t(0));
    QCOMPARE(s.aggregate->guest, uint64_t(0));
    QCOMPARE(s.aggregate->total(), uint64_t(100));
}

void TestAdversarial::procStatBtime()
{
    QCOMPARE(parse::parseProcStat("btime 0\n").bootTime, std::optional<uint64_t>(0));
    QVERIFY(!parse::parseProcStat("btime -1\n").bootTime);
    QVERIFY(!parse::parseProcStat("btime 12 34\n").bootTime);
    QVERIFY(!parse::parseProcStat("btime\n").bootTime);
    QVERIFY(!parse::parseProcStat("btimex 12\n").bootTime);
    QVERIFY(!parse::parseProcStat("btime 12abc\n").bootTime);
}

void TestAdversarial::procStatCrlfAndTabs()
{
    const auto s = parse::parseProcStat("cpu\t1\t2\t3\t4\t5\t6\t7\t8\t9\t10\r\ncpu3  1 1 1 1\r\n");
    QVERIFY(s.aggregate.has_value());
    QCOMPARE(s.aggregate->guestNice, uint64_t(10));
    QCOMPARE(s.cpus.size(), size_t(1));
    QCOMPARE(s.cpus[0].index, 3);
}

void TestAdversarial::cpuTimesTotalDoesNotSilentlyWrap()
{
    // The parser accepts the full uint64 range. total() is a public helper; a
    // sum that wraps turns an absurd counter into a small, plausible one.
    const auto s = parse::parseProcStat("cpu 18446744073709551615 0 0 2 0 0 0 0 0 0\n");
    QVERIFY(s.aggregate.has_value());
    QCOMPARE(s.aggregate->user, kU64Max);
    QVERIFY2(s.aggregate->total() >= s.aggregate->user,
             "CpuTimes::total() wrapped modulo 2^64 (returned a value smaller than one of its parts)");
}

void TestAdversarial::cpuDeltaGuestCountedOnce()
{
    parse::CpuTimes prev;
    prev.user = 1000; prev.nice = 100; prev.system = 300; prev.idle = 5000; prev.iowait = 50;
    prev.irq = 10; prev.softirq = 20; prev.steal = 5; prev.guest = 200; prev.guestNice = 40;
    parse::CpuTimes cur;
    cur.user = 1600;     // +600, of which +400 is guest time
    cur.nice = 180;      // +80,  of which +30 is guest_nice
    cur.system = 420;    // +120
    cur.idle = 5700;     // +700
    cur.iowait = 80;     // +30
    cur.irq = 15;        // +5
    cur.softirq = 35;    // +15
    cur.steal = 35;      // +30
    cur.guest = 600;
    cur.guestNice = 70;

    const auto d = metrics::cpuDelta(prev, cur);
    QVERIFY(d.has_value());
    QCOMPARE(d->user, uint64_t(680));  // 600 + 80
    QCOMPARE(d->system, uint64_t(140)); // 120 + 5 + 15
    QCOMPARE(d->idle, uint64_t(730));   // 700 + 30
    QCOMPARE(d->iowait, uint64_t(30));
    QCOMPARE(d->steal, uint64_t(30));
    QCOMPARE(d->total, uint64_t(1580)); // 2010 if guest were added again

    const auto u = metrics::cpuUsage(prev, cur);
    QVERIFY(near(u.total, 100.0 * 850 / 1580));
    QVERIFY(near(u.user, 100.0 * 680 / 1580));
    QVERIFY(near(u.system, 100.0 * 140 / 1580));
    QVERIFY(near(u.iowait, 100.0 * 30 / 1580));
    QVERIFY(near(u.steal, 100.0 * 30 / 1580));
}

void TestAdversarial::cpuDeltaIowaitRegressionInvalidates()
{
    // proc(5): iowait "can decrease in certain conditions". Approved behavior:
    // invalidate the interval and rebase, never report a wrapped/garbage value.
    parse::CpuTimes prev;
    prev.user = 100; prev.system = 100; prev.idle = 1000; prev.iowait = 500;
    parse::CpuTimes cur = prev;
    cur.user += 10; cur.idle += 90; cur.iowait = 499;
    QVERIFY(!metrics::cpuDelta(prev, cur).has_value());
    const auto u = metrics::cpuUsage(prev, cur);
    QVERIFY(!ps::isValid(u.total));
    QVERIFY(!ps::isValid(u.user));
    QVERIFY(!ps::isValid(u.system));
    QVERIFY(!ps::isValid(u.iowait));
    QVERIFY(!ps::isValid(u.steal));
}

void TestAdversarial::cpuDeltaOtherRegressionInvalidates()
{
    // CPU hot-unplug/replug resets per-CPU counters.
    parse::CpuTimes prev;
    prev.user = 5000; prev.idle = 90000; prev.steal = 7;
    parse::CpuTimes cur;
    cur.user = 12; cur.idle = 300; cur.steal = 7;
    QVERIFY(!metrics::cpuDelta(prev, cur).has_value());
    parse::CpuTimes cur2 = prev;
    cur2.user += 10;
    cur2.steal = 6;
    QVERIFY(!metrics::cpuDelta(prev, cur2).has_value());
    // A regression only in guest (not an accounted column) must not matter.
    parse::CpuTimes prev3 = prev;
    prev3.guest = 100;
    parse::CpuTimes cur3 = prev;
    cur3.user += 10; cur3.idle += 10; cur3.guest = 0;
    QVERIFY(metrics::cpuDelta(prev3, cur3).has_value());
}

void TestAdversarial::cpuDeltaZeroElapsedIsUnavailable()
{
    parse::CpuTimes t;
    t.user = 7; t.idle = 9;
    const auto u = metrics::cpuUsage(t, t);
    QVERIFY(!ps::isValid(u.total)); // not "0 % busy"
}

void TestAdversarial::cpuUsageNeverOutsidePercentRange()
{
    // Absurd but parseable counters: the eight deltas overflow when summed.
    parse::CpuTimes prev;
    parse::CpuTimes cur;
    cur.user = kU64Max;
    cur.idle = 2;
    const auto u = metrics::cpuUsage(prev, cur);
    const bool okTotal = !ps::isValid(u.total) || (u.total >= 0.0 && u.total <= 100.0);
    QVERIFY2(okTotal, qPrintable(QStringLiteral("cpuUsage total = %1 %").arg(u.total)));
    const bool okUser = !ps::isValid(u.user) || (u.user >= 0.0 && u.user <= 100.0);
    QVERIFY2(okUser, qPrintable(QStringLiteral("cpuUsage user = %1 %").arg(u.user)));
}

// ===========================================================================
// /proc/meminfo

void TestAdversarial::meminfoRealisticKbToBytes()
{
    const auto m = parse::parseMeminfo(
        "MemTotal:       16303428 kB\n"
        "MemFree:          846992 kB\n"
        "MemAvailable:    9541156 kB\n"
        "Buffers:          514512 kB\n"
        "Cached:          8263460 kB\n"
        "SwapCached:        12345 kB\n"
        "Shmem:            612340 kB\n"
        "SReclaimable:     540000 kB\n"
        "SUnreclaim:       170000 kB\n"
        "SwapTotal:       8388604 kB\n"
        "SwapFree:        8000000 kB\n"
        "HugePages_Total:       0\n"
        "Hugepagesize:       2048 kB\n");
    QCOMPARE(m.memTotal, std::optional<uint64_t>(16303428ull * 1024));
    QCOMPARE(m.memFree, std::optional<uint64_t>(846992ull * 1024));
    QCOMPARE(m.memAvailable, std::optional<uint64_t>(9541156ull * 1024));
    QCOMPARE(m.buffers, std::optional<uint64_t>(514512ull * 1024));
    QCOMPARE(m.cached, std::optional<uint64_t>(8263460ull * 1024)); // not SwapCached
    QCOMPARE(m.shmem, std::optional<uint64_t>(612340ull * 1024));
    QCOMPARE(m.sReclaimable, std::optional<uint64_t>(540000ull * 1024));
    QCOMPARE(m.swapTotal, std::optional<uint64_t>(8388604ull * 1024));
    QCOMPARE(m.swapFree, std::optional<uint64_t>(8000000ull * 1024));

    const auto mem = metrics::memoryFromRaw(m);
    QVERIFY(mem.valid);
    QCOMPARE(mem.total, 16694710272.0);                    // 16303428 KiB
    QCOMPARE(mem.used, (16303428.0 - 9541156.0) * 1024.0); // MemTotal - MemAvailable
    QCOMPARE(mem.available, 9770143744.0);
    QCOMPARE(mem.free, 867319808.0);
    QCOMPARE(mem.cache, (514512.0 + 8263460.0 + 540000.0) * 1024.0);
    QCOMPARE(mem.shared, 612340.0 * 1024.0);
    QVERIFY(mem.hasSwap);
    QCOMPARE(mem.swapUsed, 388604.0 * 1024.0);
}

void TestAdversarial::meminfoRejectsBadValues_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::newRow("negative") << QByteArray("MemTotal: -1 kB\n");
    QTest::newRow("wrong unit MB") << QByteArray("MemTotal: 16 MB\n");
    QTest::newRow("wrong unit KB") << QByteArray("MemTotal: 16 KB\n");
    QTest::newRow("float") << QByteArray("MemTotal: 1.5 kB\n");
    QTest::newRow("hex") << QByteArray("MemTotal: 0x10 kB\n");
    QTest::newRow("trailing junk on number") << QByteArray("MemTotal: 16x kB\n");
    QTest::newRow("empty value") << QByteArray("MemTotal:\n");
    QTest::newRow("unit only") << QByteArray("MemTotal: kB\n");
    QTest::newRow("no colon") << QByteArray("MemTotal 16 kB\n");
    QTest::newRow("lower-case key") << QByteArray("memtotal: 16 kB\n");
    QTest::newRow("key prefix") << QByteArray("MemTotalX: 16 kB\n");
}

void TestAdversarial::meminfoRejectsBadValues()
{
    QFETCH(QByteArray, text);
    const auto m = parse::parseMeminfo(std::string_view(text.constData(), size_t(text.size())));
    QVERIFY(!m.memTotal.has_value());
    QVERIFY(!metrics::memoryFromRaw(m).valid);
}

void TestAdversarial::meminfoMissingUnitIsNotBytes()
{
    // Every field PulseScope reads is printed by the kernel with a " kB"
    // suffix. A line that lost its unit (truncated read, corrupted input) must
    // not be taken as raw bytes: that silently reports 1/1024 of the real RAM.
    const auto m = parse::parseMeminfo("MemTotal:       16303428\nMemAvailable:    9541156 kB\n");
    QVERIFY2(!m.memTotal.has_value(),
             qPrintable(QStringLiteral("unit-less MemTotal accepted as %1 bytes")
                            .arg(m.memTotal ? quint64(*m.memTotal) : 0)));
}

void TestAdversarial::meminfoOverflowBoundary()
{
    // floor(2^64 / 1024) = 18014398509481984 overflows after scaling.
    QVERIFY(!parse::parseMeminfo("MemTotal: 18014398509481984 kB\n").memTotal);
    QCOMPARE(parse::parseMeminfo("MemTotal: 18014398509481983 kB\n").memTotal,
             std::optional<uint64_t>(18446744073709550592ull));
    QCOMPARE(parse::parseMeminfo("MemTotal:16 kB\n").memTotal, std::optional<uint64_t>(16384));
    QCOMPARE(parse::parseMeminfo("MemTotal:\t\t16\tkB\r\n").memTotal, std::optional<uint64_t>(16384));
}

void TestAdversarial::memoryUsedIsTotalMinusAvailable()
{
    const auto mem = metrics::memoryFromRaw(rawMem(8000000, 3000000, 1000000));
    QVERIFY(mem.valid);
    QCOMPARE(mem.used, 5000000.0 * 1024);
    QCOMPARE(mem.available, 3000000.0 * 1024);
    QCOMPARE(mem.free, 1000000.0 * 1024);
}

void TestAdversarial::memoryMissingAvailableNoMemFreeSubstitution()
{
    // Pre-3.14 kernels lack MemAvailable. Used must be unavailable, not
    // MemTotal-MemFree dressed up as the same metric.
    const auto m = parse::parseMeminfo("MemTotal: 8000000 kB\nMemFree: 1000000 kB\nBuffers: 1 kB\nCached: 2 kB\n");
    QVERIFY(!m.memAvailable.has_value());
    const auto mem = metrics::memoryFromRaw(m);
    QVERIFY(mem.valid);
    QVERIFY(!ps::isValid(mem.used));
    QVERIFY(!ps::isValid(mem.available));
    QCOMPARE(mem.free, 1000000.0 * 1024);
}

void TestAdversarial::memoryImpossibleValuesRejected()
{
    const auto mem = metrics::memoryFromRaw(rawMem(1000, 1001, 1002));
    QVERIFY(mem.valid);
    QVERIFY(!ps::isValid(mem.used));
    QVERIFY(!ps::isValid(mem.available));
    QVERIFY(!ps::isValid(mem.free));

    auto r = rawMem(1000, 500, 100);
    r.buffers = 600 * 1024;
    r.cached = 500 * 1024; // buffers + cached > total
    r.shmem = 2000 * 1024;
    const auto mem2 = metrics::memoryFromRaw(r);
    QVERIFY(!ps::isValid(mem2.cache));
    QVERIFY(!ps::isValid(mem2.shared));

    // Cache requires both Buffers and Cached; no silent partial sum.
    auto r3 = rawMem(1000, 500, 100);
    r3.cached = 10 * 1024;
    QVERIFY(!ps::isValid(metrics::memoryFromRaw(r3).cache));
}

void TestAdversarial::memoryZeroAvailableIsRealZero()
{
    const auto mem = metrics::memoryFromRaw(rawMem(1000, 0, 0));
    QVERIFY(ps::isValid(mem.available));
    QCOMPARE(mem.available, 0.0);
    QCOMPARE(mem.used, 1000.0 * 1024);
    QCOMPARE(mem.free, 0.0);
}

void TestAdversarial::memoryZeroTotalInvalid()
{
    const auto mem = metrics::memoryFromRaw(rawMem(0, 0, 0));
    QVERIFY(!mem.valid);
    QVERIFY(!ps::isValid(mem.total));
    QVERIFY(!ps::isValid(mem.used));
    const auto none = metrics::memoryFromRaw(parse::MemInfoRaw{});
    QVERIFY(!none.valid);
    QVERIFY(!none.hasSwap);
    QVERIFY(!ps::isValid(none.swapTotal));
}

void TestAdversarial::memoryCacheSumCannotWrap()
{
    // Buffers alone exceeds MemTotal, but Buffers+Cached wraps modulo 2^64 to
    // a small value that passes a "cache <= total" check.
    auto r = rawMem(16ull * 1024 * 1024, 8ull * 1024 * 1024, 1024); // 16 GiB total
    r.buffers = 18446744073709550592ull; // 18014398509481983 kB, the parser maximum
    r.cached = 2048 * 1024;
    const auto mem = metrics::memoryFromRaw(r);
    QVERIFY2(!ps::isValid(mem.cache),
             qPrintable(QStringLiteral("cache reported as %1 bytes from wrapped sum").arg(mem.cache, 0, 'f', 0)));
}

void TestAdversarial::swapConventions()
{
    parse::MemInfoRaw noSwap;
    noSwap.swapTotal = 0;
    noSwap.swapFree = 0;
    const auto a = metrics::memoryFromRaw(noSwap);
    QVERIFY(!a.hasSwap);
    QCOMPARE(a.swapTotal, 0.0);
    QCOMPARE(a.swapUsed, 0.0);

    parse::MemInfoRaw bad;
    bad.swapTotal = 1024;
    bad.swapFree = 2048;
    const auto b = metrics::memoryFromRaw(bad);
    QVERIFY(b.hasSwap);
    QVERIFY(!ps::isValid(b.swapUsed));
    QVERIFY(!ps::isValid(b.swapFree));

    parse::MemInfoRaw missingFree;
    missingFree.swapTotal = 4096;
    const auto c = metrics::memoryFromRaw(missingFree);
    QVERIFY(c.hasSwap);
    QVERIFY(!ps::isValid(c.swapUsed)); // not "0 used"

    parse::MemInfoRaw full;
    full.swapTotal = 4096;
    full.swapFree = 0;
    QCOMPARE(metrics::memoryFromRaw(full).swapUsed, 4096.0);
}

// ===========================================================================
// /proc/diskstats

void TestAdversarial::diskstatsExtendedFieldColumns()
{
    // Linux >= 5.5: 3 identity + 17 statistics (discard 4, flush 2).
    //  t3 reads  t4 rmerged t5 rsectors t6 rms t7 writes t8 wmerged t9 wsectors
    //  t10 wms t11 inflight t12 io_ticks t13 weighted t14..t17 discard t18..t19 flush
    const auto d = parse::parseDiskstats(
        " 259       0 nvme0n1 551234 12345 45678901 123456 876543 54321 98765432 654321 3 345678 789012 1234 11 5678 90 4321 12\n"
        " 259       1 nvme0n1p1 1 2 30 4 5 6 70 8 0 90 100 0 0 0 0 0 0\n"
        "   8      16 sdb 10 20 30 40 50 60 70 80 90 100 110 120 130 140 150\n");
    QCOMPARE(d.size(), size_t(3));
    QCOMPARE(d[0].major, 259u);
    QCOMPARE(d[0].minor, 0u);
    QCOMPARE(d[0].name, "nvme0n1"s);
    QCOMPARE(d[0].sectorsRead, uint64_t(45678901));
    QCOMPARE(d[0].sectorsWritten, uint64_t(98765432));
    QCOMPARE(d[0].ioTicksMs, uint64_t(345678));
    QCOMPARE(d[1].name, "nvme0n1p1"s);
    QCOMPARE(d[1].sectorsRead, uint64_t(30));
    QCOMPARE(d[1].sectorsWritten, uint64_t(70));
    QCOMPARE(d[1].ioTicksMs, uint64_t(90));
    // 4.18..5.4 layout (15 statistics).
    QCOMPARE(d[2].sectorsRead, uint64_t(30));
    QCOMPARE(d[2].sectorsWritten, uint64_t(70));
    QCOMPARE(d[2].ioTicksMs, uint64_t(100));
}

void TestAdversarial::diskstatsLegacy14Tokens()
{
    const auto d = parse::parseDiskstats("   7       0 loop0 1 0 8 0 0 0 0 0 0 4 0\n");
    QCOMPARE(d.size(), size_t(1));
    QCOMPARE(d[0].name, "loop0"s);
    QCOMPARE(d[0].sectorsRead, uint64_t(8));
    QCOMPARE(d[0].sectorsWritten, uint64_t(0));
    QCOMPARE(d[0].ioTicksMs, uint64_t(4));
}

void TestAdversarial::diskstatsRejectsMalformed_data()
{
    QTest::addColumn<QByteArray>("line");
    QTest::newRow("13 tokens (truncated)") << QByteArray("8 0 sda 1 2 3 4 5 6 7 8 9 10\n");
    QTest::newRow("ancient 4-stat partition") << QByteArray("8 1 sda1 100 200 300 400\n");
    QTest::newRow("negative sectors read") << QByteArray("8 0 sda 1 2 -3 4 5 6 7 8 9 10 11\n");
    QTest::newRow("junk sectors written") << QByteArray("8 0 sda 1 2 3 4 5 6 7x 8 9 10 11\n");
    QTest::newRow("overflow io_ticks") << QByteArray("8 0 sda 1 2 3 4 5 6 7 8 9 18446744073709551616 11\n");
    QTest::newRow("major > uint32") << QByteArray("4294967296 0 sda 1 2 3 4 5 6 7 8 9 10 11\n");
    QTest::newRow("minor > uint32") << QByteArray("8 4294967296 sda 1 2 3 4 5 6 7 8 9 10 11\n");
    QTest::newRow("junk major") << QByteArray("x 0 sda 1 2 3 4 5 6 7 8 9 10 11\n");
    QTest::newRow("empty") << QByteArray("\n");
}

void TestAdversarial::diskstatsRejectsMalformed()
{
    QFETCH(QByteArray, line);
    QVERIFY(parse::parseDiskstats(std::string_view(line.constData(), size_t(line.size()))).empty());
}

void TestAdversarial::diskstatsSectorsAre512ByteUnits()
{
    // iostats.rst: sector counts are always 512-byte units, regardless of the
    // device's logical block size (a 4Kn NVMe drive reads 8 "sectors" per block).
    QCOMPARE(metrics::kDiskSectorBytes, uint64_t(512));
    const auto a = parse::parseDiskstats("259 0 nvme0n1 1 0 1000 0 1 0 2000 0 0 10 0\n");
    const auto b = parse::parseDiskstats("259 0 nvme0n1 2 0 1008 0 2 0 4048 0 0 20 0\n");
    QCOMPARE(a.size(), size_t(1));
    QCOMPARE(b.size(), size_t(1));
    const auto readDelta = metrics::counterDelta(a[0].sectorsRead, b[0].sectorsRead);
    const auto writeDelta = metrics::counterDelta(a[0].sectorsWritten, b[0].sectorsWritten);
    QCOMPARE(readDelta, std::optional<uint64_t>(8));
    QCOMPARE(metrics::rate(readDelta, 1.0, metrics::kDiskSectorBytes), 4096.0);     // one 4 KiB block
    QCOMPARE(metrics::rate(writeDelta, 2.0, metrics::kDiskSectorBytes), 524288.0); // 2048*512/2
}

void TestAdversarial::diskstatsNamesKeptVerbatim()
{
    // Names are a single whitespace-free token. The parser does not sanitize
    // them (documented here; display code must not trust them blindly).
    const auto d = parse::parseDiskstats("253 3 dm-3 1 2 3 4 5 6 7 8 9 10 11\n"
                                         "253 3 dm-3 1 2 3 4 5 6 7 8 9 10 11\n"
                                         "8 0 s\x1b" "da 1 2 3 4 5 6 7 8 9 10 11\n");
    QCOMPARE(d.size(), size_t(3));
    QCOMPARE(d[2].name.size(), size_t(4));
}

// ===========================================================================
// /proc/net/dev

void TestAdversarial::netDevRealisticWithHeaders()
{
    const auto n = parse::parseNetDev(
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo: 123456789    1000    0    0    0     0          0         0 223456789    2000    0    0    0     0       0          0\n"
        "  eth0: 18446744073709551615 5 6 7 8 9 10 11 18446744073709551614 13 14 15 16 17 18 19\n");
    QCOMPARE(n.size(), size_t(2));
    QCOMPARE(n[0].name, "lo"s);
    QCOMPARE(n[0].rxBytes, uint64_t(123456789));
    QCOMPARE(n[0].txBytes, uint64_t(223456789));
    QCOMPARE(n[1].name, "eth0"s);
    QCOMPARE(n[1].rxBytes, kU64Max);
    QCOMPARE(n[1].txBytes, kU64Max - 1);
}

void TestAdversarial::netDevLegacyNoSpaceAfterColon()
{
    // Old kernels printed "%6s:%8lu", so large counters touch the colon.
    const auto n = parse::parseNetDev("  eth0:4294967295 1 2 3 4 5 6 7 1234 9 10 11 12 13 14 15\n");
    QCOMPARE(n.size(), size_t(1));
    QCOMPARE(n[0].name, "eth0"s);
    QCOMPARE(n[0].rxBytes, uint64_t(4294967295u));
    QCOMPARE(n[0].txBytes, uint64_t(1234));
}

void TestAdversarial::netDevMaxLengthName()
{
    // IFNAMSIZ-1 = 15 characters; "%6s" does not pad, so no leading space.
    const auto n = parse::parseNetDev(netLine("abcdefghijklmno"));
    QCOMPARE(n.size(), size_t(1));
    QCOMPARE(n[0].name, "abcdefghijklmno"s);
    QCOMPARE(n[0].rxBytes, uint64_t(1111));
    QCOMPARE(n[0].txBytes, uint64_t(9999));
}

void TestAdversarial::netDevLegalPipeInName()
{
    // dev_valid_name() only forbids '/', ':' and whitespace, so "br|lan" is a
    // legal interface name. The header lines contain no ':' and are already
    // skipped for that reason; dropping '|' names hides real traffic from the
    // per-interface list and the network total.
    const auto n = parse::parseNetDev(netLine("br|lan"));
    QVERIFY2(n.size() == 1, "legal interface name containing '|' was dropped");
    QCOMPARE(n[0].name, "br|lan"s);
}

void TestAdversarial::netDevRejectsMalformed_data()
{
    QTest::addColumn<QByteArray>("line");
    QTest::newRow("15 columns (truncated)") << QByteArray("eth0: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15\n");
    QTest::newRow("negative rx") << QByteArray("eth0: -1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n");
    QTest::newRow("overflow tx") << QByteArray("eth0: 1 2 3 4 5 6 7 8 18446744073709551616 10 11 12 13 14 15 16\n");
    QTest::newRow("junk rx") << QByteArray("eth0: 1k 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n");
    QTest::newRow("empty name") << QByteArray(": 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n");
    QTest::newRow("name with space") << QByteArray("et h0: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n");
    QTest::newRow("header") << QByteArray("Inter-|   Receive |  Transmit\n");
}

void TestAdversarial::netDevRejectsMalformed()
{
    QFETCH(QByteArray, line);
    QVERIFY(parse::parseNetDev(std::string_view(line.constData(), size_t(line.size()))).empty());
}

void TestAdversarial::netDevOddButLegalNamesPreserved()
{
    // UTF-8 and punctuation are legal; control bytes are also accepted by the
    // kernel. The parser keeps raw bytes (display layer must sanitize).
    const auto n = parse::parseNetDev(netLine("wg\xe2\x98\x83-0.1@x") + netLine("e\x1b[2Jx"));
    QCOMPARE(n.size(), size_t(2));
    QCOMPARE(n[0].name, "wg\xe2\x98\x83-0.1@x"s);
    QCOMPARE(n[1].name.size(), size_t(6));
}

// ===========================================================================
// /proc/<pid>/stat

void TestAdversarial::pidStatAllColumns()
{
    const auto s = parse::parsePidStat(statLine("4242", "bash"));
    QVERIFY(s.has_value());
    QCOMPARE(s->pid, 4242);
    QCOMPARE(s->comm, "bash"s);
    QCOMPARE(s->state, 'S');
    QCOMPARE(s->ppid, 7);
    QCOMPARE(s->flags, 4194560u);
    QCOMPARE(s->utime, uint64_t(250));
    QCOMPARE(s->stime, uint64_t(75));
    QCOMPARE(s->nice, int64_t(-5));        // not priority (15)
    QCOMPARE(s->numThreads, int64_t(3));
    QCOMPARE(s->startTime, uint64_t(98765)); // not itrealvalue or vsize
    QCOMPARE(s->rssPages, int64_t(456));     // not vsize or rsslim
    QVERIFY((s->flags & parse::kPfKthread) == 0);
}

void TestAdversarial::pidStatHostileComm_data()
{
    // comm is attacker controlled (prctl(PR_SET_NAME), exec of a crafted file
    // name) and may contain spaces, parentheses and control bytes.
    QTest::addColumn<QByteArray>("comm");
    QTest::addColumn<QByteArray>("expected");
    QTest::newRow("spaces") << QByteArray("Web Content") << QByteArray("Web Content");
    QTest::newRow("colon") << QByteArray("tmux: server") << QByteArray("tmux: server");
    QTest::newRow("close paren") << QByteArray("a)b") << QByteArray("a)b");
    QTest::newRow("fake fields") << QByteArray("x) R 1 2 3 (") << QByteArray("x) R 1 2 3 (");
    QTest::newRow("only parens") << QByteArray(")))((") << QByteArray(")))((");
    QTest::newRow("leading paren") << QByteArray("(sd-pam)") << QByteArray("(sd-pam)");
    QTest::newRow("empty") << QByteArray("") << QByteArray("");
    QTest::newRow("newline") << QByteArray("a\nb") << QByteArray("a?b");
    QTest::newRow("escape sequence") << QByteArray("\x1b[2J") << QByteArray("?[2J");
    QTest::newRow("tab") << QByteArray("a\tb") << QByteArray("a?b");
    QTest::newRow("utf8") << QByteArray("caf\xc3\xa9") << QByteArray("caf\xc3\xa9");
    QTest::newRow("kworker 64") << QByteArray("kworker/u16:3-events_unbound_long_name_x_y_z")
                                << QByteArray("kworker/u16:3-events_unbound_long_name_x_y_z");
}

void TestAdversarial::pidStatHostileComm()
{
    QFETCH(QByteArray, comm);
    QFETCH(QByteArray, expected);
    const auto line = statLine("99", std::string_view(comm.constData(), size_t(comm.size())));
    const auto s = parse::parsePidStat(line);
    QVERIFY(s.has_value());
    QCOMPARE(QByteArray::fromStdString(s->comm), expected);
    QCOMPARE(s->pid, 99);
    QCOMPARE(s->ppid, 7);
    QCOMPARE(s->startTime, uint64_t(98765));
    QCOMPARE(s->rssPages, int64_t(456));
}

void TestAdversarial::pidStatKernelThreadFlag()
{
    // kthreadd: ppid 0, PF_KTHREAD (0x00200000) in flags, rss 0.
    const auto s = parse::parsePidStat(
        "2 (kthreadd) S 0 0 0 0 -1 2129984 0 0 0 0 0 3 0 0 20 0 1 0 1 0 0 18446744073709551615 "
        "0 0 0 0 0 0 0 2147483647 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    QVERIFY(s.has_value());
    QCOMPARE(s->ppid, 0);
    QVERIFY((s->flags & parse::kPfKthread) != 0);
    QCOMPARE(s->rssPages, int64_t(0));
    QCOMPARE(s->startTime, uint64_t(1));
}

void TestAdversarial::pidStatStartTimePreservedAsU64()
{
    // starttime is part of process identity; it must not pass through double
    // (2^63 + 1 is not representable as a double).
    std::string tail(kStatTail);
    tail.replace(tail.find(" 98765 "), 7, " 9223372036854775809 ");
    const auto s = parse::parsePidStat(statLine("1", "init", tail));
    QVERIFY(s.has_value());
    QCOMPARE(s->startTime, uint64_t(9223372036854775809ull));
    std::string tail2(kStatTail);
    tail2.replace(tail2.find(" 250 75 "), 8, " 18446744073709551615 18446744073709551614 ");
    const auto s2 = parse::parsePidStat(statLine("1", "init", tail2));
    QVERIFY(s2.has_value());
    QCOMPARE(s2->utime, kU64Max);
    QCOMPARE(s2->stime, kU64Max - 1);
}

void TestAdversarial::pidStatRejectsMalformed_data()
{
    QTest::addColumn<QByteArray>("text");
    const auto tail = [](std::string_view from, std::string_view to) {
        std::string t(kStatTail);
        const auto pos = t.find(from);
        Q_ASSERT(pos != std::string::npos);
        t.replace(pos, from.size(), to);
        return QByteArray::fromStdString(statLine("42", "x", t));
    };
    std::string truncated(kStatTail);
    truncated = truncated.substr(0, truncated.find(" 456 ")); // ends at vsize: 21 fields
    QTest::newRow("truncated before rss") << QByteArray::fromStdString(statLine("42", "x", truncated));
    QTest::newRow("empty") << QByteArray("");
    QTest::newRow("no close paren") << QByteArray("42 (abc S 1 2 3");
    QTest::newRow("no open paren") << QByteArray("42 abc) S 1 2 3");
    QTest::newRow("pid zero") << QByteArray::fromStdString(statLine("0", "x"));
    QTest::newRow("pid negative") << QByteArray::fromStdString(statLine("-5", "x"));
    QTest::newRow("pid > int32") << QByteArray::fromStdString(statLine("4294967297", "x"));
    QTest::newRow("pid junk") << QByteArray::fromStdString(statLine("42x", "x"));
    QTest::newRow("pid missing") << QByteArray::fromStdString(statLine("", "x"));
    QTest::newRow("state two chars") << tail("S 7 ", "RS 7 ");
    QTest::newRow("ppid negative") << tail("S 7 ", "S -7 ");
    QTest::newRow("ppid junk") << tail("S 7 ", "S 7q ");
    QTest::newRow("utime junk") << tail(" 250 ", " 25o ");
    QTest::newRow("stime negative") << tail(" 75 ", " -75 ");
    QTest::newRow("threads negative") << tail(" -5 3 ", " -5 -3 ");
    QTest::newRow("starttime overflow") << tail(" 98765 ", " 18446744073709551616 ");
    QTest::newRow("starttime negative") << tail(" 98765 ", " -98765 ");
    QTest::newRow("rss negative") << tail(" 456 ", " -456 ");
    QTest::newRow("flags > uint32") << tail(" 4194560 ", " 4294967296 ");
}

void TestAdversarial::pidStatRejectsMalformed()
{
    QFETCH(QByteArray, text);
    QVERIFY(!parse::parsePidStat(std::string_view(text.constData(), size_t(text.size()))).has_value());
}

// ===========================================================================
// /proc/<pid>/io, status, cmdline

void TestAdversarial::pidIo_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<quint64>("readBytes");
    QTest::addColumn<quint64>("writeBytes");
    QTest::newRow("full") << QByteArray("rchar: 11\nwchar: 22\nsyscr: 3\nsyscw: 4\nread_bytes: 4096\n"
                                        "write_bytes: 8192\ncancelled_write_bytes: 512\n")
                          << true << quint64(4096) << quint64(8192);
    QTest::newRow("max") << QByteArray("read_bytes: 18446744073709551615\nwrite_bytes: 0\n")
                         << true << quint64(kU64Max) << quint64(0);
    QTest::newRow("only rchar/wchar") << QByteArray("rchar: 11\nwchar: 22\n") << false << quint64(0) << quint64(0);
    QTest::newRow("missing write") << QByteArray("read_bytes: 1\n") << false << quint64(0) << quint64(0);
    QTest::newRow("negative") << QByteArray("read_bytes: -1\nwrite_bytes: 0\n") << false << quint64(0) << quint64(0);
    QTest::newRow("junk") << QByteArray("read_bytes: 1 kB\nwrite_bytes: 0\n") << false << quint64(0) << quint64(0);
    QTest::newRow("overflow") << QByteArray("read_bytes: 18446744073709551616\nwrite_bytes: 0\n")
                              << false << quint64(0) << quint64(0);
    QTest::newRow("empty (EACCES yields no text)") << QByteArray("") << false << quint64(0) << quint64(0);
    QTest::newRow("later invalid duplicate") << QByteArray("read_bytes: 1\nwrite_bytes: 2\nread_bytes: x\n")
                                             << false << quint64(0) << quint64(0);
}

void TestAdversarial::pidIo()
{
    QFETCH(QByteArray, text);
    QFETCH(bool, ok);
    QFETCH(quint64, readBytes);
    QFETCH(quint64, writeBytes);
    const auto io = parse::parsePidIo(std::string_view(text.constData(), size_t(text.size())));
    QCOMPARE(io.has_value(), ok);
    if (ok) {
        QCOMPARE(quint64(io->readBytes), readBytes);
        QCOMPARE(quint64(io->writeBytes), writeBytes);
    }
}

void TestAdversarial::statusRealUid_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<quint32>("uid");
    QTest::newRow("typical") << QByteArray("Name:\tbash\nUmask:\t0022\nState:\tS (sleeping)\nPid:\t1\n"
                                           "Uid:\t1000\t0\t0\t0\nGid:\t5\t5\t5\t5\n")
                             << true << quint32(1000);
    QTest::newRow("uid in name is not Uid line") << QByteArray("Name:\tUid:\t0\nUid:\t1000\t1000\t1000\t1000\n")
                                                 << true << quint32(1000);
    QTest::newRow("max uid") << QByteArray("Uid:\t4294967295\t0\t0\t0\n") << true << quint32(4294967295u);
    QTest::newRow("uid > uint32") << QByteArray("Uid:\t4294967296\t0\t0\t0\n") << false << quint32(0);
    QTest::newRow("negative") << QByteArray("Uid:\t-1\t0\t0\t0\n") << false << quint32(0);
    QTest::newRow("empty Uid") << QByteArray("Uid:\n") << false << quint32(0);
    QTest::newRow("missing") << QByteArray("Name:\tx\nGid:\t0\t0\t0\t0\n") << false << quint32(0);
    QTest::newRow("empty") << QByteArray("") << false << quint32(0);
}

void TestAdversarial::statusRealUid()
{
    QFETCH(QByteArray, text);
    QFETCH(bool, ok);
    QFETCH(quint32, uid);
    const auto r = parse::parseStatusRealUid(std::string_view(text.constData(), size_t(text.size())));
    QCOMPARE(r.has_value(), ok);
    if (ok)
        QCOMPARE(quint32(*r), uid);
}

void TestAdversarial::cmdline_data()
{
    QTest::addColumn<QByteArray>("raw");
    QTest::addColumn<QByteArray>("expected");
    QTest::newRow("argv") << QByteArray("/usr/bin/foo\0--bar\0baz qux\0", 27)
                          << QByteArray("/usr/bin/foo --bar baz qux");
    QTest::newRow("empty (zombie/kthread)") << QByteArray("") << QByteArray("");
    QTest::newRow("only NULs") << QByteArray("\0\0\0", 3) << QByteArray("");
    QTest::newRow("setproctitle no NUL") << QByteArray("postgres: writer   ") << QByteArray("postgres: writer");
    QTest::newRow("control bytes") << QByteArray("a\nb\x1b[31m") << QByteArray("a?b?[31m");
    QTest::newRow("leading NUL") << QByteArray("\0x", 2) << QByteArray("x");
}

void TestAdversarial::cmdline()
{
    QFETCH(QByteArray, raw);
    QFETCH(QByteArray, expected);
    const auto out = parse::parseCmdline(std::string_view(raw.constData(), size_t(raw.size())));
    QCOMPARE(QByteArray::fromStdString(out), expected);
}

// ===========================================================================
// /proc/mounts

void TestAdversarial::mountUnescape_data()
{
    // The kernel octal-escapes space, tab, newline and backslash in mount fields.
    QTest::addColumn<QByteArray>("in");
    QTest::addColumn<QByteArray>("expected");
    QTest::newRow("space") << QByteArray("/media/u/My\\040Disk") << QByteArray("/media/u/My Disk");
    QTest::newRow("tab") << QByteArray("/a\\011b") << QByteArray("/a\tb");
    QTest::newRow("newline") << QByteArray("/a\\012b") << QByteArray("/a\nb");
    QTest::newRow("backslash") << QByteArray("/a\\134b") << QByteArray("/a\\b");
    QTest::newRow("escaped backslash then digits") << QByteArray("/a\\134040") << QByteArray("/a\\040");
    QTest::newRow("at end") << QByteArray("/x\\040") << QByteArray("/x ");
    QTest::newRow("only escape") << QByteArray("\\040") << QByteArray(" ");
    QTest::newRow("truncated escape") << QByteArray("/x\\04") << QByteArray("/x\\04");
    QTest::newRow("lone backslash") << QByteArray("/x\\") << QByteArray("/x\\");
    QTest::newRow("out of range octal") << QByteArray("/x\\400") << QByteArray("/x\\400");
    QTest::newRow("non-octal digit") << QByteArray("/x\\089") << QByteArray("/x\\089");
    QTest::newRow("high byte") << QByteArray("/x\\377") << QByteArray("/x\xff");
}

void TestAdversarial::mountUnescape()
{
    QFETCH(QByteArray, in);
    QFETCH(QByteArray, expected);
    const auto out = parse::unescapeMountField(std::string_view(in.constData(), size_t(in.size())));
    QCOMPARE(QByteArray::fromStdString(out), expected);
}

void TestAdversarial::mountUnescapeNeverProducesNul()
{
    // Paths cannot contain NUL and the kernel never emits "\000". Decoding it
    // yields a std::string whose c_str() is a *different, shorter* path, so a
    // later statvfs() would report the capacity of another filesystem.
    const auto out = parse::unescapeMountField("/mnt/data\\000/../..");
    QVERIFY2(out.find('\0') == std::string::npos, "unescapeMountField produced an embedded NUL byte");
    const auto mounts = parse::parseMounts("/dev/sdb1 /srv\\000x ext4 rw 0 0\n");
    for (const auto& m : mounts)
        QVERIFY2(m.mountPoint.find('\0') == std::string::npos, "parseMounts returned a mount point with NUL");
}

void TestAdversarial::mountsParsing()
{
    const auto m = parse::parseMounts(
        "/dev/nvme0n1p2 / ext4 rw,relatime 0 0\n"
        "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
        "/dev/sdb1 /media/user/My\\040Disk vfat ro,nosuid 0 0\n"
        "/dev/disk/by-label/A\\040B /mnt/ab xfs rw 0 0\n"
        "none relative ext4 rw 0 0\n"       // mount point must be absolute
        "/dev/sdc1 /mnt/trunc\n"            // truncated
        "\n"
        "systemd-1 /proc/sys/fs/binfmt_misc autofs rw 0 0");
    QCOMPARE(m.size(), size_t(5));
    QCOMPARE(m[0].source, "/dev/nvme0n1p2"s);
    QCOMPARE(m[0].mountPoint, "/"s);
    QCOMPARE(m[0].fsType, "ext4"s);
    QCOMPARE(m[0].options, "rw,relatime"s);
    QVERIFY(!m[0].readOnly());
    QCOMPARE(m[2].mountPoint, "/media/user/My Disk"s);
    QVERIFY(m[2].readOnly());
    QCOMPARE(m[3].source, "/dev/disk/by-label/A B"s);
    QCOMPARE(m[4].fsType, "autofs"s);
}

void TestAdversarial::mountReadOnly_data()
{
    QTest::addColumn<QByteArray>("options");
    QTest::addColumn<bool>("ro");
    QTest::newRow("ro") << QByteArray("ro") << true;
    QTest::newRow("ro first") << QByteArray("ro,relatime") << true;
    QTest::newRow("ro last") << QByteArray("noatime,ro") << true;
    QTest::newRow("ro trailing comma") << QByteArray("ro,") << true;
    QTest::newRow("remount-ro is not ro") << QByteArray("rw,errors=remount-ro") << false;
    QTest::newRow("rootcontext") << QByteArray("rw,rootcontext=system_u:object_r:x") << false;
    QTest::newRow("rw") << QByteArray("rw") << false;
    QTest::newRow("empty") << QByteArray("") << false;
    QTest::newRow("Ro case") << QByteArray("Ro") << false;
}

void TestAdversarial::mountReadOnly()
{
    QFETCH(QByteArray, options);
    QFETCH(bool, ro);
    parse::MountEntry e;
    e.options = options.toStdString();
    QCOMPARE(e.readOnly(), ro);
}

void TestAdversarial::localCapacityFs_data()
{
    QTest::addColumn<QByteArray>("source");
    QTest::addColumn<QByteArray>("type");
    QTest::addColumn<bool>("local");
    QTest::newRow("ext4 dev") << QByteArray("/dev/nvme0n1p2") << QByteArray("ext4") << true;
    QTest::newRow("btrfs mapper") << QByteArray("/dev/mapper/luks-root") << QByteArray("btrfs") << true;
    QTest::newRow("ext4 non-dev source") << QByteArray("/home/img.ext4") << QByteArray("ext4") << false;
    QTest::newRow("zfs dataset") << QByteArray("rpool/ROOT/ubuntu") << QByteArray("zfs") << true;
    QTest::newRow("zfs abs path") << QByteArray("/dev/zd0") << QByteArray("zfs") << false;
    QTest::newRow("zfs empty source") << QByteArray("") << QByteArray("zfs") << false;
    QTest::newRow("nfs") << QByteArray("server:/export") << QByteArray("nfs4") << false;
    QTest::newRow("cifs") << QByteArray("//srv/share") << QByteArray("cifs") << false;
    QTest::newRow("fuse sshfs") << QByteArray("u@h:/") << QByteArray("fuse.sshfs") << false;
    QTest::newRow("fuseblk") << QByteArray("/dev/sdb1") << QByteArray("fuseblk") << false;
    QTest::newRow("tmpfs") << QByteArray("tmpfs") << QByteArray("tmpfs") << false;
    QTest::newRow("overlay") << QByteArray("overlay") << QByteArray("overlay") << false;
    QTest::newRow("autofs") << QByteArray("systemd-1") << QByteArray("autofs") << false;
    QTest::newRow("type prefix ext4x") << QByteArray("/dev/sda1") << QByteArray("ext4x") << false;
    QTest::newRow("dev prefix only") << QByteArray("/devices/sda1") << QByteArray("ext4") << false;
}

void TestAdversarial::localCapacityFs()
{
    QFETCH(QByteArray, source);
    QFETCH(QByteArray, type);
    QFETCH(bool, local);
    parse::MountEntry e;
    e.source = source.toStdString();
    e.mountPoint = "/mnt";
    e.fsType = type.toStdString();
    e.options = "rw";
    QCOMPARE(parse::isLocalCapacityFs(e), local);
}

// ===========================================================================
// temperature, loadavg, uptime

void TestAdversarial::milliCelsius_data()
{
    QTest::addColumn<QByteArray>("in");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<double>("celsius");
    QTest::newRow("typical") << QByteArray("45000\n") << true << 45.0;
    QTest::newRow("fraction") << QByteArray("45123\n") << true << 45.123;
    QTest::newRow("negative") << QByteArray("-5500\n") << true << -5.5;
    QTest::newRow("upper bound") << QByteArray("200000") << true << 200.0;
    QTest::newRow("above bound") << QByteArray("200001") << false << 0.0;
    QTest::newRow("lower bound") << QByteArray("-60000") << true << -60.0;
    QTest::newRow("below bound") << QByteArray("-60001") << false << 0.0;
    QTest::newRow("error marker -273200") << QByteArray("-273200") << false << 0.0;
    QTest::newRow("int32 max marker") << QByteArray("2147483647") << false << 0.0;
    QTest::newRow("int64 max") << QByteArray("9223372036854775807") << false << 0.0;
    QTest::newRow("padded") << QByteArray("  45000 \n\n") << true << 45.0;
    QTest::newRow("decimal") << QByteArray("45.0") << false << 0.0;
    QTest::newRow("junk") << QByteArray("45000C") << false << 0.0;
    QTest::newRow("empty") << QByteArray("") << false << 0.0;
    QTest::newRow("two values") << QByteArray("45000 46000") << false << 0.0;
}

void TestAdversarial::milliCelsius()
{
    QFETCH(QByteArray, in);
    QFETCH(bool, ok);
    QFETCH(double, celsius);
    const auto r = parse::parseMilliCelsius(std::string_view(in.constData(), size_t(in.size())));
    QCOMPARE(r.has_value(), ok);
    if (ok)
        QVERIFY(near(*r, celsius));
}

void TestAdversarial::loadavg_data()
{
    QTest::addColumn<QByteArray>("in");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<double>("one");
    QTest::addColumn<double>("fifteen");
    QTest::newRow("typical") << QByteArray("0.52 0.58 0.59 3/1234 56789\n") << true << 0.52 << 0.59;
    QTest::newRow("big") << QByteArray("1024.00 512.50 256.25 1/1 1") << true << 1024.0 << 256.25;
    QTest::newRow("three only") << QByteArray("1.00 2.00 3.00") << true << 1.0 << 3.0;
    QTest::newRow("truncated") << QByteArray("0.52 0.58") << false << 0.0 << 0.0;
    QTest::newRow("negative") << QByteArray("-0.01 0.58 0.59") << false << 0.0 << 0.0;
    QTest::newRow("nan") << QByteArray("nan 0.58 0.59") << false << 0.0 << 0.0;
    QTest::newRow("inf") << QByteArray("0.52 inf 0.59") << false << 0.0 << 0.0;
    QTest::newRow("comma") << QByteArray("0,52 0,58 0,59") << false << 0.0 << 0.0;
    QTest::newRow("empty") << QByteArray("") << false << 0.0 << 0.0;
}

void TestAdversarial::loadavg()
{
    QFETCH(QByteArray, in);
    QFETCH(bool, ok);
    QFETCH(double, one);
    QFETCH(double, fifteen);
    const auto r = parse::parseLoadavg(std::string_view(in.constData(), size_t(in.size())));
    QCOMPARE(r.has_value(), ok);
    if (ok) {
        QVERIFY(near(r->one, one));
        QVERIFY(near(r->fifteen, fifteen));
    }
}

void TestAdversarial::uptime_data()
{
    QTest::addColumn<QByteArray>("in");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<double>("seconds");
    QTest::newRow("typical") << QByteArray("12345.67 98765.43\n") << true << 12345.67;
    QTest::newRow("zero") << QByteArray("0.00 0.00") << true << 0.0;
    QTest::newRow("negative") << QByteArray("-1.00 0.00") << false << 0.0;
    QTest::newRow("nan") << QByteArray("nan 0") << false << 0.0;
    QTest::newRow("junk") << QByteArray("12s 0") << false << 0.0;
    QTest::newRow("empty") << QByteArray("\n") << false << 0.0;
}

void TestAdversarial::uptime()
{
    QFETCH(QByteArray, in);
    QFETCH(bool, ok);
    QFETCH(double, seconds);
    const auto r = parse::parseUptime(std::string_view(in.constData(), size_t(in.size())));
    QCOMPARE(r.has_value(), ok);
    if (ok)
        QVERIFY(near(*r, seconds));
}

// ===========================================================================
// counter / rate / process math

void TestAdversarial::counterDeltaSemantics()
{
    QCOMPARE(metrics::counterDelta(100, 150), std::optional<uint64_t>(50));
    QCOMPARE(metrics::counterDelta(7, 7), std::optional<uint64_t>(0));
    QCOMPARE(metrics::counterDelta(0, kU64Max), std::optional<uint64_t>(kU64Max));
    QVERIFY(!metrics::counterDelta(150, 100).has_value());          // reset, 64-bit
    QVERIFY(!metrics::counterDelta(kU64Max, 0).has_value());        // no 64-bit wrap guess
    // 32-bit wrap: 2^32 - 0xFFFFFF00 = 256, plus 16.
    QCOMPARE(metrics::counterDelta(0xFFFFFF00ull, 0x10ull, 32), std::optional<uint64_t>(272));
    QVERIFY(!metrics::counterDelta(1000, 10, 32).has_value());      // low prev: reset
    QVERIFY(!metrics::counterDelta(0x1FFFFFF00ull, 0x10ull, 32).has_value()); // prev not a 32-bit value
}

void TestAdversarial::rateSemantics()
{
    QCOMPARE(metrics::rate(uint64_t(1000), 2.0), 500.0);
    QCOMPARE(metrics::rate(uint64_t(0), 1.0), 0.0); // measured zero, not NaN
    QVERIFY(!ps::isValid(metrics::rate(std::nullopt, 1.0)));
    QVERIFY(!ps::isValid(metrics::rate(uint64_t(1), 0.0)));
    QVERIFY(!ps::isValid(metrics::rate(uint64_t(1), -1.0)));
    QVERIFY(!ps::isValid(metrics::rate(uint64_t(1), ps::kNaN)));
    QVERIFY(!ps::isValid(metrics::rate(uint64_t(1), std::numeric_limits<double>::infinity())));
    QVERIFY(!ps::isValid(metrics::rate(uint64_t(1), 1.0, 0)));
    // delta * 512 overflows uint64: must be NaN, not a wrapped small rate.
    QVERIFY(!ps::isValid(metrics::rate(uint64_t(1) << 60, 1.0, 512)));
    // Glitch filter: > 1 TiB/s rejected, exactly 1 TiB/s kept.
    QCOMPARE(metrics::rate(uint64_t(1) << 40, 1.0), 1099511627776.0);
    QVERIFY(!ps::isValid(metrics::rate((uint64_t(1) << 40) + 1, 1.0)));
}

void TestAdversarial::processCpuPercentOneCoreIs100()
{
    // CLK_TCK=100: 100 ticks in 1 s = one logical CPU fully busy = 100 %.
    QCOMPARE(metrics::processCpuPercent(1000, 1100, 1.0, 100, 8), 100.0);
    QCOMPARE(metrics::processCpuPercent(1000, 1050, 0.5, 100, 8), 100.0);
    QCOMPARE(metrics::processCpuPercent(0, 400, 1.0, 100, 4), 400.0); // 4 threads on 4 CPUs
    QCOMPARE(metrics::processCpuPercent(5, 5, 1.0, 100, 1), 0.0);
    // Impossible for the machine: 8 CPUs worth on a 2-CPU system.
    QVERIFY(!ps::isValid(metrics::processCpuPercent(0, 800, 1.0, 100, 2)));
    // PID reuse / counter regression.
    QVERIFY(!ps::isValid(metrics::processCpuPercent(500, 499, 1.0, 100, 4)));
    QVERIFY(!ps::isValid(metrics::processCpuPercent(0, 1, 0.0, 100, 4)));
    QVERIFY(!ps::isValid(metrics::processCpuPercent(0, 1, ps::kNaN, 100, 4)));
    QVERIFY(!ps::isValid(metrics::processCpuPercent(0, 1, 1.0, 0, 4)));
    QVERIFY(!ps::isValid(metrics::processCpuPercent(0, kU64Max, 1.0, 100, 4)));
}

void TestAdversarial::continuity()
{
    // limit = max(2.5 * e, e + 1)
    QCOMPARE(metrics::continuityLimit(1.0), 2.5);
    QCOMPARE(metrics::continuityLimit(0.5), 1.5);
    QCOMPARE(metrics::continuityLimit(0.0), 2.5); // falls back to 1 s
    QVERIFY(!metrics::isDiscontinuous(2.5, 1.0));
    QVERIFY(metrics::isDiscontinuous(2.51, 1.0));
    QVERIFY(metrics::isDiscontinuous(0.0, 1.0));
    QVERIFY(metrics::isDiscontinuous(-0.1, 1.0));
    QVERIFY(metrics::isDiscontinuous(ps::kNaN, 1.0));
    QVERIFY(metrics::isDiscontinuous(std::numeric_limits<double>::infinity(), 1.0));
}

// ===========================================================================
// display boundary

void TestAdversarial::formatUnavailableAndHuge()
{
    QCOMPARE(ps::fmt::bytes(ps::kNaN), "n/a"s);
    QCOMPARE(ps::fmt::rate(std::numeric_limits<double>::infinity()), "n/a"s);
    QCOMPARE(ps::fmt::percent(ps::kNaN), "n/a"s);
    QCOMPARE(ps::fmt::percent(-0.01), "0.0 %"s); // no "-0.0 %"
    QCOMPARE(ps::fmt::duration(-1.0), "n/a"s);
    QCOMPARE(ps::fmt::duration(3725.0), "1h 02m"s);
    // A bogus starttime can produce an enormous process age. Converting a
    // double beyond the long long range is undefined behaviour and on x86-64
    // yields LLONG_MIN, i.e. a negative "age".
    const auto huge = ps::fmt::duration(1e300);
    QVERIFY2(huge.find('-') == std::string::npos,
             qPrintable(QStringLiteral("duration(1e300) = \"%1\"").arg(QString::fromStdString(huge))));
    const auto big = ps::fmt::duration(9.3e18);
    QVERIFY2(big.find('-') == std::string::npos,
             qPrintable(QStringLiteral("duration(9.3e18) = \"%1\"").arg(QString::fromStdString(big))));
}

// ===========================================================================
// History

void TestAdversarial::historyCapacityBound()
{
    ps::History h(5, 1e9);
    for (int i = 1; i <= 10; ++i) {
        QVERIFY(h.beginSample(i, true));
        QVERIFY(h.set("k", "K", ps::Unit::Percent, i * 10.0));
        h.endSample();
    }
    QCOMPARE(h.size(), size_t(5));
    QCOMPARE(h.capacity(), size_t(5));
    QCOMPARE(h.totalSamples(), uint64_t(10));
    QCOMPARE(h.time(0), 6.0);
    QCOMPARE(h.time(4), 10.0);
    const auto* s = h.find("k");
    QVERIFY(s);
    QCOMPARE(h.value(*s, 0), 60.0);
    QCOMPARE(h.value(*s, 4), 100.0);
    QCOMPARE(h.latestValue("k"), 100.0);
    QCOMPARE(ps::History(0, 1e9).capacity(), size_t(2));
}

void TestAdversarial::historyRealTimeCutoffAndGaps()
{
    ps::History h(1000, 10.0);
    for (int t = 0; t <= 20; ++t) {
        QVERIFY(h.beginSample(t, t != 0));
        h.set("k", "K", ps::Unit::Percent, t);
        h.endSample();
    }
    // Retain samples with time >= newest - maxAge: 10..20.
    QCOMPARE(h.size(), size_t(11));
    QCOMPARE(h.time(0), 10.0);
    QVERIFY(h.continuous(0));
    QVERIFY(h.beginSample(20.5, true));
    h.endSample();
    QCOMPARE(h.time(0), 11.0);
    QCOMPARE(h.size(), size_t(11));
    // Suspend/resume gap: everything older than the window is dropped at once
    // and the gap is marked, not bridged.
    QVERIFY(h.beginSample(1000.0, false));
    h.set("k", "K", ps::Unit::Percent, 1);
    h.endSample();
    QCOMPARE(h.size(), size_t(1));
    QCOMPARE(h.time(0), 1000.0);
    QVERIFY(!h.continuous(0));
    QCOMPARE(h.latestValue("k"), 1.0);
}

void TestAdversarial::historyRejectsBadTimesAndValues()
{
    ps::History h(10, 100);
    QVERIFY(!h.set("k", "K", ps::Unit::Percent, 1)); // no open sample
    QVERIFY(!h.beginSample(ps::kNaN, true));
    QVERIFY(!h.beginSample(std::numeric_limits<double>::infinity(), true));
    QVERIFY(h.beginSample(5, true));
    QVERIFY(h.set("k", "K", ps::Unit::Percent, std::numeric_limits<double>::infinity()));
    h.endSample();
    QVERIFY(!h.beginSample(5, true)); // equal time
    QVERIFY(!h.beginSample(4, true)); // backwards
    QCOMPARE(h.size(), size_t(1));
    const auto* s = h.find("k");
    QVERIFY(s);
    QVERIFY(!s->everValid);
    QVERIFY(std::isnan(h.value(*s, 0)));
}

void TestAdversarial::historyMissingValuesAreGapsNotZero()
{
    ps::History h(10, 100);
    h.beginSample(1, true);
    h.set("a", "A", ps::Unit::Bytes, 5);
    h.endSample();
    h.beginSample(2, true);
    h.set("b", "B", ps::Unit::Bytes, 7);
    h.endSample();
    h.beginSample(3, true);
    h.set("a", "A", ps::Unit::Bytes, 0);
    h.endSample();
    const auto* a = h.find("a");
    const auto* b = h.find("b");
    QVERIFY(a && b);
    QCOMPARE(h.value(*a, 0), 5.0);
    QVERIFY(std::isnan(h.value(*a, 1)));
    QCOMPARE(h.value(*a, 2), 0.0);
    QVERIFY(std::isnan(h.value(*b, 0))); // created later: earlier slots are gaps
    QCOMPARE(h.value(*b, 1), 7.0);
    QVERIFY(std::isnan(h.value(*b, 2)));
}

void TestAdversarial::historyDeviceChurnStaysBounded()
{
    ps::History h(10, 1e9);
    for (int i = 0; i < 1000; ++i) {
        QVERIFY(h.beginSample(i + 1, true));
        h.set("cpu", "CPU", ps::Unit::Percent, 1);
        h.set("dev." + std::to_string(i), "dev", ps::Unit::BytesPerSecond, 1);
        h.endSample();
    }
    // Oldest retained sample is #991, so dev.990..dev.999 plus cpu remain.
    QCOMPARE(h.size(), size_t(10));
    QCOMPARE(h.seriesCount(), size_t(11));
    QVERIFY(h.find("dev.999"));
    QVERIFY(h.find("dev.990"));
    QVERIFY(!h.find("dev.989"));

    // Devices that never produce a valid value must also age out.
    ps::History n(10, 1e9);
    for (int i = 0; i < 500; ++i) {
        n.beginSample(i + 1, true);
        n.set("nan." + std::to_string(i), "x", ps::Unit::BytesPerSecond, ps::kNaN);
        n.endSample();
    }
    QCOMPARE(n.seriesCount(), size_t(10));

    // Time-based retention also prunes churned series.
    ps::History t(100000, 5.0);
    for (int i = 0; i < 300; ++i) {
        t.beginSample(i, true);
        t.set("temp." + std::to_string(i), "x", ps::Unit::Celsius, 40);
        t.endSample();
    }
    QCOMPARE(t.size(), size_t(6)); // 294..299
    QCOMPARE(t.seriesCount(), size_t(6));
}

void TestAdversarial::historySeriesCapAndRecovery()
{
    ps::History h(4, 1e9);
    h.beginSample(1, true);
    int created = 0;
    for (int i = 0; i < 600; ++i)
        created += h.set("s" + std::to_string(i), "s", ps::Unit::Percent, 1) ? 1 : 0;
    h.endSample();
    QCOMPARE(created, int(ps::History::kMaxSeries));
    QCOMPARE(h.seriesCount(), ps::History::kMaxSeries);
    for (int t = 2; t <= 5; ++t) {
        h.beginSample(t, true);
        h.endSample();
    }
    // Sample #1 left the ring, so the saturated series are gone and new keys work.
    QCOMPARE(h.seriesCount(), size_t(0));
    h.beginSample(6, true);
    QVERIFY(h.set("fresh", "F", ps::Unit::Percent, 1));
    h.endSample();
    QCOMPARE(h.seriesCount(), size_t(1));
}

void TestAdversarial::historyLowerBound()
{
    ps::History h(3, 1e9); // ring wraps: 1,2,4,8 -> 2,4,8
    for (double t : {1.0, 2.0, 4.0, 8.0}) {
        h.beginSample(t, true);
        h.endSample();
    }
    QCOMPARE(h.size(), size_t(3));
    QCOMPARE(h.lowerBound(0.0), size_t(0));
    QCOMPARE(h.lowerBound(2.0), size_t(0));
    QCOMPARE(h.lowerBound(3.0), size_t(1));
    QCOMPARE(h.lowerBound(4.0), size_t(1));
    QCOMPARE(h.lowerBound(8.0), size_t(2));
    QCOMPARE(h.lowerBound(100.0), size_t(3));
    QVERIFY(std::isnan(ps::History().latestTime()));
}

void TestAdversarial::historyCsvQuotingAndBlanks()
{
    ps::History h(10, 1e9);
    h.beginSample(10.5, false);
    h.set("a", "Receive: a,b\"c", ps::Unit::BytesPerSecond, 1536.26);
    h.set("b", "Temp", ps::Unit::Celsius, ps::kNaN);
    h.endSample();
    h.beginSample(11.5, true);
    h.set("a", "Receive: a,b\"c", ps::Unit::BytesPerSecond, ps::kNaN);
    h.set("b", "Temp", ps::Unit::Celsius, -5.1234);
    h.endSample();
    QCOMPARE(h.toCsv(1700000000.0),
             "time_since_boot_s,unix_time_s,continuous,\"Receive: a,b\"\"c (B/s)\",Temp (\u00B0C)\r\n"
             "10.500,1700000010.500,0,1536.3,\r\n"
             "11.500,1700000011.500,1,,-5.123\r\n"s);
    const auto noUnix = h.toCsv(ps::kNaN);
    QVERIFY(noUnix.find("\r\n10.500,,0,1536.3,\r\n") != std::string::npos);
}

void TestAdversarial::historyRecordSnapshotConventions()
{
    ps::History h(10, 1e9);
    ps::Snapshot s;
    s.time = 5;
    s.continuous = false;
    s.cpu.total = 12.5;
    s.cores.push_back({0, true, {}});
    s.cores[0].usage.total = 50;
    s.memory.valid = false;
    s.memory.hasSwap = false;
    s.disks.push_back({});
    s.disks[0].name = "sda";
    s.disks[0].readBps = 1;
    s.disks[0].writeBps = 2;
    s.netTotal.label = "all physical interfaces";
    s.netTotal.members = 1;
    s.netTotal.a = 3;
    s.netTotal.b = 4;
    s.interfaces.push_back({});
    s.interfaces[0].name = "eth0";
    s.interfaces[0].rxBps = 3;
    s.interfaces[0].txBps = 4;
    s.temperatures.push_back({});
    s.temperatures[0].key = "k1";
    s.temperatures[0].displayName = "chip \u00B7 t";
    s.temperatures[0].celsius = -5;
    ps::recordSnapshot(h, s);

    QCOMPARE(h.size(), size_t(1));
    QVERIFY(!h.continuous(0));
    QCOMPARE(h.latestValue(ps::keys::cpuTotal), 12.5);
    QCOMPARE(h.latestValue(ps::keys::core(0)), 50.0);
    QVERIFY(!h.find(ps::keys::memUsed));
    QVERIFY(!h.find(ps::keys::swapUsed));
    QVERIFY(!h.find(ps::keys::diskAllRead)); // no aggregate members
    QCOMPARE(h.latestValue(ps::keys::diskRead("sda")), 1.0);
    QCOMPARE(h.latestValue(ps::keys::netAllRx), 3.0);
    QCOMPARE(h.latestValue(ps::keys::netTx("eth0")), 4.0);
    QCOMPARE(h.latestValue(ps::keys::temperature("k1")), -5.0);

    ps::recordSnapshot(h, s); // same timestamp: ignored
    QCOMPARE(h.size(), size_t(1));

    ps::Snapshot s2;
    s2.time = 6;
    s2.continuous = true;
    s2.memory.valid = true;
    s2.memory.used = 100;
    s2.memory.hasSwap = true;
    s2.memory.swapUsed = 0;
    ps::recordSnapshot(h, s2);
    QCOMPARE(h.size(), size_t(2));
    QCOMPARE(h.latestValue(ps::keys::swapUsed), 0.0); // real zero
    const auto* used = h.find(ps::keys::memUsed);
    QVERIFY(used);
    QVERIFY(std::isnan(h.value(*used, 0)));
    QCOMPARE(h.value(*used, 1), 100.0);
    QVERIFY(std::isnan(h.latestValue(ps::keys::cpuTotal))); // gap, not carried forward
}

// ===========================================================================
// Sampler over injected /proc and /sys trees

namespace {

void writeProc(const FixtureRoot& r, int pid, const std::string& stat, const std::string& cmdline,
               const std::string& io = {}, uint32_t uid = 1000)
{
    const std::string base = "proc/" + std::to_string(pid) + "/";
    r.write(base + "stat", stat);
    r.write(base + "status", "Name:\tx\nUmask:\t0022\nUid:\t" + u(uid) + "\t" + u(uid) + "\t" + u(uid) + "\t"
                                 + u(uid) + "\nGid:\t0\t0\t0\t0\n");
    r.write(base + "cmdline", cmdline);
    if (!io.empty())
        r.write(base + "io", io);
}

std::string ioText(uint64_t readBytes, uint64_t writeBytes)
{
    return "rchar: 1\nwchar: 2\nsyscr: 3\nsyscw: 4\nread_bytes: " + u(readBytes) + "\nwrite_bytes: " + u(writeBytes)
        + "\ncancelled_write_bytes: 0\n";
}

void addNetIf(const FixtureRoot& r, std::string_view name, int ifindex, int type, std::string_view oper,
              bool physical)
{
    const std::string base = "sys/class/net/" + std::string(name) + "/";
    r.write(base + "ifindex", std::to_string(ifindex) + "\n");
    r.write(base + "type", std::to_string(type) + "\n");
    r.write(base + "operstate", std::string(oper) + "\n");
    if (physical)
        r.mkdir(base + "device");
}

void addBlock(const FixtureRoot& r, std::string_view sysName, uint64_t sizeSectors, bool physical,
              std::string_view dmName = {}, std::string_view diskseq = {})
{
    const std::string base = "sys/block/" + std::string(sysName) + "/";
    r.write(base + "size", u(sizeSectors) + "\n");
    if (physical)
        r.mkdir(base + "device");
    if (!dmName.empty())
        r.write(base + "dm/name", std::string(dmName) + "\n");
    if (!diskseq.empty())
        r.write(base + "diskseq", std::string(diskseq) + "\n");
}

bool rootUser() { return ::geteuid() == 0; }

} // namespace

void TestAdversarial::samplerFirstSampleIsBaselineOnly()
{
    FixtureRoot r;
    QVERIFY(r.ok());
    writeBaseTree(r);
    r.write("proc/stat", cpuLine("cpu", 100, 50, 850) + cpuLine("cpu0", 100, 50, 850) + "btime 1700000000\n");
    ps::Sampler smp(fixtureOptions(r));
    const auto s1 = smp.sample(100, 1);
    QCOMPARE(s1.sequence, uint64_t(1));
    QVERIFY(!s1.continuous);
    QVERIFY(!ps::isValid(s1.interval));
    QVERIFY(!ps::isValid(s1.cpu.total));
    QCOMPARE(s1.logicalCpus, 1);
    QVERIFY(s1.memory.valid); // gauges are immediate
    QCOMPARE(s1.memory.used, 500.0 * 1024);
    QCOMPARE(s1.load1, 0.10);
    QCOMPARE(s1.load15, 0.30);
    QVERIFY(s1.problems.empty());
    const auto s2 = smp.sample(101, 1);
    QCOMPARE(s2.sequence, uint64_t(2));
    QVERIFY(s2.continuous);
    QCOMPARE(s2.interval, 1.0);
    QVERIFY(!ps::isValid(s2.cpu.total)); // no ticks elapsed: unknown, not 0 %
}

void TestAdversarial::samplerCpuCountersAbove2p53()
{
    // 2^60 has a double ulp of 256; subtracting after conversion yields 0 or 256.
    constexpr uint64_t B = uint64_t(1) << 60;
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r));
    r.write("proc/stat", cpuLine("cpu", B, 0, B) + cpuLine("cpu0", B, 0, B));
    smp.sample(10, 1);
    r.write("proc/stat", cpuLine("cpu", B + 30, 0, B + 70) + cpuLine("cpu0", B + 30, 0, B + 70));
    const auto s = smp.sample(11, 1);
    QVERIFY(near(s.cpu.total, 30.0));
    QVERIFY(near(s.cpu.user, 30.0));
    const auto* c0 = findCore(s, 0);
    QVERIFY(c0);
    QVERIFY(near(c0->usage.total, 30.0));
}

void TestAdversarial::samplerCpuGuestAndIdleAccounting()
{
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r));
    r.write("proc/stat", cpuLine("cpu", 1000, 100, 5000, 100, 300) + cpuLine("cpu0", 1000, 100, 5000, 100, 300));
    smp.sample(1, 1);
    // user +60 (guest +40 inside it), system +10, idle +20, iowait +10.
    r.write("proc/stat", cpuLine("cpu", 1060, 110, 5020, 110, 340) + cpuLine("cpu0", 1060, 110, 5020, 110, 340));
    const auto s = smp.sample(2, 1);
    // total ticks 100 (140 if guest were added again): busy 70.
    QVERIFY(near(s.cpu.total, 70.0));
    QVERIFY(near(s.cpu.user, 60.0));
    QVERIFY(near(s.cpu.system, 10.0));
    QVERIFY(near(s.cpu.iowait, 10.0));
}

void TestAdversarial::samplerCpuIowaitRegressionInvalidatesThenRebases()
{
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r));
    r.write("proc/stat", cpuLine("cpu", 100, 0, 1000, 500) + cpuLine("cpu0", 100, 0, 1000, 500));
    smp.sample(1, 1);
    r.write("proc/stat", cpuLine("cpu", 110, 0, 1090, 490) + cpuLine("cpu0", 110, 0, 1090, 490));
    const auto s2 = smp.sample(2, 1);
    QVERIFY(s2.continuous);
    QVERIFY(!ps::isValid(s2.cpu.total));
    QVERIFY(!ps::isValid(s2.cpu.iowait));
    QVERIFY(!ps::isValid(findCore(s2, 0)->usage.total));
    r.write("proc/stat", cpuLine("cpu", 130, 0, 1170, 495) + cpuLine("cpu0", 130, 0, 1170, 495));
    const auto s3 = smp.sample(3, 1);
    // Rebased on s2: user 20, idle 80, iowait 5 -> total 105.
    QVERIFY(near(s3.cpu.total, 100.0 * 20 / 105));
    QVERIFY(near(s3.cpu.iowait, 100.0 * 5 / 105));
    QVERIFY(near(findCore(s3, 0)->usage.total, 100.0 * 20 / 105));
}

void TestAdversarial::samplerCpuTruncatedRowDoesNotSpike()
{
    // A full row, then a row that lost its trailing fields, then a full row.
    // Zero-filling iowait in the middle sample must not turn into a huge
    // iowait "delta" on recovery.
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r));
    r.write("proc/stat", cpuLine("cpu", 100, 0, 1000, 500) + cpuLine("cpu0", 100, 0, 1000, 500));
    smp.sample(1, 1);
    r.write("proc/stat", "cpu 110 0 0 1090\ncpu0 110 0 0 1090\n");
    const auto s2 = smp.sample(2, 1);
    QVERIFY(!ps::isValid(s2.cpu.total));
    r.write("proc/stat", cpuLine("cpu", 120, 0, 1180, 505) + cpuLine("cpu0", 120, 0, 1180, 505));
    const auto s3 = smp.sample(3, 1);
    // Acceptable: unavailable, or measured against s1 (user 20, idle 180, iowait 5 of 205).
    const bool ok = !ps::isValid(s3.cpu.iowait) || near(s3.cpu.iowait, 100.0 * 5 / 205);
    QVERIFY2(ok, qPrintable(QStringLiteral("iowait after truncated row = %1 % (total %2 %)")
                                .arg(s3.cpu.iowait).arg(s3.cpu.total)));
}

void TestAdversarial::samplerCpuHotplug()
{
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r));
    r.write("proc/stat", cpuLine("cpu", 200, 0, 1800) + cpuLine("cpu0", 100, 0, 900) + cpuLine("cpu1", 100, 0, 900));
    smp.sample(1, 1);
    // cpu1 offline; the aggregate row is deliberately inconsistent (would read 100 %).
    r.write("proc/stat", cpuLine("cpu", 1200, 0, 1800) + cpuLine("cpu0", 150, 0, 950));
    const auto s2 = smp.sample(2, 1);
    QCOMPARE(s2.logicalCpus, 1);
    QCOMPARE(s2.cores.size(), size_t(2));
    QVERIFY(findCore(s2, 0)->online);
    QVERIFY(near(findCore(s2, 0)->usage.total, 50.0));
    QVERIFY(!findCore(s2, 1)->online);
    QVERIFY(!ps::isValid(findCore(s2, 1)->usage.total));
    QVERIFY(near(s2.cpu.total, 50.0)); // cores present in both samples only
    // cpu1 returns with reset counters.
    r.write("proc/stat", cpuLine("cpu", 165, 0, 1045) + cpuLine("cpu0", 160, 0, 1040) + cpuLine("cpu1", 5, 0, 5));
    const auto s3 = smp.sample(3, 1);
    QCOMPARE(s3.logicalCpus, 2);
    QVERIFY(findCore(s3, 1)->online);
    QVERIFY(!ps::isValid(findCore(s3, 1)->usage.total));
    QVERIFY(near(s3.cpu.total, 10.0));
    r.write("proc/stat", cpuLine("cpu", 245, 0, 1165) + cpuLine("cpu0", 190, 0, 1110) + cpuLine("cpu1", 55, 0, 55));
    const auto s4 = smp.sample(4, 1);
    QVERIFY(near(findCore(s4, 0)->usage.total, 30.0));
    QVERIFY(near(findCore(s4, 1)->usage.total, 50.0));
    QVERIFY(near(s4.cpu.total, 40.0));
}

void TestAdversarial::samplerCpuSourceLossAndRecovery()
{
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r));
    r.write("proc/stat", cpuLine("cpu", 100, 0, 900) + cpuLine("cpu0", 50, 0, 450) + cpuLine("cpu1", 50, 0, 450));
    smp.sample(1, 1);
    r.remove("proc/stat");
    const auto s2 = smp.sample(2, 1);
    QVERIFY(hasProblem(s2, "/proc/stat unreadable"));
    QVERIFY(!ps::isValid(s2.cpu.total));
    QCOMPARE(s2.cores.size(), size_t(2));
    QVERIFY(!s2.cores[0].online && !s2.cores[1].online);
    r.write("proc/stat", cpuLine("cpu", 300, 0, 1700) + cpuLine("cpu0", 150, 0, 850) + cpuLine("cpu1", 150, 0, 850));
    const auto s3 = smp.sample(3, 1);
    QVERIFY(!hasProblem(s3, "/proc/stat"));
    QVERIFY(!ps::isValid(s3.cpu.total)); // no stale two-interval delta
    QVERIFY(!ps::isValid(findCore(s3, 0)->usage.total));
    r.write("proc/stat", cpuLine("cpu", 310, 0, 1790) + cpuLine("cpu0", 155, 0, 895) + cpuLine("cpu1", 155, 0, 895));
    const auto s4 = smp.sample(4, 1);
    QVERIFY(near(s4.cpu.total, 10.0));
    QVERIFY(near(findCore(s4, 1)->usage.total, 10.0));
}

void TestAdversarial::samplerGapResetAndBackwardsTime()
{
    FixtureRoot r;
    writeBaseTree(r);
    addNetIf(r, "eth0", 2, 1, "up", true);
    ps::Sampler smp(fixtureOptions(r));
    uint64_t rx = 0;
    auto step = [&](double t) {
        rx += 1000;
        r.write("proc/net/dev", std::string(kNetHeader) + netDevLine("eth0", rx, 0));
        return smp.sample(t, 1.0);
    };
    step(100);
    const auto s2 = step(101);
    QVERIFY(s2.continuous);
    QCOMPARE(findNet(s2, "eth0")->rxBps, 1000.0);
    const auto s3 = step(110); // 9 s gap > 2.5 s limit
    QVERIFY(!s3.continuous);
    QVERIFY(!ps::isValid(s3.interval));
    QVERIFY(!ps::isValid(findNet(s3, "eth0")->rxBps));
    const auto s4 = step(111);
    QCOMPARE(findNet(s4, "eth0")->rxBps, 1000.0);
    smp.resetBaselines();
    const auto s5 = step(112);
    QVERIFY(!s5.continuous);
    QVERIFY(!ps::isValid(findNet(s5, "eth0")->rxBps));
    const auto s6 = step(111.5); // clock went backwards
    QVERIFY(!s6.continuous);
    QVERIFY(!ps::isValid(findNet(s6, "eth0")->rxBps));
    const auto s7 = step(112.5);
    QVERIFY(s7.continuous);
    QCOMPARE(findNet(s7, "eth0")->rxBps, 1000.0);
}

void TestAdversarial::samplerIrregularElapsedUsesActualInterval()
{
    FixtureRoot r;
    writeBaseTree(r);
    addNetIf(r, "eth0", 2, 1, "up", true);
    ps::Sampler smp(fixtureOptions(r));
    r.write("proc/net/dev", std::string(kNetHeader) + netDevLine("eth0", 0, 0));
    smp.sample(50.0, 0.5);
    r.write("proc/net/dev", std::string(kNetHeader) + netDevLine("eth0", 1200, 600));
    const auto s2 = smp.sample(51.2, 0.5); // late tick, within max(1.25, 1.5)
    QVERIFY(s2.continuous);
    QVERIFY(near(s2.interval, 1.2, 1e-9));
    QVERIFY(near(findNet(s2, "eth0")->rxBps, 1000.0, 1e-6)); // 1200 B / 1.2 s, not / 0.5 s
    QVERIFY(near(findNet(s2, "eth0")->txBps, 500.0, 1e-6));
    r.write("proc/net/dev", std::string(kNetHeader) + netDevLine("eth0", 5000, 600));
    const auto s3 = smp.sample(52.8, 0.5); // 1.6 s > 1.5 s limit
    QVERIFY(!s3.continuous);
    QVERIFY(!ps::isValid(findNet(s3, "eth0")->rxBps));
}

void TestAdversarial::samplerMemoryOverlapZeroSwapAndLoss()
{
    FixtureRoot r;
    writeBaseTree(r);
    const std::string full = "MemTotal: 8000000 kB\nMemFree: 1000000 kB\nMemAvailable: 5000000 kB\n"
                             "Buffers: 200000 kB\nCached: 3000000 kB\nSwapCached: 0 kB\nShmem: 400000 kB\n"
                             "SReclaimable: 300000 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n";
    r.write("proc/meminfo", full);
    ps::Sampler smp(fixtureOptions(r));
    ps::History h(100, 1e9);
    const auto s1 = smp.sample(1, 1);
    QVERIFY(s1.memory.valid);
    QCOMPARE(s1.memory.total, 8000000.0 * 1024);
    QCOMPARE(s1.memory.used, 3000000.0 * 1024);
    QCOMPARE(s1.memory.available, 5000000.0 * 1024);
    QCOMPARE(s1.memory.cache, 3500000.0 * 1024); // overlaps "available" by design
    QCOMPARE(s1.memory.shared, 400000.0 * 1024);
    QCOMPARE(s1.memory.used + s1.memory.available, s1.memory.total);
    QVERIFY(!s1.memory.hasSwap);
    QCOMPARE(s1.memory.swapUsed, 0.0);
    ps::recordSnapshot(h, s1);
    QVERIFY(!h.find(ps::keys::swapUsed));
    QCOMPARE(h.latestValue(ps::keys::memUsed), 3000000.0 * 1024);

    r.remove("proc/meminfo");
    const auto s2 = smp.sample(2, 1);
    QVERIFY(hasProblem(s2, "/proc/meminfo unreadable"));
    QVERIFY(!s2.memory.valid);
    ps::recordSnapshot(h, s2);
    QVERIFY(h.find(ps::keys::memUsed));
    QVERIFY(std::isnan(h.latestValue(ps::keys::memUsed))); // gap, not 0 and not stale

    r.write("proc/meminfo", "MemTotal: 8000000 kB\nMemFree: 1000000 kB\n"); // pre-3.14 layout
    const auto s3 = smp.sample(3, 1);
    QVERIFY(s3.memory.valid);
    QVERIFY(!ps::isValid(s3.memory.used));
    QCOMPARE(s3.memory.free, 1000000.0 * 1024);

    r.write("proc/meminfo", full);
    const auto s4 = smp.sample(4, 1);
    QCOMPARE(s4.memory.used, 3000000.0 * 1024);
}

void TestAdversarial::samplerDiskUnitsPartitionsAndAggregate()
{
    FixtureRoot r;
    writeBaseTree(r);
    addBlock(r, "sda", 1953525168, true);
    addBlock(r, "dm-0", 2048, false, "cryptroot");
    addBlock(r, "cciss!c0d0", 100, true);
    addBlock(r, "sr0", 0, true);   // no media
    addBlock(r, "loop0", 10, false); // skipped by name
    ps::Sampler smp(fixtureOptions(r));
    r.write("proc/diskstats", diskLine(8, 0, "sda", 1000, 2000, 100) + diskLine(8, 1, "sda1", 1000, 2000, 100)
                                  + diskLine(253, 0, "dm-0", 50, 60, 0) + diskLine(7, 0, "loop0", 1, 1, 1)
                                  + diskLine(104, 0, "cciss/c0d0", 10, 10, 0) + diskLine(11, 0, "sr0", 0, 0, 0));
    smp.sample(1, 1);
    r.write("proc/diskstats", diskLine(8, 0, "sda", 3048, 2001, 350) + diskLine(8, 1, "sda1", 3048, 2001, 350)
                                  + diskLine(253, 0, "dm-0", 58, 60, 0) + diskLine(7, 0, "loop0", 101, 101, 101)
                                  + diskLine(104, 0, "cciss/c0d0", 10, 20, 0) + diskLine(11, 0, "sr0", 0, 0, 0));
    const auto s = smp.sample(2, 1);
    QCOMPARE(s.disks.size(), size_t(3));
    QVERIFY(!findDisk(s, "sda1"));
    QVERIFY(!findDisk(s, "loop0"));
    QVERIFY(!findDisk(s, "sr0"));
    const auto* sda = findDisk(s, "sda");
    QVERIFY(sda);
    QCOMPARE(sda->kind, ps::DeviceKind::Physical);
    QCOMPARE(sda->sizeBytes, 1000204886016.0); // 1953525168 * 512
    QCOMPARE(sda->readBps, 1048576.0);          // 2048 sectors * 512 B / 1 s
    QCOMPARE(sda->writeBps, 512.0);
    QCOMPARE(sda->busyPercent, 25.0);           // 250 ms of 1000 ms
    const auto* dm = findDisk(s, "dm-0");
    QVERIFY(dm);
    QCOMPARE(dm->kind, ps::DeviceKind::Virtual);
    QCOMPARE(dm->label, "cryptroot"s);
    QCOMPARE(dm->readBps, 4096.0);
    QCOMPARE(dm->writeBps, 0.0);
    const auto* cc = findDisk(s, "cciss/c0d0");
    QVERIFY(cc);
    QCOMPARE(cc->writeBps, 5120.0);
    QCOMPARE(s.diskTotal.members, 2); // physical whole disks only
    QCOMPARE(s.diskTotal.a, 1048576.0);
    QCOMPARE(s.diskTotal.b, 5632.0);
}

void TestAdversarial::samplerDiskLargeCountersResetAndIdentity()
{
    constexpr uint64_t B = uint64_t(1) << 55;
    FixtureRoot r;
    writeBaseTree(r);
    addBlock(r, "nvme0n1", 1000, true, {}, "7");
    ps::Sampler smp(fixtureOptions(r));
    auto step = [&](double t, uint64_t rd, uint64_t wr) {
        r.write("proc/diskstats", diskLine(259, 0, "nvme0n1", rd, wr, 0));
        return smp.sample(t, 1);
    };
    step(1, B, B);
    const auto s2 = step(2, B + 8, B);
    QCOMPARE(findDisk(s2, "nvme0n1")->readBps, 4096.0);
    QCOMPARE(findDisk(s2, "nvme0n1")->writeBps, 0.0);
    QCOMPARE(findDisk(s2, "nvme0n1")->busyPercent, 0.0);
    const auto s3 = step(3, 100, 100); // counters reset under the same identity
    QVERIFY(!ps::isValid(findDisk(s3, "nvme0n1")->readBps));
    QVERIFY(!ps::isValid(findDisk(s3, "nvme0n1")->writeBps));
    QVERIFY(!ps::isValid(s3.diskTotal.a));
    const auto s4 = step(4, 108, 100);
    QCOMPARE(findDisk(s4, "nvme0n1")->readBps, 4096.0);
    r.write("sys/block/nvme0n1/diskseq", "8\n"); // media/device replaced
    const auto s5 = step(5, 200000, 100);
    QVERIFY(!ps::isValid(findDisk(s5, "nvme0n1")->readBps));
    const auto s6 = step(6, 200016, 100);
    QCOMPARE(findDisk(s6, "nvme0n1")->readBps, 8192.0);
}

void TestAdversarial::samplerDiskChurnAndBusyClamp()
{
    FixtureRoot r;
    writeBaseTree(r);
    addBlock(r, "sda", 1000, true);
    ps::Sampler smp(fixtureOptions(r));
    r.write("proc/diskstats", diskLine(8, 0, "sda", 0, 0, 0));
    smp.sample(1, 1);
    r.write("proc/diskstats", diskLine(8, 0, "sda", 8, 0, 1050));
    const auto s2 = smp.sample(2, 1);
    QCOMPARE(findDisk(s2, "sda")->busyPercent, 100.0); // 105 % jiffy overshoot clamped
    r.write("proc/diskstats", "");
    const auto s3 = smp.sample(3, 1);
    QVERIFY(s3.disks.empty());
    QCOMPARE(s3.diskTotal.members, 0);
    QVERIFY(!ps::isValid(s3.diskTotal.a));
    r.write("proc/diskstats", diskLine(8, 0, "sda", 5000, 5000, 5000));
    const auto s4 = smp.sample(4, 1);
    QVERIFY(!ps::isValid(findDisk(s4, "sda")->readBps)); // returned device: no stale baseline
    QVERIFY(!ps::isValid(s4.diskTotal.a));
    r.write("proc/diskstats", diskLine(8, 0, "sda", 5008, 5000, 6200));
    const auto s5 = smp.sample(5, 1);
    QCOMPARE(findDisk(s5, "sda")->readBps, 4096.0);
    QVERIFY(!ps::isValid(findDisk(s5, "sda")->busyPercent)); // 120 % is impossible
}

void TestAdversarial::samplerNetKindsAggregateAndIdentity()
{
    constexpr uint64_t B = uint64_t(1) << 62;
    FixtureRoot r;
    writeBaseTree(r);
    addNetIf(r, "eth0", 2, 1, "up", true);
    addNetIf(r, "wlan0", 3, 1, "dormant", true);
    addNetIf(r, "lo", 1, 772, "unknown", false);
    addNetIf(r, "veth0", 5, 1, "up", false);
    addNetIf(r, "lb0", 6, 772, "unknown", false);
    ps::Sampler smp(fixtureOptions(r));
    auto write = [&](uint64_t eth, uint64_t wlan, uint64_t other) {
        r.write("proc/net/dev", std::string(kNetHeader) + netDevLine("lo", other * 100, other * 100)
                                    + netDevLine("eth0", eth, eth / 4) + netDevLine("wlan0", wlan, 7)
                                    + netDevLine("veth0", other * 10, other) + netDevLine("lb0", other, other));
    };
    write(0, 0, 0);
    smp.sample(10, 1);
    write(2000, 4000, 10000);
    const auto s2 = smp.sample(12, 1); // 2 s interval
    QCOMPARE(findNet(s2, "eth0")->kind, ps::DeviceKind::Physical);
    QCOMPARE(findNet(s2, "lo")->kind, ps::DeviceKind::Loopback);
    QCOMPARE(findNet(s2, "lb0")->kind, ps::DeviceKind::Loopback);
    QCOMPARE(findNet(s2, "veth0")->kind, ps::DeviceKind::Virtual);
    QCOMPARE(findNet(s2, "wlan0")->operState, "dormant"s);
    QCOMPARE(findNet(s2, "eth0")->rxBps, 1000.0);
    QCOMPARE(findNet(s2, "eth0")->txBps, 250.0);
    QCOMPARE(findNet(s2, "wlan0")->rxBps, 2000.0);
    QCOMPARE(findNet(s2, "wlan0")->txBps, 0.0);
    QCOMPARE(s2.netTotal.label, "all physical interfaces"s);
    QCOMPARE(s2.netTotal.members, 2);
    QCOMPARE(s2.netTotal.a, 3000.0);
    QCOMPARE(s2.netTotal.b, 250.0);
    // eth0 re-created (new ifindex) with larger counters; wlan0 jumps by 2^62.
    r.write("sys/class/net/eth0/ifindex", "9\n");
    write(5000, B, 20000);
    const auto s3 = smp.sample(13, 1);
    QVERIFY(!ps::isValid(findNet(s3, "eth0")->rxBps));
    QVERIFY(!ps::isValid(findNet(s3, "wlan0")->rxBps)); // > 1 TiB/s glitch
    QVERIFY(!ps::isValid(s3.netTotal.a));
    write(6000, B + 1, 30000);
    const auto s4 = smp.sample(14, 1);
    QCOMPARE(findNet(s4, "eth0")->rxBps, 1000.0);
    QCOMPARE(findNet(s4, "wlan0")->rxBps, 1.0); // exact above 2^53
}

void TestAdversarial::samplerNetContainerFallbackAndLoss()
{
    FixtureRoot r;
    writeBaseTree(r);
    addNetIf(r, "lo", 1, 772, "unknown", false);
    addNetIf(r, "eth0", 7, 1, "up", false); // veth end inside a container: no device link
    ps::Sampler smp(fixtureOptions(r));
    r.write("proc/net/dev", std::string(kNetHeader) + netDevLine("lo", 0, 0) + netDevLine("eth0", 0, 0));
    smp.sample(1, 1);
    r.write("proc/net/dev", std::string(kNetHeader) + netDevLine("lo", 99999, 99999) + netDevLine("eth0", 300, 100));
    const auto s2 = smp.sample(2, 1);
    QCOMPARE(s2.netTotal.label, "all non-loopback interfaces"s);
    QCOMPARE(s2.netTotal.members, 1);
    QCOMPARE(s2.netTotal.a, 300.0);
    QCOMPARE(s2.netTotal.b, 100.0);
    r.remove("proc/net/dev");
    const auto s3 = smp.sample(3, 1);
    QVERIFY(hasProblem(s3, "/proc/net/dev unreadable"));
    QVERIFY(s3.interfaces.empty());
    QCOMPARE(s3.netTotal.members, 0);
    r.write("proc/net/dev", std::string(kNetHeader) + netDevLine("lo", 0, 0) + netDevLine("eth0", 900, 100));
    const auto s4 = smp.sample(4, 1);
    QVERIFY(!ps::isValid(findNet(s4, "eth0")->rxBps)); // no stale two-interval delta
    r.write("proc/net/dev", std::string(kNetHeader) + netDevLine("lo", 0, 0) + netDevLine("eth0", 1000, 100));
    const auto s5 = smp.sample(5, 1);
    QCOMPARE(findNet(s5, "eth0")->rxBps, 100.0);
}

void TestAdversarial::samplerNetSanitizedNamesStayDistinct()
{
    // Two legal (root-created) interface names that differ only in control
    // bytes sanitize to the same display name; history keys and selection are
    // derived from that name, so two devices would share one series.
    FixtureRoot r;
    writeBaseTree(r);
    addNetIf(r, "e\x01x", 10, 1, "up", true);
    addNetIf(r, "e\x02x", 11, 1, "up", true);
    ps::Sampler smp(fixtureOptions(r));
    r.write("proc/net/dev", std::string(kNetHeader) + netDevLine("e\x01x", 0, 0) + netDevLine("e\x02x", 0, 0));
    smp.sample(1, 1);
    r.write("proc/net/dev", std::string(kNetHeader) + netDevLine("e\x01x", 100, 0) + netDevLine("e\x02x", 900, 0));
    const auto s = smp.sample(2, 1);
    QCOMPARE(s.interfaces.size(), size_t(2));
    ps::History h(10, 1e9);
    ps::recordSnapshot(h, s);
    int perInterface = 0;
    for (const auto* series : h.allSeries())
        perInterface += series->key.rfind("net.if.", 0) == 0 ? 1 : 0;
    QVERIFY2(s.interfaces[0].name != s.interfaces[1].name && perInterface == 4,
             qPrintable(QStringLiteral("names '%1' / '%2', per-interface history series = %3 (expected 4)")
                            .arg(QString::fromStdString(s.interfaces[0].name),
                                 QString::fromStdString(s.interfaces[1].name))
                            .arg(perInterface)));
}

void TestAdversarial::samplerProcessCpuAgeRssIo()
{
    constexpr uint64_t B = uint64_t(1) << 60;
    FixtureRoot r;
    writeBaseTree(r);
    r.mkdir("proc/sys/kernel"); // non-numeric entries are ignored
    writeProc(r, 100, pidStat(100, "app", 1000, 0, 50000, 4194560, 4), "/usr/bin/app\0--flag\0"s, ioText(4096, 0));
    writeProc(r, 101, pidStat(101, "future", 0, 0, 200000), "future"s, ioText(0, 0), 0);
    ps::Sampler smp(fixtureOptions(r, true));
    const auto s1 = smp.sample(1, 1);
    QVERIFY(s1.processesScanned);
    QCOMPARE(s1.processes.size(), size_t(2));
    const auto* p = findProc(s1, 100);
    QVERIFY(p);
    QVERIFY(!ps::isValid(p->cpuPercent));
    QCOMPARE(p->startTicks, uint64_t(50000));
    QCOMPARE(p->ageSeconds, 500.0);      // uptime 1000 - 50000/100
    QCOMPARE(p->rssBytes, 456.0 * 4096); // pages * page size
    QCOMPARE(p->threads, 4);
    QCOMPARE(p->user, "u1000"s);
    QVERIFY(p->uidKnown);
    QCOMPARE(p->command, "/usr/bin/app --flag"s);
    QVERIFY(!p->kernelThread);
    QCOMPARE(s1.threadCount, long(5));
    QVERIFY(!ps::isValid(findProc(s1, 101)->ageSeconds)); // started 1000 s "in the future"
    QCOMPARE(findProc(s1, 101)->user, "u0"s);

    r.write("proc/uptime", "1001.00 0.00\n");
    writeProc(r, 100, pidStat(100, "app", 1030, 20, 50000, 4194560, 4), "/usr/bin/app\0--flag\0"s, ioText(5096, 2000));
    const auto s2 = smp.sample(2, 1);
    p = findProc(s2, 100);
    QCOMPARE(p->cpuPercent, 50.0); // 50 ticks / 100 Hz / 1 s
    QCOMPARE(p->ioReadBps, 1000.0);
    QCOMPARE(p->ioWriteBps, 2000.0);
    QCOMPARE(p->ageSeconds, 501.0);

    writeProc(r, 100, pidStat(100, "app", B, 0, 50000, 4194560, 4), "/usr/bin/app\0--flag\0"s, ioText(5096, 2000));
    const auto s3 = smp.sample(3, 1);
    QVERIFY(!ps::isValid(findProc(s3, 100)->cpuPercent)); // impossible jump
    writeProc(r, 100, pidStat(100, "app", B + 100, 0, 50000, 4194560, 4), "/usr/bin/app\0--flag\0"s, ioText(5096, 2000));
    const auto s4 = smp.sample(4, 1);
    QCOMPARE(findProc(s4, 100)->cpuPercent, 100.0); // exact above 2^53; one core = 100 %
}

void TestAdversarial::samplerProcessPidReuseWithHigherTicks()
{
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r, true));
    writeProc(r, 200, pidStat(200, "old", 1000, 0, 5000), "old"s, ioText(1000, 1000));
    smp.sample(1, 1);
    writeProc(r, 200, pidStat(200, "old", 1010, 0, 5000), "old"s, ioText(1000, 1000));
    QCOMPARE(findProc(smp.sample(2, 1), 200)->cpuPercent, 10.0);
    // PID reused by a new process whose counters are all *higher* than the old
    // one's, so a naive delta would look plausible (40 %, 1000 B/s).
    writeProc(r, 200, pidStat(200, "new", 1050, 0, 6000), "new\0--x\0"s, ioText(2000, 2000));
    const auto s3 = smp.sample(3, 1);
    const auto* p = findProc(s3, 200);
    QVERIFY(!ps::isValid(p->cpuPercent));
    QVERIFY(!ps::isValid(p->ioReadBps));
    QVERIFY(!ps::isValid(p->ioWriteBps));
    QCOMPARE(p->startTicks, uint64_t(6000));
    QCOMPARE(p->command, "new --x"s);
    writeProc(r, 200, pidStat(200, "new", 1080, 0, 6000), "new\0--x\0"s, ioText(2500, 2000));
    const auto s4 = smp.sample(4, 1);
    QCOMPARE(findProc(s4, 200)->cpuPercent, 30.0);
    QCOMPARE(findProc(s4, 200)->ioReadBps, 500.0);
}

void TestAdversarial::samplerProcessExecKeepsCpuHistory()
{
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r, true));
    writeProc(r, 300, pidStat(300, "sh", 100, 0, 7000), "sh\0-c\0x\0"s, ioText(0, 0));
    smp.sample(1, 1);
    writeProc(r, 300, pidStat(300, "python3", 150, 0, 7000), "python3\0app.py\0"s, ioText(0, 0));
    const auto s2 = smp.sample(2, 1);
    const auto* p = findProc(s2, 300);
    QCOMPARE(p->name, "python3"s);
    QCOMPARE(p->command, "python3 app.py"s); // refreshed after exec
    QCOMPARE(p->cpuPercent, 50.0);           // same process identity: CPU continues
}

void TestAdversarial::samplerProcessKernelThreadAndVanish()
{
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r, true));
    writeProc(r, 2, pidStat(2, "kthreadd", 0, 3, 1, 0x00208040u, 1, 0), ""s, {}, 0);
    writeProc(r, 400, pidStat(400, "worker", 100, 0, 9000), "worker"s, ioText(0, 0));
    const auto s1 = smp.sample(1, 1);
    const auto* k = findProc(s1, 2);
    QVERIFY(k);
    QVERIFY(k->kernelThread);
    QCOMPARE(k->command, "[kthreadd]"s);
    QVERIFY(!k->ioDenied);
    QVERIFY(!ps::isValid(k->ioReadBps));
    r.remove("proc/400");
    const auto s2 = smp.sample(2, 1);
    QVERIFY(!findProc(s2, 400));
    writeProc(r, 400, pidStat(400, "worker", 160, 0, 9000), "worker"s, ioText(0, 0));
    const auto s3 = smp.sample(3, 1);
    QVERIFY(!ps::isValid(findProc(s3, 400)->cpuPercent)); // no baseline from two samples ago
}

void TestAdversarial::samplerProcessIoDenied()
{
    if (rootUser())
        QSKIP("permission fixtures need a non-root user");
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r, true));
    writeProc(r, 500, pidStat(500, "secret", 100, 0, 9000), "secret"s, ioText(10, 10));
    fs::permissions(r.path("proc/500/io"), fs::perms::none);
    smp.sample(1, 1);
    writeProc(r, 500, pidStat(500, "secret", 150, 0, 9000), "secret"s);
    const auto s2 = smp.sample(2, 1);
    const auto* p = findProc(s2, 500);
    QVERIFY(p->ioDenied);
    QVERIFY(!ps::isValid(p->ioReadBps));
    QCOMPARE(p->cpuPercent, 50.0);
}

void TestAdversarial::samplerProcessIoDeniedRecovers()
{
    // EACCES on /proc/<pid>/io can be transient (e.g. while the target is
    // being set up or its credentials change). Access must be retried within a
    // bounded number of samples and restart from a fresh baseline.
    if (rootUser())
        QSKIP("permission fixtures need a non-root user");
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r, true));
    writeProc(r, 510, pidStat(510, "svc", 0, 0, 9000), "svc"s, ioText(0, 0));
    const std::string io = r.path("proc/510/io");
    fs::permissions(io, fs::perms::none);
    smp.sample(1, 1);
    QVERIFY(findProc(smp.sample(2, 1), 510)->ioDenied);
    fs::permissions(io, fs::perms::owner_read | fs::perms::owner_write);
    int recoveredAt = -1;
    bool rebasedFirst = false;
    double rateAfter = ps::kNaN;
    for (int i = 1; i <= 60; ++i) {
        r.write("proc/510/io", ioText(1000ull * i, 0));
        const auto s = smp.sample(2 + i, 1);
        const auto* p = findProc(s, 510);
        if (recoveredAt < 0 && !p->ioDenied) {
            recoveredAt = i;
            rebasedFirst = !ps::isValid(p->ioReadBps);
        } else if (recoveredAt > 0) {
            rateAfter = p->ioReadBps;
            break;
        }
    }
    QVERIFY2(recoveredAt > 0, "ioDenied stayed set for 60 samples after /proc/<pid>/io became readable");
    QVERIFY(rebasedFirst);
    QCOMPARE(rateAfter, 1000.0);
}

void TestAdversarial::samplerTemperaturesSignedZeroFault()
{
    FixtureRoot r;
    writeBaseTree(r);
    const std::string h = "sys/class/hwmon/hwmon0/";
    r.symlinkDir("sys/devices/platform/coretemp.0", h + "device");
    r.write(h + "name", "coretemp\n");
    r.write(h + "temp1_input", "45000\n");
    r.write(h + "temp1_label", "Package id 0\n");
    r.write(h + "temp2_input", "38000\n");
    r.write(h + "temp2_fault", "1\n");
    r.write(h + "temp3_input", "-5000\n");
    r.write(h + "temp4_input", "0\n");
    r.write(h + "temp5_input", "-273200\n"); // driver error marker
    r.write(h + "temp10_input", "50000\n");
    r.write(h + "temp10_label", "Core 9\n");
    r.write(h + "tempX_input", "12345\n");   // not a numbered channel
    ps::Sampler smp(fixtureOptions(r));
    const auto s1 = smp.sample(1, 1);
    QCOMPARE(s1.temperatures.size(), size_t(4));
    QCOMPARE(s1.temperatures[0].label, "Package id 0"s);
    QCOMPARE(s1.temperatures[0].celsius, 45.0);
    QCOMPARE(s1.temperatures[0].displayName, "coretemp \u00B7 Package id 0"s);
    QVERIFY(s1.temperatures[0].key.rfind("hwmon:", 0) == 0);
    QVERIFY(s1.temperatures[0].key.find(":coretemp:temp1") != std::string::npos);
    QCOMPARE(s1.temperatures[1].label, "temp3"s);
    QCOMPARE(s1.temperatures[1].celsius, -5.0);
    QCOMPARE(s1.temperatures[2].label, "temp4"s);
    QCOMPARE(s1.temperatures[2].celsius, 0.0); // a real zero reading
    QCOMPARE(s1.temperatures[3].label, "Core 9"s); // natural order: temp10 after temp4
    QVERIFY(s1.temperatureNote.empty());

    r.write(h + "temp3_input", "garbage\n");
    r.write(h + "temp2_fault", "0\n");
    const auto s2 = smp.sample(2, 1);
    QCOMPARE(s2.temperatures.size(), size_t(5));
    const auto* t3 = findTemp(s2, "temp3");
    QVERIFY(t3);
    QVERIFY(!ps::isValid(t3->celsius)); // stays listed as a gap, not dropped or zeroed
    const auto* t2 = findTemp(s2, "temp2");
    QVERIFY(t2);
    QCOMPARE(t2->celsius, 38.0);
}

void TestAdversarial::samplerTemperatureMalformedFaultIsUnavailable()
{
    // An absent *_fault file is fine, but one that exists and cannot be read
    // or parsed means health is unknown; the reading must not be trusted.
    FixtureRoot r;
    writeBaseTree(r);
    const std::string h = "sys/class/hwmon/hwmon0/";
    r.write(h + "name", "drivetemp\n");
    r.write(h + "temp1_input", "45000\n");
    r.write(h + "temp1_fault", "garbage\n");
    r.write(h + "temp2_input", "46000\n");
    r.write(h + "temp2_fault", "1\n");
    ps::Sampler smp(fixtureOptions(r));
    const auto s = smp.sample(1, 1);
    const auto* t1 = findTemp(s, "temp1");
    QVERIFY2(!t1 || !ps::isValid(t1->celsius),
             qPrintable(QStringLiteral("malformed temp1_fault treated as healthy: %1 C").arg(t1 ? t1->celsius : 0)));
    QVERIFY(!findTemp(s, "temp2"));
}

void TestAdversarial::samplerTemperatureUnreadableFaultIsUnavailable()
{
    if (rootUser())
        QSKIP("permission fixtures need a non-root user");
    FixtureRoot r;
    writeBaseTree(r);
    const std::string h = "sys/class/hwmon/hwmon0/";
    r.write(h + "name", "drivetemp\n");
    r.write(h + "temp1_input", "45000\n");
    r.write(h + "temp1_fault", "0\n");
    fs::permissions(r.path(h + "temp1_fault"), fs::perms::none);
    ps::Sampler smp(fixtureOptions(r));
    const auto s = smp.sample(1, 1);
    const auto* t1 = findTemp(s, "temp1");
    QVERIFY2(!t1 || !ps::isValid(t1->celsius),
             qPrintable(QStringLiteral("unreadable temp1_fault treated as healthy: %1 C").arg(t1 ? t1->celsius : 0)));
}

void TestAdversarial::samplerTemperatureIdentitySurvivesRenumbering()
{
    FixtureRoot r;
    writeBaseTree(r);
    r.symlinkDir("sys/devices/pci0000:00/0000:00:18.3", "sys/class/hwmon/hwmon0/device");
    r.write("sys/class/hwmon/hwmon0/name", "k10temp\n");
    r.write("sys/class/hwmon/hwmon0/temp1_input", "50000\n");
    r.write("sys/class/hwmon/hwmon0/temp1_label", "Tctl\n");
    ps::Sampler smp(fixtureOptions(r));
    const auto s1 = smp.sample(1, 1);
    const auto keys = temperatureKeys(s1);
    QCOMPARE(keys.size(), size_t(1));
    // Driver reload: hwmon0 disappears and the same device registers as hwmon5.
    r.remove("sys/class/hwmon/hwmon0");
    r.write("sys/class/hwmon/hwmon5/name", "k10temp\n");
    r.write("sys/class/hwmon/hwmon5/temp1_input", "50000\n");
    r.write("sys/class/hwmon/hwmon5/temp1_label", "Tctl\n");
    fs::create_directory_symlink(r.path("sys/devices/pci0000:00/0000:00:18.3"), r.path("sys/class/hwmon/hwmon5/device"));
    const auto s2 = smp.sample(2, 1);
    const auto* gap = findTemp(s2, "Tctl");
    QVERIFY(gap);
    QVERIFY(!ps::isValid(gap->celsius)); // vanished path: gap, not the stale 50 C
    const auto s3 = smp.sample(3, 1);
    QCOMPARE(temperatureKeys(s3), keys); // selection/history identity preserved
    QCOMPARE(findTemp(s3, "Tctl")->celsius, 50.0);
}

void TestAdversarial::samplerTemperatureCachedPathNotSplicedToNewDevice()
{
    // hwmon0 is removed and a *different* device registers as hwmon0 between
    // two samples (well inside the 10 s rediscovery period). Its reading must
    // not be reported under the previous device's key.
    FixtureRoot r;
    writeBaseTree(r);
    r.symlinkDir("sys/devices/pci0000:00/0000:00:18.3", "sys/class/hwmon/hwmon0/device");
    r.write("sys/class/hwmon/hwmon0/name", "k10temp\n");
    r.write("sys/class/hwmon/hwmon0/temp1_input", "50000\n");
    r.write("sys/class/hwmon/hwmon0/temp1_label", "Tctl\n");
    ps::Sampler smp(fixtureOptions(r));
    const auto s1 = smp.sample(1, 1);
    QCOMPARE(s1.temperatures.size(), size_t(1));
    const std::string oldKey = s1.temperatures[0].key;
    r.remove("sys/class/hwmon/hwmon0");
    r.symlinkDir("sys/devices/pci0000:00/0000:01:00.0/nvme/nvme0", "sys/class/hwmon/hwmon0/device");
    r.write("sys/class/hwmon/hwmon0/name", "nvme\n");
    r.write("sys/class/hwmon/hwmon0/temp1_input", "70000\n");
    r.write("sys/class/hwmon/hwmon0/temp1_label", "Composite\n");
    const auto s2 = smp.sample(2, 1);
    for (const auto& t : s2.temperatures) {
        if (t.key == oldKey)
            QVERIFY2(!ps::isValid(t.celsius),
                     qPrintable(QStringLiteral("new device's %1 C reported under old key %2")
                                    .arg(t.celsius).arg(QString::fromStdString(oldKey))));
    }
}

void TestAdversarial::samplerTemperatureDuplicateChipsDisambiguated()
{
    FixtureRoot r;
    writeBaseTree(r);
    r.symlinkDir("sys/devices/pci0000:00/nvme/nvme0", "sys/class/hwmon/hwmon1/device");
    r.symlinkDir("sys/devices/pci0000:00/nvme/nvme1", "sys/class/hwmon/hwmon2/device");
    for (const char* hw : {"hwmon1", "hwmon2"}) {
        const std::string b = std::string("sys/class/hwmon/") + hw + "/";
        r.write(b + "name", "nvme\n");
        r.write(b + "temp1_label", "Composite\n");
    }
    r.write("sys/class/hwmon/hwmon1/temp1_input", "40000\n");
    r.write("sys/class/hwmon/hwmon2/temp1_input", "41000\n");
    ps::Sampler smp(fixtureOptions(r));
    const auto s = smp.sample(1, 1);
    QCOMPARE(s.temperatures.size(), size_t(2));
    QVERIFY(s.temperatures[0].key != s.temperatures[1].key);
    QCOMPARE(s.temperatures[0].displayName, "nvme \u00B7 Composite (nvme0)"s);
    QCOMPARE(s.temperatures[1].displayName, "nvme \u00B7 Composite (nvme1)"s);
    QCOMPARE(s.temperatures[0].celsius, 40.0);
    QCOMPARE(s.temperatures[1].celsius, 41.0);
}

void TestAdversarial::samplerThermalZoneFallbackAndNotes()
{
    {
        FixtureRoot r; // container: neither class directory exists
        writeBaseTree(r);
        ps::Sampler smp(fixtureOptions(r));
        const auto s = smp.sample(1, 1);
        QVERIFY(s.temperatures.empty());
        QVERIFY(s.temperatureNote.find("does not expose") != std::string::npos);
    }
    {
        FixtureRoot r;
        writeBaseTree(r);
        r.write("sys/class/thermal/thermal_zone0/type", "x86_pkg_temp\n");
        r.write("sys/class/thermal/thermal_zone0/temp", "55000\n");
        r.write("sys/class/thermal/thermal_zone1/type", "acpitz\n");
        r.write("sys/class/thermal/thermal_zone1/temp", "-273200\n");
        r.mkdir("sys/class/thermal/cooling_device0");
        ps::Sampler smp(fixtureOptions(r));
        const auto s = smp.sample(1, 1);
        QCOMPARE(s.temperatures.size(), size_t(1));
        QCOMPARE(s.temperatures[0].chip, "thermal zone"s);
        QCOMPARE(s.temperatures[0].label, "x86_pkg_temp"s);
        QCOMPARE(s.temperatures[0].celsius, 55.0);
    }
    {
        FixtureRoot r;
        writeBaseTree(r);
        r.write("sys/class/hwmon/hwmon0/name", "acpitz\n");
        r.write("sys/class/hwmon/hwmon0/temp1_input", "-273200\n");
        r.write("sys/class/thermal/thermal_zone0/type", "acpitz\n");
        r.write("sys/class/thermal/thermal_zone0/temp", "40000\n");
        r.mkdir("sys/class/thermal/thermal_zone0/hwmon0"); // already represented by hwmon
        ps::Sampler smp(fixtureOptions(r));
        const auto s = smp.sample(1, 1);
        QVERIFY(s.temperatures.empty());
        QVERIFY(s.temperatureNote.rfind("1 temperature inputs", 0) == 0);
    }
}

namespace {

// Thread-safe statFs double: the sampler calls it on its capacity-probe thread.
struct StatFsRecorder {
    std::mutex m;
    std::vector<std::string> calls;
    std::map<std::string, ps::FsStats> answers;
    std::map<std::string, int> errors;
    std::vector<std::string> snapshotCalls()
    {
        std::lock_guard lock(m);
        return calls;
    }
};

ps::SamplerOptions recordingOptions(const FixtureRoot& r, const std::shared_ptr<StatFsRecorder>& rec)
{
    auto o = fixtureOptions(r);
    o.fsProbeWaitMs = 3000; // make the asynchronous probe deterministic for these tests
    o.statFs = [rec](const std::string& path, int& err) -> std::optional<ps::FsStats> {
        std::lock_guard lock(rec->m);
        rec->calls.push_back(path);
        if (auto e = rec->errors.find(path); e != rec->errors.end()) {
            err = e->second;
            return std::nullopt;
        }
        if (auto a = rec->answers.find(path); a != rec->answers.end())
            return a->second;
        return ps::FsStats{1000, 500, 500, 4096};
    };
    return o;
}

const ps::Filesystem* findFs(const ps::Snapshot& s, std::string_view source)
{
    for (const auto& f : s.filesystems)
        if (f.source == source)
            return &f;
    return nullptr;
}

// One /proc/self/mountinfo line without optional fields.
std::string mi(int id, int parent, std::string_view dev, std::string_view root, std::string_view mp,
               std::string_view opts, std::string_view type, std::string_view source)
{
    return std::to_string(id) + " " + std::to_string(parent) + " " + std::string(dev) + " " + std::string(root) + " "
        + std::string(mp) + " " + std::string(opts) + " - " + std::string(type) + " " + std::string(source) + " rw\n";
}

} // namespace

void TestAdversarial::mountInfoParsing()
{
    const auto m = parse::parseMountInfo(
        "36 35 98:0 /root /mnt rw,noatime shared:1 master:2 - ext4 /dev/sda1 rw,errors=continue\n"
        "37 1 8:17 / /media/a\\040b\\134c ro,nosuid - vfat /dev/sdb\\0401 ro\n"
        "38 1 8:18 / /x rw - ext4 /dev/sdb2 rw\n"          // minimal: no optional fields
        "39 1 8:19 / /nosep rw shared:3 ext4 /dev/sdb3 rw\n" // missing "-" separator
        "40 1 0:5 / /nosrc rw - tmpfs\n"                  // truncated before the source
        "-1 1 8:20 / /negid rw - ext4 /dev/sdb4 rw\n"
        "41 1 8-21 / /baddev rw - ext4 /dev/sdb5 rw\n"
        "42 1 8: / /nominor rw - ext4 /dev/sdb6 rw\n"
        "2147483648 1 8:22 / /bigid rw - ext4 /dev/sdb7 rw\n"
        "43 1 8:23x / /junk rw - ext4 /dev/sdb8 rw\n"
        "44 1 4294967296:0 / /bigmaj rw - ext4 /dev/sdb9 rw\n"
        "45 1 8:24 / relative rw - ext4 /dev/sdc1 rw\n"
        "46 1 8:25 / /cut rw - ext4");
    QCOMPARE(m.size(), size_t(3));
    QCOMPARE(m[0].id, 36);
    QCOMPARE(m[0].parentId, 35);
    QCOMPARE(m[0].major, 98u);
    QCOMPARE(m[0].minor, 0u);
    QVERIFY(m[0].hasDevice);
    QCOMPARE(m[0].mountPoint, "/mnt"s);
    QCOMPARE(m[0].fsType, "ext4"s);
    QCOMPARE(m[0].source, "/dev/sda1"s);
    QVERIFY(!m[0].readOnly());
    QCOMPARE(m[1].mountPoint, "/media/a b\\c"s);
    QCOMPARE(m[1].source, "/dev/sdb 1"s);
    QVERIFY(m[1].readOnly()); // per-mount options, not the superblock's
    QCOMPARE(m[2].mountPoint, "/x"s);
    QCOMPARE(m[2].minor, 18u);
}

void TestAdversarial::mountResolution()
{
    // 1: / ext4; 2: /data ext4 covered by 3: nfs on /data; 4: /mnt/disk ext4
    // hidden by 5: fuse on /mnt; 6: ext4 inside the FUSE tree; 7: autofs on
    // /auto with 8: ext4 below it; 9: /srv stacked twice (xfs over ext4).
    std::vector<parse::MountEntry> t = parse::parseMountInfo(
        mi(1, 0, "8:2", "/", "/", "rw", "ext4", "/dev/sda2") + mi(2, 1, "8:1", "/", "/data", "rw", "ext4", "/dev/sda1")
        + mi(3, 2, "0:50", "/", "/data", "rw", "nfs4", "srv:/e") + mi(4, 1, "8:17", "/", "/mnt/disk", "rw", "ext4", "/dev/sdb1")
        + mi(5, 1, "0:51", "/", "/mnt", "rw", "fuse.sshfs", "u@h:/") + mi(6, 5, "8:33", "/", "/mnt/in", "rw", "ext4", "/dev/sdc1")
        + mi(7, 1, "0:52", "/", "/auto", "rw", "autofs", "systemd-1") + mi(8, 7, "8:49", "/", "/auto/d", "rw", "ext4", "/dev/sdd1")
        + mi(9, 1, "8:65", "/", "/srv", "rw", "ext4", "/dev/sde1") + mi(10, 9, "8:81", "/", "/srv", "rw", "xfs", "/dev/sdf1"));
    QCOMPARE(t.size(), size_t(10));
    auto res = [&](std::string_view p) { return parse::resolveMountPath(t, p); };
    QCOMPARE(res("/").index, 0);
    QVERIFY(res("/").safe);
    QCOMPARE(res("/etc/passwd").index, 0);
    QCOMPARE(res("/data").index, 2);           // the NFS mount on top
    QCOMPARE(res("/mnt/disk").index, 4);       // hidden under FUSE
    QVERIFY(!res("/mnt/disk").safe);
    QCOMPARE(res("/mnt/in").index, 5);
    QVERIFY(!res("/mnt/in").safe);             // lookup walks through FUSE
    QCOMPARE(res("/auto/d").index, 7);
    QVERIFY(!res("/auto/d").safe);             // crosses an automount trigger
    QCOMPARE(res("/srv").index, 9);
    QVERIFY(res("/srv").safe);
    QCOMPARE(res("/srvx").index, 0);           // prefix is component-wise, not textual
    QCOMPARE(res("/srv/").index, 9);
    QCOMPARE(res("relative").index, -1);
    QCOMPARE(parse::resolveMountPath({}, "/").index, -1);
    // Parent cycle with no root mount must terminate.
    std::vector<parse::MountEntry> cyc = parse::parseMountInfo(mi(1, 2, "8:1", "/", "/a", "rw", "ext4", "/dev/sda1")
                                                               + mi(2, 1, "8:2", "/", "/a", "rw", "ext4", "/dev/sda2"));
    QCOMPARE(parse::resolveMountPath(cyc, "/a").index, -1);
}

void TestAdversarial::samplerFilesystemsGroupingSafetyAndErrors()
{
    FixtureRoot r;
    writeBaseTree(r);
    r.write("proc/self/mountinfo",
            mi(1, 0, "8:2", "/", "/", "rw,relatime", "ext4", "/dev/sda2")
                + mi(2, 1, "8:2", "/home", "/home", "rw", "ext4", "/dev/sda2")
                + mi(3, 1, "8:17", "/", "/media/My\\040Disk", "ro,nosuid", "vfat", "/dev/sdb1")
                + mi(4, 1, "0:50", "/", "/net", "rw", "nfs4", "server:/x")
                + mi(5, 1, "0:51", "/", "/mnt/s", "rw", "fuse.sshfs", "u@h:/")
                + mi(6, 1, "0:30", "/", "/tmp", "rw", "tmpfs", "tmpfs")
                + mi(7, 1, "8:33", "/", "/bad", "rw", "ext4", "/dev/sdc1")
                + mi(8, 1, "8:49", "/", "/denied", "rw", "xfs", "/dev/sdd1")
                + mi(9, 1, "8:65", "/", "/huge", "rw", "ext4", "/dev/sde1")
                + mi(10, 1, "8:81", "/", "/zero", "rw", "ext4", "/dev/sdf1"));
    auto rec = std::make_shared<StatFsRecorder>();
    rec->answers["/"] = {1000, 400, 300, 4096};
    rec->answers["/media/My Disk"] = {100, 100, 100, 512};
    rec->answers["/bad"] = {10, 11, 0, 4096};
    rec->answers["/huge"] = {kU64Max / 2, 0, 0, 4096};
    rec->answers["/zero"] = {0, 0, 0, 4096};
    rec->errors["/denied"] = EACCES;
    ps::Sampler smp(recordingOptions(r, rec));
    const auto s = smp.sample(1, 1);
    QCOMPARE(s.filesystems.size(), size_t(6));
    const auto calls = rec->snapshotCalls();
    const std::set<std::string> called(calls.begin(), calls.end());
    QCOMPARE(called, (std::set<std::string>{"/", "/media/My Disk", "/bad", "/denied", "/huge", "/zero"}));
    QCOMPARE(calls.size(), size_t(6)); // /home shares /dev/sda2; remote/FUSE/tmpfs never probed
    const auto* root = findFs(s, "/dev/sda2");
    QVERIFY(root);
    QCOMPARE(root->mountPoints, (std::vector<std::string>{"/", "/home"}));
    QCOMPARE(root->totalBytes, 4096000.0);  // 1000 * 4096
    QCOMPARE(root->usedBytes, 2457600.0);   // (1000 - 400) * 4096
    QCOMPARE(root->availBytes, 1228800.0);  // 300 * 4096 (unprivileged)
    QVERIFY(root->error.empty());
    QVERIFY(!root->readOnly);
    const auto* media = findFs(s, "/dev/sdb1");
    QVERIFY(media->readOnly);
    QCOMPARE(media->mountPoints, (std::vector<std::string>{"/media/My Disk"}));
    QCOMPARE(media->totalBytes, 51200.0);
    QCOMPARE(findFs(s, "/dev/sdc1")->error, "inconsistent statvfs values"s);
    QVERIFY(!ps::isValid(findFs(s, "/dev/sdc1")->totalBytes));
    QCOMPARE(findFs(s, "/dev/sdd1")->error, "permission denied"s);
    QCOMPARE(findFs(s, "/dev/sde1")->error, "inconsistent statvfs values"s); // blocks*frsize overflow
    QVERIFY(!ps::isValid(findFs(s, "/dev/sdf1")->totalBytes));               // 0-block fs is not "0 % used"
}

void TestAdversarial::samplerOvermountedLocalNotProbedThroughRemote()
{
    // A local filesystem covered by a network/FUSE mount (same mount point, or
    // a parent directory) is still listed in mountinfo; statvfs() on its path
    // would resolve to — and could block on — the remote.
    FixtureRoot r;
    writeBaseTree(r);
    r.write("proc/self/mountinfo",
            mi(1, 0, "8:2", "/", "/", "rw", "ext4", "/dev/sda2") + mi(2, 1, "8:1", "/", "/data", "rw", "ext4", "/dev/sda1")
                + mi(3, 2, "0:50", "/", "/data", "rw", "nfs4", "server:/export")
                + mi(4, 1, "8:17", "/", "/mnt/disk", "rw", "ext4", "/dev/sdb1")
                + mi(5, 1, "0:51", "/", "/mnt", "rw", "fuse.sshfs", "u@h:/")
                + mi(6, 5, "8:33", "/", "/mnt/inner", "rw", "ext4", "/dev/sdc1"));
    auto rec = std::make_shared<StatFsRecorder>();
    ps::Sampler smp(recordingOptions(r, rec));
    const auto s = smp.sample(1, 1);
    const auto calls = rec->snapshotCalls();
    const std::set<std::string> called(calls.begin(), calls.end());
    QVERIFY2(!called.count("/data"), "statFs probed /data, which is covered by an NFS mount");
    QVERIFY2(!called.count("/mnt/disk"), "statFs probed /mnt/disk, which is hidden under a FUSE mount on /mnt");
    QVERIFY2(!called.count("/mnt/inner"), "statFs probed /mnt/inner, whose lookup walks through FUSE");
    QCOMPARE(called, std::set<std::string>{"/"});
    for (const char* src : {"/dev/sda1", "/dev/sdb1", "/dev/sdc1"}) {
        const auto* f = findFs(s, src);
        QVERIFY(f);
        QVERIFY(!ps::isValid(f->totalBytes));
        QVERIFY(!f->error.empty());
    }
    QCOMPARE(findFs(s, "/dev/sda1")->error, "hidden by another mount"s);
}

void TestAdversarial::samplerOvermountedLocalByLocal()
{
    // Two local filesystems stacked on one mount point: statvfs("/data") can
    // only describe the top one, so the covered one must not get its numbers.
    FixtureRoot r;
    writeBaseTree(r);
    r.write("proc/self/mountinfo", mi(1, 0, "8:2", "/", "/", "rw", "ext4", "/dev/sda2")
                                       + mi(2, 1, "8:1", "/", "/data", "rw", "ext4", "/dev/sda1")
                                       + mi(3, 2, "8:17", "/", "/data", "rw", "xfs", "/dev/sdb1"));
    auto rec = std::make_shared<StatFsRecorder>();
    ps::Sampler smp(recordingOptions(r, rec));
    const auto s = smp.sample(1, 1);
    const auto* covered = findFs(s, "/dev/sda1");
    QVERIFY2(!covered || !ps::isValid(covered->totalBytes),
             "covered /dev/sda1 reported the capacity of the filesystem mounted over it");
    const auto* top = findFs(s, "/dev/sdb1");
    QVERIFY(top);
    QCOMPARE(top->totalBytes, 4096000.0);
}

void TestAdversarial::samplerBtrfsSubvolumeDeviceIdentity()
{
    // btrfs reports a per-subvolume anonymous st_dev from stat(), while
    // mountinfo shows the superblock's s_dev (on this kind of host: "/" is
    // 0:35 in mountinfo but stat("/") says 0:38). That is the normal state of
    // every btrfs subvolume mount, not a remount race.
    FixtureRoot r;
    writeBaseTree(r);
    r.write("proc/self/mountinfo", mi(1, 0, "0:35", "/@", "/", "rw", "btrfs", "/dev/nvme0n1p3")
                                       + mi(2, 1, "0:35", "/@home", "/home", "rw", "btrfs", "/dev/nvme0n1p3"));
    auto rec = std::make_shared<StatFsRecorder>();
    ps::FsStats st{1000, 500, 400, 4096};
    st.device = makedev(0, 38);
    st.hasDevice = true;
    rec->answers["/"] = st;
    ps::Sampler smp(recordingOptions(r, rec));
    const auto s = smp.sample(1, 1);
    const auto* f = findFs(s, "/dev/nvme0n1p3");
    QVERIFY(f);
    QVERIFY2(ps::isValid(f->totalBytes),
             qPrintable(QStringLiteral("btrfs subvolume capacity unavailable: error \"%1\"")
                            .arg(QString::fromStdString(f->error))));
    QCOMPARE(f->totalBytes, 4096000.0);
}

void TestAdversarial::samplerHostRootCapacityWithDefaultProbe()
{
    // Read-only host check with the production default statFs on "/" only:
    // copies this process's own mountinfo line for "/" into a fixture.
    std::ifstream in("/proc/self/mountinfo");
    std::string line, rootLine;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string f[5];
        for (auto& x : f)
            ss >> x;
        if (f[4] == "/")
            rootLine = line + "\n"; // last one wins: the visible root
    }
    if (rootLine.empty())
        QSKIP("no root entry in /proc/self/mountinfo");
    FixtureRoot r;
    writeBaseTree(r);
    r.write("proc/self/mountinfo", rootLine);
    auto o = fixtureOptions(r);
    o.statFs = nullptr; // production default
    o.fsProbeWaitMs = 3000;
    ps::Sampler smp(o);
    const auto s = smp.sample(1, 1);
    if (s.filesystems.empty())
        QSKIP("host root is not a local block filesystem (container or pseudo root)");
    const auto& f = s.filesystems.front();
    QVERIFY2(f.error.empty() && ps::isValid(f.totalBytes) && f.totalBytes > 0,
             qPrintable(QStringLiteral("host \"/\" (%1 %2) capacity unavailable: \"%3\"")
                            .arg(QString::fromStdString(f.fsType), QString::fromStdString(f.source),
                                 QString::fromStdString(f.error))));
    qInfo("host root evidence: %s %s total=%.0f used=%.0f avail=%.0f", f.fsType.c_str(), f.source.c_str(),
          f.totalBytes, f.usedBytes, f.availBytes);
}

void TestAdversarial::samplerLateCapacityResultCannotReviveOldMount()
{
    // A probe for /data (then /dev/sda1) is stuck; meanwhile /data is
    // remounted with /dev/sdb1. When the old answer finally arrives it must
    // not be shown as /dev/sdb1's capacity — both when the probe reports the
    // device it measured and when a statFs implementation cannot.
    for (const bool reportsDevice : {true, false}) {
        FixtureRoot r;
        writeBaseTree(r);
        r.write("proc/self/mountinfo", mi(1, 0, "8:2", "/", "/", "rw", "ext4", "/dev/sda2")
                                           + mi(2, 1, "8:1", "/", "/data", "rw", "ext4", "/dev/sda1"));
        struct Gate {
            std::mutex m;
            std::condition_variable cv;
            bool released = false;
            int dataCalls = 0;
        };
        auto gate = std::make_shared<Gate>();
        auto o = fixtureOptions(r);
        o.fsProbeWaitMs = 0; // production behaviour: never wait for the probe
        o.statFs = [gate, reportsDevice](const std::string& path, int&) -> std::optional<ps::FsStats> {
            ps::FsStats st{1000, 500, 500, 4096};
            st.hasDevice = reportsDevice;
            st.device = makedev(8, 2);
            if (path != "/data")
                return st;
            std::unique_lock lock(gate->m);
            if (++gate->dataCalls == 1) {
                gate->cv.wait_for(lock, std::chrono::seconds(10), [&] { return gate->released; });
                st.device = makedev(8, 1); // measured the old filesystem
                return st;
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(150)); // new batch is slower than one sample
            st.blocks = 2000; // the new filesystem
            st.bfree = st.bavail = 1000;
            st.device = makedev(8, 17);
            return st;
        };
        ps::Sampler smp(o);
        smp.sample(1, 1); // queues [/, /data]; /data blocks
        r.write("proc/self/mountinfo", mi(1, 0, "8:2", "/", "/", "rw", "ext4", "/dev/sda2")
                                           + mi(3, 1, "8:17", "/", "/data", "rw", "ext4", "/dev/sdb1"));
        const auto s2 = smp.sample(2, 1); // probe still outstanding
        QVERIFY(!ps::isValid(findFs(s2, "/dev/sdb1")->totalBytes));
        {
            std::lock_guard lock(gate->m);
            gate->released = true;
        }
        gate->cv.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // old batch completes between samples
        for (int i = 3; i <= 4; ++i) {
            const auto s = smp.sample(i, 1);
            const auto* f = findFs(s, "/dev/sdb1");
            QVERIFY(f);
            QVERIFY2(!ps::isValid(f->totalBytes) || f->totalBytes == 2000.0 * 4096,
                     qPrintable(QStringLiteral("reportsDevice=%1 t=%2: /dev/sdb1 shows %3 bytes, the old "
                                               "/dev/sda1 answer queued before the remount")
                                    .arg(reportsDevice).arg(i).arg(f->totalBytes, 0, 'f', 0)));
            QVERIFY(!findFs(s, "/dev/sda1"));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const auto s5 = smp.sample(5, 1);
        QCOMPARE(findFs(s5, "/dev/sdb1")->totalBytes, 2000.0 * 4096); // recovers with the new identity
    }
}

void TestAdversarial::samplerLateCapacityErrorCannotStampNewMount()
{
    // Same A -> B replacement of /data, but the stuck probe of A ends in an
    // error. A's EIO must not be shown as B's state.
    FixtureRoot r;
    writeBaseTree(r);
    r.write("proc/self/mountinfo", mi(1, 0, "8:2", "/", "/", "rw", "ext4", "/dev/sda2")
                                       + mi(2, 1, "8:1", "/", "/data", "rw", "ext4", "/dev/sda1"));
    struct Gate {
        std::mutex m;
        std::condition_variable cv;
        bool released = false;
        int dataCalls = 0;
    };
    auto gate = std::make_shared<Gate>();
    auto o = fixtureOptions(r);
    o.fsProbeWaitMs = 0;
    o.statFs = [gate](const std::string& path, int& err) -> std::optional<ps::FsStats> {
        ps::FsStats st{1000, 500, 500, 4096};
        if (path != "/data")
            return st;
        std::unique_lock lock(gate->m);
        if (++gate->dataCalls == 1) {
            gate->cv.wait_for(lock, std::chrono::seconds(10), [&] { return gate->released; });
            err = EIO; // old filesystem A failed
            return std::nullopt;
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        st.blocks = 2000;
        st.bfree = st.bavail = 1000;
        return st;
    };
    ps::Sampler smp(o);
    smp.sample(1, 1);
    r.write("proc/self/mountinfo", mi(1, 0, "8:2", "/", "/", "rw", "ext4", "/dev/sda2")
                                       + mi(3, 1, "8:17", "/", "/data", "rw", "ext4", "/dev/sdb1"));
    smp.sample(2, 1);
    {
        std::lock_guard lock(gate->m);
        gate->released = true;
    }
    gate->cv.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::string eio = std::strerror(EIO);
    for (int i = 3; i <= 4; ++i) {
        const auto s = smp.sample(i, 1);
        const auto* f = findFs(s, "/dev/sdb1");
        QVERIFY(f);
        QVERIFY2(f->error != eio, qPrintable(QStringLiteral("t=%1: /dev/sdb1 shows /dev/sda1's error \"%2\"")
                                                 .arg(i).arg(QString::fromStdString(f->error))));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    QCOMPARE(findFs(smp.sample(5, 1), "/dev/sdb1")->totalBytes, 2000.0 * 4096);
}

void TestAdversarial::samplerBlockedCapacityProbeDoesNotStallCoreMetrics()
{
    // statFs blocks (as statvfs on a dying local disk can). CPU and other
    // metrics must keep flowing, at most one probe may be outstanding, the
    // stuck filesystem is flagged once stale, and teardown stays bounded.
    FixtureRoot r;
    writeBaseTree(r);
    r.write("proc/self/mountinfo", mi(1, 0, "8:2", "/", "/", "rw", "ext4", "/dev/sda2")
                                       + mi(2, 1, "8:1", "/", "/stuck", "rw", "ext4", "/dev/sda1"));
    struct Gate {
        std::mutex m;
        std::condition_variable cv;
        bool released = false;
        std::atomic<int> calls{0};
        std::atomic<int> concurrent{0};
        std::atomic<int> maxConcurrent{0};
    };
    auto gate = std::make_shared<Gate>();
    auto o = fixtureOptions(r);
    o.statFs = [gate](const std::string& path, int& err) -> std::optional<ps::FsStats> {
        ++gate->calls;
        const int now = ++gate->concurrent;
        int prev = gate->maxConcurrent.load();
        while (now > prev && !gate->maxConcurrent.compare_exchange_weak(prev, now)) {
        }
        if (path == "/stuck") {
            std::unique_lock lock(gate->m);
            gate->cv.wait_for(lock, std::chrono::seconds(10), [&] { return gate->released; });
        }
        --gate->concurrent;
        err = EIO;
        return std::nullopt;
    };
    auto smp = std::make_unique<ps::Sampler>(o);
    o.statFs = {}; // from here only the sampler (and its probe thread) hold the gate
    QElapsedTimer timer;
    timer.start();
    ps::Snapshot last;
    for (int i = 1; i <= 12; ++i) {
        r.write("proc/stat", cpuLine("cpu", 10u * i, 0, 10u * i) + cpuLine("cpu0", 10u * i, 0, 10u * i));
        last = smp->sample(i, 1);
        if (i > 1)
            QCOMPARE(last.cpu.total, 50.0);
    }
    const qint64 sampleMs = timer.elapsed();
    const auto* stuck = findFs(last, "/dev/sda1");
    QVERIFY(stuck);
    QVERIFY(!ps::isValid(stuck->totalBytes));
    QVERIFY2(stuck->error.find("not responding") != std::string::npos,
             qPrintable(QString::fromStdString(stuck->error)));
    timer.restart();
    smp.reset();
    const qint64 teardownMs = timer.elapsed();
    const int calls = gate->calls;
    const int maxConcurrent = gate->maxConcurrent;
    {
        std::lock_guard lock(gate->m);
        gate->released = true;
    }
    gate->cv.notify_all();
    // Once the kernel call returns, the detached probe thread must finish and
    // free its state (the callback copy holding the gate) on its own.
    QElapsedTimer drain;
    drain.start();
    while (gate.use_count() > 1 && drain.elapsed() < 3000)
        QThread::msleep(10);
    QVERIFY2(gate.use_count() == 1,
             qPrintable(QStringLiteral("abandoned probe state still alive %1 ms after the blocked call returned "
                                       "(%2 references)")
                            .arg(drain.elapsed())
                            .arg(gate.use_count() - 1)));
    QVERIFY2(sampleMs < 1500, qPrintable(QStringLiteral("12 samples took %1 ms with a blocked probe").arg(sampleMs)));
    QVERIFY2(calls <= 2, qPrintable(QStringLiteral("%1 statFs calls while one probe was stuck").arg(calls)));
    QCOMPARE(maxConcurrent, 1);
    QVERIFY2(teardownMs < 3500, qPrintable(QStringLiteral("Sampler teardown took %1 ms").arg(teardownMs)));
}

void TestAdversarial::samplerFailedProcessScanInvalidatesBaselines()
{
    // opendir(/proc) fails for one sample while the other files stay readable.
    // Stale per-process baselines from before the failure must not be divided
    // by the latest (single) interval on recovery.
    if (rootUser())
        QSKIP("permission fixtures need a non-root user");
    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r, true));
    writeProc(r, 600, pidStat(600, "busy", 0, 0, 9000), "busy"s, ioText(0, 0));
    smp.sample(1, 1);
    writeProc(r, 600, pidStat(600, "busy", 50, 0, 9000), "busy"s, ioText(500, 0));
    fs::permissions(r.proc(), fs::perms::owner_exec);
    const auto s2 = smp.sample(2, 1);
    fs::permissions(r.proc(), fs::perms::owner_all);
    QVERIFY(hasProblem(s2, "/proc unreadable"));
    QVERIFY(!s2.processesScanned);
    QVERIFY(s2.memory.valid); // the rest of the sample still worked
    writeProc(r, 600, pidStat(600, "busy", 100, 0, 9000), "busy"s, ioText(1000, 0));
    const auto s3 = smp.sample(3, 1);
    const auto* p = findProc(s3, 600);
    QVERIFY(p);
    QVERIFY2(!ps::isValid(p->cpuPercent),
             qPrintable(QStringLiteral("cpu after failed scan = %1 % (100 ticks over 2 s divided by 1 s)")
                            .arg(p->cpuPercent)));
    QVERIFY2(!ps::isValid(p->ioReadBps),
             qPrintable(QStringLiteral("io read after failed scan = %1 B/s").arg(p->ioReadBps)));
}

// ===========================================================================
// Collector (UI-thread facade + worker thread)

namespace {

// Threads of this test process named after the collector thread (comm is
// truncated to 15 bytes: "pulsescope-coll").
int collectorThreadCount()
{
    int n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/proc/self/task", ec)) {
        std::ifstream f(e.path() / "comm");
        std::string comm;
        std::getline(f, comm);
        n += comm.rfind("pulsescope-col", 0) == 0;
    }
    return n;
}

} // namespace

void TestAdversarial::collectorPausedBeforeStartThenResumeSamples()
{
    FixtureRoot r;
    writeBaseTree(r);
    {
        Collector control(fixtureOptions(r));
        QSignalSpy spy(&control, &Collector::snapshotReady);
        control.start(1000);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 2500); // harness sanity
        QVERIFY(control.stop());
    }
    Collector c(fixtureOptions(r));
    QSignalSpy spy(&c, &Collector::snapshotReady);
    c.setPaused(true);
    c.start(1000);
    c.setPaused(false);
    QTRY_VERIFY2_WITH_TIMEOUT(spy.count() > 0, "setPaused(true); start(); setPaused(false) never sampled", 2500);
    QVERIFY(c.stop());
}

void TestAdversarial::collectorStopDropsInFlightSnapshots()
{
    FixtureRoot r;
    writeBaseTree(r);
    Collector c(fixtureOptions(r));
    QSignalSpy spy(&c, &Collector::snapshotReady);
    c.start(Collector::kMinIntervalMs);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 2500);
    // Let the worker queue a few snapshots while this (UI) thread is busy.
    QThread::msleep(700);
    QVERIFY(c.stop());
    const int atStop = spy.count();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QVERIFY2(spy.count() == atStop,
             qPrintable(QStringLiteral("%1 snapshot(s) emitted after stop() returned").arg(spy.count() - atStop)));
    QVERIFY(!c.isRunning());
}

void TestAdversarial::collectorBlockedGuiDeliveryBounded()
{
    FixtureRoot r;
    writeBaseTree(r);
    Collector c(fixtureOptions(r));
    QSignalSpy spy(&c, &Collector::snapshotReady);
    c.start(Collector::kMinIntervalMs);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 2500);
    QCoreApplication::processEvents();
    const int before = spy.count();
    QThread::msleep(3000); // UI thread blocked for ~12 sampling intervals
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
    const int burst = spy.count() - before;
    QVERIFY2(burst <= 2, qPrintable(QStringLiteral("%1 queued whole snapshots delivered after a 3 s UI stall")
                                        .arg(burst)));
    // The first sample taken after the stall spans ~3 s of skipped ticks and
    // must be marked discontinuous rather than averaged as one interval.
    const int seen = spy.count();
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > seen, 2500);
    const auto after = spy.at(seen).at(0).value<ps::SnapshotPtr>();
    QVERIFY(after);
    QVERIFY2(!after->continuous, qPrintable(QStringLiteral("post-stall sample continuous, interval %1 s")
                                                .arg(after->interval)));
    QVERIFY(c.stop());
}

void TestAdversarial::collectorStopBoundedWhenWorkerBlocked()
{
    // /proc/stat is a FIFO with no writer, so the worker blocks inside open()
    // exactly like a hung kernel read. stop() must return within its timeout
    // (plus scheduling slack) and report failure, and nothing is delivered.
    FixtureRoot r;
    writeBaseTree(r);
    r.remove("proc/stat");
    const std::string fifo = r.path("proc/stat");
    QCOMPARE(::mkfifo(fifo.c_str(), 0600), 0);
    // The worker's options hold the only other reference to this sentinel, so
    // its count shows whether the abandoned worker and sampler were destroyed.
    // The userName callback runs on the collector thread once the blocked
    // read returns, which identifies the QThread object to watch.
    auto sentinel = std::make_shared<int>(0);
    auto workerThread = std::make_shared<std::atomic<QThread*>>(nullptr);
    writeProc(r, 900, pidStat(900, "p", 0, 0, 100), "p"s, ioText(0, 0), 777);
    auto o = fixtureOptions(r, true);
    o.statFs = [sentinel](const std::string&, int& err) -> std::optional<ps::FsStats> {
        err = ENOENT;
        return std::nullopt;
    };
    o.userName = [workerThread](uint32_t uid) {
        workerThread->store(QThread::currentThread());
        return "u" + std::to_string(uid);
    };
    auto c = std::make_unique<Collector>(o);
    o.statFs = {};
    o.userName = {};
    QSignalSpy spy(c.get(), &Collector::snapshotReady);
    c->start(1000);
    QThread::msleep(300); // worker is now blocked in open(fifo)
    QElapsedTimer timer;
    timer.start();
    const bool stopped = c->stop(500);
    const qint64 ms = timer.elapsed();
    // Release the abandoned worker: opening the write end unblocks the reader.
    const int wfd = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (wfd >= 0)
        ::close(wfd);
    QThread::msleep(300);
    // Deletion of the QThread object can only happen through this thread's
    // event loop (it lives here), so guarding it before spinning is safe.
    QPointer<QThread> abandoned(workerThread->load());
    const bool sawThread = !abandoned.isNull();
    QCoreApplication::processEvents();
    QVERIFY(wfd >= 0); // the worker really was blocked on the FIFO
    QVERIFY2(sawThread, "worker did not resume sampling after the blocking call returned");
    QVERIFY(!stopped);
    QVERIFY2(ms < 1000, qPrintable(QStringLiteral("stop(500) took %1 ms").arg(ms)));
    QVERIFY(!c->isRunning());
    QCOMPARE(spy.count(), 0);
    c.reset(); // destructor after an abandoned stop must not hang or crash
    // Released from the kernel call, the abandoned thread must run its queued
    // shutdown, exit, and delete the worker (and with it the sampler).
    QElapsedTimer drain;
    drain.start();
    while ((sentinel.use_count() > 1 || collectorThreadCount() > 0 || abandoned) && drain.elapsed() < 3000) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    QVERIFY2(collectorThreadCount() == 0, "abandoned collector thread still running after its blocking call returned");
    QVERIFY2(sentinel.use_count() == 1,
             qPrintable(QStringLiteral("abandoned worker/sampler never destroyed (%1 references to its options)")
                            .arg(sentinel.use_count() - 1)));
    QVERIFY2(abandoned.isNull(),
             "abandoned QThread object never deleted after its thread finished (collector.cpp: stop() drops "
             "m_thread without scheduling deletion)");
}


namespace {

// Serves /proc/stat through a FIFO so every sampler read is counted exactly
// (one read == one sample == one emitted snapshot).
class StatFifoServer
{
public:
    explicit StatFifoServer(std::string path)
        : m_path(std::move(path))
    {
        m_thread = std::thread([this] { run(); });
    }
    ~StatFifoServer()
    {
        m_stop = true;
        m_thread.join();
    }
    int served() const { return m_served; }
    // Gated: no writer attaches, so the sampler blocks opening /proc/stat.
    void setGated(bool gated) { m_gated = gated; }

private:
    void run()
    {
        uint64_t n = 0;
        while (!m_stop) {
            if (m_gated) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const int fd = ::open(m_path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) { // ENXIO: no reader yet
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            n += 10;
            const std::string text = cpuLine("cpu", n, 0, n) + cpuLine("cpu0", n, 0, n);
            [[maybe_unused]] const auto w = ::write(fd, text.data(), text.size());
            ::close(fd);
            ++m_served;
            // Let the reader see EOF and close before a new writer can attach.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    std::string m_path;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_gated{false};
    std::atomic<int> m_served{0};
    std::thread m_thread;
};

class SlowDelivery : public QObject
{
public:
    bool slow = false;
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (slow && e->type() == QEvent::MetaCall)
            QThread::msleep(300); // busy UI thread, longer than one 250 ms tick
        return QObject::eventFilter(o, e);
    }
};

} // namespace

void TestAdversarial::collectorOldGenerationAcksDoNotReleaseCurrentPermit()
{
    // Repeated start() and pause/resume while the UI thread is busy. Every
    // activation wants to sample at once, and each generation change makes the
    // snapshot already queued stale. Invariant: at most one snapshot is ever in
    // flight, so no matter how many generations or activations happen, the
    // worker takes at most one sample until the UI thread handles (delivers or
    // drops) the queued one. Dropping a stale snapshot must still release the
    // permit, or the current generation would never be delivered.
    FixtureRoot r;
    writeBaseTree(r);
    r.remove("proc/stat");
    const std::string fifo = r.path("proc/stat");
    QCOMPARE(::mkfifo(fifo.c_str(), 0600), 0);
    StatFifoServer server(fifo);
    Collector c(fixtureOptions(r));
    SlowDelivery filter;
    c.installEventFilter(&filter);
    QSignalSpy spy(&c, &Collector::snapshotReady);
    c.start(Collector::kMinIntervalMs);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 2, 3000);

    // Phase 1: generations and activations churn; the UI thread never spins.
    constexpr int kCycles = 5;
    const int before = server.served();
    for (int i = 0; i < kCycles; ++i) {
        c.setPaused(true);
        c.setPaused(false);
        c.start(Collector::kMinIntervalMs); // re-activates the same generation
        QThread::msleep(300);               // > one tick: the timer fires too
    }
    QThread::msleep(300);
    const int afterBurst = server.served();
    const uint64_t heldSequence = c.outstandingSequence();
    QVERIFY2(afterBurst - before <= 1,
             qPrintable(QStringLiteral("%1 samples taken across %2 activations with no UI acknowledgement "
                                       "(one-in-flight bound broken)")
                            .arg(afterBurst - before)
                            .arg(3 * kCycles)));
    QVERIFY2(heldSequence != 0, "no snapshot in flight after the burst although the timer kept running");

    // Phase 2: the UI thread is slow (300 ms per queued call). The queued
    // snapshot may be stale; dropping it may release exactly one new sample.
    int servedAtCurrent = -1;
    const quint64 current = c.generation();
    QObject ctx;
    connect(&c, &Collector::snapshotReady, &ctx, [&](const ps::SnapshotPtr& snap) {
        if (servedAtCurrent < 0 && snap && snap->generation == current)
            servedAtCurrent = server.served();
    });
    filter.slow = true;
    QElapsedTimer t;
    t.start();
    while (servedAtCurrent < 0 && t.elapsed() < 4000)
        QCoreApplication::processEvents();
    filter.slow = false;
    QVERIFY2(servedAtCurrent >= 0, "current-generation snapshot never delivered (permit leaked by a stale drop)");
    QVERIFY2(servedAtCurrent - afterBurst <= 1,
             qPrintable(QStringLiteral("%1 samples taken before the first current-generation delivery")
                            .arg(servedAtCurrent - afterBurst)));

    // Phase 3: pause with a snapshot possibly queued. While paused nothing is
    // sampled or delivered, the dropped snapshot releases the permit, and
    // resuming produces a fresh (rebased, discontinuous) sample promptly.
    c.setPaused(true);
    QThread::msleep(300); // at most one tick can race the queued pause()
    const int servedPaused = server.served();
    QThread::msleep(700);
    QCOMPARE(server.served(), servedPaused);
    const int spyPaused = spy.count();
    t.restart();
    while (t.elapsed() < 400)
        QCoreApplication::processEvents();
    QCOMPARE(spy.count(), spyPaused);
    QCOMPARE(c.outstandingSequence(), uint64_t(0));
    c.setPaused(false);
    QTRY_VERIFY2_WITH_TIMEOUT(spy.count() > spyPaused, "resume after a paused drop never delivered", 1500);
    const auto resumed = spy.at(spyPaused).at(0).value<ps::SnapshotPtr>();
    QVERIFY(resumed);
    QCOMPARE(resumed->generation, c.generation());
    QVERIFY(!resumed->continuous);
    QVERIFY(c.stop());
}

// ===========================================================================
// Spec pass 3: numeric fidelity and pinned process identity

void TestAdversarial::memoryMissingOrInvalidSReclaimableIsNotZero()
{
    // cache = Buffers + Cached + SReclaimable (snapshot.h). Every kernel since
    // 2.6.19 reports SReclaimable, so a missing or unparseable line means the
    // input is truncated or corrupt; treating it as 0 KiB under-reports cache
    // by the whole slab-reclaimable size while looking like a real reading.
    const std::string base =
        "MemTotal: 1000 kB\nMemFree: 100 kB\nMemAvailable: 500 kB\nBuffers: 10 kB\nCached: 20 kB\n";
    const auto good = metrics::memoryFromRaw(parse::parseMeminfo(base + "SReclaimable: 5 kB\n"));
    QCOMPARE(good.cache, 35.0 * 1024);
    const auto realZero = metrics::memoryFromRaw(parse::parseMeminfo(base + "SReclaimable: 0 kB\n"));
    QCOMPARE(realZero.cache, 30.0 * 1024); // a real 0 stays a real 0

    const std::vector<std::pair<const char*, std::string>> bad = {
        {"missing", ""},
        {"junk", "SReclaimable: junk kB\n"},
        {"negative", "SReclaimable: -5 kB\n"},
        {"trailing junk", "SReclaimable: 5x kB\n"},
        {"empty value", "SReclaimable:\n"},
        {"kB overflow", "SReclaimable: 18446744073709551615 kB\n"},
    };
    for (const auto& [what, line] : bad) {
        const auto raw = parse::parseMeminfo(base + line);
        QVERIFY2(!raw.sReclaimable, what);
        const auto m = metrics::memoryFromRaw(raw);
        QCOMPARE(m.used, 500.0 * 1024); // unrelated fields are unaffected
        QVERIFY2(!ps::isValid(m.cache),
                 qPrintable(QStringLiteral("SReclaimable %1: cache reported as %2 bytes (Buffers+Cached, "
                                           "SReclaimable silently 0)")
                                .arg(QLatin1String(what))
                                .arg(m.cache, 0, 'f', 0)));
    }
}

void TestAdversarial::samplerProcessTickSumOverflowIsNotPlausibleCpu()
{
    // utime and stime are separate u64 fields. UINT64_MAX + 11 wraps to 10 in
    // uint64 arithmetic; from a previous total of 0 that becomes 10 ticks at
    // 100 Hz over 1 s = a plausible 10 % instead of an impossible jump.
    const auto st = parse::parsePidStat(pidStat(700, "wrap", UINT64_MAX, 11, 8000));
    QVERIFY(st);
    QCOMPARE(st->utime, UINT64_MAX); // parser keeps the full field
    QCOMPARE(st->stime, uint64_t(11));

    FixtureRoot r;
    writeBaseTree(r);
    ps::Sampler smp(fixtureOptions(r, true));
    writeProc(r, 700, pidStat(700, "wrap", 0, 0, 8000), "wrap"s, ioText(0, 0));
    smp.sample(1, 1);
    writeProc(r, 700, pidStat(700, "wrap", UINT64_MAX, 11, 8000), "wrap"s, ioText(0, 0));
    const auto s2 = smp.sample(2, 1);
    const auto* p = findProc(s2, 700);
    QVERIFY(p);
    QVERIFY2(!ps::isValid(p->cpuPercent),
             qPrintable(QStringLiteral("utime=UINT64_MAX stime=11 from 0/0 reported as %1 % CPU").arg(p->cpuPercent)));
}

void TestAdversarial::samplerProcessDirReplacedMidReadDoesNotMixFields()
{
    // The userName callback runs between the status and cmdline reads of a new
    // process. Replace the fixture PID directory right there with a different
    // process (same PID, different start time and command). Every field of a
    // reported process must come from one directory: the reads are pinned to
    // the directory opened for stat, so no record may pair the old stat with
    // the new command line.
    FixtureRoot r;
    writeBaseTree(r);
    writeProc(r, 400, pidStat(400, "oldproc", 5, 0, 111), "old-cmd\0--a\0"s, ioText(0, 0), 4242);
    auto o = fixtureOptions(r, true);
    bool raced = false;
    o.userName = [&](uint32_t uid) {
        if (uid == 4242 && !raced) {
            raced = true;
            fs::rename(r.path("proc/400"), r.path("proc/old400"));
            writeProc(r, 400, pidStat(400, "newproc", 9, 0, 222), "new-cmd\0--b\0"s, ioText(0, 0), 0);
        }
        return "u" + std::to_string(uid);
    };
    ps::Sampler smp(o);
    const auto s = smp.sample(1, 1);
    QVERIFY(raced);
    int seen = 0;
    for (const auto& p : s.processes) {
        if (p.pid != 400)
            continue;
        ++seen;
        const bool oldSet = p.name == "oldproc" && p.startTicks == 111 && p.command == "old-cmd --a" && p.user == "u4242";
        const bool newSet = p.name == "newproc" && p.startTicks == 222 && p.command == "new-cmd --b" && p.user == "u0";
        QVERIFY2(oldSet || newSet,
                 qPrintable(QStringLiteral("mixed record: name=%1 start=%2 command=%3 user=%4")
                                .arg(QString::fromStdString(p.name))
                                .arg(p.startTicks)
                                .arg(QString::fromStdString(p.command), QString::fromStdString(p.user))));
    }
    QVERIFY(seen >= 1);
}

void TestAdversarial::historyHighCardinalityKeepsEssentialSeries_data()
{
    QTest::addColumn<int>("cores");
    QTest::addColumn<int>("virtualIfs");
    QTest::addColumn<int>("loopDisks");
    QTest::newRow("512 cores") << 512 << 0 << 0;
    QTest::newRow("1024 cores") << 1024 << 0 << 0;
    QTest::newRow("600 veth + 300 loop devices") << 8 << 600 << 300;
}

void TestAdversarial::historyHighCardinalityKeepsEssentialSeries()
{
    // A big host (or a container host full of veth/loop devices) produces more
    // per-item series than History::kMaxSeries. The bound must hold, but it may
    // only cost per-item detail: the aggregate CPU, memory, swap, disk and
    // network series and the (single) temperature sensor behind every other
    // graph must still be recorded, sample after sample.
    QFETCH(int, cores);
    QFETCH(int, virtualIfs);
    QFETCH(int, loopDisks);
    auto snap = [&](double t, double v) {
        ps::Snapshot s;
        s.time = t;
        s.continuous = t > 1;
        s.cpu.total = v;
        s.cpu.user = v / 2;
        s.cpu.system = v / 4;
        s.cpu.iowait = 1;
        for (int i = 0; i < cores; ++i) {
            ps::CoreUsage c;
            c.index = i;
            c.online = true;
            c.usage.total = v;
            s.cores.push_back(c);
        }
        s.memory.valid = true;
        s.memory.total = 64.0 * (1ull << 30);
        s.memory.used = 16.0 * (1ull << 30);
        s.memory.available = 48.0 * (1ull << 30);
        s.memory.cache = 8.0 * (1ull << 30);
        s.memory.hasSwap = true;
        s.memory.swapTotal = 8.0 * (1ull << 30);
        s.memory.swapUsed = 1.0 * (1ull << 30);
        s.memory.swapFree = 7.0 * (1ull << 30);
        for (int i = 0; i < loopDisks; ++i) {
            ps::DiskIo d;
            d.name = "loop" + std::to_string(i);
            d.kind = ps::DeviceKind::Loopback;
            d.readBps = d.writeBps = 0;
            s.disks.push_back(d);
        }
        ps::DiskIo nvme;
        nvme.name = "nvme0n1";
        nvme.readBps = 1000 * v;
        nvme.writeBps = 2000 * v;
        s.disks.push_back(nvme);
        s.diskTotal = {"nvme0n1", 1, 1000 * v, 2000 * v};
        for (int i = 0; i < virtualIfs; ++i) {
            ps::NetIo n;
            n.name = "veth" + std::to_string(i);
            n.kind = ps::DeviceKind::Virtual;
            n.rxBps = n.txBps = 0;
            s.interfaces.push_back(n);
        }
        ps::NetIo eth;
        eth.name = "eth0";
        eth.rxBps = 300 * v;
        eth.txBps = 400 * v;
        s.interfaces.push_back(eth);
        s.netTotal = {"eth0", 1, 300 * v, 400 * v};
        ps::TemperatureReading temp;
        temp.key = "hwmon:k10temp:temp1";
        temp.chip = "k10temp";
        temp.label = "Tctl";
        temp.displayName = "k10temp Tctl";
        temp.celsius = 40 + v / 10;
        s.temperatures.push_back(temp);
        return s;
    };
    ps::History h(100, 1e9);
    for (int i = 1; i <= 3; ++i)
        ps::recordSnapshot(h, snap(i, 10.0 * i));
    QVERIFY2(h.seriesCount() <= ps::History::kMaxSeries,
             qPrintable(QStringLiteral("%1 series exceed the %2 bound").arg(h.seriesCount()).arg(ps::History::kMaxSeries)));

    using namespace ps::keys;
    const std::pair<std::string, double> essential[] = {
        {std::string(cpuTotal), 30},
        {std::string(cpuUser), 15},
        {std::string(cpuSystem), 7.5},
        {std::string(cpuIowait), 1},
        {std::string(memUsed), 16.0 * (1ull << 30)},
        {std::string(memCache), 8.0 * (1ull << 30)},
        {std::string(memAvailable), 48.0 * (1ull << 30)},
        {std::string(swapUsed), 1.0 * (1ull << 30)},
        {std::string(diskAllRead), 30000},
        {std::string(diskAllWrite), 60000},
        {std::string(netAllRx), 9000},
        {std::string(netAllTx), 12000},
        {temperature("hwmon:k10temp:temp1"), 43},
    };
    QStringList missing;
    for (const auto& [key, expected] : essential) {
        const double v = h.latestValue(key);
        if (!h.find(key) || v != expected)
            missing << QStringLiteral("%1=%2").arg(QString::fromStdString(key)).arg(v);
    }
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral("%1 of %2 series slots used by per-item detail; essential series lost: %3")
                            .arg(h.seriesCount())
                            .arg(ps::History::kMaxSeries)
                            .arg(missing.join(QStringLiteral(", ")))));
    // Explicit truncation: every per-item series that did not fit its group
    // is counted in the latest sample (items x series-per-item - quota).
    using G = ps::SeriesGroup;
    auto over = [](std::size_t wanted, G g) {
        return wanted > ps::History::groupQuota(g) ? wanted - ps::History::groupQuota(g) : std::size_t(0);
    };
    QCOMPARE(h.refusedInLastSample(G::Summary), std::size_t(0));
    QCOMPARE(h.refusedInLastSample(G::Temperature), std::size_t(0));
    QCOMPARE(h.refusedInLastSample(G::Core), over(std::size_t(cores), G::Core));
    QCOMPARE(h.refusedInLastSample(G::Disk), over(2 * std::size_t(loopDisks + 1), G::Disk));
    QCOMPARE(h.refusedInLastSample(G::Network), over(2 * std::size_t(virtualIfs + 1), G::Network));
}

// ===========================================================================
// TimeChart (offscreen QtWidgets)

#ifdef PS_ADV_WIDGETS
namespace {

QPalette plainPalette()
{
    QPalette p;
    for (auto role : {QPalette::Base, QPalette::Window, QPalette::ToolTipBase})
        p.setColor(role, Qt::white);
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ToolTipText, QPalette::Highlight})
        p.setColor(role, Qt::black);
    p.setColor(QPalette::PlaceholderText, QColor(128, 128, 128));
    return p;
}

// Series 0 on a light base is blue (0x005fa3); everything else the chart
// draws with this palette is grey (text, grid, frame, gap shading).
bool seriesPixel(QRgb px) { return qBlue(px) > qRed(px) + 60 && qBlue(px) > qGreen(px) + 20; }

QImage renderChart(TimeChart& c)
{
    QImage img(c.size(), QImage::Format_RGB32);
    img.fill(Qt::magenta);
    c.render(&img);
    return img;
}

void addPoint(ps::History& h, double t, double v, bool continuous = true, ps::Unit unit = ps::Unit::Percent)
{
    QVERIFY(h.beginSample(t, continuous));
    h.set("s", "S", unit, v);
    h.endSample();
}

// Compact-mode plot rectangle as documented by the layout: 8 px side
// margins, 6 px top/bottom, one font-height header plus 3 px.
QRectF compactPlot(const TimeChart& c)
{
    const QFontMetrics fm(c.font());
    const double top = 6 + fm.height() + 3;
    return QRectF(8, top, c.width() - 16, c.height() - 6 - top);
}

} // namespace
#endif

void TestAdversarial::chartFixedRangeHonouredForEveryUnit()
{
#ifndef PS_ADV_WIDGETS
    QSKIP("built without QtWidgets/TimeChart");
#else
    struct Case {
        ps::Unit unit;
        double inside, outside;
    };
    const Case cases[] = {
        {ps::Unit::Percent, 50, 95},
        {ps::Unit::Celsius, 45, 95},
        {ps::Unit::Bytes, 50, 95},
        {ps::Unit::BytesPerSecond, 50, 95},
    };
    for (const auto& k : cases) {
        const QString unit = QString::fromLatin1(ps::unitSuffix(k.unit));
        ps::History h(100, 1e9);
        addPoint(h, 1, k.inside, true, k.unit);
        addPoint(h, 2, k.outside, true, k.unit);
        TimeChart c(QStringLiteral("t"), k.unit);
        c.setHistory(&h);
        c.setSeries({{"s", QStringLiteral("S")}});
        c.setFixedRange(20, 80);
        QVERIFY2(c.hasFixedRange(), qPrintable(unit));
        const auto range = c.displayedRange();
        QVERIFY2(range.first == 20 && range.second == 80,
                 qPrintable(QStringLiteral("%1: setFixedRange(20, 80) displays [%2, %3]")
                                .arg(unit)
                                .arg(range.first)
                                .arg(range.second)));
        c.setAutoRange();
        QVERIFY(!c.hasFixedRange());
        const auto autoRange = c.displayedRange();
        QVERIFY2(autoRange.first <= k.inside && autoRange.second >= k.outside,
                 qPrintable(QStringLiteral("%1: auto range [%2, %3] hides data %4..%5")
                                .arg(unit)
                                .arg(autoRange.first)
                                .arg(autoRange.second)
                                .arg(k.inside)
                                .arg(k.outside)));
    }
    // Sub-zero temperatures in auto mode stay inside the axis.
    ps::History h(100, 1e9);
    addPoint(h, 1, -25, true, ps::Unit::Celsius);
    addPoint(h, 2, -5, true, ps::Unit::Celsius);
    TimeChart t(QStringLiteral("t"), ps::Unit::Celsius);
    t.setHistory(&h);
    t.setSeries({{"s", QStringLiteral("S")}});
    const auto cold = t.displayedRange();
    QVERIFY2(cold.first <= -25 && cold.second >= -5,
             qPrintable(QStringLiteral("auto Celsius range [%1, %2] for -25..-5").arg(cold.first).arg(cold.second)));
#endif
}

void TestAdversarial::chartInvalidFixedRangeFallsBackToAuto()
{
#ifndef PS_ADV_WIDGETS
    QSKIP("built without QtWidgets/TimeChart");
#else
    ps::History h(100, 1e9);
    addPoint(h, 1, 150);
    addPoint(h, 2, 350); // several cores busy
    TimeChart c(QStringLiteral("CPU"), ps::Unit::Percent);
    c.setHistory(&h);
    c.setSeries({{"s", QStringLiteral("S")}});
    QVERIFY(c.hasFixedRange()); // percent defaults to 0..100
    QCOMPARE(c.displayedRange(), std::make_pair(0.0, 100.0));
    const double bad[][2] = {{ps::kNaN, 100}, {0, INFINITY}, {-INFINITY, 0}, {80, 20}, {50, 50}};
    for (const auto& b : bad) {
        c.setFixedRange(b[0], b[1]);
        QVERIFY2(!c.hasFixedRange(), qPrintable(QStringLiteral("[%1, %2] accepted").arg(b[0]).arg(b[1])));
        const auto r = c.displayedRange();
        QVERIFY(std::isfinite(r.first) && std::isfinite(r.second) && r.second > r.first);
        QVERIFY2(r.second >= 350, qPrintable(QStringLiteral("auto range tops out at %1 for 350 %").arg(r.second)));
    }
    // Extreme but valid bounds and values must render without hanging or
    // escaping the plot.
    ps::History x(100, 1e9);
    for (int i = 0; i < 20; ++i)
        addPoint(x, i, (i % 2) ? 1e308 : -1e308);
    TimeChart e(QStringLiteral("x"), ps::Unit::Bytes);
    e.setMode(TimeChart::Mode::Compact);
    e.setPalette(plainPalette());
    e.resize(240, 120);
    e.setHistory(&x);
    e.setSeries({{"s", QStringLiteral("S")}});
    for (const auto& fr : {std::make_pair(-1e308, 1e308), std::make_pair(0.0, 1e-300), std::make_pair(1e15, 1e15 + 1)}) {
        e.setFixedRange(fr.first, fr.second);
        // Finite, ordered bounds are accepted, so painting must not feed
        // NaN geometry to QPainter (e.g. max - min overflowing to inf).
        static std::atomic<int> nanWarnings;
        static QtMessageHandler previous;
        nanWarnings = 0;
        previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
            if (msg.contains(QStringLiteral("NaN")))
                ++nanWarnings;
            else
                previous(type, ctx, msg);
        });
        const QImage img = renderChart(e);
        qInstallMessageHandler(previous);
        QVERIFY2(nanWarnings == 0, qPrintable(QStringLiteral("range [%1, %2]: %3 QPainter NaN-geometry warnings")
                                                  .arg(fr.first)
                                                  .arg(fr.second)
                                                  .arg(nanWarnings.load())));
        const QRectF plot = compactPlot(e).adjusted(-1, -1, 1, 2);
        int outside = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int xx = 0; xx < img.width(); ++xx)
                if (seriesPixel(img.pixel(xx, y)) && !plot.contains(xx + 0.5, y + 0.5))
                    ++outside;
        QVERIFY2(outside == 0, qPrintable(QStringLiteral("range [%1, %2]: %3 series pixels outside the plot")
                                              .arg(fr.first)
                                              .arg(fr.second)
                                              .arg(outside)));
    }
#endif
}

void TestAdversarial::chartOffWindowAndOutOfRangeLinesClipped()
{
#ifndef PS_ADV_WIDGETS
    QSKIP("built without QtWidgets/TimeChart");
#else
    // Window 60 s ending at t=100. t=0 lies 100 s back, so the segment
    // t=0 -> t=41 starts ~1.7 plot widths left of the plot. Spikes of +-1e9 %
    // against the fixed 0..100 % axis leave the plot vertically.
    ps::History h(1000, 1e9);
    addPoint(h, 0, 50);
    for (int t = 41; t <= 100; ++t)
        addPoint(h, t, t == 70 ? 1e9 : t == 71 ? -1e9 : 50);
    TimeChart c(QStringLiteral("CPU"), ps::Unit::Percent);
    c.setMode(TimeChart::Mode::Compact);
    c.setPalette(plainPalette());
    c.resize(240, 120);
    c.setWindowSeconds(60);
    c.setHistory(&h);
    c.setSeries({{"s", QStringLiteral("S")}});
    const QImage img = renderChart(c);
    const QRectF plot = compactPlot(c);
    const QRectF clip = plot.adjusted(-1, -1, 1, 2);
    int inside = 0, outside = 0;
    QString where;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (seriesPixel(img.pixel(x, y))) {
                if (clip.contains(x + 0.5, y + 0.5)) {
                    ++inside;
                } else {
                    if (++outside <= 3)
                        where += QStringLiteral(" (%1,%2)").arg(x).arg(y);
                }
            }
    QVERIFY2(inside > 50, "series not drawn at all (harness sanity)");
    QVERIFY2(outside == 0,
             qPrintable(QStringLiteral("%1 series pixels outside plot [%2,%3 %4x%5]:%6")
                            .arg(outside)
                            .arg(plot.left())
                            .arg(plot.top())
                            .arg(plot.width())
                            .arg(plot.height())
                            .arg(where)));
    // The off-window predecessor connects into the left edge instead of the
    // line starting one sample late.
    const int y50 = int(plot.bottom() - 0.5 * plot.height());
    bool edge = false;
    for (int dy = -2; dy <= 2; ++dy)
        edge = edge || seriesPixel(img.pixel(int(plot.left()) + 2, y50 + dy));
    QVERIFY2(edge, "no line at the left plot edge although t=0 -> t=41 crosses it");
    QCOMPARE(c.lastPaintedPoints(), 60); // t=41..100, off-window t=0 not counted
#endif
}

void TestAdversarial::chartGapsAndUnavailableSamplesNotBridged()
{
#ifndef PS_ADV_WIDGETS
    QSKIP("built without QtWidgets/TimeChart");
#else
    // Window 60 s ending at t=100: samples 40..60, a discontinuity (pause)
    // until 80, samples 80..100 with 88..92 unavailable. Neither gap may be
    // bridged by a line.
    ps::History h(1000, 1e9);
    for (int t = 40; t <= 60; ++t)
        addPoint(h, t, 50);
    for (int t = 80; t <= 100; ++t)
        addPoint(h, t, (t >= 88 && t <= 92) ? ps::kNaN : 50, t != 80);
    TimeChart c(QStringLiteral("CPU"), ps::Unit::Percent);
    c.setMode(TimeChart::Mode::Compact);
    c.setPalette(plainPalette());
    c.resize(240, 120);
    c.setWindowSeconds(60);
    c.setHistory(&h);
    c.setSeries({{"s", QStringLiteral("S")}});
    const QImage img = renderChart(c);
    const QRectF plot = compactPlot(c);
    auto xOf = [&](double t) { return plot.right() - (100 - t) / 60 * plot.width(); };
    auto seriesInColumns = [&](double x0, double x1) {
        int n = 0;
        for (int x = int(std::ceil(x0)); x <= int(std::floor(x1)); ++x)
            for (int y = int(plot.top()); y < int(plot.bottom()); ++y)
                n += seriesPixel(img.pixel(x, y));
        return n;
    };
    QVERIFY(seriesInColumns(xOf(45), xOf(55)) > 0); // sanity: the runs are drawn
    const int pause = seriesInColumns(xOf(60) + 3, xOf(80) - 3);
    QVERIFY2(pause == 0, qPrintable(QStringLiteral("%1 series pixels inside the 60..80 s pause gap").arg(pause)));
    const int nan = seriesInColumns(xOf(87) + 3, xOf(93) - 3);
    QVERIFY2(nan == 0, qPrintable(QStringLiteral("%1 series pixels bridging unavailable 88..92 s").arg(nan)));
    QCOMPARE(c.lastPaintedPoints(), 21 + 21 - 5);
#endif
}

void TestAdversarial::chartCursorReadoutKeyboardAndAccessibility()
{
#ifndef PS_ADV_WIDGETS
    QSKIP("built without QtWidgets/TimeChart");
#else
    ps::History h(1000, 1e9);
    for (int t = 91; t <= 99; ++t)
        addPoint(h, t, (t - 90) * 10.0);
    addPoint(h, 100, ps::kNaN); // latest value unavailable
    TimeChart c(QStringLiteral("CPU usage"), ps::Unit::Percent);
    c.setPalette(plainPalette());
    c.resize(520, 240);
    c.setHistory(&h);
    c.setSeries({{"s", QStringLiteral("CPU")}});
    QCOMPARE(c.accessibleName(), QStringLiteral("CPU usage"));
    QVERIFY(c.focusPolicy() & Qt::TabFocus);
    renderChart(c);
    QVERIFY(c.readoutText().isEmpty()); // no cursor yet
    const QString summary = c.accessibleDescription();
    const QString na = QString::fromStdString(ps::fmt::percent(ps::kNaN));
    QVERIFY2(na != QString::fromStdString(ps::fmt::percent(0)), "unavailable formats like 0 %");
    QVERIFY2(summary.contains(na), qPrintable(QStringLiteral("summary '%1' lacks unavailable latest").arg(summary)));

    auto key = [&](int k) {
        QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
        QCoreApplication::sendEvent(&c, &press);
    };
    auto row = [](double v) { return QStringLiteral("CPU: ") + QString::fromStdString(ps::fmt::percent(v)); };

    key(Qt::Key_End);
    QCOMPARE(c.readoutText(), QStringLiteral("Latest sample\n") + row(ps::kNaN));
    QCOMPARE(c.accessibleDescription(), c.readoutText());
    key(Qt::Key_Left);
    QCOMPARE(c.readoutText(), QStringLiteral("1.0 s ago\n") + row(90));
    QCOMPARE(c.accessibleDescription(), c.readoutText());
    key(Qt::Key_Home);
    QCOMPARE(c.readoutText(), QStringLiteral("9.0 s ago\n") + row(10));
    key(Qt::Key_Left); // already at the first sample
    QCOMPARE(c.readoutText(), QStringLiteral("9.0 s ago\n") + row(10));
    key(Qt::Key_Right);
    QCOMPARE(c.readoutText(), QStringLiteral("8.0 s ago\n") + row(20));
    QCOMPARE(c.accessibleDescription(), c.readoutText());

    // A new sample arrives while the cursor is parked: the readout ages and
    // the description follows it, never the stale text.
    addPoint(h, 101, 5);
    c.refresh();
    QCOMPARE(c.readoutText(), QStringLiteral("9.0 s ago\n") + row(20));
    QCOMPARE(c.accessibleDescription(), c.readoutText());

    auto announcesReadout = [&] {
        const QString d = c.accessibleDescription();
        return d.contains(QStringLiteral(" ago")) || d.startsWith(QStringLiteral("Latest sample"));
    };
    key(Qt::Key_Escape);
    QVERIFY(c.readoutText().isEmpty());
    QVERIFY2(!announcesReadout() && c.accessibleDescription().contains(QString::fromStdString(ps::fmt::percent(5))),
             qPrintable(QStringLiteral("cursor dismissed with Escape but description is '%1'")
                            .arg(c.accessibleDescription())));

    key(Qt::Key_End);
    QVERIFY(announcesReadout());
    QFocusEvent out(QEvent::FocusOut, Qt::TabFocusReason);
    QCoreApplication::sendEvent(&c, &out);
    QVERIFY(c.readoutText().isEmpty());
    QVERIFY2(!announcesReadout(),
             qPrintable(QStringLiteral("focus left the chart but description is still the cursor readout '%1'")
                            .arg(c.accessibleDescription())));

    // Hover inside the plot shows a readout; leaving clears it.
    renderChart(c);
    QMouseEvent move(QEvent::MouseMove, QPointF(c.width() * 0.7, c.height() * 0.6),
                     QPointF(c.width() * 0.7, c.height() * 0.6), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&c, &move);
    QVERIFY(!c.readoutText().isEmpty());
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&c, &leave);
    QVERIFY(c.readoutText().isEmpty());

    TimeChart compact(QStringLiteral("CPU"), ps::Unit::Percent);
    compact.setMode(TimeChart::Mode::Compact);
    QCOMPARE(compact.focusPolicy(), Qt::NoFocus);
#endif
}

void TestAdversarial::chartCursorNeverReadsOffWindowSample()
{
#ifndef PS_ADV_WIDGETS
    QSKIP("built without QtWidgets/TimeChart");
#else
    ps::History h(1000, 1e9);
    addPoint(h, 0, 99); // 100 s before the latest sample: outside a 60 s window
    for (int t = 41; t <= 100; ++t)
        addPoint(h, t, 50);
    TimeChart c(QStringLiteral("CPU"), ps::Unit::Percent);
    c.resize(520, 240);
    c.setWindowSeconds(60);
    c.setHistory(&h);
    c.setSeries({{"s", QStringLiteral("CPU")}});
    const QString hidden = QString::fromStdString(ps::fmt::percent(99));
    const QString firstVisible = QStringLiteral("59 s ago\nCPU: ") + QString::fromStdString(ps::fmt::percent(50));
    for (double t : {0.0, -1e18, 40.4}) {
        c.setCursorTime(t);
        QVERIFY2(!c.readoutText().contains(hidden), qPrintable(c.readoutText()));
        QCOMPARE(c.readoutText(), firstVisible);
    }
    c.setCursorTime(1e6); // far after the latest sample, still exactly representable
    QVERIFY2(c.readoutText().startsWith(QStringLiteral("Latest sample")), qPrintable(c.readoutText()));
    for (double t : {ps::kNaN, double(INFINITY), -double(INFINITY)}) {
        c.setCursorTime(t);
        QVERIFY(c.readoutText().isEmpty());
    }
    QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    QCoreApplication::sendEvent(&c, &home);
    QCOMPARE(c.readoutText(), firstVisible);
    QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    QCoreApplication::sendEvent(&c, &left);
    QCOMPARE(c.readoutText(), firstVisible);
    renderChart(c); // painting with a cursor on the edge sample must be safe
#endif
}

void TestAdversarial::chartNoVisibleSampleAfterClockAdvance_data()
{
    QTest::addColumn<int>("capacity");
    QTest::addColumn<int>("firstTime");
    // Partial ring: slots past size() were never written.
    QTest::newRow("partial ring [100,101]") << 16 << 100;
    // Wrapped ring (capacity 4, 96..101 added): slot(size()) is the oldest
    // retained sample, 98, so an index of size() reads an expired value.
    QTest::newRow("wrapped ring [98..101]") << 4 << 96;
}

void TestAdversarial::chartNoVisibleSampleAfterClockAdvance()
{
#ifndef PS_ADV_WIDGETS
    QSKIP("built without QtWidgets/TimeChart");
#else
    // Real-time axis: the view ends at the current clock (setViewEnd), so
    // with a 60 s window and the clock at 162 every sample of [..,101] has
    // aged out while the history is not empty. Then nothing is selectable:
    // cursor, hover, Home/End/arrows and Escape must give no readout and
    // announce no sample value; nothing is drawn. Widening the window or a
    // fresh sample makes samples selectable again, with honest ages.
    QFETCH(int, capacity);
    QFETCH(int, firstTime);
    ps::History h(std::size_t(capacity), 1e9);
    for (int t = firstTime; t <= 101; ++t)
        addPoint(h, t, t - 50.0); // value = t - 50 %, so every sample is distinct
    const double oldest = std::max(firstTime, 101 - capacity + 1);
    QCOMPARE(h.time(0), oldest);
    QCOMPARE(h.latestTime(), 101.0);

    TimeChart c(QStringLiteral("CPU usage"), ps::Unit::Percent);
    c.setPalette(plainPalette());
    c.resize(520, 240);
    c.setHistory(&h);
    c.setSeries({{"s", QStringLiteral("CPU")}});
    c.setWindowSeconds(60);
    c.setViewEnd(101.5);
    c.refresh();
    renderChart(c);
    auto row = [](double v) { return QStringLiteral("CPU: ") + QString::fromStdString(ps::fmt::percent(v)); };
    auto key = [&](int k) {
        QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
        QCoreApplication::sendEvent(&c, &press);
    };
    auto ageOf = [](const QString& readout) {
        const auto m = QRegularExpression(QStringLiteral("(\\d+(?:\\.\\d+)?) s ago"))
                           .match(readout.section(QLatin1Char('\n'), 0, 0));
        if (m.hasMatch())
            return m.captured(1).toDouble();
        return readout.startsWith(QStringLiteral("Latest sample")) ? 0.0 : ps::kNaN;
    };
    // Any text naming a sample: an age, or the value of any sample ever added.
    auto leak = [&](const QString& text) {
        QStringList v;
        if (text.contains(QStringLiteral("s ago")) || text.contains(QStringLiteral("Latest sample")))
            v << QStringLiteral("names a sample");
        for (int t = firstTime; t <= 101; ++t)
            if (text.contains(QString::fromStdString(ps::fmt::percent(t - 50.0))))
                v << QStringLiteral("value of t=%1").arg(t);
        if (text.contains(QStringLiteral("nan"), Qt::CaseInsensitive))
            v << QStringLiteral("NaN text");
        return v.isEmpty() ? QString() : QStringLiteral("\"%1\" (%2)").arg(text, v.join(QStringLiteral(", ")));
    };
    auto nothingSelectable = [&](const char* step) {
        const QString ro = c.readoutText();
        QVERIFY2(ro.isEmpty(), qPrintable(QStringLiteral("%1: readout of an aged-out sample: \"%2\"")
                                              .arg(QLatin1String(step), ro)));
        const QString a11y = leak(c.accessibleDescription());
        QVERIFY2(a11y.isEmpty(), qPrintable(QStringLiteral("%1: accessible description %2").arg(QLatin1String(step), a11y)));
        renderChart(c);
        QCOMPARE(c.lastPaintedPoints(), 0);
    };

#define NOTHING_SELECTABLE(step)                                                                                       \
    do {                                                                                                               \
        nothingSelectable(step);                                                                                       \
        if (QTest::currentTestFailed())                                                                                \
            return;                                                                                                    \
    } while (false)

    // An active keyboard cursor on the latest sample, then the clock advances.
    c.setCursorTime(101);
    QVERIFY2(c.readoutText().contains(row(51)), qPrintable(c.readoutText()));
    c.setViewEnd(162);
    c.refresh();
    QCOMPARE(c.viewEnd(), 162.0); // the real-time axis is kept, not rolled back to 101
    QVERIFY(!c.hasVisibleData());
    NOTHING_SELECTABLE("cursor aged out");
    for (const auto& [k, name] : {std::pair {int(Qt::Key_Home), "Home"}, {int(Qt::Key_End), "End"},
                                  {int(Qt::Key_Left), "Left"}, {int(Qt::Key_Right), "Right"},
                                  {int(Qt::Key_Escape), "Escape"}}) {
        key(k);
        NOTHING_SELECTABLE(name);
    }
    c.clearCursor();
    for (const auto& [k, name] : {std::pair {int(Qt::Key_End), "End (no cursor)"},
                                  {int(Qt::Key_Home), "Home (no cursor)"}, {int(Qt::Key_Left), "Left (no cursor)"}}) {
        key(k);
        NOTHING_SELECTABLE(name);
    }
    // Hover over the plot centre.
    renderChart(c);
    const QPointF centre(c.width() / 2.0, c.height() / 2.0);
    {
        QMouseEvent move(QEvent::MouseMove, centre, c.mapToGlobal(centre), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&c, &move);
    }
    NOTHING_SELECTABLE("hover");
    {
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(&c, &leave);
    }
    QVERIFY(c.readoutText().isEmpty());

    // Recovery 1: widening the window makes the retained samples selectable.
    c.setWindowSeconds(300);
    c.refresh();
    QVERIFY(c.hasVisibleData());
    key(Qt::Key_Home);
    QString ro = c.readoutText();
    QVERIFY2(ro.contains(row(oldest - 50)) && std::fabs(ageOf(ro) - (162 - oldest)) <= 0.5, qPrintable(ro));
    QCOMPARE(c.accessibleDescription(), ro);
    key(Qt::Key_End);
    ro = c.readoutText();
    QVERIFY2(ro.contains(row(51)) && std::fabs(ageOf(ro) - 61) <= 0.5, qPrintable(ro));
    key(Qt::Key_Escape);
    QVERIFY(c.readoutText().isEmpty());

    // Recovery 2: back to 60 s, a fresh sample arrives at the current clock.
    c.setWindowSeconds(60);
    c.refresh();
    QVERIFY(!c.hasVisibleData());
    addPoint(h, 162, 5);
    c.refresh();
    QVERIFY(c.hasVisibleData());
    key(Qt::Key_Home); // the first visible sample is the fresh one
    ro = c.readoutText();
    QVERIFY2(ro.contains(row(5)) && ageOf(ro) <= 0.5, qPrintable(ro));
    key(Qt::Key_Left);
    QVERIFY2(c.readoutText().contains(row(5)), qPrintable(c.readoutText()));
    key(Qt::Key_End);
    QVERIFY2(c.readoutText().contains(row(5)), qPrintable(c.readoutText()));
    renderChart(c);
    QCOMPARE(c.lastPaintedPoints(), 1);
    c.clearCursor();
    renderChart(c);
    {
        QMouseEvent move(QEvent::MouseMove, centre, c.mapToGlobal(centre), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&c, &move);
    }
    ro = c.readoutText();
    QVERIFY2(ro.contains(row(5)), qPrintable(QStringLiteral("hover after recovery: \"%1\"").arg(ro)));
#undef NOTHING_SELECTABLE
#endif
}

void TestAdversarial::metricButtonSparklineTracksNegativeCelsius_data()
{
    QTest::addColumn<double>("from");
    QTest::addColumn<double>("to");
    QTest::newRow("control +1 -> +10") << 1.0 << 10.0;
    QTest::newRow("-10 -> -1 (cold sensor warming)") << -10.0 << -1.0;
    QTest::newRow("-40 -> -25 (outdoor probe)") << -40.0 << -25.0;
    QTest::newRow("-5 -> +5 (crosses zero)") << -5.0 << 5.0;
}

// A rising temperature must draw a rising sparkline whatever the sign of the
// readings: an auto-scaled range that starts at 0 turns every sub-zero
// sample into the baseline, i.e. a flat line that hides a 9 degree trend.
void TestAdversarial::metricButtonSparklineTracksNegativeCelsius()
{
#if !defined(PS_ADV_WIDGETS) || !defined(PS_ADV_METRICBUTTON)
    QSKIP("built without QtWidgets/MetricButton");
#else
    QFETCH(double, from);
    QFETCH(double, to);
    ps::History h(1000, 1e9);
    for (int i = 0; i < 60; ++i) {
        QVERIFY(h.beginSample(1.0 + i, true));
        h.set("t", "Sensor", ps::Unit::Celsius, from + (to - from) * i / 59.0);
        h.endSample();
    }
    MetricButton b(QStringLiteral("Temp"));
    b.setPalette(plainPalette());
    b.setValueText(QString::fromUtf8("%1 \u00b0C").arg(to));
    b.setDetailText(QStringLiteral("sensor"));
    b.resize(320, b.sizeHint().height());
    b.setSparkline(&h, {"t"}, 0, 60);
    QImage img(b.size(), QImage::Format_RGB32);
    img.fill(Qt::white);
    b.render(&img);

    // Topmost series pixel per column over the right-hand sparkline area.
    const int x0 = b.width() - 10 - 64;
    int top = INT_MAX, bottom = -1, n = 0;
    auto columnTop = [&](int x) {
        for (int y = 0; y < img.height(); ++y)
            if (seriesPixel(img.pixel(x, y)))
                return y;
        return -1;
    };
    for (int x = x0; x < b.width() - 8; ++x) {
        const int y = columnTop(x);
        if (y < 0)
            continue;
        ++n;
        top = std::min(top, y);
        bottom = std::max(bottom, y);
    }
    QVERIFY2(n > 20, "sparkline not drawn in the series colour");
    const QFontMetrics fm(b.font());
    const int sparkH = fm.height(); // the sparkline is at least one text line tall
    QVERIFY2(bottom - top >= sparkH / 3,
             qPrintable(QStringLiteral("sparkline flat: %1 -> %2 degC spans %3 px (expect >= %4 px); "
                                       "metricbutton.cpp paintEvent auto-scale starts hi at 0 and clamps v/hi")
                            .arg(from).arg(to).arg(bottom - top).arg(sparkH / 3)));
    int left = -1, right = -1;
    for (int x = x0; x < b.width() - 8 && left < 0; ++x)
        left = columnTop(x);
    for (int x = b.width() - 9; x >= x0 && right < 0; --x)
        right = columnTop(x);
    QVERIFY2(right < left, qPrintable(QStringLiteral("rising series not drawn rising: first y=%1 last y=%2")
                                          .arg(left).arg(right)));
#endif
}

// ===========================================================================
// CLI parsing and MainWindow (offscreen, own settings file, no host writes)

#ifdef PS_ADV_MAINWINDOW
namespace {

bool hasDigit(const QString& s)
{
    for (QChar c : s)
        if (c.isDigit())
            return true;
    return false;
}

MainWindowOptions uiOptions(const QTemporaryDir& d, bool collector = false)
{
    MainWindowOptions o;
    o.startCollector = collector;
    o.settingsFile = d.filePath(QStringLiteral("settings.ini"));
    o.initialIntervalMs = 500;
    return o;
}

ps::TemperatureReading tempReading(const std::string& key, const std::string& chip, const std::string& label, double c)
{
    ps::TemperatureReading t;
    t.key = key;
    t.chip = chip;
    t.label = label;
    t.displayName = chip + " " + label;
    t.path = "/sys/class/hwmon/" + chip + "/temp1_input";
    t.celsius = c;
    return t;
}

std::shared_ptr<ps::Snapshot> uiSnap(uint64_t seq, double t, bool continuous = true, int cores = 1)
{
    auto s = std::make_shared<ps::Snapshot>();
    s->sequence = seq;
    s->generation = 1;
    s->time = t;
    s->interval = 1;
    s->continuous = continuous;
    s->collectMs = 1;
    s->logicalCpus = cores;
    s->cpu.total = 10;
    s->cpu.user = 5;
    s->cpu.system = 5;
    s->cpu.iowait = 0;
    s->cpu.steal = 0;
    for (int i = 0; i < cores; ++i) {
        ps::CoreUsage c;
        c.index = i;
        c.online = true;
        c.usage = s->cpu;
        s->cores.push_back(c);
    }
    constexpr double GiB = 1024.0 * 1024 * 1024;
    s->memory.valid = true;
    s->memory.total = 8 * GiB;
    s->memory.used = 2 * GiB;
    s->memory.available = 6 * GiB;
    s->memory.free = 1 * GiB;
    s->memory.cache = 3 * GiB;
    s->memory.shared = 0;
    s->memory.hasSwap = false;
    s->memory.swapTotal = s->memory.swapUsed = s->memory.swapFree = 0;
    s->diskTotal = {"physical disks", 0, ps::kNaN, ps::kNaN};
    s->netTotal = {"all but loopback", 0, ps::kNaN, ps::kNaN};
    return s;
}

QTreeWidgetItem* itemWithKey(QTreeWidget* list, const QString& key)
{
    for (int i = 0; list && i < list->topLevelItemCount(); ++i)
        if (list->topLevelItem(i)->data(0, Qt::UserRole).toString() == key)
            return list->topLevelItem(i);
    return nullptr;
}

QString factText(const MainWindow& w, const char* id)
{
    const auto* l = w.findChild<QLabel*>(QString::fromLatin1(id));
    return l ? l->text() : QStringLiteral("<missing fact %1>").arg(QLatin1String(id));
}

// y of the first and last drawn sparkline columns (series colour 0), or -1.
std::pair<int, int> sparkEnds(MetricButton* b)
{
    QImage img(b->size(), QImage::Format_RGB32);
    img.fill(Qt::white);
    b->render(&img);
    auto columnTop = [&](int x) {
        for (int y = 0; y < img.height(); ++y)
            if (seriesPixel(img.pixel(x, y)))
                return y;
        return -1;
    };
    int first = -1, last = -1;
    for (int x = b->width() - 10 - 64; x < b->width() - 8 && first < 0; ++x)
        first = columnTop(x);
    for (int x = b->width() - 9; x >= b->width() - 10 - 64 && last < 0; --x)
        last = columnTop(x);
    return {first, last};
}

} // namespace
#endif

void TestAdversarial::cliRejectsMalformedNumbers_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("intervalMs"); // -1: rejected
    QTest::addColumn<int>("delayMs");    // -1: rejected
    QTest::newRow("0.5") << "0.5" << 500 << 500;
    QTest::newRow("1") << "1" << 1000 << 1000;
    QTest::newRow("padded 2") << " 2 " << 2000 << 2000;
    QTest::newRow("5.0") << "5.0" << 5000 << 5000;
    QTest::newRow("1e0") << "1e0" << 1000 << 1000;
    QTest::newRow("2.5e-1") << "2.5e-1" << -1 << 250;
    QTest::newRow("3 (not a choice)") << "3" << -1 << 3000;
    QTest::newRow("0") << "0" << -1 << 0;
    QTest::newRow("-0") << "-0" << -1 << 0;
    QTest::newRow("-1") << "-1" << -1 << -1;
    QTest::newRow("-0.5") << "-0.5" << -1 << -1;
    QTest::newRow("86400 (max)") << "86400" << -1 << 86400000;
    QTest::newRow("86400.001") << "86400.001" << -1 << -1;
    QTest::newRow("1e100") << "1e100" << -1 << -1;
    QTest::newRow("1e308") << "1e308" << -1 << -1;
    QTest::newRow("1e309 overflow") << "1e309" << -1 << -1;
    QTest::newRow("inf") << "inf" << -1 << -1;
    QTest::newRow("-inf") << "-inf" << -1 << -1;
    QTest::newRow("nan") << "nan" << -1 << -1;
    QTest::newRow("empty") << "" << -1 << -1;
    QTest::newRow("blank") << "   " << -1 << -1;
    QTest::newRow("abc") << "abc" << -1 << -1;
    QTest::newRow("unit suffix") << "1s" << -1 << -1;
    QTest::newRow("decimal comma") << "1,5" << -1 << -1;
    QTest::newRow("hex") << "0x10" << -1 << -1;
    QTest::newRow("two numbers") << "1 2" << -1 << -1;
    QTest::newRow("embedded NUL") << QString::fromUtf8("1\0" "0", 3) << -1 << -1;
}

void TestAdversarial::cliRejectsMalformedNumbers()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without app/cli.h");
#else
    QFETCH(QString, text);
    QFETCH(int, intervalMs);
    QFETCH(int, delayMs);
    const auto i = cli::parseIntervalMs(text);
    const auto d = cli::parseDelayMs(text);
    QCOMPARE(i ? *i : -1, intervalMs);
    QCOMPARE(d ? *d : -1, delayMs);
    // A caller-supplied bound can only tighten the limit, never lift it.
    QVERIFY(!cli::parseDelayMs(QStringLiteral("10"), 5));
    QVERIFY(!cli::parseDelayMs(QStringLiteral("1"), ps::kNaN));
    QVERIFY(!cli::parseDelayMs(QStringLiteral("1"), -1));
    QVERIFY(!cli::parseDelayMs(QStringLiteral("100000"), 1e9));
#endif
}

void TestAdversarial::hostRootFilesystemHasCapacity()
{
    // Production defaults on the real host (read-only: mountinfo + statvfs
    // of local mounts, no process scan). The row that contains "/" must carry
    // real capacity, whatever else shares its source (btrfs subvolumes).
    ps::SamplerOptions o;
    o.scanProcesses = false;
    o.fsProbeWaitMs = 3000;
    ps::Sampler smp(o);
    const ps::Filesystem* root = nullptr;
    ps::Snapshot s;
    for (int i = 0; i < 3; ++i) {
        s = smp.sample(ps::monotonicSeconds(), 1);
        root = nullptr;
        for (const auto& f : s.filesystems)
            for (const auto& mp : f.mountPoints)
                if (mp == "/")
                    root = &f;
        if (root && root->error.empty() && ps::isValid(root->totalBytes))
            break;
        QTest::qWait(300);
    }
    if (!root && s.filesystems.empty())
        QSKIP("no local filesystems visible (container)");
    QVERIFY2(root, "no filesystem row lists \"/\" among its mount points");
    QVERIFY2(root->error.empty() && ps::isValid(root->totalBytes) && root->totalBytes > 0,
             qPrintable(QStringLiteral("\"/\" (%1 %2) has no capacity: \"%3\"")
                            .arg(QString::fromStdString(root->fsType), QString::fromStdString(root->source),
                                 QString::fromStdString(root->error))));
    QVERIFY(root->usedBytes >= 0 && root->availBytes >= 0);
    QVERIFY(root->usedBytes <= root->totalBytes && root->availBytes <= root->totalBytes);
    qInfo("real root row: %s %s, %zu mount points, total=%.0f used=%.0f avail=%.0f", root->fsType.c_str(),
          root->source.c_str(), root->mountPoints.size(), root->totalBytes, root->usedBytes, root->availBytes);
}

void TestAdversarial::mainWindowStaleThenPauseDoesNotResurrect()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    // A real collector on a fixture tree whose /proc/stat is a FIFO: gating
    // the writer blocks the collector thread, so the window goes stale.
    // Pausing a stale window, or the collector recovering while paused, must
    // never present the old facts as current; only a new sample may.
    FixtureRoot r;
    QVERIFY(r.ok());
    writeBaseTree(r);
    r.remove("proc/stat");
    const std::string fifo = r.path("proc/stat");
    QCOMPARE(::mkfifo(fifo.c_str(), 0600), 0);
    r.write("proc/self/mountinfo", mi(20, 1, "8:1", "/", "/", "rw,relatime", "ext4", "/dev/sda1"));
    r.write("proc/mounts", "/dev/sda1 / ext4 rw 0 0\n");
    r.write("sys/class/hwmon/hwmon0/name", "k10temp\n");
    r.write("sys/class/hwmon/hwmon0/temp1_input", "45000\n");
    r.write("sys/class/hwmon/hwmon0/temp1_label", "Tctl\n");
    writeProc(r, 400, pidStat(400, "worker", 100, 50, 500), std::string("worker\0--x\0", 11));
    StatFifoServer server(fifo);
    QTemporaryDir d;
    auto o = uiOptions(d, true);
    o.sampler = fixtureOptions(r, true);
    o.sampler.statFs = [](const std::string&, int& err) -> std::optional<ps::FsStats> {
        err = 0;
        return ps::FsStats{1000000, 500000, 400000, 4096};
    };
    MainWindow w(o);
    struct Ungate {
        StatFifoServer& s;
        ~Ungate() { s.setGated(false); } // before ~MainWindow joins the collector
    } ungate{server};
    w.showPage(MainWindow::ProcessPage);
    auto* fs = w.findChild<QTreeWidget*>(QStringLiteral("filesystemTable"));
    auto* sensors = w.findChild<QTreeWidget*>(QStringLiteral("sensorList"));
    auto* procs = w.findChild<QTableView*>(QStringLiteral("processTable"));
    auto* cpuChart = w.findChild<TimeChart*>(QStringLiteral("cpuChart"));
    auto* procCount = w.findChild<QLabel*>(QStringLiteral("processCount"));
    QVERIFY(fs && sensors && procs && cpuChart && procCount);
    auto procCell = [&](int column) {
        const auto* m = procs->model();
        for (int row = 0; row < m->rowCount(); ++row)
            if (m->data(m->index(row, ProcessModel::Pid)).toInt() == 400)
                return m->data(m->index(row, column)).toString();
        return QStringLiteral("<no row>");
    };
    auto fsCell = [&](int c) { return fs->topLevelItemCount() ? fs->topLevelItem(0)->text(c) : QStringLiteral("<no row>"); };
    auto sensorCell = [&] { return sensors->topLevelItemCount() ? sensors->topLevelItem(0)->text(1) : QStringLiteral("<no row>"); };

    QTRY_VERIFY2_WITH_TIMEOUT(w.snapshotsApplied() >= 3 && hasDigit(fsCell(3)) && hasDigit(sensorCell())
                                  && hasDigit(procCell(ProcessModel::Memory))
                                  && hasDigit(w.findChild<MetricButton*>(QStringLiteral("navCpu"))->valueText()),
                              qPrintable(QStringLiteral("fresh view never populated: fs=%1 sensor=%2 rss=%3")
                                             .arg(fsCell(3), sensorCell(), procCell(ProcessModel::Memory))),
                              8000);

    auto violations = [&] {
        QStringList v;
        for (auto* b : w.findChildren<MetricButton*>())
            if (hasDigit(b->valueText()))
                v << QStringLiteral("nav %1=\"%2\"").arg(b->objectName(), b->valueText());
        for (const char* id : {"cpu_total", "cpu_load", "mem_total", "mem_used", "mem_available"})
            if (hasDigit(factText(w, id)))
                v << QStringLiteral("fact %1=\"%2\"").arg(QLatin1String(id), factText(w, id));
        for (int c = 3; c <= 6; ++c)
            if (hasDigit(fsCell(c)))
                v << QStringLiteral("filesystem col %1=\"%2\"").arg(c).arg(fsCell(c));
        if (hasDigit(sensorCell()))
            v << QStringLiteral("sensor=\"%1\"").arg(sensorCell());
        for (int c : {int(ProcessModel::Cpu), int(ProcessModel::Memory), int(ProcessModel::Threads), int(ProcessModel::Age)})
            if (hasDigit(procCell(c)))
                v << QStringLiteral("process col %1=\"%2\"").arg(c).arg(procCell(c));
        if (cpuChart->notice().isEmpty())
            v << QStringLiteral("cpu chart has no stale/paused notice");
        return v.join(QStringLiteral("; "));
    };

    server.setGated(true);
    QTRY_VERIFY_WITH_TIMEOUT(w.isStale(), 6000);
    QVERIFY2(violations().isEmpty(), qPrintable(QStringLiteral("stale: ") + violations()));
    QVERIFY2(procCount->text().contains(QStringLiteral("No recent samples")), qPrintable(procCount->text()));

    const int applied = w.snapshotsApplied();
    w.setPaused(true);
    QTest::qWait(700);
    QVERIFY2(w.isStale(), "pausing cleared staleness without a new sample");
    QVERIFY2(violations().isEmpty(), qPrintable(QStringLiteral("stale->paused: ") + violations()));
    QVERIFY2(w.statusText().contains(QStringLiteral("unavailable")), qPrintable(w.statusText()));

    server.setGated(false); // the collector recovers while the view is paused
    QTest::qWait(1500);
    QCOMPARE(w.snapshotsApplied(), applied);
    QVERIFY(w.isStale());
    QVERIFY2(violations().isEmpty(), qPrintable(QStringLiteral("paused, collector recovered: ") + violations()));

    w.setPaused(false);
    QTRY_VERIFY_WITH_TIMEOUT(!w.isStale() && w.snapshotsApplied() > applied, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(hasDigit(sensorCell()) && hasDigit(fsCell(3)), 3000);
    QVERIFY(hasDigit(factText(w, "mem_used")));
#endif
}

void TestAdversarial::mainWindowKnownSwapTotalUnknownFree_data()
{
    QTest::addColumn<QByteArray>("swapLines");
    QTest::addColumn<bool>("configured");
    QTest::newRow("SwapFree missing") << QByteArray("SwapTotal: 4194304 kB\n") << true;
    QTest::newRow("SwapFree garbage") << QByteArray("SwapTotal: 4194304 kB\nSwapFree: 12x kB\n") << true;
    QTest::newRow("SwapFree > SwapTotal") << QByteArray("SwapTotal: 4194304 kB\nSwapFree: 5000000 kB\n") << true;
    QTest::newRow("control: SwapTotal 0") << QByteArray("SwapTotal: 0 kB\nSwapFree: 0 kB\n") << false;
}

void TestAdversarial::mainWindowKnownSwapTotalUnknownFree()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    // End to end: meminfo -> Sampler -> MainWindow. A known positive
    // SwapTotal with an unusable SwapFree is "swap exists, use unknown";
    // only a measured SwapTotal of 0 is "no swap configured".
    QFETCH(QByteArray, swapLines);
    QFETCH(bool, configured);
    FixtureRoot r;
    QVERIFY(r.ok());
    writeBaseTree(r);
    r.write("proc/meminfo", "MemTotal: 8388608 kB\nMemFree: 1048576 kB\nMemAvailable: 6291456 kB\n"
                                + swapLines.toStdString());
    ps::Sampler smp(fixtureOptions(r));
    smp.sample(100, 1);
    const auto s = smp.sample(101, 1);
    QVERIFY(s.memory.valid);
    QTemporaryDir d;
    MainWindow w(uiOptions(d));
    w.applySnapshot(std::make_shared<ps::Snapshot>(s));
    const QString nav = w.findChild<MetricButton*>(QStringLiteral("navMemory"))->detailText();
    const QString total = factText(w, "swap_total");
    const QString used = factText(w, "swap_used");
    const QString what = QStringLiteral("nav detail=\"%1\" swap total=\"%2\" swap used=\"%3\"").arg(nav, total, used);
    if (configured) {
        QCOMPARE(s.memory.swapTotal, 4194304.0 * 1024);
        QVERIFY2(s.memory.hasSwap, "positive SwapTotal parsed as no swap");
        QVERIFY2(!ps::isValid(s.memory.swapUsed), "swap used invented from an unusable SwapFree");
        QVERIFY2(!nav.contains(QStringLiteral("no swap"), Qt::CaseInsensitive), qPrintable(what));
        QVERIFY2(!total.contains(QStringLiteral("None")) && total.contains(QStringLiteral("GiB")), qPrintable(what));
        QVERIFY2(!used.contains(QStringLiteral("None")) && !hasDigit(used), qPrintable(what));
    } else {
        QVERIFY(!s.memory.hasSwap);
        QVERIFY2(total.contains(QStringLiteral("None")), qPrintable(what));
        QVERIFY2(nav.contains(QStringLiteral("no swap")), qPrintable(what));
    }
#endif
}

void TestAdversarial::mainWindowUnpluggedSensorKeepsBoundedPlaceholder()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    const std::string A = "hwmon:k10temp:temp1", B = "hwmon:nvme:temp1";
    const QString keyA = QString::fromStdString(A);
    QTemporaryDir d;
    MainWindow w(uiOptions(d));
    auto* list = w.findChild<QTreeWidget*>(QStringLiteral("sensorList"));
    auto* chart = w.findChild<TimeChart*>(QStringLiteral("temperatureChart"));
    QVERIFY(list && chart);
    uint64_t seq = 0;
    auto feed = [&](double t, bool withA, bool continuous = true) {
        auto s = uiSnap(++seq, t, continuous);
        if (withA)
            s->temperatures.push_back(tempReading(A, "k10temp", "Tctl", 40 + (t > 8 ? 1 : 0)));
        s->temperatures.push_back(tempReading(B, "nvme", "Composite", 50));
        w.applySnapshot(s);
    };
    for (int t = 1; t <= 5; ++t)
        feed(t, true);
    QVERIFY(itemWithKey(list, keyA));
    QCOMPARE(itemWithKey(list, keyA)->checkState(0), Qt::Checked);

    for (int t = 6; t <= 8; ++t)
        feed(t, false); // unplugged
    auto* a = itemWithKey(list, keyA);
    QVERIFY2(a, "unplugged sensor vanished from the list while its history is retained");
    QCOMPARE(a->text(0), QStringLiteral("k10temp Tctl"));
    QVERIFY2(!a->text(1).isEmpty() && !hasDigit(a->text(1)), qPrintable(a->text(1)));
    QCOMPARE(a->checkState(0), Qt::Checked);
    bool plotted = false;
    for (const auto& cs : chart->series())
        plotted = plotted || cs.key == ps::keys::temperature(A);
    QVERIFY2(plotted, "selected unplugged sensor dropped from the chart (its history line would disappear)");
    const auto* series = w.history().find(ps::keys::temperature(A));
    QVERIFY(series);
    QVERIFY(!ps::isValid(w.history().latestValue(ps::keys::temperature(A))));
    QCOMPARE(w.history().value(*series, w.history().size() - 4), 40.0); // t=5 still retained
    chart->setCursorTime(8);
    const QStringList lines = chart->readoutText().split(QLatin1Char('\n'));
    for (const QString& line : lines)
        if (line.startsWith(QStringLiteral("k10temp Tctl")))
            QVERIFY2(!hasDigit(line.mid(line.indexOf(QLatin1Char(':')))),
                     qPrintable(QStringLiteral("gap bridged in readout: ") + line));

    feed(9, true); // replugged: still selected, live again
    a = itemWithKey(list, keyA);
    QVERIFY(a && a->checkState(0) == Qt::Checked && hasDigit(a->text(1)));

    feed(10, false);
    // Bounded: once retention (900 s) has passed, the placeholder must go.
    feed(1000, false, false);
    feed(1001, false);
    QVERIFY2(!w.history().find(ps::keys::temperature(A)), "expired sensor series still retained");
    QVERIFY2(!itemWithKey(list, keyA),
             qPrintable(QStringLiteral("placeholder for an expired sensor is kept forever (%1 rows)")
                            .arg(list->topLevelItemCount())));
    QCOMPARE(list->topLevelItemCount(), 1);
#endif
}

void TestAdversarial::mainWindowTemperatureNavSingleSource()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    // The user plots only a cooling CPU sensor while an NVMe drive heats up
    // and becomes the hottest sensor. The nav entry's number, name and trend
    // must all describe one sensor: 70 degrees, the NVMe name and a rising line
    // (the CPU line falls, so any mix-up flips the trend).
    const std::string cpu = "hwmon:k10temp:temp1", nvme = "hwmon:nvme:temp1";
    QTemporaryDir d;
    MainWindow w(uiOptions(d));
    w.setPalette(plainPalette());
    w.resize(1180, 780);
    w.show();
    auto* list = w.findChild<QTreeWidget*>(QStringLiteral("sensorList"));
    auto* chart = w.findChild<TimeChart*>(QStringLiteral("temperatureChart"));
    auto* nav = w.findChild<MetricButton*>(QStringLiteral("navTemperatures"));
    QVERIFY(list && chart && nav);
    for (int i = 0; i <= 30; ++i) {
        auto s = uiSnap(uint64_t(i + 1), 1.0 + i);
        s->temperatures.push_back(tempReading(cpu, "k10temp", "Tctl", 50 - i / 3.0));
        s->temperatures.push_back(tempReading(nvme, "nvme", "Composite", 45 + i * 25 / 30.0));
        w.applySnapshot(s);
        if (i == 0) {
            auto* item = itemWithKey(list, QString::fromStdString(nvme));
            QVERIFY(item);
            item->setCheckState(0, Qt::Unchecked);
        }
    }
    QCOMPARE(chart->series().size(), std::size_t(1));
    QCOMPARE(chart->series().front().key, ps::keys::temperature(cpu));
    const QString what = QStringLiteral("value=\"%1\" detail=\"%2\"").arg(nav->valueText(), nav->detailText());
    QVERIFY2(nav->valueText().contains(QStringLiteral("70")), qPrintable(what));
    QVERIFY2(nav->detailText().contains(QStringLiteral("nvme Composite")), qPrintable(what));
    QVERIFY2(nav->toolTip().contains(QStringLiteral("70")) && nav->toolTip().contains(QStringLiteral("nvme Composite")),
             qPrintable(nav->toolTip()));
    const auto [first, last] = sparkEnds(nav);
    QVERIFY2(first >= 0 && last >= 0, "temperature nav sparkline not drawn");
    QVERIFY2(last < first, qPrintable(QStringLiteral("nav trend does not follow the NVMe sensor it names: first y=%1 "
                                                     "last y=%2 (rising expected)")
                                          .arg(first)
                                          .arg(last)));
#endif
}

void TestAdversarial::mainWindowElidedIdentitiesHaveFullTooltips()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    // Narrow columns elide names and rates; the full identity and value must
    // stay reachable (tooltip or user-resizable column), without restyling.
    const std::string longName = "Composite sensor behind PCIe switch 0000:41:00.0";
    const std::string source = "/dev/mapper/luks-0123456789abcdef0123456789abcdef";
    const std::string longCmd = "/usr/lib/very/long/path/to/some-daemon --config=/etc/some-daemon/"
                                "extremely-long-configuration-file-name.conf --verbose";
    QTemporaryDir d;
    MainWindow w(uiOptions(d));
    w.resize(1180, 780);
    w.show();
    for (int i = 1; i <= 2; ++i) {
        auto s = uiSnap(uint64_t(i), i);
        s->temperatures.push_back(tempReading("hwmon:nvme:temp1", "nvme", longName, 55));
        ps::Filesystem f;
        f.source = source;
        f.fsType = "btrfs";
        f.mountPoints = {"/", "/home/some/deeply/nested/mount/point/for/testing"};
        f.totalBytes = 1e12;
        f.usedBytes = 4e11;
        f.availBytes = 5e11;
        s->filesystems.push_back(f);
        ps::NetIo n;
        n.name = "eth0";
        n.rxBps = 987.6 * 1024 * 1024;
        n.txBps = 876.5 * 1024 * 1024;
        s->interfaces.push_back(n);
        s->netTotal = {"all but loopback", 1, n.rxBps, n.txBps};
        s->processesScanned = true;
        ps::ProcessInfo p;
        p.pid = 400;
        p.ppid = 1;
        p.startTicks = 500;
        p.uidKnown = true;
        p.user = "a-rather-long-directory-user-name";
        p.name = "some-daemon";
        p.command = longCmd;
        p.threads = 1;
        s->processes.push_back(p);
        w.applySnapshot(s);
    }
    QStringList problems;

    auto* sensors = w.findChild<QTreeWidget*>(QStringLiteral("sensorList"));
    QVERIFY(sensors && sensors->topLevelItemCount() == 1);
    auto* split = qobject_cast<QSplitter*>(sensors->parentWidget());
    if (!split || split->childrenCollapsible())
        problems << QStringLiteral("sensor list is not in a non-collapsible splitter");
    const QString fullName = QString::fromStdString("nvme " + longName);
    if (!sensors->topLevelItem(0)->toolTip(0).contains(fullName))
        problems << QStringLiteral("sensor name tooltip lacks the full name: \"%1\"")
                        .arg(sensors->topLevelItem(0)->toolTip(0));

    auto* fs = w.findChild<QTreeWidget*>(QStringLiteral("filesystemTable"));
    QVERIFY(fs && fs->topLevelItemCount() == 1);
    const QString src = QString::fromStdString(source);
    if (QFontMetrics(fs->font()).horizontalAdvance(src) > fs->columnWidth(1)
        && !fs->topLevelItem(0)->toolTip(1).contains(src))
        problems << QStringLiteral("elided filesystem device has no full tooltip: \"%1\"")
                        .arg(fs->topLevelItem(0)->toolTip(1));
    for (int c = 0; c < fs->columnCount(); ++c)
        if (fs->header()->sectionResizeMode(c) == QHeaderView::Fixed)
            problems << QStringLiteral("filesystem column %1 not resizable").arg(c);

    auto* netNav = w.findChild<MetricButton*>(QStringLiteral("navNetwork"));
    QVERIFY(netNav);
    const QString value = netNav->valueText();
    const int up = value.indexOf(QChar(0x2191));
    const QString tx = up >= 0 ? value.mid(up + 1).trimmed() : QString();
    const QString rx = up >= 0 ? value.mid(value.indexOf(QChar(0x2193)) + 1, up - 1).trimmed() : QString();
    if (tx.isEmpty() || !netNav->toolTip().contains(tx) || !netNav->toolTip().contains(rx)
        || !netNav->accessibleDescription().contains(tx))
        problems << QStringLiteral("network nav full rates not in tooltip/description: value=\"%1\" tip=\"%2\"")
                        .arg(value, netNav->toolTip());

    auto* procs = w.findChild<QTableView*>(QStringLiteral("processTable"));
    QVERIFY(procs);
    for (int c : {int(ProcessModel::Name), int(ProcessModel::User), int(ProcessModel::Command)})
        if (procs->horizontalHeader()->sectionResizeMode(c) == QHeaderView::Fixed)
            problems << QStringLiteral("process column %1 not resizable").arg(c);
    w.showPage(MainWindow::ProcessPage);
    const auto* m = procs->model();
    QVERIFY(m->rowCount() == 1);
    if (m->data(m->index(0, ProcessModel::Command), Qt::ToolTipRole).toString() != QString::fromStdString(longCmd))
        problems << QStringLiteral("command tooltip is not the full command line");
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
#endif
}

void TestAdversarial::mainWindowFactGridsVisibleAboveCharts_data()
{
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("height");
    QTest::addColumn<double>("fontScale");
    QTest::newRow("1180x780") << 1180 << 780 << 1.0;
    QTest::newRow("852x600") << 852 << 600 << 1.0;
    QTest::newRow("1180x780 font130") << 1180 << 780 << 1.3;
    QTest::newRow("900x640 font130") << 900 << 640 << 1.3;
}

void TestAdversarial::mainWindowFactGridsVisibleAboveCharts()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    // The "Name: value" fact rows above the CPU/Memory/Disks/Network charts
    // must actually be on screen in a real-sized shown window: the container
    // itself visible with room for every row it arranges, each name/value
    // label visible and inside it, and nothing covered by the next widget
    // (the chart). Per-label clipping checks miss a zero-height parent.
    QFETCH(int, width);
    QFETCH(int, height);
    QFETCH(double, fontScale);
    const QFont appFont = QApplication::font();
    struct Restore {
        QFont f;
        ~Restore() { QApplication::setFont(f); }
    } restore {appFont};
    if (fontScale != 1.0) {
        QFont f = appFont;
        f.setPointSizeF(appFont.pointSizeF() * fontScale);
        QApplication::setFont(f);
    }
    QTemporaryDir d;
    MainWindow w(uiOptions(d));
    w.resize(width, height);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    for (int i = 1; i <= 2; ++i)
        w.applySnapshot(uiSnap(uint64_t(i), i, true, 4));

    struct PageFacts { MainWindow::Page page; const char* name; const char* firstFact; int pairs; };
    const PageFacts pages[] = {{MainWindow::CpuPage, "CPU", "cpu_total", 8},
                               {MainWindow::MemoryPage, "Memory", "mem_total", 8},
                               {MainWindow::DiskPage, "Disks", "disk_read", 4},
                               {MainWindow::NetworkPage, "Network", "net_rx", 4}};
    QStringList problems;
    for (const auto& pf : pages) {
        w.showPage(pf.page);
        for (int k = 0; k < 3; ++k) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QCoreApplication::processEvents();
        }
        const QString where = QStringLiteral("%1 page at %2x%3 font x%4").arg(QLatin1String(pf.name)).arg(width).arg(height).arg(fontScale);
        auto* first = w.findChild<QLabel*>(QLatin1String(pf.firstFact));
        if (!first || !first->parentWidget()) {
            problems << where + QStringLiteral(": fact %1 missing").arg(QLatin1String(pf.firstFact));
            continue;
        }
        QWidget* grid = first->parentWidget();
        QWidget* page = grid->parentWidget();
        const QRect g = grid->geometry();
        if (!grid->isVisible())
            problems << where + QStringLiteral(": fact container hidden");
        const int needed = grid->hasHeightForWidth() ? grid->heightForWidth(g.width()) : grid->sizeHint().height();
        const auto labels = grid->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
        if (labels.size() != 2 * pf.pairs)
            problems << where + QStringLiteral(": %1 labels in the fact container, expected %2").arg(labels.size()).arg(2 * pf.pairs);
        int rowH = 0;
        for (auto* l : labels)
            rowH = std::max(rowH, l->sizeHint().height());
        if (g.height() <= 0 || g.height() < needed || g.height() < rowH)
            problems << where + QStringLiteral(": fact container %1x%2 at y=%3, needs height %4 (one row %5)")
                                    .arg(g.width()).arg(g.height()).arg(g.y()).arg(needed).arg(rowH);
        // The widget laid out after the grid (the chart) must not cover it.
        QWidget* next = nullptr;
        if (auto* lay = page ? page->layout() : nullptr) {
            const int at = lay->indexOf(grid);
            for (int j = at + 1; at >= 0 && j < lay->count() && !next; ++j)
                if (auto* nw = lay->itemAt(j)->widget(); nw && nw->isVisible())
                    next = nw;
        }
        if (!next)
            problems << where + QStringLiteral(": no visible widget after the fact container");
        else if (next->geometry().top() <= g.bottom())
            problems << where + QStringLiteral(": %1 (y=%2) overlaps fact container [%3..%4]")
                                    .arg(next->objectName().isEmpty() ? QString::fromLatin1(next->metaObject()->className()) : next->objectName())
                                    .arg(next->geometry().top()).arg(g.top()).arg(g.bottom());
        int hiddenLabels = 0, outside = 0, squashed = 0;
        QString example;
        for (auto* l : labels) {
            const QRect r = l->geometry();
            const bool bad = !l->isVisible() || !grid->rect().contains(r) || r.height() < l->fontMetrics().height()
                             || r.width() < l->fontMetrics().horizontalAdvance(l->text());
            hiddenLabels += !l->isVisible();
            outside += l->isVisible() && !grid->rect().contains(r);
            squashed += l->isVisible() && (r.height() < l->fontMetrics().height()
                                           || r.width() < l->fontMetrics().horizontalAdvance(l->text()));
            if (bad && example.isEmpty())
                example = QStringLiteral("\"%1\" %2x%3+%4+%5").arg(l->text()).arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y());
        }
        if (hiddenLabels || outside || squashed)
            problems << where + QStringLiteral(": fact labels hidden %1, outside container %2, too small %3 (e.g. %4)")
                                    .arg(hiddenLabels).arg(outside).arg(squashed).arg(example);
        if (first->visibleRegion().isEmpty())
            problems << where + QStringLiteral(": %1 has no visible region").arg(QLatin1String(pf.firstFact));
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n  "))));
#endif
}

void TestAdversarial::mainWindowReportsHistoryTruncation()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    constexpr int kCores = 300;
    const std::size_t over = kCores - ps::History::groupQuota(ps::SeriesGroup::Core);
    QTemporaryDir d;
    MainWindow w(uiOptions(d));
    w.applySnapshot(uiSnap(1, 1, false, kCores));
    w.applySnapshot(uiSnap(2, 2, true, kCores));
    QCOMPARE(w.history().refusedInLastSample(ps::SeriesGroup::Core), over);
    const QString expected = QStringLiteral("%1 CPU cores").arg(over);
    QVERIFY2(w.statusText().contains(QStringLiteral("History limit")) && w.statusText().contains(expected),
             qPrintable(w.statusText()));
    QVERIFY(ps::isValid(w.history().latestValue(ps::keys::cpuTotal)));
    // Back under the quota: the notice must clear rather than stick.
    w.applySnapshot(uiSnap(3, 3, true, 8));
    QVERIFY2(!w.statusText().contains(QStringLiteral("History limit")), qPrintable(w.statusText()));
#endif
}

void TestAdversarial::mainWindowCsvExportNeverBlocksUi()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    // Injected writer (current public MainWindowOptions::csvWriter API) that
    // blocks like a hung destination. Nothing is written to disk.
    struct Gate {
        std::mutex m;
        std::condition_variable cv;
        bool open = false;
        std::atomic<int> entered{0}, left{0};
        std::vector<std::string> data;
        void release()
        {
            std::lock_guard l(m);
            open = true;
            cv.notify_all();
        }
    };
    auto gate = std::make_shared<Gate>();
    auto writer = [gate](const std::string& data, const QString&, QString*) {
        ++gate->entered;
        std::unique_lock l(gate->m);
        gate->data.push_back(data);
        gate->cv.wait(l, [&] { return gate->open; });
        ++gate->left;
        return true;
    };
    QTemporaryDir d;
    const QString path = d.filePath(QStringLiteral("never-written.csv"));
    {
        auto o = uiOptions(d);
        o.csvWriter = writer;
        MainWindow w(o);
        QSignalSpy done(&w, &MainWindow::csvExportFinished);
        for (int i = 1; i <= 3; ++i)
            w.applySnapshot(uiSnap(uint64_t(i), i, i > 1));
        QElapsedTimer t;
        t.start();
        QString err;
        QVERIFY2(w.startCsvExport(path, &err), qPrintable(err));
        QVERIFY2(t.elapsed() < 500, "startCsvExport blocked the UI thread");
        QTRY_COMPARE_WITH_TIMEOUT(gate->entered.load(), 1, 2000);
        QVERIFY(w.isExporting());
        w.applySnapshot(uiSnap(4, 4)); // UI keeps sampling while the writer hangs
        QCOMPARE(w.snapshotsApplied(), 4);
        QString err2;
        QVERIFY2(!w.startCsvExport(path, &err2), "second concurrent export accepted");
        QVERIFY(!err2.isEmpty());
        QCOMPARE(gate->entered.load(), 1);
        gate->release();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
        QCOMPARE(done.first().at(0).toBool(), true);
        QVERIFY(!w.isExporting());
        // The job exported a private copy: 3 samples, not the 4th added later.
        QVERIFY(w.startCsvExport(path, &err));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 3000);
        auto rows = [](const std::string& s) { return std::count(s.begin(), s.end(), '\n'); };
        QCOMPARE(rows(gate->data.at(1)) - rows(gate->data.at(0)), std::ptrdiff_t(1));
        QVERIFY(!QFile::exists(path));
    }
    // Shutdown while the destination hangs is bounded, and emits nothing.
    auto gate2 = std::make_shared<Gate>();
    {
        auto o = uiOptions(d);
        o.csvWriter = [gate2](const std::string& data, const QString& p, QString* e) {
            ++gate2->entered;
            std::unique_lock l(gate2->m);
            gate2->cv.wait(l, [&] { return gate2->open; });
            ++gate2->left;
            (void)data, (void)p, (void)e;
            return true;
        };
        MainWindow w(o);
        QSignalSpy done(&w, &MainWindow::csvExportFinished);
        w.applySnapshot(uiSnap(1, 1, false));
        QVERIFY(w.startCsvExport(path));
        QTRY_COMPARE_WITH_TIMEOUT(gate2->entered.load(), 1, 2000);
        QElapsedTimer t;
        t.start();
        QVERIFY2(!w.shutdown(500), "shutdown claims the hung export finished");
        QVERIFY2(t.elapsed() < 1500, qPrintable(QStringLiteral("shutdown took %1 ms").arg(t.elapsed())));
        QCoreApplication::processEvents();
        QCOMPARE(done.count(), 0);
    }
    gate2->release(); // let the abandoned job end before the test does
    QTRY_COMPARE_WITH_TIMEOUT(gate2->left.load(), 1, 2000);
#endif
}

// ===========================================================================
// RC-3: a frame's own acquisition time decides whether it is current

#ifdef PS_ADV_MAINWINDOW
namespace {

// The window's stale limit for an interval (MainWindow::checkStale).
double staleLimitSeconds(int intervalMs)
{
    return std::max(2.5 * intervalMs, intervalMs + 1000.0) / 1000.0;
}

// Whenever the window presents itself as live (not stale, not paused) after
// applying a frame, the newest recorded frame must have been acquired within
// the stale limit of the current CLOCK_BOOTTIME. 250 ms covers event latency.
struct FreshnessMonitor {
    QStringList violations;
    int checks = 0;
};

void watchFreshness(MainWindow& w, FreshnessMonitor& m, int intervalMs)
{
    const double limit = staleLimitSeconds(intervalMs);
    QObject::connect(&w, &MainWindow::snapshotApplied, &w, [&w, &m, limit] {
        ++m.checks;
        if (w.isStale() || w.isPaused() || w.history().empty())
            return;
        const double age = ps::monotonicSeconds() - w.history().latestTime();
        if (age > limit + 0.25)
            m.violations << QStringLiteral("live view after applying a frame acquired %1 s ago (limit %2 s), status "
                                           "\"%3\"")
                                .arg(age, 0, 'f', 2)
                                .arg(limit, 0, 'f', 2)
                                .arg(w.statusText());
    });
}

// Values that must not be presented while the window is stale.
QString staleLeaks(MainWindow& w)
{
    QStringList v;
    for (auto* b : w.findChildren<MetricButton*>())
        if (hasDigit(b->valueText()))
            v << QStringLiteral("nav %1=\"%2\"").arg(b->objectName(), b->valueText());
    for (const char* id : {"cpu_total", "cpu_load", "mem_total", "mem_used", "mem_available"})
        if (hasDigit(factText(w, id)))
            v << QStringLiteral("fact %1=\"%2\"").arg(QLatin1String(id), factText(w, id));
    if (auto* fs = w.findChild<QTreeWidget*>(QStringLiteral("filesystemTable")))
        for (int i = 0; i < fs->topLevelItemCount(); ++i)
            for (int c = 3; c <= 6; ++c)
                if (hasDigit(fs->topLevelItem(i)->text(c)))
                    v << QStringLiteral("filesystem row %1 col %2=\"%3\"").arg(i).arg(c).arg(fs->topLevelItem(i)->text(c));
    if (auto* sl = w.findChild<QTreeWidget*>(QStringLiteral("sensorList")))
        for (int i = 0; i < sl->topLevelItemCount(); ++i)
            if (hasDigit(sl->topLevelItem(i)->text(1)))
                v << QStringLiteral("sensor row %1=\"%2\"").arg(i).arg(sl->topLevelItem(i)->text(1));
    if (auto* c = w.findChild<TimeChart*>(QStringLiteral("cpuChart")); c && c->notice().isEmpty())
        v << QStringLiteral("cpu chart has no stale notice");
    if (!w.statusText().contains(QStringLiteral("unavailable")))
        v << QStringLiteral("status \"%1\"").arg(w.statusText());
    return v.join(QStringLiteral("; "));
}

// Age stated by a chart readout's first line: "N s ago" anywhere in it (for
// example "20 s ago" or "Latest sample (20 s ago)"), else -1 for a plain
// "Latest sample", else NaN. The caption wording is not prescribed.
double readoutAgo(const QString& readout)
{
    const QString first = readout.section(QLatin1Char('\n'), 0, 0);
    const auto m = QRegularExpression(QStringLiteral("(\\d+(?:\\.\\d+)?) s ago")).match(first);
    if (m.hasMatch())
        return m.captured(1).toDouble();
    return first.startsWith(QStringLiteral("Latest sample")) ? -1 : ps::kNaN;
}

struct Ungate {
    StatFifoServer& s;
    ~Ungate() { s.setGated(false); } // before ~MainWindow joins the collector
};

} // namespace
#endif

void TestAdversarial::mainWindowOldFrameKeepsStaleAndChartClock()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    // A real collector that never completes a sample (its /proc/stat is a
    // FIFO with no writer), so the window goes stale on its own. Frames are
    // then delivered directly with known acquisition times. A frame acquired
    // 20 s ago is history, not current data: it must not clear the stale
    // state, the charts' "now" stays the current boot-time clock, and it is
    // recorded (if at all) at its own time, never restamped.
    FixtureRoot r;
    QVERIFY(r.ok());
    writeBaseTree(r);
    r.remove("proc/stat");
    const std::string fifo = r.path("proc/stat");
    QCOMPARE(::mkfifo(fifo.c_str(), 0600), 0);
    StatFifoServer server(fifo);
    server.setGated(true);
    QTemporaryDir d;
    auto o = uiOptions(d, true);
    o.sampler = fixtureOptions(r);
    MainWindow w(o);
    Ungate ungate{server};
    auto* cpuChart = w.findChild<TimeChart*>(QStringLiteral("cpuChart"));
    auto* sensors = w.findChild<QTreeWidget*>(QStringLiteral("sensorList"));
    QVERIFY(cpuChart && sensors);
    FreshnessMonitor mon;
    watchFreshness(w, mon, 500);
    auto frame = [](uint64_t seq, double t, bool continuous, double usedGiB = 2) {
        auto s = uiSnap(seq, t, continuous);
        s->memory.used = usedGiB * 1024.0 * 1024 * 1024;
        s->memory.available = s->memory.total - s->memory.used;
        s->temperatures.push_back(tempReading("hwmon0/temp1", "k10temp", "Tctl", 45));
        return s;
    };

    QTRY_VERIFY_WITH_TIMEOUT(w.isStale(), 8000);
    QCOMPARE(w.snapshotsApplied(), 0);

    const double now = ps::monotonicSeconds();
    const double old1 = now - 20.5, old2 = now - 20.0;
    w.applySnapshot(frame(1, old1, false));
    w.applySnapshot(frame(2, old2, true));
    QVERIFY2(w.isStale(), qPrintable(QStringLiteral("frames acquired 20 s ago cleared the stale state; status \"%1\"")
                                         .arg(w.statusText())));
    QString leaks = staleLeaks(w);
    QVERIFY2(leaks.isEmpty(), qPrintable(QStringLiteral("old frame shown as current: ") + leaks));
    const auto& h = w.history();
    if (!h.empty())
        QCOMPARE(h.latestTime(), old2); // its own acquisition time, not the receipt time
    cpuChart->setCursorTime(old2);
    const QString ro = cpuChart->readoutText();
    cpuChart->clearCursor();
    if (!h.empty()) {
        const double ago = readoutAgo(ro);
        QVERIFY2(ago >= 19 && ago <= 60,
                 qPrintable(QStringLiteral("chart now is not the current clock: a sample acquired 20 s ago reads "
                                           "\"%1\"")
                                .arg(ro.section(QLatin1Char('\n'), 0, 0))));
    }

    // Pause semantics are unchanged: paused frames are ignored, and neither
    // pausing nor resuming makes the old data current.
    w.setPaused(true);
    const int applied = w.snapshotsApplied();
    const std::size_t samples = h.size();
    w.applySnapshot(frame(3, now - 19.5, true));
    QCOMPARE(w.snapshotsApplied(), applied);
    QCOMPARE(h.size(), samples);
    QVERIFY(w.isStale());
    w.setPaused(false);
    QTest::qWait(700);
    QVERIFY2(w.isStale(), "resuming cleared the stale state without a recent frame");
    leaks = staleLeaks(w);
    QVERIFY2(leaks.isEmpty(), qPrintable(QStringLiteral("after resume: ") + leaks));
    w.applySnapshot(frame(4, ps::monotonicSeconds() - 15, true));
    QVERIFY2(w.isStale(), "a 15 s old frame after resume cleared the stale state");

    // A recent frame restores the live view.
    const double fresh = ps::monotonicSeconds();
    w.applySnapshot(frame(5, fresh, false));
    QVERIFY2(!w.isStale(), qPrintable(QStringLiteral("fresh frame did not restore; status \"%1\"").arg(w.statusText())));
    QVERIFY2(hasDigit(factText(w, "mem_used")), qPrintable(factText(w, "mem_used")));
    QVERIFY(sensors->topLevelItemCount() > 0 && hasDigit(sensors->topLevelItem(0)->text(1)));
    QCOMPARE(h.latestTime(), fresh);
    cpuChart->setCursorTime(fresh);
    QVERIFY2(readoutAgo(cpuChart->readoutText()) < 2, qPrintable(cpuChart->readoutText()));
    cpuChart->clearCursor();

    // An out-of-order frame older than what is shown must not replace it.
    const QString usedBefore = factText(w, "mem_used");
    w.applySnapshot(frame(6, fresh - 10, true, 5));
    QVERIFY2(w.isStale() || factText(w, "mem_used") == usedBefore,
             qPrintable(QStringLiteral("frame acquired 10 s before the shown one replaced it: mem_used \"%1\" -> "
                                       "\"%2\"")
                            .arg(usedBefore, factText(w, "mem_used"))));
    QCOMPARE(h.latestTime(), fresh);
    QVERIFY2(mon.violations.isEmpty(), qPrintable(mon.violations.join(QStringLiteral("; "))));
#endif
}

void TestAdversarial::mainWindowDelayedGuiDeliveryKeepsStale()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    // The collector keeps acquiring while the UI thread is busy for 4 s. The
    // frame queued meanwhile was acquired >= 3.5 s before it is delivered, so
    // delivering it must not make the view live; a later recent frame does.
    FixtureRoot r;
    QVERIFY(r.ok());
    writeBaseTree(r);
    r.write("sys/class/hwmon/hwmon0/name", "k10temp\n");
    r.write("sys/class/hwmon/hwmon0/temp1_input", "45000\n");
    QTemporaryDir d;
    auto o = uiOptions(d, true);
    o.sampler = fixtureOptions(r);
    MainWindow w(o);
    FreshnessMonitor mon;
    watchFreshness(w, mon, 500);
    QTRY_VERIFY_WITH_TIMEOUT(w.snapshotsApplied() >= 3 && !w.isStale(), 8000);
    QVERIFY2(mon.violations.isEmpty(), qPrintable(mon.violations.join(QStringLiteral("; "))));
    const int before = w.snapshotsApplied();
    const int checksBefore = mon.checks;
    QThread::msleep(4000); // UI thread busy (no event processing)
    QTRY_VERIFY_WITH_TIMEOUT(w.snapshotsApplied() >= before + 3 && !w.isStale(), 8000);
    QVERIFY(mon.checks > checksBefore);
    QVERIFY2(mon.violations.isEmpty(), qPrintable(mon.violations.join(QStringLiteral("; "))));
    QVERIFY(hasDigit(factText(w, "mem_used")));
#endif
}

void TestAdversarial::mainWindowSlowCollectionKeepsStaleWithoutRateSpike()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    // One collection blocks for ~4 s (its /proc/stat read waits on a FIFO);
    // meanwhile the disk transfers D sectors. That sample is stamped when it
    // started but reads the disk counters when it unblocks. It must neither
    // make the view live on arrival (it is 3.5+ s old) nor record a transfer
    // rate that assumes the counters were read on time: dividing D sectors by
    // the nominal 0.5 s would show an 8x historical spike. A gap, drop or
    // rebaseline is acceptable; a time-consistent rate is at most D*512 B over
    // the real time between the two disk reads (> the gated time).
    FixtureRoot r;
    QVERIFY(r.ok());
    writeBaseTree(r);
    r.remove("proc/stat");
    const std::string fifo = r.path("proc/stat");
    QCOMPARE(::mkfifo(fifo.c_str(), 0600), 0);
    addBlock(r, "sda", 2048, true);
    r.write("proc/diskstats", diskLine(8, 0, "sda", 1000, 0, 0));
    StatFifoServer server(fifo);
    QTemporaryDir d;
    auto o = uiOptions(d, true);
    o.sampler = fixtureOptions(r);
    MainWindow w(o);
    Ungate ungate{server};
    FreshnessMonitor mon;
    watchFreshness(w, mon, 500);
    QTRY_VERIFY_WITH_TIMEOUT(w.snapshotsApplied() >= 3 && !w.isStale(), 8000);

    server.setGated(true);
    const double gateOn = ps::monotonicSeconds();
    QTest::qWait(700); // the collector is now blocked opening /proc/stat
    constexpr uint64_t D = 8192;
    r.write("proc/diskstats", diskLine(8, 0, "sda", 1000 + D, 0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(w.isStale(), 6000);
    QString leaks = staleLeaks(w);
    QVERIFY2(leaks.isEmpty(), qPrintable(QStringLiteral("blocked collector: ") + leaks));
    while (ps::monotonicSeconds() - gateOn < 4.0)
        QTest::qWait(50);
    const int applied = w.snapshotsApplied();
    const double release = ps::monotonicSeconds();
    server.setGated(false);
    QTRY_VERIFY_WITH_TIMEOUT(!w.isStale() && w.snapshotsApplied() >= applied + 2, 6000);

    const double honest = double(D) * 512.0 / (release - gateOn);
    QStringList spikes;
    const auto& h = w.history();
    for (const std::string& key : {std::string(ps::keys::diskAllRead), ps::keys::diskRead("sda")}) {
        const auto* s = h.find(key);
        if (!s)
            continue;
        for (std::size_t i = 0; i < h.size(); ++i) {
            const double v = h.value(*s, i);
            if (ps::isValid(v) && v > honest * 1.05)
                spikes << QStringLiteral("%1 = %2 B/s at t=%3 s (gate %4..%5 s)")
                              .arg(QString::fromStdString(key))
                              .arg(v, 0, 'f', 0)
                              .arg(h.time(i) - gateOn, 0, 'f', 2)
                              .arg(0.0, 0, 'f', 2)
                              .arg(release - gateOn, 0, 'f', 2);
        }
    }
    const QString spikeText = spikes.isEmpty()
        ? QString()
        : QStringLiteral("mis-timed rate recorded (time-consistent max %1 B/s): %2")
              .arg(honest, 0, 'f', 0)
              .arg(spikes.join(QStringLiteral("; ")));
    const QString problems = mon.violations.join(QStringLiteral("; ")) + QStringLiteral(" | ") + spikeText;
    QVERIFY2(mon.violations.isEmpty() && spikes.isEmpty(), qPrintable(problems));
#endif
}

#ifdef PS_ADV_MAINWINDOW
namespace {

// Drives the next QFileDialog shown by a nested modal loop; closes any modal
// window after 5 s so a missing dialog cannot hang the suite.
class FileDialogDriver : public QObject
{
public:
    bool seen = false;
    QString initialFile;
    QString initialDir;
    qint64 shownAfterMs = -1;

    explicit FileDialogDriver(std::function<void(QFileDialog*)> act)
        : m_act(std::move(act))
    {
        m_clock.start();
        m_timer.setInterval(10);
        connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
        m_timer.start();
    }

private:
    void poll()
    {
        for (QWidget* top : QApplication::topLevelWidgets()) {
            auto* fd = qobject_cast<QFileDialog*>(top);
            if (!fd || !fd->isVisible())
                continue;
            m_timer.stop();
            seen = true;
            shownAfterMs = m_clock.elapsed();
            initialFile = fd->selectedFiles().value(0);
            initialDir = fd->directory().absolutePath();
            m_act(fd);
            return;
        }
        if (m_clock.elapsed() > 5000) {
            m_timer.stop();
            if (QWidget* m = QApplication::activeModalWidget())
                m->close();
        }
    }
    std::function<void(QFileDialog*)> m_act;
    QElapsedTimer m_clock;
    QTimer m_timer;
};

} // namespace
#endif

void TestAdversarial::mainWindowInteractiveExportUsesChooser()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    // The real interactive entry (the export action): the chooser opens with a
    // default name in Documents, cancelling exports nothing, choosing a path
    // exports there. Documents is redirected to a temporary directory through
    // XDG_CONFIG_HOME/user-dirs.dirs (restored afterwards). A missing
    // Documents directory still opens the chooser promptly.
    QTemporaryDir d;
    QVERIFY(d.isValid());
    const QString cfg = d.filePath(QStringLiteral("config"));
    const QString docs = d.filePath(QStringLiteral("Docs"));
    QVERIFY(QDir().mkpath(cfg) && QDir().mkpath(docs));
    {
        QFile f(cfg + QStringLiteral("/user-dirs.dirs"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray("XDG_DOCUMENTS_DIR=\"") + docs.toLocal8Bit() + "\"\n");
    }
    struct EnvRestore {
        bool had = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        QByteArray value = qgetenv("XDG_CONFIG_HOME");
        ~EnvRestore()
        {
            if (had)
                qputenv("XDG_CONFIG_HOME", value);
            else
                qunsetenv("XDG_CONFIG_HOME");
        }
    } restore;
    qputenv("XDG_CONFIG_HOME", cfg.toLocal8Bit());
    QCOMPARE(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), docs);

    auto o = uiOptions(d);
    MainWindow w(o);
    auto* action = w.findChild<QAction*>(QStringLiteral("exportAction"));
    QVERIFY(action);
    QSignalSpy done(&w, &MainWindow::csvExportFinished);
    const QRegularExpression defaultName(QStringLiteral("^pulsescope-\\d{8}-\\d{6}\\.csv$"));

    { // No samples: no chooser, an explanation instead.
        FileDialogDriver drv([](QFileDialog* fd) { fd->reject(); });
        action->trigger();
        QTest::qWait(100);
        QVERIFY2(!drv.seen, "chooser shown although there is nothing to export");
        QVERIFY2(!w.statusBar()->currentMessage().isEmpty(), "no explanation for the refused export");
    }
    w.applySnapshot(uiSnap(1, 1, false));
    w.applySnapshot(uiSnap(2, 2, true));

    { // Cancel exports nothing.
        FileDialogDriver drv([](QFileDialog* fd) { fd->reject(); });
        action->trigger();
        QVERIFY2(drv.seen, "export action did not open the chooser");
        QVERIFY2(defaultName.match(QFileInfo(drv.initialFile).fileName()).hasMatch(), qPrintable(drv.initialFile));
        QCOMPARE(QFileInfo(drv.initialFile).absolutePath(), docs);
        QCoreApplication::processEvents();
        QVERIFY(!w.isExporting());
        QCOMPARE(done.count(), 0);
        QVERIFY(QDir(docs).entryList(QDir::Files | QDir::Hidden | QDir::System).isEmpty());
    }
    { // Choosing a path exports there.
        const QString target = docs + QStringLiteral("/chosen.csv");
        FileDialogDriver drv([&](QFileDialog* fd) {
            fd->selectFile(target);
            if (auto* edit = fd->findChild<QLineEdit*>(QStringLiteral("fileNameEdit")))
                edit->setText(QFileInfo(target).fileName()); // what a user types into the name field
            static_cast<QDialog*>(fd)->accept(); // QFileDialog::accept (validates the name)
        });
        action->trigger();
        QVERIFY(drv.seen);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(2).toString()));
        QCOMPARE(QFileInfo(done.first().at(1).toString()).absoluteFilePath(), target);
        QFile f(target);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray csv = f.readAll();
        QVERIFY2(csv.count('\n') >= 3, csv.left(200).constData()); // header + 2 samples
    }
    { // Documents missing: the chooser still opens promptly with the default name.
        QVERIFY(QDir(docs).removeRecursively());
        FileDialogDriver drv([](QFileDialog* fd) { fd->reject(); });
        action->trigger();
        QVERIFY2(drv.seen, "no chooser when Documents does not exist");
        QVERIFY2(drv.shownAfterMs < 2000, qPrintable(QString::number(drv.shownAfterMs)));
        QVERIFY2(defaultName.match(QFileInfo(drv.initialFile).fileName()).hasMatch(), qPrintable(drv.initialFile));
        QCoreApplication::processEvents();
        QVERIFY(!w.isExporting());
        QCOMPARE(done.count(), 1);
    }
#endif
}

#ifdef PS_ADV_MAINWINDOW
namespace {

// Subprocess helper (fresh process, started through QProcess): lowers only its
// own soft RLIMIT_NPROC so that creating the export thread fails, checks the
// failure is reported and retryable, then restores the limit.
int runCsvThreadStartHelper(int argc, char** argv)
{
    QApplication app(argc, argv);
    QTextStream out(stdout);
    int failures = 0;
    auto fail = [&](const QString& m) {
        out << "FAIL: " << m << Qt::endl;
        ++failures;
    };
    auto waitFor = [](const std::function<bool()>& cond, int ms) {
        QElapsedTimer t;
        t.start();
        while (!cond() && t.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(5);
        }
        return cond();
    };
    QTemporaryDir d;
    auto writes = std::make_shared<std::atomic<int>>(0);
    auto writer = [writes](const std::string&, const QString&, QString*) {
        ++*writes;
        return true;
    };
    auto o = uiOptions(d);
    o.csvWriter = writer;
    MainWindow w(o);
    QSignalSpy done(&w, &MainWindow::csvExportFinished);
    w.applySnapshot(uiSnap(1, 1, false));
    w.applySnapshot(uiSnap(2, 2, true));
    auto* action = w.findChild<QAction*>(QStringLiteral("exportAction"));
    CsvExporter ex(writer);
    QSignalSpy exDone(&ex, &CsvExporter::finished);
    ps::History h;
    ps::recordSnapshot(h, *uiSnap(1, 1, false));
    const QString path = d.filePath(QStringLiteral("never-written.csv"));

    rlimit old {};
    if (::getrlimit(RLIMIT_NPROC, &old) != 0) {
        out << "SKIP: getrlimit(RLIMIT_NPROC) failed: " << std::strerror(errno) << Qt::endl;
        return 0;
    }
    rlimit low = old;
    low.rlim_cur = 1; // soft limit of this process only; the hard limit is untouched
    if (::setrlimit(RLIMIT_NPROC, &low) != 0) {
        out << "SKIP: setrlimit(RLIMIT_NPROC soft=1) failed: " << std::strerror(errno) << Qt::endl;
        return 0;
    }
    int code = 0;
    try {
        std::thread t([] {});
        t.join();
    } catch (const std::system_error& e) {
        code = e.code().value();
    }
    if (code == 0) {
        ::setrlimit(RLIMIT_NPROC, &old);
        out << "SKIP: soft RLIMIT_NPROC=1 does not block thread creation here (privileged or not enforced)"
            << Qt::endl;
        return 0;
    }
    out << "PROBE: thread creation blocked with errno " << code << " (" << std::strerror(code) << ")" << Qt::endl;
    const QString expected = QString::fromLocal8Bit(std::strerror(code));

    bool started = false;
    QString err, escaped;
    try {
        started = w.startCsvExport(path, &err);
    } catch (const std::exception& e) {
        escaped = QString::fromLocal8Bit(e.what());
    } catch (...) {
        escaped = QStringLiteral("non-std exception");
    }
    const bool windowBusy = w.isExporting();
    const bool actionEnabled = action && action->isEnabled();
    bool exStarted = false;
    QString exEscaped;
    try {
        exStarted = ex.start(h, 0, path);
    } catch (const std::exception& e) {
        exEscaped = QString::fromLocal8Bit(e.what());
    } catch (...) {
        exEscaped = QStringLiteral("non-std exception");
    }
    const bool exBusy = ex.isBusy();
    ::setrlimit(RLIMIT_NPROC, &old);

    if (!escaped.isEmpty())
        fail(QStringLiteral("MainWindow::startCsvExport let the thread-start exception escape: \"%1\"").arg(escaped));
    else {
        if (started)
            fail(QStringLiteral("MainWindow::startCsvExport returned true although no thread could start"));
        if (err.contains(QStringLiteral("still running")))
            fail(QStringLiteral("start failure reported as busy: \"%1\"").arg(err));
        if (!err.contains(expected))
            fail(QStringLiteral("error does not carry the start failure \"%1\": \"%2\"").arg(expected, err));
    }
    if (windowBusy)
        fail(QStringLiteral("window reports an export running after the failed start"));
    if (!actionEnabled)
        fail(QStringLiteral("export action left disabled after the failed start"));
    if (!exEscaped.isEmpty())
        fail(QStringLiteral("CsvExporter::start let the thread-start exception escape: \"%1\"").arg(exEscaped));
    else if (exStarted)
        fail(QStringLiteral("CsvExporter::start returned true although no thread could start"));
    if (exBusy)
        fail(QStringLiteral("CsvExporter stays busy after the failed start"));
    QCoreApplication::processEvents();
    if (done.count() != 0 && done.first().at(0).toBool())
        fail(QStringLiteral("a successful finish was reported for the failed start"));
    const int doneBefore = int(done.count());

    // Retry with the limit restored.
    err.clear();
    if (!w.startCsvExport(path, &err))
        fail(QStringLiteral("retry refused after the limit was restored: \"%1\"").arg(err));
    else if (!waitFor([&] { return done.count() > doneBefore; }, 5000))
        fail(QStringLiteral("retried export never finished"));
    else if (!done.last().at(0).toBool())
        fail(QStringLiteral("retried export failed: \"%1\"").arg(done.last().at(2).toString()));
    if (!ex.start(h, 0, path))
        fail(QStringLiteral("CsvExporter retry refused after the limit was restored"));
    else if (!waitFor([&] { return exDone.count() > 0; }, 5000))
        fail(QStringLiteral("CsvExporter retry never finished"));
    else if (!exDone.last().at(0).toBool())
        fail(QStringLiteral("CsvExporter retry failed: \"%1\"").arg(exDone.last().at(2).toString()));
    if (writes->load() != 2)
        fail(QStringLiteral("writer ran %1 times, expected 2 (the two retries)").arg(writes->load()));
    w.shutdown(2000);
    out << (failures ? "RESULT: FAIL" : "RESULT: OK") << Qt::endl;
    return failures ? 1 : 0;
}

} // namespace
#endif

void TestAdversarial::csvExportThreadStartFailureIsRetryable()
{
#ifndef PS_ADV_MAINWINDOW
    QSKIP("built without MainWindow");
#else
    QProcess p;
    p.setProgram(QCoreApplication::applicationFilePath());
    p.setArguments({QStringLiteral("--ps-adv-helper"), QStringLiteral("csv-thread-start")});
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start();
    QVERIFY(p.waitForStarted(5000));
    if (!p.waitForFinished(30000)) {
        p.kill();
        p.waitForFinished(2000);
        QFAIL("helper did not finish in 30 s");
    }
    const QString out = QString::fromLocal8Bit(p.readAll()).trimmed();
    qInfo().noquote() << "helper:" << out;
    if (out.contains(QStringLiteral("SKIP:")))
        QSKIP(qPrintable(out));
    QVERIFY2(p.exitStatus() == QProcess::NormalExit,
             qPrintable(QStringLiteral("helper crashed (exit status %1, code %2): %3")
                            .arg(int(p.exitStatus()))
                            .arg(p.exitCode())
                            .arg(out)));
    QVERIFY2(out.contains(QStringLiteral("PROBE:")), qPrintable(out));
    QVERIFY2(p.exitCode() == 0 && out.contains(QStringLiteral("RESULT: OK")), qPrintable(out));
#endif
}

int main(int argc, char** argv)
{
#ifdef PS_ADV_MAINWINDOW
    if (argc >= 3 && std::strcmp(argv[1], "--ps-adv-helper") == 0) {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
            qputenv("QT_QPA_PLATFORM", "offscreen");
        if (std::strcmp(argv[2], "csv-thread-start") == 0)
            return runCsvThreadStartHelper(argc, argv);
        return 2;
    }
#endif
#ifdef PS_ADV_WIDGETS
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
#else
    QCoreApplication app(argc, argv);
#endif
    TestAdversarial tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_adversarial.moc"
