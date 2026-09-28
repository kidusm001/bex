// Independent adversarial tests for the grouped process view: which
// processes own a window (Apps), which are Background and Kernel, and the
// grouped model and filter that show them.
//
// Oracles are written from the approved behaviour, not from the code:
//  - Apps holds only processes a current, complete window list names, pinned
//    to (PID, start time); a reused PID never inherits it.
//  - A missing, failed, partial or unsupported window list is never an empty
//    success: user processes are then unclassified, never "background".
//  - Kernel threads (PF_KTHREAD) are always Kernel.
//  - The groups keep the order Apps, Background, Kernel whatever the sort;
//    rows move between groups instead of the model resetting.
//  - Window requests are bounded (deadline, bytes, windows), one at a time,
//    cancellable, and results from before a pause are never delivered.
// Nothing here reads the real desktop's window list or touches another
// process: sockets, /proc trees and query functions are local fakes.
#include "app/mainwindow.h"
#include "app/processmodel.h"
#include "app/windowowners.h"
#include "core/snapshot.h"

#include <QAbstractItemModelTester>
#include <QAction>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QTreeView>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

using windows::Status;

namespace {

ps::ProcessInfo proc(int pid, uint64_t start, const char* name, double cpu, double rss = 1e6, bool kernel = false)
{
    ps::ProcessInfo p;
    p.pid = pid;
    p.ppid = kernel ? 2 : 1;
    p.startTicks = start;
    p.kernelThread = kernel;
    p.name = name;
    p.command = kernel ? std::string("[") + name + "]" : std::string("/usr/bin/") + name;
    p.user = kernel ? "root" : "tester";
    p.uid = kernel ? 0 : 1000;
    p.uidKnown = true;
    p.state = 'S';
    p.threads = 1;
    p.cpuPercent = cpu;
    p.rssBytes = kernel ? 0 : rss;
    p.ageSeconds = 10;
    return p;
}

ProcessGrouping detected(std::initializer_list<ProcessKey> apps)
{
    ProcessGrouping g;
    g.state = ProcessGrouping::State::Detected;
    g.source = QStringLiteral("Test");
    for (const ProcessKey& k : apps)
        g.apps.insert(k);
    return g;
}

ProcessGrouping unavailable(const QString& reason)
{
    ProcessGrouping g;
    g.state = ProcessGrouping::State::Unavailable;
    g.reason = reason;
    return g;
}

// (group, pid, start) of every process row, in model order.
struct Row {
    int group;
    int pid;
    qulonglong start;
    bool operator==(const Row& o) const { return group == o.group && pid == o.pid && start == o.start; }
};
QList<Row> rowsOf(const QAbstractItemModel& m)
{
    QList<Row> out;
    for (int g = 0; g < m.rowCount(); ++g) {
        const QModelIndex gi = m.index(g, 0);
        for (int r = 0; r < m.rowCount(gi); ++r) {
            const QModelIndex i = m.index(r, 0, gi);
            out << Row {i.data(ProcessModel::GroupRole).toInt(), i.data(ProcessModel::PidRole).toInt(),
                        i.data(ProcessModel::StartTicksRole).toULongLong()};
        }
    }
    return out;
}
QString describeRows(const QList<Row>& rows)
{
    QStringList s;
    for (const Row& r : rows)
        s << QStringLiteral("%1:%2/%3").arg(r.group).arg(r.pid).arg(r.start);
    return s.join(QLatin1Char(' '));
}
QList<int> groupOrder(const QAbstractItemModel& m)
{
    QList<int> out;
    for (int g = 0; g < m.rowCount(); ++g)
        out << m.index(g, 0).data(ProcessModel::GroupRole).toInt();
    return out;
}
QList<int> pidsIn(const QAbstractItemModel& m, int groupRow)
{
    QList<int> out;
    const QModelIndex gi = m.index(groupRow, 0);
    for (int r = 0; r < m.rowCount(gi); ++r)
        out << m.index(r, 0, gi).data(ProcessModel::PidRole).toInt();
    return out;
}

// A /proc/PID/stat line with the given flags and start time (field 22).
QByteArray statLine(int pid, const QByteArray& comm, unsigned flags, uint64_t startTicks)
{
    QByteArray s = QByteArray::number(pid) + " (" + comm + ") S 1 1 1 0 -1 " + QByteArray::number(flags);
    for (int field = 10; field <= 21; ++field)
        s += " 0";
    s += " " + QByteArray::number(qulonglong(startTicks));
    for (int field = 23; field <= 52; ++field)
        s += " 0";
    return s + "\n";
}
void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

// A Unix socket served by a thread of this process, for one connection.
class FakeSocketServer {
public:
    enum class Mode { Reply, Silent, CloseWithoutReply, PartialThenHold, PartialThenClose, Drip };
    FakeSocketServer(const QString& path, Mode mode, QByteArray reply = {}) : m_mode(mode), m_reply(std::move(reply))
    {
        m_stop = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        m_listen = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        const QByteArray p = QFile::encodeName(path);
        std::memcpy(addr.sun_path, p.constData(), std::min<std::size_t>(p.size(), sizeof addr.sun_path - 1));
        m_ok = m_listen >= 0 && bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 &&
               listen(m_listen, 4) == 0;
        if (m_ok)
            m_thread = std::thread([this] { serve(); });
    }
    ~FakeSocketServer()
    {
        const uint64_t one = 1;
        [[maybe_unused]] auto n = write(m_stop, &one, sizeof one);
        if (m_thread.joinable())
            m_thread.join();
        if (m_listen >= 0)
            close(m_listen);
        close(m_stop);
    }
    bool ok() const { return m_ok; }
    QByteArray request() const
    {
        std::lock_guard<std::mutex> l(m_mutex);
        return m_request;
    }
    int connections() const { return m_connections.load(); }

private:
    bool stopped(int ms)
    {
        pollfd p {m_stop, POLLIN, 0};
        return poll(&p, 1, ms) > 0;
    }
    void serve()
    {
        pollfd p[2] = {{m_listen, POLLIN, 0}, {m_stop, POLLIN, 0}};
        if (poll(p, 2, 10000) <= 0 || (p[1].revents & POLLIN))
            return;
        const int c = accept4(m_listen, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0)
            return;
        ++m_connections;
        // Read the request until it is complete or the client pauses.
        QByteArray req;
        for (;;) {
            pollfd q[2] = {{c, POLLIN, 0}, {m_stop, POLLIN, 0}};
            if (poll(q, 2, 150) <= 0 || (q[1].revents & POLLIN))
                break;
            char buf[256];
            const ssize_t n = read(c, buf, sizeof buf);
            if (n <= 0)
                break;
            req.append(buf, int(n));
            if (req.size() >= 9 && req.endsWith("clients"))
                break;
        }
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_request = req;
        }
        auto send = [&](const QByteArray& d) { return ::send(c, d.constData(), std::size_t(d.size()), MSG_NOSIGNAL); };
        switch (m_mode) {
        case Mode::Reply:
            for (qsizetype off = 0; off < m_reply.size() && !stopped(0);) {
                const ssize_t n = send(m_reply.mid(off, 65536));
                if (n <= 0)
                    break;
                off += n;
            }
            break;
        case Mode::Silent:
            stopped(10000);
            break;
        case Mode::CloseWithoutReply:
            break;
        case Mode::PartialThenHold:
            send(m_reply.left(m_reply.size() / 2));
            stopped(10000);
            break;
        case Mode::PartialThenClose:
            send(m_reply.left(m_reply.size() / 2));
            break;
        case Mode::Drip:
            for (int i = 0; i < 400 && !stopped(40); ++i)
                if (send(QByteArray(1, i == 0 ? '[' : ' ')) <= 0)
                    break;
            break;
        }
        close(c);
    }

    Mode m_mode;
    QByteArray m_reply;
    int m_listen = -1, m_stop = -1;
    bool m_ok = false;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    QByteArray m_request;
    std::atomic<int> m_connections {0};
};

windows::Environment hyprEnv(const QString& runtimeDir, const char* sig = "testsig")
{
    windows::Environment e;
    e.hyprlandSignature = sig;
    e.runtimeDir = runtimeDir.toStdString();
    e.waylandDisplay = "wayland-1";
    e.sessionType = "wayland";
    return e;
}

// A fake /proc: PID 500 started at 1 s, 501 at 2000 s, 502 absent,
// 503 a kernel thread, 504 with a hostile name.
struct FakeProc {
    QTemporaryDir dir;
    std::string root;
    FakeProc()
    {
        root = dir.path().toStdString();
        writeFile(dir.filePath(QStringLiteral("500/stat")), statLine(500, "app", 0x400000, 100));
        writeFile(dir.filePath(QStringLiteral("501/stat")), statLine(501, "late", 0x400000, 200000));
        writeFile(dir.filePath(QStringLiteral("503/stat")), statLine(503, "kworker/0:1", 0x00200040, 50));
        // The name ends in ") S 1 ..." look-alikes; field 22 is counted from
        // the last ')'.
        writeFile(dir.filePath(QStringLiteral("504/stat")), statLine(504, "x) R 1 1 1 0 -1 0 9 9 (y", 0x400000, 300));
    }
};

// Clock for probes: CLOCK_BOOTTIME-like seconds, fixed well after the fakes
// started (1000 s), advancing with real time.
std::function<double()> fakeClock()
{
    auto t0 = std::make_shared<QElapsedTimer>();
    t0->start();
    return [t0] { return 1000.0 + t0->elapsed() / 1000.0; };
}

windows::ProbeOptions probeOptions(const FakeProc& fp, const QString& runtimeDir, windows::QueryFunction query)
{
    windows::ProbeOptions o;
    o.environment = hyprEnv(runtimeDir);
    o.procRoot = fp.root;
    o.clockTicks = 100;
    o.visibleIntervalMs = 60;
    o.backgroundIntervalMs = 200;
    o.maxBackoffMs = 400;
    o.clock = fakeClock();
    o.query = std::move(query);
    return o;
}

windows::Query okQuery(std::vector<int> pids)
{
    windows::Query q;
    q.status = Status::Ok;
    q.windows = int(pids.size());
    q.pids = std::move(pids);
    return q;
}

// ---------------------------------------------------------------------------
// Main window fixtures: injected snapshots and window lists, no collector.

// One systemd, one kernel thread, a shell and two window candidates.
std::vector<ps::ProcessInfo> hostProcesses()
{
    return {proc(1, 10, "systemd", 0.1, 12e6), proc(2, 5, "kthreadd", 0.0, 0, true), proc(500, 100, "bash", 3.5, 5e6),
            proc(4242, 5242, "Web Content (x)", 150.0, 900e6), proc(777, 1777, "fire) fox", 1.0, 300e6)};
}

ps::SnapshotPtr snap(double time, std::vector<ps::ProcessInfo> processes = hostProcesses())
{
    static uint64_t seq = 0;
    auto s = std::make_shared<ps::Snapshot>();
    s->sequence = ++seq;
    s->time = time;
    s->interval = 1.0;
    s->continuous = true;
    s->logicalCpus = 4;
    s->processesScanned = true;
    s->threadCount = int(processes.size());
    s->processes = std::move(processes);
    return s;
}

windows::ResultPtr owners(std::vector<windows::Owner> list, double time, double validFor = 3.0)
{
    auto r = std::make_shared<windows::Result>();
    r->status = Status::Ok;
    r->backend = windows::Backend::Hyprland;
    r->requested = time;
    r->time = time;
    r->validFor = validFor;
    r->windows = int(list.size());
    r->owners = std::move(list);
    return r;
}

windows::ResultPtr failed(Status status, const char* detail, double time)
{
    auto r = std::make_shared<windows::Result>();
    r->status = status;
    r->backend = windows::Backend::Hyprland;
    r->detail = QString::fromLatin1(detail);
    r->requested = time;
    r->time = time;
    return r;
}

struct WindowEnv {
    QTemporaryDir dir;
    std::shared_ptr<double> clock = std::make_shared<double>(100.0);
    QString settings() const { return dir.filePath(QStringLiteral("pulsescope.ini")); }
    MainWindowOptions options(bool withClock = true) const
    {
        MainWindowOptions o;
        o.startCollector = false;
        o.settingsFile = settings();
        o.windowProbe.enabled = false; // results only through applyWindowOwners()
        if (withClock) {
            auto c = clock;
            o.clock = [c] { return *c; };
        }
        return o;
    }
};

QTreeView* treeOf(MainWindow& w)
{
    return w.findChild<QTreeView*>(QStringLiteral("processTable"));
}
QModelIndex groupAt(QTreeView* t, ProcessModel::Group g)
{
    auto* m = t->model();
    for (int r = 0; r < m->rowCount(); ++r)
        if (m->index(r, 0).data(ProcessModel::GroupRole).toInt() == g)
            return m->index(r, 0);
    return {};
}
QList<int> pidsOf(QTreeView* t, ProcessModel::Group g)
{
    QList<int> out;
    const QModelIndex gi = groupAt(t, g);
    for (int r = 0; r < t->model()->rowCount(gi); ++r)
        out << t->model()->index(r, 0, gi).data(ProcessModel::PidRole).toInt();
    std::sort(out.begin(), out.end());
    return out;
}
QModelIndex rowOf(QTreeView* t, int pid)
{
    auto* m = t->model();
    for (int g = 0; g < m->rowCount(); ++g) {
        const QModelIndex gi = m->index(g, 0);
        for (int r = 0; r < m->rowCount(gi); ++r)
            if (m->index(r, 0, gi).data(ProcessModel::PidRole).toInt() == pid)
                return m->index(r, 0, gi);
    }
    return {};
}
// (pid, start) of the selected process rows.
QList<QPair<int, qulonglong>> selected(QTreeView* t)
{
    QList<QPair<int, qulonglong>> out;
    for (const QModelIndex& i : t->selectionModel()->selectedRows())
        if (!i.data(ProcessModel::IsGroupRole).toBool())
            out << qMakePair(i.data(ProcessModel::PidRole).toInt(), i.data(ProcessModel::StartTicksRole).toULongLong());
    return out;
}
QString titles(QTreeView* t)
{
    QStringList out;
    for (int r = 0; r < t->model()->rowCount(); ++r)
        out << t->model()->index(r, 0).data().toString();
    return out.join(QStringLiteral(" | "));
}
QString expansion(QTreeView* t)
{
    QString s;
    for (auto g : {ProcessModel::Apps, ProcessModel::Background, ProcessModel::Kernel})
        s += t->isExpanded(groupAt(t, g)) ? QLatin1Char('+') : QLatin1Char('-');
    return s;
}

} // namespace

Q_DECLARE_METATYPE(QList<int>)

class TestProcessGroups : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();

    // Window list: backend choice, parsing, the socket request, identities.
    void selectBackendChoosesOnlyGenuineWindowLists_data();
    void selectBackendChoosesOnlyGenuineWindowLists();
    void statusIdentifiersAreDistinct();
    void hyprlandParserCountsWindowsAndPids_data();
    void hyprlandParserCountsWindowsAndPids();
    void hyprlandParserKeepsNoWindowText();
    void hyprlandSocketQueryIsBounded_data();
    void hyprlandSocketQueryIsBounded();
    void hyprlandSocketQueryCancels();
    void hyprlandSocketUnreachable();
    void resolveOwnersPinsIdentity();
    void hyprlandFullBacklogIsBusyNotAnotherUser();
    void probeRetriesAfterFullBacklog();
    void partialListDoesNotEstablishBackground_data();
    void partialListDoesNotEstablishBackground();
    void completeEmptyListIsDetectedWithNoApps();

    // The probe thread.
    void probeDeliversPinnedOwners();
    void probeKeepsOneRequestInFlight();
    void probeDropsResultsFromBeforePause();
    void probeStopCancelsBlockedRequest();
    void probeDoesNotRetryFinalStatus();
    void probeBacksOffAfterFailures();

    // Classification and the grouped model.
    void groupForClassifiesByIdentity();
    void unavailableGroupingNeverClaimsBackground();
    void modelPassesItemModelTesterThroughGroupMoves();
    void modelMovesRowsInsteadOfResetting();
    void modelShowsEachIdentityOnce();
    void proxyKeepsGroupOrderWhenSorting();
    void proxyFilterCountsAndKeepsGroups();
    void staleModelKeepsGroupsAndIdentity();

    // The main window: expansion, freshness of the window list, selection.
    void windowExpansionDefaultsPersistAndMigrate();
    void windowOwnersOrderedAndNeverFromTheFuture();
    void expiredListNotReusedAfterResume();
    void failureKeepsListUntilExpiryThenExplains();
    void reusedPidDoesNotInheritWindow();
    void selectionFollowsIdentityThroughRegroupAndFilter();
    void groupRowsStaySpannedWithoutValues();
    void probeIdlesWhileProcessListHidden();
};

void TestProcessGroups::initTestCase()
{
    qRegisterMetaType<windows::ResultPtr>();
}

// ---------------------------------------------------------------------------
// Backend choice

void TestProcessGroups::selectBackendChoosesOnlyGenuineWindowLists_data()
{
    QTest::addColumn<QString>("signature");
    QTest::addColumn<QString>("runtimeDir");
    QTest::addColumn<QString>("waylandDisplay");
    QTest::addColumn<QString>("sessionType");
    QTest::addColumn<QString>("display");
    QTest::addColumn<bool>("x11Available");
    QTest::addColumn<int>("backend");  // expected windows::Backend
    QTest::addColumn<bool>("ok");      // Status::Ok expected
    QTest::addColumn<QString>("address");

    const int none = int(windows::Backend::None), hypr = int(windows::Backend::Hyprland), x11 = int(windows::Backend::X11);
    QTest::newRow("hyprland") << "abc_123_456" << "/run/user/1000" << "wayland-1" << "wayland" << ":0" << true << hypr << true
                              << "/run/user/1000/hypr/abc_123_456/.socket.sock";
    QTest::newRow("hyprland, runtime dir unset") << "abc" << "" << "wayland-1" << "wayland" << "" << false << hypr << false << "";
    QTest::newRow("hyprland, relative runtime dir") << "abc" << "run/user" << "wayland-1" << "wayland" << "" << false << hypr
                                                    << false << "";
    QTest::newRow("hyprland, signature escapes the directory") << "../../../tmp/evil" << "/run/user/1000" << "wayland-1"
                                                               << "wayland" << "" << false << hypr << false << "";
    QTest::newRow("hyprland, signature with a slash") << "a/b" << "/run/user/1000" << "wayland-1" << "wayland" << "" << false
                                                      << hypr << false << "";
    // XWayland lists X11 windows only: never taken for the whole list.
    QTest::newRow("other wayland compositor with XWayland") << "" << "/run/user/1000" << "wayland-0" << "wayland" << ":1"
                                                            << true << none << false << "";
    QTest::newRow("wayland session type only, DISPLAY set") << "" << "/run/user/1000" << "" << "wayland" << ":1" << true
                                                            << none << false << "";
    QTest::newRow("x11 session") << "" << "/run/user/1000" << "" << "x11" << ":0" << true << x11 << true << ":0";
    QTest::newRow("x11 session, no X11 support built") << "" << "/run/user/1000" << "" << "x11" << ":0" << false << x11
                                                       << false << "";
    QTest::newRow("nothing") << "" << "" << "" << "" << "" << true << none << false << "";
    QTest::newRow("tty session") << "" << "/run/user/1000" << "" << "tty" << "" << true << none << false << "";
}

void TestProcessGroups::selectBackendChoosesOnlyGenuineWindowLists()
{
    QFETCH(QString, signature);
    QFETCH(QString, runtimeDir);
    QFETCH(QString, waylandDisplay);
    QFETCH(QString, sessionType);
    QFETCH(QString, display);
    QFETCH(bool, x11Available);
    QFETCH(int, backend);
    QFETCH(bool, ok);
    QFETCH(QString, address);
    windows::Environment e;
    e.hyprlandSignature = signature.toStdString();
    e.runtimeDir = runtimeDir.toStdString();
    e.waylandDisplay = waylandDisplay.toStdString();
    e.sessionType = sessionType.toStdString();
    e.display = display.toStdString();
    const windows::Selection s = windows::selectBackend(e, x11Available);
    QCOMPARE(s.status == Status::Ok, ok);
    if (ok) {
        QCOMPARE(int(s.backend), backend);
        QCOMPARE(QString::fromStdString(s.address), address);
    } else {
        // Not usable: nothing to connect to, and a reason to show.
        QVERIFY2(s.backend == windows::Backend::None || s.address.empty() || !QString::fromStdString(s.address).contains(QLatin1String("..")),
                 qPrintable(QStringLiteral("backend %1 at \"%2\"").arg(int(s.backend)).arg(QString::fromStdString(s.address))));
        QVERIFY2(!s.detail.isEmpty(), "no reason given");
        if (backend == int(windows::Backend::None))
            QCOMPARE(int(s.backend), backend);
    }
}

void TestProcessGroups::statusIdentifiersAreDistinct()
{
    std::set<std::string> ids;
    const int last = int(Status::Failed);
    for (int i = 0; i <= last; ++i) {
        const char* id = windows::statusId(Status(i));
        QVERIFY(id && *id);
        QVERIFY2(ids.insert(id).second, id);
        windows::Result r;
        r.status = Status(i);
        r.detail = QStringLiteral("detail");
        if (Status(i) == Status::Ok)
            QVERIFY(windows::describe(r).isEmpty());
        else
            QVERIFY2(!windows::describe(r).isEmpty(), id);
    }
    // The environment will not change: asking again cannot help.
    QVERIFY(windows::isFinal(Status::Unsupported));
    // A busy or restarting compositor can answer next time.
    for (Status s : {Status::Timeout, Status::Disconnected, Status::Truncated})
        QVERIFY2(!windows::isFinal(s), windows::statusId(s));
}

// ---------------------------------------------------------------------------
// Parsing a complete j/clients reply (the byte limit applies while reading the
// socket, see hyprlandSocketQueryIsBounded)

void TestProcessGroups::hyprlandParserCountsWindowsAndPids_data()
{
    QTest::addColumn<QByteArray>("reply");
    QTest::addColumn<int>("maxWindows");
    QTest::addColumn<int>("maxBytes");            // 0: the default
    QTest::addColumn<QList<int>>("statuses"); // acceptable
    QTest::addColumn<int>("windows");
    QTest::addColumn<int>("withoutPid");
    QTest::addColumn<QList<int>>("pids");
    const int ok = int(Status::Ok);
    auto bad = [](std::initializer_list<Status> s) {
        QList<int> l;
        for (Status x : s)
            l << int(x);
        return l;
    };
    QTest::newRow("two windows of one process") << QByteArray(R"([{"pid":500,"title":"a"},{"pid":500,"title":"b"}])")
                                                << 1024 << 0 << QList<int> {ok} << 2 << 0 << QList<int> {500};
    QTest::newRow("unsorted") << QByteArray(R"([{"pid":900},{"pid":12},{"pid":500},{"pid":12}])") << 1024 << 0
                              << QList<int> {ok} << 4 << 0 << QList<int> {12, 500, 900};
    QTest::newRow("no windows") << QByteArray("[]") << 1024 << 0 << QList<int> {ok} << 0 << 0 << QList<int> {};
    QTest::newRow("trailing newline") << QByteArray("[{\"pid\":7}]\n") << 1024 << 0 << QList<int> {ok} << 1 << 0
                                      << QList<int> {7};
    QTest::newRow("pids outside 1..4194304 or not integers")
        << QByteArray(R"([{"pid":-1},{"pid":0},{"pid":4194305},{"pid":1.5},{"pid":"77"},{},{"pid":null},{"pid":true},)"
                      R"({"pid":4194304},{"pid":1},{"pid":1e20},{"pid":2147483648},{"pid":-2147483649},{"pid":[5]},{"pid":{"x":5}}])")
        << 1024 << 0 << QList<int> {ok} << 15 << 13 << QList<int> {1, 4194304};
    QTest::newRow("object, not a list") << QByteArray(R"({"pid":5})") << 1024 << 0
                                        << bad({Status::NotArray, Status::Malformed}) << 0 << 0 << QList<int> {};
    QTest::newRow("json null") << QByteArray("null") << 1024 << 0 << bad({Status::NotArray, Status::Malformed}) << 0
                               << 0 << QList<int> {};
    QTest::newRow("error text") << QByteArray("unknown request") << 1024 << 0
                                << bad({Status::Malformed, Status::NotArray}) << 0 << 0 << QList<int> {};
    QTest::newRow("records not objects") << QByteArray("[500,501]") << 1024 << 0 << bad({Status::Malformed}) << 0 << 0
                                         << QList<int> {};
    QTest::newRow("one record not an object") << QByteArray(R"([{"pid":5},6])") << 1024 << 0 << bad({Status::Malformed})
                                              << 0 << 0 << QList<int> {};
    QTest::newRow("nested list") << QByteArray(R"([[{"pid":5}]])") << 1024 << 0 << bad({Status::Malformed}) << 0 << 0
                                 << QList<int> {};
    QTest::newRow("cut short") << QByteArray(R"([{"pid":5},{"pid":6)") << 1024 << 0
                               << bad({Status::Malformed, Status::Truncated}) << 0 << 0 << QList<int> {};
    QTest::newRow("garbage after the list") << QByteArray(R"([{"pid":5}] [{"pid":6}])") << 1024 << 0
                                            << bad({Status::Malformed}) << 0 << 0 << QList<int> {};
    QTest::newRow("empty reply") << QByteArray() << 1024 << 0
                                 << bad({Status::Disconnected, Status::Malformed, Status::NotArray}) << 0 << 0 << QList<int> {};
    QTest::newRow("exactly the window limit") << QByteArray(R"([{"pid":1},{"pid":2},{"pid":3}])") << 3 << 0
                                              << QList<int> {ok} << 3 << 0 << QList<int> {1, 2, 3};
    QTest::newRow("one window over the limit") << QByteArray(R"([{"pid":1},{"pid":2},{"pid":3}])") << 2 << 0
                                               << bad({Status::TooMany}) << 0 << 0 << QList<int> {};
}

void TestProcessGroups::hyprlandParserCountsWindowsAndPids()
{
    QFETCH(QByteArray, reply);
    QFETCH(int, maxWindows);
    QFETCH(int, maxBytes);
    QFETCH(QList<int>, statuses);
    QFETCH(int, windows);
    QFETCH(int, withoutPid);
    QFETCH(QList<int>, pids);
    windows::Limits limits;
    limits.maxWindows = maxWindows;
    if (maxBytes > 0)
        limits.maxBytes = maxBytes;
    const windows::Query q = windows::parseHyprlandClients(reply, limits);
    QVERIFY2(statuses.contains(int(q.status)),
             qPrintable(QStringLiteral("status %1 (%2)").arg(windows::statusId(q.status), q.detail)));
    if (q.status == Status::Ok) {
        QCOMPARE(q.windows, windows);
        QCOMPARE(q.withoutPid, withoutPid);
        QCOMPARE(QList<int>(q.pids.begin(), q.pids.end()), pids);
    } else {
        // A failed reply is no list at all, never a partial one.
        QVERIFY2(q.pids.empty(), qPrintable(QStringLiteral("%1 pids kept from a failed reply").arg(q.pids.size())));
        QVERIFY(!q.detail.isEmpty());
    }
}

void TestProcessGroups::hyprlandParserKeepsNoWindowText()
{
    const QByteArray secret = "SECRET-WINDOW-TITLE-7f3a";
    windows::Limits limits;
    const QByteArray good = R"([{"pid":500,"title":")" + secret + R"(","class":")" + secret + R"(","initialTitle":")" + secret + R"("}])";
    const QByteArray bad = secret + " is not json";
    const QByteArray wrong = R"({"title":")" + secret + R"("})";
    const QByteArray cut = R"([{"pid":500,"title":")" + secret;
    for (const QByteArray& reply : {good, bad, wrong, cut}) {
        const windows::Query q = windows::parseHyprlandClients(reply, limits);
        QVERIFY2(!q.detail.contains(QString::fromLatin1(secret)), qPrintable(q.detail));
        windows::Result r;
        r.status = q.status;
        r.detail = q.detail;
        QVERIFY2(!windows::describe(r).contains(QString::fromLatin1(secret)), qPrintable(windows::describe(r)));
    }
}

// ---------------------------------------------------------------------------
// The socket request

void TestProcessGroups::hyprlandSocketQueryIsBounded_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<QByteArray>("reply");
    QTest::addColumn<int>("maxBytes");
    QTest::addColumn<QList<int>>("statuses");
    QTest::addColumn<QList<int>>("pids");
    QTest::addColumn<bool>("waitsForDeadline"); // must take (about) the whole deadline
    using M = FakeSocketServer::Mode;
    const QByteArray list = R"([{"pid":500,"title":"a"},{"pid":42,"title":"b"},{"pid":500}])";
    QTest::newRow("reply and close") << int(M::Reply) << list << 0 << QList<int> {int(Status::Ok)} << QList<int> {42, 500}
                                     << false;
    QTest::newRow("empty list") << int(M::Reply) << QByteArray("[]") << 0 << QList<int> {int(Status::Ok)} << QList<int> {}
                                << false;
    QTest::newRow("never replies") << int(M::Silent) << QByteArray() << 0 << QList<int> {int(Status::Timeout)}
                                   << QList<int> {} << true;
    QTest::newRow("closes without replying") << int(M::CloseWithoutReply) << QByteArray() << 0
                                             << QList<int> {int(Status::Disconnected)} << QList<int> {} << false;
    QTest::newRow("half a reply, then silence") << int(M::PartialThenHold) << list << 0 << QList<int> {int(Status::Timeout)}
                                                << QList<int> {} << true;
    QTest::newRow("half a reply, then close") << int(M::PartialThenClose) << list << 0
                                              << QList<int> {int(Status::Truncated), int(Status::Malformed)} << QList<int> {}
                                              << false;
    // One byte every 40 ms: the deadline is for the whole request, not
    // for each read.
    QTest::newRow("drips a byte at a time") << int(M::Drip) << QByteArray() << 0 << QList<int> {int(Status::Timeout)}
                                            << QList<int> {} << true;
    QByteArray big = "[";
    for (int i = 0; i < 20000; ++i)
        big += R"({"pid":500,"title":"xxxxxxxxxxxxxxxxxxxxxxxx"},)";
    big += R"({"pid":1}])";
    QTest::newRow("reply over the byte limit") << int(M::Reply) << big << 4096 << QList<int> {int(Status::TooLarge)}
                                               << QList<int> {} << false;
}

void TestProcessGroups::hyprlandSocketQueryIsBounded()
{
    QFETCH(int, mode);
    QFETCH(QByteArray, reply);
    QFETCH(int, maxBytes);
    QFETCH(QList<int>, statuses);
    QFETCH(QList<int>, pids);
    QFETCH(bool, waitsForDeadline);
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral(".socket.sock"));
    FakeSocketServer server(path, FakeSocketServer::Mode(mode), reply);
    QVERIFY(server.ok());
    windows::Limits limits;
    limits.deadlineMs = 300;
    if (maxBytes > 0)
        limits.maxBytes = maxBytes;
    QElapsedTimer t;
    t.start();
    const windows::Query q = windows::queryHyprland(path.toStdString(), limits, -1);
    const qint64 ms = t.elapsed();
    qInfo("%s after %lld ms (%s)", windows::statusId(q.status), ms, qPrintable(q.detail));
    QVERIFY2(statuses.contains(int(q.status)), qPrintable(QStringLiteral("status %1 (%2)").arg(windows::statusId(q.status), q.detail)));
    QCOMPARE(QList<int>(q.pids.begin(), q.pids.end()), pids);
    QCOMPARE(server.request(), QByteArray("j/clients"));
    // Bounded: never much past the deadline, and not given up early.
    QVERIFY2(ms <= limits.deadlineMs + 200, qPrintable(QStringLiteral("%1 ms for a %2 ms deadline").arg(ms).arg(limits.deadlineMs)));
    if (waitsForDeadline)
        QVERIFY2(ms >= limits.deadlineMs - 30, qPrintable(QStringLiteral("gave up after %1 ms").arg(ms)));
}

void TestProcessGroups::hyprlandSocketQueryCancels()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral(".socket.sock"));
    FakeSocketServer server(path, FakeSocketServer::Mode::Silent);
    QVERIFY(server.ok());
    const int cancel = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    QVERIFY(cancel >= 0);
    windows::Limits limits;
    limits.deadlineMs = 5000;
    std::thread canceller([cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        const uint64_t one = 1;
        [[maybe_unused]] auto n = write(cancel, &one, sizeof one);
    });
    QElapsedTimer t;
    t.start();
    const windows::Query q = windows::queryHyprland(path.toStdString(), limits, cancel);
    const qint64 ms = t.elapsed();
    canceller.join();
    close(cancel);
    QCOMPARE(windows::statusId(q.status), windows::statusId(Status::Cancelled));
    QVERIFY2(ms < 1000, qPrintable(QStringLiteral("cancel took %1 ms").arg(ms)));
    QVERIFY(q.pids.empty());
}

void TestProcessGroups::hyprlandSocketUnreachable()
{
    QTemporaryDir dir;
    windows::Limits limits;
    limits.deadlineMs = 300;
    // No socket at all.
    windows::Query q = windows::queryHyprland(dir.filePath(QStringLiteral("absent.sock")).toStdString(), limits, -1);
    QCOMPARE(windows::statusId(q.status), windows::statusId(Status::NotRunning));
    // A socket file left behind by a compositor that exited.
    const QString stale = dir.filePath(QStringLiteral("stale.sock"));
    {
        const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        const QByteArray p = QFile::encodeName(stale);
        std::memcpy(addr.sun_path, p.constData(), std::size_t(p.size()));
        QVERIFY(bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        close(s);
    }
    QElapsedTimer t;
    t.start();
    q = windows::queryHyprland(stale.toStdString(), limits, -1);
    QCOMPARE(windows::statusId(q.status), windows::statusId(Status::NotRunning));
    QVERIFY(t.elapsed() < 300);
    // A socket this user may not connect to.
    if (geteuid() != 0) {
        const QString locked = dir.filePath(QStringLiteral("locked.sock"));
        FakeSocketServer server(locked, FakeSocketServer::Mode::Reply, "[]");
        QVERIFY(server.ok());
        QVERIFY(chmod(QFile::encodeName(locked).constData(), 0) == 0);
        q = windows::queryHyprland(locked.toStdString(), limits, -1);
        QCOMPARE(windows::statusId(q.status), windows::statusId(Status::PermissionDenied));
        QVERIFY(q.pids.empty());
    }
    // A regular file where the socket should be.
    const QString file = dir.filePath(QStringLiteral("file.sock"));
    writeFile(file, "[]");
    q = windows::queryHyprland(file.toStdString(), limits, -1);
    QVERIFY2(q.status != Status::Ok, windows::statusId(q.status));
    QVERIFY(q.pids.empty());
}

void TestProcessGroups::resolveOwnersPinsIdentity()
{
    FakeProc fp;
    windows::Result r;
    r.status = Status::Ok;
    // The list was requested at 1000 s. 500 started at 1 s (owner), 501 at
    // 2000 s (its PID was reused after the list was taken), 502 is gone, 503
    // is a kernel thread, 504 started at 3 s behind a hostile name.
    windows::resolveOwners({500, 501, 502, 503, 504}, 1000.0, fp.root, 100, r);
    QList<QPair<int, qulonglong>> owners;
    for (const windows::Owner& o : r.owners)
        owners << qMakePair(o.pid, qulonglong(o.startTicks));
    QCOMPARE(owners, (QList<QPair<int, qulonglong>> {{500, 100}, {504, 300}}));
    QCOMPARE(r.ambiguous, 1);
    QCOMPARE(r.unresolved, 2);
    // Unsorted and repeated input gives a sorted, unique list.
    windows::Result r2;
    windows::resolveOwners({504, 500, 504, 500}, 1000.0, fp.root, 100, r2);
    QCOMPARE(r2.owners.size(), std::size_t(2));
    QVERIFY(r2.owners[0] < r2.owners[1]);
}

// ---------------------------------------------------------------------------
// The probe thread

void TestProcessGroups::probeDeliversPinnedOwners()
{
    FakeProc fp;
    QTemporaryDir run;
    windows::WindowProbe probe(probeOptions(fp, run.path(), [](const windows::QueryContext& c) {
        Q_ASSERT(c.selection.backend == windows::Backend::Hyprland);
        auto q = okQuery({500, 501, 502, 504});
        q.windows = 6;
        q.withoutPid = 2;
        return q;
    }));
    QSignalSpy spy(&probe, &windows::WindowProbe::resultReady);
    probe.start(true);
    QVERIFY(spy.wait(2000));
    const auto r = spy.first().first().value<windows::ResultPtr>();
    QVERIFY(r);
    QCOMPARE(windows::statusId(r->status), windows::statusId(Status::Ok));
    QCOMPARE(int(r->backend), int(windows::Backend::Hyprland));
    QCOMPARE(r->generation, probe.generation());
    QCOMPARE(r->windows, 6);
    QCOMPARE(r->withoutPid, 2);
    QCOMPARE(r->owners.size(), std::size_t(2));
    QCOMPARE(r->owners[0], (windows::Owner {500, 100}));
    QCOMPARE(r->owners[1], (windows::Owner {504, 300}));
    QCOMPARE(r->ambiguous, 1);
    QCOMPARE(r->unresolved, 1);
    QVERIFY(std::isfinite(r->requested) && std::isfinite(r->time));
    QVERIFY(r->time >= r->requested);
    QVERIFY(r->validFor > 0);
    QVERIFY(probe.stop(2000));
}

void TestProcessGroups::probeKeepsOneRequestInFlight()
{
    FakeProc fp;
    QTemporaryDir run;
    auto inFlight = std::make_shared<std::atomic<int>>(0);
    auto most = std::make_shared<std::atomic<int>>(0);
    auto calls = std::make_shared<std::atomic<int>>(0);
    windows::WindowProbe probe(probeOptions(fp, run.path(), [=](const windows::QueryContext& c) {
        const int now = ++*inFlight;
        int m = most->load();
        while (now > m && !most->compare_exchange_weak(m, now)) {
        }
        ++*calls;
        pollfd p {c.cancelFd, POLLIN, 0};
        poll(&p, c.cancelFd >= 0 ? 1 : 0, 150); // a slow compositor
        --*inFlight;
        return okQuery({500});
    }));
    QSignalSpy spy(&probe, &windows::WindowProbe::resultReady);
    probe.start(true);
    // Page changes, visibility flips and pause/resume while requests run.
    for (int i = 0; i < 40; ++i) {
        probe.setVisible(i % 2 == 0);
        if (i % 7 == 3)
            probe.setPaused(true);
        if (i % 7 == 5)
            probe.setPaused(false);
        QTest::qWait(15);
    }
    probe.setPaused(false);
    probe.setVisible(true);
    QTest::qWait(600);
    QVERIFY(probe.stop(2000));
    qInfo("%d requests, at most %d at once, %lld results", calls->load(), most->load(), qlonglong(spy.size()));
    QCOMPARE(most->load(), 1);
    QVERIFY(calls->load() >= 2);
    // Results are delivered in request order, never an older one after a newer.
    uint64_t last = 0;
    for (const auto& args : spy) {
        const auto r = args.first().value<windows::ResultPtr>();
        QVERIFY(r->sequence > last);
        last = r->sequence;
    }
}

void TestProcessGroups::probeDropsResultsFromBeforePause()
{
    FakeProc fp;
    QTemporaryDir run;
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto calls = std::make_shared<std::atomic<int>>(0);
    windows::WindowProbe probe(probeOptions(fp, run.path(), [=](const windows::QueryContext&) {
        const int n = ++*calls;
        // The first request is slow; it finishes after the pause.
        while (n == 1 && !release->load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return okQuery(n == 1 ? std::vector<int> {504} : std::vector<int> {500});
    }));
    QSignalSpy spy(&probe, &windows::WindowProbe::resultReady);
    probe.start(true);
    QTRY_COMPARE_WITH_TIMEOUT(calls->load(), 1, 2000);
    const quint64 before = probe.generation();
    probe.setPaused(true);
    QVERIFY(probe.isPaused());
    release->store(true);
    QTest::qWait(400);
    QCOMPARE(spy.size(), 0); // nothing while paused
    probe.setPaused(false);
    QVERIFY(probe.generation() != before);
    QVERIFY(spy.wait(2000));
    QTest::qWait(200);
    QVERIFY(probe.stop(2000));
    for (const auto& args : spy) {
        const auto r = args.first().value<windows::ResultPtr>();
        QVERIFY2(r->generation != before, "a result asked for before the pause was delivered after it");
        QVERIFY2(!(r->owners.size() == 1 && r->owners[0].pid == 504), "the pre-pause list came back");
    }
}

void TestProcessGroups::probeStopCancelsBlockedRequest()
{
    FakeProc fp;
    QTemporaryDir run;
    auto started = std::make_shared<std::atomic<bool>>(false);
    auto sawCancel = std::make_shared<std::atomic<bool>>(false);
    windows::WindowProbe probe(probeOptions(fp, run.path(), [=](const windows::QueryContext& c) {
        started->store(true);
        pollfd p {c.cancelFd, POLLIN, 0};
        const int n = poll(&p, 1, 10000); // blocks until cancelled
        sawCancel->store(n > 0);
        windows::Query q;
        q.status = Status::Cancelled;
        q.detail = QStringLiteral("cancelled");
        return q;
    }));
    QSignalSpy spy(&probe, &windows::WindowProbe::resultReady);
    probe.start(true);
    QTRY_VERIFY_WITH_TIMEOUT(started->load(), 2000);
    // outstandingSequence() counts a delivered, untaken result, not a request
    // in progress: nothing has been delivered yet.
    QCOMPARE(probe.outstandingSequence(), uint64_t(0));
    QElapsedTimer t;
    t.start();
    QVERIFY(probe.stop(1000));
    QVERIFY2(t.elapsed() < 1000, qPrintable(QStringLiteral("stop took %1 ms").arg(t.elapsed())));
    QVERIFY(sawCancel->load());
    QVERIFY(probe.isStopped());
    QVERIFY(!probe.isRunning());
    QTest::qWait(100);
    QCOMPARE(spy.size(), 0);
}

void TestProcessGroups::probeDoesNotRetryFinalStatus()
{
    FakeProc fp;
    // The injected query replaces the backend and is handed the selection;
    // like the real one it answers an unusable selection with its status.
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto seen = std::make_shared<std::atomic<int>>(-1);
    windows::ProbeOptions o = probeOptions(fp, QString(), [=](const windows::QueryContext& c) {
        ++*calls;
        seen->store(int(c.selection.status));
        if (c.selection.status != Status::Ok) {
            windows::Query q;
            q.status = c.selection.status;
            q.detail = c.selection.detail;
            return q;
        }
        return okQuery({500});
    });
    windows::Environment e; // another Wayland compositor
    e.waylandDisplay = "wayland-0";
    e.sessionType = "wayland";
    e.display = ":1";
    e.runtimeDir = "/run/user/1000";
    o.environment = e;
    windows::WindowProbe probe(o);
    QSignalSpy spy(&probe, &windows::WindowProbe::resultReady);
    probe.start(true);
    QVERIFY(spy.wait(2000));
    QTest::qWait(600); // ten visible intervals
    const auto r = spy.first().first().value<windows::ResultPtr>();
    QCOMPARE(windows::statusId(Status(seen->load())), windows::statusId(Status::Unsupported));
    QCOMPARE(windows::statusId(r->status), windows::statusId(Status::Unsupported));
    QVERIFY(r->owners.empty());
    QCOMPARE(calls->load(), 1);
    QVERIFY(!windows::describe(*r).isEmpty());
    QCOMPARE(spy.size(), 1);
    QVERIFY(probe.stop(2000));
}

void TestProcessGroups::probeBacksOffAfterFailures()
{
    FakeProc fp;
    QTemporaryDir run;
    auto times = std::make_shared<std::vector<qint64>>();
    auto mutex = std::make_shared<std::mutex>();
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    windows::WindowProbe probe(probeOptions(fp, run.path(), [=](const windows::QueryContext&) {
        {
            std::lock_guard<std::mutex> l(*mutex);
            times->push_back(clock->elapsed());
        }
        windows::Query q;
        q.status = Status::Timeout;
        q.detail = QStringLiteral("timeout");
        return q;
    }));
    QSignalSpy spy(&probe, &windows::WindowProbe::resultReady);
    probe.start(true);
    QTest::qWait(2000);
    QVERIFY(probe.stop(2000));
    std::vector<qint64> t;
    {
        std::lock_guard<std::mutex> l(*mutex);
        t = *times;
    }
    QStringList gaps;
    for (std::size_t i = 1; i < t.size(); ++i)
        gaps << QString::number(t[i] - t[i - 1]);
    qInfo("%zu failing requests in 2 s, gaps %s ms", t.size(), qPrintable(gaps.join(QLatin1Char(' '))));
    // Every failure is reported (the view says why), but asking slows down:
    // at a steady 60 ms there would be about 33 requests.
    QVERIFY(t.size() >= 3);
    QVERIFY2(t.size() <= 15, "no back-off after repeated failures");
    QVERIFY2(t.back() - t[t.size() - 2] >= 150, "the last gap did not grow");
    for (std::size_t i = 1; i < t.size(); ++i)
        QVERIFY2(t[i] - t[i - 1] <= 400 + 250, "a gap exceeded the maximum back-off");
    for (const auto& args : spy)
        QVERIFY(!args.first().value<windows::ResultPtr>()->ok());
}

// ---------------------------------------------------------------------------
// Classification

void TestProcessGroups::groupForClassifiesByIdentity()
{
    const auto app = proc(500, 100, "editor", 1);
    const auto reused = proc(500, 900, "editor", 1); // same PID, later start
    const auto daemon = proc(600, 100, "daemon", 1);
    const auto kthread = proc(2, 1, "kthreadd", 0, 0, true);
    const ProcessGrouping g = detected({{500, 100}, {2, 1}});
    QCOMPARE(int(ProcessModel::groupFor(app, g)), int(ProcessModel::Apps));
    QCOMPARE(int(ProcessModel::groupFor(reused, g)), int(ProcessModel::Background));
    QCOMPARE(int(ProcessModel::groupFor(daemon, g)), int(ProcessModel::Background));
    // A kernel thread never owns a window, whatever a list says.
    QCOMPARE(int(ProcessModel::groupFor(kthread, g)), int(ProcessModel::Kernel));
    // A complete, empty window list: every user process is background.
    const ProcessGrouping none = detected({});
    QCOMPARE(int(ProcessModel::groupFor(app, none)), int(ProcessModel::Background));
    QCOMPARE(int(ProcessModel::groupFor(kthread, none)), int(ProcessModel::Kernel));
    QVERIFY(ProcessModel::groupTitle(ProcessModel::Apps, g).contains(QLatin1String("Apps"), Qt::CaseInsensitive));
    QVERIFY(ProcessModel::groupTitle(ProcessModel::Background, g).contains(QLatin1String("Background"), Qt::CaseInsensitive));
    QVERIFY(ProcessModel::groupTitle(ProcessModel::Kernel, g).contains(QLatin1String("Kernel"), Qt::CaseInsensitive));
}

void TestProcessGroups::unavailableGroupingNeverClaimsBackground()
{
    const auto app = proc(500, 100, "editor", 1);
    const auto kthread = proc(2, 1, "kthreadd", 0, 0, true);
    const QString reason = QStringLiteral("App detection is unavailable: test compositor");
    ProcessGrouping pending;
    pending.state = ProcessGrouping::State::Pending;
    for (const ProcessGrouping& g : {unavailable(reason), pending}) {
        const auto group = ProcessModel::groupFor(app, g);
        QVERIFY2(group != ProcessModel::Apps, "an unknown process was put in Apps");
        // Where they are shown, the title must not call them background.
        const QString title = ProcessModel::groupTitle(group, g);
        QVERIFY2(!title.contains(QLatin1String("Background"), Qt::CaseInsensitive), qPrintable(title));
        // Kernel still works.
        QCOMPARE(int(ProcessModel::groupFor(kthread, g)), int(ProcessModel::Kernel));
        // Apps is not shown as a counted, empty success ("Apps (0)").
        QVERIFY2(!ProcessModel::groupCounted(ProcessModel::Apps, g), "Apps counted without a window list");
    }
    // The reason reaches the Apps header.
    QVERIFY2(ProcessModel::groupToolTip(ProcessModel::Apps, unavailable(reason)).contains(reason),
             qPrintable(ProcessModel::groupToolTip(ProcessModel::Apps, unavailable(reason))));
    ProcessModel m;
    m.setGrouping(unavailable(reason));
    m.setProcesses({app, kthread});
    QCOMPARE(m.groupSize(ProcessModel::Apps), 0);
    QCOMPARE(m.groupSize(ProcessModel::Kernel), 1);
    const QString shown = m.groupIndex(ProcessModel::Background).data().toString() +
                          m.groupIndex(ProcessModel::Apps).data().toString();
    QVERIFY2(!shown.contains(QLatin1String("Background"), Qt::CaseInsensitive) || m.groupSize(ProcessModel::Background) == 0,
             qPrintable(shown));
}

// ---------------------------------------------------------------------------
// The grouped model

void TestProcessGroups::modelPassesItemModelTesterThroughGroupMoves()
{
    ProcessModel m;
    ProcessFilterModel proxy;
    proxy.setSourceModel(&m);
    proxy.setDynamicSortFilter(true);
    proxy.sort(ProcessModel::Cpu, Qt::DescendingOrder);
    QAbstractItemModelTester t1(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QAbstractItemModelTester t2(&proxy, QAbstractItemModelTester::FailureReportingMode::QtTest);

    QCOMPARE(m.rowCount(), 3); // the groups exist before any process
    QCOMPARE(groupOrder(m), (QList<int> {ProcessModel::Apps, ProcessModel::Background, ProcessModel::Kernel}));
    const auto a = proc(500, 100, "editor", 5), b = proc(600, 100, "fox", 50), k = proc(2, 1, "kthreadd", 0, 0, true),
               c = proc(700, 100, "nan", ps::kNaN);
    m.setGrouping(unavailable(QStringLiteral("unknown")));
    m.setProcesses({a, b, k, c});
    QCOMPARE(m.processCount(), 4);
    QCOMPARE(m.groupSize(ProcessModel::Apps), 0);
    QCOMPARE(m.groupSize(ProcessModel::Kernel), 1);

    m.setGrouping(detected({{500, 100}}));
    QCOMPARE(rowsOf(m).size(), 4);
    QCOMPARE(pidsIn(m, 0), QList<int> {500});
    QCOMPARE(pidsIn(proxy, 1), (QList<int> {600, 700})); // CPU descending, unavailable last

    m.setGrouping(detected({{600, 100}}));
    QCOMPARE(pidsIn(m, 0), QList<int> {600});
    QCOMPARE(m.groupSize(ProcessModel::Background), 2);

    // 500 exits and its PID is reused; the old Apps key must not match.
    auto reused = proc(500, 300, "other", 90);
    m.setGrouping(detected({{500, 100}, {600, 100}}));
    m.setProcesses({reused, b, k, c});
    QCOMPARE(pidsIn(m, 0), QList<int> {600});
    QVERIFY(pidsIn(m, 1).contains(500));
    QCOMPARE(pidsIn(proxy, 1), (QList<int> {500, 700}));

    // A process moves between all three groups over refreshes.
    auto kernelReuse = proc(700, 999, "kworker", 0, 0, true);
    m.setProcesses({reused, b, k, kernelReuse});
    QCOMPARE(m.groupSize(ProcessModel::Kernel), 2);
    m.setGrouping(detected({{500, 300}}));
    QCOMPARE(pidsIn(m, 0), QList<int> {500});
    QCOMPARE(pidsIn(m, 1), QList<int> {600});
    m.setProcesses({});
    QCOMPARE(m.processCount(), 0);
    QCOMPARE(m.rowCount(), 3);
    m.setProcesses({a, b, k});
    m.clear();
    QCOMPARE(m.rowCount(), 3);
    QCOMPARE(m.processCount(), 0);
}

void TestProcessGroups::modelMovesRowsInsteadOfResetting()
{
    ProcessModel m;
    ProcessFilterModel proxy;
    proxy.setSourceModel(&m);
    proxy.setDynamicSortFilter(true);
    proxy.sort(ProcessModel::Cpu, Qt::DescendingOrder);
    const auto a = proc(500, 100, "editor", 5), b = proc(600, 100, "fox", 50), k = proc(2, 1, "kthreadd", 0, 0, true);
    m.setGrouping(detected({}));
    m.setProcesses({a, b, k});
    QSignalSpy resets(&m, &QAbstractItemModel::modelAboutToBeReset);
    auto find = [&](const QAbstractItemModel& model, int pid) {
        for (int g = 0; g < model.rowCount(); ++g)
            for (int r = 0; r < model.rowCount(model.index(g, 0)); ++r) {
                const QModelIndex i = model.index(r, ProcessModel::Cpu, model.index(g, 0));
                if (i.data(ProcessModel::PidRole).toInt() == pid)
                    return i;
            }
        return QModelIndex();
    };
    QPersistentModelIndex src(find(m, 500)), view(find(proxy, 500));
    QVERIFY(src.isValid() && view.isValid());
    m.setGrouping(detected({{500, 100}}));
    QVERIFY2(src.isValid() && view.isValid(), "the process row was removed and re-inserted, not moved");
    QCOMPARE(src.parent(), m.groupIndex(ProcessModel::Apps));
    QCOMPARE(src.data(ProcessModel::PidRole).toInt(), 500);
    QCOMPARE(src.data(ProcessModel::StartTicksRole).toULongLong(), qulonglong(100));
    QCOMPARE(src.column(), int(ProcessModel::Cpu));
    QCOMPARE(view.data(ProcessModel::PidRole).toInt(), 500);
    QCOMPARE(view.parent().data(ProcessModel::GroupRole).toInt(), int(ProcessModel::Apps));
    // A value refresh keeps the row.
    auto hotter = a;
    hotter.cpuPercent = 99;
    m.setProcesses({hotter, b, k});
    QVERIFY(src.isValid());
    QCOMPARE(src.data(ProcessModel::PidRole).toInt(), 500);
    // Back to Background, and the PID reused: the old row goes away.
    m.setGrouping(detected({}));
    QCOMPARE(src.parent(), m.groupIndex(ProcessModel::Background));
    m.setProcesses({proc(500, 400, "other", 1), b, k});
    QVERIFY2(!src.isValid() || src.data(ProcessModel::StartTicksRole).toULongLong() != 100,
             "the reused PID kept the old process's row");
    QCOMPARE(resets.size(), 0);
    // Group rows are never process rows.
    for (int g = 0; g < ProcessModel::GroupCount; ++g) {
        const QModelIndex gi = m.groupIndex(ProcessModel::Group(g));
        QVERIFY(gi.data(ProcessModel::IsGroupRole).toBool());
        QVERIFY(m.process(gi) == nullptr);
        QVERIFY(!gi.parent().isValid());
    }
}

void TestProcessGroups::modelShowsEachIdentityOnce()
{
    ProcessModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
    const auto a = proc(500, 100, "editor", 5);
    m.setGrouping(detected({{500, 100}}));
    m.setProcesses({a, a, proc(500, 200, "reused", 1)});
    QCOMPARE(m.processCount(), 2);
    QCOMPARE(m.groupSize(ProcessModel::Apps), 1);
    QCOMPARE(m.groupSize(ProcessModel::Background), 1);
    const QList<Row> rows = rowsOf(m);
    QCOMPARE(rows.size(), 2);
    QVERIFY2(rows[0] == (Row {ProcessModel::Apps, 500, 100}), qPrintable(describeRows(rows)));
}

void TestProcessGroups::proxyKeepsGroupOrderWhenSorting()
{
    ProcessModel m;
    ProcessFilterModel proxy;
    proxy.setSourceModel(&m);
    proxy.setDynamicSortFilter(true);
    QAbstractItemModelTester tester(&proxy, QAbstractItemModelTester::FailureReportingMode::QtTest);
    m.setGrouping(detected({{10, 1}, {11, 1}, {12, 1}}));
    m.setProcesses({proc(10, 1, "b-app", 30, 5e6), proc(11, 1, "a-app", 2, 9e6), proc(12, 1, "c-app", ps::kNaN, 1e6),
                    proc(20, 1, "zz-bg", 70, 2e6), proc(21, 1, "aa-bg", 1, 3e6), proc(2, 1, "kthreadd", 0, 0, true),
                    proc(3, 1, "rcu", 0.5, 0, true)});
    const QList<int> fixed {ProcessModel::Apps, ProcessModel::Background, ProcessModel::Kernel};
    for (int col : {int(ProcessModel::Name), int(ProcessModel::Pid), int(ProcessModel::Cpu), int(ProcessModel::Memory)})
        for (Qt::SortOrder order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
            proxy.sort(col, order);
            QCOMPARE(groupOrder(proxy), fixed);
        }
    proxy.sort(ProcessModel::Cpu, Qt::DescendingOrder);
    QCOMPARE(pidsIn(proxy, 0), (QList<int> {10, 11, 12})); // unavailable last
    QCOMPARE(pidsIn(proxy, 1), (QList<int> {20, 21}));
    QCOMPARE(pidsIn(proxy, 2), (QList<int> {3, 2}));
    proxy.sort(ProcessModel::Cpu, Qt::AscendingOrder);
    QCOMPARE(pidsIn(proxy, 0), (QList<int> {12, 11, 10}));
    proxy.sort(ProcessModel::Pid, Qt::DescendingOrder);
    QCOMPARE(pidsIn(proxy, 1), (QList<int> {21, 20})); // numbers, not text
    proxy.sort(ProcessModel::Name, Qt::AscendingOrder);
    QCOMPARE(pidsIn(proxy, 0), (QList<int> {11, 10, 12}));
    // Sorting follows values after a refresh, and groups stay fixed.
    proxy.sort(ProcessModel::Cpu, Qt::DescendingOrder);
    m.setProcesses({proc(10, 1, "b-app", 1, 5e6), proc(11, 1, "a-app", 80, 9e6), proc(12, 1, "c-app", 40, 1e6),
                    proc(20, 1, "zz-bg", 70, 2e6), proc(21, 1, "aa-bg", 1, 3e6), proc(2, 1, "kthreadd", 0, 0, true),
                    proc(3, 1, "rcu", 0.5, 0, true)});
    QCOMPARE(groupOrder(proxy), fixed);
    QCOMPARE(pidsIn(proxy, 0), (QList<int> {11, 12, 10}));
}

void TestProcessGroups::proxyFilterCountsAndKeepsGroups()
{
    ProcessModel m;
    ProcessFilterModel proxy;
    proxy.setSourceModel(&m);
    proxy.setDynamicSortFilter(true);
    QAbstractItemModelTester tester(&proxy, QAbstractItemModelTester::FailureReportingMode::QtTest);
    m.setGrouping(detected({{10, 1}, {11, 1}, {12, 1}}));
    m.setProcesses({proc(10, 1, "firefox", 30), proc(11, 1, "editor", 2), proc(12, 1, "term", 1), proc(4242, 1, "fox-helper", 1),
                    proc(21, 1, "daemon", 1), proc(2, 1, "kthreadd", 0, 0, true)});
    proxy.setFilterString(QStringLiteral("FOX"));
    QVERIFY(proxy.isFiltering());
    QCOMPARE(groupOrder(proxy), (QList<int> {ProcessModel::Apps, ProcessModel::Background, ProcessModel::Kernel}));
    QCOMPARE(proxy.visibleInGroup(ProcessModel::Apps), 1);
    QCOMPARE(proxy.visibleInGroup(ProcessModel::Background), 1);
    QCOMPARE(proxy.visibleInGroup(ProcessModel::Kernel), 0);
    QCOMPARE(proxy.visibleProcessCount(), 2);
    QCOMPARE(pidsIn(proxy, 0), QList<int> {10});
    QCOMPARE(pidsIn(proxy, 2), QList<int> {});
    const QString appsTitle = proxy.groupIndex(ProcessModel::Apps).data().toString();
    QVERIFY2(appsTitle.contains(QLatin1String("1 of 3")), qPrintable(appsTitle));
    // Numbers match PIDs; the group names are not process text.
    proxy.setFilterString(QStringLiteral("4242"));
    QCOMPARE(proxy.visibleProcessCount(), 1);
    QCOMPARE(pidsIn(proxy, 1), QList<int> {4242});
    for (const char* groupWord : {"apps", "background", "kernel processes"}) {
        proxy.setFilterString(QString::fromLatin1(groupWord));
        QVERIFY2(proxy.visibleProcessCount() == 0, groupWord);
        QCOMPARE(proxy.rowCount(), 3);
    }
    // A process moving into Apps while filtered is counted there.
    proxy.setFilterString(QStringLiteral("daemon"));
    m.setGrouping(detected({{21, 1}}));
    QCOMPARE(proxy.visibleInGroup(ProcessModel::Apps), 1);
    QCOMPARE(pidsIn(proxy, 0), QList<int> {21});
    proxy.setFilterString(QString());
    QVERIFY(!proxy.isFiltering());
    QCOMPARE(proxy.visibleProcessCount(), 6);
    QVERIFY(!proxy.groupIndex(ProcessModel::Apps).data().toString().contains(QLatin1String(" of ")));
}

void TestProcessGroups::staleModelKeepsGroupsAndIdentity()
{
    ProcessModel m;
    QAbstractItemModelTester tester(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
    m.setGrouping(detected({{500, 100}}));
    m.setProcesses({proc(500, 100, "editor", 12.5, 3e8), proc(600, 100, "daemon", 1), proc(2, 1, "kthreadd", 0, 0, true)});
    const QModelIndex cpu = m.index(0, ProcessModel::Cpu, m.groupIndex(ProcessModel::Apps));
    QVERIFY(cpu.data().toString().contains(QLatin1String("12.5")));
    m.setStale(true);
    QCOMPARE(m.groupSize(ProcessModel::Apps), 1);
    QCOMPARE(m.groupSize(ProcessModel::Background), 1);
    QCOMPARE(m.groupSize(ProcessModel::Kernel), 1);
    const QModelIndex again = m.index(0, ProcessModel::Cpu, m.groupIndex(ProcessModel::Apps));
    QCOMPARE(again.data(ProcessModel::PidRole).toInt(), 500);
    QVERIFY2(!again.data().toString().contains(QLatin1String("12.5")), qPrintable(again.data().toString()));
    QCOMPARE(m.index(0, ProcessModel::Name, m.groupIndex(ProcessModel::Apps)).data().toString(), QStringLiteral("editor"));
    m.setStale(false);
    QVERIFY(m.index(0, ProcessModel::Cpu, m.groupIndex(ProcessModel::Apps)).data().toString().contains(QLatin1String("12.5")));
}

// ---------------------------------------------------------------------------
// SYS008: a compositor too busy to accept (its listen backlog is full) is
// busy, not "another user", and it recovers once the backlog drains. Only a
// listener owned by this test is involved.

namespace {

// A listening socket with a backlog of 0, filled by one queued client.
struct FullBacklog {
    QString path;
    int listener = -1;
    int filler = -1;
    bool full = false;
    QString problem;
    explicit FullBacklog(const QString& socketPath)
        : path(socketPath)
    {
        QDir().mkpath(QFileInfo(path).path());
        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        const QByteArray p = QFile::encodeName(path);
        std::memcpy(addr.sun_path, p.constData(), std::size_t(p.size()));
        listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener < 0 || ::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0
            || ::listen(listener, 0) != 0) {
            problem = QStringLiteral("listener: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
            return;
        }
        filler = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (::connect(filler, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            problem = QStringLiteral("filler: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
            return;
        }
        // The backlog must now refuse another connection with EAGAIN.
        const int extra = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        const int rc = ::connect(extra, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        const int err = errno;
        ::close(extra);
        full = rc != 0 && err == EAGAIN;
        if (!full)
            problem = QStringLiteral("backlog not full: connect %1 (%2)").arg(rc).arg(QString::fromLocal8Bit(std::strerror(err)));
    }
    // Accept and drop the queued client: the backlog is free again.
    bool drain()
    {
        const int c = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0)
            return false;
        ::close(c);
        ::close(filler);
        filler = -1;
        return true;
    }
    ~FullBacklog()
    {
        if (filler >= 0)
            ::close(filler);
        if (listener >= 0)
            ::close(listener);
    }
};

// Answers every connection on `listener` with `reply` until stopped.
struct Answerer {
    std::atomic<bool> stop {false};
    std::atomic<int> served {0};
    std::thread thread;
    Answerer(int listener, QByteArray reply)
        : thread([this, listener, reply] {
            while (!stop.load()) {
                pollfd p {listener, POLLIN, 0};
                if (::poll(&p, 1, 50) <= 0)
                    continue;
                const int c = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
                if (c < 0)
                    continue;
                char buf[64];
                pollfd r {c, POLLIN, 0};
                if (::poll(&r, 1, 500) > 0)
                    [[maybe_unused]] const ssize_t n = ::recv(c, buf, sizeof buf, 0);
                [[maybe_unused]] const ssize_t w = ::send(c, reply.constData(), std::size_t(reply.size()), MSG_NOSIGNAL);
                ::close(c);
                ++served;
            }
        })
    {
    }
    ~Answerer()
    {
        stop.store(true);
        thread.join();
    }
};

} // namespace

void TestProcessGroups::hyprlandFullBacklogIsBusyNotAnotherUser()
{
    QTemporaryDir dir;
    FullBacklog backlog(dir.filePath(QStringLiteral("hypr.sock")));
    QVERIFY2(backlog.full, qPrintable(backlog.problem));
    windows::Limits limits;
    limits.deadlineMs = 300;
    QElapsedTimer t;
    t.start();
    const windows::Query busy = windows::queryHyprland(backlog.path.toStdString(), limits, -1);
    const qint64 ms = t.elapsed();
    qInfo("full backlog: %s after %lld ms (%s)", windows::statusId(busy.status), static_cast<long long>(ms),
          qPrintable(busy.detail));
    QVERIFY2(busy.status != Status::OtherUser && busy.status != Status::OtherNamespace,
             qPrintable(QStringLiteral("a full backlog was reported as %1: \"%2\"")
                            .arg(QLatin1String(windows::statusId(busy.status)), busy.detail)));
    QVERIFY2(!windows::isFinal(busy.status), windows::statusId(busy.status));
    QVERIFY(busy.status != Status::Ok);
    QVERIFY(busy.pids.empty());
    QVERIFY2(ms <= limits.deadlineMs + 200, qPrintable(QString::number(ms)));

    // The compositor catches up: the next request works.
    QVERIFY(backlog.drain());
    const QByteArray reply = "[{\"pid\":" + QByteArray::number(::getpid()) + "}]";
    Answerer answer(backlog.listener, reply);
    const windows::Query ok = windows::queryHyprland(backlog.path.toStdString(), limits, -1);
    QCOMPARE(windows::statusId(ok.status), windows::statusId(Status::Ok));
    QCOMPARE(QList<int>(ok.pids.begin(), ok.pids.end()), QList<int> {int(::getpid())});
}

void TestProcessGroups::probeRetriesAfterFullBacklog()
{
    // The real Hyprland backend (no injected query) at a test socket. Only
    // this test process's own PID and /proc entries are involved.
    QTemporaryDir run;
    const windows::Environment env = hyprEnv(run.path(), "backlogsig");
    const windows::Selection sel = windows::selectBackend(env, false);
    QCOMPARE(windows::statusId(sel.status), windows::statusId(Status::Ok));
    FullBacklog backlog(QString::fromStdString(sel.address));
    QVERIFY2(backlog.full, qPrintable(backlog.problem));
    windows::ProbeOptions o;
    o.environment = env;
    o.clockTicks = ::sysconf(_SC_CLK_TCK);
    o.limits.deadlineMs = 200;
    o.visibleIntervalMs = 100;
    o.backgroundIntervalMs = 100;
    o.maxBackoffMs = 300;
    windows::WindowProbe probe(o);
    QSignalSpy spy(&probe, &windows::WindowProbe::resultReady);
    probe.start(true);
    QVERIFY(spy.wait(3000));
    const auto first = spy.first().first().value<windows::ResultPtr>();
    qInfo("probe, full backlog: %s", windows::statusId(first->status));
    QVERIFY2(first->status != Status::OtherUser && !windows::isFinal(first->status),
             qPrintable(QStringLiteral("probe gave up on a busy compositor: %1").arg(QLatin1String(windows::statusId(first->status)))));
    QVERIFY(first->owners.empty());
    QVERIFY(backlog.drain());
    Answerer answer(backlog.listener, "[{\"pid\":" + QByteArray::number(::getpid()) + "}]");
    windows::ResultPtr recovered;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
                                 for (const auto& args : spy)
                                     if (auto r = args.first().value<windows::ResultPtr>(); r->ok())
                                         recovered = r;
                                 return recovered != nullptr;
                             }()),
                             4000);
    QCOMPARE(recovered->owners.size(), std::size_t(1));
    QCOMPARE(recovered->owners[0].pid, int(::getpid()));
    QVERIFY(probe.stop(2000));
}

// ---------------------------------------------------------------------------
// The main window

void TestProcessGroups::windowExpansionDefaultsPersistAndMigrate()
{
    WindowEnv env;
    {
        MainWindow w(env.options());
        w.applySnapshot(snap(99.5));
        w.showPage(MainWindow::ProcessPage);
        QTreeView* t = treeOf(w);
        QVERIFY(t);
        w.applyWindowOwners(owners({{4242, 5242}}, 99.6));
        QCOMPARE(expansion(t), QStringLiteral("++-")); // Apps, Background open; Kernel closed
        // The user's choice is saved ...
        t->collapse(groupAt(t, ProcessModel::Background));
        t->expand(groupAt(t, ProcessModel::Kernel));
        QCOMPARE(expansion(t), QStringLiteral("+-+"));
        // ... but not one made only for a filter.
        auto* filter = w.findChild<QLineEdit*>(QStringLiteral("processFilter"));
        QVERIFY(filter);
        filter->setText(QStringLiteral("bash"));
        QCOMPARE(expansion(t), QStringLiteral("+++"));
        t->collapse(groupAt(t, ProcessModel::Apps));
        w.applySnapshot(snap(99.7));
        QCOMPARE(expansion(t), QStringLiteral("-++"));
        filter->clear();
        QCOMPARE(expansion(t), QStringLiteral("+-+"));
        w.close();
    }
    {
        QSettings saved(env.settings(), QSettings::IniFormat);
        QCOMPARE(saved.value(QStringLiteral("processView/expandApps")).toBool(), true);
        QCOMPARE(saved.value(QStringLiteral("processView/expandBackground")).toBool(), false);
        QCOMPARE(saved.value(QStringLiteral("processView/expandKernel")).toBool(), true);
    }
    {
        MainWindow w(env.options());
        w.applySnapshot(snap(100));
        w.showPage(MainWindow::ProcessPage);
        QCOMPARE(expansion(treeOf(w)), QStringLiteral("+-+"));
        w.close();
    }

    // Settings from before groups: the kernel checkbox opens Kernel and the
    // old PID column (0) is the new PID column.
    WindowEnv old;
    {
        QSettings s(old.settings(), QSettings::IniFormat);
        s.setValue(QStringLiteral("showKernelThreads"), true);
        s.setValue(QStringLiteral("processSortColumn"), 0);
        s.setValue(QStringLiteral("processSortOrder"), 0);
    }
    {
        MainWindow w(old.options());
        w.applySnapshot(snap(100));
        w.showPage(MainWindow::ProcessPage);
        QTreeView* t = treeOf(w);
        QCOMPARE(expansion(t), QStringLiteral("++" "+"));
        QCOMPARE(t->header()->sortIndicatorSection(), int(ProcessModel::Pid));
        QCOMPARE(t->header()->sortIndicatorOrder(), Qt::AscendingOrder);
        w.close();
    }
    // A saved group choice wins over the old checkbox.
    WindowEnv both;
    {
        QSettings s(both.settings(), QSettings::IniFormat);
        s.setValue(QStringLiteral("showKernelThreads"), true);
        s.setValue(QStringLiteral("processView/expandKernel"), false);
    }
    {
        MainWindow w(both.options());
        w.applySnapshot(snap(100));
        w.showPage(MainWindow::ProcessPage);
        QCOMPARE(expansion(treeOf(w)), QStringLiteral("++-"));
        w.close();
    }
}

void TestProcessGroups::windowOwnersOrderedAndNeverFromTheFuture()
{
    WindowEnv env;
    MainWindow w(env.options());
    QSignalSpy applied(&w, &MainWindow::windowOwnersApplied);
    w.applySnapshot(snap(99.5));
    w.showPage(MainWindow::ProcessPage);
    QTreeView* t = treeOf(w);
    QVERIFY(t);
    QAbstractItemModelTester tester(t->model(), QAbstractItemModelTester::FailureReportingMode::QtTest);

    w.applyWindowOwners(owners({{4242, 5242}}, 100.0));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {4242});
    // Older, equally old, from the future (beyond a second), or timeless:
    // none may replace the newest list.
    w.applyWindowOwners(owners({{777, 1777}}, 99.9));
    w.applyWindowOwners(owners({{777, 1777}}, 100.0));
    w.applyWindowOwners(owners({{777, 1777}}, 101.5));
    w.applyWindowOwners(owners({{777, 1777}}, ps::kNaN));
    w.applyWindowOwners(owners({{777, 1777}}, std::numeric_limits<double>::infinity()));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {4242});
    QCOMPARE(applied.size(), 1);
    // The rejected future list did not become the ordering reference.
    w.applyWindowOwners(owners({{777, 1777}}, 100.5));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {777});
    QCOMPARE(applied.size(), 2);

    // Paused: a list is not applied, and not kept for later either.
    *env.clock = 100.6;
    w.setPaused(true);
    w.applyWindowOwners(owners({{500, 100}}, 100.6));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {777});
    w.setPaused(false);
    QCoreApplication::processEvents();
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {777});
    QCOMPARE(applied.size(), 2);
    w.applyWindowOwners(owners({{500, 100}}, 100.7));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {500});
    QCOMPARE(applied.size(), 3);
    w.close();
}

void TestProcessGroups::expiredListNotReusedAfterResume()
{
    WindowEnv env;
    MainWindow w(env.options());
    w.applySnapshot(snap(99.5));
    w.showPage(MainWindow::ProcessPage);
    QTreeView* t = treeOf(w);
    auto* note = w.findChild<QLabel*>(QStringLiteral("processGroupNote"));
    QVERIFY(t && note);
    w.applyWindowOwners(owners({{4242, 5242}}, 100.0, 3.0));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {4242});
    *env.clock = 100.5;
    w.setPaused(true);
    // Paused: the view, window list included, is frozen as it was.
    *env.clock = 160.0;
    QTest::qWait(700);
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {4242});
    // Resumed a minute later: the first new sample must not be sorted by a
    // list that expired 56 s ago.
    w.setPaused(false);
    w.applySnapshot(snap(160.0));
    const bool stillDetected = w.processGrouping().state == ProcessGrouping::State::Detected;
    const bool stillApp = pidsOf(t, ProcessModel::Apps).contains(4242);
    const QString shownAfterResume = titles(t);
    QElapsedTimer lingering;
    lingering.start();
    // Within a stale check, at the latest, the view says why.
    QTRY_COMPARE_WITH_TIMEOUT(int(w.processGrouping().state), int(ProcessGrouping::State::Unavailable), 1500);
    const qint64 lingeredMs = lingering.elapsed();
    QVERIFY2(!stillDetected && !stillApp,
             qPrintable(QStringLiteral("after resume the new sample was grouped by a list that expired 56 s earlier "
                                       "(detected %1, PID 4242 in Apps %2, \"%3\") for another %4 ms")
                            .arg(stillDetected)
                            .arg(stillApp)
                            .arg(shownAfterResume)
                            .arg(lingeredMs)));
    QVERIFY(note->isVisibleTo(&w));
    QVERIFY(pidsOf(t, ProcessModel::Apps).isEmpty());
    QCOMPARE(pidsOf(t, ProcessModel::Kernel), QList<int> {2});
    QVERIFY2(!titles(t).contains(QStringLiteral("Background")), qPrintable(titles(t)));
    w.close();
}

void TestProcessGroups::failureKeepsListUntilExpiryThenExplains()
{
    WindowEnv env;
    MainWindow w(env.options());
    w.applySnapshot(snap(99.5));
    w.showPage(MainWindow::ProcessPage);
    QTreeView* t = treeOf(w);
    auto* note = w.findChild<QLabel*>(QStringLiteral("processGroupNote"));
    auto* count = w.findChild<QLabel*>(QStringLiteral("processCount"));
    QVERIFY(t && note && count);
    const QString countText = count->text();
    w.applyWindowOwners(owners({{4242, 5242}}, 100.0, 3.0));
    // One missed request inside the list's lifetime does not reshuffle.
    const auto timeout = failed(Status::Timeout, "no answer within 400 ms", 101.0);
    w.applyWindowOwners(timeout);
    QCOMPARE(int(w.processGrouping().state), int(ProcessGrouping::State::Detected));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {4242});
    QVERIFY(!note->isVisibleTo(&w));
    // Past the lifetime: explicit, with the failure's own reason.
    *env.clock = 103.2;
    QTRY_COMPARE_WITH_TIMEOUT(int(w.processGrouping().state), int(ProcessGrouping::State::Unavailable), 1500);
    QCOMPARE(w.processGrouping().reason, windows::describe(*timeout));
    QVERIFY(note->isVisibleTo(&w));
    QVERIFY2(note->text().contains(windows::describe(*timeout)), qPrintable(note->text()));
    QVERIFY(pidsOf(t, ProcessModel::Apps).isEmpty());
    QCOMPARE(pidsOf(t, ProcessModel::Background), (QList<int> {1, 500, 777, 4242}));
    QCOMPARE(pidsOf(t, ProcessModel::Kernel), QList<int> {2});
    QVERIFY2(!titles(t).contains(QStringLiteral("Background")) && !titles(t).contains(QStringLiteral("Apps (0)")),
             qPrintable(titles(t)));
    // Still 5 processes (the old sample is now also stale, which the label
    // adds after the count); group rows are not processes.
    QVERIFY2(count->text().startsWith(countText), qPrintable(count->text()));
    // Recovery.
    w.applyWindowOwners(owners({{4242, 5242}}, 103.3));
    QCOMPARE(int(w.processGrouping().state), int(ProcessGrouping::State::Detected));
    QVERIFY(!note->isVisibleTo(&w));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {4242});
    // A late failure asked before that success does not undo it.
    w.applyWindowOwners(failed(Status::Disconnected, "closed", 103.25));
    QCOMPARE(int(w.processGrouping().state), int(ProcessGrouping::State::Detected));
    w.close();
}

void TestProcessGroups::reusedPidDoesNotInheritWindow()
{
    WindowEnv env;
    MainWindow w(env.options());
    w.applySnapshot(snap(99.5));
    w.showPage(MainWindow::ProcessPage);
    QTreeView* t = treeOf(w);
    w.applyWindowOwners(owners({{4242, 5242}, {777, 1777}}, 100.0, 30.0));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), (QList<int> {777, 4242}));
    // 4242 exits and the PID is reused by a later process while the list is
    // still valid: the newcomer has no window.
    auto procs = hostProcesses();
    procs[3].startTicks = 9000;
    procs[3].name = "newcomer";
    w.applySnapshot(snap(100.5, procs));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {777});
    QVERIFY(pidsOf(t, ProcessModel::Background).contains(4242));
    QCOMPARE(rowOf(t, 4242).data(ProcessModel::StartTicksRole).toULongLong(), qulonglong(9000));
    // A list naming the newcomer itself does.
    w.applyWindowOwners(owners({{4242, 9000}}, 100.6, 30.0));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {4242});
    w.close();
}

void TestProcessGroups::selectionFollowsIdentityThroughRegroupAndFilter()
{
    WindowEnv env;
    MainWindow w(env.options());
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.applySnapshot(snap(99.5));
    w.showPage(MainWindow::ProcessPage);
    QTreeView* t = treeOf(w);
    auto* filter = w.findChild<QLineEdit*>(QStringLiteral("processFilter"));
    QVERIFY(t && filter);
    w.applyWindowOwners(owners({{4242, 5242}}, 100.0, 30.0));
    const auto fox = qMakePair(777, qulonglong(1777));
    t->selectionModel()->setCurrentIndex(rowOf(t, 777), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QCOMPARE(selected(t), (QList<QPair<int, qulonglong>> {fox}));
    // Into Apps, filtered, out of Apps, unfiltered, refreshed, Details:
    // still the same process.
    w.applyWindowOwners(owners({{4242, 5242}, {777, 1777}}, 100.1, 30.0));
    QCOMPARE(selected(t), (QList<QPair<int, qulonglong>> {fox}));
    QCOMPARE(t->currentIndex().parent().data(ProcessModel::GroupRole).toInt(), int(ProcessModel::Apps));
    filter->setText(QStringLiteral("fire"));
    QCOMPARE(selected(t), (QList<QPair<int, qulonglong>> {fox}));
    w.applyWindowOwners(owners({{4242, 5242}}, 100.2, 30.0));
    QCOMPARE(selected(t), (QList<QPair<int, qulonglong>> {fox}));
    QCOMPARE(t->currentIndex().parent().data(ProcessModel::GroupRole).toInt(), int(ProcessModel::Background));
    filter->clear();
    QCOMPARE(selected(t), (QList<QPair<int, qulonglong>> {fox}));
    w.applySnapshot(snap(100.5));
    QCOMPARE(selected(t), (QList<QPair<int, qulonglong>> {fox}));
    w.setDetailsVisible(true);
    QCOMPARE(selected(t), (QList<QPair<int, qulonglong>> {fox}));
    w.setDetailsVisible(false);
    QCOMPARE(selected(t), (QList<QPair<int, qulonglong>> {fox}));
    // PID reuse: the newcomer is never the selected process.
    auto procs = hostProcesses();
    procs[4].startTicks = 8000;
    w.applySnapshot(snap(101.0, procs));
    for (const auto& sel : selected(t))
        QVERIFY2(sel != qMakePair(777, qulonglong(8000)), "selection moved to a reused PID");
    w.close();
}

void TestProcessGroups::groupRowsStaySpannedWithoutValues()
{
    WindowEnv env;
    MainWindow w(env.options());
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.applySnapshot(snap(99.5));
    w.showPage(MainWindow::ProcessPage);
    QTreeView* t = treeOf(w);
    auto* filter = w.findChild<QLineEdit*>(QStringLiteral("processFilter"));
    QVERIFY(t && filter);
    auto check = [&](const char* when) {
        QStringList problems;
        auto* m = t->model();
        if (m->rowCount() != ProcessModel::GroupCount)
            problems << QStringLiteral("%1 root rows").arg(m->rowCount());
        for (int r = 0; r < m->rowCount(); ++r) {
            if (m->index(r, 0).data(ProcessModel::GroupRole).toInt() != r)
                problems << QStringLiteral("group order at %1").arg(r);
            if (!t->isFirstColumnSpanned(r, QModelIndex()))
                problems << QStringLiteral("group %1 not spanned").arg(r);
            for (int c = 1; c < ProcessModel::ColumnCount; ++c)
                if (!m->index(r, c).data().toString().isEmpty())
                    problems << QStringLiteral("group %1 column %2 shows \"%3\"").arg(r).arg(c).arg(m->index(r, c).data().toString());
        }
        QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(when), problems.join(QStringLiteral("; ")))));
    };
    check("unavailable");
    w.applyWindowOwners(owners({{4242, 5242}}, 100.0, 30.0));
    check("detected");
    for (int c = 0; c < ProcessModel::ColumnCount; ++c)
        for (auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
            t->sortByColumn(c, order);
            check("sorted");
        }
    filter->setText(QStringLiteral("zzz-no-match"));
    check("filtered to nothing");
    QCOMPARE(t->model()->rowCount(groupAt(t, ProcessModel::Apps)), 0);
    filter->setText(QStringLiteral("Apps")); // a group word matches no process here
    check("filtered by a group word");
    QCOMPARE(t->model()->rowCount(groupAt(t, ProcessModel::Apps)) + t->model()->rowCount(groupAt(t, ProcessModel::Background))
                 + t->model()->rowCount(groupAt(t, ProcessModel::Kernel)),
             0);
    filter->clear();
    w.setDetailsVisible(true);
    check("details");
    w.setDetailsVisible(false);
    w.applyWindowOwners(failed(Status::Timeout, "no answer", 100.1));
    *env.clock = 131.0;
    QTRY_COMPARE_WITH_TIMEOUT(int(w.processGrouping().state), int(ProcessGrouping::State::Unavailable), 1500);
    check("expired");
    w.close();
}

void TestProcessGroups::probeIdlesWhileProcessListHidden()
{
    // The real probe thread with a counting query: often while the process
    // list is on screen, rarely when it is not, and one probe only however
    // often pages or Details are toggled.
    FakeProc fp;
    WindowEnv env;
    auto calls = std::make_shared<std::atomic<int>>(0);
    MainWindowOptions o = env.options(false);
    const auto t0 = std::chrono::steady_clock::now();
    o.clock = [t0] { return 100.0 + std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    o.windowProbe.enabled = true;
    o.windowProbe.environment = hyprEnv(QStringLiteral("/nonexistent-run"));
    o.windowProbe.procRoot = fp.root;
    o.windowProbe.clockTicks = 100;
    o.windowProbe.visibleIntervalMs = 100;
    o.windowProbe.backgroundIntervalMs = 5000;
    o.windowProbe.query = [calls](const windows::QueryContext&) {
        ++*calls;
        return okQuery({500});
    };
    MainWindow w(std::move(o));
    QVERIFY(w.windowProbe());
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.showPage(MainWindow::ProcessPage);
    QTest::qWait(200);
    int c0 = calls->load();
    QTest::qWait(1000);
    const int shown = calls->load() - c0;
    w.showPage(MainWindow::CpuPage);
    QTest::qWait(150);
    c0 = calls->load();
    QTest::qWait(1000);
    const int otherPage = calls->load() - c0;
    w.showPage(MainWindow::ProcessPage);
    QTest::qWait(150);
    w.hide();
    QTest::qWait(150);
    c0 = calls->load();
    QTest::qWait(1000);
    const int hidden = calls->load() - c0;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.showPage(MainWindow::ProcessPage);
    for (int k = 0; k < 10; ++k) {
        w.setDetailsVisible(k % 2 == 0);
        w.showPage(k % 2 ? MainWindow::ProcessPage : MainWindow::MemoryPage);
    }
    w.showPage(MainWindow::ProcessPage);
    QTest::qWait(150);
    c0 = calls->load();
    QTest::qWait(1000);
    const int toggled = calls->load() - c0;
    qInfo("probe requests per second: shown %d, other page %d, hidden %d, after toggling %d", shown, otherPage, hidden,
          toggled);
    QVERIFY2(shown >= 4, qPrintable(QString::number(shown)));
    QVERIFY2(otherPage <= 1, qPrintable(QString::number(otherPage)));
    QVERIFY2(hidden <= 1, qPrintable(QString::number(hidden)));
    QVERIFY2(toggled >= 4 && toggled <= 12, qPrintable(QString::number(toggled)));
    QVERIFY(w.shutdown(2000));
    const int atShutdown = calls->load();
    QTest::qWait(300);
    QCOMPARE(calls->load(), atShutdown);
}

// ---------------------------------------------------------------------------
// SYS010: an Ok list that omits windows (no PID, unresolved or ambiguous
// owners) cannot establish complete Background membership. It never replaces
// or renews the last complete list; a complete list may be used only through
// its own lifetime, and otherwise the grouping is explicitly unavailable.

namespace {

windows::ResultPtr partialOwners(std::vector<windows::Owner> list, double time, double validFor, const QString& counter)
{
    auto r = std::make_shared<windows::Result>(*owners(std::move(list), time, validFor));
    if (counter == QLatin1String("withoutPid"))
        r->withoutPid = 1;
    else if (counter == QLatin1String("unresolved"))
        r->unresolved = 1;
    else
        r->ambiguous = 1;
    ++r->windows;
    return r;
}

QString groupingText(const ProcessGrouping& g, QTreeView* t)
{
    return QStringLiteral("state %1, apps [%2], reason \"%3\", roots %4")
        .arg(int(g.state))
        .arg([&] {
            QStringList l;
            for (int p : pidsOf(t, ProcessModel::Apps))
                l << QString::number(p);
            return l.join(QLatin1Char(','));
        }())
        .arg(g.reason, titles(t));
}

} // namespace

void TestProcessGroups::partialListDoesNotEstablishBackground_data()
{
    QTest::addColumn<QString>("counter");
    QTest::addColumn<bool>("withKnown");
    for (const char* c : {"withoutPid", "unresolved", "ambiguous"}) {
        QTest::addRow("%s, no known owners", c) << QString::fromLatin1(c) << false;
        QTest::addRow("%s, some known owners", c) << QString::fromLatin1(c) << true;
    }
}

void TestProcessGroups::partialListDoesNotEstablishBackground()
{
    QFETCH(QString, counter);
    QFETCH(bool, withKnown);
    using State = ProcessGrouping::State;
    const auto known = [&] {
        return withKnown ? std::vector<windows::Owner> {{4242, 5242}, {777, 1777}} : std::vector<windows::Owner> {};
    };
    WindowEnv env;
    MainWindow w(env.options());
    w.applySnapshot(snap(99.5));
    w.showPage(MainWindow::ProcessPage);
    QTreeView* t = treeOf(w);
    auto* note = w.findChild<QLabel*>(QStringLiteral("processGroupNote"));
    QVERIFY(t && note);
    const auto unclassified = [&](const char* when) {
        const ProcessGrouping& g = w.processGrouping();
        const QString what = QLatin1String(when) + QStringLiteral(": ") + groupingText(g, t);
        QVERIFY2(g.state == State::Unavailable, qPrintable(what));
        QVERIFY2(pidsOf(t, ProcessModel::Apps).isEmpty(), qPrintable(what));
        QVERIFY2(!titles(t).contains(QStringLiteral("Background")), qPrintable(what));
        QVERIFY2(!g.reason.isEmpty() && g.reason != QStringLiteral("The desktop has not answered"), qPrintable(what));
        QVERIFY2(note->isVisibleTo(&w), qPrintable(what));
        QCOMPARE(pidsOf(t, ProcessModel::Kernel), QList<int> {2}); // kernel threads still work
    };

    // 1. The first list is partial: nothing is complete yet.
    w.applyWindowOwners(partialOwners(known(), 100.0, 30.0, counter));
    unclassified("first partial list");
    if (QTest::currentTestFailed())
        return;

    // 2. A complete list, then a partial one inside its lifetime: the
    //    complete list stays in charge, with its own members and expiry.
    *env.clock = 101.0;
    w.applyWindowOwners(owners({{4242, 5242}}, 101.0, 3.0));
    QCOMPARE(int(w.processGrouping().state), int(State::Detected));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {4242});
    *env.clock = 101.5;
    w.applyWindowOwners(partialOwners(known(), 101.5, 30.0, counter));
    {
        const QString what = QStringLiteral("partial after complete: ") + groupingText(w.processGrouping(), t);
        QVERIFY2(w.processGrouping().state == State::Detected, qPrintable(what));
        QVERIFY2(pidsOf(t, ProcessModel::Apps) == QList<int> {4242}, qPrintable(what));
    }
    // The partial list's 30 s did not extend the complete list's 3 s.
    *env.clock = 104.2;
    QTRY_VERIFY_WITH_TIMEOUT(w.processGrouping().state != State::Detected, 1500);
    unclassified("complete list expired, only a partial one is newer");
    if (QTest::currentTestFailed())
        return;

    // 3. Another partial list after expiry changes nothing.
    *env.clock = 104.5;
    w.applyWindowOwners(partialOwners(known(), 104.5, 30.0, counter));
    unclassified("partial after expiry");
    if (QTest::currentTestFailed())
        return;

    // 4. Recovery with a complete list.
    *env.clock = 105.0;
    w.applyWindowOwners(owners({{777, 1777}}, 105.0, 3.0));
    QCOMPARE(int(w.processGrouping().state), int(State::Detected));
    QCOMPARE(pidsOf(t, ProcessModel::Apps), QList<int> {777});
    QCOMPARE(pidsOf(t, ProcessModel::Background), (QList<int> {1, 500, 4242}));
    QVERIFY(titles(t).contains(QStringLiteral("Background")));
    w.close();
}

void TestProcessGroups::completeEmptyListIsDetectedWithNoApps()
{
    WindowEnv env;
    MainWindow w(env.options());
    w.applySnapshot(snap(99.5));
    w.showPage(MainWindow::ProcessPage);
    QTreeView* t = treeOf(w);
    auto* note = w.findChild<QLabel*>(QStringLiteral("processGroupNote"));
    QVERIFY(t && note);
    const auto empty = owners({}, 100.0, 3.0); // all omission counts 0
    QCOMPARE(empty->withoutPid + empty->unresolved + empty->ambiguous, 0);
    w.applyWindowOwners(empty);
    QCOMPARE(int(w.processGrouping().state), int(ProcessGrouping::State::Detected));
    QVERIFY(pidsOf(t, ProcessModel::Apps).isEmpty());
    QCOMPARE(pidsOf(t, ProcessModel::Background), (QList<int> {1, 500, 777, 4242}));
    QCOMPARE(pidsOf(t, ProcessModel::Kernel), QList<int> {2});
    QVERIFY2(titles(t).contains(QStringLiteral("Background")), qPrintable(titles(t)));
    QVERIFY(!note->isVisibleTo(&w));
    w.close();
}


QTEST_MAIN(TestProcessGroups)
#include "tst_process_groups.moc"
