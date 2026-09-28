// End task: the pidfd backend against this test's own forked children, and
// the Processes page controls with an injected backend. Only children this
// test created are ever signalled, by their exact PID and only while
// unreaped; every refused case also records that no signal was attempted.
#include "app/mainwindow.h"
#include "app/processmodel.h"
#include "core/endtask.h"
#include "core/linuxparsers.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest/QtTest>

#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using ps::endtask::Outcome;
using ps::endtask::Signal;
using ps::endtask::Target;

namespace {

std::string readFile(const std::string& path)
{
    std::ifstream in(path);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// A child of this test: sleeps until signalled, optionally ignoring SIGTERM,
// or exits at once (a zombie until reaped). It dies with the test. The test
// signals it only through the pidfd taken before the child may run on, so a
// failed start, an early exit or a reap can never redirect a signal to
// another process; without that pidfd the child exits by itself.
struct Child {
    enum Mode { Sleep, IgnoreTerm, Exit };
    using ForkFn = pid_t (*)();
    using OpenFn = int (*)(pid_t);
    static int openPidfd(pid_t pid) { return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0u)); }

    pid_t pid = -1;
    int fd = -1;
    bool reaped = false;
    int status = 0;

    explicit Child(Mode mode, ForkFn forkFn = ::fork, OpenFn openFn = openPidfd)
    {
        int ready[2];
        int go[2];
        if (::pipe2(ready, O_CLOEXEC) != 0)
            return;
        if (::pipe2(go, O_CLOEXEC) != 0) {
            ::close(ready[0]);
            ::close(ready[1]);
            return;
        }
        const pid_t parent = ::getpid();
        const pid_t forked = forkFn();
        if (forked == 0) {
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() != parent)
                ::_exit(3);
            ::prctl(PR_SET_NAME, "ps-endtask-kid");
            // Not the test framework's crash handler, inherited from here.
            ::signal(SIGTERM, mode == IgnoreTerm ? SIG_IGN : SIG_DFL);
            ::close(go[1]);
            char g = 0;
            ssize_t n;
            do
                n = ::read(go[0], &g, 1);
            while (n < 0 && errno == EINTR);
            if (n != 1)
                ::_exit(4); // the parent has no pidfd for it
            if (mode == Exit)
                ::_exit(0);
            const char c = 'r';
            (void)!::write(ready[1], &c, 1);
            for (;;)
                ::pause();
        }
        ::close(ready[1]);
        ::close(go[0]);
        if (forked > 0) {
            // Unreaped, so the number is still this child's.
            fd = openFn(forked);
            pid = forked;
            if (fd >= 0) {
                const char g = 'g';
                (void)!::write(go[1], &g, 1);
            }
        }
        ::close(go[1]);
        if (pid > 0 && fd < 0) {
            wait(); // it exits by itself on the closed pipe
            pid = -1;
        } else if (pid > 0 && mode != Exit) {
            char c = 0;
            while (::read(ready[0], &c, 1) < 0 && errno == EINTR) {
            }
        }
        ::close(ready[0]);
    }
    ~Child()
    {
        if (owned())
            signal(SIGKILL);
        wait();
        if (fd >= 0)
            ::close(fd);
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    bool owned() const { return pid > 0 && fd >= 0 && !reaped; }
    // Only this child, through its pidfd; false (nothing sent) otherwise.
    bool signal(int sig)
    {
        return owned() && ::syscall(SYS_pidfd_send_signal, fd, sig, static_cast<void*>(nullptr), 0u) == 0;
    }
    // Blocks until it ended and reaps it; true if it did. A child already
    // reaped elsewhere (ECHILD) is no longer ours either way.
    bool wait()
    {
        if (pid <= 0 || reaped)
            return reaped;
        pid_t r;
        do
            r = ::waitpid(pid, &status, 0);
        while (r < 0 && errno == EINTR);
        if (r == pid || (r < 0 && errno == ECHILD))
            reaped = true;
        return r == pid;
    }
    // Still running after ms; an exit is only observed (WNOWAIT), wait()
    // still reaps it and gets its status.
    bool aliveAfter(int ms)
    {
        QTest::qWait(ms);
        if (pid <= 0 || reaped)
            return false;
        siginfo_t info {};
        int r;
        do
            r = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
        while (r < 0 && errno == EINTR);
        if (r < 0) {
            if (errno == ECHILD)
                reaped = true;
            return false;
        }
        return info.si_pid == 0;
    }
    char state() const
    {
        const auto st = ps::parse::parsePidStat(readFile("/proc/" + std::to_string(pid) + "/stat"));
        return st ? st->state : '?';
    }
    // The child as the process list would show it now.
    Target target() const
    {
        Target t;
        const std::string dir = "/proc/" + std::to_string(pid);
        const auto st = ps::parse::parsePidStat(readFile(dir + "/stat"));
        const auto uid = ps::parse::parseStatusRealUid(readFile(dir + "/status"));
        if (!st || !uid)
            return t;
        t.pid = pid;
        t.startTicks = st->startTime;
        t.name = st->comm;
        t.uid = *uid;
        t.uidKnown = true;
        t.kernelThread = (st->flags & ps::parse::kPfKthread) != 0;
        return t;
    }
};

// Hooks that record every attempt; `realSend` false refuses them all, so a
// refused case can never signal anything even if the checks were wrong.
struct Recorder {
    std::vector<int> opened;
    std::vector<int> sent;
    bool realSend = false;
    ps::endtask::Hooks hooks()
    {
        ps::endtask::Hooks h;
        h.pidfdOpen = [this](int pid) {
            opened.push_back(pid);
            return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0u));
        };
        h.sendSignal = [this](int fd, int sig) {
            sent.push_back(sig);
            if (!realSend) {
                errno = EPERM;
                return -1;
            }
            return static_cast<int>(::syscall(SYS_pidfd_send_signal, fd, sig, static_cast<void*>(nullptr), 0u));
        };
        return h;
    }
};

int openFds()
{
    int n = 0;
    for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator("/proc/self/fd"))
        ++n;
    return n;
}

bool pidfdsWork()
{
    const int fd = static_cast<int>(::syscall(SYS_pidfd_open, ::getpid(), 0u));
    if (fd < 0)
        return false;
    ::close(fd);
    return true;
}

// ---- UI --------------------------------------------------------------------

// A thread-safe fake backend: records requests and returns `result`,
// optionally holding each request until released.
struct FakeBackend {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::pair<Target, Signal>> calls;
    ps::endtask::Result result {Outcome::Sent, 0, 'S', false};
    bool hold = false;
    bool running = false;

    EndTaskRunner::Backend backend()
    {
        return [this](const Target& t, Signal s) {
            std::unique_lock lock(mutex);
            calls.emplace_back(t, s);
            running = true;
            cv.notify_all();
            cv.wait(lock, [this] { return !hold; });
            running = false;
            cv.notify_all();
            return result;
        };
    }
    // A request abandoned by a failed test still finishes before this goes.
    ~FakeBackend()
    {
        std::unique_lock lock(mutex);
        hold = false;
        cv.notify_all();
        cv.wait(lock, [this] { return !running; });
    }
    std::size_t count()
    {
        const std::lock_guard lock(mutex);
        return calls.size();
    }
    void release()
    {
        const std::lock_guard lock(mutex);
        hold = false;
        cv.notify_all();
    }
};

const uint32_t kMe = static_cast<uint32_t>(::getuid());

ps::ProcessInfo proc(int pid, const char* name, uint32_t uid = kMe, bool uidKnown = true, bool kernel = false)
{
    ps::ProcessInfo p;
    p.pid = pid;
    p.ppid = 1;
    p.startTicks = 5000 + static_cast<uint64_t>(pid);
    p.uid = uid;
    p.uidKnown = uidKnown;
    p.user = uid == kMe ? "me" : "other";
    p.kernelThread = kernel;
    p.state = 'S';
    p.threads = 1;
    p.name = name;
    p.command = std::string("/usr/bin/") + name;
    p.cpuPercent = 1.0;
    p.rssBytes = 1e6;
    p.ageSeconds = 10;
    return p;
}

std::shared_ptr<ps::Snapshot> snapshot(double time, std::vector<ps::ProcessInfo> processes)
{
    static uint64_t seq = 0;
    auto s = std::make_shared<ps::Snapshot>();
    s->sequence = ++seq;
    s->time = time;
    s->interval = 1.0;
    s->continuous = seq > 1;
    s->logicalCpus = 1;
    s->processesScanned = true;
    s->processes = std::move(processes);
    s->threadCount = static_cast<int>(s->processes.size());
    return s;
}

std::vector<ps::ProcessInfo> standardList()
{
    return {
        proc(1, "systemd"),
        proc(2, "kthreadd", 0, true, true),
        proc(4000, "editor"),
        proc(4001, "<b>bold</b> & co"),
        proc(4002, "someone-elses", kMe + 1),
        proc(4003, "unknown-owner", 0, false),
        proc(static_cast<int>(::getpid()), "tst_endtask"),
    };
}

struct Env {
    QTemporaryDir dir;
    FakeBackend fake;
    MainWindowOptions options(bool injected = true)
    {
        MainWindowOptions o;
        o.startCollector = false;
        o.settingsFile = dir.filePath(QStringLiteral("pulsescope.ini"));
        o.windowProbe.enabled = false;
        if (injected)
            o.endTask = fake.backend();
        return o;
    }
};

template <typename T>
T* child(QObject& w, const char* name)
{
    return w.findChild<T*>(QString::fromLatin1(name));
}

QModelIndex row(QTreeView* table, int pid)
{
    auto* m = table->model();
    for (int g = 0; g < m->rowCount(); ++g) {
        const QModelIndex parent = m->index(g, 0);
        for (int r = 0; r < m->rowCount(parent); ++r)
            if (m->index(r, 0, parent).data(ProcessModel::PidRole).toInt() == pid)
                return m->index(r, 0, parent);
    }
    return {};
}

void select(QTreeView* table, const QModelIndex& index)
{
    table->scrollTo(index);
    table->setCurrentIndex(index);
}

// The open one: a closed box lingers until its deferred delete.
QMessageBox* confirmBox(QWidget& w)
{
    for (auto* box : w.findChildren<QMessageBox*>(QStringLiteral("endTaskConfirmBox")))
        if (box->isVisible())
            return box;
    return nullptr;
}

QMenu* openMenu(QWidget& w)
{
    for (auto* menu : w.findChildren<QMenu*>(QStringLiteral("processContextMenu")))
        if (menu->isVisible())
            return menu;
    return nullptr;
}

void closeMenus(QWidget& w)
{
    for (auto* menu : w.findChildren<QMenu*>(QStringLiteral("processContextMenu")))
        menu->close();
    QTest::qWait(10);
}

// Right-click, as the platform delivers it: to the viewport at `pos`.
void rightClick(QTreeView* table, QPoint pos)
{
    QContextMenuEvent e(QContextMenuEvent::Mouse, pos, table->viewport()->mapToGlobal(pos));
    QApplication::sendEvent(table->viewport(), &e);
}

} // namespace

class TestEndTask : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();

    // Backend, real children
    void precheckRules();
    void terminateEndsTheChild();
    void terminateNeverEscalates();
    void forceEndsTheChild();
    void changedIdentityIsRefused();
    void ownershipIsRequired();
    void protectedTargetsAreRefused();
    void liveKernelThreadIsRefused();
    void zombieIsReportedExited();
    void reapedChildIsNeverSignalled();
    void exitJustBeforeSendIsHonest();
    void syscallErrorsAreReported();
    void swappedPidfdIsRefused();
    void invalidSignalIsRefused();
    void stoppedChildIsSentWithNote();
    void noDescriptorLeaks();
    void fixtureSignalsOnlyItsOwnChild();

    // UI, injected backend
    void liveBackendNeedsRealProc();
    void buttonAndDeleteConfirmThenSend();
    void cancelAndEscapeSendNothing();
    void deleteInFilterEditsText();
    void menuTargetsRowUnderPointer();
    void keyboardMenuUsesSelection();
    void noMenuOnGroupsOrEmptySpace();
    void blockedRowsCannotBeEnded();
    void filteredOutSelectionIsNotTargeted();
    void refreshDuringConfirmationKeepsTarget();
    void duplicatesBlockedWhilePending();
    void closingDuringConfirmationSendsNothing();
    void namesArePlainText();
    void failureIsShown();
    void pausedSuccessSaysSo();

    // UI with the live backend, on this test's own child
    void liveEndTaskEndsOwnChild();
};

void TestEndTask::initTestCase()
{
    if (!pidfdsWork())
        QSKIP("pidfd_open is unavailable here");
}

void TestEndTask::precheckRules()
{
    Target t;
    t.pid = 4000;
    t.uid = 1000;
    t.uidKnown = true;
    QCOMPARE(ps::endtask::precheck(t, 1000, 99), Outcome::Sent);
    for (int bad : {0, -1, -4000})
        QCOMPARE(ps::endtask::precheck(Target {bad, 1, "x", 1000, true, false}, 1000, 99), Outcome::Invalid);
    QCOMPARE(ps::endtask::precheck(Target {1, 1, "init", 1000, true, false}, 1000, 99), Outcome::Protected);
    QCOMPARE(ps::endtask::precheck(Target {99, 1, "self", 1000, true, false}, 1000, 99), Outcome::Protected);
    QCOMPARE(ps::endtask::precheck(Target {4000, 1, "k", 1000, true, true}, 1000, 99), Outcome::Protected);
    QCOMPARE(ps::endtask::precheck(Target {4000, 1, "x", 1001, true, false}, 1000, 99), Outcome::NotOwned);
    QCOMPARE(ps::endtask::precheck(Target {4000, 1, "x", 1000, false, false}, 1000, 99), Outcome::NotOwned);
}

void TestEndTask::terminateEndsTheChild()
{
    Child c(Child::Sleep);
    QVERIFY(c.owned());
    const auto r = ps::endtask::send(c.target(), Signal::Terminate);
    QCOMPARE(r.outcome, Outcome::Sent);
    QVERIFY(c.wait());
    QVERIFY(WIFSIGNALED(c.status));
    QCOMPARE(WTERMSIG(c.status), SIGTERM);
    const auto text = ps::endtask::describe(r, c.target(), Signal::Terminate);
    QVERIFY2(text.find("SIGTERM") != std::string::npos && text.find("sent") != std::string::npos, text.c_str());
}

void TestEndTask::terminateNeverEscalates()
{
    Child c(Child::IgnoreTerm);
    QVERIFY(c.owned());
    const Target t = c.target();
    QCOMPARE(ps::endtask::send(t, Signal::Terminate).outcome, Outcome::Sent);
    QVERIFY(c.aliveAfter(300));
    QCOMPARE(ps::endtask::send(t, Signal::Kill).outcome, Outcome::Sent);
    QVERIFY(c.wait());
    QVERIFY(WIFSIGNALED(c.status));
    QCOMPARE(WTERMSIG(c.status), SIGKILL);
}

void TestEndTask::forceEndsTheChild()
{
    Child c(Child::Sleep);
    QVERIFY(c.owned());
    Recorder rec;
    rec.realSend = true;
    QCOMPARE(ps::endtask::send(c.target(), Signal::Kill, rec.hooks()).outcome, Outcome::Sent);
    QVERIFY(rec.sent == std::vector<int> {SIGKILL});
    QVERIFY(c.wait());
    QCOMPARE(WTERMSIG(c.status), SIGKILL);
}

void TestEndTask::changedIdentityIsRefused()
{
    Child c(Child::Sleep);
    QVERIFY(c.owned());
    const Target real = c.target();
    std::vector<Target> changed(4, real);
    changed[0].startTicks += 1;
    changed[1].startTicks = 0;
    changed[2].name = "ps-endtask-ki";
    changed[3].name += "x";
    for (Signal s : {Signal::Terminate, Signal::Kill}) {
        for (const Target& t : changed) {
            Recorder rec;
            const auto r = ps::endtask::send(t, s, rec.hooks());
            QCOMPARE(r.outcome, Outcome::Changed);
            QVERIFY(rec.sent.empty());
            QVERIFY(ps::endtask::describe(r, t, s).find("Nothing was sent.") != std::string::npos);
        }
        // A listed owner that no longer matches (own UID is taken as that
        // owner, so only the live check can object).
        Target other = real;
        other.uid = real.uid + 1;
        Recorder rec;
        auto hooks = rec.hooks();
        hooks.ownUid = other.uid;
        QCOMPARE(ps::endtask::send(other, s, hooks).outcome, Outcome::Changed);
        QVERIFY(rec.sent.empty());
    }
    QVERIFY(c.aliveAfter(20));
}

void TestEndTask::ownershipIsRequired()
{
    Child c(Child::Sleep);
    QVERIFY(c.owned());
    Target t = c.target();
    Recorder rec;
    t.uidKnown = false;
    QCOMPARE(ps::endtask::send(t, Signal::Kill, rec.hooks()).outcome, Outcome::NotOwned);
    t = c.target();
    auto hooks = rec.hooks();
    hooks.ownUid = t.uid + 1;
    QCOMPARE(ps::endtask::send(t, Signal::Kill, hooks).outcome, Outcome::NotOwned);
    QVERIFY(rec.opened.empty());
    QVERIFY(rec.sent.empty());
    QVERIFY(c.aliveAfter(20));
}

void TestEndTask::protectedTargetsAreRefused()
{
    Recorder rec;
    const Target init {1, 1, "systemd", kMe, true, false};
    const Target self {static_cast<int>(::getpid()), 1, "tst_endtask", kMe, true, false};
    const Target kernel {2, 1, "kthreadd", kMe, true, true};
    for (const Target& t : {init, self, kernel})
        for (Signal s : {Signal::Terminate, Signal::Kill})
            QCOMPARE(ps::endtask::send(t, s, rec.hooks()).outcome, Outcome::Protected);
    QVERIFY(rec.opened.empty());
    QVERIFY(rec.sent.empty());
}

void TestEndTask::liveKernelThreadIsRefused()
{
    // kthreadd's real identity, listed as an ordinary process of ours: only
    // the live flags can refuse it.
    const auto st = ps::parse::parsePidStat(readFile("/proc/2/stat"));
    if (!st || !(st->flags & ps::parse::kPfKthread))
        QSKIP("PID 2 is not kthreadd here");
    const Target t {2, st->startTime, st->comm, kMe, true, false};
    Recorder rec;
    const auto r = ps::endtask::send(t, Signal::Kill, rec.hooks());
    QCOMPARE(r.outcome, Outcome::Protected);
    QVERIFY(r.kernelThread);
    QVERIFY(rec.sent.empty());
}

void TestEndTask::zombieIsReportedExited()
{
    Child c(Child::Exit);
    QVERIFY(c.owned());
    QTRY_COMPARE(c.state(), 'Z');
    Target t = c.target();
    Recorder rec;
    const auto r = ps::endtask::send(t, Signal::Terminate, rec.hooks());
    QCOMPARE(r.outcome, Outcome::Exited);
    QCOMPARE(r.state, 'Z');
    QVERIFY(rec.sent.empty());
    QVERIFY(ps::endtask::describe(r, t, Signal::Terminate).find("already exited") != std::string::npos);
    QVERIFY(c.wait());
    QVERIFY(WIFEXITED(c.status));
}

void TestEndTask::reapedChildIsNeverSignalled()
{
    Target t;
    {
        Child c(Child::Sleep);
        QVERIFY(c.owned());
        t = c.target();
        QVERIFY(c.signal(SIGKILL));
        QVERIFY(c.wait());
    }
    // The PID is free (or, rarely, someone else's by now): never signalled.
    Recorder rec;
    const auto r = ps::endtask::send(t, Signal::Kill, rec.hooks());
    QVERIFY2(r.outcome == Outcome::Exited || r.outcome == Outcome::Changed || r.outcome == Outcome::NotOwned,
             qPrintable(QString::number(static_cast<int>(r.outcome))));
    QVERIFY(rec.sent.empty());
}

void TestEndTask::exitJustBeforeSendIsHonest()
{
    Child c(Child::Sleep);
    QVERIFY(c.owned());
    Recorder rec;
    rec.realSend = true;
    auto hooks = rec.hooks();
    bool killed = false;
    hooks.beforeSend = [&c, &killed] {
        killed = c.signal(SIGKILL);
        c.wait(); // reaped: its PID may now be reused, but the pidfd is not
    };
    const auto r = ps::endtask::send(c.target(), Signal::Terminate, hooks);
    QVERIFY(killed);
    QCOMPARE(r.outcome, Outcome::Exited);
    QVERIFY(rec.sent == std::vector<int> {SIGTERM}); // attempted on the stale pidfd only
    QCOMPARE(WTERMSIG(c.status), SIGKILL);
}

void TestEndTask::syscallErrorsAreReported()
{
    Child c(Child::Sleep);
    QVERIFY(c.owned());
    const Target t = c.target();
    const auto openFails = [&](int err) {
        ps::endtask::Hooks h;
        h.pidfdOpen = [err](int) {
            errno = err;
            return -1;
        };
        h.sendSignal = [](int, int) {
            errno = EPERM;
            return -1;
        };
        return ps::endtask::send(t, Signal::Kill, h);
    };
    QCOMPARE(openFails(ENOSYS).outcome, Outcome::Unsupported);
    QCOMPARE(openFails(EPERM).outcome, Outcome::Unsupported);
    QCOMPARE(openFails(ESRCH).outcome, Outcome::Exited);
    QCOMPARE(openFails(EINVAL).outcome, Outcome::Changed);
    const auto emfile = openFails(EMFILE);
    QCOMPARE(emfile.outcome, Outcome::Failed);
    QCOMPARE(emfile.error, EMFILE);
    const auto nosys = openFails(ENOSYS);
    QVERIFY(ps::endtask::describe(nosys, t, Signal::Kill).find("Linux 5.3") != std::string::npos);

    const auto sendFails = [&](int err) {
        ps::endtask::Hooks h;
        h.sendSignal = [err](int, int) {
            errno = err;
            return -1;
        };
        return ps::endtask::send(t, Signal::Kill, h);
    };
    QCOMPARE(sendFails(EPERM).outcome, Outcome::Denied);
    QCOMPARE(sendFails(ESRCH).outcome, Outcome::Exited);
    QCOMPARE(sendFails(ENOSYS).outcome, Outcome::Unsupported);
    QCOMPARE(sendFails(EINVAL).outcome, Outcome::Failed);
    QVERIFY(c.aliveAfter(20));
}

void TestEndTask::swappedPidfdIsRefused()
{
    Child target(Child::Sleep);
    Child bystander(Child::Sleep);
    QVERIFY(target.owned() && bystander.owned());
    Recorder rec;
    rec.realSend = true;
    auto hooks = rec.hooks();
    hooks.pidfdOpen = [&](int) { return Child::openPidfd(bystander.pid); };
    QCOMPARE(ps::endtask::send(target.target(), Signal::Kill, hooks).outcome, Outcome::Unsupported);
    QVERIFY(rec.sent.empty());
    QVERIFY(target.aliveAfter(20));
    QVERIFY(bystander.aliveAfter(0));
}

void TestEndTask::invalidSignalIsRefused()
{
    Child c(Child::Sleep);
    QVERIFY(c.owned());
    Recorder rec;
    rec.realSend = true;
    for (int v : {2, -1, 15, 9}) {
        const auto s = static_cast<Signal>(v);
        const auto r = ps::endtask::send(c.target(), s, rec.hooks());
        QCOMPARE(r.outcome, Outcome::Invalid);
        QVERIFY(ps::endtask::describe(r, c.target(), s).find("Nothing was sent.") != std::string::npos);
    }
    QVERIFY(rec.opened.empty());
    QVERIFY(rec.sent.empty());
    QVERIFY(c.aliveAfter(20));
}

void TestEndTask::stoppedChildIsSentWithNote()
{
    Child c(Child::Sleep);
    QVERIFY(c.owned());
    QVERIFY(c.signal(SIGSTOP));
    QTRY_COMPARE(c.state(), 'T');
    const Target t = c.target();
    const auto r = ps::endtask::send(t, Signal::Terminate);
    QCOMPARE(r.outcome, Outcome::Sent);
    QCOMPARE(r.state, 'T');
    QVERIFY(ps::endtask::describe(r, t, Signal::Terminate).find("stopped") != std::string::npos);
    QVERIFY(c.aliveAfter(100)); // pending until continued
    QVERIFY(c.signal(SIGCONT));
    QVERIFY(c.wait());
    QCOMPARE(WTERMSIG(c.status), SIGTERM);
}

void TestEndTask::noDescriptorLeaks()
{
    Child c(Child::Sleep);
    QVERIFY(c.owned());
    const Target t = c.target();
    const int before = openFds();
    for (int i = 0; i < 50; ++i) {
        Recorder rec;
        Target changed = t;
        changed.startTicks += 1;
        (void)ps::endtask::send(changed, Signal::Kill, rec.hooks());
        (void)ps::endtask::send(t, Signal::Kill, rec.hooks()); // refused by the recorder: Denied
        ps::endtask::Hooks h;
        h.pidfdOpen = [](int) {
            errno = EMFILE;
            return -1;
        };
        (void)ps::endtask::send(t, Signal::Kill, h);
    }
    QCOMPARE(openFds(), before);
    QCOMPARE(ps::endtask::send(t, Signal::Terminate).outcome, Outcome::Sent);
    QVERIFY(c.wait());
    QCOMPARE(openFds(), before);
}

pid_t g_refusedPid = -1;

// The fixture itself: a child it could not start, or whose pidfd it could
// not take, is never signalled; one reaped behind its back is unreachable
// through the stale pidfd; an exit is observed without losing the status.
void TestEndTask::fixtureSignalsOnlyItsOwnChild()
{
    const int before = openFds();
    {
        Child c(Child::Sleep, []() -> pid_t {
            errno = EAGAIN;
            return -1;
        });
        QVERIFY(!c.owned());
        QCOMPARE(c.pid, -1);
        QVERIFY(!c.signal(SIGKILL));
    }
    QCOMPARE(openFds(), before);
    {
        g_refusedPid = -1;
        Child c(Child::Sleep, ::fork, [](pid_t p) {
            g_refusedPid = p;
            errno = EMFILE;
            return -1;
        });
        QVERIFY(g_refusedPid > 0);
        QVERIFY(!c.owned());
        QCOMPARE(c.pid, -1);
        QVERIFY(!c.signal(SIGKILL));
        // It left by itself on the closed pipe, and was reaped.
        QVERIFY(c.reaped);
        QVERIFY(WIFEXITED(c.status));
        QCOMPARE(WEXITSTATUS(c.status), 4);
        errno = 0;
        const pid_t gone = ::waitpid(g_refusedPid, nullptr, WNOHANG);
        const int err = errno;
        QCOMPARE(gone, -1);
        QCOMPARE(err, ECHILD);
    }
    QCOMPARE(openFds(), before);
    {
        Child c(Child::Sleep);
        QVERIFY(c.owned());
        QVERIFY(c.signal(SIGKILL));
        int st = 0;
        pid_t r;
        do
            r = ::waitpid(c.pid, &st, 0); // reaped outside the fixture
        while (r < 0 && errno == EINTR);
        QCOMPARE(r, c.pid);
        const bool sent = c.signal(SIGTERM); // the stale pidfd: whatever has the number now is not reached
        const int err = errno;
        QVERIFY(!sent);
        QCOMPARE(err, ESRCH);
        QVERIFY(!c.aliveAfter(0)); // ECHILD: no longer ours
        QVERIFY(!c.owned());
        QVERIFY(!c.signal(SIGKILL));
    }
    {
        Child c(Child::Sleep);
        QVERIFY(c.owned());
        QVERIFY(c.signal(SIGKILL));
        QTRY_VERIFY(!c.aliveAfter(5));
        QVERIFY(!c.reaped); // observed, not reaped
        QVERIFY(c.wait());
        QVERIFY(WIFSIGNALED(c.status));
        QCOMPARE(WTERMSIG(c.status), SIGKILL);
        QVERIFY(!c.owned());
    }
    QCOMPARE(openFds(), before);
}

// ---- UI --------------------------------------------------------------------

void TestEndTask::liveBackendNeedsRealProc()
{
    Env env;
    MainWindow w(env.options(false)); // injected snapshots, no backend
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    auto* table = child<QTreeView>(w, "processTable");
    auto* button = child<QPushButton>(w, "endTaskButton");
    auto* action = child<QAction>(*table, "endTaskAction");
    QVERIFY(table && button && action);
    select(table, row(table, 4000));
    QVERIFY(!action->isEnabled());
    QVERIFY(!button->isEnabled());
    QVERIFY2(button->toolTip().contains(QStringLiteral("/proc")), qPrintable(button->toolTip()));
    QCOMPARE(button->accessibleDescription(), button->toolTip());
    table->setFocus();
    QTest::keyClick(table, Qt::Key_Delete);
    QVERIFY(!confirmBox(w));
}

void TestEndTask::buttonAndDeleteConfirmThenSend()
{
    Env env;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    auto* button = child<QPushButton>(w, "endTaskButton");
    QVERIFY(!button->isEnabled()); // nothing selected
    select(table, row(table, 4000));
    QVERIFY(button->isEnabled());
    QVERIFY(button->toolTip().contains(QStringLiteral("Delete")));
    QSignalSpy finished(&w, &MainWindow::endTaskFinished);

    // Delete, with the list focused
    table->setFocus();
    QTest::keyClick(table, Qt::Key_Delete);
    QMessageBox* box = confirmBox(w);
    QVERIFY(box);
    QVERIFY(w.isEndTaskPending());
    QCOMPARE(box->textFormat(), Qt::PlainText);
    QCOMPARE(box->text(), QString::fromUtf8("End \u201ceditor\u201d (PID 4000)?"));
    QVERIFY(box->informativeText().contains(QStringLiteral("SIGTERM")));
    QVERIFY(box->informativeText().contains(QStringLiteral("Unsaved work")));
    QCOMPARE(box->property("psForce").toBool(), false);
    QCOMPARE(box->defaultButton(), box->button(QMessageBox::Cancel));
    QCOMPARE(box->escapeButton(), box->button(QMessageBox::Cancel));
    child<QPushButton>(*box, "endTaskConfirmButton")->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(env.fake.count(), std::size_t {1});
    QCOMPARE(env.fake.calls[0].first.pid, 4000);
    QCOMPARE(env.fake.calls[0].first.startTicks, uint64_t {9000});
    QCOMPARE(env.fake.calls[0].first.name, std::string("editor"));
    QCOMPARE(env.fake.calls[0].second, Signal::Terminate);
    QCOMPARE(finished.at(0).at(0).toInt(), 4000);
    QCOMPARE(finished.at(0).at(3).toInt(), static_cast<int>(Outcome::Sent));
    const QString message = w.statusBar()->currentMessage();
    QVERIFY2(message.contains(QStringLiteral("sent")), qPrintable(message));
    QVERIFY(!child<QMessageBox>(w, "endTaskErrorBox"));
    QVERIFY(!w.isEndTaskPending());
    QVERIFY(row(table, 4000).isValid()); // until a sample no longer lists it

    // The button
    button->click();
    box = confirmBox(w);
    QVERIFY(box);
    child<QPushButton>(*box, "endTaskConfirmButton")->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(env.fake.count(), std::size_t {2});
}

void TestEndTask::cancelAndEscapeSendNothing()
{
    Env env;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    select(table, row(table, 4000));
    auto* button = child<QPushButton>(w, "endTaskButton");

    button->click();
    QMessageBox* box = confirmBox(w);
    QVERIFY(box);
    QTest::keyClick(box, Qt::Key_Escape);
    QTRY_VERIFY(!confirmBox(w));

    button->click();
    box = confirmBox(w);
    QVERIFY(box);
    box->button(QMessageBox::Cancel)->click();
    QTRY_VERIFY(!confirmBox(w));

    button->click();
    box = confirmBox(w);
    QVERIFY(box);
    // Enter or Space on the focused button, and Enter on the box: Cancel
    QCOMPARE(box->focusWidget(), box->button(QMessageBox::Cancel));
    QTest::keyClick(box->focusWidget(), Qt::Key_Return);
    QTRY_VERIFY(!confirmBox(w));
    button->click();
    box = confirmBox(w);
    QVERIFY(box);
    QCOMPARE(box->focusWidget(), box->button(QMessageBox::Cancel));
    QTest::keyClick(box->focusWidget(), Qt::Key_Space);
    QTRY_VERIFY(!confirmBox(w));
    button->click();
    box = confirmBox(w);
    QVERIFY(box);
    QTest::keyClick(box, Qt::Key_Return);
    QTRY_VERIFY(!confirmBox(w));

    button->click();
    box = confirmBox(w);
    QVERIFY(box);
    box->close();
    QTRY_VERIFY(!confirmBox(w));

    QTest::qWait(50);
    QCOMPARE(env.fake.count(), std::size_t {0});
    QVERIFY(!w.isEndTaskPending());
    QVERIFY(button->isEnabled());
}

void TestEndTask::deleteInFilterEditsText()
{
    Env env;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    select(table, row(table, 4000));
    auto* filter = child<QLineEdit>(w, "processFilter");
    QVERIFY(filter);
    filter->setFocus();
    filter->setText(QStringLiteral("xeditor"));
    filter->setCursorPosition(0);
    QTest::keyClick(filter, Qt::Key_Delete);
    QCOMPARE(filter->text(), QStringLiteral("editor"));
    QVERIFY(!confirmBox(w));
    QCOMPARE(env.fake.count(), std::size_t {0});
}

void TestEndTask::menuTargetsRowUnderPointer()
{
    Env env;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    select(table, row(table, 4000));
    const QModelIndex other = row(table, 4001);
    rightClick(table, table->visualRect(other).center());
    QMenu* menu = openMenu(w);
    QVERIFY(menu);
    auto* end = child<QAction>(*menu, "endTaskMenuAction");
    auto* force = child<QAction>(*menu, "forceEndTaskMenuAction");
    QVERIFY(end && force && end->isEnabled() && force->isEnabled());
    const QList<QAction*> actions = menu->actions();
    QCOMPARE(actions.size(), 3);
    QVERIFY(actions[1]->isSeparator());
    force->trigger();
    closeMenus(w);
    QMessageBox* box = confirmBox(w);
    QVERIFY(box);
    QCOMPARE(box->property("psForce").toBool(), true);
    QVERIFY(box->text().contains(QStringLiteral("PID 4001")));
    QVERIFY(box->text().startsWith(QStringLiteral("Force end")));
    QVERIFY(box->informativeText().contains(QStringLiteral("SIGKILL")));
    QSignalSpy finished(&w, &MainWindow::endTaskFinished);
    child<QPushButton>(*box, "endTaskConfirmButton")->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(env.fake.count(), std::size_t {1});
    QCOMPARE(env.fake.calls[0].first.pid, 4001);
    QCOMPARE(env.fake.calls[0].second, Signal::Kill);
    QVERIFY(w.statusBar()->currentMessage().contains(QStringLiteral("SIGKILL")));
}

void TestEndTask::keyboardMenuUsesSelection()
{
    Env env;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    select(table, row(table, 4000));
    table->setFocus();

    // The Menu key (the platform sends this event), with the pointer
    // elsewhere
    QContextMenuEvent key(QContextMenuEvent::Keyboard, QPoint(1, 1), table->mapToGlobal(QPoint(1, 1)));
    QApplication::sendEvent(table, &key);
    QMenu* menu = openMenu(w);
    QVERIFY(menu);
    child<QAction>(*menu, "endTaskMenuAction")->trigger();
    closeMenus(w);
    QMessageBox* box = confirmBox(w);
    QVERIFY(box);
    QVERIFY(box->text().contains(QStringLiteral("PID 4000")));
    box->button(QMessageBox::Cancel)->click();
    QTRY_VERIFY(!confirmBox(w));

    // Shift+F10
    table->setFocus();
    QTest::keyClick(table, Qt::Key_F10, Qt::ShiftModifier);
    menu = openMenu(w);
    QVERIFY(menu);
    child<QAction>(*menu, "forceEndTaskMenuAction")->trigger();
    closeMenus(w);
    box = confirmBox(w);
    QVERIFY(box);
    QVERIFY(box->text().contains(QStringLiteral("PID 4000")));
    box->button(QMessageBox::Cancel)->click();
    QTRY_VERIFY(!confirmBox(w));
    QCOMPARE(env.fake.count(), std::size_t {0});
}

void TestEndTask::noMenuOnGroupsOrEmptySpace()
{
    Env env;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.resize(900, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    select(table, row(table, 4000));
    auto* model = table->model();
    for (int g = 0; g < model->rowCount(); ++g) {
        const QModelIndex group = model->index(g, 0);
        QVERIFY(group.data(ProcessModel::IsGroupRole).toBool());
        rightClick(table, table->visualRect(group).center());
        QVERIFY(!openMenu(w));
        // Selected group row: no target either
        table->setCurrentIndex(group);
        QVERIFY(!child<QAction>(*table, "endTaskAction")->isEnabled());
        table->setFocus();
        QTest::keyClick(table, Qt::Key_Delete);
        QTest::keyClick(table, Qt::Key_F10, Qt::ShiftModifier);
        QVERIFY(!confirmBox(w));
        QVERIFY(!openMenu(w));
        select(table, row(table, 4000));
    }
    // Below the last row
    const QPoint empty(10, table->viewport()->height() - 3);
    QVERIFY(!table->indexAt(empty).isValid());
    rightClick(table, empty);
    QVERIFY(!openMenu(w));
    QVERIFY(!confirmBox(w));
    QCOMPARE(env.fake.count(), std::size_t {0});
}

void TestEndTask::blockedRowsCannotBeEnded()
{
    Env env;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    table->expandAll();
    auto* action = child<QAction>(*table, "endTaskAction");
    auto* button = child<QPushButton>(w, "endTaskButton");
    const std::vector<std::pair<int, const char*>> blocked {
        {1, "init"}, {2, "Kernel"}, {4002, "own"}, {4003, "unknown"}, {static_cast<int>(::getpid()), "itself"}};
    for (const auto& [pid, why] : blocked) {
        const QModelIndex index = row(table, pid);
        QVERIFY2(index.isValid(), qPrintable(QString::number(pid)));
        select(table, index);
        QVERIFY(!action->isEnabled());
        QVERIFY(!button->isEnabled());
        QVERIFY2(button->toolTip().contains(QLatin1String(why)), qPrintable(button->toolTip()));
        table->setFocus();
        QTest::keyClick(table, Qt::Key_Delete);
        QVERIFY(!confirmBox(w));
        rightClick(table, table->visualRect(index).center());
        QMenu* menu = openMenu(w);
        QVERIFY(menu);
        auto* end = child<QAction>(*menu, "endTaskMenuAction");
        auto* force = child<QAction>(*menu, "forceEndTaskMenuAction");
        QVERIFY(!end->isEnabled() && !force->isEnabled());
        QCOMPARE(force->toolTip(), button->toolTip());
        force->trigger(); // disabled: ignored
        closeMenus(w);
        QVERIFY(!confirmBox(w));
    }
    QCOMPARE(env.fake.count(), std::size_t {0});
}

void TestEndTask::filteredOutSelectionIsNotTargeted()
{
    Env env;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    select(table, row(table, 4000));
    auto* action = child<QAction>(*table, "endTaskAction");
    QVERIFY(action->isEnabled());
    child<QLineEdit>(w, "processFilter")->setText(QStringLiteral("unknown-owner"));
    QTRY_VERIFY(!row(table, 4000).isValid());
    QVERIFY(!action->isEnabled());
    table->setFocus();
    QTest::keyClick(table, Qt::Key_Delete);
    QVERIFY(!confirmBox(w));
    QCOMPARE(env.fake.count(), std::size_t {0});
}

void TestEndTask::refreshDuringConfirmationKeepsTarget()
{
    Env env;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    select(table, row(table, 4000));
    child<QPushButton>(w, "endTaskButton")->click();
    QMessageBox* box = confirmBox(w);
    QVERIFY(box);
    const QString text = box->text();

    // PID 4000 is reused by another process, others come and go, the sort
    // changes, and the selection moves.
    auto list = standardList();
    list[2] = proc(4000, "newcomer");
    list[2].startTicks = 777777;
    list.push_back(proc(3999, "aaa"));
    list.erase(list.begin() + 3);
    w.applySnapshot(snapshot(2, list));
    table->sortByColumn(ProcessModel::Pid, Qt::DescendingOrder);
    w.applySnapshot(snapshot(3, list));
    select(table, row(table, 3999));
    QCOMPARE(box->text(), text);

    QSignalSpy finished(&w, &MainWindow::endTaskFinished);
    child<QPushButton>(*box, "endTaskConfirmButton")->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(env.fake.count(), std::size_t {1});
    QCOMPARE(env.fake.calls[0].first.pid, 4000);
    QCOMPARE(env.fake.calls[0].first.startTicks, uint64_t {9000});
    QCOMPARE(env.fake.calls[0].first.name, std::string("editor"));
}

void TestEndTask::duplicatesBlockedWhilePending()
{
    Env env;
    env.fake.hold = true;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    auto* action = child<QAction>(*table, "endTaskAction");
    auto* button = child<QPushButton>(w, "endTaskButton");
    select(table, row(table, 4000));
    button->click();
    QMessageBox* box = confirmBox(w);
    QVERIFY(box);
    QVERIFY(!action->isEnabled() && !button->isEnabled());
    action->trigger();
    int open = 0;
    for (auto* b : w.findChildren<QMessageBox*>(QStringLiteral("endTaskConfirmBox")))
        open += b->isVisible() ? 1 : 0;
    QCOMPARE(open, 1);

    QSignalSpy finished(&w, &MainWindow::endTaskFinished);
    child<QPushButton>(*box, "endTaskConfirmButton")->click();
    {
        std::unique_lock lock(env.fake.mutex);
        QVERIFY(env.fake.cv.wait_for(lock, std::chrono::seconds(5), [&] { return env.fake.running; }));
    }
    QTRY_VERIFY(!confirmBox(w));
    QVERIFY(w.isEndTaskPending()); // running
    QVERIFY(!action->isEnabled() && !button->isEnabled());
    select(table, row(table, 4001));
    QVERIFY(!action->isEnabled());
    rightClick(table, table->visualRect(row(table, 4001)).center());
    QMenu* menu = openMenu(w);
    QVERIFY(menu);
    QVERIFY(!child<QAction>(*menu, "endTaskMenuAction")->isEnabled());
    closeMenus(w);
    QCOMPARE(finished.count(), 0);
    env.fake.release();
    QVERIFY(finished.wait(5000));
    QVERIFY(!w.isEndTaskPending());
    QVERIFY(action->isEnabled());
    QCOMPARE(env.fake.count(), std::size_t {1});
}

void TestEndTask::closingDuringConfirmationSendsNothing()
{
    Env env;
    auto w = std::make_unique<MainWindow>(env.options());
    w->applySnapshot(snapshot(1, standardList()));
    w->showPage(MainWindow::ProcessPage);
    w->show();
    QVERIFY(QTest::qWaitForWindowExposed(w.get()));
    auto* table = child<QTreeView>(*w, "processTable");
    select(table, row(table, 4000));
    child<QPushButton>(*w, "endTaskButton")->click();
    QPointer<QMessageBox> box = confirmBox(*w);
    QVERIFY(box);
    QPointer<QPushButton> ok = child<QPushButton>(*box, "endTaskConfirmButton");
    QVERIFY(w->shutdown(2000));
    // Even a click that still reaches the box after shutdown sends nothing.
    if (box && ok)
        ok->click();
    w.reset();
    QTest::qWait(50);
    QVERIFY(!box);
    QCOMPARE(env.fake.count(), std::size_t {0});
}

void TestEndTask::namesArePlainText()
{
    Env env;
    env.fake.result = {Outcome::Changed, 0, 'S', false};
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    select(table, row(table, 4001));
    child<QPushButton>(w, "endTaskButton")->click();
    QMessageBox* box = confirmBox(w);
    QVERIFY(box);
    QCOMPARE(box->textFormat(), Qt::PlainText);
    QVERIFY(box->text().contains(QStringLiteral("<b>bold</b> & co")));
    QSignalSpy finished(&w, &MainWindow::endTaskFinished);
    child<QPushButton>(*box, "endTaskConfirmButton")->click();
    QVERIFY(finished.wait(5000));
    auto* error = child<QMessageBox>(w, "endTaskErrorBox");
    QVERIFY(error);
    QCOMPARE(error->textFormat(), Qt::PlainText);
    QVERIFY(error->text().contains(QStringLiteral("<b>bold</b> & co")));
    error->close();
}

void TestEndTask::failureIsShown()
{
    Env env;
    env.fake.result = {Outcome::Denied, EPERM, 'S', false};
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    select(table, row(table, 4000));
    child<QPushButton>(w, "endTaskButton")->click();
    QSignalSpy finished(&w, &MainWindow::endTaskFinished);
    child<QPushButton>(*confirmBox(w), "endTaskConfirmButton")->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(finished.at(0).at(3).toInt(), static_cast<int>(Outcome::Denied));
    auto* error = child<QMessageBox>(w, "endTaskErrorBox");
    QVERIFY(error && error->isVisible());
    QVERIFY2(error->text().endsWith(QStringLiteral("Nothing was sent.")), qPrintable(error->text()));
    QCOMPARE(w.statusBar()->currentMessage(), error->text());
    QVERIFY(!error->text().contains(QStringLiteral(" sent to")));
    error->close();
}

void TestEndTask::pausedSuccessSaysSo()
{
    Env env;
    MainWindow w(env.options());
    w.applySnapshot(snapshot(1, standardList()));
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.setPaused(true);
    auto* table = child<QTreeView>(w, "processTable");
    select(table, row(table, 4000));
    child<QPushButton>(w, "endTaskButton")->click();
    QSignalSpy finished(&w, &MainWindow::endTaskFinished);
    child<QPushButton>(*confirmBox(w), "endTaskConfirmButton")->click();
    QVERIFY(finished.wait(5000));
    QVERIFY(w.isPaused()); // not resumed
    QVERIFY(w.statusBar()->currentMessage().endsWith(QStringLiteral("The list is paused.")));
    QVERIFY(row(table, 4000).isValid());
}

void TestEndTask::liveEndTaskEndsOwnChild()
{
    Child c(Child::Sleep);
    QVERIFY(c.owned());
    QTemporaryDir dir;
    MainWindowOptions o;
    o.settingsFile = dir.filePath(QStringLiteral("pulsescope.ini"));
    o.initialIntervalMs = 500;
    o.windowProbe.enabled = false;
    MainWindow w(o); // the real collector and backend
    w.showPage(MainWindow::ProcessPage);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* table = child<QTreeView>(w, "processTable");
    child<QLineEdit>(w, "processFilter")->setText(QString::number(c.pid));
    QTRY_VERIFY_WITH_TIMEOUT(row(table, c.pid).isValid(), 10000);
    select(table, row(table, c.pid));
    QVERIFY(table->currentIndex().data(ProcessModel::PidRole).toInt() == c.pid);
    table->setFocus();
    QTest::keyClick(table, Qt::Key_Delete);
    QMessageBox* box = confirmBox(w);
    QVERIFY(box);
    QVERIFY(box->text().contains(QStringLiteral("ps-endtask-kid")));
    QSignalSpy finished(&w, &MainWindow::endTaskFinished);
    child<QPushButton>(*box, "endTaskConfirmButton")->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(finished.at(0).at(3).toInt(), static_cast<int>(Outcome::Sent));
    QVERIFY(c.wait());
    QCOMPARE(WTERMSIG(c.status), SIGTERM);
    QTRY_VERIFY_WITH_TIMEOUT(!row(table, c.pid).isValid(), 10000);
    QVERIFY(w.shutdown(2000));
}

QTEST_MAIN(TestEndTask)
#include "tst_endtask.moc"
