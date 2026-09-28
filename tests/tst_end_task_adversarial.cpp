// Independent adversarial tests for End task (SIGTERM) and Force end task
// (SIGKILL) on the Processes page.
//
// Safety: the only processes these tests ever let the application signal are
// children this test spawned itself (posix_spawn of this same binary in child
// mode). Each child dies with its parent (PR_SET_PDEATHSIG), kills itself by
// alarm() after a bounded time, and is reaped by this test. No test names a
// process by user-visible name, and none signals a group or a process it did
// not start.
//
// Portability (Qt 6.4 / GCC 11 / glibc 2.36): no libc pidfd wrappers. pidfds
// come from syscall(SYS_pidfd_open) where the kernel has it; a child is
// opened before it is reaped, so its PID cannot have been reused.

#include <QApplication>
#include <QElapsedTimer>
#include <QtTest>

#ifdef PS_ET_CORE
#include "core/endtask.h"
#include "core/linuxparsers.h"
#endif
#ifdef PS_ET_APP
#include "app/mainwindow.h"
#include "app/processmodel.h"
#include "app/windowowners.h"

#include <QAbstractButton>
#include <QContextMenuEvent>
#include <QLabel>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeView>

#include <condition_variable>
#include <mutex>
#include <thread>
#endif

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

extern char** environ;

namespace {

int sysPidfdOpen(pid_t pid)
{
#ifdef SYS_pidfd_open
    return int(::syscall(SYS_pidfd_open, pid, 0));
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

int sysPidfdSignal(int pidfd, int sig)
{
#ifdef SYS_pidfd_send_signal
    return int(::syscall(SYS_pidfd_send_signal, pidfd, sig, nullptr, 0));
#else
    (void)pidfd;
    (void)sig;
    errno = ENOSYS;
    return -1;
#endif
}

constexpr int kReportFd = 3;           // child -> parent: 'R' ready, 'T' SIGTERM seen
constexpr unsigned kChildLifetimeS = 60; // hard bound on any child's life
constexpr const char kChildComm[] = "ps-et-child";

// ---------------------------------------------------------------------------
// Child side. Single-threaded, before any Qt object exists.

volatile sig_atomic_t g_termCount = 0;
int g_argc = 0;
char** g_argv = nullptr;

void onTerm(int)
{
    g_termCount = g_termCount + 1;
    const char t = 'T';
    [[maybe_unused]] auto n = ::write(kReportFd, &t, 1);
}

void onTermGrandchild(int)
{
    const char g = 'G';
    [[maybe_unused]] auto n = ::write(kReportFd, &g, 1);
}

void ignoreTerm(void (*handler)(int))
{
    struct sigaction sa {};
    sa.sa_handler = handler;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
}

std::string fromHex(const char* hex)
{
    std::string out;
    for (std::size_t i = 0; hex && hex[i] && hex[i + 1]; i += 2)
        out.push_back(char(std::stoi(std::string(hex + i, 2), nullptr, 16)));
    return out;
}

int childNamespaceProbe();
int childThreadFailProbe(int argc, char** argv);
int childFdExhaustProbe();

int childMain(const char* mode)
{
    const std::string m = mode;
    if (m == "ns-probe")
        return childNamespaceProbe();
    if (m == "fd-exhaust")
        return childFdExhaustProbe();
    if (m == "ui-thread-fail")
        return childThreadFailProbe(g_argc, g_argv);
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    const std::string comm = std::getenv("PS_ET_COMM") ? fromHex(std::getenv("PS_ET_COMM")) : std::string(kChildComm);
    ::prctl(PR_SET_NAME, comm.c_str());
    ::alarm(kChildLifetimeS);
    if (m == "ignore-term") {
        ignoreTerm(onTerm); // observes and survives SIGTERM: only SIGKILL ends it
    } else if (m == "grandchild") {
        ignoreTerm(onTerm);
        const pid_t g = ::fork();
        if (g == 0) {
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            ignoreTerm(onTermGrandchild);
            const char c = 'g';
            [[maybe_unused]] auto n = ::write(kReportFd, &c, 1);
            for (;;)
                ::pause();
        }
    } else if (m == "exit") {
        const char r = 'R';
        [[maybe_unused]] auto n = ::write(kReportFd, &r, 1);
        ::usleep(50000);
        ::_exit(0);
    } else if (m != "default") {
        return 2;
    }
    const char r = 'R';
    if (::write(kReportFd, &r, 1) != 1)
        return 3;
    for (;;)
        ::pause();
}

// ---------------------------------------------------------------------------
// Parent side

std::string selfExe()
{
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, std::size_t(n)) : std::string();
}

std::string toHex(const std::string& s)
{
    static const char* k = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out.push_back(k[c >> 4]);
        out.push_back(k[c & 15]);
    }
    return out;
}

// Field 22 (starttime, clock ticks since boot) of /proc/<pid>/stat: an
// independent reading for the test oracle, after the last ')' of comm.
std::optional<unsigned long long> startTicksOf(pid_t pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::stringstream all;
    all << in.rdbuf(); // whole file: comm may hold a newline
    const std::string line = all.str();
    const auto close = line.rfind(')');
    if (close == std::string::npos)
        return std::nullopt;
    std::istringstream rest(line.substr(close + 2));
    std::string field;
    for (int i = 3; i <= 22; ++i)
        if (!(rest >> field))
            return std::nullopt;
    return std::stoull(field);
}

char stateOf(pid_t pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::stringstream all;
    all << in.rdbuf();
    const std::string line = all.str();
    const auto close = line.rfind(')');
    return close == std::string::npos || close + 2 >= line.size() ? '?' : line[close + 2];
}

std::string readLink(const std::string& path)
{
    char target[512];
    const ssize_t len = ::readlink(path.c_str(), target, sizeof target - 1);
    return len > 0 ? std::string(target, std::size_t(len)) : std::string();
}

// pidfds this process holds (anon_inode:[pidfd]), and all descriptors, to
// prove none leak.
int openPidfds()
{
    int n = 0;
    for (int fd = 0; fd < 4096; ++fd)
        n += readLink("/proc/self/fd/" + std::to_string(fd)).find("pidfd") != std::string::npos;
    return n;
}
int openFds()
{
    int n = 0;
    for (int fd = 0; fd < 4096; ++fd)
        n += ::fcntl(fd, F_GETFD) != -1;
    return n;
}

// "Pid:" of a pidfd, as the kernel reports it; INT_MIN if none.
int fdinfoPid(int fd)
{
    std::ifstream in("/proc/self/fdinfo/" + std::to_string(fd));
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("Pid:", 0) == 0)
            return std::stoi(line.substr(4));
    return INT_MIN;
}

class OwnedChild {
public:
    explicit OwnedChild(const char* mode, const std::string& comm = kChildComm)
    {
        int report[2];
        if (::pipe2(report, O_CLOEXEC) != 0)
            return;
        m_report = report[0];
        posix_spawn_file_actions_t fa;
        ::posix_spawn_file_actions_init(&fa);
        ::posix_spawn_file_actions_adddup2(&fa, report[1], kReportFd);
        // Its own process group: a group signal could never reach the test.
        posix_spawnattr_t attr;
        ::posix_spawnattr_init(&attr);
        ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        ::posix_spawnattr_setpgroup(&attr, 0);
        const std::string exe = selfExe();
        std::vector<std::string> envs {std::string("PS_ET_CHILD=") + mode, "PS_ET_COMM=" + toHex(comm)};
        for (char** e = environ; *e; ++e)
            if (std::strncmp(*e, "PS_ET_", 6) != 0)
                envs.emplace_back(*e);
        std::vector<char*> envp;
        for (auto& e : envs)
            envp.push_back(e.data());
        envp.push_back(nullptr);
        char* argv[] = {const_cast<char*>(exe.c_str()), nullptr};
        const int rc = ::posix_spawn(&m_pid, exe.c_str(), &fa, &attr, argv, envp.data());
        ::posix_spawn_file_actions_destroy(&fa);
        ::posix_spawnattr_destroy(&attr);
        ::close(report[1]);
        if (rc != 0) {
            m_pid = -1;
            return;
        }
        // Not reaped yet, so the PID is still this child's.
        m_pidfd = sysPidfdOpen(m_pid);
        if (m_pidfd >= 0)
            ::fcntl(m_pidfd, F_SETFD, FD_CLOEXEC);
        m_ready = waitFor('R', 5000);
        m_startTicks = startTicksOf(m_pid).value_or(0);
    }
    ~OwnedChild()
    {
        if (m_pid > 0 && !m_reaped) {
            signal(SIGKILL);
            waitExit(5000);
        }
        if (m_pidfd >= 0)
            ::close(m_pidfd);
        if (m_report >= 0)
            ::close(m_report);
    }
    OwnedChild(const OwnedChild&) = delete;
    OwnedChild& operator=(const OwnedChild&) = delete;

    bool ready() const { return m_ready && m_pid > 0 && m_startTicks > 0; }
    bool hasPidfd() const { return m_pidfd >= 0; }
    int pidfd() const { return m_pidfd; }
    pid_t pid() const { return m_pid; }
    unsigned long long startTicks() const { return m_startTicks; }

    // Signals this unreaped child (fixtures and cleanup only): by its pidfd,
    // or where the kernel has none by its PID, which cannot have been reused
    // while the child is not reaped.
    bool signal(int sig)
    {
        if (m_pid <= 0 || m_reaped)
            return false;
        if (m_pidfd >= 0)
            return sysPidfdSignal(m_pidfd, sig) == 0;
        return ::kill(m_pid, sig) == 0;
    }

    // Exited within timeoutMs? Reaps it and records how.
    bool waitExit(int timeoutMs)
    {
        if (m_reaped)
            return true;
        if (m_pid <= 0)
            return false;
        QElapsedTimer t;
        t.start();
        for (;;) {
            siginfo_t si {};
            if (::waitid(P_PID, id_t(m_pid), &si, WEXITED | WNOHANG) == 0 && si.si_pid == m_pid) {
                m_reaped = true;
                m_code = si.si_code;
                m_status = si.si_status;
                return true;
            }
            const int left = timeoutMs - int(t.elapsed());
            if (left <= 0)
                return false;
            if (m_pidfd >= 0) {
                pollfd p {m_pidfd, POLLIN, 0};
                ::poll(&p, 1, std::min(left, 50));
            } else {
                ::usleep(10000);
            }
        }
    }
    // Exited but deliberately not reaped: a zombie.
    bool waitZombie(int timeoutMs)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            siginfo_t si {};
            if (::waitid(P_PID, id_t(m_pid), &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == m_pid)
                return stateOf(m_pid) == 'Z';
            ::usleep(5000);
        }
        return false;
    }
    bool waitStopped(int timeoutMs)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            const char st = stateOf(m_pid);
            if (st == 'T' || st == 't')
                return true;
            ::usleep(5000);
        }
        return false;
    }
    // Still running (not exited, not a zombie) after ms.
    bool aliveAfter(int ms)
    {
        if (waitExit(ms))
            return false;
        const char st = stateOf(m_pid);
        return st != 'Z' && st != 'X' && st != '?';
    }
    bool killedBy(int sig) const { return m_reaped && m_code == CLD_KILLED && m_status == sig; }
    QString how() const
    {
        if (!m_reaped)
            return QStringLiteral("still running (state %1)").arg(QChar::fromLatin1(stateOf(m_pid)));
        return m_code == CLD_KILLED || m_code == CLD_DUMPED ? QStringLiteral("killed by signal %1").arg(m_status)
                                                             : QStringLiteral("exited with %1").arg(m_status);
    }
    // Bytes the child reported: 'T' SIGTERM seen, 'G' grandchild saw SIGTERM.
    int seen(char c)
    {
        drain();
        return int(std::count(m_bytes.begin(), m_bytes.end(), c));
    }
    int termsSeen() { return seen('T'); }
    bool waitFor(char c, int timeoutMs)
    {
        QElapsedTimer t;
        t.start();
        for (;;) {
            drain();
            if (m_bytes.find(c) != std::string::npos)
                return true;
            const int left = timeoutMs - int(t.elapsed());
            if (left <= 0)
                return false;
            pollfd p {m_report, POLLIN, 0};
            ::poll(&p, 1, left);
        }
    }

private:
    void drain()
    {
        char buf[64];
        pollfd p {m_report, POLLIN, 0};
        while (m_report >= 0 && ::poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
            const ssize_t n = ::read(m_report, buf, sizeof buf);
            if (n <= 0)
                break;
            m_bytes.append(buf, std::size_t(n));
        }
    }
    int m_pidfd = -1;
    int m_report = -1;
    pid_t m_pid = -1;
    unsigned long long m_startTicks = 0;
    bool m_ready = false;
    bool m_reaped = false;
    int m_code = 0;
    int m_status = 0;
    std::string m_bytes;
};

// SIGTERMs delivered to the test process itself: a self or group signal
// from the code under test would otherwise end the run.
volatile sig_atomic_t g_selfTerms = 0;
void onSelfTerm(int) { g_selfTerms = g_selfTerms + 1; }

} // namespace

#ifdef PS_ET_CORE
namespace et = ps::endtask;
using et::Outcome;

namespace {

const char* outcomeName(Outcome o)
{
    switch (o) {
    case Outcome::Sent: return "Sent";
    case Outcome::Exited: return "Exited";
    case Outcome::Changed: return "Changed";
    case Outcome::NotOwned: return "NotOwned";
    case Outcome::Protected: return "Protected";
    case Outcome::Invalid: return "Invalid";
    case Outcome::Denied: return "Denied";
    case Outcome::Unsupported: return "Unsupported";
    case Outcome::Failed: return "Failed";
    }
    return "(out of range)";
}

// The target exactly as the sampler lists it: the child's own /proc/<pid>/stat
// through the parser the sampler uses (ProcessInfo::name = PidStat::comm).
et::Target listed(OwnedChild& c)
{
    et::Target t;
    t.pid = c.pid();
    t.startTicks = c.startTicks();
    std::ifstream in("/proc/" + std::to_string(c.pid()) + "/stat");
    std::stringstream all;
    all << in.rdbuf();
    if (auto st = ps::parse::parsePidStat(all.str()))
        t.name = st->comm;
    t.uid = ::getuid();
    t.uidKnown = true;
    t.kernelThread = false;
    return t;
}

// Records every call; forwards to the real syscalls only when told to.
struct Recorder {
    struct Call {
        int pidfd;
        int sig;
        int fdinfoPid;
    };
    std::vector<int> opens;
    std::vector<Call> sends;
    bool realOpen = true;
    bool realSend = false; // default: refuse, so nothing is ever signalled
    int sendErrno = EPERM;
    et::Hooks hooks()
    {
        et::Hooks h;
        h.pidfdOpen = [this](int pid) {
            opens.push_back(pid);
            if (!realOpen) {
                errno = EPERM;
                return -1;
            }
            return sysPidfdOpen(pid);
        };
        h.sendSignal = [this](int pidfd, int sig) {
            sends.push_back({pidfd, sig, fdinfoPid(pidfd)});
            if (realSend)
                return sysPidfdSignal(pidfd, sig);
            errno = sendErrno;
            return -1;
        };
        return h;
    }
};

QString describeResult(const et::Result& r)
{
    return QStringLiteral("%1 (errno %2, state %3)").arg(QLatin1String(outcomeName(r.outcome))).arg(r.error).arg(QChar::fromLatin1(r.state));
}

} // namespace
#endif

#ifdef PS_ET_APP
namespace {

// Stands in for ps::endtask::send in the window: records each request and
// its thread, answers with `answer`, and can be held until released.
struct FakeBackend {
    struct Call {
        et::Target target;
        et::Signal signal;
        std::thread::id thread;
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Call> calls;
    et::Result answer {Outcome::Sent, 0, 'S', false};
    bool hold = false;
    bool entered = false;

    std::size_t count()
    {
        std::lock_guard lock(mutex);
        return calls.size();
    }
    Call call(std::size_t i)
    {
        std::lock_guard lock(mutex);
        return calls.at(i);
    }
    void release()
    {
        {
            std::lock_guard lock(mutex);
            hold = false;
        }
        cv.notify_all();
    }
    bool waitEntered(int ms)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return entered; });
    }
};

EndTaskRunner::Backend backendOf(const std::shared_ptr<FakeBackend>& fake)
{
    return [fake](const et::Target& t, et::Signal sig) {
        std::unique_lock lock(fake->mutex);
        fake->calls.push_back({t, sig, std::this_thread::get_id()});
        fake->entered = true;
        fake->cv.notify_all();
        fake->cv.wait_for(lock, std::chrono::seconds(10), [&] { return !fake->hold; });
        return fake->answer;
    };
}

uint32_t ownUid() { return uint32_t(::getuid()); }

ps::ProcessInfo row(int pid, uint64_t start, const char* name, double cpu, uint32_t uid = ownUid(), bool kernel = false)
{
    ps::ProcessInfo p;
    p.pid = pid;
    p.ppid = kernel ? 2 : 1;
    p.startTicks = start;
    p.kernelThread = kernel;
    p.name = name;
    p.command = kernel ? std::string("[") + name + "]" : std::string("/opt/qa/") + name;
    p.user = uid == ownUid() ? "qa" : "other";
    p.uid = uid;
    p.uidKnown = true;
    p.state = 'S';
    p.threads = 1;
    p.cpuPercent = cpu;
    p.rssBytes = kernel ? 0 : 1e6 * double(pid % 97 + 1);
    p.ageSeconds = 10;
    return p;
}

// Synthetic rows: none of these PIDs is ever signalled (the fake stands in).
constexpr int kAlpha = 900001, kBeta = 900002, kOther = 900004, kKernel = 900005, kOdd = 900006;

std::vector<ps::ProcessInfo> rows(uint64_t betaStart = 222, double betaCpu = 50)
{
    return {row(kAlpha, 111, "alpha", 10), row(kBeta, betaStart, "beta", betaCpu),
            row(kOther, 444, "stranger", 5, ownUid() + 1), row(kKernel, 555, "kqa", 1, 0, true),
            row(kOdd, 666, "a<b>&amp;c (d)", 2)};
}

ps::SnapshotPtr snapshotOf(double time, std::vector<ps::ProcessInfo> processes)
{
    static uint64_t seq = 0;
    auto s = std::make_shared<ps::Snapshot>();
    s->sequence = ++seq;
    s->time = time;
    s->interval = 1.0;
    s->continuous = true;
    s->logicalCpus = 4;
    s->processesScanned = true;
    s->threadCount = long(processes.size());
    s->processes = std::move(processes);
    return s;
}

windows::ResultPtr ownersOf(std::vector<windows::Owner> list, double time)
{
    auto r = std::make_shared<windows::Result>();
    r->status = windows::Status::Ok;
    r->backend = windows::Backend::Hyprland;
    r->requested = time;
    r->time = time;
    r->validFor = 3.0;
    r->windows = int(list.size());
    r->owners = std::move(list);
    return r;
}

struct UiEnv {
    QTemporaryDir dir;
    std::shared_ptr<FakeBackend> fake = std::make_shared<FakeBackend>();
    MainWindowOptions options(bool withFake = true) const
    {
        MainWindowOptions o;
        o.startCollector = false;
        o.settingsFile = dir.filePath(QStringLiteral("bex.ini"));
        o.windowProbe.enabled = false;
        o.clock = [] { return 100.0; };
        if (withFake)
            o.endTask = backendOf(fake);
        return o;
    }
};

QTreeView* tableOf(MainWindow& w) { return w.findChild<QTreeView*>(QStringLiteral("processTable")); }
QPushButton* buttonOf(MainWindow& w) { return w.findChild<QPushButton*>(QStringLiteral("endTaskButton")); }
QAction* actionOf(MainWindow& w) { return w.findChild<QAction*>(QStringLiteral("endTaskAction")); }
QLineEdit* filterOf(MainWindow& w) { return w.findChild<QLineEdit*>(QStringLiteral("processFilter")); }

QModelIndex rowIndex(QTreeView* t, int pid)
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
QModelIndex groupIndex(QTreeView* t, int group)
{
    auto* m = t->model();
    for (int g = 0; g < m->rowCount(); ++g)
        if (m->index(g, 0).data(ProcessModel::GroupRole).toInt() == group)
            return m->index(g, 0);
    return {};
}
void select(QTreeView* t, const QModelIndex& i)
{
    t->selectionModel()->setCurrentIndex(i, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}
// Every expanded, so rows have geometry.
void expandAll(QTreeView* t)
{
    for (int g = 0; g < t->model()->rowCount(); ++g)
        t->expand(t->model()->index(g, 0));
}

QMessageBox* openBox(const char* name)
{
    for (QWidget* w : QApplication::allWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(w); box && box->objectName() == QLatin1String(name) && box->isVisible())
            return box;
    return nullptr;
}
int visibleBoxes(const char* name)
{
    int n = 0;
    for (QWidget* w : QApplication::allWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(w); box && box->objectName() == QLatin1String(name) && box->isVisible())
            ++n;
    return n;
}
QMenu* openMenu()
{
    for (QWidget* w : QApplication::allWidgets())
        if (auto* m = qobject_cast<QMenu*>(w); m && m->objectName() == QLatin1String("processContextMenu") && m->isVisible())
            return m;
    return nullptr;
}
QAction* menuAction(QMenu* m, const char* name)
{
    for (QAction* a : m->actions())
        if (a->objectName() == QLatin1String(name))
            return a;
    return nullptr;
}
void closeAll(const char* name)
{
    while (QMessageBox* b = openBox(name)) {
        b->reject();
        QCoreApplication::processEvents();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
QAbstractButton* confirmButton(QMessageBox* box)
{
    return box ? box->findChild<QAbstractButton*>(QStringLiteral("endTaskConfirmButton")) : nullptr;
}
// Mouse context menu on the viewport at a point.
void mouseMenu(QTreeView* t, const QPoint& pos)
{
    QContextMenuEvent e(QContextMenuEvent::Mouse, pos, t->viewport()->mapToGlobal(pos));
    QCoreApplication::sendEvent(t->viewport(), &e);
    QCoreApplication::processEvents();
}
void keyboardMenu(QTreeView* t)
{
    const QRect r = t->visualRect(t->currentIndex());
    const QPoint pos = r.isValid() ? r.center() : QPoint(4, 4);
    QContextMenuEvent e(QContextMenuEvent::Keyboard, pos, t->mapToGlobal(pos));
    QCoreApplication::sendEvent(t, &e);
    QCoreApplication::processEvents();
}
void closeMenus()
{
    while (QMenu* m = openMenu()) {
        m->close();
        QCoreApplication::processEvents();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

struct Window {
    UiEnv env;
    std::unique_ptr<MainWindow> w;
    QTreeView* t = nullptr;
    explicit Window(bool withFake = true, std::vector<ps::ProcessInfo> processes = rows())
    {
        w = std::make_unique<MainWindow>(env.options(withFake));
        w->applySnapshot(snapshotOf(99.5, std::move(processes)));
        w->showPage(MainWindow::ProcessPage);
        w->resize(1100, 700);
        w->show();
        [[maybe_unused]] const bool exposed = QTest::qWaitForWindowExposed(w.get());
        t = tableOf(*w);
        if (t)
            expandAll(t);
    }
    ~Window()
    {
        closeMenus();
        closeAll("endTaskConfirmBox");
        closeAll("endTaskErrorBox");
        if (w) {
            env.fake->release();
            w->shutdown(3000);
        }
    }
};

} // namespace
#endif

class TestEndTaskAdversarial : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void fixtureDefaultChildEndsOnTerm();
    void fixtureIgnoringChildSurvivesTermEndsOnKill();
    void fixtureLeavesNoPidfd();
#ifdef PS_ET_CORE
    void precheckNeedsNoIo_data();
    void precheckNeedsNoIo();
    void targetOfCopiesIdentity();
    void terminateEndsOwnChild();
    void terminateNeverEscalates();
    void killEndsChildIgnoringTerm();
    void invalidSignalIsNeverSent();
    void changedIdentityIsNotSignalled_data();
    void changedIdentityIsNotSignalled();
    void refusedBeforeOpening_data();
    void refusedBeforeOpening();
    void liveKernelThreadIsProtected();
    void zombieIsExitedNotSent();
    void reapedTargetIsNeverSignalled();
    void stoppedChildRequestIsSentTruthfully();
    void openFailuresMapAndSendNothing_data();
    void openFailuresMapAndSendNothing();
    void sendFailuresMapAndCloseThePidfd_data();
    void sendFailuresMapAndCloseThePidfd();
    void swappedPidfdIsRefused();
    void nonPidfdDescriptorIsRefused();
    void exitJustBeforeSendIsExited();
    void descendantsAndGroupUntouched();
    void unusualCommStillMatches_data();
    void unusualCommStillMatches();
    void otherPidNamespaceRefuses();
    void pidfdCheckReadErrorIsFailed();
#endif
#ifdef PS_ET_APP
    void uiControlsAreNotOnTheToolbar();
    void uiButtonFollowsSelection();
    void uiCancelIsSafeDefault();
    void uiConfirmationTargetsTheCopiedProcess();
    void uiForceIsSeparateAndExplicit();
    void uiMouseMenuTargetsRowUnderPointer();
    void uiNoMenuOnGroupOrEmptySpace();
    void uiUnactionableRowsNeverSignal();
    void uiKeyboardMenuUsesSelection();
    void uiDeleteOnlyInTheTree();
    void uiDuplicateRequestsAreBlocked();
    void uiClosingWithConfirmationOpenSendsNothing();
    void uiClosingWhileRequestRunsIsSafe();
    void uiWorkerThreadFailureIsReported();
    void uiOutcomesAreReportedTruthfully();
    void uiNameIsPlainText();
    void uiPausedStillWorksAndChangesNothing();
    void uiWithoutRealProcControlsStayDisabled();
    void uiLiveEndToEndOnOwnChild();
#endif
};

void TestEndTaskAdversarial::initTestCase()
{
    struct sigaction sa {};
    sa.sa_handler = onSelfTerm;
    ::sigemptyset(&sa.sa_mask);
    QVERIFY(::sigaction(SIGTERM, &sa, nullptr) == 0);
}

void TestEndTaskAdversarial::cleanupTestCase()
{
    QCOMPARE(int(g_selfTerms), 0);
}

// The fixtures themselves: a child that SIGTERM ends, one that survives it,
// and no handle left behind. Signals here go through the test's own pidfd.
void TestEndTaskAdversarial::fixtureDefaultChildEndsOnTerm()
{
    OwnedChild c("default");
    QVERIFY(c.ready());
    QCOMPARE(startTicksOf(c.pid()).value_or(0), c.startTicks());
    QVERIFY(c.aliveAfter(100));
    QVERIFY(c.signal(SIGTERM));
    QVERIFY2(c.waitExit(3000), qPrintable(c.how()));
    QVERIFY2(c.killedBy(SIGTERM), qPrintable(c.how()));
}

void TestEndTaskAdversarial::fixtureIgnoringChildSurvivesTermEndsOnKill()
{
    OwnedChild c("ignore-term");
    QVERIFY(c.ready());
    QVERIFY(c.signal(SIGTERM));
    QVERIFY(c.waitFor('T', 2000));
    QVERIFY2(c.aliveAfter(500), qPrintable(c.how()));
    QVERIFY(c.signal(SIGKILL));
    QVERIFY2(c.waitExit(3000), qPrintable(c.how()));
    QVERIFY2(c.killedBy(SIGKILL), qPrintable(c.how()));
}

void TestEndTaskAdversarial::fixtureLeavesNoPidfd()
{
    const int before = openPidfds();
    {
        OwnedChild a("default");
        OwnedChild b("ignore-term");
        QVERIFY(a.ready() && b.ready());
        if (!a.hasPidfd())
            QSKIP("no pidfd_open on this kernel");
        QCOMPARE(openPidfds(), before + 2);
    } // destructors force-end through their own pidfds and reap
    QCOMPARE(openPidfds(), before);
}

#ifdef PS_ET_CORE

// ---------------------------------------------------------------------------
// Backend: ps::endtask

void TestEndTaskAdversarial::precheckNeedsNoIo_data()
{
    QTest::addColumn<int>("pid");
    QTest::addColumn<bool>("uidKnown");
    QTest::addColumn<int>("uidDelta");
    QTest::addColumn<bool>("kernelThread");
    QTest::addColumn<int>("expected");
    const int self = 4242;
    QTest::newRow("pid 0") << 0 << true << 0 << false << int(Outcome::Invalid);
    QTest::newRow("pid -1") << -1 << true << 0 << false << int(Outcome::Invalid);
    QTest::newRow("pid INT_MIN") << INT_MIN << true << 0 << false << int(Outcome::Invalid);
    QTest::newRow("pid 0, kernel thread") << 0 << true << 0 << true << int(Outcome::Invalid);
    QTest::newRow("pid 1") << 1 << true << 0 << false << int(Outcome::Protected);
    QTest::newRow("pid 1, other owner") << 1 << true << 1 << false << int(Outcome::Protected);
    QTest::newRow("self") << self << true << 0 << false << int(Outcome::Protected);
    QTest::newRow("kernel thread") << 777 << true << 0 << true << int(Outcome::Protected);
    QTest::newRow("kernel thread, root") << 778 << true << 1 << true << int(Outcome::Protected);
    QTest::newRow("owner unknown") << 779 << false << 0 << false << int(Outcome::NotOwned);
    QTest::newRow("other owner") << 780 << true << 1 << false << int(Outcome::NotOwned);
    QTest::newRow("own, pid 2") << 2 << true << 0 << false << int(Outcome::Sent);
    QTest::newRow("own") << 781 << true << 0 << false << int(Outcome::Sent);
}

void TestEndTaskAdversarial::precheckNeedsNoIo()
{
    QFETCH(int, pid);
    QFETCH(bool, uidKnown);
    QFETCH(int, uidDelta);
    QFETCH(bool, kernelThread);
    QFETCH(int, expected);
    const uint32_t own = 1000;
    et::Target t;
    t.pid = pid;
    t.startTicks = 5;
    t.name = "x";
    t.uid = own + uint32_t(uidDelta);
    t.uidKnown = uidKnown;
    t.kernelThread = kernelThread;
    QCOMPARE(outcomeName(et::precheck(t, own, 4242)), outcomeName(Outcome(expected)));
}

void TestEndTaskAdversarial::targetOfCopiesIdentity()
{
    ps::ProcessInfo p;
    p.pid = 31337;
    p.startTicks = (uint64_t(1) << 53) + 3; // not representable as a double
    p.uid = 4000000000u;
    p.uidKnown = true;
    p.kernelThread = true;
    p.name = "a<b>&c (d)";
    p.command = "/other --thing";
    p.user = "someone";
    const et::Target t = et::targetOf(p);
    QCOMPARE(t.pid, p.pid);
    QCOMPARE(t.startTicks, p.startTicks);
    QCOMPARE(t.name, p.name);
    QCOMPARE(t.uid, p.uid);
    QCOMPARE(t.uidKnown, true);
    QCOMPARE(t.kernelThread, true);
    p.name = "changed"; // a copy, not a view
    QCOMPARE(t.name, std::string("a<b>&c (d)"));
}

void TestEndTaskAdversarial::terminateEndsOwnChild()
{
    const int pidfds = openPidfds();
    OwnedChild c("default");
    QVERIFY(c.ready());
    const et::Target t = listed(c);
    QCOMPARE(t.name, std::string(kChildComm));
    const et::Result r = et::send(t, et::Signal::Terminate);
    QVERIFY2(r.outcome == Outcome::Sent, qPrintable(describeResult(r)));
    QCOMPARE(r.error, 0);
    QVERIFY2(c.waitExit(3000) && c.killedBy(SIGTERM), qPrintable(c.how()));
    // Truthful: a request sent, not a claimed ending; names what it targeted.
    const QString text = QString::fromStdString(et::describe(r, t, et::Signal::Terminate));
    QVERIFY2(text.contains(QString::number(t.pid)) || text.contains(QLatin1String(kChildComm)), qPrintable(text));
    for (const char* claim : {"has ended", "was ended", "has been ended", "was terminated", "has been terminated",
                              "was killed", "has been killed", "is gone"})
        QVERIFY2(!text.contains(QLatin1String(claim), Qt::CaseInsensitive), qPrintable(text));
    QCOMPARE(openPidfds(), pidfds + 1); // only the fixture's own, until it goes
}

void TestEndTaskAdversarial::terminateNeverEscalates()
{
    OwnedChild c("ignore-term");
    QVERIFY(c.ready());
    const et::Result r = et::send(listed(c), et::Signal::Terminate);
    QVERIFY2(r.outcome == Outcome::Sent, qPrintable(describeResult(r)));
    QVERIFY(c.waitFor('T', 2000));
    QVERIFY2(c.aliveAfter(1500), qPrintable(c.how())); // no SIGKILL follows by itself
    QCOMPARE(c.termsSeen(), 1);
    const et::Result k = et::send(listed(c), et::Signal::Kill);
    QVERIFY2(k.outcome == Outcome::Sent, qPrintable(describeResult(k)));
    QVERIFY2(c.waitExit(3000) && c.killedBy(SIGKILL), qPrintable(c.how()));
    QCOMPARE(c.termsSeen(), 1);
}

void TestEndTaskAdversarial::killEndsChildIgnoringTerm()
{
    OwnedChild c("ignore-term");
    QVERIFY(c.ready());
    Recorder rec;
    rec.realSend = true;
    const et::Result r = et::send(listed(c), et::Signal::Kill, rec.hooks());
    QVERIFY2(r.outcome == Outcome::Sent, qPrintable(describeResult(r)));
    QCOMPARE(int(rec.sends.size()), 1);
    QCOMPARE(rec.sends[0].sig, SIGKILL);
    QCOMPARE(rec.sends[0].fdinfoPid, int(c.pid()));
    QVERIFY2(c.waitExit(3000) && c.killedBy(SIGKILL), qPrintable(c.how()));
    QCOMPARE(c.termsSeen(), 0); // no SIGTERM first
}

void TestEndTaskAdversarial::invalidSignalIsNeverSent()
{
    OwnedChild c("ignore-term");
    QVERIFY(c.ready());
    for (int raw : {2, 3, -1, 9, 15, 255}) {
        Recorder rec;
        rec.realSend = true;
        const et::Result r = et::send(listed(c), static_cast<et::Signal>(raw), rec.hooks());
        QVERIFY2(r.outcome != Outcome::Sent,
                 qPrintable(QStringLiteral("Signal(%1): %2, sent %3").arg(raw).arg(describeResult(r)).arg(rec.sends.size())));
        QVERIFY2(rec.sends.empty(), qPrintable(QStringLiteral("Signal(%1) sent signal %2").arg(raw).arg(rec.sends.empty() ? 0 : rec.sends.front().sig)));
        QVERIFY(!et::describe(r, listed(c), static_cast<et::Signal>(raw)).empty());
    }
    QVERIFY2(c.aliveAfter(300), qPrintable(c.how()));
    QCOMPARE(c.termsSeen(), 0);
}

void TestEndTaskAdversarial::changedIdentityIsNotSignalled_data()
{
    QTest::addColumn<int>("signal");
    QTest::addColumn<QString>("change");
    for (int sig : {int(et::Signal::Terminate), int(et::Signal::Kill)}) {
        const char* s = sig == int(et::Signal::Terminate) ? "End" : "Force";
        for (const char* change : {"start+1", "start-1", "start 0", "start+2^40", "name other", "name prefix",
                                   "name longer", "name case", "name empty", "listed uid other, own uid same"})
            QTest::newRow(qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(s), QLatin1String(change))))
                << sig << QString::fromLatin1(change);
    }
}

void TestEndTaskAdversarial::changedIdentityIsNotSignalled()
{
    QFETCH(int, signal);
    QFETCH(QString, change);
    const int pidfds = openPidfds();
    {
        OwnedChild c("ignore-term");
        QVERIFY(c.ready());
        et::Target t = listed(c);
        Recorder rec;
        rec.realSend = true; // the check alone must stop it
        auto hooks = rec.hooks();
        if (change == QLatin1String("start+1"))
            t.startTicks += 1;
        else if (change == QLatin1String("start-1"))
            t.startTicks -= 1;
        else if (change == QLatin1String("start 0"))
            t.startTicks = 0;
        else if (change == QLatin1String("start+2^40"))
            t.startTicks += uint64_t(1) << 40;
        else if (change == QLatin1String("name other"))
            t.name = "someone-else";
        else if (change == QLatin1String("name prefix"))
            t.name = t.name.substr(0, t.name.size() - 1);
        else if (change == QLatin1String("name longer"))
            t.name += "x";
        else if (change == QLatin1String("name case"))
            t.name = "PS-ET-CHILD";
        else if (change == QLatin1String("name empty"))
            t.name.clear();
        else { // listed and own UID agree with each other but not with the process
            t.uid = ::getuid() + 1;
            hooks.ownUid = ::getuid() + 1;
        }
        const et::Result r = et::send(t, et::Signal(signal), hooks);
        QVERIFY2(r.outcome == Outcome::Changed, qPrintable(describeResult(r)));
        QVERIFY2(rec.sends.empty(), "a signal was sent to a process that no longer matches");
        QVERIFY2(c.aliveAfter(300), qPrintable(c.how()));
        QCOMPARE(c.termsSeen(), 0);
    }
    QCOMPARE(openPidfds(), pidfds);
}

void TestEndTaskAdversarial::refusedBeforeOpening_data()
{
    QTest::addColumn<QString>("which");
    QTest::addColumn<int>("expected");
    QTest::newRow("PID 1") << QStringLiteral("pid1") << int(Outcome::Protected);
    QTest::newRow("bex itself") << QStringLiteral("self") << int(Outcome::Protected);
    QTest::newRow("listed as kernel thread") << QStringLiteral("kthread") << int(Outcome::Protected);
    QTest::newRow("owner unknown") << QStringLiteral("unknown") << int(Outcome::NotOwned);
    QTest::newRow("listed other owner") << QStringLiteral("other") << int(Outcome::NotOwned);
    QTest::newRow("own account differs") << QStringLiteral("ownuid") << int(Outcome::NotOwned);
    QTest::newRow("pid 0") << QStringLiteral("zero") << int(Outcome::Invalid);
    QTest::newRow("negative pid (a group)") << QStringLiteral("negative") << int(Outcome::Invalid);
}

void TestEndTaskAdversarial::refusedBeforeOpening()
{
    QFETCH(QString, which);
    QFETCH(int, expected);
    OwnedChild c("ignore-term");
    QVERIFY(c.ready());
    et::Target t = listed(c);
    Recorder rec;
    rec.realOpen = false; // nothing real is ever opened or signalled here
    auto hooks = rec.hooks();
    if (which == QLatin1String("pid1")) {
        t.pid = 1;
    } else if (which == QLatin1String("self")) {
        t.pid = ::getpid();
        t.startTicks = startTicksOf(::getpid()).value_or(0);
        std::ifstream in("/proc/self/comm");
        std::getline(in, t.name);
    } else if (which == QLatin1String("kthread")) {
        t.kernelThread = true;
    } else if (which == QLatin1String("unknown")) {
        t.uidKnown = false;
    } else if (which == QLatin1String("other")) {
        t.uid = ::getuid() + 1;
    } else if (which == QLatin1String("ownuid")) {
        hooks.ownUid = ::getuid() + 1;
    } else if (which == QLatin1String("zero")) {
        t.pid = 0;
    } else {
        t.pid = -c.pid();
    }
    for (et::Signal sig : {et::Signal::Terminate, et::Signal::Kill}) {
        const et::Result r = et::send(t, sig, hooks);
        QCOMPARE(outcomeName(r.outcome), outcomeName(Outcome(expected)));
        QVERIFY(!et::describe(r, t, sig).empty());
    }
    QVERIFY2(rec.opens.empty(), qPrintable(QStringLiteral("pidfd_open(%1) for a refused target").arg(rec.opens.empty() ? 0 : rec.opens.front())));
    QVERIFY(rec.sends.empty());
    QVERIFY(c.aliveAfter(200));
    QCOMPARE(c.termsSeen(), 0);
    QCOMPARE(int(g_selfTerms), 0);
}

void TestEndTaskAdversarial::liveKernelThreadIsProtected()
{
    // PID 2 (kthreadd) listed as an ordinary process of this account: only the
    // live PF_KTHREAD flag can refuse it. Signals are refused by the recorder,
    // so nothing can reach it whatever the code under test does.
    const auto ticks = startTicksOf(2);
    if (!ticks)
        QSKIP("no PID 2 in this PID namespace");
    std::ifstream in("/proc/2/comm");
    et::Target t;
    t.pid = 2;
    t.startTicks = *ticks;
    std::getline(in, t.name);
    t.uid = 0;
    t.uidKnown = true;
    Recorder rec;
    auto hooks = rec.hooks();
    hooks.ownUid = 0;
    const et::Result r = et::send(t, et::Signal::Kill, hooks);
    QVERIFY2(r.outcome == Outcome::Protected, qPrintable(describeResult(r)));
    QVERIFY(rec.sends.empty());
}

void TestEndTaskAdversarial::zombieIsExitedNotSent()
{
    OwnedChild c("exit");
    QVERIFY(c.ready());
    const et::Target t = listed(c);
    QVERIFY(c.waitZombie(3000));
    Recorder rec;
    rec.realSend = true;
    const et::Result r = et::send(t, et::Signal::Kill, rec.hooks());
    QVERIFY2(r.outcome == Outcome::Exited, qPrintable(describeResult(r)));
    QVERIFY(rec.sends.empty());
    QVERIFY(c.waitExit(1000));
    QVERIFY2(!c.killedBy(SIGKILL), qPrintable(c.how()));
}

void TestEndTaskAdversarial::reapedTargetIsNeverSignalled()
{
    et::Target t;
    {
        OwnedChild c("default");
        QVERIFY(c.ready());
        t = listed(c);
    } // killed and reaped: the PID may now belong to anyone
    Recorder rec; // refuses every signal
    const int pidfds = openPidfds();
    const et::Result r = et::send(t, et::Signal::Kill, rec.hooks());
    QVERIFY2(r.outcome == Outcome::Exited || r.outcome == Outcome::Changed, qPrintable(describeResult(r)));
    QVERIFY(rec.sends.empty());
    QCOMPARE(openPidfds(), pidfds);
}

void TestEndTaskAdversarial::stoppedChildRequestIsSentTruthfully()
{
    OwnedChild c("default");
    QVERIFY(c.ready());
    QVERIFY(c.signal(SIGSTOP));
    QVERIFY(c.waitStopped(3000));
    const et::Target t = listed(c);
    const et::Result r = et::send(t, et::Signal::Terminate);
    QVERIFY2(r.outcome == Outcome::Sent, qPrintable(describeResult(r)));
    QVERIFY2(r.state == 'T' || r.state == 't', qPrintable(describeResult(r)));
    QVERIFY2(c.aliveAfter(500), "a stopped process acted on SIGTERM before it was continued");
    // The message must not claim an ending that cannot happen yet.
    const QString text = QString::fromStdString(et::describe(r, t, et::Signal::Terminate));
    QVERIFY2(text.contains(QLatin1String("stopped"), Qt::CaseInsensitive), qPrintable(text));
    QVERIFY(c.signal(SIGCONT));
    QVERIFY2(c.waitExit(3000) && c.killedBy(SIGTERM), qPrintable(c.how()));
}

void TestEndTaskAdversarial::openFailuresMapAndSendNothing_data()
{
    QTest::addColumn<int>("err");
    QTest::addColumn<int>("expected");
    QTest::newRow("ESRCH") << ESRCH << int(Outcome::Exited);
    QTest::newRow("ENOSYS") << ENOSYS << int(Outcome::Unsupported);
    QTest::newRow("EPERM") << EPERM << int(Outcome::Unsupported);
    QTest::newRow("EINVAL") << EINVAL << int(Outcome::Changed);
    QTest::newRow("EMFILE") << EMFILE << int(Outcome::Failed);
    QTest::newRow("ENFILE") << ENFILE << int(Outcome::Failed);
    QTest::newRow("ENOMEM") << ENOMEM << int(Outcome::Failed);
}

void TestEndTaskAdversarial::openFailuresMapAndSendNothing()
{
    QFETCH(int, err);
    QFETCH(int, expected);
    OwnedChild c("ignore-term");
    QVERIFY(c.ready());
    Recorder rec;
    rec.realSend = true;
    auto hooks = rec.hooks();
    int opens = 0;
    hooks.pidfdOpen = [&](int) {
        ++opens;
        errno = err;
        return -1;
    };
    bool before = false;
    hooks.beforeSend = [&] { before = true; };
    for (et::Signal sig : {et::Signal::Terminate, et::Signal::Kill}) {
        const et::Result r = et::send(listed(c), sig, hooks);
        QCOMPARE(outcomeName(r.outcome), outcomeName(Outcome(expected)));
        QCOMPARE(r.error, err);
        QVERIFY(!et::describe(r, listed(c), sig).empty());
    }
    QCOMPARE(opens, 2);
    QVERIFY(!before);
    QVERIFY(rec.sends.empty());
    QVERIFY(c.aliveAfter(200));
    QCOMPARE(c.termsSeen(), 0);
}

void TestEndTaskAdversarial::sendFailuresMapAndCloseThePidfd_data()
{
    QTest::addColumn<int>("err");
    QTest::addColumn<int>("expected");
    QTest::newRow("ESRCH") << ESRCH << int(Outcome::Exited);
    QTest::newRow("EPERM") << EPERM << int(Outcome::Denied);
    QTest::newRow("ENOSYS") << ENOSYS << int(Outcome::Unsupported);
    QTest::newRow("EIO") << EIO << int(Outcome::Failed);
    QTest::newRow("EINVAL") << EINVAL << int(Outcome::Failed);
}

void TestEndTaskAdversarial::sendFailuresMapAndCloseThePidfd()
{
    QFETCH(int, err);
    QFETCH(int, expected);
    OwnedChild c("ignore-term");
    QVERIFY(c.ready());
    const int pidfds = openPidfds();
    const int fds = openFds();
    for (et::Signal sig : {et::Signal::Terminate, et::Signal::Kill}) {
        Recorder rec;
        rec.sendErrno = err;
        bool before = false;
        auto hooks = rec.hooks();
        hooks.beforeSend = [&] { before = true; };
        const et::Result r = et::send(listed(c), sig, hooks);
        QCOMPARE(outcomeName(r.outcome), outcomeName(Outcome(expected)));
        QCOMPARE(r.error, err);
        QVERIFY(before);
        QCOMPARE(int(rec.sends.size()), 1); // one attempt, no retry or fallback
        QCOMPARE(rec.sends[0].sig, sig == et::Signal::Kill ? SIGKILL : SIGTERM);
        QCOMPARE(rec.sends[0].fdinfoPid, int(c.pid())); // the pidfd is the target's
        QCOMPARE(openPidfds(), pidfds);
        QCOMPARE(openFds(), fds);
    }
    QVERIFY(c.aliveAfter(200));
    QCOMPARE(c.termsSeen(), 0);
}

void TestEndTaskAdversarial::swappedPidfdIsRefused()
{
    // pidfd_open hands back another process's pidfd (a reused PID, a bug):
    // the /proc checks must be tied to the pidfd, and nobody signalled.
    OwnedChild a("ignore-term");
    OwnedChild b("ignore-term");
    QVERIFY(a.ready() && b.ready());
    const int fds = openFds();
    Recorder rec;
    rec.realSend = true;
    auto hooks = rec.hooks();
    hooks.pidfdOpen = [&](int) { return sysPidfdOpen(b.pid()); };
    const et::Result r = et::send(listed(a), et::Signal::Terminate, hooks);
    QVERIFY2(r.outcome != Outcome::Sent, qPrintable(describeResult(r)));
    QVERIFY2(r.outcome == Outcome::Unsupported || r.outcome == Outcome::Changed, qPrintable(describeResult(r)));
    QVERIFY(rec.sends.empty());
    QCOMPARE(openFds(), fds);
    QVERIFY(a.aliveAfter(200) && b.aliveAfter(0));
    QCOMPARE(a.termsSeen() + b.termsSeen(), 0);
}

void TestEndTaskAdversarial::nonPidfdDescriptorIsRefused()
{
    OwnedChild c("ignore-term");
    QVERIFY(c.ready());
    const int fds = openFds();
    Recorder rec;
    rec.realSend = true;
    auto hooks = rec.hooks();
    hooks.pidfdOpen = [](int) { return ::open("/dev/null", O_RDONLY | O_CLOEXEC); };
    const et::Result r = et::send(listed(c), et::Signal::Kill, hooks);
    QVERIFY2(r.outcome == Outcome::Unsupported, qPrintable(describeResult(r)));
    QVERIFY(rec.sends.empty());
    QCOMPARE(openFds(), fds); // the backend owns and closes what it was given
    QVERIFY(c.aliveAfter(200));
}

void TestEndTaskAdversarial::exitJustBeforeSendIsExited()
{
    OwnedChild c("ignore-term");
    QVERIFY(c.ready());
    const int pidfds = openPidfds();
    et::Hooks hooks;
    hooks.beforeSend = [&] {
        c.signal(SIGKILL);
        c.waitExit(3000); // reaped: its PID is free for reuse from here on
    };
    const et::Result r = et::send(listed(c), et::Signal::Terminate, hooks);
    QVERIFY2(r.outcome == Outcome::Exited, qPrintable(describeResult(r)));
    QCOMPARE(r.error, ESRCH);
    QVERIFY2(c.killedBy(SIGKILL), qPrintable(c.how()));
    QCOMPARE(openPidfds(), pidfds); // the backend closed its own; the fixture keeps its until destroyed
}

void TestEndTaskAdversarial::descendantsAndGroupUntouched()
{
    OwnedChild c("grandchild");
    QVERIFY(c.ready());
    QVERIFY(c.waitFor('g', 3000));
    QVERIFY(::getpgid(c.pid()) == c.pid() && ::getpgid(0) != c.pid());
    const et::Result r = et::send(listed(c), et::Signal::Terminate);
    QVERIFY2(r.outcome == Outcome::Sent, qPrintable(describeResult(r)));
    QVERIFY(c.waitFor('T', 2000));
    QTest::qWait(300);
    QCOMPARE(c.termsSeen(), 1);
    QCOMPARE(c.seen('G'), 0); // its child (same group) received nothing
    QCOMPARE(int(g_selfTerms), 0);
}

void TestEndTaskAdversarial::unusualCommStillMatches_data()
{
    QTest::addColumn<QByteArray>("comm");
    QTest::newRow("markup and ampersand") << QByteArray("a<b>&c</b>");
    QTest::newRow("parentheses") << QByteArray("x) (y) z");
    QTest::newRow("spaces") << QByteArray("  two  words ");
    QTest::newRow("newline and tab") << QByteArray("line\nnext\tx");
    QTest::newRow("escape char") << QByteArray("esc\x1b[31mred");
    QTest::newRow("backslash") << QByteArray("back\\slash");
    QTest::newRow("UTF-8") << QByteArray("caf\xc3\xa9 \xe2\x9c\x93");
    QTest::newRow("15 bytes") << QByteArray("abcdefghijklmno");
}

void TestEndTaskAdversarial::unusualCommStillMatches()
{
    QFETCH(QByteArray, comm);
    OwnedChild c("default", comm.toStdString());
    QVERIFY(c.ready());
    const et::Target t = listed(c);
    QVERIFY(!t.name.empty());
    const et::Result r = et::send(t, et::Signal::Terminate);
    QVERIFY2(r.outcome == Outcome::Sent, qPrintable(describeResult(r) + QStringLiteral(" for listed name \"%1\"")
                                                                           .arg(QString::fromStdString(t.name))));
    QVERIFY2(c.waitExit(3000) && c.killedBy(SIGTERM), qPrintable(c.how()));
}

// A backend running in a child PID namespace that still sees the host's
// /proc: numbers there are not its PIDs, so it must refuse rather than
// signal whatever that number means to it. The probe runs in its own
// subprocess; its only possible target is a child of this test.
void TestEndTaskAdversarial::otherPidNamespaceRefuses()
{
    OwnedChild target("ignore-term");
    QVERIFY(target.ready());
    const et::Target t = listed(target);
    QProcess probe;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PS_ET_CHILD"), QStringLiteral("ns-probe"));
    env.insert(QStringLiteral("PS_ET_TARGET"),
               QStringLiteral("%1 %2 %3 %4").arg(t.pid).arg(t.startTicks).arg(QString::fromStdString(toHex(t.name))).arg(t.uid));
    probe.setProcessEnvironment(env);
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(QCoreApplication::applicationFilePath(), {});
    QVERIFY(probe.waitForStarted(5000));
    if (!probe.waitForFinished(20000)) {
        probe.kill();
        probe.waitForFinished(3000);
        QFAIL("namespace probe did not finish");
    }
    const QString out = QString::fromLocal8Bit(probe.readAll());
    if (probe.exitCode() == 77)
        QSKIP(qPrintable(QStringLiteral("no unprivileged user+PID namespace here: ") + out.trimmed()));
    QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0, qPrintable(out));
    QVERIFY2(!out.contains(QLatin1String("FAIL")), qPrintable(out));
    QVERIFY2(target.aliveAfter(200), qPrintable(target.how()));
    QCOMPARE(target.termsSeen(), 0);
}

// The pidfd's fdinfo cannot be read because descriptors ran out (EMFILE): a
// local failure with its errno, not a claimed namespace mismatch, and no
// signal. The descriptors run out only in a subprocess of this test (its own
// RLIMIT_NOFILE); its only possible target is this test's child, and its
// recorder refuses every signal.
void TestEndTaskAdversarial::pidfdCheckReadErrorIsFailed()
{
    OwnedChild target("ignore-term");
    QVERIFY(target.ready());
    const et::Target t = listed(target);
    QProcess probe;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PS_ET_CHILD"), QStringLiteral("fd-exhaust"));
    env.insert(QStringLiteral("PS_ET_TARGET"),
               QStringLiteral("%1 %2 %3 %4").arg(t.pid).arg(t.startTicks).arg(QString::fromStdString(toHex(t.name))).arg(t.uid));
    probe.setProcessEnvironment(env);
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(QCoreApplication::applicationFilePath(), {});
    QVERIFY(probe.waitForStarted(5000));
    if (!probe.waitForFinished(20000)) {
        probe.kill();
        probe.waitForFinished(3000);
        QFAIL("descriptor-exhaustion probe did not finish");
    }
    const QString out = QString::fromLocal8Bit(probe.readAll());
    if (probe.exitCode() == 77)
        QSKIP(qPrintable(out.trimmed()));
    QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0, qPrintable(out));
    QVERIFY2(!out.contains(QLatin1String("FAIL")), qPrintable(out));
    QVERIFY2(target.aliveAfter(200), qPrintable(target.how()));
    QCOMPARE(target.termsSeen(), 0);
}

#endif // PS_ET_CORE

#ifdef PS_ET_APP

// ---------------------------------------------------------------------------
// The Processes page. A fake backend stands in, except in the one live case
// whose only target is this test's own child.

void TestEndTaskAdversarial::uiControlsAreNotOnTheToolbar()
{
    Window x;
    QVERIFY(x.t && buttonOf(*x.w) && actionOf(*x.w) && filterOf(*x.w));
    for (QToolBar* tb : x.w->findChildren<QToolBar*>()) {
        for (QAction* a : tb->actions()) {
            QVERIFY2(!a->text().contains(QLatin1String("end task"), Qt::CaseInsensitive), qPrintable(a->text()));
            QVERIFY(a != actionOf(*x.w));
            if (QWidget* wa = tb->widgetForAction(a))
                QVERIFY(!wa->isAncestorOf(buttonOf(*x.w)) && wa != buttonOf(*x.w));
        }
        QVERIFY(!tb->isAncestorOf(buttonOf(*x.w)));
    }
    // In the Processes header, after the filter.
    QPushButton* b = buttonOf(*x.w);
    QVERIFY(b->isVisible());
    QVERIFY(b->text().startsWith(QLatin1String("End task")));
    QVERIFY(b->mapTo(x.w.get(), QPoint()).x() > filterOf(*x.w)->mapTo(x.w.get(), QPoint()).x());
    QCOMPARE(qAbs(b->mapTo(x.w.get(), b->rect().center()).y() - filterOf(*x.w)->mapTo(x.w.get(), filterOf(*x.w)->rect().center()).y()) < b->height(), true);
    // Delete: a shortcut of the table only.
    QAction* a = actionOf(*x.w);
    QCOMPARE(a->shortcut(), QKeySequence(Qt::Key_Delete));
    QCOMPARE(int(a->shortcutContext()), int(Qt::WidgetShortcut));
    QVERIFY(x.t->actions().contains(a));
    QVERIFY(!filterOf(*x.w)->actions().contains(a));
    QVERIFY(!x.w->actions().contains(a));
}

void TestEndTaskAdversarial::uiButtonFollowsSelection()
{
    Window x;
    QPushButton* b = buttonOf(*x.w);
    x.t->selectionModel()->clearSelection();
    x.t->selectionModel()->setCurrentIndex(QModelIndex(), QItemSelectionModel::Clear);
    QCoreApplication::processEvents();
    QVERIFY2(!b->isEnabled(), "enabled with nothing selected");
    select(x.t, groupIndex(x.t, ProcessModel::Background));
    QVERIFY2(!b->isEnabled(), "enabled on a group row");
    select(x.t, rowIndex(x.t, kAlpha));
    QVERIFY2(b->isEnabled(), qPrintable(b->toolTip()));
    QVERIFY(actionOf(*x.w)->isEnabled());
    for (int pid : {kOther, kKernel}) {
        select(x.t, rowIndex(x.t, pid));
        QVERIFY2(!b->isEnabled(), qPrintable(QStringLiteral("enabled for PID %1").arg(pid)));
        QVERIFY2(!actionOf(*x.w)->isEnabled(), qPrintable(QStringLiteral("action enabled for PID %1").arg(pid)));
        QVERIFY2(!b->toolTip().isEmpty(), "no reason for a disabled End task");
    }
    // Two process rows: not a single target.
    select(x.t, rowIndex(x.t, kAlpha));
    x.t->selectionModel()->select(rowIndex(x.t, kBeta), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    if (x.t->selectionModel()->selectedRows().size() > 1)
        QVERIFY2(!b->isEnabled(), "enabled for two selected processes");
    // A filter that hides the selected row leaves nothing to act on.
    select(x.t, rowIndex(x.t, kAlpha));
    filterOf(*x.w)->setText(QStringLiteral("beta"));
    QCoreApplication::processEvents();
    QVERIFY(!rowIndex(x.t, kAlpha).isValid());
    const bool enabled = b->isEnabled();
    b->click();
    QCoreApplication::processEvents();
    if (QMessageBox* box = openBox("endTaskConfirmBox"))
        QVERIFY2(box->text().contains(QLatin1String("beta")) && !box->text().contains(QLatin1String("alpha")),
                 qPrintable(QStringLiteral("filtered-out alpha offered: ") + box->text()));
    else
        QVERIFY2(!enabled, "enabled but did nothing");
    closeAll("endTaskConfirmBox");
    QCOMPARE(x.env.fake->count(), std::size_t(0));
}

void TestEndTaskAdversarial::uiCancelIsSafeDefault()
{
    Window x;
    QSignalSpy finished(x.w.get(), &MainWindow::endTaskFinished);
    for (int how = 0; how < 4; ++how) {
        select(x.t, rowIndex(x.t, kAlpha));
        buttonOf(*x.w)->click();
        QCoreApplication::processEvents();
        QMessageBox* box = openBox("endTaskConfirmBox");
        QVERIFY(box);
        QVERIFY(x.w->isEndTaskPending());
        QCOMPARE(int(box->icon()), int(QMessageBox::Warning));
        QCOMPARE(int(box->textFormat()), int(Qt::PlainText));
        QCOMPARE(box->property("psForce").toBool(), false);
        QCOMPARE(int(box->windowModality()), int(Qt::WindowModal));
        QAbstractButton* cancel = box->button(QMessageBox::Cancel);
        QVERIFY(cancel);
        QCOMPARE(static_cast<QAbstractButton*>(box->defaultButton()), cancel);
        QCOMPARE(box->escapeButton(), cancel);
        QVERIFY(confirmButton(box));
        QCOMPARE(confirmButton(box)->text(), QStringLiteral("End task"));
        QVERIFY(box->text().contains(QLatin1String("alpha")));
        QVERIFY(box->text().contains(QStringLiteral("PID %1").arg(kAlpha)));
        QVERIFY2(!buttonOf(*x.w)->isEnabled(), "End task offered again while its confirmation is open");
        QPointer<QMessageBox> gone(box);
        switch (how) {
        case 0: QTest::keyClick(box, Qt::Key_Escape); break;
        case 1: QTest::keyClick(box, Qt::Key_Return); break; // the default: Cancel
        case 2: cancel->click(); break;
        default: box->close(); break;
        }
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(!openBox("endTaskConfirmBox"));
        QVERIFY2(gone.isNull(), "the confirmation was not deleted on close");
        QVERIFY(!x.w->isEndTaskPending());
        QVERIFY(buttonOf(*x.w)->isEnabled());
    }
    QTest::qWait(100);
    QCOMPARE(x.env.fake->count(), std::size_t(0));
    QCOMPARE(finished.count(), 0);
}

// Chosen: beta (PID kBeta, start 222). While the confirmation is open the
// list re-sorts, is filtered, regrouped, and finally lists another process
// under the same PID. Confirming still names only the copied identity.
void TestEndTaskAdversarial::uiConfirmationTargetsTheCopiedProcess()
{
    Window x;
    QSignalSpy finished(x.w.get(), &MainWindow::endTaskFinished);
    select(x.t, rowIndex(x.t, kBeta));
    buttonOf(*x.w)->click();
    QCoreApplication::processEvents();
    QMessageBox* box = openBox("endTaskConfirmBox");
    QVERIFY(box);
    x.t->sortByColumn(ProcessModel::Cpu, Qt::AscendingOrder);
    x.w->applySnapshot(snapshotOf(99.6, rows(222, 1)));
    filterOf(*x.w)->setText(QStringLiteral("alpha"));
    x.w->applyWindowOwners(ownersOf({{kAlpha, 111}}, 99.7));
    filterOf(*x.w)->clear();
    x.w->applySnapshot(snapshotOf(99.8, rows(999))); // kBeta reused by another process
    select(x.t, rowIndex(x.t, kAlpha));                // and the selection moved
    QCoreApplication::processEvents();
    QVERIFY(openBox("endTaskConfirmBox") == box);
    confirmButton(box)->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(x.env.fake->count(), std::size_t(1));
    const auto c = x.env.fake->call(0);
    QCOMPARE(c.target.pid, kBeta);
    QCOMPARE(c.target.startTicks, uint64_t(222));
    QCOMPARE(c.target.name, std::string("beta"));
    QCOMPARE(c.target.uid, ownUid());
    QVERIFY(c.target.uidKnown && !c.target.kernelThread);
    QCOMPARE(int(c.signal), int(et::Signal::Terminate));
    QVERIFY2(c.thread != std::this_thread::get_id(), "the backend ran on the UI thread");
    const auto args = finished.at(0);
    QCOMPARE(args.at(0).toInt(), kBeta);
    QCOMPARE(args.at(1).toULongLong(), qulonglong(222));
    QCOMPARE(args.at(2).toInt(), int(et::Signal::Terminate));
    QCOMPARE(args.at(3).toInt(), int(Outcome::Sent));
    QVERIFY(args.at(4).toString().contains(QString::number(kBeta)));
    QVERIFY(!x.w->isEndTaskPending());
    QVERIFY(!openBox("endTaskErrorBox"));
}

void TestEndTaskAdversarial::uiForceIsSeparateAndExplicit()
{
    Window x;
    QSignalSpy finished(x.w.get(), &MainWindow::endTaskFinished);
    select(x.t, rowIndex(x.t, kAlpha));
    mouseMenu(x.t, x.t->visualRect(rowIndex(x.t, kAlpha)).center());
    QMenu* m = openMenu();
    QVERIFY(m);
    QAction* end = menuAction(m, "endTaskMenuAction");
    QAction* force = menuAction(m, "forceEndTaskMenuAction");
    QVERIFY(end && force);
    QVERIFY(end->text().startsWith(QLatin1String("End task")));
    QVERIFY(force->text().startsWith(QLatin1String("Force end task")));
    const int ie = int(m->actions().indexOf(end)), iforce = int(m->actions().indexOf(force));
    QVERIFY(ie < iforce);
    bool separated = false;
    for (int i = ie + 1; i < iforce; ++i)
        separated |= m->actions().at(i)->isSeparator();
    QVERIFY2(separated, "Force end task is not set apart");
    force->trigger();
    closeMenus();
    QMessageBox* box = openBox("endTaskConfirmBox");
    QVERIFY(box);
    QCOMPARE(box->property("psForce").toBool(), true);
    QCOMPARE(confirmButton(box)->text(), QStringLiteral("Force end task"));
    QCOMPARE(static_cast<QAbstractButton*>(box->defaultButton()), box->button(QMessageBox::Cancel));
    QTest::keyClick(box, Qt::Key_Return); // default Cancel, even for Force
    QTRY_VERIFY(!openBox("endTaskConfirmBox"));
    QTest::qWait(50);
    QCOMPARE(x.env.fake->count(), std::size_t(0));
    // Now confirmed: exactly one SIGKILL request, no SIGTERM before it.
    mouseMenu(x.t, x.t->visualRect(rowIndex(x.t, kAlpha)).center());
    QVERIFY(openMenu());
    menuAction(openMenu(), "forceEndTaskMenuAction")->trigger();
    closeMenus();
    QVERIFY(openBox("endTaskConfirmBox"));
    confirmButton(openBox("endTaskConfirmBox"))->click();
    QVERIFY(finished.wait(5000));
    QTest::qWait(100);
    QCOMPARE(x.env.fake->count(), std::size_t(1));
    QCOMPARE(int(x.env.fake->call(0).signal), int(et::Signal::Kill));
    QCOMPARE(finished.at(0).at(2).toInt(), int(et::Signal::Kill));
    // The plain End task stays SIGTERM.
    mouseMenu(x.t, x.t->visualRect(rowIndex(x.t, kAlpha)).center());
    menuAction(openMenu(), "endTaskMenuAction")->trigger();
    closeMenus();
    QCOMPARE(openBox("endTaskConfirmBox")->property("psForce").toBool(), false);
    confirmButton(openBox("endTaskConfirmBox"))->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(x.env.fake->count(), std::size_t(2));
    QCOMPARE(int(x.env.fake->call(1).signal), int(et::Signal::Terminate));
}

void TestEndTaskAdversarial::uiMouseMenuTargetsRowUnderPointer()
{
    Window x;
    QSignalSpy finished(x.w.get(), &MainWindow::endTaskFinished);
    select(x.t, rowIndex(x.t, kAlpha)); // current and selected: alpha
    const QRect r = x.t->visualRect(rowIndex(x.t, kBeta).siblingAtColumn(ProcessModel::Cpu));
    QVERIFY(r.isValid());
    mouseMenu(x.t, r.center());
    QMenu* m = openMenu();
    QVERIFY2(m, "no menu for a process row");
    menuAction(m, "endTaskMenuAction")->trigger();
    closeMenus();
    QMessageBox* box = openBox("endTaskConfirmBox");
    QVERIFY(box);
    QVERIFY2(box->text().contains(QLatin1String("beta")) && !box->text().contains(QLatin1String("alpha")), qPrintable(box->text()));
    // The menu's target is captured at popup time as well.
    confirmButton(box)->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(x.env.fake->call(0).target.pid, kBeta);
    QCOMPARE(x.env.fake->call(0).target.startTicks, uint64_t(222));

    mouseMenu(x.t, x.t->visualRect(rowIndex(x.t, kBeta)).center());
    m = openMenu();
    QVERIFY(m);
    QAction* end = menuAction(m, "endTaskMenuAction");
    x.w->applySnapshot(snapshotOf(99.6, rows(999, 0))); // beta replaced while the menu is open
    x.t->sortByColumn(ProcessModel::Name, Qt::DescendingOrder);
    QCoreApplication::processEvents();
    if (openMenu() && end && end->isEnabled()) {
        end->trigger();
        closeMenus();
        if (QMessageBox* b2 = openBox("endTaskConfirmBox")) {
            confirmButton(b2)->click();
            QVERIFY(finished.wait(5000));
            QCOMPARE(x.env.fake->count(), std::size_t(2));
            QCOMPARE(x.env.fake->call(1).target.startTicks, uint64_t(222)); // the copy, not the new row
        }
    }
    closeMenus();
}

void TestEndTaskAdversarial::uiNoMenuOnGroupOrEmptySpace()
{
    Window x;
    select(x.t, rowIndex(x.t, kAlpha));
    const QRect g = x.t->visualRect(groupIndex(x.t, ProcessModel::Background));
    QVERIFY(g.isValid());
    mouseMenu(x.t, g.center());
    QVERIFY2(!openMenu(), "a menu for a group header (the selected row would be the target)");
    closeMenus();
    const QPoint empty(x.t->viewport()->width() / 2, x.t->viewport()->height() - 2);
    QVERIFY2(!x.t->indexAt(empty).isValid(), "no empty space below the rows");
    mouseMenu(x.t, empty);
    QVERIFY2(!openMenu(), "a menu in empty space (the selected row would be the target)");
    closeMenus();
    QVERIFY(!openBox("endTaskConfirmBox"));
    QCOMPARE(x.env.fake->count(), std::size_t(0));
}

void TestEndTaskAdversarial::uiUnactionableRowsNeverSignal()
{
    Window x;
    for (int pid : {kOther, kKernel}) {
        select(x.t, rowIndex(x.t, pid));
        buttonOf(*x.w)->click();
        actionOf(*x.w)->trigger();
        QCoreApplication::processEvents();
        QVERIFY2(!openBox("endTaskConfirmBox"), qPrintable(QStringLiteral("confirmation for PID %1").arg(pid)));
        mouseMenu(x.t, x.t->visualRect(rowIndex(x.t, pid)).center());
        if (QMenu* m = openMenu()) {
            for (const char* n : {"endTaskMenuAction", "forceEndTaskMenuAction"}) {
                QAction* a = menuAction(m, n);
                QVERIFY(a);
                QVERIFY2(!a->isEnabled(), qPrintable(QStringLiteral("%1 enabled for PID %2").arg(QLatin1String(n)).arg(pid)));
                a->trigger();
                QCoreApplication::processEvents();
            }
        }
        closeMenus();
        QVERIFY(!openBox("endTaskConfirmBox"));
    }
    QTest::qWait(50);
    QCOMPARE(x.env.fake->count(), std::size_t(0));
}

void TestEndTaskAdversarial::uiKeyboardMenuUsesSelection()
{
    Window x;
    QSignalSpy finished(x.w.get(), &MainWindow::endTaskFinished);
    x.t->setFocus();
    select(x.t, rowIndex(x.t, kBeta));
    keyboardMenu(x.t);
    QMenu* m = openMenu();
    QVERIFY2(m, "no menu from the keyboard");
    menuAction(m, "endTaskMenuAction")->trigger();
    closeMenus();
    QMessageBox* box = openBox("endTaskConfirmBox");
    QVERIFY(box);
    QVERIFY(box->text().contains(QLatin1String("beta")));
    confirmButton(box)->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(x.env.fake->call(0).target.pid, kBeta);
    // A selected group row: no menu from the keyboard either.
    select(x.t, groupIndex(x.t, ProcessModel::Background));
    keyboardMenu(x.t);
    QVERIFY2(!openMenu(), "keyboard menu for a group header");
    closeMenus();
}

void TestEndTaskAdversarial::uiDeleteOnlyInTheTree()
{
    Window x;
    x.w->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(x.w.get()));
    select(x.t, rowIndex(x.t, kAlpha));
    QLineEdit* f = filterOf(*x.w);
    f->setFocus();
    f->setText(QStringLiteral("alphz"));
    f->setCursorPosition(4);
    QTRY_VERIFY(f->hasFocus());
    QTest::keyClick(f, Qt::Key_Delete);
    QCoreApplication::processEvents();
    QCOMPARE(f->text(), QStringLiteral("alph")); // ordinary editing
    QVERIFY2(!openBox("endTaskConfirmBox"), "Delete in the filter asked to end a process");
    f->setText(QStringLiteral("alpha"));
    QVERIFY(rowIndex(x.t, kAlpha).isValid());
    select(x.t, rowIndex(x.t, kAlpha));
    x.t->setFocus();
    QTRY_VERIFY(x.t->hasFocus());
    QTest::keyClick(x.t, Qt::Key_Delete);
    QCoreApplication::processEvents();
    QMessageBox* box = openBox("endTaskConfirmBox");
    QVERIFY2(box, "Delete on a focused process row did not ask");
    QVERIFY(box->text().contains(QLatin1String("alpha")));
    QCOMPARE(box->property("psForce").toBool(), false);
    // Delete again while it is open: nothing more.
    QTest::keyClick(x.t, Qt::Key_Delete);
    QCOMPARE(visibleBoxes("endTaskConfirmBox"), 1);
    closeAll("endTaskConfirmBox");
    QCOMPARE(x.env.fake->count(), std::size_t(0));
}

void TestEndTaskAdversarial::uiDuplicateRequestsAreBlocked()
{
    Window x;
    QSignalSpy finished(x.w.get(), &MainWindow::endTaskFinished);
    select(x.t, rowIndex(x.t, kAlpha));
    buttonOf(*x.w)->click();
    QMessageBox* box = openBox("endTaskConfirmBox");
    QVERIFY(box);
    // Every other way in, while it is open.
    buttonOf(*x.w)->click();
    actionOf(*x.w)->trigger();
    mouseMenu(x.t, x.t->visualRect(rowIndex(x.t, kBeta)).center());
    if (QMenu* m = openMenu()) {
        for (const char* n : {"endTaskMenuAction", "forceEndTaskMenuAction"})
            if (QAction* a = menuAction(m, n))
                a->trigger();
    }
    closeMenus();
    QCOMPARE(visibleBoxes("endTaskConfirmBox"), 1);
    QVERIFY(openBox("endTaskConfirmBox") == box);
    // Confirm while the backend is held: still pending, nothing new offered.
    x.env.fake->hold = true;
    confirmButton(box)->click();
    QVERIFY(x.env.fake->waitEntered(5000));
    QCoreApplication::processEvents();
    QVERIFY(x.w->isEndTaskPending());
    QVERIFY(!buttonOf(*x.w)->isEnabled());
    buttonOf(*x.w)->click();
    actionOf(*x.w)->trigger();
    QCoreApplication::processEvents();
    QVERIFY2(!openBox("endTaskConfirmBox"), "a second confirmation while a request runs");
    x.env.fake->release();
    QVERIFY(finished.wait(5000));
    QTest::qWait(100);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(x.env.fake->count(), std::size_t(1));
    QCOMPARE(x.env.fake->call(0).target.pid, kAlpha);
    QVERIFY(!x.w->isEndTaskPending());
    QVERIFY(buttonOf(*x.w)->isEnabled());
}

void TestEndTaskAdversarial::uiClosingWithConfirmationOpenSendsNothing()
{
    int finished = 0;
    {
        Window x;
        QObject::connect(x.w.get(), &MainWindow::endTaskFinished, [&] { ++finished; });
        select(x.t, rowIndex(x.t, kAlpha));
        buttonOf(*x.w)->click();
        QVERIFY(openBox("endTaskConfirmBox"));
        const auto calls = [&] { return x.env.fake->count(); };
        x.w->close();
        QCoreApplication::processEvents();
        QCOMPARE(calls(), std::size_t(0));
        QVERIFY(x.w->shutdown(3000));
        x.w.reset(); // destroyed with the confirmation still open
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!openBox("endTaskConfirmBox"));
    }
    QCOMPARE(finished, 0);
}

void TestEndTaskAdversarial::uiClosingWhileRequestRunsIsSafe()
{
    UiEnv env;
    env.fake->hold = true;
    int finished = 0;
    QElapsedTimer t;
    {
        auto w = std::make_unique<MainWindow>(env.options());
        QObject::connect(w.get(), &MainWindow::endTaskFinished, [&] { ++finished; });
        w->applySnapshot(snapshotOf(99.5, rows()));
        w->showPage(MainWindow::ProcessPage);
        w->show();
        QTreeView* tv = tableOf(*w);
        expandAll(tv);
        select(tv, rowIndex(tv, kAlpha));
        buttonOf(*w)->click();
        QVERIFY(openBox("endTaskConfirmBox"));
        confirmButton(openBox("endTaskConfirmBox"))->click();
        QVERIFY(env.fake->waitEntered(5000));
        t.start();
        w->close();
        w->shutdown(500);
        w.reset(); // the request is still held
    }
    QVERIFY2(t.elapsed() < 5000, "closing waited for the whole held request");
    env.fake->release();
    QTest::qWait(300); // the abandoned job ends; nothing may touch the window
    QCoreApplication::processEvents();
    QCOMPARE(finished, 0);
    QCOMPARE(env.fake->count(), std::size_t(1));
}

// The worker thread cannot be created (RLIMIT_NPROC, lowered only inside a
// subprocess of this test, for the one click): an explicit failure, nothing
// sent, and End task usable again afterwards.
void TestEndTaskAdversarial::uiWorkerThreadFailureIsReported()
{
    if (::geteuid() == 0)
        QSKIP("RLIMIT_NPROC does not bind root");
    QProcess probe;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PS_ET_CHILD"), QStringLiteral("ui-thread-fail"));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    probe.setProcessEnvironment(env);
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(QCoreApplication::applicationFilePath(), {});
    QVERIFY(probe.waitForStarted(5000));
    if (!probe.waitForFinished(30000)) {
        probe.kill();
        probe.waitForFinished(3000);
        QFAIL("thread-failure probe did not finish");
    }
    const QString out = QString::fromLocal8Bit(probe.readAll());
    if (probe.exitCode() == 77)
        QSKIP(qPrintable(out.trimmed()));
    QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0, qPrintable(out));
    QVERIFY2(!out.contains(QLatin1String("FAIL")), qPrintable(out));
}

void TestEndTaskAdversarial::uiOutcomesAreReportedTruthfully()
{
    for (Outcome o : {Outcome::Sent, Outcome::Exited, Outcome::Changed, Outcome::NotOwned, Outcome::Protected,
                      Outcome::Invalid, Outcome::Denied, Outcome::Unsupported, Outcome::Failed}) {
        Window x;
        x.env.fake->answer = {o, o == Outcome::Denied ? EPERM : 0, 'S', false};
        QSignalSpy finished(x.w.get(), &MainWindow::endTaskFinished);
        select(x.t, rowIndex(x.t, kAlpha));
        buttonOf(*x.w)->click();
        confirmButton(openBox("endTaskConfirmBox"))->click();
        QVERIFY(finished.wait(5000));
        const QString message = finished.at(0).at(4).toString();
        QCOMPARE(finished.at(0).at(3).toInt(), int(o));
        et::Target t;
        t.pid = kAlpha;
        t.startTicks = 111;
        t.name = "alpha";
        t.uid = ownUid();
        t.uidKnown = true;
        QCOMPARE(message, QString::fromStdString(et::describe(x.env.fake->answer, t, et::Signal::Terminate)));
        QTRY_VERIFY(!x.w->statusBar()->currentMessage().isEmpty());
        QVERIFY2(x.w->statusBar()->isVisible(), "the result is not visible");
        QMessageBox* err = openBox("endTaskErrorBox");
        if (o == Outcome::Sent) {
            QVERIFY2(!err, "an error box for a request that was sent");
        } else {
            QVERIFY2(err, outcomeName(o));
            QCOMPARE(int(err->textFormat()), int(Qt::PlainText));
            QCOMPARE(int(err->icon()), int(QMessageBox::Warning));
            QVERIFY(err->text().contains(message) || message.contains(err->text()) || err->informativeText().contains(message));
            QVERIFY2(!message.contains(QLatin1String("sent to"), Qt::CaseInsensitive), qPrintable(message));
        }
        QCOMPARE(x.env.fake->count(), std::size_t(1));
    }
}

void TestEndTaskAdversarial::uiNameIsPlainText()
{
    Window x;
    QSignalSpy finished(x.w.get(), &MainWindow::endTaskFinished);
    x.env.fake->answer = {Outcome::Changed, 0, 'S', false};
    select(x.t, rowIndex(x.t, kOdd));
    buttonOf(*x.w)->click();
    QMessageBox* box = openBox("endTaskConfirmBox");
    QVERIFY(box);
    QCOMPARE(int(box->textFormat()), int(Qt::PlainText));
    QVERIFY2(box->text().contains(QLatin1String("a<b>&amp;c (d)")), qPrintable(box->text()));
    QVERIFY(box->informativeText().isEmpty() || !Qt::mightBeRichText(box->informativeText()) || box->textFormat() == Qt::PlainText);
    confirmButton(box)->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(x.env.fake->call(0).target.name, std::string("a<b>&amp;c (d)"));
    QMessageBox* err = openBox("endTaskErrorBox");
    QVERIFY(err);
    QCOMPARE(int(err->textFormat()), int(Qt::PlainText));
    QVERIFY2(err->text().contains(QLatin1String("a<b>&amp;c (d)")), qPrintable(err->text()));
    for (QLabel* l : err->findChildren<QLabel*>())
        QVERIFY2(l->textFormat() != Qt::RichText && (l->textFormat() == Qt::PlainText || !Qt::mightBeRichText(l->text())),
                 qPrintable(l->text()));
}

void TestEndTaskAdversarial::uiPausedStillWorksAndChangesNothing()
{
    Window x;
    QSignalSpy finished(x.w.get(), &MainWindow::endTaskFinished);
    const int interval = x.w->intervalMs();
    const int window = x.w->windowSeconds();
    const bool details = x.w->detailsVisible();
    x.w->setPaused(true);
    QVERIFY(x.w->isPaused());
    const auto samples = x.w->history().size();
    const int applied = x.w->snapshotsApplied();
    select(x.t, rowIndex(x.t, kAlpha));
    QVERIFY2(buttonOf(*x.w)->isEnabled(), "End task not offered while paused");
    buttonOf(*x.w)->click();
    confirmButton(openBox("endTaskConfirmBox"))->click();
    QVERIFY(finished.wait(5000));
    QCOMPARE(x.env.fake->count(), std::size_t(1));
    QVERIFY(x.w->isPaused());
    QCOMPARE(x.w->history().size(), samples);
    QCOMPARE(x.w->snapshotsApplied(), applied);
    QCOMPARE(x.w->intervalMs(), interval);
    QCOMPARE(x.w->windowSeconds(), window);
    QCOMPARE(x.w->detailsVisible(), details);
}

void TestEndTaskAdversarial::uiWithoutRealProcControlsStayDisabled()
{
    // No backend injected and no collector reading the real /proc.
    Window x(false);
    select(x.t, rowIndex(x.t, kAlpha));
    QCoreApplication::processEvents();
    QPushButton* b = buttonOf(*x.w);
    QVERIFY2(!b->isEnabled(), "live End task offered for injected rows");
    QVERIFY2(!b->toolTip().isEmpty(), "no reason given");
    QVERIFY(!actionOf(*x.w)->isEnabled());
    b->click();
    actionOf(*x.w)->trigger();
    mouseMenu(x.t, x.t->visualRect(rowIndex(x.t, kAlpha)).center());
    if (QMenu* m = openMenu())
        for (const char* n : {"endTaskMenuAction", "forceEndTaskMenuAction"})
            if (QAction* a = menuAction(m, n)) {
                QVERIFY2(!a->isEnabled(), n);
                a->trigger();
            }
    closeMenus();
    QVERIFY(!openBox("endTaskConfirmBox"));
    // Another procRoot with the collector running: still no live backend.
    UiEnv env;
    MainWindowOptions o = env.options(false);
    QTemporaryDir fakeRoot;
    o.startCollector = true;
    o.sampler.procRoot = fakeRoot.path().toStdString();
    o.sampler.sysRoot = fakeRoot.path().toStdString();
    o.clock = {};
    MainWindow w(o);
    w.showPage(MainWindow::ProcessPage);
    w.applySnapshot(snapshotOf(ps::monotonicSeconds(), rows()));
    QTreeView* tv = tableOf(w);
    expandAll(tv);
    select(tv, rowIndex(tv, kAlpha));
    QCoreApplication::processEvents();
    QVERIFY2(!buttonOf(w)->isEnabled(), "live End task offered for a synthetic procRoot");
    QVERIFY(!buttonOf(w)->toolTip().isEmpty());
    w.shutdown(3000);
}

// The real backend through the whole window, on this test's own children
// only: the row is found by its exact (PID, start) from the collector.
void TestEndTaskAdversarial::uiLiveEndToEndOnOwnChild()
{
    OwnedChild c("ignore-term");
    QVERIFY(c.ready());
    UiEnv env;
    MainWindowOptions o = env.options(false);
    o.startCollector = true;
    o.clock = {};
    o.initialIntervalMs = 500;
    MainWindow w(o);
    QSignalSpy finished(&w, &MainWindow::endTaskFinished);
    w.showPage(MainWindow::ProcessPage);
    w.resize(1100, 700);
    w.show();
    QTreeView* t = tableOf(w);
    filterOf(w)->setText(QString::number(c.pid()));
    QModelIndex i;
    QTRY_VERIFY_WITH_TIMEOUT((expandAll(t), i = rowIndex(t, c.pid())).isValid(), 10000);
    QCOMPARE(i.data(ProcessModel::StartTicksRole).toULongLong(), qulonglong(c.startTicks()));
    const auto confirm = [&](bool force) {
        i = rowIndex(t, c.pid());
        select(t, i);
        mouseMenu(t, t->visualRect(i).center());
        QMenu* m = openMenu();
        if (!m)
            return false;
        menuAction(m, force ? "forceEndTaskMenuAction" : "endTaskMenuAction")->trigger();
        closeMenus();
        QMessageBox* box = openBox("endTaskConfirmBox");
        if (!box || !confirmButton(box))
            return false;
        confirmButton(box)->click();
        return finished.wait(5000);
    };
    QVERIFY2(confirm(false), qPrintable(buttonOf(w)->toolTip()));
    QCOMPARE(finished.last().at(3).toInt(), int(Outcome::Sent));
    QVERIFY(c.waitFor('T', 3000));
    QVERIFY2(c.aliveAfter(1000), qPrintable(c.how())); // no escalation
    QVERIFY(!openBox("endTaskErrorBox"));
    QVERIFY2(confirm(true), "force end");
    QCOMPARE(finished.last().at(3).toInt(), int(Outcome::Sent));
    QCOMPARE(finished.last().at(2).toInt(), int(et::Signal::Kill));
    QVERIFY2(c.waitExit(3000) && c.killedBy(SIGKILL), qPrintable(c.how()));
    QCOMPARE(c.termsSeen(), 1);
    QVERIFY(w.shutdown(3000));
}

#endif // PS_ET_APP

#ifdef PS_ET_CORE
namespace {
// Runs as PID 1 of a new user+PID namespace, with the host's /proc.
int nsProbeBody(const et::Target& t)
{
    int failures = 0;
    const auto fail = [&](const std::string& m) {
        std::printf("FAIL: %s\n", m.c_str());
        ++failures;
    };
    std::printf("INFO: getpid %d, /proc/self -> %s\n", int(::getpid()), readLink("/proc/self").c_str());
    Recorder rec; // refuses every signal: nothing can be reached from here
    rec.realOpen = true;
    for (et::Signal sig : {et::Signal::Terminate, et::Signal::Kill}) {
        const et::Result r = et::send(t, sig, rec.hooks());
        std::printf("INFO: send -> %s errno %d\n", outcomeName(r.outcome), r.error);
        if (r.outcome != Outcome::Unsupported)
            fail(std::string("send in another PID namespace gave ") + outcomeName(r.outcome) + ", want Unsupported");
    }
    if (!rec.sends.empty())
        fail("a signal was attempted from another PID namespace");
    return failures ? 1 : 0;
}
} // namespace
#endif

namespace {
int childNamespaceProbe()
{
#ifdef PS_ET_CORE
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    ::alarm(15);
    et::Target t;
    {
        std::istringstream in(std::getenv("PS_ET_TARGET") ? std::getenv("PS_ET_TARGET") : "");
        std::string hex;
        in >> t.pid >> t.startTicks >> hex >> t.uid;
        t.name = fromHex(hex.c_str());
        t.uidKnown = true;
    }
    const uid_t uid = ::getuid();
    const gid_t gid = ::getgid();
    if (::unshare(CLONE_NEWUSER | CLONE_NEWPID) != 0) {
        std::printf("unshare: %s\n", std::strerror(errno));
        return 77;
    }
    const auto put = [](const char* path, const std::string& text) {
        std::ofstream f(path);
        f << text;
        return bool(f.flush());
    };
    // The same UID inside, so ownership is not what refuses it.
    if (!put("/proc/self/setgroups", "deny") || !put("/proc/self/uid_map", std::to_string(uid) + " " + std::to_string(uid) + " 1\n")
        || !put("/proc/self/gid_map", std::to_string(gid) + " " + std::to_string(gid) + " 1\n")) {
        std::printf("id maps: %s\n", std::strerror(errno));
        return 77;
    }
    std::fflush(stdout);
    const pid_t inner = ::fork();
    if (inner < 0)
        return 77;
    if (inner == 0) {
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        const int rc = nsProbeBody(t);
        std::fflush(stdout);
        ::_exit(rc);
    }
    int status = 0;
    const int rc = ::waitpid(inner, &status, 0) != inner ? 3 : WIFEXITED(status) ? WEXITSTATUS(status) : 4;
    std::fflush(stdout);
    // No exit handlers: this process can no longer fork (its PID namespace's
    // init has exited), which LeakSanitizer's exit check needs.
    ::_exit(rc);
#else
    return 77;
#endif
}
} // namespace

#ifdef PS_ET_APP
namespace {
int threadFailBody()
{
    int failures = 0;
    const auto check = [&](bool ok, const QString& what) {
        if (!ok) {
            std::printf("FAIL: %s\n", qPrintable(what));
            ++failures;
        }
    };
    rlimit saved {};
    if (::getrlimit(RLIMIT_NPROC, &saved) != 0) {
        std::printf("getrlimit: %s\n", std::strerror(errno));
        return 77;
    }
    Window x;
    if (!x.t || !buttonOf(*x.w))
        return 5;
    QSignalSpy finished(x.w.get(), &MainWindow::endTaskFinished);
    select(x.t, rowIndex(x.t, kAlpha));
    buttonOf(*x.w)->click();
    QMessageBox* box = openBox("endTaskConfirmBox");
    if (!box || !confirmButton(box))
        return 6;
    // std::thread fails with EAGAIN while this user has more processes than
    // the limit; restored right after the click, which starts the request.
    rlimit low = saved;
    low.rlim_cur = 1;
    if (::setrlimit(RLIMIT_NPROC, &low) != 0) {
        std::printf("setrlimit: %s\n", std::strerror(errno));
        return 77;
    }
    confirmButton(box)->click();
    ::setrlimit(RLIMIT_NPROC, &saved);
    QCoreApplication::processEvents();
    if (x.env.fake->count() != 0 && finished.isEmpty()) {
        std::printf("the thread started despite RLIMIT_NPROC\n");
        return 77;
    }
    check(x.env.fake->count() == 0, QStringLiteral("backend called although its thread could not start"));
    check(finished.count() == 1, QStringLiteral("endTaskFinished emitted %1 times").arg(finished.count()));
    if (!finished.isEmpty()) {
        const auto a = finished.at(0);
        check(a.at(0).toInt() == kAlpha && a.at(1).toULongLong() == 111, QStringLiteral("wrong target reported"));
        check(a.at(3).toInt() != int(Outcome::Sent), QStringLiteral("reported as sent"));
        const QString m = a.at(4).toString();
        check(m.contains(QLatin1String("thread"), Qt::CaseInsensitive), QStringLiteral("reason missing: ") + m);
        check(m.contains(QLatin1String("Nothing was sent")), QStringLiteral("not explicit that nothing was sent: ") + m);
        check(!m.contains(QLatin1String("sent to"), Qt::CaseInsensitive), QStringLiteral("claims a send: ") + m);
    }
    QMessageBox* err = openBox("endTaskErrorBox");
    check(err != nullptr, QStringLiteral("no error box"));
    check(!x.w->statusBar()->currentMessage().isEmpty(), QStringLiteral("no status message"));
    check(!x.w->isEndTaskPending(), QStringLiteral("still pending after the failed start"));
    closeAll("endTaskErrorBox");
    select(x.t, rowIndex(x.t, kAlpha));
    check(buttonOf(*x.w)->isEnabled(), QStringLiteral("End task not usable again: ") + buttonOf(*x.w)->toolTip());
    // And it works once threads can start again: exactly one request.
    buttonOf(*x.w)->click();
    if (QMessageBox* again = openBox("endTaskConfirmBox")) {
        confirmButton(again)->click();
        check(finished.wait(5000), QStringLiteral("no result after the limit was restored"));
        check(x.env.fake->count() == 1, QStringLiteral("%1 backend calls after recovery").arg(x.env.fake->count()));
        check(finished.count() == 2 && finished.at(1).at(3).toInt() == int(Outcome::Sent), QStringLiteral("recovery not sent"));
    } else {
        check(false, QStringLiteral("no confirmation after recovery"));
    }
    return failures ? 1 : 0;
}
} // namespace
#endif

namespace {
int childThreadFailProbe(int argc, char** argv)
{
#ifdef PS_ET_APP
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    ::alarm(25);
    QApplication app(argc, argv);
    const int rc = threadFailBody();
    std::fflush(stdout);
    return rc;
#else
    (void)argc;
    (void)argv;
    return 77;
#endif
}
} // namespace

namespace {
int childFdExhaustProbe()
{
#ifdef PS_ET_CORE
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    ::alarm(15);
    et::Target t;
    {
        std::istringstream in(std::getenv("PS_ET_TARGET") ? std::getenv("PS_ET_TARGET") : "");
        std::string hex;
        in >> t.pid >> t.startTicks >> hex >> t.uid;
        t.name = fromHex(hex.c_str());
        t.uidKnown = true;
    }
    int failures = 0;
    const auto fail = [&](const std::string& m) {
        std::printf("FAIL: %s\n", m.c_str());
        ++failures;
    };
    rlimit saved {};
    if (::getrlimit(RLIMIT_NOFILE, &saved) != 0)
        return 77;
    for (et::Signal sig : {et::Signal::Terminate, et::Signal::Kill}) {
        const int before = openFds();
        std::vector<int> fillers;
        int opened = -1;
        bool exhausted = false;
        et::Hooks hooks;
        // Opens the target's real pidfd, then fills every descriptor this
        // process may still open, so the next open (the fdinfo) is EMFILE.
        hooks.pidfdOpen = [&](int pid) {
            opened = sysPidfdOpen(pid);
            if (opened < 0)
                return -1;
            rlimit low = saved;
            low.rlim_cur = rlim_t(opened) + 64;
            if (low.rlim_cur > saved.rlim_cur || ::setrlimit(RLIMIT_NOFILE, &low) != 0)
                return opened;
            for (;;) {
                const int fd = ::fcntl(opened, F_DUPFD_CLOEXEC, 0);
                if (fd < 0) {
                    exhausted = errno == EMFILE;
                    break;
                }
                fillers.push_back(fd);
            }
            return opened;
        };
        std::vector<int> sends;
        hooks.sendSignal = [&](int, int s) {
            sends.push_back(s);
            errno = EPERM;
            return -1;
        };
        const et::Result r = et::send(t, sig, hooks);
        for (int fd : fillers)
            ::close(fd);
        ::setrlimit(RLIMIT_NOFILE, &saved);
        const std::string text = et::describe(r, t, sig);
        std::printf("INFO: fillers %zu, exhausted %d -> %s errno %d: %s\n", fillers.size(), int(exhausted),
                    outcomeName(r.outcome), r.error, text.c_str());
        if (opened < 0)
            return 77;
        if (!exhausted) {
            std::printf("could not exhaust descriptors\n");
            return 77;
        }
        if (!sends.empty())
            fail("a signal was attempted although the pidfd could not be checked");
        if (r.outcome != Outcome::Failed)
            fail(std::string("fdinfo EMFILE gave ") + outcomeName(r.outcome) + ", want Failed");
        if (r.error != EMFILE)
            fail("fdinfo EMFILE reported errno " + std::to_string(r.error));
        if (text.find("namespace") != std::string::npos)
            fail("a local descriptor shortage described as a namespace mismatch: " + text);
        if (text.find(std::strerror(EMFILE)) == std::string::npos
            && text.find(std::error_code(EMFILE, std::generic_category()).message()) == std::string::npos)
            fail("the message does not name the error: " + text);
        if (text.find("Nothing was sent") == std::string::npos)
            fail("the message does not say nothing was sent: " + text);
        if (openFds() != before)
            fail("descriptors leaked: " + std::to_string(openFds() - before));
    }
    std::fflush(stdout);
    return failures ? 1 : 0;
#else
    return 77;
#endif
}
} // namespace

int main(int argc, char** argv)
{
    g_argc = argc;
    g_argv = argv;
    if (const char* mode = std::getenv("PS_ET_CHILD"))
        return childMain(mode);
    QApplication app(argc, argv);
    TestEndTaskAdversarial t;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&t, argc, argv);
}

#include "tst_end_task_adversarial.moc"
